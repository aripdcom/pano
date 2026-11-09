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

#include "terminal-profile-file.hh"

#include <glib/gi18n.h>

#define PROFILE_GROUP "Pano Terminal Profile"
#define NAME_KEY "visible-name"

static GSettingsSchema*
profile_ref_schema(GSettings* profile)
{
  GSettingsSchema* schema = nullptr;
  g_object_get(profile, "settings-schema", &schema, nullptr);
  return schema;
}

static int
compare_strings(void const* a,
                void const* b)
{
  return strcmp(*(char const* const*)a, *(char const* const*)b);
}

/**
 * terminal_profile_file_export:
 * @profile: the profile's #GSettings
 * @length: (out) (optional): the length of the text
 *
 * Returns: (transfer full): the profile file's text, with the settings that
 *   differ from their defaults, and always the profile's name
 */
char*
terminal_profile_file_export(GSettings* profile,
                             gsize* length)
{
  g_return_val_if_fail(G_IS_SETTINGS(profile), nullptr);

  g_autoptr(GSettingsSchema) schema = profile_ref_schema(profile);
  g_auto(GStrv) keys = g_settings_schema_list_keys(schema);
  // A stable order makes files easy to compare
  qsort(keys, g_strv_length(keys), sizeof(char*), compare_strings);

  g_autoptr(GKeyFile) file = g_key_file_new();
  for (auto key = keys; *key; ++key) {
    g_autoptr(GVariant) value = g_settings_get_user_value(profile, *key);
    if (!value && g_str_equal(*key, NAME_KEY))
      value = g_settings_get_value(profile, *key);
    if (!value)
      continue;

    g_autofree char* text = g_variant_print(value, false);
    g_key_file_set_value(file, PROFILE_GROUP, *key, text);
  }

  g_key_file_set_comment(file, PROFILE_GROUP, nullptr,
                         " A Pano Terminal profile; import it in Preferences", nullptr);
  return g_key_file_to_data(file, length, nullptr);
}

/**
 * terminal_profile_file_load:
 * @data: the file's contents
 * @length: the length of @data
 * @error: return location for a #GError
 *
 * Returns: (transfer full) (nullable): the profile file, or %NULL if @data
 *   is not a profile file
 */
GKeyFile*
terminal_profile_file_load(char const* data,
                           gsize length,
                           GError** error)
{
  g_autoptr(GKeyFile) file = g_key_file_new();
  if (!g_key_file_load_from_data(file, data, length, G_KEY_FILE_NONE, nullptr) ||
      !g_key_file_has_group(file, PROFILE_GROUP)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        _("The file is not a Pano Terminal profile."));
    return nullptr;
  }

  return static_cast<GKeyFile*>(g_steal_pointer(&file));
}

/**
 * terminal_profile_file_dup_name:
 * @file: a profile file
 *
 * Returns: (transfer full) (nullable): the profile's name
 */
char*
terminal_profile_file_dup_name(GKeyFile* file)
{
  g_autofree char* text = g_key_file_get_value(file, PROFILE_GROUP, NAME_KEY, nullptr);
  if (!text)
    return nullptr;

  g_autoptr(GVariant) value = g_variant_parse(G_VARIANT_TYPE_STRING, text,
                                              nullptr, nullptr, nullptr);
  if (!value || !g_variant_get_string(value, nullptr)[0])
    return nullptr;

  return g_variant_dup_string(value, nullptr);
}

/**
 * terminal_profile_file_apply:
 * @file: a profile file
 * @profile: the profile's #GSettings
 *
 * Writes the settings in @file to @profile, all at once. Settings that the
 * profile does not have, or whose values are of the wrong type or out of
 * range, for example from a newer version, are skipped.
 *
 * Returns: the number of skipped settings
 */
guint
terminal_profile_file_apply(GKeyFile* file,
                            GSettings* profile)
{
  g_return_val_if_fail(G_IS_SETTINGS(profile), 0);

  g_autoptr(GSettingsSchema) schema = profile_ref_schema(profile);
  g_auto(GStrv) keys = g_key_file_get_keys(file, PROFILE_GROUP, nullptr, nullptr);
  if (!keys)
    return 0;

  // Write everything at once, through a settings object of our own, since
  // delaying cannot be turned off again on @profile, which others share
  g_autoptr(GSettingsBackend) backend = nullptr;
  g_autofree char* path = nullptr;
  g_object_get(profile, "backend", &backend, "path", &path, nullptr);
  g_autoptr(GSettings) settings = g_settings_new_full(schema, backend, path);
  g_settings_delay(settings);

  auto skipped = 0u;
  for (auto key = keys; *key; ++key) {
    if (!g_settings_schema_has_key(schema, *key)) {
      ++skipped;
      continue;
    }

    g_autoptr(GSettingsSchemaKey) schema_key = g_settings_schema_get_key(schema, *key);
    g_autofree char* text = g_key_file_get_value(file, PROFILE_GROUP, *key, nullptr);
    g_autoptr(GVariant) value =
      g_variant_parse(g_settings_schema_key_get_value_type(schema_key), text,
                      nullptr, nullptr, nullptr);
    if (!value || !g_settings_schema_key_range_check(schema_key, value)) {
      ++skipped;
      continue;
    }

    g_settings_set_value(settings, *key, value);
  }
  g_settings_apply(settings);

  return skipped;
}
