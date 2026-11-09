/*
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

#include <glib/gi18n.h>

#include "terminal-app.hh"
#include "terminal-preferences-list-item.hh"
#include "terminal-preferences-window.hh"
#include "terminal-profile-editor.hh"
#include "terminal-profile-file.hh"
#include "terminal-profile-row.hh"
#include "terminal-profiles-list.hh"
#include "terminal-schemas.hh"
#include "terminal-shortcut-editor.hh"
#include "terminal-util.hh"
#include "terminal-debug.hh"
#include "terminal-libgsystem.hh"

struct _TerminalPreferencesWindow
{
  AdwPreferencesWindow  parent_instance;

  AdwSwitchRow         *accelerator_key;
  GtkListBoxRow        *add_profile_row;
  AdwSwitchRow         *always_check_default;
  AdwComboRow          *new_terminal_mode;
  GListModel           *new_terminal_modes;
  GtkListBox           *profiles_list_box;
  AdwSwitchRow         *notify_command_finished;
  AdwSwitchRow         *toast_on_copy;
  AdwSwitchRow         *ignore_program_titles;
  AdwSpinRow           *notify_command_min_duration;
  AdwSwitchRow         *restore_session;
  AdwSwitchRow         *preserve_container;
  AdwSwitchRow         *highlight_root_and_remote;
  AdwSpinRow           *quick_terminal_width;
  AdwSpinRow           *quick_terminal_height;
  AdwSwitchRow         *restore_window_size;
  AdwComboRow          *rounded_corners;
  GListModel           *rounded_corners_model;
  AdwComboRow          *tab_position;
  GListModel           *tab_positions;
  AdwComboRow          *tab_middle_click;
  GListModel           *tab_middle_clicks;
  AdwComboRow          *tab_bar_position;
  GListModel           *tab_bar_positions;
  AdwComboRow          *theme_variant;
  GListModel           *theme_variants;

  GMenuModel* context_menu_model;
  GtkPopoverMenu* context_menu;
  GSettings* context_settings; // unowned
  char const* context_settings_key; // unowned
};

G_DEFINE_FINAL_TYPE (TerminalPreferencesWindow, terminal_preferences_window, ADW_TYPE_PREFERENCES_WINDOW)

static void
recurse_remove_pop_on_escape(GtkWidget *widget)
{
  if (ADW_IS_NAVIGATION_VIEW(widget)) {
    adw_navigation_view_set_pop_on_escape(ADW_NAVIGATION_VIEW(widget), false);
    return;
  }

  for (auto child = gtk_widget_get_first_child (widget);
       child != nullptr;
       child = gtk_widget_get_next_sibling (child)) {
    recurse_remove_pop_on_escape(child);
  }
}

static bool
terminal_preferences_window_show_context_menu(TerminalPreferencesWindow* self,
                                              double x,
                                              double y)
{
  auto picked = gtk_widget_pick(GTK_WIDGET(self), x, y, GTK_PICK_DEFAULT);
  if (!picked)
    return false;

  auto row = gtk_widget_get_ancestor(picked, ADW_TYPE_PREFERENCES_ROW);
  if (!row)
    return false;

  if (ADW_IS_ENTRY_ROW(row))
    return false; // don't override the context menu for rows having a text entry

  if (!terminal_util_get_settings_and_key_for_widget(row,
                                                     &self->context_settings,
                                                     &self->context_settings_key))
    return false;

  if (!self->context_menu) {
    self->context_menu = GTK_POPOVER_MENU(gtk_popover_menu_new_from_model(self->context_menu_model));
    gtk_popover_set_has_arrow(GTK_POPOVER(self->context_menu), false);
    gtk_popover_set_position(GTK_POPOVER(self->context_menu), GTK_POS_BOTTOM);
    gtk_widget_set_halign(GTK_WIDGET(self->context_menu), GTK_ALIGN_START);
    gtk_widget_set_parent(GTK_WIDGET(self->context_menu), GTK_WIDGET(self));
  }

  auto const rect = cairo_rectangle_int_t{int(x), int(y), 0, 0};
  gtk_popover_set_pointing_to(GTK_POPOVER(self->context_menu), &rect);

  gtk_popover_popup (GTK_POPOVER(self->context_menu));

  return true;
}

static void
terminal_preferences_window_click_pressed_cb(GtkGestureClick* gesture,
                                             int n_press,
                                             double x,
                                             double y,
                                             TerminalPreferencesWindow* self)
{
  auto handled = false;

  if (n_press == 1)
    handled = terminal_preferences_window_show_context_menu(self, x, y);

  if (handled)
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  else
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_DENIED);
}

static void
terminal_preferences_window_add_profile (GtkWidget  *widget,
                                         const char *action_name,
                                         GVariant   *param)
{
  TerminalApp *app = terminal_app_get ();
  g_autofree char *uuid = terminal_app_new_profile (app, nullptr, _("New Profile"));

  /* Showing the new profile immediately is a bit jarring because we're
   * not able to show the action that happened on the list. Just reload
   * the list (happens externally) and let the user click the next action.
   */
#if 0
  TerminalSettingsList *profiles = terminal_app_get_profiles_list (app);
  g_autoptr(GSettings) settings = terminal_settings_list_ref_child (profiles, uuid);

  terminal_preferences_window_edit_profile (TERMINAL_PREFERENCES_WINDOW (widget), settings);
#endif
}

static void
show_toast (TerminalPreferencesWindow *self,
            const char                *title)
{
  adw_preferences_window_add_toast (ADW_PREFERENCES_WINDOW (self),
                                    adw_toast_new (title));
}

static GtkFileDialog *
new_profile_file_dialog (const char *title)
{
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dialog, title);
  gtk_file_dialog_set_modal (dialog, true);

  g_autoptr(GtkFileFilter) profiles = gtk_file_filter_new ();
  gtk_file_filter_set_name (profiles, _("Pano Terminal Profiles"));
  gtk_file_filter_add_suffix (profiles, TERMINAL_PROFILE_FILE_SUFFIX + 1);
  g_autoptr(GtkFileFilter) all = gtk_file_filter_new ();
  gtk_file_filter_set_name (all, _("All Files"));
  gtk_file_filter_add_pattern (all, "*");

  g_autoptr(GListStore) filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
  g_list_store_append (filters, profiles);
  g_list_store_append (filters, all);
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  gtk_file_dialog_set_default_filter (dialog, profiles);

  return dialog;
}

/* Exporting a profile */

typedef struct {
  TerminalPreferencesWindow *window;
  char *name;
} ExportData;

static void
export_data_free (ExportData *data)
{
  g_object_unref (data->window);
  g_free (data->name);
  g_free (data);
}

static void
export_written_cb (GObject      *source,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  ExportData *data = (ExportData *)user_data;
  g_autoptr(GError) error = nullptr;

  if (g_file_replace_contents_finish (G_FILE (source), result, nullptr, &error)) {
    /* Translators: %s is the name of a profile */
    g_autofree char *title = g_strdup_printf (_("Exported “%s”"), data->name);
    show_toast (data->window, title);
  } else {
    /* Translators: %s is an error message */
    g_autofree char *title = g_strdup_printf (_("Could not export the profile: %s"), error->message);
    show_toast (data->window, title);
  }

  export_data_free (data);
}

static void
export_save_cb (GObject      *source,
                GAsyncResult *result,
                gpointer      user_data)
{
  g_autoptr(GSettings) settings = G_SETTINGS (user_data);
  g_autoptr(GFile) file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), result, nullptr);
  auto window = TERMINAL_PREFERENCES_WINDOW (g_object_get_data (source, "window"));

  if (file == nullptr) /* cancelled */
    return;

  gsize length;
  char *text = terminal_profile_file_export (settings, &length);
  g_autoptr(GBytes) bytes = g_bytes_new_take (text, length);

  ExportData *data = g_new0 (ExportData, 1);
  data->window = TERMINAL_PREFERENCES_WINDOW (g_object_ref (window));
  data->name = g_settings_get_string (settings, "visible-name");
  g_file_replace_contents_bytes_async (file, bytes, nullptr, false,
                                       G_FILE_CREATE_REPLACE_DESTINATION,
                                       nullptr, export_written_cb, data);
}

void
terminal_preferences_window_export_profile (TerminalPreferencesWindow *self,
                                            GSettings                 *settings)
{
  g_return_if_fail (TERMINAL_IS_PREFERENCES_WINDOW (self));
  g_return_if_fail (G_IS_SETTINGS (settings));

  g_autoptr(GtkFileDialog) dialog = new_profile_file_dialog (_("Export Profile"));
  g_object_set_data_full (G_OBJECT (dialog), "window", g_object_ref (self), g_object_unref);

  g_autofree char *name = g_settings_get_string (settings, "visible-name");
  /* Keep the name usable as a file name */
  g_strdelimit (name, "/", '-');
  g_autofree char *file_name = g_strconcat (name, TERMINAL_PROFILE_FILE_SUFFIX, nullptr);
  gtk_file_dialog_set_initial_name (dialog, file_name);

  gtk_file_dialog_save (dialog, GTK_WINDOW (self), nullptr,
                        export_save_cb, g_object_ref (settings));
}

/* Importing a profile */

static void
import_loaded_cb (GObject      *source,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  g_autoptr(TerminalPreferencesWindow) self = TERMINAL_PREFERENCES_WINDOW (user_data);
  g_autoptr(GError) error = nullptr;
  g_autofree char *contents = nullptr;
  gsize length = 0;

  g_autoptr(GKeyFile) file = nullptr;
  if (g_file_load_contents_finish (G_FILE (source), result, &contents, &length, nullptr, &error))
    file = terminal_profile_file_load (contents, length, &error);

  if (file == nullptr) {
    /* Translators: %s is an error message */
    g_autofree char *title = g_strdup_printf (_("Could not import the profile: %s"), error->message);
    show_toast (self, title);
    return;
  }

  g_autofree char *name = terminal_profile_file_dup_name (file);
  if (name == nullptr) {
    g_autofree char *basename = g_file_get_basename (G_FILE (source));
    if (g_str_has_suffix (basename, TERMINAL_PROFILE_FILE_SUFFIX))
      basename[strlen (basename) - strlen (TERMINAL_PROFILE_FILE_SUFFIX)] = '\0';
    name = basename[0] ? g_strdup (basename) : g_strdup (_("Imported Profile"));
  }

  TerminalApp *app = terminal_app_get ();
  g_autofree char *uuid = terminal_app_new_profile (app, nullptr, name);
  g_autoptr(GSettings) settings =
    terminal_settings_list_ref_child (terminal_app_get_profiles_list (app), uuid);
  if (settings == nullptr)
    return;

  guint skipped = terminal_profile_file_apply (file, settings);
  /* The file may hold an empty name, which the fallback replaces */
  g_settings_set_string (settings, "visible-name", name);

  g_autofree char *title = nullptr;
  if (skipped == 0)
    /* Translators: %s is the name of a profile */
    title = g_strdup_printf (_("Imported “%s”"), name);
  else
    /* Translators: %s is the name of a profile; %u is a number of settings
     * the profile file had, but which this version does not know or accept */
    title = g_strdup_printf (g_dngettext (GETTEXT_PACKAGE,
                                          "Imported “%s”, skipping %u setting",
                                          "Imported “%s”, skipping %u settings",
                                          skipped),
                             name, skipped);
  show_toast (self, title);
}

static void
import_open_cb (GObject      *source,
                GAsyncResult *result,
                gpointer      user_data)
{
  g_autoptr(TerminalPreferencesWindow) self = TERMINAL_PREFERENCES_WINDOW (user_data);
  g_autoptr(GFile) file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), result, nullptr);

  if (file == nullptr) /* cancelled */
    return;

  g_file_load_contents_async (file, nullptr, import_loaded_cb, g_steal_pointer (&self));
}

static void
terminal_preferences_window_import_profile (GtkWidget  *widget,
                                            const char *action_name,
                                            GVariant   *param)
{
  g_autoptr(GtkFileDialog) dialog = new_profile_file_dialog (_("Import Profile"));

  gtk_file_dialog_open (dialog, GTK_WINDOW (widget), nullptr,
                        import_open_cb, g_object_ref (widget));
}

static void
terminal_preferences_window_view_shortcuts (GtkWidget  *widget,
                                            const char *action_name,
                                            GVariant   *param)
{
  adw_preferences_window_push_subpage (ADW_PREFERENCES_WINDOW (widget),
                                       ADW_NAVIGATION_PAGE (terminal_shortcut_editor_new ()));
}

static void
terminal_preferences_window_set_as_default (GtkWidget  *widget,
                                            const char *action_name,
                                            GVariant   *param)
{
  terminal_app_make_default_terminal (terminal_app_get ());
}

static void
terminal_preferences_window_reset(GtkWidget* widget,
                                  char const* action_name,
                                  GVariant* parameter)
{
  auto const self = TERMINAL_PREFERENCES_WINDOW(widget);

  g_settings_reset(self->context_settings,
                   self->context_settings_key);
}

static void
notify_is_default_terminal_cb (TerminalPreferencesWindow *self,
                               GParamSpec                *pspec,
                               TerminalApp               *app)
{
  g_assert (TERMINAL_IS_PREFERENCES_WINDOW (self));
  g_assert (TERMINAL_IS_APP (app));

  gtk_widget_action_set_enabled (GTK_WIDGET (self),
                                 "terminal.set-as-default",
                                 !terminal_app_is_default_terminal (app));
}

static void
terminal_preferences_window_profile_row_activated_cb (TerminalPreferencesWindow *self,
                                                      TerminalProfileRow        *row)
{
  g_assert (TERMINAL_IS_PREFERENCES_WINDOW (self));
  g_assert (TERMINAL_IS_PROFILE_ROW (row));

  terminal_preferences_window_edit_profile (self,
                                            terminal_profile_row_get_settings (row));
}

static void
terminal_preferences_window_reload_profiles (TerminalPreferencesWindow *self)
{
  g_autolist(GSettings) profiles_settings = nullptr;
  TerminalSettingsList *profiles;
  TerminalApp *app;
  GtkWidget *child;

  g_assert (TERMINAL_IS_PREFERENCES_WINDOW (self));

  app = terminal_app_get ();
  profiles = terminal_app_get_profiles_list (app);
  profiles_settings = terminal_profiles_list_ref_children_sorted (profiles);

  child = gtk_widget_get_first_child (GTK_WIDGET (self->profiles_list_box));

  while (child != nullptr) {
    GtkListBoxRow *row = GTK_LIST_BOX_ROW (child);

    child = gtk_widget_get_next_sibling (child);

    if (row != self->add_profile_row)
      gtk_list_box_remove (self->profiles_list_box, GTK_WIDGET (row));
  }

  for (const GList *iter = g_list_last (profiles_settings); iter; iter = iter->prev) {
    GSettings *settings = G_SETTINGS (iter->data);
    GtkWidget *row = terminal_profile_row_new (settings);

    g_signal_connect_object (row,
                             "activated",
                             G_CALLBACK (terminal_preferences_window_profile_row_activated_cb),
                             self,
                             G_CONNECT_SWAPPED);
    gtk_list_box_prepend (self->profiles_list_box, row);
  }
}

static gboolean
string_to_index (GValue   *value,
                 GVariant *variant,
                 gpointer  user_data)
{
  GListModel *model = G_LIST_MODEL (user_data);
  guint n_items = g_list_model_get_n_items (model);

  for (guint i = 0; i < n_items; i++) {
    g_autoptr(TerminalPreferencesListItem) item = TERMINAL_PREFERENCES_LIST_ITEM (g_list_model_get_item (model, i));
    GVariant *item_value = terminal_preferences_list_item_get_value (item);

    if (g_variant_equal (variant, item_value)) {
      g_value_set_uint (value, i);
      return TRUE;
    }
  }

  return FALSE;
}

static GVariant *
index_to_string (const GValue       *value,
                 const GVariantType *type,
                 gpointer            user_data)
{
  guint index = g_value_get_uint (value);
  GListModel *model = G_LIST_MODEL (user_data);
  g_autoptr(TerminalPreferencesListItem) item = TERMINAL_PREFERENCES_LIST_ITEM (g_list_model_get_item (model, index));

  if (item != nullptr)
    return g_variant_ref (terminal_preferences_list_item_get_value (item));

  return nullptr;
}

static void
terminal_preferences_window_size_allocate(GtkWidget* widget,
                                          int width,
                                          int height,
                                          int baseline)
{
  auto const self = TERMINAL_PREFERENCES_WINDOW(widget);

  GTK_WIDGET_CLASS(terminal_preferences_window_parent_class)->size_allocate(widget, width, height, baseline);

  if (self->context_menu) {
    gtk_popover_present(GTK_POPOVER(self->context_menu));
  }
}

static void
terminal_preferences_window_constructed (GObject *object)
{
  TerminalPreferencesWindow *self = (TerminalPreferencesWindow *)object;
  TerminalSettingsList *profiles;
  TerminalApp *app;
  GSettings *settings;

  G_OBJECT_CLASS (terminal_preferences_window_parent_class)->constructed (object);

  app = terminal_app_get ();
  settings = terminal_app_get_global_settings (app);
  profiles = terminal_app_get_profiles_list (app);

  g_signal_connect_object (app,
                           "notify::is-default-terminal",
                           G_CALLBACK (notify_is_default_terminal_cb),
                           self,
                           G_CONNECT_SWAPPED);
  notify_is_default_terminal_cb (self, nullptr, app);

  terminal_util_g_settings_bind (settings,
                   TERMINAL_SETTING_ALWAYS_CHECK_DEFAULT_KEY,
                   self->always_check_default,
                   "active",
                   GSettingsBindFlags(G_SETTINGS_BIND_GET | G_SETTINGS_BIND_SET));
  terminal_util_g_settings_bind (settings,
                   TERMINAL_SETTING_IGNORE_PROGRAM_TITLES_KEY,
                   self->ignore_program_titles,
                   "active",
                   GSettingsBindFlags(G_SETTINGS_BIND_GET | G_SETTINGS_BIND_SET));
  terminal_util_g_settings_bind (settings,
                   TERMINAL_SETTING_TOAST_ON_COPY_KEY,
                   self->toast_on_copy,
                   "active",
                   GSettingsBindFlags(G_SETTINGS_BIND_GET | G_SETTINGS_BIND_SET));
  terminal_util_g_settings_bind (settings,
                   TERMINAL_SETTING_NOTIFY_COMMAND_FINISHED_KEY,
                   self->notify_command_finished,
                   "active",
                   GSettingsBindFlags(G_SETTINGS_BIND_GET | G_SETTINGS_BIND_SET));
  terminal_util_g_settings_bind (settings,
                   TERMINAL_SETTING_NOTIFY_COMMAND_MIN_DURATION_KEY,
                   self->notify_command_min_duration,
                   "value",
                   GSettingsBindFlags(G_SETTINGS_BIND_GET | G_SETTINGS_BIND_SET));
  terminal_util_g_settings_bind (settings,
                   TERMINAL_SETTING_QUICK_TERMINAL_WIDTH_KEY,
                   self->quick_terminal_width,
                   "value",
                   GSettingsBindFlags(G_SETTINGS_BIND_GET | G_SETTINGS_BIND_SET));
  terminal_util_g_settings_bind (settings,
                   TERMINAL_SETTING_QUICK_TERMINAL_HEIGHT_KEY,
                   self->quick_terminal_height,
                   "value",
                   GSettingsBindFlags(G_SETTINGS_BIND_GET | G_SETTINGS_BIND_SET));
  terminal_util_g_settings_bind (settings,
                   TERMINAL_SETTING_HIGHLIGHT_ROOT_AND_REMOTE_KEY,
                   self->highlight_root_and_remote,
                   "active",
                   GSettingsBindFlags(G_SETTINGS_BIND_GET | G_SETTINGS_BIND_SET));
  terminal_util_g_settings_bind (settings,
                   TERMINAL_SETTING_PRESERVE_CONTAINER_KEY,
                   self->preserve_container,
                   "active",
                   GSettingsBindFlags(G_SETTINGS_BIND_GET | G_SETTINGS_BIND_SET));
  terminal_util_g_settings_bind (settings,
                   TERMINAL_SETTING_RESTORE_SESSION_KEY,
                   self->restore_session,
                   "active",
                   GSettingsBindFlags(G_SETTINGS_BIND_GET | G_SETTINGS_BIND_SET));
  terminal_util_g_settings_bind (settings,
                   TERMINAL_SETTING_RESTORE_WINDOW_SIZE_KEY,
                   self->restore_window_size,
                   "active",
                   GSettingsBindFlags(G_SETTINGS_BIND_GET | G_SETTINGS_BIND_SET));
  terminal_util_g_settings_bind (settings,
                   TERMINAL_SETTING_ENABLE_MENU_BAR_ACCEL_KEY,
                   self->accelerator_key,
                   "active",
                   GSettingsBindFlags(G_SETTINGS_BIND_GET | G_SETTINGS_BIND_SET));

  g_signal_connect_object (profiles,
                           "children-changed",
                           G_CALLBACK (terminal_preferences_window_reload_profiles),
                           self,
                           G_CONNECT_SWAPPED);
  g_signal_connect_object (profiles,
                           "default-changed",
                           G_CALLBACK (terminal_preferences_window_reload_profiles),
                           self,
                           G_CONNECT_SWAPPED);
  g_signal_connect_object (profiles,
                           "child-changed::" TERMINAL_PROFILE_VISIBLE_NAME_KEY,
                           G_CALLBACK(terminal_preferences_window_reload_profiles),
                           self,
                           G_CONNECT_SWAPPED);
  terminal_preferences_window_reload_profiles (self);

  terminal_util_g_settings_bind_with_mapping (settings,
                                TERMINAL_SETTING_ROUNDED_CORNERS_KEY,
                                self->rounded_corners,
                                "selected",
                                GSettingsBindFlags(G_SETTINGS_BIND_GET | G_SETTINGS_BIND_SET),
                                string_to_index,
                                index_to_string,
                                g_object_ref (self->rounded_corners_model),
                                g_object_unref);

  terminal_util_g_settings_bind_with_mapping (settings,
                                TERMINAL_SETTING_TAB_POSITION_KEY,
                                self->tab_bar_position,
                                "selected",
                                GSettingsBindFlags(G_SETTINGS_BIND_GET | G_SETTINGS_BIND_SET),
                                string_to_index,
                                index_to_string,
                                g_object_ref (self->tab_bar_positions),
                                g_object_unref);

  terminal_util_g_settings_bind_with_mapping (settings,
                                TERMINAL_SETTING_THEME_VARIANT_KEY,
                                self->theme_variant,
                                "selected",
                                GSettingsBindFlags(G_SETTINGS_BIND_GET | G_SETTINGS_BIND_SET),
                                string_to_index,
                                index_to_string,
                                g_object_ref (self->theme_variants),
                                g_object_unref);

  terminal_util_g_settings_bind_with_mapping (settings,
                                TERMINAL_SETTING_NEW_TERMINAL_MODE_KEY,
                                self->new_terminal_mode,
                                "selected",
                                GSettingsBindFlags(G_SETTINGS_BIND_GET | G_SETTINGS_BIND_SET),
                                string_to_index,
                                index_to_string,
                                g_object_ref (self->new_terminal_modes),
                                g_object_unref);

  terminal_util_g_settings_bind_with_mapping (settings,
                                TERMINAL_SETTING_NEW_TAB_POSITION_KEY,
                                self->tab_position,
                                "selected",
                                GSettingsBindFlags(G_SETTINGS_BIND_GET | G_SETTINGS_BIND_SET),
                                string_to_index,
                                index_to_string,
                                g_object_ref (self->tab_positions),
                                g_object_unref);

  terminal_util_g_settings_bind_with_mapping (settings,
                                TERMINAL_SETTING_TAB_MIDDLE_CLICK_KEY,
                                self->tab_middle_click,
                                "selected",
                                GSettingsBindFlags(G_SETTINGS_BIND_GET | G_SETTINGS_BIND_SET),
                                string_to_index,
                                index_to_string,
                                g_object_ref (self->tab_middle_clicks),
                                g_object_unref);

  // Unfortunately there is no API for AdwPreferencesWindow to do this,
  // nor can we directly get its AdwNavigationView since it's not an
  // internal-child in GtkBuilder.
  recurse_remove_pop_on_escape(GTK_WIDGET(self));
  terminal_util_remove_widget_shortcuts(GTK_WIDGET(self));
}

static void
terminal_preferences_window_dispose (GObject *object)
{
  TerminalPreferencesWindow *self = (TerminalPreferencesWindow *)object;

  g_clear_pointer(reinterpret_cast<GtkWidget**>(&self->context_menu), gtk_widget_unparent);

  gtk_widget_dispose_template (GTK_WIDGET (self), TERMINAL_TYPE_PREFERENCES_WINDOW);

  G_OBJECT_CLASS (terminal_preferences_window_parent_class)->dispose (object);
}

static void
terminal_preferences_window_class_init (TerminalPreferencesWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->constructed = terminal_preferences_window_constructed;
  object_class->dispose = terminal_preferences_window_dispose;

  widget_class->size_allocate = terminal_preferences_window_size_allocate;

  gtk_widget_class_set_template_from_resource (widget_class, "/com/aripd/pano/ui/preferences-window.ui");

  gtk_widget_class_bind_template_child (widget_class, TerminalPreferencesWindow, accelerator_key);
  gtk_widget_class_bind_template_child (widget_class, TerminalPreferencesWindow, add_profile_row);
  gtk_widget_class_bind_template_child (widget_class, TerminalPreferencesWindow, always_check_default);
  gtk_widget_class_bind_template_child (widget_class, TerminalPreferencesWindow, notify_command_finished);
  gtk_widget_class_bind_template_child (widget_class, TerminalPreferencesWindow, toast_on_copy);
  gtk_widget_class_bind_template_child (widget_class, TerminalPreferencesWindow, ignore_program_titles);
  gtk_widget_class_bind_template_child (widget_class, TerminalPreferencesWindow, notify_command_min_duration);
  gtk_widget_class_bind_template_child (widget_class, TerminalPreferencesWindow, restore_session);
  gtk_widget_class_bind_template_child (widget_class, TerminalPreferencesWindow, preserve_container);
  gtk_widget_class_bind_template_child (widget_class, TerminalPreferencesWindow, highlight_root_and_remote);
  gtk_widget_class_bind_template_child (widget_class, TerminalPreferencesWindow, quick_terminal_width);
  gtk_widget_class_bind_template_child (widget_class, TerminalPreferencesWindow, quick_terminal_height);
  gtk_widget_class_bind_template_child (widget_class, TerminalPreferencesWindow, restore_window_size);
  gtk_widget_class_bind_template_child (widget_class, TerminalPreferencesWindow, new_terminal_mode);
  gtk_widget_class_bind_template_child (widget_class, TerminalPreferencesWindow, new_terminal_modes);
  gtk_widget_class_bind_template_child (widget_class, TerminalPreferencesWindow, profiles_list_box);
  gtk_widget_class_bind_template_child (widget_class, TerminalPreferencesWindow, rounded_corners);
  gtk_widget_class_bind_template_child (widget_class, TerminalPreferencesWindow, rounded_corners_model);
  gtk_widget_class_bind_template_child (widget_class, TerminalPreferencesWindow, tab_bar_position);
  gtk_widget_class_bind_template_child (widget_class, TerminalPreferencesWindow, tab_bar_positions);
  gtk_widget_class_bind_template_child (widget_class, TerminalPreferencesWindow, tab_position);
  gtk_widget_class_bind_template_child (widget_class, TerminalPreferencesWindow, tab_positions);
  gtk_widget_class_bind_template_child (widget_class, TerminalPreferencesWindow, tab_middle_click);
  gtk_widget_class_bind_template_child (widget_class, TerminalPreferencesWindow, tab_middle_clicks);
  gtk_widget_class_bind_template_child (widget_class, TerminalPreferencesWindow, theme_variant);
  gtk_widget_class_bind_template_child (widget_class, TerminalPreferencesWindow, theme_variants);
  gtk_widget_class_bind_template_child (widget_class, TerminalPreferencesWindow, context_menu_model);
  gtk_widget_class_bind_template_callback(widget_class, terminal_preferences_window_click_pressed_cb);

  gtk_widget_class_install_action (widget_class,
                                   "terminal.set-as-default",
                                   nullptr,
                                   terminal_preferences_window_set_as_default);

  gtk_widget_class_install_action (widget_class,
                                   "preferences.view-shortcuts",
                                   nullptr,
                                   terminal_preferences_window_view_shortcuts);

  gtk_widget_class_install_action (widget_class,
                                   "profile.add",
                                   nullptr,
                                   terminal_preferences_window_add_profile);

  gtk_widget_class_install_action (widget_class,
                                   "profile.import",
                                   nullptr,
                                   terminal_preferences_window_import_profile);

  gtk_widget_class_install_action (widget_class,
                                   "preferences.reset",
                                   nullptr,
                                   terminal_preferences_window_reset);

  g_type_ensure (TERMINAL_TYPE_PREFERENCES_LIST_ITEM);
  g_type_ensure (TERMINAL_TYPE_PROFILE_EDITOR);
  g_type_ensure (TERMINAL_TYPE_SHORTCUT_EDITOR);
}

static void
terminal_preferences_window_init (TerminalPreferencesWindow *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

#ifdef ENABLE_DEBUG
  _TERMINAL_DEBUG_IF(TERMINAL_DEBUG_FOCUS) {
    _terminal_debug_attach_focus_listener(GTK_WIDGET(self));
  }
#endif
}

GtkWindow*
terminal_preferences_window_new (GtkApplication* application)
{
  return reinterpret_cast<GtkWindow*>(g_object_new(TERMINAL_TYPE_PREFERENCES_WINDOW,
                                                   "application", application,
                                                   nullptr));
}

void
terminal_preferences_window_edit_profile (TerminalPreferencesWindow *self,
                                          GSettings                 *settings)
{
  GtkWidget *editor;

  g_return_if_fail (TERMINAL_IS_PREFERENCES_WINDOW (self));
  g_return_if_fail (G_IS_SETTINGS (settings));

  editor = terminal_profile_editor_new (settings);

  adw_preferences_window_push_subpage (ADW_PREFERENCES_WINDOW (self),
                                       ADW_NAVIGATION_PAGE (editor));
}
