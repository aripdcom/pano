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

/* The quick terminal's key, as a system-wide shortcut from the desktop's
 * GlobalShortcuts portal (GNOME 48 or newer, KDE Plasma), while the
 * quick-terminal-global-shortcut setting is on.
 *
 * The desktop delivers the shortcut only while Pano Terminal's server
 * runs, so the server then stays running without windows, and starts when
 * the user logs in.
 *
 * Applications outside a sandbox must tell the portal their ID before
 * their first portal call on a connection. GTK already uses the portals on
 * the application's connection, so this uses a connection of its own.
 */

#include "config.h"

#include "terminal-global-shortcuts.hh"
#include "terminal-defines.hh"
#include "terminal-flatpak.hh"
#include "terminal-intl.hh"
#include "terminal-libgsystem.hh"
#include "terminal-schemas.hh"

#include <glib/gi18n.h>
#include <glib/gstdio.h>

#define PORTAL_BUS_NAME "org.freedesktop.portal.Desktop"
#define PORTAL_OBJECT_PATH "/org/freedesktop/portal/desktop"
#define PORTAL_GLOBAL_SHORTCUTS "org.freedesktop.portal.GlobalShortcuts"
#define PORTAL_REQUEST "org.freedesktop.portal.Request"
#define PORTAL_SESSION "org.freedesktop.portal.Session"
#define PORTAL_REGISTRY "org.freedesktop.host.portal.Registry"
#define PORTAL_BACKGROUND "org.freedesktop.portal.Background"

#define SHORTCUT_ID "quick-terminal"
#define PREFERRED_TRIGGER "F12"

#define AUTOSTART_FILE TERMINAL_APPLICATION_ID ".QuickTerminal.desktop"

static struct {
  GApplication* app; // unowned
  GSettings* settings;
  bool held;
  GCancellable* cancellable; // of the current connection
  GDBusConnection* connection;
  char* session_handle;
  guint activated_id;
  guint token_serial;
} state;

/* Requests: a portal method answers with the Response signal of a request
 * object, whose path follows from a token of ours */

using ResponseFunc = void (*)(guint response,
                              GVariant* results);

struct Request {
  GDBusConnection* connection;
  guint signal_id;
  ResponseFunc func;
  GCancellable* cancellable;
};

static void
request_free(Request* request)
{
  if (request->signal_id)
    g_dbus_connection_signal_unsubscribe(request->connection, request->signal_id);
  g_object_unref(request->connection);
  g_object_unref(request->cancellable);
  g_free(request);
}

static void
request_response_cb(GDBusConnection* connection,
                    char const* sender_name,
                    char const* object_path,
                    char const* interface_name,
                    char const* signal_name,
                    GVariant* parameters,
                    void* user_data)
{
  auto const request = reinterpret_cast<Request*>(user_data);
  guint response = 2;
  gs_unref_variant GVariant* results = nullptr;
  g_variant_get(parameters, "(u@a{sv})", &response, &results);

  auto const func = request->func;
  auto const cancelled = g_cancellable_is_cancelled(request->cancellable);
  request_free(request);
  if (!cancelled)
    func(response, results);
}

static void
request_call_cb(GObject* source,
                GAsyncResult* result,
                void* user_data)
{
  auto const request = reinterpret_cast<Request*>(user_data);
  gs_free_error GError* error = nullptr;
  gs_unref_variant GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source),
                                                                   result, &error);
  if (reply)
    return; // the answer comes with the Response signal

  if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    g_dbus_error_strip_remote_error(error);
    g_warning("The global shortcuts portal failed: %s", error->message);
  }
  auto const func = request->func;
  auto const cancelled = g_cancellable_is_cancelled(request->cancellable);
  request_free(request);
  if (!cancelled)
    func(2, nullptr);
}

static char*
new_token(void)
{
  return g_strdup_printf("pano%u", ++state.token_serial);
}

/* Calls @method of @interface with @parameters, whose options have the
 * handle token @token, and passes the response to @func */
static void
portal_request(GDBusConnection* connection,
               char const* interface,
               char const* method,
               GVariant* parameters,
               char const* token,
               ResponseFunc func,
               GCancellable* cancellable)
{
  gs_free char* sender = g_strdup(g_dbus_connection_get_unique_name(connection) + 1);
  g_strdelimit(sender, ".", '_');
  gs_free char* path = g_strdup_printf(PORTAL_OBJECT_PATH "/request/%s/%s", sender, token);

  auto const request = g_new0(Request, 1);
  request->connection = G_DBUS_CONNECTION(g_object_ref(connection));
  request->func = func;
  request->cancellable = G_CANCELLABLE(g_object_ref(cancellable));
  request->signal_id = g_dbus_connection_signal_subscribe(connection,
                                                          PORTAL_BUS_NAME,
                                                          PORTAL_REQUEST,
                                                          "Response",
                                                          path,
                                                          nullptr,
                                                          G_DBUS_SIGNAL_FLAGS_NONE,
                                                          request_response_cb,
                                                          request, nullptr);

  // The bus handles the match rule of the subscription before the call
  g_dbus_connection_call(connection,
                         PORTAL_BUS_NAME,
                         PORTAL_OBJECT_PATH,
                         interface,
                         method,
                         parameters,
                         G_VARIANT_TYPE("(o)"),
                         G_DBUS_CALL_FLAGS_NONE,
                         -1,
                         cancellable,
                         request_call_cb,
                         request);
}

static GVariant*
options_with_token(char const* token,
                   char const* session_token = nullptr)
{
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(token));
  if (session_token)
    g_variant_builder_add(&builder, "{sv}", "session_handle_token",
                          g_variant_new_string(session_token));
  return g_variant_builder_end(&builder);
}

/* The shortcut */

static void
activated_cb(GDBusConnection* connection,
             char const* sender_name,
             char const* object_path,
             char const* interface_name,
             char const* signal_name,
             GVariant* parameters,
             void* user_data)
{
  char const* session = nullptr;
  char const* id = nullptr;
  g_variant_get(parameters, "(&o&sta{sv})", &session, &id, nullptr, nullptr);

  if (state.session_handle && g_str_equal(session, state.session_handle) &&
      g_str_equal(id, SHORTCUT_ID))
    g_action_group_activate_action(G_ACTION_GROUP(state.app), "quick-terminal", nullptr);
}

static void
shortcuts_bound_cb(guint response,
                   GVariant* results)
{
  if (response != 0)
    g_message("The quick terminal's shortcut was not set up (response %u)", response);
}

static GVariant*
shortcuts_to_bind(void)
{
  GVariantBuilder props;
  g_variant_builder_init(&props, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&props, "{sv}", "description",
                        g_variant_new_string(_("Show or hide the quick terminal")));
  g_variant_builder_add(&props, "{sv}", "preferred_trigger",
                        g_variant_new_string(PREFERRED_TRIGGER));

  GVariantBuilder shortcuts;
  g_variant_builder_init(&shortcuts, G_VARIANT_TYPE("a(sa{sv})"));
  g_variant_builder_add(&shortcuts, "(s@a{sv})", SHORTCUT_ID, g_variant_builder_end(&props));
  return g_variant_builder_end(&shortcuts);
}

/* Binds the shortcut, unless the desktop knows it from before, so that
 * the user is not asked each time */
static void
shortcuts_listed_cb(guint response,
                    GVariant* results)
{
  if (response != 0 || !state.connection)
    return;

  gs_unref_variant GVariant* shortcuts =
    results ? g_variant_lookup_value(results, "shortcuts", G_VARIANT_TYPE("a(sa{sv})")) : nullptr;
  if (shortcuts) {
    GVariantIter iter;
    char const* id;
    g_variant_iter_init(&iter, shortcuts);
    while (g_variant_iter_next(&iter, "(&s@a{sv})", &id, nullptr)) {
      if (g_str_equal(id, SHORTCUT_ID))
        return;
    }
  }

  gs_free char* token = new_token();
  portal_request(state.connection, PORTAL_GLOBAL_SHORTCUTS, "BindShortcuts",
                 g_variant_new("(o@a(sa{sv})s@a{sv})",
                               state.session_handle,
                               shortcuts_to_bind(),
                               "",
                               options_with_token(token)),
                 token, shortcuts_bound_cb, state.cancellable);
}

static void
session_created_cb(guint response,
                   GVariant* results)
{
  if (response != 0 || !results || !state.connection)
    return;

  char const* handle = nullptr;
  if (!g_variant_lookup(results, "session_handle", "&s", &handle) &&
      !g_variant_lookup(results, "session_handle", "&o", &handle))
    return;

  g_free(state.session_handle);
  state.session_handle = g_strdup(handle);
  state.activated_id = g_dbus_connection_signal_subscribe(state.connection,
                                                          PORTAL_BUS_NAME,
                                                          PORTAL_GLOBAL_SHORTCUTS,
                                                          "Activated",
                                                          PORTAL_OBJECT_PATH,
                                                          nullptr,
                                                          G_DBUS_SIGNAL_FLAGS_NONE,
                                                          activated_cb,
                                                          nullptr, nullptr);

  gs_free char* token = new_token();
  portal_request(state.connection, PORTAL_GLOBAL_SHORTCUTS, "ListShortcuts",
                 g_variant_new("(o@a{sv})", state.session_handle, options_with_token(token)),
                 token, shortcuts_listed_cb, state.cancellable);
}

static void
create_session(void)
{
  gs_free char* token = new_token();
  gs_free char* session_token = new_token();
  portal_request(state.connection, PORTAL_GLOBAL_SHORTCUTS, "CreateSession",
                 g_variant_new("(@a{sv})", options_with_token(token, session_token)),
                 token, session_created_cb, state.cancellable);
}

static void
registered_cb(GObject* source,
              GAsyncResult* result,
              void* user_data)
{
  gs_free_error GError* error = nullptr;
  gs_unref_variant GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source),
                                                                   result, &error);
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;

  // Older portals do not have the registry; they may still know the app
  if (error)
    g_debug("Could not register the application ID with the portal: %s", error->message);

  create_session();
}

static void
connection_cb(GObject* source,
              GAsyncResult* result,
              void* user_data)
{
  gs_free_error GError* error = nullptr;
  auto const connection = g_dbus_connection_new_for_address_finish(result, &error);
  if (!connection) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning("Failed to connect to the session bus: %s", error->message);
    return;
  }
  state.connection = connection;

  if (terminal_flatpak_is_sandboxed()) {
    create_session();
    return;
  }

  g_dbus_connection_call(connection,
                         PORTAL_BUS_NAME,
                         PORTAL_OBJECT_PATH,
                         PORTAL_REGISTRY,
                         "Register",
                         g_variant_new("(s@a{sv})", TERMINAL_APPLICATION_ID,
                                       g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0)),
                         nullptr,
                         G_DBUS_CALL_FLAGS_NONE,
                         -1,
                         state.cancellable,
                         registered_cb,
                         nullptr);
}

/* Starting with the session */

static void
background_cb(guint response,
              GVariant* results)
{
}

static void
update_autostart(bool enable)
{
  if (terminal_flatpak_is_sandboxed()) {
    // The background portal writes the autostart file for the sandbox
    auto const connection = g_application_get_dbus_connection(state.app);
    if (!connection)
      return;

    char const* const commandline[] = { TERM_LIBEXECDIR "/" TERMINAL_PROGRAM_NAME "-server", nullptr };
    gs_free char* token = new_token();
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(token));
    g_variant_builder_add(&options, "{sv}", "reason",
                          g_variant_new_string(_("The quick terminal’s shortcut works while Pano Terminal runs")));
    g_variant_builder_add(&options, "{sv}", "autostart", g_variant_new_boolean(enable));
    g_variant_builder_add(&options, "{sv}", "commandline", g_variant_new_strv(commandline, -1));
    gs_unref_object GCancellable* cancellable = g_cancellable_new();
    portal_request(connection, PORTAL_BACKGROUND, "RequestBackground",
                   g_variant_new("(s@a{sv})", "", g_variant_builder_end(&options)),
                   token, background_cb, cancellable);
    return;
  }

  gs_free char* dir = g_build_filename(g_get_user_config_dir(), "autostart", nullptr);
  gs_free char* path = g_build_filename(dir, AUTOSTART_FILE, nullptr);
  if (!enable) {
    g_unlink(path);
    return;
  }

  gs_free char* contents =
    g_strdup_printf("[Desktop Entry]\n"
                    "Type=Application\n"
                    "Name=%s\n"
                    "Comment=%s\n"
                    "Exec=%s\n"
                    "NoDisplay=true\n"
                    "X-GNOME-Autostart-enabled=true\n",
                    _("Pano Terminal Quick Terminal"),
                    _("Makes the quick terminal’s shortcut work after logging in"),
                    TERM_LIBEXECDIR "/" TERMINAL_PROGRAM_NAME "-server");
  gs_free_error GError* error = nullptr;
  if (g_mkdir_with_parents(dir, 0700) != 0 ||
      !g_file_set_contents(path, contents, -1, &error))
    g_warning("Failed to write %s: %s", path, error ? error->message : g_strerror(errno));
}

/* Turning it on and off */

static void
disconnect(void)
{
  if (state.cancellable) {
    g_cancellable_cancel(state.cancellable);
    g_clear_object(&state.cancellable);
  }

  if (state.connection) {
    if (state.activated_id)
      g_dbus_connection_signal_unsubscribe(state.connection, state.activated_id);
    if (state.session_handle)
      g_dbus_connection_call(state.connection, PORTAL_BUS_NAME, state.session_handle,
                             PORTAL_SESSION, "Close", nullptr, nullptr,
                             G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
    g_dbus_connection_flush_sync(state.connection, nullptr, nullptr);
    g_dbus_connection_close(state.connection, nullptr, nullptr, nullptr);
    g_clear_object(&state.connection);
  }
  state.activated_id = 0;
  g_clear_pointer(&state.session_handle, g_free);

  if (state.held) {
    g_application_release(state.app);
    state.held = false;
  }
}

static void
connect(void)
{
  if (state.cancellable)
    return;

  // The desktop sends the shortcut only to a running server
  g_application_hold(state.app);
  state.held = true;

  state.cancellable = g_cancellable_new();
  gs_free_error GError* error = nullptr;
  gs_free char* address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  if (!address) {
    g_warning("Failed to find the session bus: %s", error->message);
    return;
  }

  g_dbus_connection_new_for_address(address,
                                    GDBusConnectionFlags(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                         G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
                                    nullptr,
                                    state.cancellable,
                                    connection_cb,
                                    nullptr);
}

static void
setting_changed_cb(GSettings* settings,
                   char const* key,
                   void* user_data)
{
  auto const enable = g_settings_get_boolean(settings, TERMINAL_SETTING_QUICK_TERMINAL_GLOBAL_SHORTCUT_KEY);
  auto const was_enabled = state.cancellable != nullptr;
  if (enable == was_enabled)
    return;

  if (enable)
    connect();
  else
    disconnect();

  // Only when the user changes it, not each time the server starts
  if (key)
    update_autostart(enable);
}

/**
 * terminal_global_shortcuts_init:
 * @app: the server's application
 * @settings: the global settings
 *
 * Sets up the quick terminal's system-wide shortcut while the setting is on.
 */
void
terminal_global_shortcuts_init(GApplication* app,
                               GSettings* settings)
{
  state.app = app;
  state.settings = G_SETTINGS(g_object_ref(settings));
  g_signal_connect(settings, "changed::" TERMINAL_SETTING_QUICK_TERMINAL_GLOBAL_SHORTCUT_KEY,
                   G_CALLBACK(setting_changed_cb), nullptr);
  setting_changed_cb(settings, nullptr, nullptr);
}

void
terminal_global_shortcuts_shutdown(void)
{
  if (!state.settings)
    return;

  g_signal_handlers_disconnect_by_func(state.settings, (void*)setting_changed_cb, nullptr);
  g_clear_object(&state.settings);
  disconnect();
}
