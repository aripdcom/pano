/*
 * Copyright © 2026 aripdcem
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "terminal-session.hh"

#include "terminal-app.hh"
#include "terminal-debug.hh"
#include "terminal-libgsystem.hh"
#include "terminal-profiles-list.hh"
#include "terminal-schemas.hh"
#include "terminal-settings-list.hh"
#include "terminal-tab.hh"

#include <glib/gstdio.h>
#include <glib-unix.h>

#include <signal.h>

/* The session file holds an a{sv} dictionary:
 *
 *   "version"   u          SESSION_VERSION
 *   "windows"   aa{sv}     most recently used window first
 *   "last-size" (ii)       columns and rows of the last used terminal
 *
 * A window has "tabs" (aa{sv}), "active-tab" (i), "maximized" (b), "width"
 * and "height" (i, in pixels) and, unless its active tab is split,
 * "columns" and "rows" (i). A tab has "layout" (see terminal-tab.hh),
 * "active" (i, the index of the active pane), "pinned" and "zoomed" (b).
 * A pane has "profile" (s, a profile UUID), "cwd" (s), "zoom" (d) and,
 * if the user set one, "title" (s).
 */

#define SESSION_VERSION 1u
#define SAVE_INTERVAL_SECONDS 5
// When the desktop session ends, shells may exit before the server and
// close their tabs one by one. Keep the saved state for this long.
#define FREEZE_SECONDS 60

static struct {
  guint timeout_id;
  guint sigterm_id;
  GBytes* written; // what the file contains
  GVariant* last_size; // (ii) or nullptr
  gint64 frozen_until; // monotonic time
  bool shut_down;
} session;

static bool
session_is_frozen(void)
{
  return session.shut_down || g_get_monotonic_time() < session.frozen_until;
}

static char*
session_dir(void)
{
  return g_build_filename(g_get_user_state_dir(), TERMINAL_PROGRAM_NAME, nullptr);
}

static char*
session_path(void)
{
  gs_free char* dir = session_dir();
  return g_build_filename(dir, "session.gvariant", nullptr);
}

static GVariant*
session_load(void)
{
  gs_free char* path = session_path();
  gs_free char* contents = nullptr;
  gsize length = 0;
  if (!g_file_get_contents(path, &contents, &length, nullptr))
    return nullptr;

  auto const bytes = g_bytes_new_take(g_steal_pointer(&contents), length);
  gs_unref_variant GVariant* data = g_variant_ref_sink(g_variant_new_from_bytes(G_VARIANT_TYPE_VARDICT, bytes, false));
  g_bytes_unref(bytes);

  // The file might be damaged; only trust it in normal form
  auto const value = g_variant_get_normal_form(data);

  guint32 version = 0;
  if (!g_variant_lookup(value, "version", "u", &version) ||
      version != SESSION_VERSION) {
    g_variant_unref(value);
    return nullptr;
  }

  return value;
}

/* Saving */

static GVariant*
save_screen(TerminalScreen* screen,
            void* user_data)
{
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);

  gs_unref_object GSettings* profile = terminal_screen_ref_profile(screen);
  gs_free char* uuid = profile
    ? terminal_settings_list_dup_uuid_from_child(terminal_app_get_profiles_list(terminal_app_get()),
                                                 profile)
    : nullptr;
  if (uuid)
    g_variant_builder_add(&builder, "{sv}", "profile", g_variant_new_string(uuid));

  gs_free char* cwd = terminal_screen_get_current_dir(screen);
  if (cwd)
    g_variant_builder_add(&builder, "{sv}", "cwd", g_variant_new_string(cwd));

  g_variant_builder_add(&builder, "{sv}", "zoom",
                        g_variant_new_double(vte_terminal_get_font_scale(VTE_TERMINAL(screen))));

  if (auto const title = terminal_screen_get_user_title(screen))
    g_variant_builder_add(&builder, "{sv}", "title", g_variant_new_string(title));

  gs_free char* container_runtime = nullptr;
  gs_free char* container_name = nullptr;
  if (terminal_screen_get_container(screen, &container_runtime, &container_name)) {
    g_variant_builder_add(&builder, "{sv}", "container-runtime", g_variant_new_string(container_runtime));
    g_variant_builder_add(&builder, "{sv}", "container-name", g_variant_new_string(container_name));
  }

  return g_variant_builder_end(&builder);
}

static bool
window_has_free_size(TerminalWindow* window)
{
  return !gtk_window_is_maximized(GTK_WINDOW(window)) &&
    !gtk_window_is_fullscreen(GTK_WINDOW(window));
}

/**
 * terminal_session_save_tab:
 * @tab: a #TerminalTab
 *
 * Returns: (transfer floating) (nullable): the panes, working directories
 *   and profiles of @tab, for terminal_session_restore_tab()
 */
GVariant*
terminal_session_save_tab(TerminalTab* tab)
{
  g_return_val_if_fail(TERMINAL_IS_TAB(tab), nullptr);

  auto const layout = terminal_tab_save_layout(tab, save_screen, nullptr);
  if (!layout)
    return nullptr;

  auto const screens = terminal_tab_list_screens(tab);
  auto const active_pane = g_list_index(screens, terminal_tab_get_screen(tab));
  g_list_free(screens);

  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&builder, "{sv}", "layout", layout);
  g_variant_builder_add(&builder, "{sv}", "active", g_variant_new_int32(MAX(active_pane, 0)));
  g_variant_builder_add(&builder, "{sv}", "pinned", g_variant_new_boolean(terminal_tab_get_pinned(tab)));
  g_variant_builder_add(&builder, "{sv}", "zoomed", g_variant_new_boolean(terminal_tab_get_zoomed(tab)));
  return g_variant_builder_end(&builder);
}

static GVariant*
save_window(TerminalWindow* window)
{
  auto const tabs = terminal_window_list_tabs(window);
  if (!tabs)
    return nullptr;

  auto const active_tab = terminal_window_get_active_tab(window);
  auto active_tab_index = 0;

  GVariantBuilder tabs_builder;
  g_variant_builder_init(&tabs_builder, G_VARIANT_TYPE("aa{sv}"));

  auto index = 0;
  for (auto l = tabs; l; l = l->next, ++index) {
    auto const tab = TERMINAL_TAB(l->data);
    if (tab == active_tab)
      active_tab_index = index;

    if (auto const tab_data = terminal_session_save_tab(tab))
      g_variant_builder_add_value(&tabs_builder, tab_data);
  }
  g_list_free(tabs);

  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&builder, "{sv}", "tabs", g_variant_builder_end(&tabs_builder));
  g_variant_builder_add(&builder, "{sv}", "active-tab", g_variant_new_int32(active_tab_index));
  g_variant_builder_add(&builder, "{sv}", "maximized",
                        g_variant_new_boolean(gtk_window_is_maximized(GTK_WINDOW(window))));

  int width = 0, height = 0;
  gtk_window_get_default_size(GTK_WINDOW(window), &width, &height);
  if (width > 0 && height > 0) {
    g_variant_builder_add(&builder, "{sv}", "width", g_variant_new_int32(width));
    g_variant_builder_add(&builder, "{sv}", "height", g_variant_new_int32(height));
  }

  // Unless the active tab is split, the window size follows the grid of its
  // terminal, which keeps working if the font changes.
  if (active_tab && !terminal_tab_is_split(active_tab)) {
    int columns, rows;
    terminal_screen_get_size(terminal_tab_get_screen(active_tab), &columns, &rows);
    g_variant_builder_add(&builder, "{sv}", "columns", g_variant_new_int32(columns));
    g_variant_builder_add(&builder, "{sv}", "rows", g_variant_new_int32(rows));
  }

  return g_variant_builder_end(&builder);
}

/* Returns: (transfer full): the current session, without @closing unless
 * it is the last window
 */
static GVariant*
session_build(TerminalWindow* closing)
{
  GVariantBuilder windows_builder;
  g_variant_builder_init(&windows_builder, G_VARIANT_TYPE("aa{sv}"));

  // Sorted by last use, most recent first. A window that is being destroyed
  // may already be gone from the list.
  GList* windows = nullptr;
  // The quick terminal is not part of the session.
  for (auto l = gtk_application_get_windows(GTK_APPLICATION(terminal_app_get())); l; l = l->next) {
    if (TERMINAL_IS_WINDOW(l->data) && l->data != closing &&
        !terminal_window_is_quick(TERMINAL_WINDOW(l->data)))
      windows = g_list_prepend(windows, l->data);
  }
  windows = g_list_reverse(windows);

  // The last window stays in the session
  if (!windows && closing && !terminal_window_is_quick(closing))
    windows = g_list_prepend(windows, closing);

  auto size_saved = false;
  for (auto l = windows; l; l = l->next) {
    auto const window = TERMINAL_WINDOW(l->data);

    if (auto const value = save_window(window))
      g_variant_builder_add_value(&windows_builder, value);

    // Remember the size of the most recently used terminal for new windows
    auto const tab = terminal_window_get_active_tab(window);
    if (!size_saved && tab && !terminal_tab_is_split(tab) && window_has_free_size(window)) {
      int columns, rows;
      terminal_screen_get_size(terminal_tab_get_screen(tab), &columns, &rows);
      g_clear_pointer(&session.last_size, g_variant_unref);
      session.last_size = g_variant_ref_sink(g_variant_new("(ii)", columns, rows));
      size_saved = true;
    }
  }
  g_list_free(windows);

  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&builder, "{sv}", "version", g_variant_new_uint32(SESSION_VERSION));
  g_variant_builder_add(&builder, "{sv}", "windows", g_variant_builder_end(&windows_builder));
  if (session.last_size)
    g_variant_builder_add(&builder, "{sv}", "last-size", session.last_size);

  return g_variant_ref_sink(g_variant_builder_end(&builder));
}

static void
session_write(GVariant* value)
{
  gs_unref_variant GVariant* normal = g_variant_get_normal_form(value);
  auto const bytes = g_variant_get_data_as_bytes(normal);

  if (session.written && g_bytes_equal(bytes, session.written)) {
    g_bytes_unref(bytes);
    return;
  }

  gs_free char* dir = session_dir();
  gs_free char* path = session_path();
  gs_free_error GError* error = nullptr;
  gsize size = 0;
  auto const data = g_bytes_get_data(bytes, &size);
  if (g_mkdir_with_parents(dir, 0700) != 0 ||
      !g_file_set_contents_full(path, reinterpret_cast<char const*>(data), size,
                                G_FILE_SET_CONTENTS_CONSISTENT, 0600, &error)) {
    g_warning("Failed to save the session to %s: %s",
              path, error ? error->message : g_strerror(errno));
    g_bytes_unref(bytes);
    return;
  }

  _terminal_debug_print(TERMINAL_DEBUG_SERVER, "Saved the session (%" G_GSIZE_FORMAT " bytes)\n", size);

  g_clear_pointer(&session.written, g_bytes_unref);
  session.written = bytes;
}

static void
session_save(TerminalWindow* closing)
{
  gs_unref_variant GVariant* value = session_build(closing);
  session_write(value);
}

static bool
have_terminal_windows(TerminalWindow* except)
{
  auto const windows = gtk_application_get_windows(GTK_APPLICATION(terminal_app_get()));
  for (auto l = windows; l; l = l->next) {
    if (TERMINAL_IS_WINDOW(l->data) && l->data != except &&
        !terminal_window_is_quick(TERMINAL_WINDOW(l->data)))
      return true;
  }
  return false;
}

static gboolean
session_save_timeout_cb(void* user_data)
{
  // Without windows, the file keeps the state of the last one closed
  if (!session_is_frozen() && have_terminal_windows(nullptr))
    session_save(nullptr);

  return G_SOURCE_CONTINUE;
}

/* Saves the session now and ignores changes for a while */
static void
session_save_and_freeze(void)
{
  if (session_is_frozen())
    return;

  if (have_terminal_windows(nullptr))
    session_save(nullptr);

  session.frozen_until = g_get_monotonic_time() + FREEZE_SECONDS * G_USEC_PER_SEC;
}

static void
query_end_cb(GtkApplication* application,
             void* user_data)
{
  session_save_and_freeze();
}

static gboolean
sigterm_cb(void* user_data)
{
  session_save_and_freeze();
  g_application_quit(G_APPLICATION(terminal_app_get()));

  return G_SOURCE_CONTINUE;
}

/**
 * terminal_session_init:
 *
 * Starts saving the session regularly.
 */
void
terminal_session_init(void)
{
  gs_unref_variant GVariant* data = session_load();
  if (data)
    session.last_size = g_variant_lookup_value(data, "last-size", G_VARIANT_TYPE("(ii)"));

  session.timeout_id = g_timeout_add_seconds(SAVE_INTERVAL_SECONDS,
                                             session_save_timeout_cb,
                                             nullptr);

  g_signal_connect(terminal_app_get(), "query-end", G_CALLBACK(query_end_cb), nullptr);
  session.sigterm_id = g_unix_signal_add(SIGTERM, sigterm_cb, nullptr);
}

/**
 * terminal_session_shutdown:
 *
 * Saves the session one last time, for example when the user logs out
 * with windows still open.
 */
void
terminal_session_shutdown(void)
{
  if (session.shut_down)
    return;

  if (!session_is_frozen() && have_terminal_windows(nullptr))
    session_save(nullptr);

  session.shut_down = true;
  g_clear_handle_id(&session.timeout_id, g_source_remove);
  g_clear_handle_id(&session.sigterm_id, g_source_remove);
  g_clear_pointer(&session.written, g_bytes_unref);
  g_clear_pointer(&session.last_size, g_variant_unref);
}

/**
 * terminal_session_window_closing:
 * @window: a #TerminalWindow that is being destroyed
 *
 * Removes @window from the session, unless it is the last window: then the
 * session keeps it so that it can be restored. A window whose last tab was
 * closed has no tabs left and so leaves an empty session.
 */
void
terminal_session_window_closing(TerminalWindow* window)
{
  if (session_is_frozen() || terminal_window_is_quick(window))
    return;

  session_save(window);
}

/* Restoring */

struct RestoreContext {
  GPtrArray* screens; // TerminalScreen*, unowned
  GPtrArray* cwds; // char*, owned
};

static TerminalScreen*
restore_screen_cb(GVariant* pane,
                  void* user_data)
{
  auto const context = reinterpret_cast<RestoreContext*>(user_data);
  auto const profiles = terminal_app_get_profiles_list(terminal_app_get());

  char const* uuid = nullptr;
  g_variant_lookup(pane, "profile", "&s", &uuid);

  // The profile may have been deleted since; fall back to the default
  gs_unref_object GSettings* profile =
    terminal_profiles_list_ref_profile_by_uuid(profiles, uuid, nullptr);
  if (!profile)
    profile = terminal_profiles_list_ref_profile_by_uuid(profiles, nullptr, nullptr);
  if (!profile)
    return nullptr;

  double zoom;
  if (!g_variant_lookup(pane, "zoom", "d", &zoom))
    zoom = 1.0;

  auto const screen = terminal_screen_new(profile, nullptr, zoom);

  char const* title = nullptr;
  if (g_variant_lookup(pane, "title", "&s", &title))
    terminal_screen_set_user_title(screen, title);

  char const* container_runtime = nullptr;
  char const* container_name = nullptr;
  if (g_variant_lookup(pane, "container-runtime", "&s", &container_runtime) &&
      g_variant_lookup(pane, "container-name", "&s", &container_name))
    terminal_screen_set_container(screen, container_runtime, container_name);

  char const* cwd = nullptr;
  g_variant_lookup(pane, "cwd", "&s", &cwd);
  g_ptr_array_add(context->screens, screen);
  g_ptr_array_add(context->cwds, g_strdup(cwd));

  return screen;
}

/**
 * terminal_session_restore_tab:
 * @window: a #TerminalWindow
 * @data: a tab from terminal_session_save_tab()
 * @position: where to put the tab, or -1 for the end
 *
 * Creates the tab with a new shell in every pane, in its old working
 * directory if that still exists.
 *
 * Returns: (transfer none) (nullable): the new tab
 */
TerminalTab*
terminal_session_restore_tab(TerminalWindow* window,
                             GVariant* data,
                             int position)
{
  g_return_val_if_fail(TERMINAL_IS_WINDOW(window), nullptr);
  g_return_val_if_fail(data != nullptr, nullptr);

  gs_unref_variant GVariant* layout = g_variant_lookup_value(data, "layout", G_VARIANT_TYPE_VARDICT);
  if (!layout)
    return nullptr;

  RestoreContext context{g_ptr_array_new(), g_ptr_array_new_with_free_func(g_free)};
  auto const tab = terminal_tab_new_from_layout(layout, restore_screen_cb, &context);
  if (!tab) {
    g_ptr_array_unref(context.screens);
    g_ptr_array_unref(context.cwds);
    return nullptr;
  }

  terminal_window_insert_tab(window, tab, position);

  gboolean pinned;
  if (g_variant_lookup(data, "pinned", "b", &pinned) && pinned)
    terminal_window_set_tab_pinned(window, tab, true);

  int active;
  if (g_variant_lookup(data, "active", "i", &active)) {
    auto const screens = terminal_tab_list_screens(tab);
    if (auto const screen = g_list_nth_data(screens, guint(MAX(active, 0))))
      terminal_tab_set_active_screen(tab, TERMINAL_SCREEN(screen));
    g_list_free(screens);
  }

  gboolean zoomed;
  if (g_variant_lookup(data, "zoomed", "b", &zoomed) && zoomed)
    terminal_tab_set_zoomed(tab, true);

  for (guint i = 0; i < context.screens->len; ++i) {
    auto const screen = TERMINAL_SCREEN(g_ptr_array_index(context.screens, i));
    auto cwd = reinterpret_cast<char const*>(g_ptr_array_index(context.cwds, i));
    if (cwd && !g_file_test(cwd, G_FILE_TEST_IS_DIR))
      cwd = nullptr;

    gs_free_error GError* error = nullptr;
    if (!terminal_screen_exec(screen,
                              nullptr /* argv: the shell */,
                              nullptr /* envv */,
                              true /* as shell */,
                              cwd,
                              nullptr, nullptr, /* fd list and array */
                              nullptr, nullptr, nullptr, /* callback */
                              nullptr /* cancellable */,
                              &error))
      g_warning("Failed to start a restored terminal: %s", error->message);
  }
  g_ptr_array_unref(context.screens);
  g_ptr_array_unref(context.cwds);

  return tab;
}

static TerminalWindow*
restore_window(GVariant* data,
               char const* activation_token,
               char const* startup_id)
{
  gs_unref_variant GVariant* tabs_data = g_variant_lookup_value(data, "tabs", G_VARIANT_TYPE("aa{sv}"));
  if (!tabs_data || g_variant_n_children(tabs_data) == 0)
    return nullptr;

  auto const window = terminal_window_new(G_APPLICATION(terminal_app_get()));
  if (activation_token)
    gtk_window_set_startup_id(GTK_WINDOW(window), activation_token);
  else if (startup_id)
    gtk_window_set_startup_id(GTK_WINDOW(window), startup_id);

  gs_unref_ptrarray GPtrArray* tabs = g_ptr_array_new();

  auto const n_tabs = g_variant_n_children(tabs_data);
  for (gsize i = 0; i < n_tabs; ++i) {
    gs_unref_variant GVariant* tab_data = g_variant_get_child_value(tabs_data, i);
    if (auto const tab = terminal_session_restore_tab(window, tab_data, -1))
      g_ptr_array_add(tabs, tab);
  }

  if (tabs->len == 0) {
    gtk_window_destroy(GTK_WINDOW(window));
    return nullptr;
  }

  int active_tab;
  if (!g_variant_lookup(data, "active-tab", "i", &active_tab) ||
      active_tab < 0 || guint(active_tab) >= tabs->len)
    active_tab = 0;
  auto const tab = TERMINAL_TAB(g_ptr_array_index(tabs, active_tab));
  terminal_window_switch_screen(window, terminal_tab_get_screen(tab));

  // Size: the terminal grid if the active tab is not split, else pixels
  int columns, rows, width, height;
  if (g_variant_lookup(data, "columns", "i", &columns) &&
      g_variant_lookup(data, "rows", "i", &rows) &&
      columns > 0 && rows > 0 && !terminal_tab_is_split(tab)) {
    for (guint i = 0; i < tabs->len; ++i) {
      auto const other = TERMINAL_TAB(g_ptr_array_index(tabs, i));
      if (!terminal_tab_is_split(other))
        vte_terminal_set_size(VTE_TERMINAL(terminal_tab_get_screen(other)), columns, rows);
    }
    gtk_window_set_default_size(GTK_WINDOW(window), -1, -1);
  } else if (g_variant_lookup(data, "width", "i", &width) &&
             g_variant_lookup(data, "height", "i", &height) &&
             width > 0 && height > 0) {
    gtk_window_set_default_size(GTK_WINDOW(window), width, height);
  }

  gboolean maximized;
  if (g_variant_lookup(data, "maximized", "b", &maximized) && maximized)
    gtk_window_maximize(GTK_WINDOW(window));

  gtk_window_present(GTK_WINDOW(window));
  return window;
}

/**
 * terminal_session_restore:
 * @activation_token: (nullable): an XDG activation token
 * @startup_id: (nullable): an X11 startup notification ID
 *
 * Opens the windows of the saved session, if restoring sessions is enabled,
 * no window is open yet, and the saved session has windows.
 *
 * Returns: %true if at least one window was restored
 */
bool
terminal_session_restore(char const* activation_token,
                         char const* startup_id)
{
  auto const settings = terminal_app_get_global_settings(terminal_app_get());
  if (!g_settings_get_boolean(settings, TERMINAL_SETTING_RESTORE_SESSION_KEY) ||
      have_terminal_windows(nullptr))
    return false;

  gs_unref_variant GVariant* data = session_load();
  if (!data)
    return false;

  gs_unref_variant GVariant* windows = g_variant_lookup_value(data, "windows", G_VARIANT_TYPE("aa{sv}"));
  if (!windows)
    return false;

  // Open the least recently used window first, so that the most recently
  // used one ends up on top and gets the activation token.
  auto restored = false;
  auto const n_windows = g_variant_n_children(windows);
  for (auto i = n_windows; i > 0; --i) {
    gs_unref_variant GVariant* window_data = g_variant_get_child_value(windows, i - 1);
    auto const last = i == 1;
    if (restore_window(window_data,
                       last ? activation_token : nullptr,
                       last ? startup_id : nullptr))
      restored = true;
  }

  return restored;
}

/**
 * terminal_session_apply_window_size:
 * @screen: the first #TerminalScreen of a new window
 *
 * Gives @screen the size of the last used terminal, if remembering the
 * window size is enabled.
 */
void
terminal_session_apply_window_size(TerminalScreen* screen)
{
  auto const settings = terminal_app_get_global_settings(terminal_app_get());
  if (!session.last_size ||
      !g_settings_get_boolean(settings, TERMINAL_SETTING_RESTORE_WINDOW_SIZE_KEY))
    return;

  int columns, rows;
  g_variant_get(session.last_size, "(ii)", &columns, &rows);
  if (columns > 0 && rows > 0)
    vte_terminal_set_size(VTE_TERMINAL(screen), columns, rows);
}
