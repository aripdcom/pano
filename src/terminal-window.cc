/*
 * Copyright © 2001 Havoc Pennington
 * Copyright © 2002 Red Hat, Inc.
 * Copyright © 2007, 2008, 2009, 2011, 2017, 2023 Christian Persch
 * Copyright © 2023 Christian Hergert
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

#include <string.h>
#include <stdlib.h>

#include <algorithm>

#include <glib.h>
#include <glib/gi18n.h>

#include <gtk/gtk.h>
#include <uuid.h>

#ifdef GDK_WINDOWING_X11
# include <gdk/x11/gdkx.h>
#endif
#ifdef GDK_WINDOWING_WAYLAND
# include <gdk/wayland/gdkwayland.h>
#endif

#include "terminal-app.hh"
#include "terminal-defines.hh"
#include "terminal-debug.hh"
#include "terminal-enums.hh"
#include "terminal-headerbar.hh"
#include "terminal-find-bar.hh"
#include "terminal-command-palette.hh"
#include "terminal-gnome-terminal-import.hh"
#include "terminal-icon-button.hh"
#include "terminal-intl.hh"
#include "terminal-notebook.hh"
#include "terminal-schemas.hh"
#include "terminal-session.hh"
#include "terminal-tab.hh"
#include "terminal-tab-sidebar.hh"
#include "terminal-util.hh"
#include "terminal-window.hh"
#include "terminal-libgsystem.hh"

struct _TerminalWindow
{
  AdwApplicationWindow parent_instance;

  char *uuid;

  GdkClipboard *clipboard;

  TerminalScreenPopupInfo *popup_info;

  GtkWidget *titlebar;
  AdwTabBar *tab_bar;
  TerminalTabSidebar* tab_sidebar;
  GtkPositionType tab_position;

  /* While a tab is dragged from a tab bar over the terminals: the pane it
   * would join, see window_transfer_motion_cb() */
  TerminalScreen* tab_drop_screen; // weak pointer
  TerminalPaneDrop tab_drop_where;
  AdwTabOverview* tab_overview;
  TerminalHeaderbar *headerbar;
  TerminalNotebook *notebook;
  GActionMap* notebook_context_action_group; // unowned
  AdwToolbarView *toolbar_view;
  GtkWidget* content_box;
  AdwToastOverlay* toast_overlay;
  AdwToast* closed_tab_toast; // weak pointer
  AdwToast* copy_toast; // weak pointer
  GtkCssProvider* colors_provider; // the header bar in the terminal's colors
  char* colors_class; // the CSS class that selects this window
  GtkWidget *main_vbox;
  GtkWidget* ask_default_infobar;
  GtkWidget* import_infobar;
  AdwBanner* broadcast_banner;
  bool quick; /* the quick terminal, see terminal_window_set_quick() */
  int broadcast_mode; /* BroadcastMode */
  TerminalScreen *active_screen;
  TerminalTab* notebook_context_tab; // unowned
  gboolean closing_confirmed; // close tabs without asking again

  /* Size of a character cell in pixels */
  int old_char_width;
  int old_char_height;

  /* Width and height added to the actual terminal grid by "chrome" inside
   * what was traditionally the X11 window: menu bar, title bar,
   * style-provided padding. This must be included when resizing the window
   * and also included in geometry hints. */
  int old_chrome_width;
  int old_chrome_height;

  /* Width and height added to the window by client-side decorations.
   * This must be included in geometry hints but must not be included when
   * resizing the window. */
  int old_csd_width;
  int old_csd_height;

  /* Width and height of the padding around the geometry widget. */
  int old_padding_width;
  int old_padding_height;

  void *old_geometry_widget; /* only used for pointer value as it may be freed */

  /* For restoring hints after unmaximizing etc */
  GdkToplevelState window_state;

  GtkWidget *confirm_close_dialog;

  TerminalFindBar* find_bar;
  GtkRevealer* find_bar_revealer;

  GtkPopoverMenu *context_menu;

  /* A GSource delaying transition until animations complete */
  guint fullscreen_transition;

  guint focus_active_tab_source;

  /* Gives the focus back to the terminal after a menu closed */
  guint refocus_idle;

  guint disposed : 1;
  guint present_on_insert : 1;
  guint tab_overview_animating : 1;
  guint tab_bar_at_bottom : 1;
  guint tabs_hidden_for_fullscreen : 1;

  guint realized : 1;
};

#define MIN_WIDTH_CHARS 4
#define MIN_HEIGHT_CHARS 1

/* See bug #789356 and issue gnome-terminal#129*/
static inline constexpr auto
window_state_is_snapped(GdkToplevelState state) noexcept
{
  return (state & (GDK_TOPLEVEL_STATE_FULLSCREEN |
                   GDK_TOPLEVEL_STATE_MAXIMIZED |
                   GDK_TOPLEVEL_STATE_BOTTOM_TILED |
                   GDK_TOPLEVEL_STATE_LEFT_TILED |
                   GDK_TOPLEVEL_STATE_RIGHT_TILED |
                   GDK_TOPLEVEL_STATE_TOP_TILED |
                   GDK_TOPLEVEL_STATE_TILED)) != 0;
}

static void terminal_window_dispose  (GObject *object);
static void terminal_window_finalize (GObject *object);

static void terminal_window_state_event (GtkWidget  *widget,
                                         GParamSpec *pspec,
                                         GdkToplevel *surface);

static gboolean terminal_window_close_request (GtkWindow *window);

static void notebook_screen_switched_cb   (TerminalNotebook *notebook,
                                           TerminalScreen   *old_active_screen,
                                           TerminalScreen   *screen,
                                           TerminalWindow   *window);
static void notebook_screen_added_cb      (TerminalNotebook *notebook,
                                           TerminalScreen   *screen,
                                           TerminalWindow   *window);
static void notebook_screen_removed_cb    (TerminalNotebook *notebook,
                                           TerminalScreen   *screen,
                                           TerminalWindow   *window);
static void notebook_screens_reordered_cb (TerminalNotebook *notebook,
                                           TerminalWindow   *window);

static void window_update_tabs_visibility(TerminalWindow* window);
static void notebook_setup_menu_cb(TerminalNotebook* notebook,
                                   TerminalTab* tab,
                                   TerminalWindow* window);

static gboolean screen_close_request_cb (TerminalNotebook *notebook,
                                         TerminalScreen   *screen,
                                         TerminalWindow   *window);

/* Menu action callbacks */
static gboolean find_larger_zoom_factor                   (double         *zoom);
static gboolean find_smaller_zoom_factor                  (double         *zoom);
static void     terminal_window_update_zoom_sensitivity   (TerminalWindow *window);
static void     terminal_window_update_search_sensitivity (TerminalWindow *window);
static void     terminal_window_update_paste_sensitivity  (TerminalWindow *window);
static void     terminal_window_show                      (GtkWidget      *widget);
// What a close confirmation is about
enum class CloseKind {
  WINDOW, // all terminals of the window
  TAB,    // all panes of the tab of the screen
  PANE,   // only the screen
  OTHER_TABS, // the tabs other than the screen's, except pinned ones
};

static void terminal_window_update_pane_actions(TerminalWindow* window);
static void window_sync_translucency(TerminalWindow* window);
static void window_update_broadcast(TerminalWindow* window);

// Where typed input goes besides the active terminal
enum BroadcastMode {
  BROADCAST_OFF,
  BROADCAST_TAB, // all panes of the active tab
  BROADCAST_WINDOW, // all terminals of the window
};
static gboolean confirm_close_window_or_tab (TerminalWindow *window,
                                             TerminalScreen *screen,
                                             CloseKind kind);
static void window_close_other_tabs(TerminalWindow* window,
                                    TerminalTab* tab);

G_DEFINE_FINAL_TYPE (TerminalWindow, terminal_window, ADW_TYPE_APPLICATION_WINDOW)

/* Zoom helpers */

static const double zoom_factors[] = {
  TERMINAL_SCALE_MINIMUM,
  TERMINAL_SCALE_XXXXX_SMALL,
  TERMINAL_SCALE_XXXX_SMALL,
  TERMINAL_SCALE_XXX_SMALL,
  PANGO_SCALE_XX_SMALL,
  PANGO_SCALE_X_SMALL,
  PANGO_SCALE_SMALL,
  PANGO_SCALE_MEDIUM,
  PANGO_SCALE_LARGE,
  PANGO_SCALE_X_LARGE,
  PANGO_SCALE_XX_LARGE,
  TERMINAL_SCALE_XXX_LARGE,
  TERMINAL_SCALE_XXXX_LARGE,
  TERMINAL_SCALE_XXXXX_LARGE,
  TERMINAL_SCALE_MAXIMUM
};

static gboolean
find_larger_zoom_factor (double *zoom)
{
  double current = *zoom;
  guint i;

  for (i = 0; i < G_N_ELEMENTS (zoom_factors); ++i)
    {
      /* Find a font that's larger than this one */
      if ((zoom_factors[i] - current) > 1e-6)
        {
          *zoom = zoom_factors[i];
          return TRUE;
        }
    }

  return FALSE;
}

static gboolean
find_smaller_zoom_factor (double *zoom)
{
  double current = *zoom;
  int i;

  i = (int) G_N_ELEMENTS (zoom_factors) - 1;
  while (i >= 0)
    {
      /* Find a font that's smaller than this one */
      if ((current - zoom_factors[i]) > 1e-6)
        {
          *zoom = zoom_factors[i];
          return TRUE;
        }

      --i;
    }

  return FALSE;
}

static inline GSimpleAction *
lookup_action (TerminalWindow *window,
               const char *name)
{
  GAction *action;

  action = g_action_map_lookup_action (G_ACTION_MAP (window), name);
  g_return_val_if_fail (action != nullptr, nullptr);

  return G_SIMPLE_ACTION (action);
}

/* New terminals opened from @parent start in its container, if any */
static void
window_preserve_container(TerminalScreen* screen,
                          TerminalScreen* parent)
{
  if (!parent ||
      !g_settings_get_boolean(terminal_app_get_global_settings(terminal_app_get()),
                              TERMINAL_SETTING_PRESERVE_CONTAINER_KEY))
    return;

  gs_free char* runtime = nullptr;
  gs_free char* name = nullptr;
  if (terminal_screen_get_container(parent, &runtime, &name))
    terminal_screen_set_container(screen, runtime, name);
}

static TerminalTab*
terminal_window_new_tab(TerminalWindow* window,
                        char const* mode_str,
                        char const* uuid_str,
                        bool grab_focus)
{
  TerminalApp *app;
  TerminalSettingsList *profiles_list;
  gs_unref_object GSettings *profile = nullptr;
  gboolean can_toggle = FALSE;

  g_assert (TERMINAL_IS_WINDOW (window));

  app = terminal_app_get ();

  TerminalNewTerminalMode mode;
  if (g_str_equal (mode_str, "tab"))
    mode = TERMINAL_NEW_TERMINAL_MODE_TAB;
  else if (g_str_equal (mode_str, "window"))
    mode = TERMINAL_NEW_TERMINAL_MODE_WINDOW;
  else if (g_str_equal (mode_str, "tab-default")) {
    mode = TERMINAL_NEW_TERMINAL_MODE_TAB;
    can_toggle = TRUE;
  } else {
    mode = TerminalNewTerminalMode(g_settings_get_enum (terminal_app_get_global_settings (app),
						      TERMINAL_SETTING_NEW_TERMINAL_MODE_KEY));
    can_toggle = TRUE;
  }

  if (can_toggle) {
    GdkDisplay *display = gdk_display_get_default ();
    GdkSeat *seat = gdk_display_get_default_seat (display);
    GdkDevice *keyboard = gdk_seat_get_keyboard (seat);
    GdkModifierType modifiers = GdkModifierType(gdk_device_get_modifier_state (keyboard) & gtk_accelerator_get_default_mod_mask ());

    if (modifiers & GDK_CONTROL_MASK) {
      /* Invert */
      if (mode == TERMINAL_NEW_TERMINAL_MODE_WINDOW)
        mode = TERMINAL_NEW_TERMINAL_MODE_TAB;
      else
        mode = TERMINAL_NEW_TERMINAL_MODE_WINDOW;
    }
  }

  TerminalScreen *parent_screen = window->active_screen;
  auto parent_tab = parent_screen ? terminal_tab_get_from_screen(parent_screen) : nullptr;

  profiles_list = terminal_app_get_profiles_list (app);
  if (g_str_equal (uuid_str, "current"))
    profile = terminal_screen_ref_profile (parent_screen);
  else if (g_str_equal (uuid_str, "default"))
    profile = terminal_settings_list_ref_default_child (profiles_list);
  else
    profile = terminal_settings_list_ref_child (profiles_list, uuid_str);

  if (profile == nullptr)
    return nullptr;

  if (mode == TERMINAL_NEW_TERMINAL_MODE_WINDOW) {
    window = terminal_window_new (G_APPLICATION (app));
    // Cannot use not use the parent tab since it belongs to a different window
    parent_tab = nullptr;
  }

  TerminalScreen *screen = terminal_screen_new (profile,
                                                nullptr /* title */,
                                                1.0);
  if (mode == TERMINAL_NEW_TERMINAL_MODE_WINDOW)
    terminal_session_apply_window_size (screen);
  window_preserve_container (screen, parent_screen);
  auto const tab = terminal_tab_new(screen);

  // Now add the new screen to the window.
  terminal_window_add_tab(window, tab, parent_tab);
  terminal_window_switch_screen (window, screen);

  if (grab_focus)
    gtk_widget_grab_focus(GTK_WIDGET(screen));

  /* Start child process, if possible by using the same args as the parent screen */
  terminal_screen_reexec_from_screen (screen, parent_screen, nullptr, nullptr);

  if (mode == TERMINAL_NEW_TERMINAL_MODE_WINDOW)
    gtk_window_present (GTK_WINDOW (window));

  return tab;
}

static gboolean
terminal_window_focus_active_tab_cb(void* data)
{
  auto const window = reinterpret_cast<TerminalWindow*>(data);

  g_assert(TERMINAL_IS_WINDOW(window));

  window->focus_active_tab_source = 0;
  window->tab_overview_animating = false;

  if (auto const active_tab = terminal_window_get_active_tab(window)) {
    gtk_widget_grab_focus(GTK_WIDGET(active_tab));
    gtk_widget_queue_resize(GTK_WIDGET(active_tab));
  }

  return G_SOURCE_REMOVE;
}

static void
terminal_window_tab_overview_notify_open_cb(AdwTabOverview* tab_overview,
                                            GParamSpec* pspec,
                                            TerminalWindow* window)
{
  g_assert(TERMINAL_IS_WINDOW(window));
  g_assert(ADW_IS_TAB_OVERVIEW(tab_overview));

  // For some reason when we get here the selected page is not
  // getting focused. So work around libadwaita by deferring the
  // focus to an idle so that we can ensure we're working with
  // the appropriate focus tab.
  //
  // See https://gitlab.gnome.org/GNOME/libadwaita/-/issues/670

  g_clear_handle_id(&window->focus_active_tab_source, g_source_remove);

  if (!adw_tab_overview_get_open(tab_overview)) {
    auto delay_msec = 425u; // Sync with libadwaita!
    auto gtk_enable_animations = gboolean{false};
    g_object_get (gtk_settings_get_default(),
                  "gtk-enable-animations", &gtk_enable_animations,
                  nullptr);

    if (!gtk_enable_animations)
      delay_msec = 10;

    window->focus_active_tab_source = g_timeout_add_full(G_PRIORITY_LOW,
                                                         delay_msec,
                                                         terminal_window_focus_active_tab_cb,
                                                         window, nullptr);

    if (auto const active_tab = terminal_window_get_active_tab(window))
      gtk_widget_grab_focus(GTK_WIDGET(active_tab));
  }

  window->tab_overview_animating = false;
}

static AdwTabPage*
terminal_window_tab_overview_create_tab_cb(AdwTabOverview* tab_overview,
                                           TerminalWindow* window)
{
  auto const tab = terminal_window_new_tab(window, "tab", "default", false);
  return adw_tab_view_get_page (terminal_notebook_get_tab_view(window->notebook),
                                GTK_WIDGET(tab));
}

/* GAction callbacks */

/* Opens a tab with the default profile in a container; the parameter is
 * (runtime, name), see terminal-containers.cc */
static void
action_new_terminal_in_container_cb(GSimpleAction* action,
                                    GVariant* parameter,
                                    gpointer user_data)
{
  auto const window = TERMINAL_WINDOW(user_data);
  char const* runtime;
  char const* name;
  g_variant_get(parameter, "(&s&s)", &runtime, &name);

  gs_unref_object auto profile =
    terminal_settings_list_ref_default_child(terminal_app_get_profiles_list(terminal_app_get()));
  if (!profile)
    return;

  auto const parent_screen = window->active_screen;
  auto const screen = terminal_screen_new(profile, nullptr, 1.0);
  terminal_screen_set_container(screen, runtime, name);

  auto const tab = terminal_tab_new(screen);
  terminal_window_add_tab(window, tab, parent_screen ? terminal_tab_get_from_screen(parent_screen) : nullptr);
  terminal_window_switch_screen(window, screen);

  if (parent_screen) {
    terminal_screen_reexec_from_screen(screen, parent_screen, nullptr, nullptr);
  } else {
    terminal_screen_exec(screen, nullptr, nullptr, true, g_get_home_dir(), false,
                         nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
  }
}

static void
action_new_terminal_cb (GSimpleAction *action,
                        GVariant      *parameter,
                        gpointer      user_data)
{
  const char *mode_str, *uuid_str;
  g_variant_get (parameter, "(&s&s)", &mode_str, &uuid_str);

  auto const window = reinterpret_cast<TerminalWindow*>(user_data);
  terminal_window_new_tab(window, mode_str, uuid_str, true);
}

/* Save Contents */

// The folder of the last saved file, to start there the next time
static GFile* last_save_folder;

static void
window_show_toast(TerminalWindow* window,
                  char const* title)
{
  adw_toast_overlay_add_toast(window->toast_overlay, adw_toast_new(title));
}

static void
save_contents_written_cb(GObject* source,
                         GAsyncResult* result,
                         void* user_data)
{
  gs_unref_object auto window = TERMINAL_WINDOW(user_data);
  auto const file = G_FILE(source);
  gs_free_error GError* error = nullptr;

  if (!g_file_replace_contents_finish(file, result, nullptr, &error)) {
    /* Translators: %s is an error message */
    gs_free char* title = g_strdup_printf(_("Could not save the contents: %s"), error->message);
    window_show_toast(window, title);
    return;
  }

  gs_free char* name = g_file_get_basename(file);
  /* Translators: %s is a file name */
  gs_free char* title = g_strdup_printf(_("Saved “%s”"), name);
  window_show_toast(window, title);
}

static void
save_contents_file_chosen_cb(GObject* source,
                             GAsyncResult* result,
                             void* user_data)
{
  gs_unref_object auto screen = TERMINAL_SCREEN(user_data);
  gs_unref_object auto file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, nullptr);
  if (!file) // cancelled
    return;

  g_clear_object(&last_save_folder);
  last_save_folder = g_file_get_parent(file);

  // The terminal may have closed meanwhile
  auto window = reinterpret_cast<TerminalWindow*>(gtk_widget_get_ancestor(GTK_WIDGET(screen),
                                                                          TERMINAL_TYPE_WINDOW));
  if (!window)
    return;

  // Collecting the text needs the terminal, so it happens here, but only
  // in memory, which is quick; writing the file, which may take long, runs
  // in the background.
  gs_free_error GError* error = nullptr;
  gs_unref_object auto stream = g_memory_output_stream_new_resizable();
  if (!vte_terminal_write_contents_sync(VTE_TERMINAL(screen), stream,
                                        VTE_WRITE_DEFAULT, nullptr, &error) ||
      !g_output_stream_close(stream, nullptr, &error)) {
    /* Translators: %s is an error message */
    gs_free char* title = g_strdup_printf(_("Could not save the contents: %s"), error->message);
    window_show_toast(window, title);
    return;
  }

  gs_unref_bytes auto bytes =
    g_memory_output_stream_steal_as_bytes(G_MEMORY_OUTPUT_STREAM(stream));
  g_file_replace_contents_bytes_async(file, bytes, nullptr, false,
                                      G_FILE_CREATE_REPLACE_DESTINATION,
                                      nullptr,
                                      save_contents_written_cb,
                                      g_object_ref(window));
}

static void
action_save_contents_cb (GSimpleAction *action,
                         GVariant *parameter,
                         gpointer user_data)
{
  auto const window = TERMINAL_WINDOW(user_data);
  if (window->active_screen == nullptr)
    return;

  gs_unref_object auto dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, _("Save Contents"));
  gtk_file_dialog_set_modal(dialog, true);

  if (last_save_folder) {
    gtk_file_dialog_set_initial_folder(dialog, last_save_folder);
  } else {
    // Without the dialog would start in the server's folder
    auto const documents = g_get_user_special_dir(G_USER_DIRECTORY_DOCUMENTS);
    gs_unref_object auto folder =
      g_file_new_for_path(documents && g_file_test(documents, G_FILE_TEST_IS_DIR)
                          ? documents : g_get_home_dir());
    gtk_file_dialog_set_initial_folder(dialog, folder);
  }

  // Name the file after the terminal, as far as that makes a file name
  gs_free char* name = g_strdup(terminal_screen_get_title(window->active_screen));
  g_strdelimit(name, "/\\:", '-');
  auto base = g_strstrip(name);
  while (*base == '~' || *base == '-' || *base == '.' || *base == ' ')
    ++base;
  gs_free char* file_name = g_strconcat(base[0] ? base : "terminal", ".txt", nullptr);
  gtk_file_dialog_set_initial_name(dialog, file_name);

  gtk_file_dialog_save(dialog, GTK_WINDOW(window), nullptr,
                       save_contents_file_chosen_cb,
                       g_object_ref(window->active_screen));
}

static void
action_close_cb (GSimpleAction *action,
                 GVariant *parameter,
                 gpointer user_data)
{
  TerminalWindow *window = (TerminalWindow*)user_data;
  const char *mode_str;

  terminal_assert_nonnull (parameter);
  g_variant_get (parameter, "&s", &mode_str);

  if (g_str_equal (mode_str, "tab")) {
    // Closes all panes of the tab. The tab view asks for confirmation
    // through the close request.
    if (window->active_screen)
      terminal_notebook_close_tab(window->notebook,
                                  terminal_tab_get_from_screen(window->active_screen));
  } else if (g_str_equal (mode_str, "other-tabs")) {
    if (window->active_screen)
      window_close_other_tabs(window, terminal_tab_get_from_screen(window->active_screen));
  } else if (g_str_equal (mode_str, "window")) {
    if (confirm_close_window_or_tab (window, nullptr, CloseKind::WINDOW))
      return;

    gtk_window_destroy (GTK_WINDOW (window));
  }
}

/* Returns: (transfer container): the tabs that closing the tabs other than
 * @tab closes; pinned tabs stay */
static GList*
window_list_other_tabs(TerminalWindow* window,
                       TerminalTab* tab)
{
  GList* others = nullptr;
  auto const tabs = terminal_notebook_list_tabs(window->notebook);
  for (auto l = tabs; l != nullptr; l = l->next) {
    auto const other = TERMINAL_TAB(l->data);
    if (other != tab && !terminal_tab_get_pinned(other))
      others = g_list_prepend(others, other);
  }
  g_list_free(tabs);

  return g_list_reverse(others);
}

static void
window_close_other_tabs_confirmed(TerminalWindow* window,
                                  TerminalTab* tab)
{
  // The tab may have gone while the user was asked
  if (!gtk_widget_is_ancestor(GTK_WIDGET(tab), GTK_WIDGET(window->notebook)))
    return;

  terminal_notebook_set_active_screen(window->notebook, terminal_tab_get_screen(tab));

  // Each tab closes at once, and can be reopened like any closed tab
  auto const others = window_list_other_tabs(window, tab);
  window->closing_confirmed = true;
  for (auto l = others; l != nullptr; l = l->next)
    terminal_notebook_close_tab(window->notebook, TERMINAL_TAB(l->data));
  window->closing_confirmed = false;
  g_list_free(others);
}

/* Closes the tabs other than @tab, except the pinned ones, asking first if
 * a process runs in any of them */
static void
window_close_other_tabs(TerminalWindow* window,
                        TerminalTab* tab)
{
  if (!tab)
    return;

  if (confirm_close_window_or_tab(window, terminal_tab_get_screen(tab), CloseKind::OTHER_TABS))
    return;

  window_close_other_tabs_confirmed(window, tab);
}

static TerminalTab*
window_get_active_tab(TerminalWindow* window)
{
  return window->active_screen ? terminal_tab_get_from_screen(window->active_screen) : nullptr;
}

static bool
parse_direction(GVariant* parameter,
                GtkDirectionType* direction)
{
  auto const str = g_variant_get_string(parameter, nullptr);
  if (g_str_equal(str, "left"))
    *direction = GTK_DIR_LEFT;
  else if (g_str_equal(str, "right"))
    *direction = GTK_DIR_RIGHT;
  else if (g_str_equal(str, "up"))
    *direction = GTK_DIR_UP;
  else if (g_str_equal(str, "down"))
    *direction = GTK_DIR_DOWN;
  else
    return false;

  return true;
}

static void
action_split_cb(GSimpleAction* action,
                GVariant* parameter,
                gpointer user_data)
{
  auto const window = TERMINAL_WINDOW(user_data);
  auto const parent_screen = window->active_screen;
  if (!parent_screen)
    return;

  GtkDirectionType direction;
  if (!parse_direction(parameter, &direction))
    return;

  auto const orientation = direction == GTK_DIR_DOWN
    ? GTK_ORIENTATION_VERTICAL
    : GTK_ORIENTATION_HORIZONTAL;

  // The new terminal gets the profile, zoom and working directory of the
  // active one.
  gs_unref_object auto profile = terminal_screen_ref_profile(parent_screen);
  auto const screen = terminal_screen_new(profile,
                                          nullptr /* title */,
                                          vte_terminal_get_font_scale(VTE_TERMINAL(parent_screen)));

  window_preserve_container(screen, parent_screen);
  terminal_tab_split(terminal_tab_get_from_screen(parent_screen),
                     parent_screen,
                     screen,
                     orientation,
                     0.5);
  gtk_widget_grab_focus(GTK_WIDGET(screen));

  terminal_screen_reexec_from_screen(screen, parent_screen, nullptr, nullptr);

  terminal_window_update_pane_actions(window);
}

static void
action_prompt_cb(GSimpleAction* action,
                 GVariant* parameter,
                 gpointer user_data)
{
  auto const window = TERMINAL_WINDOW(user_data);
  if (!window->active_screen)
    return;

  auto const direction = g_str_equal(g_variant_get_string(parameter, nullptr), "previous") ? -1 : 1;
  terminal_screen_scroll_to_prompt(window->active_screen, direction);
}

/* Tells that something was copied, unless turned off. A new notice
 * replaces the last one rather than waiting behind it. */
static void
window_show_copy_toast(TerminalWindow* window)
{
  auto const settings = terminal_app_get_global_settings(terminal_app_get());
  if (!g_settings_get_boolean(settings, TERMINAL_SETTING_TOAST_ON_COPY_KEY))
    return;

  if (window->copy_toast)
    adw_toast_dismiss(window->copy_toast);

  auto const toast = adw_toast_new(_("Copied to clipboard"));
  adw_toast_set_timeout(toast, 2);
  window->copy_toast = toast;
  g_object_add_weak_pointer(G_OBJECT(toast), reinterpret_cast<void**>(&window->copy_toast));
  adw_toast_overlay_add_toast(window->toast_overlay, toast);
}

static void
action_copy_output_cb(GSimpleAction* action,
                      GVariant* parameter,
                      gpointer user_data)
{
  auto const window = TERMINAL_WINDOW(user_data);
  if (!window->active_screen)
    return;

  gs_free char* text = terminal_screen_dup_command_output(window->active_screen);
  if (!text)
    return;

  gdk_clipboard_set_text(window->clipboard, text);
  window_show_copy_toast(window);
}

static void
action_copy_output_here_cb(GSimpleAction* action,
                           GVariant* parameter,
                           gpointer user_data)
{
  auto const window = TERMINAL_WINDOW(user_data);
  if (!window->active_screen || !window->popup_info)
    return;

  gs_free char* text = terminal_screen_dup_command_output_at(window->active_screen,
                                                             window->popup_info->row);
  if (!text)
    return;

  gdk_clipboard_set_text(window->clipboard, text);
  window_show_copy_toast(window);
}

/* Prompt jumping and copying command output need shell integration. The
 * shortcuts are only taken from the terminal while they can work. */
static void
terminal_window_update_prompt_actions(TerminalWindow* window)
{
  if (window->disposed)
    return;

  auto const screen = window->active_screen;
  auto const has_prompts = screen && terminal_screen_has_prompts(screen);
  auto const has_output = screen && terminal_screen_has_command_output(screen);

  g_simple_action_set_enabled(lookup_action(window, "scroll-to-prompt"), has_prompts);
  g_simple_action_set_enabled(lookup_action(window, "copy-output"), has_output);
  g_simple_action_set_enabled(lookup_action(window, "shadow-prompt"), has_prompts);
}

static void
screen_prompts_changed_cb(TerminalScreen* screen,
                          TerminalWindow* window)
{
  if (screen == window->active_screen)
    terminal_window_update_prompt_actions(window);
}

static void
action_focus_tab_list_cb(GSimpleAction* action,
                         GVariant* parameter,
                         gpointer user_data)
{
  auto const window = TERMINAL_WINDOW(user_data);
  terminal_tab_sidebar_focus_list(window->tab_sidebar);
}

/* After a menu closes, GTK gives the focus to the button that opened it,
 * or to the first button of the header bar for the context menu, so that
 * Enter or Space would then open a tab. Give it back to the terminal,
 * unless the menu item has moved it elsewhere, such as into a dialog or
 * the search bar.
 */
static gboolean
refocus_terminal_idle_cb(gpointer user_data)
{
  auto const window = TERMINAL_WINDOW(user_data);
  window->refocus_idle = 0;

  if (window->disposed || !window->active_screen)
    return G_SOURCE_REMOVE;

  auto const focus = gtk_root_get_focus(GTK_ROOT(window));
  if (!focus ||
      gtk_widget_is_ancestor(focus, GTK_WIDGET(window->headerbar)) ||
      (window->context_menu &&
       (focus == GTK_WIDGET(window->context_menu) ||
        gtk_widget_is_ancestor(focus, GTK_WIDGET(window->context_menu)))))
    gtk_widget_grab_focus(GTK_WIDGET(window->active_screen));

  return G_SOURCE_REMOVE;
}

static void
menu_closed_cb(GtkPopover* popover,
               TerminalWindow* window)
{
  if (window->refocus_idle == 0)
    window->refocus_idle = g_idle_add(refocus_terminal_idle_cb, window);
}

/* F10 and the Show Primary Menu shortcut. In fullscreen, where the header
 * bar is hidden, the context menu of the terminal opens instead.
 */
static void
action_header_menu_cb(GSimpleAction* action,
                      GVariant* parameter,
                      gpointer user_data)
{
  auto const window = TERMINAL_WINDOW(user_data);
  if (adw_toolbar_view_get_reveal_top_bars(window->toolbar_view) &&
      terminal_headerbar_popup_menu(window->headerbar))
    return;

  if (window->active_screen)
    gtk_widget_activate_action(GTK_WIDGET(window->active_screen), "menu.popup", nullptr);
}

static void
action_close_pane_cb(GSimpleAction* action,
                     GVariant* parameter,
                     gpointer user_data)
{
  auto const window = TERMINAL_WINDOW(user_data);
  auto const tab = window_get_active_tab(window);
  if (!tab || !terminal_tab_is_split(tab))
    return;

  auto const screen = window->active_screen;
  if (confirm_close_window_or_tab(window, screen, CloseKind::PANE))
    return;

  terminal_window_remove_screen(window, screen);
}

static void
action_pane_focus_cb(GSimpleAction* action,
                     GVariant* parameter,
                     gpointer user_data)
{
  auto const window = TERMINAL_WINDOW(user_data);
  auto const tab = window_get_active_tab(window);
  GtkDirectionType direction;
  if (tab && parse_direction(parameter, &direction))
    terminal_tab_focus_pane(tab, direction);
}

static void
action_pane_resize_cb(GSimpleAction* action,
                      GVariant* parameter,
                      gpointer user_data)
{
  auto const window = TERMINAL_WINDOW(user_data);
  auto const tab = window_get_active_tab(window);
  GtkDirectionType direction;
  if (tab && parse_direction(parameter, &direction))
    terminal_tab_resize_pane(tab, direction);
}

static void
action_zoom_pane_state_cb(GSimpleAction* action,
                          GVariant* state,
                          gpointer user_data)
{
  auto const window = TERMINAL_WINDOW(user_data);
  auto const tab = window_get_active_tab(window);
  if (!tab)
    return;

  terminal_tab_set_zoomed(tab, g_variant_get_boolean(state));
  terminal_window_update_pane_actions(window);
}

/* Makes the window background transparent while the active tab has a
 * translucent terminal, so that the desktop shows through it. Header bar,
 * tab bars and side bar keep their background, see terminal.common.css.
 */
static void
window_sync_translucency(TerminalWindow* window)
{
  if (window->disposed)
    return;

  auto translucent = false;
  if (auto const tab = window_get_active_tab(window)) {
    auto const screens = terminal_tab_list_screens(tab);
    for (auto l = screens; l && !translucent; l = l->next) {
      gs_unref_object auto profile = terminal_screen_ref_profile(TERMINAL_SCREEN(l->data));
      translucent = profile &&
        g_settings_get_double(profile, TERMINAL_PROFILE_BACKGROUND_OPACITY_KEY) < 1.0;
    }
    g_list_free(screens);
  }

  if (translucent)
    gtk_widget_add_css_class(GTK_WIDGET(window), "translucent");
  else
    gtk_widget_remove_css_class(GTK_WIDGET(window), "translucent");
}

/* Colors the header bar while the active terminal runs as root or over SSH */
static void
window_sync_process_kind(TerminalWindow* window)
{
  if (window->disposed)
    return;

  auto const kind = window->active_screen
    ? terminal_screen_get_process_kind(window->active_screen)
    : TERMINAL_PROCESS_KIND_NORMAL;

  auto const widget = GTK_WIDGET(window);
  if (kind == TERMINAL_PROCESS_KIND_ROOT)
    gtk_widget_add_css_class(widget, "root-session");
  else
    gtk_widget_remove_css_class(widget, "root-session");
  if (kind == TERMINAL_PROCESS_KIND_REMOTE)
    gtk_widget_add_css_class(widget, "remote-session");
  else
    gtk_widget_remove_css_class(widget, "remote-session");
}

/* Gives the header bar, the tab bar and the tab list the colors of the
 * active terminal, when it has colors of its own. The colors of root and
 * remote sessions take precedence. */
static void
window_sync_colors(TerminalWindow* window)
{
  if (window->disposed)
    return;

  auto const settings = terminal_app_get_global_settings(terminal_app_get());
  GdkRGBA fg, bg;
  auto const custom = window->active_screen &&
    g_settings_get_boolean(settings, TERMINAL_SETTING_HEADER_BAR_COLORS_KEY) &&
    terminal_screen_get_custom_colors(window->active_screen, &fg, &bg);

  if (!custom) {
    gtk_widget_remove_css_class(GTK_WIDGET(window), "terminal-colors");
    return;
  }

  if (!window->colors_provider) {
    static guint n_windows = 0;
    window->colors_class = g_strdup_printf("pano-window-%u", ++n_windows);
    gtk_widget_add_css_class(GTK_WIDGET(window), window->colors_class);
    window->colors_provider = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(gtk_widget_get_display(GTK_WIDGET(window)),
                                               GTK_STYLE_PROVIDER(window->colors_provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
  }

  gs_free char* fg_str = gdk_rgba_to_string(&fg);
  gs_free char* bg_str = gdk_rgba_to_string(&bg);
  gs_free char* css = g_strdup_printf(
    "window.terminal-window.%1$s.terminal-colors:not(.root-session):not(.remote-session) toolbarview .top-bar,"
    "window.terminal-window.%1$s.terminal-colors:not(.root-session):not(.remote-session) tabbar .box,"
    "window.terminal-window.%1$s.terminal-colors:not(.root-session):not(.remote-session) tabsidebar"
    " { background-color: %2$s; color: %3$s; }",
    window->colors_class, bg_str, fg_str);
  gtk_css_provider_load_from_string(window->colors_provider, css);
  gtk_widget_add_css_class(GTK_WIDGET(window), "terminal-colors");
}

static void
screen_colors_changed_cb(TerminalScreen* screen,
                         TerminalWindow* window)
{
  if (screen == window->active_screen)
    window_sync_colors(window);
}

static void
window_header_bar_colors_changed_cb(GSettings* settings,
                                    char const* key,
                                    TerminalWindow* window)
{
  window_sync_colors(window);
}

static void
screen_process_kind_changed_cb(TerminalScreen* screen,
                               GParamSpec* pspec,
                               TerminalWindow* window)
{
  if (screen == window->active_screen)
    window_sync_process_kind(window);
}

static void
profile_opacity_changed_cb(TerminalWindow* window)
{
  window_sync_translucency(window);
}

static void
terminal_window_update_pane_actions(TerminalWindow* window)
{
  if (window->disposed)
    return;

  auto const tab = window_get_active_tab(window);
  auto const split = tab && terminal_tab_is_split(tab);
  auto const zoomed = tab && terminal_tab_get_zoomed(tab);

  g_simple_action_set_enabled(lookup_action(window, "split"), tab != nullptr);
  g_simple_action_set_enabled(lookup_action(window, "close-pane"), split);
  g_simple_action_set_enabled(lookup_action(window, "zoom-pane"), split);
  g_simple_action_set_enabled(lookup_action(window, "equalize-panes"), split);
  g_simple_action_set_enabled(lookup_action(window, "move-pane"), split);
  g_simple_action_set_enabled(lookup_action(window, "pane-focus"), split && !zoomed);
  g_simple_action_set_enabled(lookup_action(window, "pane-resize"), split && !zoomed);
  g_simple_action_set_enabled(lookup_action(window, "swap-pane"), split && !zoomed);
  // Only take the pane shortcuts away from the terminal when there are panes
  g_simple_action_set_enabled(lookup_action(window, "shadow-pane"), split);
  g_simple_action_set_state(lookup_action(window, "zoom-pane"),
                            g_variant_new_boolean(zoomed));

  // Panes come and go with the same calls
  window_sync_translucency(window);
  window_sync_process_kind(window);
  window_sync_colors(window);
  window_update_broadcast(window);
}

static void
action_copy_cb (GSimpleAction *action,
                GVariant *parameter,
                gpointer user_data)
{
  TerminalWindow *window = (TerminalWindow*)user_data;
  const char *format_str;
  VteFormat format;

  if (window->active_screen == nullptr)
    return;

  terminal_assert_nonnull (parameter);
  g_variant_get (parameter, "&s", &format_str);

  if (g_str_equal (format_str, "text"))
    format = VTE_FORMAT_TEXT;
  else if (g_str_equal (format_str, "html"))
    format = VTE_FORMAT_HTML;
  else
    return;

  if (!vte_terminal_get_has_selection (VTE_TERMINAL (window->active_screen)))
    return;

  vte_terminal_copy_clipboard_format (VTE_TERMINAL (window->active_screen), format);
  window_show_copy_toast (window);
}

/* Clipboard helpers */

typedef struct {
  GWeakRef screen_weak_ref;
} PasteData;

static void
clipboard_uris_received_cb (GObject *object,
                            GAsyncResult *result,
                            gpointer user_data)
{
  GdkClipboard *clipboard = GDK_CLIPBOARD (object);
  PasteData *data = (PasteData *)user_data;
  gs_unref_object TerminalScreen *screen = nullptr;
  const GValue *value;

  if ((value = gdk_clipboard_read_value_finish (clipboard, result, nullptr)) &&
      G_VALUE_HOLDS (value, GDK_TYPE_FILE_LIST) &&
      (screen = (TerminalScreen*)g_weak_ref_get (&data->screen_weak_ref))) {
    const GList *uris = (const GList *)g_value_get_boxed (value);
    g_autoptr(GString) string = g_string_new (nullptr);

    for (const GList *iter = uris; iter; iter = iter->next) {
      GFile *file = G_FILE (iter->data);

      if (g_file_is_native (file)) {
        g_autofree char *quoted = g_shell_quote (g_file_peek_path (file));

        g_string_append (string, quoted);
        g_string_append_c (string, ' ');
      } else {
        g_autofree char *uri = g_file_get_uri (file);
        g_autofree char *quoted = g_shell_quote (uri);

        g_string_append (string, quoted);
        g_string_append_c (string, ' ');
      }
    }

    terminal_screen_paste_text (screen, string->str, string->len);
  }

  g_weak_ref_clear (&data->screen_weak_ref);
  g_slice_free (PasteData, data);
}

/* Reopening closed tabs */

#define MAX_CLOSED_TABS 10

// The last closed tabs of all windows, most recent first: a{sv} with
// "tab" (see terminal_session_save_tab()) and "position" (i)
static GQueue closed_tabs = G_QUEUE_INIT;

static void
update_reopen_tab_actions(void)
{
  auto const can_reopen = !g_queue_is_empty(&closed_tabs);
  for (auto l = gtk_application_get_windows(GTK_APPLICATION(terminal_app_get())); l; l = l->next) {
    if (!TERMINAL_IS_WINDOW(l->data) || TERMINAL_WINDOW(l->data)->disposed)
      continue;
    g_simple_action_set_enabled(lookup_action(TERMINAL_WINDOW(l->data), "reopen-tab"), can_reopen);
  }
}

/* Remembers a tab that is about to close, and offers to undo that */
static void
window_remember_closed_tab(TerminalWindow* window,
                           TerminalTab* tab)
{
  auto const data = terminal_session_save_tab(tab);
  if (!data)
    return;

  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&builder, "{sv}", "tab", data);
  g_variant_builder_add(&builder, "{sv}", "position",
                        g_variant_new_int32(terminal_notebook_get_tab_position(window->notebook, tab)));
  g_queue_push_head(&closed_tabs, g_variant_ref_sink(g_variant_builder_end(&builder)));

  while (g_queue_get_length(&closed_tabs) > MAX_CLOSED_TABS)
    g_variant_unref(reinterpret_cast<GVariant*>(g_queue_pop_tail(&closed_tabs)));

  update_reopen_tab_actions();

  // The window goes away with its last tab. A newer toast replaces an
  // older one rather than waiting behind it.
  if (terminal_notebook_get_n_screens(window->notebook) > 1) {
    if (window->closed_tab_toast)
      adw_toast_dismiss(window->closed_tab_toast);

    auto const toast = adw_toast_new(_("Tab closed"));
    adw_toast_set_button_label(toast, _("_Undo"));
    adw_toast_set_action_name(toast, "win.reopen-tab");
    window->closed_tab_toast = toast;
    g_object_add_weak_pointer(G_OBJECT(toast), reinterpret_cast<void**>(&window->closed_tab_toast));
    adw_toast_overlay_add_toast(window->toast_overlay, toast);
  }
}

static void
action_reopen_tab_cb(GSimpleAction* action,
                     GVariant* parameter,
                     gpointer user_data)
{
  auto const window = TERMINAL_WINDOW(user_data);

  if (window->closed_tab_toast)
    adw_toast_dismiss(window->closed_tab_toast);

  gs_unref_variant auto record = reinterpret_cast<GVariant*>(g_queue_pop_head(&closed_tabs));
  update_reopen_tab_actions();
  if (!record)
    return;

  gs_unref_variant GVariant* data = g_variant_lookup_value(record, "tab", G_VARIANT_TYPE_VARDICT);
  int position = -1;
  g_variant_lookup(record, "position", "i", &position);

  if (auto const tab = data ? terminal_session_restore_tab(window, data, position) : nullptr)
    terminal_window_switch_screen(window, terminal_tab_get_screen(tab));
}

/* Input broadcasting */

// Set while sending input on to other terminals, so that it is not sent on again
static bool broadcasting = false;

/* Returns: (transfer container): the terminals that broadcast input goes
 * to, including the active one */
static GList*
window_broadcast_screens(TerminalWindow* window)
{
  GList* screens = nullptr;
  if (window->broadcast_mode == BROADCAST_WINDOW) {
    screens = terminal_notebook_list_screens(window->notebook);
  } else if (window->broadcast_mode == BROADCAST_TAB) {
    if (auto const tab = terminal_window_get_active_tab(window))
      screens = terminal_tab_list_screens(tab);
  }

  for (auto l = screens; l; ) {
    auto const next = l->next;
    if (terminal_screen_get_broadcast_excluded(TERMINAL_SCREEN(l->data)))
      screens = g_list_delete_link(screens, l);
    l = next;
  }

  return screens;
}

static void
screen_commit_cb(TerminalScreen* screen,
                 char const* text,
                 guint size,
                 TerminalWindow* window)
{
  if (broadcasting || window->broadcast_mode == BROADCAST_OFF ||
      !terminal_screen_is_user_input(screen, text, size))
    return;

  auto const screens = window_broadcast_screens(window);
  if (g_list_find(screens, screen)) {
    broadcasting = true;
    for (auto l = screens; l; l = l->next) {
      if (l->data != screen)
        vte_terminal_feed_child(VTE_TERMINAL(l->data), text, gssize(size));
    }
    broadcasting = false;
  }
  g_list_free(screens);
}

static void
broadcast_paste_cb(GObject* source,
                   GAsyncResult* result,
                   void* user_data)
{
  gs_unref_object auto window = TERMINAL_WINDOW(user_data);
  gs_free char* text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), result, nullptr);
  if (!text || window->disposed)
    return;

  // Every terminal pastes for itself, so that each uses bracketed paste
  // if its program asked for it.
  auto const screens = window_broadcast_screens(window);
  broadcasting = true;
  for (auto l = screens; l; l = l->next)
    vte_terminal_paste_text(VTE_TERMINAL(l->data), text);
  broadcasting = false;
  g_list_free(screens);
}

/* Pastes into all terminals that get broadcast input, if the active one does.
 * Returns: whether it did */
static bool
window_paste_broadcast(TerminalWindow* window)
{
  if (window->broadcast_mode == BROADCAST_OFF)
    return false;

  auto const screens = window_broadcast_screens(window);
  auto const active = g_list_find(screens, window->active_screen) != nullptr;
  g_list_free(screens);
  if (!active)
    return false;

  gdk_clipboard_read_text_async(window->clipboard, nullptr,
                                broadcast_paste_cb, g_object_ref(window));
  return true;
}

/* Shows which terminals get the input: a banner, and a frame around them */
static void
window_update_broadcast(TerminalWindow* window)
{
  if (window->disposed || !window->broadcast_banner)
    return;

  auto const all = terminal_notebook_list_screens(window->notebook);
  auto const screens = window_broadcast_screens(window);
  for (auto l = all; l; l = l->next)
    terminal_tab_set_screen_css_class(TERMINAL_SCREEN(l->data), "broadcast",
                                      g_list_find(screens, l->data) != nullptr);

  auto const n = g_list_length(screens);
  if (window->broadcast_mode == BROADCAST_OFF) {
    adw_banner_set_revealed(window->broadcast_banner, false);
  } else {
    gs_free char* title = n == 0
      ? g_strdup(_("Broadcasting input, but no terminal takes part"))
      : g_strdup_printf(g_dngettext(GETTEXT_PACKAGE,
                                    "Input goes to %u terminal",
                                    "Input goes to %u terminals", n), n);
    adw_banner_set_title(window->broadcast_banner, title);
    adw_banner_set_revealed(window->broadcast_banner, true);
  }

  auto const receive = window->active_screen &&
    !terminal_screen_get_broadcast_excluded(window->active_screen);
  g_simple_action_set_state(lookup_action(window, "broadcast-receive"),
                            g_variant_new_boolean(receive));
  g_simple_action_set_enabled(lookup_action(window, "broadcast-receive"),
                              window->broadcast_mode != BROADCAST_OFF);

  g_list_free(screens);
  g_list_free(all);
}

static void
action_broadcast_cb(GSimpleAction* action,
                    GVariant* parameter,
                    gpointer user_data)
{
  // The same mode again turns broadcasting off, for toggling with a shortcut
  gs_unref_variant auto state = g_action_get_state(G_ACTION(action));
  if (g_variant_equal(state, parameter))
    g_action_change_state(G_ACTION(action), g_variant_new_string("off"));
  else
    g_action_change_state(G_ACTION(action), parameter);
}

static void
action_broadcast_state_cb(GSimpleAction* action,
                          GVariant* state,
                          gpointer user_data)
{
  auto const window = TERMINAL_WINDOW(user_data);
  auto const mode = g_variant_get_string(state, nullptr);

  if (g_str_equal(mode, "tab"))
    window->broadcast_mode = BROADCAST_TAB;
  else if (g_str_equal(mode, "window"))
    window->broadcast_mode = BROADCAST_WINDOW;
  else
    window->broadcast_mode = BROADCAST_OFF;

  g_simple_action_set_state(action, state);
  window_update_broadcast(window);
}

static void
action_broadcast_receive_state_cb(GSimpleAction* action,
                                  GVariant* state,
                                  gpointer user_data)
{
  auto const window = TERMINAL_WINDOW(user_data);
  if (!window->active_screen)
    return;

  terminal_screen_set_broadcast_excluded(window->active_screen,
                                         !g_variant_get_boolean(state));
  window_update_broadcast(window);
}

static void
request_clipboard_contents_for_paste (TerminalWindow *window,
                                      gboolean paste_as_uris)
{
  GdkContentFormats *targets;

  if (window->active_screen == nullptr)
    return;

  targets = terminal_app_get_clipboard_targets (terminal_app_get (),
                                                window->clipboard);
  if (targets == nullptr)
    return;

  if (paste_as_uris && gdk_content_formats_contain_gtype (targets, GDK_TYPE_FILE_LIST)) {
    PasteData *data = g_slice_new (PasteData);
    g_weak_ref_init (&data->screen_weak_ref, window->active_screen);

    gdk_clipboard_read_value_async (window->clipboard,
                                    GDK_TYPE_FILE_LIST,
                                    G_PRIORITY_DEFAULT,
                                    nullptr,
                                    clipboard_uris_received_cb,
                                    data);
    return;
  } else if (gdk_content_formats_contain_gtype (targets, G_TYPE_STRING)) {
    if (window_paste_broadcast (window))
      return;

    vte_terminal_paste_clipboard (VTE_TERMINAL (window->active_screen));
  }
}

static void
action_paste_text_cb (GSimpleAction *action,
                      GVariant *parameter,
                      gpointer user_data)
{
  TerminalWindow *window = (TerminalWindow*)user_data;

  request_clipboard_contents_for_paste (window, FALSE);
}

static void
action_paste_uris_cb (GSimpleAction *action,
                      GVariant *parameter,
                      gpointer user_data)
{
  TerminalWindow *window = (TerminalWindow*)user_data;

  request_clipboard_contents_for_paste (window, TRUE);
}

static void
action_select_all_cb (GSimpleAction *action,
                      GVariant *parameter,
                      gpointer user_data)
{
  TerminalWindow *window = (TerminalWindow*)user_data;

  if (window->active_screen == nullptr)
    return;

  vte_terminal_select_all (VTE_TERMINAL (window->active_screen));
}

static void
action_reset_cb (GSimpleAction *action,
                 GVariant *parameter,
                 gpointer user_data)
{
  TerminalWindow *window = (TerminalWindow*)user_data;

  terminal_assert_nonnull (parameter);

  if (window->active_screen == nullptr)
    return;

  vte_terminal_reset (VTE_TERMINAL (window->active_screen),
                      TRUE,
                      g_variant_get_boolean (parameter));
}

static void
tab_switch_relative (TerminalWindow *window,
                     int change)
{
  int n_screens, value;

  n_screens = terminal_notebook_get_n_screens (window->notebook);

  if (gtk_widget_get_direction(GTK_WIDGET(window->notebook)) == GTK_TEXT_DIR_RTL)
    change = -change;
  value = terminal_notebook_get_active_screen_num (window->notebook) + change;

  if (value < 0)
    value += n_screens;
  else if (value >= n_screens)
    value -= n_screens;

  if (value < 0 || value >= n_screens)
    return;

  g_action_change_state (G_ACTION (lookup_action (window, "active-tab")),
                         g_variant_new_int32 (value));
}

static void
action_tab_switch_left_cb (GSimpleAction *action,
                           GVariant *parameter,
                           gpointer user_data)
{
  TerminalWindow *window = (TerminalWindow*)user_data;

  tab_switch_relative (window, -1);
}

static void
action_tab_switch_right_cb (GSimpleAction *action,
                            GVariant *parameter,
                            gpointer user_data)
{
  TerminalWindow *window = (TerminalWindow*)user_data;

  tab_switch_relative (window, 1);
}

static void
action_tab_move_left_cb (GSimpleAction *action,
                         GVariant *parameter,
                         gpointer user_data)
{
  TerminalWindow *window = (TerminalWindow*)user_data;

  if (window->active_screen == nullptr)
    return;

  terminal_notebook_reorder_screen (window->notebook,
                                    terminal_notebook_get_active_screen (window->notebook),
                                    -1);
}

static void
action_tab_move_right_cb (GSimpleAction *action,
                          GVariant *parameter,
                          gpointer user_data)
{
  TerminalWindow *window = (TerminalWindow*)user_data;

  if (window->active_screen == nullptr)
    return;

  terminal_notebook_reorder_screen (window->notebook,
                                    terminal_notebook_get_active_screen (window->notebook),
                                    1);
}

static void
action_tab_move_start_cb(GSimpleAction* action,
                         GVariant* parameter,
                         void* user_data)
{
  auto const window = TERMINAL_WINDOW(user_data);

  if (!window->active_screen)
    return;

  terminal_notebook_reorder_screen_limits(window->notebook,
                                          terminal_notebook_get_active_screen(window->notebook),
                                          -1);
}

static void
action_tab_move_end_cb(GSimpleAction* action,
                       GVariant* parameter,
                       void* user_data)
{
  auto const window = TERMINAL_WINDOW(user_data);

  if (!window->active_screen)
    return;

  terminal_notebook_reorder_screen_limits(window->notebook,
                                          terminal_notebook_get_active_screen(window->notebook),
                                          1);
}

static void
action_zoom_in_cb (GSimpleAction *action,
                   GVariant *parameter,
                   gpointer user_data)
{
  TerminalWindow *window = (TerminalWindow*)user_data;
  double zoom;

  if (window->active_screen == nullptr)
    return;

  zoom = vte_terminal_get_font_scale (VTE_TERMINAL (window->active_screen));
  if (!find_larger_zoom_factor (&zoom))
    return;

  gtk_window_set_default_size (GTK_WINDOW (window), -1, -1);
  vte_terminal_set_font_scale (VTE_TERMINAL (window->active_screen), zoom);
  terminal_window_update_zoom_sensitivity (window);
}

static void
action_zoom_out_cb (GSimpleAction *action,
                    GVariant *parameter,
                    gpointer user_data)
{
  TerminalWindow *window = (TerminalWindow*)user_data;
  double zoom;

  if (window->active_screen == nullptr)
    return;

  zoom = vte_terminal_get_font_scale (VTE_TERMINAL (window->active_screen));
  if (!find_smaller_zoom_factor (&zoom))
    return;

  gtk_window_set_default_size (GTK_WINDOW (window), -1, -1);
  vte_terminal_set_font_scale (VTE_TERMINAL (window->active_screen), zoom);
  terminal_window_update_zoom_sensitivity (window);
}

static void
action_zoom_normal_cb (GSimpleAction *action,
                       GVariant *parameter,
                       gpointer user_data)
{
  TerminalWindow *window = (TerminalWindow*)user_data;

  if (window->active_screen == nullptr)
    return;

  vte_terminal_set_font_scale (VTE_TERMINAL (window->active_screen), PANGO_SCALE_MEDIUM);
  terminal_window_update_zoom_sensitivity (window);
}

static void
detach_tab(TerminalWindow* window,
           TerminalTab* tab)
{
  auto const screen = terminal_tab_get_screen(tab);

  int width, height;
  terminal_screen_get_size (screen, &width, &height);
  char geometry[32];
  g_snprintf(geometry, sizeof(geometry), "%dx%d", width, height);

  auto const new_window = terminal_window_new(G_APPLICATION(terminal_app_get()));

  terminal_notebook_transfer_screen(window->notebook,
                                    screen,
                                    new_window->notebook,
                                    0);

  terminal_window_parse_geometry (new_window, geometry);

  gtk_window_present (GTK_WINDOW (new_window));
}

static void
action_quick_select_cb(GSimpleAction* action,
                       GVariant* parameter,
                       gpointer user_data)
{
  auto const window = TERMINAL_WINDOW(user_data);
  if (!window->active_screen)
    return;

  if (!terminal_screen_start_quick_select(window->active_screen))
    window_show_toast(window, _("Nothing to select"));
}

/* Quick select picked @text: copy it, paste it into the terminal, or open
 * it if it is a URL or a path */
static void
screen_quick_select_chosen_cb(TerminalScreen* screen,
                              char const* text,
                              int action,
                              gboolean openable,
                              TerminalWindow* window)
{
  if (action == TERMINAL_QUICK_SELECT_PASTE) {
    vte_terminal_paste_text(VTE_TERMINAL(screen), text);
    return;
  }

  if (action == TERMINAL_QUICK_SELECT_OPEN && openable) {
    if (strstr(text, "://")) {
      terminal_util_open_url(GTK_WIDGET(window), text, FLAVOR_AS_IS, GDK_CURRENT_TIME);
      return;
    }

    // A path, from the home folder or the terminal's working directory
    gs_free char* path = nullptr;
    if (g_str_has_prefix(text, "~/") || g_str_equal(text, "~")) {
      path = g_build_filename(g_get_home_dir(), text + 1, nullptr);
    } else if (g_path_is_absolute(text)) {
      path = g_strdup(text);
    } else {
      gs_free char* cwd = terminal_screen_get_current_dir(screen);
      path = g_build_filename(cwd ? cwd : g_get_home_dir(), text, nullptr);
    }

    if (g_file_test(path, G_FILE_TEST_EXISTS)) {
      gs_free char* uri = g_filename_to_uri(path, nullptr, nullptr);
      if (uri) {
        terminal_util_open_url(GTK_WIDGET(window), uri, FLAVOR_AS_IS, GDK_CURRENT_TIME);
        return;
      }
    }

    /* Translators: %s is a file name */
    gs_free char* title = g_strdup_printf(_("“%s” does not exist"), text);
    window_show_toast(window, title);
    return;
  }

  gdk_clipboard_set_text(window->clipboard, text);
  window_show_copy_toast(window);
}

static void
action_command_palette_cb(GSimpleAction* action,
                          GVariant* parameter,
                          gpointer user_data)
{
  auto const window = TERMINAL_WINDOW(user_data);
  terminal_command_palette_show(window, GTK_WIDGET(window->toast_overlay));
}

static void
action_equalize_panes_cb(GSimpleAction* action,
                         GVariant* parameter,
                         gpointer user_data)
{
  auto const window = TERMINAL_WINDOW(user_data);
  if (auto const tab = window_get_active_tab(window))
    terminal_tab_equalize(tab);
}

/**
 * terminal_window_move_pane:
 * @window: a #TerminalWindow
 * @screen: a pane of a split tab in @window
 * @new_window: whether to move it into a new window
 *
 * Moves the pane of @screen, with what runs in it, into a new tab next to
 * its old one, or into a new window.
 */
void
terminal_window_move_pane(TerminalWindow* window,
                          TerminalScreen* screen,
                          gboolean new_window)
{
  g_return_if_fail(TERMINAL_IS_WINDOW(window));

  auto const tab = terminal_tab_get_from_screen(screen);
  if (!screen || !tab || !terminal_tab_is_split(tab))
    return;

  gs_unref_object auto taken = terminal_tab_take_screen(tab, screen);
  if (!taken)
    return;

  auto const new_tab = terminal_tab_new(taken);
  terminal_notebook_insert_tab(window->notebook, new_tab, tab, false);
  terminal_notebook_set_active_screen(window->notebook, taken);
  gtk_widget_grab_focus(GTK_WIDGET(taken));

  if (new_window)
    detach_tab(window, new_tab);

  terminal_window_update_pane_actions(window);
}

static void
action_move_pane_cb(GSimpleAction* action,
                    GVariant* parameter,
                    gpointer user_data)
{
  auto const window = TERMINAL_WINDOW(user_data);
  terminal_window_move_pane(window, window->active_screen,
                            g_str_equal(g_variant_get_string(parameter, nullptr), "window"));
}

static void
action_swap_pane_cb(GSimpleAction* action,
                    GVariant* parameter,
                    gpointer user_data)
{
  auto const window = TERMINAL_WINDOW(user_data);
  auto const tab = window_get_active_tab(window);
  GtkDirectionType direction;
  if (tab && parse_direction(parameter, &direction))
    terminal_tab_swap_pane(tab, direction);
}

/* Drag and drop of panes. A pane dragged by its title bar can go next to
 * another pane, in this or another tab or window, take the place of a pane
 * of its own tab, or become a tab. A tab dropped onto a pane becomes panes
 * there. See also the drag source and drop target in terminal-tab.cc. */

/* Makes a tab of @screen, a pane of a split tab in this or another window,
 * at @position in @window */
static gboolean
window_take_pane_as_tab(TerminalWindow* window,
                        TerminalScreen* screen,
                        int position)
{
  auto const tab = terminal_tab_get_from_screen(screen);
  if (!tab || !terminal_tab_is_split(tab))
    return false;

  auto const source = gtk_widget_get_root(GTK_WIDGET(tab));
  gs_unref_object auto taken = terminal_tab_take_screen(tab, screen);
  if (!taken)
    return false;

  terminal_notebook_insert_tab_at(window->notebook, terminal_tab_new(taken), position);
  terminal_notebook_set_active_screen(window->notebook, taken);
  gtk_widget_grab_focus(GTK_WIDGET(taken));

  if (TERMINAL_IS_WINDOW(source) && TERMINAL_WINDOW(source) != window)
    terminal_window_update_pane_actions(TERMINAL_WINDOW(source));
  terminal_window_update_pane_actions(window);
  gtk_window_present(GTK_WINDOW(window));
  return true;
}

static GdkDragAction
tab_bar_extra_drag_value_cb(AdwTabBar* tab_bar,
                            AdwTabPage* page,
                            GValue* value,
                            TerminalWindow* window)
{
  auto const screen = value ? TERMINAL_SCREEN(g_value_get_object(value)) : nullptr;
  auto const tab = terminal_tab_get_from_screen(screen);
  return tab && terminal_tab_is_split(tab) ? GDK_ACTION_MOVE : GdkDragAction(0);
}

/* A drop, done once the drag is over: the dragged pane is still busy with
 * it, and a tab dragged from a tab bar is out of its view until then, see
 * handle_tab_dropped_on_desktop() */
struct PaneDrop {
  GWeakRef target; // the pane that got the drop, or
  GWeakRef window; // the window that got it in its tab bar or list,
  int position;    // at this position
  GObject* object; // the dropped TerminalScreen or AdwTabPage
  TerminalPaneDrop where;
  guint tries;
};

static PaneDrop* pending_pane_drop = nullptr;

static void
pane_drop_free(PaneDrop* drop)
{
  g_weak_ref_clear(&drop->target);
  g_weak_ref_clear(&drop->window);
  g_object_unref(drop->object);
  g_free(drop);
}

static AdwTabView*
page_get_view(AdwTabPage* page)
{
  auto const child = adw_tab_page_get_child(page);
  return child ? ADW_TAB_VIEW(gtk_widget_get_ancestor(child, ADW_TYPE_TAB_VIEW)) : nullptr;
}

/* Whether the tab dragged out of a tab bar is about to become panes */
static bool
pane_drop_takes_detached_page(void)
{
  return pending_pane_drop &&
    ADW_IS_TAB_PAGE(pending_pane_drop->object) &&
    !page_get_view(ADW_TAB_PAGE(pending_pane_drop->object));
}

static void
pane_drop_screen(TerminalScreen* target,
                 TerminalScreen* screen,
                 TerminalPaneDrop where)
{
  auto const tab = terminal_tab_get_from_screen(target);
  auto const source_tab = terminal_tab_get_from_screen(screen);
  if (!tab || !source_tab || screen == target)
    return;

  auto const window = TERMINAL_WINDOW(gtk_widget_get_root(GTK_WIDGET(tab)));
  auto const source = gtk_widget_get_root(GTK_WIDGET(source_tab));

  if (where == TERMINAL_PANE_DROP_SWAP) {
    if (source_tab == tab)
      terminal_tab_swap_screens(tab, screen, target);
    return;
  }

  if (!terminal_tab_is_split(source_tab))
    return;

  gs_unref_object auto taken = terminal_tab_take_screen(source_tab, screen);
  if (!taken)
    return;

  terminal_tab_insert_screen(tab, target, taken, GtkPositionType(where));
  terminal_tab_set_active_screen(tab, taken);
  gtk_widget_grab_focus(GTK_WIDGET(taken));

  if (TERMINAL_IS_WINDOW(source) && TERMINAL_WINDOW(source) != window)
    terminal_window_update_pane_actions(TERMINAL_WINDOW(source));
  terminal_window_update_pane_actions(window);
  gtk_window_present(GTK_WINDOW(window));
}

static void
pane_drop_page(TerminalScreen* target,
               AdwTabPage* page,
               TerminalPaneDrop where)
{
  auto const tab = terminal_tab_get_from_screen(target);
  auto view = page_get_view(page);
  auto const other = TERMINAL_TAB(adw_tab_page_get_child(page));
  if (!tab || !view || !TERMINAL_IS_TAB(other) || other == tab ||
      where == TERMINAL_PANE_DROP_SWAP)
    return;

  auto const window = TERMINAL_WINDOW(gtk_widget_get_root(GTK_WIDGET(tab)));
  auto const active = terminal_tab_get_screen(other);

  // The tab view selected the dropped tab; show the one it goes into
  auto const target_view = ADW_TAB_VIEW(gtk_widget_get_ancestor(GTK_WIDGET(tab), ADW_TYPE_TAB_VIEW));
  if (auto const target_page = target_view ? adw_tab_view_get_page(target_view, GTK_WIDGET(tab)) : nullptr)
    adw_tab_view_set_selected_page(target_view, target_page);

  // Its window closes when the last screen leaves it
  g_object_ref(view);
  g_object_ref(page);
  terminal_tab_merge(tab, target, other, GtkPositionType(where));
  if (page_get_view(page) == view) {
    adw_tab_view_set_page_pinned(view, page, false);
    adw_tab_view_close_page(view, page);
  }
  g_object_unref(page);
  g_object_unref(view);

  if (active) {
    terminal_tab_set_active_screen(tab, active);
    gtk_widget_grab_focus(GTK_WIDGET(active));
  }
  terminal_window_update_pane_actions(window);
  gtk_window_present(GTK_WINDOW(window));
}

static gboolean
pane_drop_cb(void* user_data)
{
  auto const drop = pending_pane_drop;
  if (!drop)
    return G_SOURCE_REMOVE;

  // Wait for a tab to be back in its view, for a second at most
  if (ADW_IS_TAB_PAGE(drop->object) &&
      !page_get_view(ADW_TAB_PAGE(drop->object)) &&
      ++drop->tries < 20)
    return G_SOURCE_CONTINUE;

  pending_pane_drop = nullptr;
  if (auto const window = reinterpret_cast<TerminalWindow*>(g_weak_ref_get(&drop->window))) {
    if (!window->disposed)
      window_take_pane_as_tab(window, TERMINAL_SCREEN(drop->object), drop->position);
    g_object_unref(window);
  } else if (auto const target = reinterpret_cast<TerminalScreen*>(g_weak_ref_get(&drop->target))) {
    if (TERMINAL_IS_SCREEN(drop->object))
      pane_drop_screen(target, TERMINAL_SCREEN(drop->object), drop->where);
    else
      pane_drop_page(target, ADW_TAB_PAGE(drop->object), drop->where);
    g_object_unref(target);
  }
  pane_drop_free(drop);
  return G_SOURCE_REMOVE;
}

/**
 * terminal_window_drop_on_pane:
 * @window: a #TerminalWindow
 * @target: the #TerminalScreen of the pane that got the drop
 * @object: the dropped #TerminalScreen or #AdwTabPage
 * @where: where it goes
 *
 * Returns: whether the drop is taken
 */
gboolean
terminal_window_drop_on_pane(TerminalWindow* window,
                             TerminalScreen* target,
                             GObject* object,
                             TerminalPaneDrop where)
{
  g_return_val_if_fail(TERMINAL_IS_WINDOW(window), false);

  if (pending_pane_drop || !(TERMINAL_IS_SCREEN(object) || ADW_IS_TAB_PAGE(object)))
    return false;

  auto const drop = g_new0(PaneDrop, 1);
  g_weak_ref_init(&drop->target, target);
  g_weak_ref_init(&drop->window, nullptr);
  drop->position = -1;
  drop->object = G_OBJECT(g_object_ref(object));
  drop->where = where;
  pending_pane_drop = drop;

  // Not while the drag is still going on
  g_timeout_add(50, pane_drop_cb, nullptr);
  return true;
}

/* Makes a tab at @position of a pane dropped onto the tab bar or list */
static gboolean
window_drop_pane_as_tab(TerminalWindow* window,
                        TerminalScreen* screen,
                        int position)
{
  auto const tab = terminal_tab_get_from_screen(screen);
  if (pending_pane_drop || !tab || !terminal_tab_is_split(tab))
    return false;

  auto const drop = g_new0(PaneDrop, 1);
  g_weak_ref_init(&drop->target, nullptr);
  g_weak_ref_init(&drop->window, window);
  drop->position = position;
  drop->object = G_OBJECT(g_object_ref(screen));
  pending_pane_drop = drop;

  g_timeout_add(50, pane_drop_cb, nullptr);
  return true;
}

/* A tab dragged from a tab bar: libadwaita keeps it away from the
 * terminals, and drops it onto the tab view itself. So the window follows
 * the pointer over the terminals, and makes the dropped tab panes there
 * once the tab view has taken it. */

static void
window_set_tab_drop(TerminalWindow* window,
                    TerminalScreen* screen,
                    TerminalPaneDrop where)
{
  if (window->tab_drop_screen && window->tab_drop_screen != screen) {
    terminal_tab_show_drop_indicator(window->tab_drop_screen, -1);
    g_object_remove_weak_pointer(G_OBJECT(window->tab_drop_screen),
                                 reinterpret_cast<void**>(&window->tab_drop_screen));
    window->tab_drop_screen = nullptr;
  }

  if (screen && screen != window->tab_drop_screen) {
    window->tab_drop_screen = screen;
    g_object_add_weak_pointer(G_OBJECT(screen),
                              reinterpret_cast<void**>(&window->tab_drop_screen));
  }

  window->tab_drop_where = where;
  if (screen)
    terminal_tab_show_drop_indicator(screen, where);
}

static void
window_transfer_motion_cb(GtkDropControllerMotion* controller,
                          double x,
                          double y,
                          TerminalWindow* window)
{
  auto const view = terminal_notebook_get_tab_view(window->notebook);
  auto const tab = window_get_active_tab(window);
  auto where = TERMINAL_PANE_DROP_RIGHT;
  auto const screen = tab && adw_tab_view_get_is_transferring_page(view)
    ? terminal_tab_pick_drop(tab, GTK_WIDGET(window->notebook), x, y, &where)
    : nullptr;
  window_set_tab_drop(window, screen, where);
}

static void
window_transfer_leave_cb(GtkDropControllerMotion* controller,
                         TerminalWindow* window)
{
  window_set_tab_drop(window, nullptr, TERMINAL_PANE_DROP_RIGHT);
}

/* The tab view took a dropped tab; over a pane, the tab becomes panes. The
 * drop comes before the pointer leaves. */
static void
window_page_attached_cb(AdwTabView* view,
                        AdwTabPage* page,
                        int position,
                        TerminalWindow* window)
{
  auto const screen = window->tab_drop_screen;
  if (!screen || !adw_tab_view_get_is_transferring_page(view))
    return;

  auto const where = window->tab_drop_where;
  window_set_tab_drop(window, nullptr, TERMINAL_PANE_DROP_RIGHT);
  terminal_window_drop_on_pane(window, screen, G_OBJECT(page), where);
}

/* A pane dropped onto a tab in the tab bar becomes a tab after it */
static gboolean
tab_bar_extra_drag_drop_cb(AdwTabBar* tab_bar,
                           AdwTabPage* page,
                           GValue* value,
                           TerminalWindow* window)
{
  auto const screen = TERMINAL_SCREEN(g_value_get_object(value));
  auto const view = terminal_notebook_get_tab_view(window->notebook);
  return screen && window_drop_pane_as_tab(window, screen,
                                           adw_tab_view_get_page_position(view, page) + 1);
}

static gboolean
sidebar_screen_dropped_cb(TerminalTabSidebar* sidebar,
                          TerminalScreen* screen,
                          int position,
                          TerminalWindow* window)
{
  return window_drop_pane_as_tab(window, screen, position);
}

static void
action_tab_detach_cb (GSimpleAction *action,
                      GVariant *parameter,
                      gpointer user_data)
{
  auto const window = TERMINAL_WINDOW(user_data);

  if (!window->active_screen)
    return;

  detach_tab(window, terminal_tab_get_from_screen(window->active_screen));
}

static void
action_help_cb (GSimpleAction *action,
                GVariant *parameter,
                gpointer user_data)
{
  TerminalWindow *window = (TerminalWindow*)user_data;

  terminal_util_show_help (GTK_WINDOW(window), nullptr);
}

static void
action_about_cb (GSimpleAction *action,
                 GVariant *parameter,
                 gpointer user_data)
{
  terminal_util_show_about ();
}

static void
action_edit_preferences_cb (GSimpleAction *action,
                            GVariant *parameter,
                            gpointer user_data)
{
  TerminalWindow *window = (TerminalWindow*)user_data;

  terminal_app_edit_preferences (terminal_app_get (),
                                 terminal_screen_get_profile (window->active_screen),
                                 nullptr);
}

static void
action_size_to_cb (GSimpleAction *action,
                   GVariant      *parameter,
                   gpointer       user_data)
{
  TerminalWindow *window = (TerminalWindow*)user_data;
  guint width, height;

  terminal_assert_nonnull (parameter);

  if (window->active_screen == nullptr)
    return;

  g_variant_get (parameter, "(uu)", &width, &height);
  if (width < MIN_WIDTH_CHARS || height < MIN_HEIGHT_CHARS ||
      width > 256 || height > 256)
    return;

  vte_terminal_set_size (VTE_TERMINAL (window->active_screen), width, height);
  terminal_window_update_size (window);
}

static void
action_open_match_cb (GSimpleAction *action,
                      GVariant      *parameter,
                      gpointer       user_data)
{
  TerminalWindow *window = (TerminalWindow*)user_data;
  TerminalScreenPopupInfo *info = window->popup_info;

  if (info == nullptr)
    return;
  if (info->url == nullptr)
    return;

  terminal_util_open_url (GTK_WIDGET (window), info->url, info->url_flavor,
                          GDK_CURRENT_TIME);
}

static void
action_copy_match_cb (GSimpleAction *action,
                      GVariant      *parameter,
                      gpointer       user_data)
{
  TerminalWindow *window = (TerminalWindow*)user_data;
  TerminalScreenPopupInfo *info = window->popup_info;

  if (info == nullptr)
    return;
  if (info->url == nullptr)
    return;

  gdk_clipboard_set_text (window->clipboard, info->url);
  window_show_copy_toast (window);
}

static void
action_open_hyperlink_cb (GSimpleAction *action,
                          GVariant      *parameter,
                          gpointer       user_data)
{
  TerminalWindow *window = (TerminalWindow*)user_data;
  TerminalScreenPopupInfo *info = window->popup_info;

  if (info == nullptr)
    return;
  if (info->hyperlink == nullptr)
    return;

  terminal_util_open_url (GTK_WIDGET (window), info->hyperlink, FLAVOR_AS_IS,
                          GDK_CURRENT_TIME);
}

static void
action_copy_hyperlink_cb (GSimpleAction *action,
                          GVariant      *parameter,
                          gpointer       user_data)
{
  TerminalWindow *window = (TerminalWindow*)user_data;
  TerminalScreenPopupInfo *info = window->popup_info;

  if (info == nullptr)
    return;
  if (info->hyperlink == nullptr)
    return;

  gdk_clipboard_set_text (window->clipboard, info->hyperlink);
  window_show_copy_toast (window);
}

/* Pastes the target of a hyperlink into the terminal: the path of a local
 * file, or else the address, quoted for the shell */
static void
action_paste_hyperlink_cb (GSimpleAction *action,
                           GVariant      *parameter,
                           gpointer       user_data)
{
  TerminalWindow *window = (TerminalWindow*)user_data;
  TerminalScreenPopupInfo *info = window->popup_info;

  if (info == nullptr || info->hyperlink == nullptr || window->active_screen == nullptr)
    return;

  gs_free char *host = nullptr;
  gs_free char *path = g_filename_from_uri (info->hyperlink, &host, nullptr);
  auto const local = path && (!host || !host[0] ||
                              g_ascii_strcasecmp (host, "localhost") == 0 ||
                              g_ascii_strcasecmp (host, g_get_host_name ()) == 0);
  gs_free char *quoted = g_shell_quote (local ? path : info->hyperlink);
  gs_free char *text = g_strconcat (quoted, " ", nullptr);
  terminal_screen_paste_text (window->active_screen, text, -1);
}

static void
action_toggle_fullscreen_cb (GSimpleAction *action,
                             GVariant      *parameter,
                             gpointer       user_data)
{
  TerminalWindow *window = (TerminalWindow*)user_data;

  g_action_group_change_action_state (G_ACTION_GROUP (window), "fullscreen",
                                      g_variant_new_boolean (!gtk_window_is_fullscreen (GTK_WINDOW (window))));
}

static void
action_inspector_cb (GSimpleAction *action,
                     GVariant      *parameter,
                     gpointer       user_data)
{
  gtk_window_set_interactive_debugging (TRUE);
}

static void
action_find_cb (GSimpleAction *action,
                GVariant *parameter,
                gpointer user_data)
{
  TerminalWindow *window = (TerminalWindow*)user_data;

  if (G_UNLIKELY(window->active_screen == nullptr))
    return;

  gtk_revealer_set_reveal_child(window->find_bar_revealer, true);
  gtk_widget_grab_focus(GTK_WIDGET(window->find_bar));
}

static void
action_find_forward_cb (GSimpleAction *action,
                        GVariant *parameter,
                        gpointer user_data)
{
  TerminalWindow *window = (TerminalWindow*)user_data;

  if (window->active_screen == nullptr)
    return;

  // Show the bar, so that the matches are highlighted
  gtk_revealer_set_reveal_child (window->find_bar_revealer, true);
  terminal_find_bar_find (window->find_bar, 1);
}

static void
action_find_backward_cb (GSimpleAction *action,
                         GVariant *parameter,
                         gpointer user_data)
{
  TerminalWindow *window = (TerminalWindow*)user_data;

  if (window->active_screen == nullptr)
    return;

  gtk_revealer_set_reveal_child (window->find_bar_revealer, true);
  terminal_find_bar_find (window->find_bar, -1);
}

static void
action_find_clear_cb (GSimpleAction *action,
                      GVariant *parameter,
                      gpointer user_data)
{
  TerminalWindow *window = (TerminalWindow*)user_data;

  if (window->active_screen == nullptr)
    return;

  terminal_find_bar_clear (window->find_bar);
}

static void
action_shadow_activate_cb (GSimpleAction *action,
                           GVariant *parameter,
                           gpointer user_data)
{
  TerminalWindow *window = (TerminalWindow*)user_data;
  gs_free char *param = g_variant_print(parameter, TRUE);

  _terminal_debug_print (TERMINAL_DEBUG_ACCELS,
                         "Window %p shadow action activated for %s\n",
                         window, param);

  /* We make sure in terminal-accels to always install the keybinding
   * for the real action first, so that it's first in line for activation.
   * That means we can make this here a NOP, instead of forwarding the
   * activation to the shadowed action.
   *
   * Except for Copy: it is enabled when VTE says that the selection
   * changed, which it does only when the mouse button is released, so
   * copying while still selecting comes here.
   */
  gs_free char *name = nullptr;
  gs_unref_variant GVariant *target = nullptr;
  if (g_action_parse_detailed_name (g_variant_get_string (parameter, nullptr),
                                    &name, &target, nullptr) &&
      g_str_equal (name, "win.copy") &&
      target != nullptr)
    action_copy_cb (G_SIMPLE_ACTION (lookup_action (window, "copy")), target, window);
}

static gboolean
terminal_window_do_fullscreen (gpointer data)
{
  TerminalWindow *window = TERMINAL_WINDOW (data);

  window->fullscreen_transition = 0;

  if (!window->disposed) {
    if (gtk_window_is_fullscreen (GTK_WINDOW (window)))
      gtk_window_unfullscreen (GTK_WINDOW (window));
    else
      gtk_window_fullscreen (GTK_WINDOW (window));
  }

  return G_SOURCE_REMOVE;
}

static void
action_fullscreen_state_cb (GSimpleAction *action,
                            GVariant *state,
                            gpointer user_data)
{
  TerminalWindow *window = (TerminalWindow*)user_data;
  gboolean fullscreen = g_variant_get_boolean (state);

  if (!gtk_widget_get_realized (GTK_WIDGET (window)))
    return;

  adw_toolbar_view_set_reveal_top_bars (window->toolbar_view, !fullscreen);

  /* The top tabs bar is hidden with the top bars above; also
   * hide the tabs when they are at the bottom or at the side.
   */
  window->tabs_hidden_for_fullscreen = fullscreen;
  window_update_tabs_visibility (window);

  /* Wait for the revealer transition to finish plus a small amount
   * of time, then do fullscreen state change. This helps ensure that
   * the compositor can animate the approprate screen contents between
   * the two state changes.
   *
   * Since the headerbar is relatively short in height, only a few
   * frames of transition is necessary for the eyes to catch up.
   */
  g_clear_handle_id (&window->fullscreen_transition, g_source_remove);
  window->fullscreen_transition =
    g_timeout_add_full (G_PRIORITY_DEFAULT,
                        275, /* 250msec for revealer + delay */
                        terminal_window_do_fullscreen,
                        g_object_ref (window),
                        g_object_unref);

  /* The window-state-changed callback will update the action's actual state */
}

static void
action_read_only_state_cb (GSimpleAction *action,
                           GVariant *state,
                           gpointer user_data)
{
  TerminalWindow *window = (TerminalWindow*)user_data;

  terminal_assert_nonnull (state);

  g_simple_action_set_state (action, state);

  terminal_window_update_paste_sensitivity (window);

  if (window->active_screen == nullptr)
    return;

  vte_terminal_set_input_enabled (VTE_TERMINAL (window->active_screen),
                                  !g_variant_get_boolean (state));
}

static void
action_profile_state_cb (GSimpleAction *action,
                         GVariant *state,
                         gpointer user_data)
{
  TerminalWindow *window = (TerminalWindow*)user_data;
  TerminalSettingsList *profiles_list;
  const gchar *uuid;
  gs_unref_object GSettings *profile;

  terminal_assert_nonnull (state);

  uuid = g_variant_get_string (state, nullptr);
  profiles_list = terminal_app_get_profiles_list (terminal_app_get ());
  profile = terminal_settings_list_ref_child (profiles_list, uuid);
  if (profile == nullptr)
    return;

  g_simple_action_set_state (action, state);

  terminal_screen_set_profile (window->active_screen, profile);
}

static void
action_active_tab_set_cb (GSimpleAction *action,
                          GVariant *parameter,
                          gpointer user_data)
{
  TerminalWindow *window = (TerminalWindow*)user_data;
  int value, n_screens;

  terminal_assert_nonnull (parameter);

  n_screens = terminal_notebook_get_n_screens (window->notebook);

  value = g_variant_get_int32 (parameter);
  if (value < 0)
    value += n_screens;
  if (value < 0 || value >= n_screens)
    return;

  g_action_change_state (G_ACTION (action), g_variant_new_int32 (value));
}

static void
action_active_tab_state_cb (GSimpleAction *action,
                            GVariant *state,
                            gpointer user_data)
{
  TerminalWindow *window = (TerminalWindow*)user_data;

  terminal_assert_nonnull (state);

  g_simple_action_set_state (action, state);

  terminal_notebook_set_active_screen_num (window->notebook, g_variant_get_int32 (state));
}

/* Notebook context menu actions */

static void
action_notebook_tab_close_cb(GSimpleAction* action,
                             GVariant* parameter,
                             void* user_data)
{
  auto const window = TERMINAL_WINDOW(user_data);
  if (!window->notebook_context_tab)
    return;

  auto const tab = window->notebook_context_tab;
  window->notebook_context_tab = nullptr;

  terminal_notebook_close_tab(window->notebook, tab);

  // window may be destroyed at this point
}

static void
action_notebook_tab_close_others_cb(GSimpleAction* action,
                                    GVariant* parameter,
                                    void* user_data)
{
  auto const window = TERMINAL_WINDOW(user_data);
  if (!window->notebook_context_tab)
    return;

  auto const tab = window->notebook_context_tab;
  window->notebook_context_tab = nullptr;

  window_close_other_tabs(window, tab);
}

static void
set_title_response_cb(AdwMessageDialog* dialog,
                      char const* response,
                      TerminalScreen* screen)
{
  if (!g_str_equal(response, "set"))
    return;

  auto const entry = GTK_EDITABLE(adw_message_dialog_get_extra_child(dialog));
  terminal_screen_set_user_title(screen, gtk_editable_get_text(entry));
}

/* Asks for a title for @screen that programs cannot change */
static void
window_ask_screen_title(TerminalWindow* window,
                        TerminalScreen* screen)
{
  auto const dialog = ADW_MESSAGE_DIALOG(adw_message_dialog_new(GTK_WINDOW(window),
                                                                _("Set Title"),
                                                                _("Programs cannot change this title. Leave it empty to show the title from the program again.")));
  adw_message_dialog_add_responses(dialog,
                                   "cancel", _("_Cancel"),
                                   "set", _("_Set"),
                                   nullptr);
  adw_message_dialog_set_response_appearance(dialog, "set", ADW_RESPONSE_SUGGESTED);
  adw_message_dialog_set_default_response(dialog, "set");
  adw_message_dialog_set_close_response(dialog, "cancel");

  auto const entry = gtk_entry_new();
  gtk_entry_set_activates_default(GTK_ENTRY(entry), true);
  auto const user_title = terminal_screen_get_user_title(screen);
  gtk_editable_set_text(GTK_EDITABLE(entry),
                        user_title ? user_title : terminal_screen_get_title(screen));
  adw_message_dialog_set_extra_child(dialog, entry);

  // The screen may close while the dialog is open
  g_signal_connect_object(dialog, "response",
                          G_CALLBACK(set_title_response_cb), screen,
                          GConnectFlags(0));
  g_signal_connect_object(screen, "destroy",
                          G_CALLBACK(gtk_window_destroy), dialog,
                          G_CONNECT_SWAPPED);

  gtk_window_present(GTK_WINDOW(dialog));
  gtk_widget_grab_focus(entry);
}

static void
action_notebook_set_title_cb(GSimpleAction* action,
                             GVariant* parameter,
                             void* user_data)
{
  auto const window = TERMINAL_WINDOW(user_data);
  if (!window->notebook_context_tab)
    return;

  auto const tab = window->notebook_context_tab;
  window->notebook_context_tab = nullptr;

  if (auto const screen = terminal_tab_get_screen(tab))
    window_ask_screen_title(window, screen);
}

static void
action_set_title_cb(GSimpleAction* action,
                    GVariant* parameter,
                    void* user_data)
{
  auto const window = TERMINAL_WINDOW(user_data);
  if (window->active_screen)
    window_ask_screen_title(window, window->active_screen);
}

static void
action_notebook_tab_detach_cb(GSimpleAction* action,
                              GVariant* parameter,
                              void* user_data)
{
  auto const window = TERMINAL_WINDOW(user_data);
  if (!window->notebook_context_tab)
    return;

  auto const tab = window->notebook_context_tab;
  window->notebook_context_tab = nullptr;

  detach_tab(window, tab);

  // window may be destroyed at this point
}

static void
action_notebook_tab_move_left_cb(GSimpleAction* action,
                                 GVariant* parameter,
                                 void* user_data)
{
  auto const window = TERMINAL_WINDOW(user_data);
  if (!window->notebook_context_tab)
    return;

  terminal_notebook_reorder_tab(window->notebook, window->notebook_context_tab, -1);
  window->notebook_context_tab = nullptr;
}

static void
action_notebook_tab_move_right_cb(GSimpleAction* action,
                                  GVariant* parameter,
                                  void* user_data)
{
  auto const window = TERMINAL_WINDOW(user_data);
  if (!window->notebook_context_tab)
    return;

  terminal_notebook_reorder_tab(window->notebook, window->notebook_context_tab, 1);
  window->notebook_context_tab = nullptr;
}

static void
action_notebook_tab_pin_cb(GSimpleAction* action,
                           GVariant* parameter,
                           void* user_data)
{
  auto const window = TERMINAL_WINDOW(user_data);
  if (!window->notebook_context_tab)
    return;

  terminal_notebook_set_tab_pinned(window->notebook, window->notebook_context_tab, true);
  window->notebook_context_tab = nullptr;
}

static void
action_notebook_tab_unpin_cb(GSimpleAction* action,
                             GVariant* parameter,
                             void* user_data)
{
  auto const window = TERMINAL_WINDOW(user_data);
  if (!window->notebook_context_tab)
    return;

  terminal_notebook_set_tab_pinned(window->notebook, window->notebook_context_tab, false);
  window->notebook_context_tab = nullptr;
}

/* utility functions */

static void
terminal_window_update_set_profile_menu_active_profile (TerminalWindow *window)
{
  GSettings *new_active_profile;
  TerminalSettingsList *profiles_list;
  char *uuid;

  if (window->active_screen == nullptr)
    return;

  new_active_profile = terminal_screen_get_profile (window->active_screen);

  profiles_list = terminal_app_get_profiles_list (terminal_app_get ());
  uuid = terminal_settings_list_dup_uuid_from_child (profiles_list, new_active_profile);

  g_simple_action_set_state (lookup_action (window, "profile"),
                             g_variant_new_take_string (uuid));
}

static void
terminal_window_update_terminal_menu (TerminalWindow *window)
{
  if (window->active_screen == nullptr)
    return;

  gboolean read_only = !vte_terminal_get_input_enabled (VTE_TERMINAL (window->active_screen));
  g_simple_action_set_state (lookup_action (window, "read-only"),
                             g_variant_new_boolean (read_only));

  gboolean activity, silence;
  terminal_screen_get_monitoring (window->active_screen, &activity, &silence);
  g_simple_action_set_state (lookup_action (window, "monitor-activity"),
                             g_variant_new_boolean (activity));
  g_simple_action_set_state (lookup_action (window, "monitor-silence"),
                             g_variant_new_boolean (silence));
}

static void
action_monitor_state_cb (GSimpleAction *action,
                         GVariant *state,
                         gpointer user_data)
{
  auto const window = TERMINAL_WINDOW (user_data);

  g_simple_action_set_state (action, state);
  if (!window->active_screen)
    return;

  gboolean activity, silence;
  terminal_screen_get_monitoring (window->active_screen, &activity, &silence);
  if (g_str_equal (g_action_get_name (G_ACTION (action)), "monitor-activity"))
    activity = g_variant_get_boolean (state);
  else
    silence = g_variant_get_boolean (state);
  terminal_screen_set_monitoring (window->active_screen, activity, silence);
}

/* Whether the user can see @screen: in the selected tab of the focused
 * window, maybe as one of several panes */
static bool
window_screen_in_sight (TerminalWindow* window,
                        TerminalScreen* screen)
{
  auto const tab = terminal_tab_get_from_screen (screen);
  return gtk_window_is_active (GTK_WINDOW (window)) &&
    tab && tab == window_get_active_tab (window);
}

#define ACTIVITY_NOTIFIED_KEY "pano-activity-notified"

/* A monitored terminal out of sight printed something: tell once, until
 * the user looks at it */
static void
screen_activity_cb (TerminalScreen* screen,
                    TerminalWindow* window)
{
  if (window_screen_in_sight (window, screen) ||
      g_object_get_data (G_OBJECT (screen), ACTIVITY_NOTIFIED_KEY))
    return;

  g_object_set_data (G_OBJECT (screen), ACTIVITY_NOTIFIED_KEY, GINT_TO_POINTER (1));
  if (auto const tab = terminal_tab_get_from_screen (screen); tab && tab != window_get_active_tab (window))
    terminal_notebook_set_tab_needs_attention (window->notebook, tab, true);

  terminal_app_notify_screen (terminal_app_get (), screen,
                              _("Activity in a terminal"),
                              terminal_screen_get_title (screen));
}

/* The user looks at the window again: the terminals of its selected tab
 * may tell about activity again later */
static void
window_is_active_changed_cb (TerminalWindow* window,
                             GParamSpec* pspec,
                             void* user_data)
{
  auto const tab = window_get_active_tab (window);
  if (!tab || !gtk_window_is_active (GTK_WINDOW (window)))
    return;

  gs_free_list GList* screens = terminal_tab_list_screens (tab);
  for (auto l = screens; l; l = l->next)
    g_object_set_data (G_OBJECT (l->data), ACTIVITY_NOTIFIED_KEY, nullptr);
}

/* A monitored terminal out of sight went quiet */
static void
screen_silence_cb (TerminalScreen* screen,
                   TerminalWindow* window)
{
  if (window_screen_in_sight (window, screen))
    return;

  if (auto const tab = terminal_tab_get_from_screen (screen); tab && tab != window_get_active_tab (window))
    terminal_notebook_set_tab_needs_attention (window->notebook, tab, true);

  auto const settings = terminal_app_get_global_settings (terminal_app_get ());
  auto const seconds = g_settings_get_uint (settings, TERMINAL_SETTING_SILENCE_DURATION_KEY);
  /* Translators: %s is the title of a terminal, %u a number of seconds */
  gs_free char* body = g_strdup_printf (g_dngettext (GETTEXT_PACKAGE,
                                                     "%s\nNo output for %u second",
                                                     "%s\nNo output for %u seconds",
                                                     seconds),
                                        terminal_screen_get_title (screen), seconds);
  terminal_app_notify_screen (terminal_app_get (), screen,
                              _("Silence in a terminal"), body);
}

/* Actions stuff */

static void
terminal_window_update_copy_sensitivity (TerminalScreen *screen,
                                         TerminalWindow *window)
{
  gboolean can_copy;

  if (screen != window->active_screen)
    return;

  can_copy = vte_terminal_get_has_selection (VTE_TERMINAL (screen));
  g_simple_action_set_enabled (lookup_action (window, "copy"), can_copy);
}

static void
terminal_window_update_zoom_sensitivity (TerminalWindow *window)
{
  TerminalScreen *screen;

  screen = window->active_screen;
  if (screen == nullptr)
    return;

  double v;
  double zoom = v = vte_terminal_get_font_scale (VTE_TERMINAL (screen));
  g_simple_action_set_enabled (lookup_action (window, "zoom-in"),
                               find_larger_zoom_factor (&v));

  v = zoom;
  g_simple_action_set_enabled (lookup_action (window, "zoom-out"),
                               find_smaller_zoom_factor (&v));
}

static void
terminal_window_update_search_sensitivity (TerminalWindow *window)
{
  auto const can_search = window->active_screen && terminal_find_bar_has_search(window->find_bar);

  g_simple_action_set_enabled (lookup_action (window, "find-forward"), can_search);
  g_simple_action_set_enabled (lookup_action (window, "find-backward"), can_search);
  g_simple_action_set_enabled (lookup_action (window, "find-clear"), can_search);
}

static void
clipboard_targets_changed_cb (TerminalApp *app,
                              GdkClipboard *clipboard,
                              TerminalWindow *window)
{
  if (clipboard != window->clipboard)
    return;

  terminal_window_update_paste_sensitivity (window);
}

static void
terminal_window_update_paste_sensitivity (TerminalWindow *window)
{
  GdkContentFormats *targets = terminal_app_get_clipboard_targets (terminal_app_get(), window->clipboard);

  gboolean can_paste = gdk_content_formats_contain_gtype (targets, G_TYPE_STRING);
  gboolean can_paste_uris = gdk_content_formats_contain_gtype (targets, GDK_TYPE_FILE_LIST);

  gs_unref_variant GVariant *ro_state = g_action_get_state (g_action_map_lookup_action (G_ACTION_MAP (window), "read-only"));
  gboolean read_only = g_variant_get_boolean (ro_state);

  g_simple_action_set_enabled (lookup_action (window, "paste-text"), can_paste && !read_only);
  g_simple_action_set_enabled (lookup_action (window, "paste-uris"), can_paste_uris && !read_only);
  g_simple_action_set_enabled (lookup_action (window, "paste-hyperlink"), !read_only);
}

static inline bool
should_update_window_size (TerminalWindow *window);

static void
screen_resize_window_cb (TerminalScreen *screen,
                         guint columns,
                         guint rows,
                         TerminalWindow* window)
{
  /* Only the terminal in front may resize the window: a terminal in a
   * background tab or another pane is not what the user is looking at */
  if (screen != window->active_screen ||
      !should_update_window_size (window) ||
      !g_settings_get_boolean (terminal_app_get_global_settings (terminal_app_get ()),
                               TERMINAL_SETTING_ALLOW_RESIZE_WINDOW_KEY))
    return;

  vte_terminal_set_size (VTE_TERMINAL (screen), columns, rows);
  terminal_window_update_size (window);
}

static void
terminal_window_update_tabs_actions_sensitivity (TerminalWindow *window)
{
  g_assert (TERMINAL_IS_WINDOW (window));
  g_assert (TERMINAL_IS_NOTEBOOK (window->notebook));

  if (window->disposed)
    return;

  if (!window->active_screen)
    return;

  auto const tab = terminal_tab_get_from_screen(window->active_screen);

  bool can_switch_left, can_switch_right;
  bool can_reorder_left, can_reorder_right, can_reorder_start, can_reorder_end;
  bool can_close, can_detach;
  terminal_notebook_get_tab_actions(window->notebook, tab,
                                    &can_switch_left,
                                    &can_switch_right,
                                    &can_reorder_left,
                                    &can_reorder_right,
                                    &can_reorder_start,
                                    &can_reorder_end,
                                    &can_close,
                                    &can_detach);

  auto const num_pages = terminal_notebook_get_n_screens(window->notebook);
  auto const not_only = num_pages > 1;

  auto const page_num = terminal_notebook_get_active_screen_num (window->notebook);

  /* Hide the tabs menu in single-tab windows */
  g_simple_action_set_enabled (lookup_action (window, "tabs-menu"), not_only);

  /* Disable shadowing of MDI actions in SDI windows */
  g_simple_action_set_enabled (lookup_action (window, "shadow-mdi"), not_only);

  /* Disable tab switching (and all its shortcuts) in SDI windows */
  g_simple_action_set_enabled (lookup_action (window, "active-tab"), not_only);

  // FIXME: do the equivalent of
  // g_simple_action_set_enabled (lookup_action (window, "tab-overview"), not_only);
  // or just hide the "show overview" button?

  /* Set the active tab */
  g_simple_action_set_state (lookup_action (window, "active-tab"),
                             g_variant_new_int32 (page_num));

  g_simple_action_set_enabled (lookup_action (window, "tab-switch-left"), can_switch_left);
  g_simple_action_set_enabled (lookup_action (window, "tab-switch-right"), can_switch_right);
  g_simple_action_set_enabled (lookup_action (window, "tab-move-left"), can_reorder_left);
  g_simple_action_set_enabled (lookup_action (window, "tab-move-right"), can_reorder_right);
  g_simple_action_set_enabled (lookup_action (window, "tab-move-start"), can_reorder_start);
  g_simple_action_set_enabled (lookup_action (window, "tab-move-end"), can_reorder_end);
  g_simple_action_set_enabled (lookup_action (window, "tab-detach"), can_detach);
}

static AdwTabView *
handle_tab_dropped_on_desktop (AdwTabView     *tab_view,
                               TerminalWindow *window)
{
  TerminalWindow *source_window;
  TerminalWindow *new_window;

  g_assert (ADW_IS_TAB_VIEW (tab_view));
  g_assert (TERMINAL_IS_WINDOW (window));

  source_window = TERMINAL_WINDOW (gtk_widget_get_root (GTK_WIDGET (tab_view)));
  g_return_val_if_fail (TERMINAL_IS_WINDOW (source_window), nullptr);

  /* The tab was dropped onto a pane: it goes back to its view first, then
   * becomes panes, see terminal_window_drop_on_pane() */
  if (pane_drop_takes_detached_page ())
    return nullptr;

  new_window = terminal_window_new (G_APPLICATION (terminal_app_get ()));
  new_window->present_on_insert = TRUE;

  return terminal_notebook_get_tab_view (new_window->notebook);
}

/* Terminal screen popup menu handling */

static void
remove_popup_info (TerminalWindow *window)
{
  if (window->popup_info != nullptr)
    {
      terminal_screen_popup_info_unref (window->popup_info);
      window->popup_info = nullptr;
    }
}

static void
menu_append_item(GMenu* menu,
                 GMenuItem* item)
{
  g_menu_item_set_attribute(item, "accel", "s", "");
  g_menu_append_item(menu, item);
}

static void
menu_append(GMenu* menu,
            char const* label,
            char const* detailed_action)
{
  gs_unref_object auto item = g_menu_item_new(label, detailed_action);
  menu_append_item(menu, item);
}

static void
screen_show_popup_menu_cb (TerminalScreen *screen,
                           TerminalScreenPopupInfo *info,
                           TerminalWindow *window)
{
  TerminalApp *app = terminal_app_get ();

  if (screen != window->active_screen)
    return;

  remove_popup_info (window);
  window->popup_info = terminal_screen_popup_info_ref (info);

  gs_unref_object GMenu *menu = g_menu_new ();

  /* Hyperlink section */
  if (info->hyperlink != nullptr) {
    gs_unref_object GMenu *section1 = g_menu_new ();

    menu_append (section1, _("Open _Hyperlink"), "win.open-hyperlink");
    menu_append (section1, _("Copy Hyperlink _Address"), "win.copy-hyperlink");
    if (g_action_get_enabled (G_ACTION (lookup_action (window, "paste-hyperlink"))))
      menu_append (section1, _("Paste Hyperlink _Target"), "win.paste-hyperlink");
    g_menu_append_section (menu, nullptr, G_MENU_MODEL (section1));
  }
  /* Matched link section */
  else if (info->url != nullptr) {
    gs_unref_object GMenu *section2 = g_menu_new ();

    const char *open_label = nullptr, *copy_label = nullptr;
    switch (info->url_flavor) {
    case FLAVOR_EMAIL:
      open_label = _("Send Mail _To…");
      copy_label = _("Copy Mail _Address");
      break;
    case FLAVOR_VOIP_CALL:
      open_label = _("Call _To…");
      copy_label = _("Copy Call _Address ");
      break;
    case FLAVOR_AS_IS:
    case FLAVOR_DEFAULT_TO_HTTP:
    default:
      open_label = _("_Open Link");
      copy_label = _("Copy _Link");
      break;
    }

    menu_append (section2, open_label, "win.open-match");
    menu_append (section2, copy_label, "win.copy-match");
    g_menu_append_section (menu, nullptr, G_MENU_MODEL (section2));
  }

  /* Info section */
  gs_strfreev char** citems = g_settings_get_strv (terminal_app_get_global_settings (terminal_app_get ()),
                                                   TERMINAL_SETTING_CONTEXT_INFO_KEY);

  gs_unref_object GMenu *section3 = g_menu_new ();

  for (int i = 0; citems[i] != nullptr; ++i) {
    const char *citem = citems[i];

    if (g_str_equal (citem, "numbers") &&
        info->number_info != nullptr) {
      /* Non-existent action will make this item insensitive */
      gs_unref_object GMenuItem *item3 = g_menu_item_new (info->number_info, "win.notexist");
      menu_append_item (section3, item3);
    }

    if (g_str_equal (citem, "timestamps") &&
        info->timestamp_info != nullptr) {
      /* Non-existent action will make this item insensitive */
      gs_unref_object GMenuItem *item3 = g_menu_item_new (info->timestamp_info, "win.notexist");
      menu_append_item (section3, item3);
    }
  }

  if (g_menu_model_get_n_items(G_MENU_MODEL (section3)) > 0)
    g_menu_append_section (menu, nullptr, G_MENU_MODEL (section3));

  /* Clipboard section */
  gs_unref_object GMenu *section4 = g_menu_new ();

  menu_append (section4, _("_Copy"), "win.copy::text");
  menu_append (section4, _("Copy as _HTML"), "win.copy::html");
  menu_append (section4, _("_Paste"), "win.paste-text");
  // The command under the pointer, else the last one
  if (terminal_screen_has_command_output_at (screen, info->row))
    menu_append (section4, _("Copy _Output of This Command"), "win.copy-output-here");
  else if (terminal_screen_has_command_output (screen))
    menu_append (section4, _("Copy _Output of Last Command"), "win.copy-output");
  if (g_action_get_enabled (G_ACTION (lookup_action (window, "paste-uris"))))
    menu_append (section4, _("Paste as _Filenames"), "win.paste-uris");

  g_menu_append_section (menu, nullptr, G_MENU_MODEL (section4));

  /* Profile and property section */
  gs_unref_object GMenu *section5 = g_menu_new ();
  menu_append (section5, _("Read-_Only"), "win.read-only");
  menu_append (section5, _("Monitor for _Activity"), "win.monitor-activity");
  menu_append (section5, _("Monitor for _Silence"), "win.monitor-silence");

  GMenuModel *profiles_menu = terminal_app_get_profilemenu (app);
  if (profiles_menu != nullptr && g_menu_model_get_n_items (profiles_menu) > 1) {
    gs_unref_object GMenu *submenu5 = g_menu_new ();
    g_menu_append_section (submenu5, nullptr, profiles_menu);

    gs_unref_object GMenuItem *item5 = g_menu_item_new (_("P_rofiles"), nullptr);
    g_menu_item_set_submenu (item5, G_MENU_MODEL (submenu5));
    g_menu_append_item (section5, item5);
  }

  menu_append (section5, _("_Preferences"), "win.edit-preferences");

  g_menu_append_section (menu, nullptr, G_MENU_MODEL (section5));

  /* Pane section */
  gs_unref_object GMenu *pane_section = g_menu_new ();
  gs_unref_object GMenuItem *split_right_item = g_menu_item_new (_("Split _Right"), nullptr);
  g_menu_item_set_action_and_target (split_right_item, "win.split", "s", "right");
  menu_append_item (pane_section, split_right_item);
  gs_unref_object GMenuItem *split_down_item = g_menu_item_new (_("Split _Down"), nullptr);
  g_menu_item_set_action_and_target (split_down_item, "win.split", "s", "down");
  menu_append_item (pane_section, split_down_item);
  if (auto const tab = window_get_active_tab (window); tab && terminal_tab_is_split (tab)) {
    menu_append (pane_section, _("_Zoom Pane"), "win.zoom-pane");
    menu_append (pane_section, _("Make Panes the Same _Size"), "win.equalize-panes");

    gs_unref_object GMenu *move_menu = g_menu_new ();
    gs_unref_object GMenuItem *to_tab_item = g_menu_item_new (_("To a New _Tab"), nullptr);
    g_menu_item_set_action_and_target (to_tab_item, "win.move-pane", "s", "tab");
    menu_append_item (move_menu, to_tab_item);
    gs_unref_object GMenuItem *to_window_item = g_menu_item_new (_("To a New _Window"), nullptr);
    g_menu_item_set_action_and_target (to_window_item, "win.move-pane", "s", "window");
    menu_append_item (move_menu, to_window_item);
    // Swap with a neighbouring pane, where there is one
    static char const* const swaps[][2] = {
      { "left", N_("Swap With the Pane on the _Left") },
      { "right", N_("Swap With the Pane on the _Right") },
      { "up", N_("Swap With the Pane _Above") },
      { "down", N_("Swap With the Pane _Below") },
    };
    static GtkDirectionType const swap_directions[] = {
      GTK_DIR_LEFT, GTK_DIR_RIGHT, GTK_DIR_UP, GTK_DIR_DOWN
    };
    gs_unref_object GMenu *swap_section = g_menu_new ();
    for (auto i = 0u; i < G_N_ELEMENTS (swaps); ++i) {
      if (!terminal_tab_has_neighbor (tab, swap_directions[i]))
        continue;
      gs_unref_object GMenuItem *item = g_menu_item_new (_(swaps[i][1]), nullptr);
      g_menu_item_set_action_and_target (item, "win.swap-pane", "s", swaps[i][0]);
      menu_append_item (swap_section, item);
    }
    g_menu_append_section (move_menu, nullptr, G_MENU_MODEL (swap_section));
    g_menu_append_submenu (pane_section, _("_Move Pane"), G_MENU_MODEL (move_menu));

    menu_append (pane_section, _("Close P_ane"), "win.close-pane");
  }

  gs_unref_object GMenu *broadcast_menu = g_menu_new ();
  static char const* const broadcast_modes[][2] = {
    { "off", N_("_Off") },
    { "tab", N_("To All Panes in This _Tab") },
    { "window", N_("To All Terminals in This _Window") },
  };
  for (auto const& mode : broadcast_modes) {
    gs_unref_object GMenuItem *item = g_menu_item_new (_(mode[1]), nullptr);
    g_menu_item_set_action_and_target (item, "win.broadcast", "s", mode[0]);
    menu_append_item (broadcast_menu, item);
  }
  if (window->broadcast_mode != BROADCAST_OFF) {
    gs_unref_object GMenu *receive_section = g_menu_new ();
    menu_append (receive_section, _("_Include This Terminal"), "win.broadcast-receive");
    g_menu_append_section (broadcast_menu, nullptr, G_MENU_MODEL (receive_section));
  }
  gs_unref_object GMenuItem *broadcast_item = g_menu_item_new (_("_Broadcast Input"), nullptr);
  g_menu_item_set_submenu (broadcast_item, G_MENU_MODEL (broadcast_menu));
  g_menu_append_item (pane_section, broadcast_item);

  g_menu_append_section (menu, nullptr, G_MENU_MODEL (pane_section));

  /* New Terminal section */
  gs_unref_object GMenu *section6 = g_menu_new ();
  if (terminal_app_get_menu_unified (app)) {
    gs_unref_object GMenuItem *item6 = g_menu_item_new (_("New _Terminal"), nullptr);
    g_menu_item_set_action_and_target (item6, "win.new-terminal",
                                       "(ss)", "default", "current");
    menu_append_item (section6, item6);
  } else {
    gs_unref_object GMenuItem *item61 = g_menu_item_new (_("New _Window"), nullptr);
    g_menu_item_set_action_and_target (item61, "win.new-terminal",
                                       "(ss)", "window", "current");
    menu_append_item (section6, item61);
    gs_unref_object GMenuItem *item62 = g_menu_item_new (_("New _Tab"), nullptr);
    g_menu_item_set_action_and_target (item62, "win.new-terminal",
                                       "(ss)", "tab", "current");
    menu_append_item (section6, item62);
  }
  g_menu_append_section (menu, nullptr, G_MENU_MODEL (section6));

  /* Window section */
  gs_unref_object GMenu *section7 = g_menu_new ();
  if (gtk_window_is_fullscreen (GTK_WINDOW (window)))
    menu_append (section7, _("Leave Full Screen"), "win.toggle-fullscreen");
  g_menu_append_section (menu, nullptr, G_MENU_MODEL (section7));

  if (window->context_menu == nullptr) {
    window->context_menu = GTK_POPOVER_MENU (gtk_popover_menu_new_from_model (nullptr));
    g_signal_connect (window->context_menu, "closed", G_CALLBACK (menu_closed_cb), window);
    gtk_popover_set_has_arrow (GTK_POPOVER (window->context_menu), FALSE);
    gtk_popover_set_position (GTK_POPOVER (window->context_menu), GTK_POS_BOTTOM);
    gtk_widget_set_halign (GTK_WIDGET (window->context_menu), GTK_ALIGN_START);
    gtk_widget_set_parent (GTK_WIDGET (window->context_menu), GTK_WIDGET (window));
  }

  graphene_point_t point = {float(info->x), float(info->y)};
  if (gtk_widget_compute_point (GTK_WIDGET (screen), GTK_WIDGET (window), &point, &point)) {
    cairo_rectangle_int_t rect = {int(point.x), int(point.y), 1, 1};
    gtk_popover_menu_set_menu_model (window->context_menu, G_MENU_MODEL (menu));
    gtk_popover_set_pointing_to (GTK_POPOVER (window->context_menu), &rect);
    gtk_popover_popup (GTK_POPOVER (window->context_menu));
  }
}

static gboolean
screen_match_clicked_cb (TerminalScreen *screen,
                         const char *url,
                         int url_flavor,
                         guint state,
                         TerminalWindow *window)
{
  if (screen != window->active_screen)
    return FALSE;

  gtk_widget_grab_focus (GTK_WIDGET (screen));
  terminal_util_open_url (GTK_WIDGET (window), url, TerminalURLFlavor(url_flavor),
                          GDK_CURRENT_TIME);

  return TRUE;
}

/* A command entered at the prompt has finished. Tell the user if it ran for
 * a while and its terminal is not in sight. */
static void
screen_command_finished_cb(TerminalScreen* screen,
                           char const* command,
                           unsigned status,
                           guint64 duration_ms,
                           TerminalWindow* window)
{
  auto const app = terminal_app_get();
  auto const settings = terminal_app_get_global_settings(app);
  if (!g_settings_get_boolean(settings, TERMINAL_SETTING_NOTIFY_COMMAND_FINISHED_KEY))
    return;

  auto const min_duration = g_settings_get_uint(settings, TERMINAL_SETTING_NOTIFY_COMMAND_MIN_DURATION_KEY);
  if (duration_ms < guint64(min_duration) * 1000)
    return;

  auto const tab = terminal_tab_get_from_screen(screen);
  auto const tab_selected = tab && tab == window_get_active_tab(window);
  if (tab_selected && gtk_window_is_active(GTK_WINDOW(window)))
    return;

  if (tab && !tab_selected)
    terminal_notebook_set_tab_needs_attention(window->notebook, tab, true);

  terminal_app_notify_command_finished(app, screen, command, status, duration_ms);
}

/* With the visual bell, the bell marks a tab in the background, since the
 * terminal's own flash cannot be seen there */
static void
screen_bell_cb(TerminalScreen* screen,
               TerminalWindow* window)
{
  auto const profile = terminal_screen_get_profile(screen);
  if (!g_settings_get_boolean(profile, TERMINAL_PROFILE_VISUAL_BELL_KEY))
    return;

  auto const tab = terminal_tab_get_from_screen(screen);
  if (tab && tab != window_get_active_tab(window))
    terminal_notebook_set_tab_needs_attention(window->notebook, tab, true);
}

static void
screen_close_cb (TerminalScreen *screen,
                 TerminalWindow *window)
{
  terminal_window_remove_screen (window, screen);
}

static void
notebook_update_tabs_menu_cb (GtkMenuButton *button,
                              gpointer user_data)
{
  TerminalWindow *window = TERMINAL_WINDOW (user_data);
  gs_unref_object GMenu *menu;
  gs_free_list GList *tabs;
  GList *t;
  int i;

  g_assert (GTK_IS_MENU_BUTTON (button));
  g_assert (TERMINAL_IS_WINDOW (window));

  menu = g_menu_new ();
  tabs = terminal_window_list_tabs (window);

  for (t = tabs, i = 0; t != nullptr; t = t->next, i++) {
    TerminalTab *tab = (TerminalTab*)t->data;
    TerminalScreen *screen = terminal_tab_get_screen (tab);
    gs_unref_object GMenuItem *item;
    const char *title;

    if (t->next == nullptr) {
      /* Last entry. If it has no dedicated shortcut "Switch to Tab N",
       * display the accel of "Switch to Last Tab". */
      GtkApplication *app = GTK_APPLICATION (g_application_get_default ());
      gs_free gchar *detailed_action = g_strdup_printf("win.active-tab(%d)", i);
      gs_strfreev gchar **accels = gtk_application_get_accels_for_action (app, detailed_action);
      if (accels[0] == nullptr)
        i = -1;
    }

    title = terminal_screen_get_title (screen);

    item = g_menu_item_new (title && title[0] ? title : "Pano Terminal", nullptr);
    g_menu_item_set_action_and_target (item, "win.active-tab", "i", i);
    g_menu_append_item (menu, item);
  }

  gtk_menu_button_set_menu_model (button, G_MENU_MODEL (menu));
}

static void
terminal_window_fill_notebook_action_box (TerminalWindow *window,
                                          gboolean add_new_tab_button)
{
  GtkWidget *box, *new_tab_button, *tabs_menu_button;

  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  adw_tab_bar_set_end_action_widget (window->tab_bar, box);

  /* Create the NewTerminal button */
  if (add_new_tab_button)
    {
      new_tab_button = terminal_icon_button_new ("tab-new-symbolic");
      gtk_actionable_set_action_name (GTK_ACTIONABLE (new_tab_button), "win.new-terminal");
      gtk_actionable_set_action_target (GTK_ACTIONABLE (new_tab_button), "(ss)", "tab", "current");
      gtk_box_prepend (GTK_BOX (box), new_tab_button);
      gtk_widget_show (new_tab_button);
    }

  /* Create Tabs menu button */
  tabs_menu_button = gtk_menu_button_new ();
  gtk_widget_add_css_class (tabs_menu_button, "flat");
  gtk_menu_button_set_create_popup_func (GTK_MENU_BUTTON (tabs_menu_button),
                                         notebook_update_tabs_menu_cb,
                                         window, nullptr);
  gtk_box_prepend (GTK_BOX (box), tabs_menu_button);
}

static void
window_hide_ask_default_terminal(TerminalWindow* window)
{
  if (!window->ask_default_infobar ||
      !gtk_widget_get_visible(window->ask_default_infobar))
    return;

  gtk_widget_hide(window->ask_default_infobar);
  terminal_window_update_size(window);
}

static void
window_sync_ask_default_terminal_cb(TerminalApp* app,
                                    GParamSpec* pspect,
                                    TerminalWindow* window)
{
  window_hide_ask_default_terminal(window);
}

static void
window_sync_rounded_corners_cb(GSettings* settings,
                               char const* key,
                               TerminalWindow* window)
{
  auto const corners = g_settings_get_enum(settings, key);

  static char const* css_classes[3] = {
    "no-rounded-corners",
    "top-rounded-corners",
    "all-rounded-corners",
  };

  auto const widget = GTK_WIDGET(window);
  for (auto i = 0; i < 3; ++i) {
    if (corners == i)
      gtk_widget_add_css_class(widget, css_classes[i]);
    else
      gtk_widget_remove_css_class(widget, css_classes[i]);
  }
}

static void
window_sync_padding_cb(GSettings* settings,
                       char const* key,
                       TerminalWindow* window)
{
  auto const padding = g_settings_get_enum(settings, key);

  static char const* css_classes[3] = {
    "no-padding",
    "small-padding",
    "large-padding",
  };

  auto const widget = GTK_WIDGET(window);
  for (auto i = 0; i < 3; ++i) {
    if (padding == i)
      gtk_widget_add_css_class(widget, css_classes[i]);
    else
      gtk_widget_remove_css_class(widget, css_classes[i]);
  }
}

static void
window_sync_dim_panes_cb(GSettings* settings,
                         char const* key,
                         TerminalWindow* window)
{
  if (g_settings_get_boolean(settings, key))
    gtk_widget_add_css_class(GTK_WIDGET(window), "dim-inactive-panes");
  else
    gtk_widget_remove_css_class(GTK_WIDGET(window), "dim-inactive-panes");
}

static void
window_update_tabs_visibility(TerminalWindow* window)
{
  auto const position = window->tab_position;
  auto const horizontal = position == GTK_POS_TOP || position == GTK_POS_BOTTOM;
  auto const hidden = bool(window->tabs_hidden_for_fullscreen);

  gtk_widget_set_visible(GTK_WIDGET(window->tab_bar),
                         horizontal && !(hidden && position == GTK_POS_BOTTOM));
  gtk_widget_set_visible(GTK_WIDGET(window->tab_sidebar),
                         !horizontal && !hidden);

  // Only take the shortcut for the tab list from the terminal while it is there
  auto const sidebar_shown = !horizontal && !hidden;
  g_simple_action_set_enabled(lookup_action(window, "focus-tab-list"), sidebar_shown);
  g_simple_action_set_enabled(lookup_action(window, "shadow-tablist"), sidebar_shown);
}

static void
window_sync_tab_position_cb(GSettings* settings,
                            char const* key,
                            TerminalWindow* window)
{
  // The enum values are the same as GtkPositionType's
  auto const position = GtkPositionType(g_settings_get_enum(settings, key));
  GtkWidget* tab_bar = GTK_WIDGET(window->tab_bar);
  auto const horizontal = position == GTK_POS_TOP || position == GTK_POS_BOTTOM;

  if (horizontal) {
    // AdwTabBar is horizontal only; move it between the top and
    // bottom bars of the toolbar view. The bottom tab bar is placed
    // below the find bar, at the window's edge.
    auto const at_bottom = position == GTK_POS_BOTTOM;
    if (bool(window->tab_bar_at_bottom) != at_bottom) {
      g_object_ref(tab_bar);
      adw_toolbar_view_remove(window->toolbar_view, tab_bar);
      if (at_bottom)
        adw_toolbar_view_add_bottom_bar(window->toolbar_view, tab_bar);
      else
        adw_toolbar_view_add_top_bar(window->toolbar_view, tab_bar);
      g_object_unref(tab_bar);

      window->tab_bar_at_bottom = at_bottom;
    }
  } else {
    // Use the vertical tab sidebar at the left or right of the terminal
    gtk_box_reorder_child_after(GTK_BOX(window->content_box),
                                GTK_WIDGET(window->tab_sidebar),
                                position == GTK_POS_LEFT ? nullptr : window->main_vbox);
    terminal_tab_sidebar_set_position(window->tab_sidebar, position);
  }

  window->tab_position = position;
  window_update_tabs_visibility(window);

  // Keep the terminal grid size; the window grows or shrinks instead
  terminal_window_update_size(window);
}

/* Offering to import GNOME Terminal's settings, until the user answers */

static void
window_hide_import_infobar(TerminalWindow* window)
{
  if (window->import_infobar)
    gtk_widget_set_visible(window->import_infobar, false);
}

static void
window_import_offered_changed_cb(GSettings* settings,
                                 char const* key,
                                 TerminalWindow* window)
{
  if (g_settings_get_boolean(settings, key))
    window_hide_import_infobar(window);
}

static void
import_infobar_response_cb(GtkInfoBar* infobar,
                           int response,
                           TerminalWindow* window)
{
  auto const settings = terminal_app_get_global_settings(terminal_app_get());

  if (response == GTK_RESPONSE_YES) {
    auto const n_profiles = terminal_gnome_terminal_import();
    gs_free char* title =
      g_strdup_printf(g_dngettext(GETTEXT_PACKAGE,
                                  "Imported %u profile and the preferences of GNOME Terminal",
                                  "Imported %u profiles and the preferences of GNOME Terminal",
                                  n_profiles),
                      n_profiles);
    adw_toast_overlay_add_toast(window->toast_overlay, adw_toast_new(title));
    g_settings_set_boolean(settings, TERMINAL_SETTING_GNOME_TERMINAL_IMPORT_OFFERED_KEY, true);
  } else if (response == GTK_RESPONSE_NO) {
    g_settings_set_boolean(settings, TERMINAL_SETTING_GNOME_TERMINAL_IMPORT_OFFERED_KEY, true);
  } else { // GTK_RESPONSE_CLOSE: ask again in the next window
    window_hide_import_infobar(window);
  }
}

static void
window_offer_import(TerminalWindow* window)
{
  auto const app = terminal_app_get();
  auto const settings = terminal_app_get_global_settings(app);
  if (g_settings_get_boolean(settings, TERMINAL_SETTING_GNOME_TERMINAL_IMPORT_OFFERED_KEY) ||
      !terminal_gnome_terminal_settings_exist(terminal_app_get_settings_backend(app)))
    return;

  auto const infobar = window->import_infobar = gtk_info_bar_new();
  gtk_info_bar_set_show_close_button(GTK_INFO_BAR(infobar), true);
  gtk_info_bar_set_message_type(GTK_INFO_BAR(infobar), GTK_MESSAGE_QUESTION);

  auto const question = gtk_label_new(_("Import your profiles, keyboard shortcuts and preferences from GNOME Terminal?"));
  gtk_label_set_wrap(GTK_LABEL(question), true);
  gtk_label_set_xalign(GTK_LABEL(question), 0.0f);
  gtk_info_bar_add_child(GTK_INFO_BAR(infobar), question);

  gtk_info_bar_add_button(GTK_INFO_BAR(infobar), _("_Import"), GTK_RESPONSE_YES);
  gtk_info_bar_add_button(GTK_INFO_BAR(infobar), _("_No"), GTK_RESPONSE_NO);
  g_signal_connect(infobar, "response",
                   G_CALLBACK(import_infobar_response_cb), window);

  gtk_box_prepend(GTK_BOX(window->main_vbox), infobar);
  g_signal_connect(settings, "changed::" TERMINAL_SETTING_GNOME_TERMINAL_IMPORT_OFFERED_KEY,
                   G_CALLBACK(window_import_offered_changed_cb), window);
}

static void
default_infobar_response_cb(GtkInfoBar* infobar,
                            int response,
                            TerminalWindow* window)
{
  auto const app = terminal_app_get();

  if (response == GTK_RESPONSE_YES) {
    terminal_app_make_default_terminal(app);
    terminal_app_unset_ask_default_terminal(app);
  } else if (response == GTK_RESPONSE_NO) {
    terminal_app_unset_ask_default_terminal(app);
  } else { // GTK_RESPONSE_CLOSE
    window_hide_ask_default_terminal(window);
  }
}

static void
screen_pty_paste_primary_cb(TerminalScreen* screen,
                            GParamSpec* pspec,
                            void* user_data)
{
  if (!vte_terminal_get_pty(VTE_TERMINAL(screen)))
    return;

  g_signal_handlers_disconnect_by_func(screen, (void*)screen_pty_paste_primary_cb, user_data);
  vte_terminal_paste_primary(VTE_TERMINAL(screen));
}

/* Pastes the primary selection into @screen. A terminal that was never
 * shown has not started its shell yet, and would drop the text. */
static void
screen_paste_primary(TerminalScreen* screen)
{
  if (vte_terminal_get_pty(VTE_TERMINAL(screen))) {
    vte_terminal_paste_primary(VTE_TERMINAL(screen));
    return;
  }

  g_signal_handlers_disconnect_by_func(screen, (void*)screen_pty_paste_primary_cb, nullptr);
  g_signal_connect(screen, "notify::pty", G_CALLBACK(screen_pty_paste_primary_cb), nullptr);
}

/* A middle click on a tab, in the tab bar or the tab list, does what the
 * tab-middle-click setting says */
static void
window_tab_middle_clicked(TerminalWindow* window,
                          AdwTabPage* page)
{
  auto const tab = TERMINAL_TAB(adw_tab_page_get_child(page));
  auto const settings = terminal_app_get_global_settings(terminal_app_get());

  switch (TerminalTabMiddleClick(g_settings_get_enum(settings, TERMINAL_SETTING_TAB_MIDDLE_CLICK_KEY))) {
  case TERMINAL_TAB_MIDDLE_CLICK_CLOSE:
    terminal_notebook_close_tab(window->notebook, tab);
    // window may be destroyed at this point
    break;
  case TERMINAL_TAB_MIDDLE_CLICK_PASTE: {
    auto const screen = terminal_tab_get_screen(tab);
    if (!screen)
      break;

    terminal_notebook_set_active_screen(window->notebook, screen);
    screen_paste_primary(screen);
    break;
  }
  case TERMINAL_TAB_MIDDLE_CLICK_NOTHING:
  default:
    break;
  }
}

static void
tab_sidebar_page_middle_clicked_cb(TerminalTabSidebar* sidebar,
                                   AdwTabPage* page,
                                   TerminalWindow* window)
{
  window_tab_middle_clicked(window, page);
}

// Adwaita's tab bar always closes tabs on middle click, and there is no
// API to change that. A capturing gesture claims the middle click before
// AdwTabBox sees it, and then does what the setting says.

static void
tab_bar_click_pressed_cb(GtkGesture* gesture,
                         int n_press,
                         double x,
                         double y,
                         void* user_data)
{
  gtk_gesture_set_state (gesture, GTK_EVENT_SEQUENCE_CLAIMED);
}

/* Returns: (transfer none) (nullable): the page of the tab at @x, @y in
 * the AdwTabBox @box */
static AdwTabPage*
tab_box_get_page_at(GtkWidget* box,
                    double x,
                    double y)
{
  // AdwTab is private, but has a "page" property
  static auto const tab_type = g_type_from_name("AdwTab");
  if (!tab_type)
    return nullptr;

  for (auto widget = gtk_widget_pick(box, x, y, GTK_PICK_DEFAULT);
       widget != nullptr && widget != box;
       widget = gtk_widget_get_parent(widget)) {
    if (!g_type_is_a(G_OBJECT_TYPE(widget), tab_type))
      continue;
    if (!g_object_class_find_property(G_OBJECT_GET_CLASS(widget), "page"))
      return nullptr;

    AdwTabPage* page = nullptr;
    g_object_get(widget, "page", &page, nullptr);
    if (page)
      g_object_unref(page); // the view keeps it
    return page;
  }

  return nullptr;
}

static void
tab_bar_click_released_cb(GtkGesture* gesture,
                          int n_press,
                          double x,
                          double y,
                          TerminalWindow* window)
{
  gtk_gesture_set_state (gesture, GTK_EVENT_SEQUENCE_CLAIMED);

  auto const box = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  if (auto const page = tab_box_get_page_at(box, x, y))
    window_tab_middle_clicked(window, page);
}

static void
capture_middle_click(GtkWidget* widget,
                     TerminalWindow* window)
{
  auto gesture = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture),
                                GDK_BUTTON_MIDDLE);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(gesture),
                                             GTK_PHASE_CAPTURE);
  g_signal_connect(gesture, "pressed",
                   G_CALLBACK(tab_bar_click_pressed_cb), nullptr);
  g_signal_connect(gesture, "released",
                   G_CALLBACK(tab_bar_click_released_cb), window);

  gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(gesture)); // adopts gesture
}

static void
recurse_capture_middle_click(GtkWidget *widget,
                             GType type,
                             TerminalWindow* window)
{
  if (g_type_is_a(G_OBJECT_TYPE(widget), type)) {
    capture_middle_click(widget, window);
    return;
  }

  for (auto child = gtk_widget_get_first_child(widget);
       child != nullptr;
       child = gtk_widget_get_next_sibling(child)) {
    recurse_capture_middle_click(child, type, window);
  }
}

/*****************************************/

static void
terminal_window_realize (GtkWidget *widget)
{
  TerminalWindow *window = TERMINAL_WINDOW (widget);
  GtkAllocation widget_allocation;
  GdkSurface *surface;

  gtk_widget_get_allocation (widget, &widget_allocation);

  _terminal_debug_print (TERMINAL_DEBUG_GEOMETRY,
                         "[window %p] realize, size %d : %d at (%d, %d)\n",
                         widget,
                         widget_allocation.width, widget_allocation.height,
                         widget_allocation.x, widget_allocation.y);

  GTK_WIDGET_CLASS (terminal_window_parent_class)->realize (widget);

  surface = gtk_native_get_surface (GTK_NATIVE (window));

  g_signal_connect_object (surface,
                           "notify::state",
                           G_CALLBACK (terminal_window_state_event),
                           window,
                           G_CONNECT_SWAPPED);

  /* Now that we've been realized, we should know precisely how large the
   * client-side decorations are going to be. Recalculate the geometry hints,
   * export them to the windowing system, and resize the window accordingly. */
  window->realized = TRUE;
  terminal_window_update_size (window);
}

static void
terminal_window_state_event (GtkWidget *widget,
                             GParamSpec *pspec,
                             GdkToplevel *surface)
{
  auto const window = TERMINAL_WINDOW(widget);
  auto window_state = gdk_toplevel_get_state (surface);
  auto changed_mask = GdkToplevelState(unsigned(window->window_state) ^ unsigned(window_state));

  _terminal_debug_print (TERMINAL_DEBUG_GEOMETRY,
                         "Window state changed mask %x old state %x new state %x\n",
                         unsigned(changed_mask),
                         unsigned(window->window_state),
                         unsigned(window_state));

  window->window_state = window_state;

  if (changed_mask & GDK_TOPLEVEL_STATE_FULLSCREEN)
    {
      auto const is_fullscreen = (window->window_state & GDK_TOPLEVEL_STATE_FULLSCREEN) != 0;

      g_simple_action_set_state (lookup_action (window, "fullscreen"),
                                 g_variant_new_boolean (is_fullscreen));
      g_simple_action_set_enabled (lookup_action (window, "size-to"),
                                   !is_fullscreen);
    }
}

static void
terminal_window_init (TerminalWindow *window)
{
  const GActionEntry action_entries[] = {
    /* Actions without state */
    { "about",               action_about_cb,            nullptr,   nullptr, nullptr },
    { "close",               action_close_cb,            "s",    nullptr, nullptr },
    { "close-pane",          action_close_pane_cb,       nullptr,   nullptr, nullptr },
    { "reopen-tab",          action_reopen_tab_cb,       nullptr,   nullptr, nullptr },
    { "set-title",           action_set_title_cb,        nullptr,   nullptr, nullptr },
    { "copy-output",         action_copy_output_cb,      nullptr,   nullptr, nullptr },
    { "copy-output-here",    action_copy_output_here_cb, nullptr,   nullptr, nullptr },
    { "scroll-to-prompt",    action_prompt_cb,           "s",    nullptr, nullptr },
    { "copy",                action_copy_cb,             "s",    nullptr, nullptr },
    { "copy-hyperlink",      action_copy_hyperlink_cb,   nullptr,   nullptr, nullptr },
    { "paste-hyperlink",     action_paste_hyperlink_cb,  nullptr,   nullptr, nullptr },
    { "copy-match",          action_copy_match_cb,       nullptr,   nullptr, nullptr },
    { "edit-preferences",    action_edit_preferences_cb, nullptr,   nullptr, nullptr },
    { "toggle-fullscreen",   action_toggle_fullscreen_cb,nullptr,   nullptr, nullptr },
    { "find",                action_find_cb,             nullptr,   nullptr, nullptr },
    { "find-backward",       action_find_backward_cb,    nullptr,   nullptr, nullptr },
    { "find-clear",          action_find_clear_cb,       nullptr,   nullptr, nullptr },
    { "find-forward",        action_find_forward_cb,     nullptr,   nullptr, nullptr },
    { "help",                action_help_cb,             nullptr,   nullptr, nullptr },
    { "inspector",           action_inspector_cb,        nullptr,   nullptr, nullptr },
    { "new-terminal",        action_new_terminal_cb,     "(ss)", nullptr, nullptr },
    { "new-terminal-in-container", action_new_terminal_in_container_cb, "(ss)", nullptr, nullptr },
    { "open-match",          action_open_match_cb,       nullptr,   nullptr, nullptr },
    { "open-hyperlink",      action_open_hyperlink_cb,   nullptr,   nullptr, nullptr },
    { "pane-focus",          action_pane_focus_cb,       "s",    nullptr, nullptr },
    { "pane-resize",         action_pane_resize_cb,      "s",    nullptr, nullptr },
    { "swap-pane",           action_swap_pane_cb,        "s",    nullptr, nullptr },
    { "paste-text",          action_paste_text_cb,       nullptr,   nullptr, nullptr },
    { "paste-uris",          action_paste_uris_cb,       nullptr,   nullptr, nullptr },
    { "reset",               action_reset_cb,            "b",    nullptr, nullptr },
    { "select-all",          action_select_all_cb,       nullptr,   nullptr, nullptr },
    { "size-to",             action_size_to_cb,          "(uu)", nullptr, nullptr },
    { "split",               action_split_cb,            "s",    nullptr, nullptr },
    { "equalize-panes",      action_equalize_panes_cb,   nullptr, nullptr, nullptr },
    { "command-palette",     action_command_palette_cb,  nullptr, nullptr, nullptr },
    { "quick-select",        action_quick_select_cb,     nullptr, nullptr, nullptr },
    { "move-pane",           action_move_pane_cb,        "s",    nullptr, nullptr },
    { "tab-detach",          action_tab_detach_cb,       nullptr,   nullptr, nullptr },
    { "tab-move-left",       action_tab_move_left_cb,    nullptr,   nullptr, nullptr },
    { "tab-move-right",      action_tab_move_right_cb,   nullptr,   nullptr, nullptr },
    { "tab-move-start",      action_tab_move_start_cb,   nullptr,   nullptr, nullptr },
    { "tab-move-end",        action_tab_move_end_cb,     nullptr,   nullptr, nullptr },
    { "tab-switch-left",     action_tab_switch_left_cb,  nullptr,   nullptr, nullptr },
    { "tab-switch-right",    action_tab_switch_right_cb, nullptr,   nullptr, nullptr },
    { "tabs-menu",           nullptr,                       nullptr,   nullptr, nullptr },
    { "zoom-in",             action_zoom_in_cb,          nullptr,   nullptr, nullptr },
    { "zoom-normal",         action_zoom_normal_cb,      nullptr,   nullptr, nullptr },
    { "zoom-out",            action_zoom_out_cb,         nullptr,   nullptr, nullptr },
    { "save-contents",       action_save_contents_cb,    nullptr,   nullptr, nullptr },

    /* Shadow actions for keybinding comsumption, see comment in terminal-accels.c */
    { "shadow",              action_shadow_activate_cb,  "s",    nullptr, nullptr },
    { "shadow-mdi",          action_shadow_activate_cb,  "s",    nullptr, nullptr },
    { "shadow-pane",         action_shadow_activate_cb,  "s",    nullptr, nullptr },
    { "shadow-prompt",       action_shadow_activate_cb,  "s",    nullptr, nullptr },
    { "shadow-tablist",      action_shadow_activate_cb,  "s",    nullptr, nullptr },
    { "focus-tab-list",      action_focus_tab_list_cb,   nullptr,   nullptr, nullptr },
    { "header-menu",         action_header_menu_cb,      nullptr,   nullptr, nullptr },

    /* Actions with state */
    { "active-tab",          action_active_tab_set_cb,   "i",  "@i 0",    action_active_tab_state_cb      },
    { "fullscreen",          nullptr /* toggles state */,   nullptr, "false",   action_fullscreen_state_cb      },
    { "profile",             nullptr /* changes state */,   "s",  "''",      action_profile_state_cb         },
    { "read-only",           nullptr /* toggles state */,   nullptr, "false",   action_read_only_state_cb       },
    { "monitor-activity",    nullptr /* toggles state */,   nullptr, "false",   action_monitor_state_cb         },
    { "monitor-silence",     nullptr /* toggles state */,   nullptr, "false",   action_monitor_state_cb         },
    { "zoom-pane",           nullptr /* toggles state */,   nullptr, "false",   action_zoom_pane_state_cb       },
    { "broadcast",           action_broadcast_cb,           "s",  "'off'",   action_broadcast_state_cb       },
    { "broadcast-receive",   nullptr /* toggles state */,   nullptr, "true",    action_broadcast_receive_state_cb },
  };
  static GActionEntry const notebook_context_action_entries[] = {
    { "notebook-tab-close",      action_notebook_tab_close_cb,      nullptr, nullptr, nullptr },
    { "notebook-tab-close-others", action_notebook_tab_close_others_cb, nullptr, nullptr, nullptr },
    { "notebook-set-title",      action_notebook_set_title_cb,      nullptr, nullptr, nullptr },
    { "notebook-tab-detach",     action_notebook_tab_detach_cb,     nullptr, nullptr, nullptr },
    { "notebook-tab-move-left",  action_notebook_tab_move_left_cb,  nullptr, nullptr, nullptr },
    { "notebook-tab-move-right", action_notebook_tab_move_right_cb, nullptr, nullptr, nullptr },
    { "notebook-tab-pin",        action_notebook_tab_pin_cb,        nullptr, nullptr, nullptr },
    { "notebook-tab-unpin",      action_notebook_tab_unpin_cb,      nullptr, nullptr, nullptr },
  };

  TerminalApp *app;
  GSettings *gtk_debug_settings;
  GtkWindowGroup *window_group;
  //  GtkAccelGroup *accel_group;
  uuid_t u;
  char uuidstr[37];
  GSimpleAction *action;

  app = terminal_app_get ();

  window->active_screen = nullptr;

  window->old_char_width = -1;
  window->old_char_height = -1;

  window->old_chrome_width = -1;
  window->old_chrome_height = -1;
  window->old_csd_width = -1;
  window->old_csd_height = -1;
  window->old_padding_width = -1;
  window->old_padding_height = -1;

  window->old_geometry_widget = nullptr;

  // Matches the template; updated from the settings in constructed()
  window->tab_position = GTK_POS_TOP;
  window->tab_bar_at_bottom = FALSE;

  uuid_generate (u);
  uuid_unparse (u, uuidstr);
  window->uuid = g_strdup (uuidstr);

  gtk_widget_init_template (GTK_WIDGET (window));

  terminal_headerbar_connect_menus_closed (window->headerbar,
                                           G_CALLBACK (menu_closed_cb), window);

  /* GAction setup */
  g_action_map_add_action_entries (G_ACTION_MAP (window),
                                   action_entries, G_N_ELEMENTS (action_entries),
                                   window);

  // Property actions
  gs_unref_object auto propaction = g_property_action_new("tab-overview",
                                                          window->tab_overview,
                                                          "open");
  g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(propaction));

  // Notebook actions
  gs_unref_object auto action_group = window->notebook_context_action_group =
    G_ACTION_MAP(g_simple_action_group_new());

  g_action_map_add_action_entries(action_group,
                                  notebook_context_action_entries,
                                  G_N_ELEMENTS(notebook_context_action_entries),
                                  window);
  gtk_widget_insert_action_group(GTK_WIDGET(window),
                                 "notebook",
                                 G_ACTION_GROUP(action_group));

  /* Add "Set as default terminal" infobar */
  if (terminal_app_get_ask_default_terminal(app)) {
    auto const infobar = window->ask_default_infobar = gtk_info_bar_new();
    gtk_info_bar_set_show_close_button(GTK_INFO_BAR(infobar), true);
    gtk_info_bar_set_message_type(GTK_INFO_BAR(infobar), GTK_MESSAGE_QUESTION);

    auto const question = gtk_label_new (_("Set Pano Terminal as your default terminal?"));
    gtk_label_set_wrap(GTK_LABEL(question), true);
    gtk_info_bar_add_child(GTK_INFO_BAR(infobar), question);
    gtk_widget_show(question);

    gtk_info_bar_add_button(GTK_INFO_BAR(infobar), _("_Yes"), GTK_RESPONSE_YES);
    gtk_info_bar_add_button(GTK_INFO_BAR(infobar), _("_No"), GTK_RESPONSE_NO);

    g_signal_connect (infobar, "response",
                      G_CALLBACK(default_infobar_response_cb), window);

    gtk_box_prepend(GTK_BOX(window->main_vbox), infobar);

    gtk_widget_show(infobar);
    g_signal_connect(app, "notify::ask-default-terminal",
                     G_CALLBACK(window_sync_ask_default_terminal_cb), window);
  }

  window_offer_import(window);

  /* Maybe make Inspector available */
  action = lookup_action (window, "inspector");
  gtk_debug_settings = terminal_app_get_gtk_debug_settings (app);
  if (gtk_debug_settings != nullptr)
    g_settings_bind (gtk_debug_settings,
                     "enable-inspector-keybinding",
                     action,
                     "enabled",
                     GSettingsBindFlags(G_SETTINGS_BIND_GET |
					G_SETTINGS_BIND_NO_SENSITIVITY));
  else
    g_simple_action_set_enabled (action, FALSE);

  window->clipboard = gtk_widget_get_clipboard (GTK_WIDGET (window));
  clipboard_targets_changed_cb (app, window->clipboard, window);
  g_signal_connect (app, "clipboard-targets-changed",
                    G_CALLBACK (clipboard_targets_changed_cb), window);

  g_signal_connect_swapped (window->find_bar, "notify::has-search",
                            G_CALLBACK (terminal_window_update_search_sensitivity), window);

  g_simple_action_set_enabled (lookup_action (window, "reopen-tab"),
                               !g_queue_is_empty (&closed_tabs));

  g_settings_bind (terminal_app_get_global_settings (app),
                   TERMINAL_SETTING_TAB_SIDEBAR_WIDTH_KEY,
                   window->tab_sidebar, "width",
                   G_SETTINGS_BIND_DEFAULT);
  g_signal_connect (window->tab_sidebar, "page-middle-clicked",
                    G_CALLBACK (tab_sidebar_page_middle_clicked_cb), window);

  // Panes dropped onto the tab bar or the tab list become tabs there
  GType pane_types[] = { TERMINAL_TYPE_SCREEN };
  adw_tab_bar_setup_extra_drop_target (window->tab_bar, GDK_ACTION_MOVE,
                                       pane_types, G_N_ELEMENTS (pane_types));
  adw_tab_bar_set_extra_drag_preload (window->tab_bar, true);
  g_signal_connect (window->tab_bar, "extra-drag-drop",
                    G_CALLBACK (tab_bar_extra_drag_drop_cb), window);
  g_signal_connect (window->tab_bar, "extra-drag-value",
                    G_CALLBACK (tab_bar_extra_drag_value_cb), window);
  g_signal_connect (window->tab_sidebar, "screen-dropped",
                    G_CALLBACK (sidebar_screen_dropped_cb), window);

  auto const transfer_motion = gtk_drop_controller_motion_new ();
  g_signal_connect (transfer_motion, "enter",
                    G_CALLBACK (window_transfer_motion_cb), window);
  g_signal_connect (transfer_motion, "motion",
                    G_CALLBACK (window_transfer_motion_cb), window);
  g_signal_connect (transfer_motion, "leave",
                    G_CALLBACK (window_transfer_leave_cb), window);
  gtk_widget_add_controller (GTK_WIDGET (window->notebook), transfer_motion);
  g_signal_connect (terminal_notebook_get_tab_view (window->notebook), "page-attached",
                    G_CALLBACK (window_page_attached_cb), window);

  window->broadcast_banner = ADW_BANNER (adw_banner_new (""));
  adw_banner_set_button_label (window->broadcast_banner, _("_Stop"));
  gtk_actionable_set_action_name (GTK_ACTIONABLE (window->broadcast_banner), "win.broadcast");
  gtk_actionable_set_action_target (GTK_ACTIONABLE (window->broadcast_banner), "s", "off");
  gtk_box_prepend (GTK_BOX (window->main_vbox), GTK_WIDGET (window->broadcast_banner));

  g_signal_connect_object (terminal_app_get_profiles_list (app),
                           "child-changed::" TERMINAL_PROFILE_BACKGROUND_OPACITY_KEY,
                           G_CALLBACK (profile_opacity_changed_cb),
                           window,
                           G_CONNECT_SWAPPED);

  terminal_window_fill_notebook_action_box (window, FALSE);

  window_group = gtk_window_group_new ();
  gtk_window_group_add_window (window_group, GTK_WINDOW (window));
  g_object_unref (window_group);

#ifdef GDK_WINDOWING_X11
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (window));
  if (GDK_IS_X11_SURFACE (surface)) {
    char role[64];
    g_snprintf (role, sizeof (role), "pano-terminal-window-%s", uuidstr);
    gdk_x11_surface_set_utf8_property (surface, "WM_WINDOW_ROLE", role);
  }
#endif

#ifdef ENABLE_DEBUG
  _TERMINAL_DEBUG_IF(TERMINAL_DEBUG_FOCUS) {
    _terminal_debug_attach_focus_listener(GTK_WIDGET(window));
  }
#endif

  // Intercept middle-click on the tab bar, see above
  recurse_capture_middle_click(GTK_WIDGET(window->tab_bar),
                               g_type_from_name("AdwTabBox"),
                               window);
}

static void
terminal_window_css_changed (GtkWidget *widget,
                             GtkCssStyleChange *change)
{
  TerminalWindow *window = TERMINAL_WINDOW (widget);

  GTK_WIDGET_CLASS (terminal_window_parent_class)->css_changed (widget, change);

  terminal_window_update_size (window);
}

static gboolean
policy_type_to_autohide (GValue   *value,
                         GVariant *variant,
                         gpointer  user_data)
{
  auto policy_type = g_variant_get_string (variant, nullptr);
  gboolean autohide = g_strcmp0 (policy_type, "automatic") == 0;
  g_value_set_boolean (value, autohide);
  return TRUE;
}

static void
terminal_window_size_allocate (GtkWidget *widget,
                               int width,
                               int height,
                               int baseline)
{
  TerminalWindow *window = TERMINAL_WINDOW (widget);

  GTK_WIDGET_CLASS (terminal_window_parent_class)->size_allocate (widget, width, height, baseline);

  if (window->context_menu != nullptr) {
    gtk_popover_present (GTK_POPOVER (window->context_menu));
  }

}

static void
terminal_window_constructed (GObject *object)
{
  TerminalWindow *window = TERMINAL_WINDOW (object);
  GSettings *settings;

  G_OBJECT_CLASS (terminal_window_parent_class)->constructed (object);

  settings = terminal_app_get_global_settings (terminal_app_get ());

  g_settings_bind_with_mapping (settings,
                                TERMINAL_SETTING_TAB_POLICY_KEY,
                                window->tab_bar,
                                "autohide",
                                GSettingsBindFlags(G_SETTINGS_BIND_GET |
                                                   G_SETTINGS_BIND_NO_SENSITIVITY),
                                policy_type_to_autohide, nullptr, nullptr, nullptr);
  g_settings_bind_with_mapping (settings,
                                TERMINAL_SETTING_TAB_POLICY_KEY,
                                window->tab_sidebar,
                                "autohide",
                                GSettingsBindFlags(G_SETTINGS_BIND_GET |
                                                   G_SETTINGS_BIND_NO_SENSITIVITY),
                                policy_type_to_autohide, nullptr, nullptr, nullptr);

  window_sync_tab_position_cb(settings, TERMINAL_SETTING_TAB_POSITION_KEY, window);
  g_signal_connect(settings, "changed::" TERMINAL_SETTING_TAB_POSITION_KEY,
                   G_CALLBACK(window_sync_tab_position_cb), window);

  window_sync_rounded_corners_cb(settings, TERMINAL_SETTING_ROUNDED_CORNERS_KEY, window);
  g_signal_connect(settings, "changed::" TERMINAL_SETTING_ROUNDED_CORNERS_KEY,
                   G_CALLBACK(window_sync_rounded_corners_cb), window);
  window_sync_padding_cb(settings, TERMINAL_SETTING_TERMINAL_PADDING_KEY, window);
  g_signal_connect(settings, "changed::" TERMINAL_SETTING_TERMINAL_PADDING_KEY,
                   G_CALLBACK(window_sync_padding_cb), window);
  window_sync_dim_panes_cb(settings, TERMINAL_SETTING_DIM_INACTIVE_PANES_KEY, window);
  g_signal_connect(settings, "changed::" TERMINAL_SETTING_DIM_INACTIVE_PANES_KEY,
                   G_CALLBACK(window_sync_dim_panes_cb), window);
  g_signal_connect(settings, "changed::" TERMINAL_SETTING_HEADER_BAR_COLORS_KEY,
                   G_CALLBACK(window_header_bar_colors_changed_cb), window);
  g_signal_connect(window, "notify::is-active",
                   G_CALLBACK(window_is_active_changed_cb), nullptr);
}

static void
terminal_window_class_init (TerminalWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GtkWindowClass *window_class = GTK_WINDOW_CLASS (klass);

  object_class->constructed = terminal_window_constructed;
  object_class->dispose = terminal_window_dispose;
  object_class->finalize = terminal_window_finalize;

  widget_class->show = terminal_window_show;
  widget_class->realize = terminal_window_realize;
  widget_class->css_changed = terminal_window_css_changed;
  widget_class->size_allocate = terminal_window_size_allocate;

  window_class->close_request = terminal_window_close_request;

  gtk_widget_class_set_template_from_resource (widget_class, "/com/aripd/pano/ui/window.ui");

  gtk_widget_class_bind_template_callback (widget_class, screen_close_request_cb);
  gtk_widget_class_bind_template_callback (widget_class, notebook_screen_switched_cb);
  gtk_widget_class_bind_template_callback (widget_class, notebook_screen_added_cb);
  gtk_widget_class_bind_template_callback (widget_class, notebook_screen_removed_cb);
  gtk_widget_class_bind_template_callback (widget_class, notebook_screens_reordered_cb);
  gtk_widget_class_bind_template_callback (widget_class, notebook_setup_menu_cb);
  gtk_widget_class_bind_template_callback (widget_class, terminal_window_update_geometry);
  gtk_widget_class_bind_template_callback (widget_class, handle_tab_dropped_on_desktop);
  gtk_widget_class_bind_template_callback (widget_class, terminal_window_tab_overview_notify_open_cb);
  gtk_widget_class_bind_template_callback (widget_class, terminal_window_tab_overview_create_tab_cb);

  gtk_widget_class_bind_template_child (widget_class, TerminalWindow, find_bar);
  gtk_widget_class_bind_template_child (widget_class, TerminalWindow, find_bar_revealer);
  gtk_widget_class_bind_template_child (widget_class, TerminalWindow, headerbar);
  gtk_widget_class_bind_template_child (widget_class, TerminalWindow, content_box);
  gtk_widget_class_bind_template_child (widget_class, TerminalWindow, main_vbox);
  gtk_widget_class_bind_template_child (widget_class, TerminalWindow, notebook);
  gtk_widget_class_bind_template_child (widget_class, TerminalWindow, tab_bar);
  gtk_widget_class_bind_template_child (widget_class, TerminalWindow, tab_overview);
  gtk_widget_class_bind_template_child (widget_class, TerminalWindow, tab_sidebar);
  gtk_widget_class_bind_template_child (widget_class, TerminalWindow, toast_overlay);
  gtk_widget_class_bind_template_child (widget_class, TerminalWindow, toolbar_view);

  g_type_ensure (TERMINAL_TYPE_HEADERBAR);
  g_type_ensure (TERMINAL_TYPE_FIND_BAR);
  g_type_ensure (TERMINAL_TYPE_NOTEBOOK);
  g_type_ensure (TERMINAL_TYPE_TAB_SIDEBAR);
}

static void
terminal_window_dispose (GObject *object)
{
  TerminalWindow *window = TERMINAL_WINDOW (object);
  TerminalApp *app = terminal_app_get ();

  if (window->closed_tab_toast) {
    g_object_remove_weak_pointer(G_OBJECT(window->closed_tab_toast),
                                 reinterpret_cast<void**>(&window->closed_tab_toast));
    window->closed_tab_toast = nullptr;
  }
  if (window->copy_toast) {
    g_object_remove_weak_pointer(G_OBJECT(window->copy_toast),
                                 reinterpret_cast<void**>(&window->copy_toast));
    window->copy_toast = nullptr;
  }
  if (window->tab_drop_screen) {
    g_object_remove_weak_pointer(G_OBJECT(window->tab_drop_screen),
                                 reinterpret_cast<void**>(&window->tab_drop_screen));
    window->tab_drop_screen = nullptr;
  }

  // Before the tabs go away, so that the last window can be restored
  if (!window->disposed)
    terminal_session_window_closing (window);

  window->disposed = TRUE;

  gtk_widget_dispose_template (GTK_WIDGET (window), TERMINAL_TYPE_WINDOW);

  g_clear_pointer ((GtkWidget **)&window->context_menu, gtk_widget_unparent);
  g_clear_handle_id (&window->fullscreen_transition, g_source_remove);
  g_clear_handle_id (&window->refocus_idle, g_source_remove);
  g_clear_handle_id(&window->focus_active_tab_source, g_source_remove);

  if (window->clipboard != nullptr) {
    g_signal_handlers_disconnect_by_func (app,
                                          (void*)clipboard_targets_changed_cb,
                                          window);
    window->clipboard = nullptr;
  }

  if (window->ask_default_infobar) {
    g_signal_handlers_disconnect_by_func(app,
                                         (void*)window_sync_ask_default_terminal_cb,
                                         window);
    window->ask_default_infobar = nullptr;
  }

  remove_popup_info (window);

  window->notebook_context_action_group = nullptr;

  auto const settings = terminal_app_get_global_settings(terminal_app_get());
  g_signal_handlers_disconnect_by_func(settings,
                                       (void*)window_sync_rounded_corners_cb,
                                       window);
  g_signal_handlers_disconnect_by_func(settings,
                                       (void*)window_sync_padding_cb,
                                       window);
  g_signal_handlers_disconnect_by_func(settings,
                                       (void*)window_sync_dim_panes_cb,
                                       window);
  g_signal_handlers_disconnect_by_func(settings,
                                       (void*)window_header_bar_colors_changed_cb,
                                       window);
  if (window->colors_provider) {
    gtk_style_context_remove_provider_for_display(gtk_widget_get_display(GTK_WIDGET(window)),
                                                  GTK_STYLE_PROVIDER(window->colors_provider));
    g_clear_object(&window->colors_provider);
  }
  g_clear_pointer(&window->colors_class, g_free);
  g_signal_handlers_disconnect_by_func(settings,
                                       (void*)window_import_offered_changed_cb,
                                       window);
  window->import_infobar = nullptr;
  g_signal_handlers_disconnect_by_func(settings,
                                       (void*)window_sync_tab_position_cb,
                                       window);

  G_OBJECT_CLASS (terminal_window_parent_class)->dispose (object);
}

static void
terminal_window_finalize (GObject *object)
{
  TerminalWindow *window = TERMINAL_WINDOW (object);

  if (window->confirm_close_dialog)
    gtk_dialog_response (GTK_DIALOG (window->confirm_close_dialog),
                         GTK_RESPONSE_DELETE_EVENT);

  g_free (window->uuid);

  G_OBJECT_CLASS (terminal_window_parent_class)->finalize (object);
}

static gboolean
terminal_window_close_request (GtkWindow *window)
{
  /* Closing the quick terminal from the window manager only hides it */
  if (TERMINAL_WINDOW (window)->quick) {
    gtk_widget_set_visible (GTK_WIDGET (window), false);
    return TRUE;
  }

  return confirm_close_window_or_tab (TERMINAL_WINDOW (window), nullptr, CloseKind::WINDOW);
}

static void
terminal_window_show (GtkWidget *widget)
{
  TerminalWindow *window = TERMINAL_WINDOW (widget);
  GtkAllocation widget_allocation;

  gtk_widget_get_allocation (widget, &widget_allocation);

  _terminal_debug_print (TERMINAL_DEBUG_GEOMETRY,
                         "[window %p] show, size %d : %d at (%d, %d)\n",
                         widget,
                         widget_allocation.width, widget_allocation.height,
                         widget_allocation.x, widget_allocation.y);

  if (window->active_screen != nullptr)
    {
      /* At this point, we have our GdkScreen, and hence the right
       * font size, so we can go ahead and size the window. */
      terminal_window_update_size (window);
    }

  GTK_WIDGET_CLASS (terminal_window_parent_class)->show (widget);
}

TerminalWindow*
terminal_window_new (GApplication *app)
{
  return reinterpret_cast<TerminalWindow*>
    (g_object_new (TERMINAL_TYPE_WINDOW,
		   "application", app,
		   nullptr));
}

static void
profile_set_cb (TerminalScreen *screen,
                GSettings *old_profile,
                TerminalWindow *window)
{
  if (!gtk_widget_get_realized (GTK_WIDGET (window)))
    return;

  if (terminal_tab_get_from_screen(screen) == window_get_active_tab(window))
    window_sync_translucency(window);

  if (screen != window->active_screen)
    return;

  terminal_window_update_set_profile_menu_active_profile (window);
}

static void
sync_screen_title (TerminalScreen *screen,
                   GParamSpec *psepc,
                   TerminalWindow *window)
{
  const char *title;

  if (screen != window->active_screen)
    return;

  title = terminal_screen_get_title (screen);
  gtk_window_set_title (GTK_WINDOW (window),
                        title && title[0] ? title : "Pano Terminal");
}

static void
screen_font_any_changed_cb (TerminalScreen *screen,
                            GParamSpec *psepc,
                            TerminalWindow *window)
{
  if (!gtk_widget_get_realized (GTK_WIDGET (window)))
    return;

  if (screen != window->active_screen)
    return;

  terminal_window_update_size (window);
}

static void
screen_hyperlink_hover_uri_changed (TerminalScreen *screen,
                                    const char *uri,
                                    const GdkRectangle *bbox G_GNUC_UNUSED,
                                    TerminalWindow *window G_GNUC_UNUSED)
{
  gs_free char *label = nullptr;

  if (!gtk_widget_get_realized (GTK_WIDGET (screen)))
    return;

  label = terminal_util_hyperlink_uri_label (uri);

  gtk_widget_set_tooltip_text (GTK_WIDGET (screen), label);
}

/* Tab view callbacks */

static gboolean
screen_close_request_cb (TerminalNotebook *notebook,
                         TerminalScreen   *screen,
                         TerminalWindow   *window)
{
  // A tab whose panes went elsewhere, see terminal_tab_merge()
  if (!screen)
    return GDK_EVENT_PROPAGATE;

  if (!window->closing_confirmed &&
      confirm_close_window_or_tab (window, screen, CloseKind::TAB))
    return GDK_EVENT_STOP;

  if (auto const tab = terminal_tab_get_from_screen (screen))
    window_remember_closed_tab (window, tab);
  return GDK_EVENT_PROPAGATE;
}

int
terminal_window_get_active_screen_num (TerminalWindow *window)
{
  return terminal_notebook_get_active_screen_num (window->notebook);
}

void
terminal_window_add_tab(TerminalWindow* window,
                        TerminalTab* tab,
                        TerminalTab* parent_tab)
{
  auto const global_settings = terminal_app_get_global_settings(terminal_app_get());
  auto const position_pref = TerminalNewTabPosition
    (g_settings_get_enum(global_settings, TERMINAL_SETTING_NEW_TAB_POSITION_KEY));

  if (position_pref == TERMINAL_NEW_TAB_POSITION_NEXT &&
      parent_tab) {
    terminal_notebook_insert_tab(window->notebook, tab, parent_tab, false);
  } else {
    terminal_notebook_append_tab(window->notebook, tab, false);
  }

  gtk_widget_grab_focus(GTK_WIDGET(terminal_tab_get_screen(tab)));
}

void
terminal_window_insert_tab(TerminalWindow* window,
                           TerminalTab* tab,
                           int position)
{
  g_return_if_fail(TERMINAL_IS_WINDOW(window));

  terminal_notebook_insert_tab_at(window->notebook, tab, position);
  gtk_widget_grab_focus(GTK_WIDGET(terminal_tab_get_screen(tab)));
}

void
terminal_window_remove_screen (TerminalWindow *window,
                               TerminalScreen *screen)
{
  terminal_notebook_remove_screen (window->notebook, screen);
  terminal_window_update_pane_actions(window);
}
GList*
terminal_window_list_tabs (TerminalWindow *window)
{
  return terminal_notebook_list_tabs (window->notebook);
}

static inline bool
should_update_window_size (TerminalWindow *window)
{
  /* Don't adjust the size of maximized or tiled (snapped, half-maximized)
   * windows: if we do, there will be ugly gaps of up to 1 character cell
   * around otherwise tiled windows, and unnecessary SIGWINCHes when
   * switching tabs. */
  if (window->realized && window_state_is_snapped(window->window_state))
    return false;

  /* The quick terminal has its size from the screen */
  if (window->quick)
    return false;

  /* A split tab shares the window between its panes, so the window size
   * does not follow the grid of one terminal. */
  auto const tab = window_get_active_tab(window);
  return !(tab && terminal_tab_is_split(tab));
}

void
terminal_window_update_size (TerminalWindow *window)
{
  int grid_width, grid_height;
  int pixel_width, pixel_height;

  if (!should_update_window_size(window))
    return;

  if (!window->active_screen)
    return;

  /* be sure our geometry is up-to-date */
  terminal_window_update_geometry (window);

  terminal_screen_get_size (window->active_screen, &grid_width, &grid_height);
  _terminal_debug_print (TERMINAL_DEBUG_GEOMETRY,
                         "[window %p] size is %dx%d cells of %dx%d px\n",
                         window, grid_width, grid_height,
                         window->old_char_width, window->old_char_height);

  /* the "old" struct members were updated by update_geometry */
  pixel_width = window->old_chrome_width + grid_width * window->old_char_width;
  pixel_height = window->old_chrome_height + grid_height * window->old_char_height;
  _terminal_debug_print (TERMINAL_DEBUG_GEOMETRY,
                         "[window %p] %dx%d + %dx%d = %dx%d\n",
                         window, grid_width * window->old_char_width,
                         grid_height * window->old_char_height,
                         window->old_chrome_width, window->old_chrome_height,
                         pixel_width, pixel_height);

  gtk_window_set_default_size (GTK_WINDOW (window), pixel_width, pixel_height);
}

void
terminal_window_switch_screen (TerminalWindow *window,
                               TerminalScreen *screen)
{
  terminal_notebook_set_active_screen (window->notebook, screen);
}

TerminalScreen*
terminal_window_get_active (TerminalWindow *window)
{
  return terminal_notebook_get_active_screen (window->notebook);
}

TerminalTab*
terminal_window_get_active_tab(TerminalWindow *window)
{
  return terminal_notebook_get_active_tab(window->notebook);
}

static void
notebook_screen_switched_cb (TerminalNotebook *notebook,
                             TerminalScreen   *old_active_screen,
                             TerminalScreen   *screen,
                             TerminalWindow   *window)
{
  int old_grid_width, old_grid_height;

  _terminal_debug_print (TERMINAL_DEBUG_MDI,
                         "[window %p] MDI: screen-switched old %p new %p\n",
                         window, old_active_screen, screen);

  if (window->disposed)
    return;

  if (old_active_screen == screen)
    return;

  window->active_screen = screen;

  _terminal_debug_print (TERMINAL_DEBUG_MDI,
                         "[window %p] MDI: setting active tab to screen %p (old active screen %p)\n",
                         window, screen, old_active_screen);

  /* Moving between panes of a tab, or to or from a split tab, keeps the
   * window size; the terminals adapt to their space instead. */
  auto const old_tab = terminal_tab_get_from_screen(old_active_screen);
  auto const new_tab = terminal_tab_get_from_screen(screen);
  auto const keep_window_size = (old_tab && old_tab == new_tab) ||
    (old_tab && terminal_tab_is_split(old_tab)) ||
    (new_tab && terminal_tab_is_split(new_tab));

  if (old_active_screen != nullptr && screen != nullptr && !keep_window_size &&
      should_update_window_size(window)) {
    terminal_screen_get_size (old_active_screen, &old_grid_width, &old_grid_height);

    /* This is so that we maintain the same grid */
    vte_terminal_set_size (VTE_TERMINAL (screen), old_grid_width, old_grid_height);
  }

  if (!screen) {
    gtk_revealer_set_reveal_child(window->find_bar_revealer, false);
    gtk_window_set_title(GTK_WINDOW(window), "Pano Terminal");
    terminal_window_update_search_sensitivity(window);
  }

  terminal_find_bar_set_screen(window->find_bar, screen);
  g_simple_action_set_enabled(lookup_action(window, "find"), screen != nullptr);

  if (!screen)
    return;

  sync_screen_title (screen, nullptr, window);

  // The user is looking at it now
  terminal_app_withdraw_command_notification (terminal_app_get (), screen);
  g_object_set_data (G_OBJECT (screen), ACTIVITY_NOTIFIED_KEY, nullptr);

  /* set size of window to current grid size */
  if (should_update_window_size(window))
    _terminal_debug_print (TERMINAL_DEBUG_GEOMETRY,
                           "[window %p] setting size after flipping notebook pages\n",
                           window);
  if (!keep_window_size)
    terminal_window_update_size (window);

  terminal_window_update_tabs_actions_sensitivity (window);
  terminal_window_update_terminal_menu (window);
  terminal_window_update_set_profile_menu_active_profile (window);
  terminal_window_update_copy_sensitivity (screen, window);
  terminal_window_update_zoom_sensitivity (window);
  terminal_window_update_search_sensitivity(window);
  terminal_window_update_paste_sensitivity (window);
  terminal_window_update_pane_actions(window);
  terminal_window_update_prompt_actions(window);
}

static void
notebook_screen_added_cb (TerminalNotebook *notebook,
                          TerminalScreen   *screen,
                          TerminalWindow   *window)
{
  int pages;

  g_assert (TERMINAL_IS_NOTEBOOK (notebook));
  g_assert (TERMINAL_IS_SCREEN (screen));
  g_assert (TERMINAL_IS_WINDOW (window));

  _terminal_debug_print (TERMINAL_DEBUG_MDI,
                         "[window %p] MDI: screen %p inserted\n",
                         window, screen);

  g_signal_connect (G_OBJECT (screen),
                    "profile-set",
                    G_CALLBACK (profile_set_cb),
                    window);

  /* FIXME: only connect on the active screen, not all screens! */
  g_signal_connect (screen, "notify::title",
                    G_CALLBACK (sync_screen_title), window);
  g_signal_connect (screen, "notify::font-desc",
                    G_CALLBACK (screen_font_any_changed_cb), window);
  g_signal_connect (screen, "notify::font-scale",
                    G_CALLBACK (screen_font_any_changed_cb), window);
  g_signal_connect (screen, "notify::cell-height-scale",
                    G_CALLBACK (screen_font_any_changed_cb), window);
  g_signal_connect (screen, "notify::cell-width-scale",
                    G_CALLBACK (screen_font_any_changed_cb), window);
  g_signal_connect (screen, "selection-changed",
                    G_CALLBACK (terminal_window_update_copy_sensitivity), window);
  g_signal_connect (screen, "hyperlink-hover-uri-changed",
                    G_CALLBACK (screen_hyperlink_hover_uri_changed), window);

  g_signal_connect (screen, "show-popup-menu",
                    G_CALLBACK (screen_show_popup_menu_cb), window);
  g_signal_connect (screen, "match-clicked",
                    G_CALLBACK (screen_match_clicked_cb), window);
  g_signal_connect (screen, "resize-window",
                    G_CALLBACK (screen_resize_window_cb), window);

  g_signal_connect (screen, "close-screen",
                    G_CALLBACK (screen_close_cb), window);
  g_signal_connect (screen, "command-finished",
                    G_CALLBACK (screen_command_finished_cb), window);
  g_signal_connect (screen, "bell",
                    G_CALLBACK (screen_bell_cb), window);
  g_signal_connect (screen, "quick-select-chosen",
                    G_CALLBACK (screen_quick_select_chosen_cb), window);
  g_signal_connect (screen, "colors-changed",
                    G_CALLBACK (screen_colors_changed_cb), window);
  g_signal_connect (screen, "activity",
                    G_CALLBACK (screen_activity_cb), window);
  g_signal_connect (screen, "silence",
                    G_CALLBACK (screen_silence_cb), window);
  g_signal_connect (screen, "prompts-changed",
                    G_CALLBACK (screen_prompts_changed_cb), window);
  g_signal_connect (screen, "commit",
                    G_CALLBACK (screen_commit_cb), window);
  g_signal_connect (screen, "notify::process-kind",
                    G_CALLBACK (screen_process_kind_changed_cb), window);

  terminal_window_update_tabs_actions_sensitivity (window);
  terminal_window_update_search_sensitivity(window);
  terminal_window_update_paste_sensitivity (window);
  terminal_window_update_pane_actions(window);

#if 0
  /* FIXMEchpe: wtf is this doing? */

  /* If we have an active screen, match its size and zoom */
  if (window->active_screen)
    {
      int current_width, current_height;
      double scale;

      terminal_screen_get_size (window->active_screen, &current_width, &current_height);
      vte_terminal_set_size (VTE_TERMINAL (screen), current_width, current_height);

      scale = terminal_screen_get_font_scale (window->active_screen);
      terminal_screen_set_font_scale (screen, scale);
    }
#endif

  if (window->present_on_insert)
    {
      gtk_window_present (GTK_WINDOW (window));
      window->present_on_insert = FALSE;
    }

  pages = terminal_notebook_get_n_screens (notebook);
  if (pages == 2)
    {
      terminal_window_update_size (window);
    }
}

static void
notebook_screen_removed_cb (TerminalNotebook *notebook,
                            TerminalScreen   *screen,
                            TerminalWindow   *window)
{
  int pages;

  g_assert (TERMINAL_IS_NOTEBOOK (notebook));
  g_assert (TERMINAL_IS_SCREEN (screen));
  g_assert (TERMINAL_IS_WINDOW (window));

  if (window->disposed)
    return;

  if (window->notebook_context_tab &&
      terminal_tab_get_from_screen(screen) == window->notebook_context_tab)
    window->notebook_context_tab = nullptr;

  _terminal_debug_print (TERMINAL_DEBUG_MDI,
                         "[window %p] MDI: screen %p removed\n",
                         window, screen);

  g_signal_handlers_disconnect_by_func (G_OBJECT (screen),
                                        (void*)profile_set_cb,
                                        window);

  g_signal_handlers_disconnect_by_func (G_OBJECT (screen),
                                        (void*)sync_screen_title,
                                        window);

  g_signal_handlers_disconnect_by_func (G_OBJECT (screen),
                                        (void*)screen_font_any_changed_cb,
                                        window);

  g_signal_handlers_disconnect_by_func (G_OBJECT (screen),
                                        (void*)terminal_window_update_copy_sensitivity,
                                        window);

  g_signal_handlers_disconnect_by_func (G_OBJECT (screen),
                                        (void*)screen_hyperlink_hover_uri_changed,
                                        window);

  g_signal_handlers_disconnect_by_func (screen,
                                        (void*)screen_show_popup_menu_cb,
                                        window);

  g_signal_handlers_disconnect_by_func (screen,
                                        (void*)screen_match_clicked_cb,
                                        window);
  g_signal_handlers_disconnect_by_func (screen,
                                        (void*)screen_resize_window_cb,
                                        window);

  g_signal_handlers_disconnect_by_func (screen,
                                        (void*)screen_close_cb,
                                        window);
  g_signal_handlers_disconnect_by_func (screen,
                                        (void*)screen_command_finished_cb,
                                        window);
  g_signal_handlers_disconnect_by_func (screen,
                                        (void*)screen_bell_cb,
                                        window);
  g_signal_handlers_disconnect_by_func (screen,
                                        (void*)screen_quick_select_chosen_cb,
                                        window);
  g_signal_handlers_disconnect_by_func (screen,
                                        (void*)screen_colors_changed_cb,
                                        window);
  g_signal_handlers_disconnect_by_func (screen,
                                        (void*)screen_activity_cb,
                                        window);
  g_signal_handlers_disconnect_by_func (screen,
                                        (void*)screen_silence_cb,
                                        window);
  g_signal_handlers_disconnect_by_func (screen,
                                        (void*)screen_prompts_changed_cb,
                                        window);
  g_signal_handlers_disconnect_by_func (screen,
                                        (void*)screen_commit_cb,
                                        window);
  g_signal_handlers_disconnect_by_func (screen,
                                        (void*)screen_process_kind_changed_cb,
                                        window);
  terminal_tab_set_screen_css_class (screen, "broadcast", false);
  terminal_app_withdraw_command_notification (terminal_app_get (), screen);

  /* We already got a switch-page signal whose handler sets the active tab to the
   * new active tab, unless this screen was the only one in the notebook, so
   * window->active_tab is valid here.
   */

  pages = terminal_notebook_get_n_screens (notebook);
  if (pages == 0)
    {
      window->active_screen = nullptr;

      /* That was the last tab in the window; close it. */
      gtk_window_destroy (GTK_WINDOW (window));
      return;
    }

  terminal_window_update_tabs_actions_sensitivity (window);
  terminal_window_update_search_sensitivity(window);

  if (pages == 1)
    {
      TerminalScreen *active_screen = terminal_notebook_get_active_screen (notebook);

      if (active_screen != nullptr)
        gtk_widget_grab_focus (GTK_WIDGET (active_screen));  /* bug 742422 */

      terminal_window_update_size (window);
    }
}

static void
notebook_screens_reordered_cb (TerminalNotebook *notebook,
                               TerminalWindow   *window)
{
  g_assert (TERMINAL_IS_WINDOW (window));
  g_assert (TERMINAL_IS_NOTEBOOK (notebook));

  terminal_window_update_tabs_actions_sensitivity (window);
}

static inline GSimpleAction *
lookup_notebook_action(TerminalWindow* window,
                       char const* name)
{
  GAction *action;

  action = g_action_map_lookup_action(G_ACTION_MAP(window->notebook_context_action_group), name);
  g_return_val_if_fail(action != nullptr, nullptr);

  return G_SIMPLE_ACTION(action);
}


static void
notebook_setup_menu_cb(TerminalNotebook* notebook,
                       TerminalTab* tab,
                       TerminalWindow* window)
{
  window->notebook_context_tab = tab; // unowned!
  if (!tab)
    return;

  bool can_switch_left, can_switch_right;
  bool can_reorder_left, can_reorder_right, can_reorder_start, can_reorder_end;
  bool can_close, can_detach;
  terminal_notebook_get_tab_actions(window->notebook, tab,
                                    &can_switch_left,
                                    &can_switch_right,
                                    &can_reorder_left,
                                    &can_reorder_right,
                                    &can_reorder_start,
                                    &can_reorder_end,
                                    &can_close,
                                    &can_detach);
  auto const pinned = terminal_tab_get_pinned(tab);

  auto action = lookup_notebook_action(window, "notebook-tab-move-left");
  g_simple_action_set_enabled(G_SIMPLE_ACTION(action), can_reorder_left);

  action = lookup_notebook_action(window, "notebook-tab-move-right");
  g_simple_action_set_enabled(G_SIMPLE_ACTION(action), can_reorder_right);

  action = lookup_notebook_action(window, "notebook-tab-detach");
  g_simple_action_set_enabled(G_SIMPLE_ACTION(action), can_detach);

  action = lookup_notebook_action(window, "notebook-tab-close");
  g_simple_action_set_enabled(G_SIMPLE_ACTION(action), can_close);

  auto const others = window_list_other_tabs(window, tab);
  action = lookup_notebook_action(window, "notebook-tab-close-others");
  g_simple_action_set_enabled(G_SIMPLE_ACTION(action), others != nullptr);
  g_list_free(others);

  action = lookup_notebook_action(window, "notebook-tab-pin");
  g_simple_action_set_enabled(G_SIMPLE_ACTION(action), !pinned);

  action = lookup_notebook_action(window, "notebook-tab-unpin");
  g_simple_action_set_enabled(G_SIMPLE_ACTION(action), pinned);
}

gboolean
terminal_window_parse_geometry (TerminalWindow *window,
                                const char     *geometry)
{
  int grid_width, grid_height;

  g_assert (TERMINAL_IS_WINDOW (window));

  terminal_window_update_geometry (window);

  /* An X geometry: [=][COLUMNSxROWS][{+-}X{+-}Y]. GTK 4 cannot place
   * windows, so a position is accepted but has no effect.
   */
  if (geometry == nullptr)
    return FALSE;

  auto p = geometry;
  if (*p == '=')
    p++;

  auto have_size = false;
  if (g_ascii_isdigit (*p)) {
    char* end;
    auto const columns = g_ascii_strtoll (p, &end, 10);
    if ((*end != 'x' && *end != 'X') || !g_ascii_isdigit (end[1]))
      return FALSE;
    auto const rows = g_ascii_strtoll (end + 1, &end, 10);
    if (columns < 1 || columns > 10000 || rows < 1 || rows > 10000)
      return FALSE;
    grid_width = int(columns);
    grid_height = int(rows);
    have_size = true;
    p = end;
  }

  if (*p == '+' || *p == '-') {
    auto const position = g_regex_match_simple ("^[+-]-?[0-9]+[+-]-?[0-9]+$", p,
                                                GRegexCompileFlags(0),
                                                GRegexMatchFlags(0));
    if (!position)
      return FALSE;
  } else if (*p != '\0' || !have_size) {
    return FALSE;
  }

  if (!have_size)
    return TRUE;

  /* Set the size of the active screen and then reset our default-size
   * so that GtkWindow will run through layout and reallocate to the
   * newly requested size.
   */
  if (window->active_screen) {
    vte_terminal_set_size (VTE_TERMINAL (window->active_screen),
                           grid_width, grid_height);
    gtk_window_set_default_size (GTK_WINDOW (window), -1, -1);
  }

  return TRUE;
}

void
terminal_window_update_geometry (TerminalWindow *window)
{
  GtkWidget *widget;
  GtkBorder padding;
  GtkRequisition contents_request, widget_request;
  int grid_width, grid_height;
  int char_width, char_height;
  int chrome_width, chrome_height;
  int csd_width = 0, csd_height = 0;

  if (gtk_widget_in_destruction (GTK_WIDGET (window)))
    return;

  if (window->active_screen == nullptr)
    return;

  widget = GTK_WIDGET (window->active_screen);

  /* We set geometry hints from the active term; best thing
   * I can think of to do. Other option would be to try to
   * get some kind of union of all hints from all terms in the
   * window, but that doesn't make too much sense.
   */
  terminal_screen_get_cell_size (window->active_screen, &char_width, &char_height);

  terminal_screen_get_size (window->active_screen, &grid_width, &grid_height);
  _terminal_debug_print (TERMINAL_DEBUG_GEOMETRY, "%dx%d cells of %dx%d px = %dx%d px\n",
                         grid_width, grid_height, char_width, char_height,
                         char_width * grid_width, char_height * grid_height);

  gtk_style_context_get_padding(gtk_widget_get_style_context(widget),
                                &padding);

  _terminal_debug_print (TERMINAL_DEBUG_GEOMETRY, "padding = %dx%d px\n",
                         padding.left + padding.right,
                         padding.top + padding.bottom);

  gtk_widget_get_preferred_size (GTK_WIDGET (window->toolbar_view), nullptr, &contents_request);
  _terminal_debug_print (TERMINAL_DEBUG_GEOMETRY, "content area requests %dx%d px\n",
                         contents_request.width, contents_request.height);

  chrome_width = contents_request.width - (char_width * grid_width);
  chrome_height = contents_request.height - (char_height * grid_height);
  _terminal_debug_print (TERMINAL_DEBUG_GEOMETRY, "chrome: %dx%d px\n",
                         chrome_width, chrome_height);

  if (window->realized)
    {
      /* Only when having been realize the CSD can be calculated. Do this by
       * using the actual allocation rather then the preferred size as the
       * the preferred size takes the natural size of e.g. the title bar into
       * account which can be far wider then the contents size when using a
       * very long title */
      GtkAllocation toplevel_allocation, toolbar_view_allocation;

      gtk_widget_get_allocation (GTK_WIDGET (window->toolbar_view), &toolbar_view_allocation);
      _terminal_debug_print (TERMINAL_DEBUG_GEOMETRY,
                             "terminal widget allocation %dx%d px\n",
                             toolbar_view_allocation.width, toolbar_view_allocation.height);

      gtk_widget_get_allocation (GTK_WIDGET (window), &toplevel_allocation);
      _terminal_debug_print (TERMINAL_DEBUG_GEOMETRY, "window allocation %dx%d px\n",
                             toplevel_allocation.width, toplevel_allocation.height);

      csd_width =  toplevel_allocation.width - toolbar_view_allocation.width;
      csd_height = toplevel_allocation.height - toolbar_view_allocation.height;
      _terminal_debug_print (TERMINAL_DEBUG_GEOMETRY, "CSDs: %dx%d px\n",
                             csd_width, csd_height);
    }

  gtk_widget_get_preferred_size (widget, nullptr, &widget_request);
  _terminal_debug_print (TERMINAL_DEBUG_GEOMETRY, "terminal widget requests %dx%d px\n",
                         widget_request.width, widget_request.height);

  if (!window->realized)
    {
      /* Don't actually set the geometry hints until we have been realized,
       * because we don't know how large the client-side decorations are going
       * to be. We also avoid setting window->old_csd_width or
       * window->old_csd_height, so that next time through this function we'll
       * definitely recalculate the hints.
       *
       * Similarly, the size request doesn't seem to include the padding
       * until we've been redrawn at least once. Don't resize the window
       * until we've done that. */
      _terminal_debug_print (TERMINAL_DEBUG_GEOMETRY, "not realized yet\n");
    }
  else if (window_state_is_snapped(window->window_state))
    {
      _terminal_debug_print (TERMINAL_DEBUG_GEOMETRY,
                             "Not applying geometry in snapped state\n");
    }
  else if (char_width != window->old_char_width ||
      char_height != window->old_char_height ||
      padding.left + padding.right != window->old_padding_width ||
      padding.top + padding.bottom != window->old_padding_height ||
      chrome_width != window->old_chrome_width ||
      chrome_height != window->old_chrome_height ||
      csd_width != window->old_csd_width ||
      csd_height != window->old_csd_height ||
      widget != (GtkWidget*) window->old_geometry_widget)
    {
      window->old_csd_width = csd_width;
      window->old_csd_height = csd_height;
      window->old_geometry_widget = widget;
    }
  else
    {
      _terminal_debug_print (TERMINAL_DEBUG_GEOMETRY,
                             "[window %p] hints: increment unchanged, not setting\n",
                             window);
    }

  /* We need these for the size calculation in terminal_window_update_size()
   * (at least under GTK >= 3.19), so we set them unconditionally. */
  window->old_char_width = char_width;
  window->old_char_height = char_height;
  window->old_chrome_width = chrome_width;
  window->old_chrome_height = chrome_height;
  window->old_padding_width = padding.left + padding.right;
  window->old_padding_height = padding.top + padding.bottom;
}

static gboolean
destroy_window_in_main (gpointer user_data)
{
  TerminalWindow *window = TERMINAL_WINDOW (user_data);
  gtk_window_destroy (GTK_WINDOW (window));
  return G_SOURCE_REMOVE;
}

static void
confirm_close_response_cb (GtkWidget      *dialog,
                           int             response,
                           TerminalWindow *window)
{
  TerminalScreen *screen;

  g_assert (GTK_IS_WINDOW (dialog));
  g_assert (TERMINAL_IS_WINDOW (window));

  screen = (TerminalScreen*)g_object_get_data (G_OBJECT (dialog), "close-screen");
  auto const kind = CloseKind(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(dialog), "close-kind")));
  auto const accepted = response == GTK_RESPONSE_ACCEPT;
  // Destroying the dialog drops its data
  gs_unref_object auto keep_tab = reinterpret_cast<TerminalTab*>(g_object_get_data(G_OBJECT(dialog), "keep-tab"));
  if (keep_tab)
    g_object_ref(keep_tab);

  gtk_window_destroy (GTK_WINDOW (dialog));

  if (kind == CloseKind::PANE) {
    if (accepted)
      terminal_window_remove_screen(window, screen);
  } else if (kind == CloseKind::TAB) {
    if (accepted)
      if (auto const tab = terminal_tab_get_from_screen (screen))
        window_remember_closed_tab (window, tab);
    terminal_notebook_confirm_close (window->notebook, screen, accepted);
  } else if (kind == CloseKind::OTHER_TABS) {
    if (accepted)
      window_close_other_tabs_confirmed (window, keep_tab);
  } else if (accepted)
    g_idle_add_full (G_PRIORITY_DEFAULT,
                     destroy_window_in_main,
                     g_object_ref (window),
                     g_object_unref);
}

/* Returns: TRUE if closing needs to wait until user confirmation;
 * FALSE if the terminal or window can close immediately.
 */
static gboolean
confirm_close_window_or_tab (TerminalWindow *window,
                             TerminalScreen *screen,
                             CloseKind kind)
{
  GtkWidget *dialog;
  gboolean do_confirm;
  int n_tabs;

  if (window->confirm_close_dialog)
    {
      /* WTF, already have one? It's modal, so how did that happen? */
      gtk_dialog_response (GTK_DIALOG (window->confirm_close_dialog),
                           GTK_RESPONSE_DELETE_EVENT);
    }

  do_confirm = g_settings_get_boolean (terminal_app_get_global_settings (terminal_app_get ()),
                                       TERMINAL_SETTING_CONFIRM_CLOSE_KEY);
  if (!do_confirm)
    return FALSE;

  if (kind == CloseKind::PANE)
    {
      do_confirm = terminal_screen_has_running_jobs (screen);
      n_tabs = 1;
    }
  else
    {
      GList* screens = nullptr;
      if (kind == CloseKind::TAB) {
        screens = terminal_tab_list_screens(terminal_tab_get_from_screen(screen));
        // A tab with several panes is closed like a single terminal
        n_tabs = 1;
      } else if (kind == CloseKind::OTHER_TABS) {
        auto const others = window_list_other_tabs(window, terminal_tab_get_from_screen(screen));
        for (auto l = others; l != nullptr; l = l->next)
          screens = g_list_concat(screens, terminal_tab_list_screens(TERMINAL_TAB(l->data)));
        n_tabs = int(g_list_length(others));
        g_list_free(others);
      } else {
        screens = terminal_notebook_list_screens(window->notebook);
        n_tabs = int(g_list_length(screens));
      }

      do_confirm = FALSE;
      for (auto l = screens; l != nullptr; l = l->next)
        {
          if (terminal_screen_has_running_jobs (TERMINAL_SCREEN (l->data)))
            {
              do_confirm = TRUE;
              break;
            }
        }
      g_list_free (screens);
    }

  if (!do_confirm)
    return FALSE;

  char const* title;
  char const* text;
  char const* button;
  if (kind == CloseKind::OTHER_TABS) {
    title = n_tabs > 1 ? _("Close the other terminals?") : _("Close the other terminal?");
    text = n_tabs > 1
      ? _("There are still processes running in some of the other terminals. "
          "Closing them will kill all of them.")
      : _("There is still a process running in the other terminal. "
          "Closing the terminal will kill it.");
    button = n_tabs > 1 ? _("C_lose Terminals") : _("C_lose Terminal");
  } else if (n_tabs > 1) {
    title = _("Close this window?");
    text = _("There are still processes running in some terminals in this window. "
             "Closing the window will kill all of them.");
    button = _("C_lose Window");
  } else {
    title = _("Close this terminal?");
    text = _("There is still a process running in this terminal. "
             "Closing the terminal will kill it.");
    button = _("C_lose Terminal");
  }

  dialog = window->confirm_close_dialog =
    gtk_message_dialog_new (GTK_WINDOW (window),
                            GtkDialogFlags(GTK_DIALOG_MODAL |
                                           GTK_DIALOG_DESTROY_WITH_PARENT),
                            GTK_MESSAGE_WARNING,
                            GTK_BUTTONS_CANCEL,
                            "%s", title);
  gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog), "%s", text);

  gtk_window_set_title (GTK_WINDOW (dialog), ""); 

  GtkWidget *remove_button = gtk_dialog_add_button (GTK_DIALOG (dialog), button, GTK_RESPONSE_ACCEPT);
  gtk_style_context_add_class (gtk_widget_get_style_context (remove_button), "destructive-action");
  gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);

  g_object_set_data (G_OBJECT (dialog), "close-screen", screen);
  g_object_set_data (G_OBJECT (dialog), "close-kind", GINT_TO_POINTER (int(kind)));
  if (kind == CloseKind::OTHER_TABS)
    g_object_set_data_full (G_OBJECT (dialog), "keep-tab",
                            g_object_ref (terminal_tab_get_from_screen (screen)),
                            g_object_unref);

  g_signal_connect_swapped (dialog, "destroy",
                            G_CALLBACK (g_nullify_pointer),
                            &window->confirm_close_dialog);
  g_signal_connect (dialog,
                    "response",
                    G_CALLBACK (confirm_close_response_cb),
                    window);

  gtk_window_present (GTK_WINDOW (dialog));

  return TRUE;
}

void
terminal_window_request_close (TerminalWindow *window)
{
  g_return_if_fail (TERMINAL_IS_WINDOW (window));

  if (confirm_close_window_or_tab (window, nullptr, CloseKind::WINDOW))
    return;

  gtk_window_destroy (GTK_WINDOW (window));
}

const char *
terminal_window_get_uuid (TerminalWindow *window)
{
  g_return_val_if_fail (TERMINAL_IS_WINDOW (window), nullptr);

  return window->uuid;
}

gboolean
terminal_window_in_fullscreen_transition (TerminalWindow *window)
{
  g_return_val_if_fail (TERMINAL_IS_WINDOW (window), FALSE);

  return window->fullscreen_transition != 0;
}

static GdkMonitor*
quick_terminal_monitor(TerminalWindow* window)
{
  auto const display = gtk_widget_get_display(GTK_WIDGET(window));
  if (auto const surface = gtk_native_get_surface(GTK_NATIVE(window)))
    if (auto const monitor = gdk_display_get_monitor_at_surface(display, surface))
      return monitor;

  auto const monitors = gdk_display_get_monitors(display);
  gs_unref_object auto monitor = GDK_MONITOR(g_list_model_get_item(monitors, 0));
  return monitor; // the display keeps a reference
}

#ifdef GDK_WINDOWING_X11

#define QUICK_SLIDE_DURATION_US (180 * G_TIME_SPAN_MILLISECOND)

/* Asks the window manager to keep the quick terminal above other windows */
static void
quick_terminal_keep_above(GdkSurface* surface)
{
  auto const display = GDK_SURFACE_XDISPLAY(surface);
  XEvent event{};
  event.xclient.type = ClientMessage;
  event.xclient.window = GDK_SURFACE_XID(surface);
  event.xclient.message_type = XInternAtom(display, "_NET_WM_STATE", false);
  event.xclient.format = 32;
  event.xclient.data.l[0] = 1; // _NET_WM_STATE_ADD
  event.xclient.data.l[1] = long(XInternAtom(display, "_NET_WM_STATE_ABOVE", false));
  event.xclient.data.l[3] = 1; // from an application
  XSendEvent(display, DefaultRootWindow(display), false,
             SubstructureRedirectMask | SubstructureNotifyMask, &event);
}

/* Moves the quick terminal down from above the top edge, easing out */
static gboolean
quick_terminal_slide_cb(GtkWidget* widget,
                        GdkFrameClock* clock,
                        void* user_data)
{
  auto const surface = gtk_native_get_surface(GTK_NATIVE(widget));
  auto const start = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(widget), "quick-slide-start"));
  auto const x = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "quick-slide-x"));
  auto const top = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "quick-slide-top"));
  auto const height = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "quick-slide-height"));
  if (!surface || !GDK_IS_X11_SURFACE(surface))
    return G_SOURCE_REMOVE;

  auto const t = std::clamp(double(gdk_frame_clock_get_frame_time(clock) - gint64(start)) /
                            double(QUICK_SLIDE_DURATION_US), 0.0, 1.0);
  auto const eased = 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);
  auto const y = top - int((1.0 - eased) * height);
  XMoveWindow(GDK_SURFACE_XDISPLAY(surface), GDK_SURFACE_XID(surface), x, y);

  return t < 1.0 ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

#endif /* GDK_WINDOWING_X11 */

/* On X11 the quick terminal goes to the top of the screen, above other
 * windows, maybe sliding in. Wayland does not let applications place their
 * windows. */
static void
quick_terminal_map_cb(TerminalWindow* window)
{
#ifdef GDK_WINDOWING_WAYLAND
  /* On Wayland, the desktop places windows. An application ID of its own
   * lets the GNOME Shell extension in data/gnome-shell-extension find the
   * quick terminal and place it; com.aripd.Pano.QuickTerminal.desktop
   * gives it the icon. */
  auto const wayland_surface = gtk_native_get_surface(GTK_NATIVE(window));
  if (GDK_IS_WAYLAND_TOPLEVEL(wayland_surface)) {
    gdk_wayland_toplevel_set_application_id(GDK_TOPLEVEL(wayland_surface),
                                            TERMINAL_APPLICATION_ID ".QuickTerminal");
    return;
  }
#endif

#ifdef GDK_WINDOWING_X11
  auto const surface = gtk_native_get_surface(GTK_NATIVE(window));
  auto const monitor = quick_terminal_monitor(window);
  if (!GDK_IS_X11_SURFACE(surface) || !monitor)
    return;

  GdkRectangle geometry;
  gdk_monitor_get_geometry(monitor, &geometry);
  // The window is not allocated yet when it is mapped; use its default size
  int width = 0, height = 0;
  gtk_window_get_default_size(GTK_WINDOW(window), &width, &height);
  auto const scale = gdk_surface_get_scale_factor(surface);
  auto const x = MAX(geometry.x + (geometry.width - width) / 2, geometry.x) * scale;
  auto const top = geometry.y * scale;

  quick_terminal_keep_above(surface);

  auto const settings = terminal_app_get_global_settings(terminal_app_get());
  if (!g_settings_get_boolean(settings, TERMINAL_SETTING_QUICK_TERMINAL_SLIDE_KEY) || height <= 0) {
    XMoveWindow(GDK_SURFACE_XDISPLAY(surface), GDK_SURFACE_XID(surface), x, top);
    return;
  }

  auto const object = G_OBJECT(window);
  g_object_set_data(object, "quick-slide-start", GSIZE_TO_POINTER(gsize(g_get_monotonic_time())));
  g_object_set_data(object, "quick-slide-x", GINT_TO_POINTER(x));
  g_object_set_data(object, "quick-slide-top", GINT_TO_POINTER(top));
  g_object_set_data(object, "quick-slide-height", GINT_TO_POINTER(height * scale));
  XMoveWindow(GDK_SURFACE_XDISPLAY(surface), GDK_SURFACE_XID(surface), x, top - height * scale);
  gtk_widget_add_tick_callback(GTK_WIDGET(window), quick_terminal_slide_cb, nullptr, nullptr);
#endif
}

/* Hides the quick terminal once the focus has gone to another application;
 * not for its own dialogs, or the preferences */
static gboolean
quick_terminal_unfocus_timeout_cb(void* user_data)
{
  auto const window = TERMINAL_WINDOW(user_data);

  auto const windows = gtk_application_get_windows(GTK_APPLICATION(terminal_app_get()));
  for (auto l = windows; l; l = l->next)
    if (gtk_window_is_active(GTK_WINDOW(l->data)))
      return G_SOURCE_REMOVE;

  gtk_widget_set_visible(GTK_WIDGET(window), false);
  return G_SOURCE_REMOVE;
}

static void
quick_terminal_is_active_cb(TerminalWindow* window,
                            GParamSpec* pspec,
                            void* user_data)
{
  auto const settings = terminal_app_get_global_settings(terminal_app_get());
  if (gtk_window_is_active(GTK_WINDOW(window)) ||
      !gtk_widget_get_visible(GTK_WIDGET(window)) ||
      !g_settings_get_boolean(settings, TERMINAL_SETTING_QUICK_TERMINAL_HIDE_ON_UNFOCUS_KEY))
    return;

  // Let the focus settle first
  g_timeout_add_full(G_PRIORITY_DEFAULT, 200, quick_terminal_unfocus_timeout_cb,
                     g_object_ref(window), g_object_unref);
}

/**
 * terminal_window_update_quick_size:
 * @window: the quick terminal
 *
 * Sizes the quick terminal from the settings, as a share of the screen.
 */
void
terminal_window_update_quick_size(TerminalWindow* window)
{
  g_return_if_fail(TERMINAL_IS_WINDOW(window));

  auto const monitor = quick_terminal_monitor(window);
  if (!monitor)
    return;

  auto const settings = terminal_app_get_global_settings(terminal_app_get());
  auto const width = g_settings_get_uint(settings, TERMINAL_SETTING_QUICK_TERMINAL_WIDTH_KEY);
  auto const height = g_settings_get_uint(settings, TERMINAL_SETTING_QUICK_TERMINAL_HEIGHT_KEY);

  GdkRectangle geometry;
  gdk_monitor_get_geometry(monitor, &geometry);
  gtk_window_set_default_size(GTK_WINDOW(window),
                              geometry.width * int(width) / 100,
                              geometry.height * int(height) / 100);
}

/**
 * terminal_window_set_quick:
 * @window: a #TerminalWindow
 * @quick: whether @window is the quick terminal
 *
 * The quick terminal is shown and hidden with pano-terminal --quick. Its
 * size is a share of the screen rather than the terminal grid, closing it
 * hides it, and it is not part of the saved session.
 */
void
terminal_window_set_quick(TerminalWindow* window,
                          bool quick)
{
  g_return_if_fail(TERMINAL_IS_WINDOW(window));

  window->quick = quick;
  if (quick) {
    gtk_widget_add_css_class(GTK_WIDGET(window), "quick-terminal");
    g_signal_connect(window, "map", G_CALLBACK(quick_terminal_map_cb), nullptr);
    g_signal_connect(window, "notify::is-active", G_CALLBACK(quick_terminal_is_active_cb), nullptr);
    terminal_window_update_quick_size(window);
  }
}

bool
terminal_window_is_quick(TerminalWindow* window)
{
  g_return_val_if_fail(TERMINAL_IS_WINDOW(window), false);

  return window->quick;
}

void
terminal_window_set_tab_pinned(TerminalWindow* window,
                               TerminalTab* tab,
                               bool pinned)
{
  g_return_if_fail(TERMINAL_IS_WINDOW(window));

  terminal_notebook_set_tab_pinned(window->notebook, tab, pinned);
}

bool
terminal_window_is_animating (TerminalWindow* window)
{
  g_return_val_if_fail(TERMINAL_IS_WINDOW(window), false);

  return window->tab_overview_animating;
}
