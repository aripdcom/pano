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

#define G_SETTINGS_ENABLE_BACKEND
#include <gio/gsettingsbackend.h>

#include "terminal-gnome-terminal-import.hh"
#include "terminal-app.hh"
#include "terminal-libgsystem.hh"
#include "terminal-schemas.hh"
#include "terminal-settings-list.hh"
#include "terminal-settings-utils.hh"

#define GT_PATH "/org/gnome/terminal/legacy/"
#define GT_PROFILES_PATH GT_PATH "profiles:/"
#define GT_DEFAULT_PROFILE "b1dcc9dd-5262-4d8d-a863-c897e6d979b9"

static GVariant*
read_value(GSettingsBackend* backend,
           char const* key,
           char const* type)
{
  return terminal_g_settings_backend_read(backend, key, G_VARIANT_TYPE(type), false);
}

/**
 * terminal_gnome_terminal_settings_exist:
 * @backend: the settings backend
 *
 * Returns: whether GNOME Terminal has been used: it writes its schema
 *   version on its first start, and its list of profiles once there is
 *   more than one
 */
gboolean
terminal_gnome_terminal_settings_exist(GSettingsBackend* backend)
{
  gs_unref_variant auto version = read_value(backend, GT_PATH "schema-version", "u");
  gs_unref_variant auto list = read_value(backend, GT_PROFILES_PATH "list", "as");
  gs_unref_variant auto default_profile = read_value(backend, GT_PROFILES_PATH "default", "s");

  return version || list || default_profile;
}

static char*
dup_schema_path(GSettingsSchemaSource* source,
                char const* schema_id)
{
  gs_unref_settings_schema auto schema =
    g_settings_schema_source_lookup(source, schema_id, true);
  return schema ? g_strdup(g_settings_schema_get_path(schema)) : nullptr;
}

/**
 * terminal_gnome_terminal_import:
 *
 * Copies GNOME Terminal's preferences, keyboard shortcuts and profiles.
 * Its profiles keep their identifiers: they are added to ours, replacing
 * profiles with the same identifier, such as the default profile, which
 * both start with. Its default profile becomes ours.
 *
 * Returns: the number of profiles copied
 */
guint
terminal_gnome_terminal_import(void)
{
  auto const app = terminal_app_get();
  auto const backend = terminal_app_get_settings_backend(app);
  auto const source = terminal_app_get_schema_source(app);
  auto const profiles = terminal_app_get_profiles_list(app);

  gs_free auto settings_path = dup_schema_path(source, TERMINAL_SETTING_SCHEMA);
  gs_free auto profiles_path = dup_schema_path(source, TERMINAL_PROFILES_LIST_SCHEMA);
  if (!settings_path || !profiles_path)
    return 0;

  auto const tree = terminal_g_settings_backend_create_tree();

  // Preferences, except the schema version, which is about our own keys
  terminal_g_settings_backend_clone_schema(backend, source, TERMINAL_SETTING_SCHEMA,
                                           GT_PATH, settings_path, tree);
  gs_free auto version_key = g_strconcat(settings_path, TERMINAL_SETTING_SCHEMA_VERSION, nullptr);
  g_tree_remove(tree, version_key);

  terminal_g_settings_backend_clone_schema(backend, source, TERMINAL_KEYBINDINGS_SCHEMA,
                                           GT_PATH "keybindings/", TERMINAL_KEYBINDINGS_SCHEMA_PATH,
                                           tree);

  // Profiles. Without a list, GNOME Terminal has only its default profile.
  gs_unref_variant auto list = read_value(backend, GT_PROFILES_PATH "list", "as");
  gs_unref_variant auto default_value = read_value(backend, GT_PROFILES_PATH "default", "s");
  auto const default_uuid = default_value ? g_variant_get_string(default_value, nullptr)
                                          : GT_DEFAULT_PROFILE;

  gs_free char const** gt_uuids = nullptr;
  if (list)
    gt_uuids = g_variant_get_strv(list, nullptr);

  gs_unref_ptrarray auto uuids = g_ptr_array_new();
  if (gt_uuids) {
    for (auto uuid = gt_uuids; *uuid; ++uuid)
      g_ptr_array_add(uuids, (void*)*uuid);
  } else {
    g_ptr_array_add(uuids, (void*)default_uuid);
  }

  auto n_profiles = 0u;
  for (auto i = 0u; i < uuids->len; ++i) {
    auto const uuid = reinterpret_cast<char const*>(g_ptr_array_index(uuids, i));
    if (!terminal_settings_list_valid_uuid(uuid))
      continue;

    gs_free auto gt_path = g_strdup_printf(GT_PROFILES_PATH ":%s/", uuid);
    gs_free auto path = g_strdup_printf("%s:%s/", profiles_path, uuid);
    terminal_g_settings_backend_clone_schema(backend, source, TERMINAL_PROFILE_SCHEMA,
                                             gt_path, path, tree);
    ++n_profiles;
  }

  // The profiles' settings first, so that they are there once listed
  static int origin_tag;
  (void)terminal_g_settings_backend_write_tree(backend, tree, &origin_tag);
  g_tree_unref(tree);

  gs_strfreev auto current = g_settings_get_strv(G_SETTINGS(profiles), TERMINAL_SETTINGS_LIST_LIST_KEY);
  gs_unref_ptrarray auto merged = g_ptr_array_new();
  for (auto uuid = current; *uuid; ++uuid)
    g_ptr_array_add(merged, *uuid);
  for (auto i = 0u; i < uuids->len; ++i) {
    auto const uuid = reinterpret_cast<char*>(g_ptr_array_index(uuids, i));
    if (terminal_settings_list_valid_uuid(uuid) &&
        !g_strv_contains(const_cast<char const* const*>(current), uuid))
      g_ptr_array_add(merged, uuid);
  }
  g_ptr_array_add(merged, nullptr);
  g_settings_set_strv(G_SETTINGS(profiles), TERMINAL_SETTINGS_LIST_LIST_KEY,
                      reinterpret_cast<char const* const*>(merged->pdata));

  if (terminal_settings_list_valid_uuid(default_uuid))
    terminal_settings_list_set_default_child(profiles, default_uuid);

  return n_profiles;
}
