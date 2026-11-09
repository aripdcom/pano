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

#include "terminal-profile-file.hh"
#include "terminal-schemas.hh"

static GSettingsSchema* profile_schema;
static GSettingsBackend* backend;

static GSettings*
new_profile(char const* name)
{
  g_autofree char* path = g_strdup_printf("/test/profiles/%s/", name);
  return g_settings_new_full(profile_schema, backend, path);
}

static void
test_round_trip(void)
{
  g_autoptr(GSettings) source = new_profile("source");
  g_settings_set_string(source, "visible-name", "Solarized \"night\"\nshift");
  g_settings_set_string(source, "font", "Monospace 12");
  g_settings_set_boolean(source, "use-system-font", false);
  g_settings_set_double(source, "background-opacity", 0.75);
  g_settings_set_enum(source, "cursor-shape", 1);

  gsize length;
  g_autofree char* text = terminal_profile_file_export(source, &length);
  g_assert_nonnull(strstr(text, "[Pano Terminal Profile]"));
  g_assert_nonnull(strstr(text, "font='Monospace 12'"));
  g_assert_nonnull(strstr(text, "use-system-font=false"));
  // Settings left at their defaults are not written
  g_assert_null(strstr(text, "scrollback-lines="));

  g_autoptr(GError) error = nullptr;
  g_autoptr(GKeyFile) file = terminal_profile_file_load(text, length, &error);
  g_assert_no_error(error);

  g_autofree char* name = terminal_profile_file_dup_name(file);
  g_assert_cmpstr(name, ==, "Solarized \"night\"\nshift");

  g_autoptr(GSettings) target = new_profile("target");
  g_assert_cmpuint(terminal_profile_file_apply(file, target), ==, 0);

  g_auto(GStrv) keys = g_settings_schema_list_keys(profile_schema);
  for (auto key = keys; *key; ++key) {
    g_autoptr(GVariant) a = g_settings_get_value(source, *key);
    g_autoptr(GVariant) b = g_settings_get_value(target, *key);
    g_assert_true(g_variant_equal(a, b));
  }
}

static void
test_skip_invalid(void)
{
  auto const text =
    "[Pano Terminal Profile]\n"
    "visible-name='Imported'\n"
    "font='Sans 10'\n"
    "no-such-setting=true\n"
    "use-system-font='not a boolean'\n"
    "cursor-shape='no-such-shape'\n"
    "scrollback-lines=@@@\n";

  g_autoptr(GError) error = nullptr;
  g_autoptr(GKeyFile) file = terminal_profile_file_load(text, strlen(text), &error);
  g_assert_no_error(error);

  g_autoptr(GSettings) target = new_profile("invalid");
  g_assert_cmpuint(terminal_profile_file_apply(file, target), ==, 4);

  g_autofree char* font = g_settings_get_string(target, "font");
  g_assert_cmpstr(font, ==, "Sans 10");
  g_autoptr(GVariant) system_font = g_settings_get_user_value(target, "use-system-font");
  g_assert_null(system_font);
}

static void
test_not_a_profile(void)
{
  char const* const texts[] = {
    "",
    "just some text",
    "[Palette]\nName=Not a profile\n",
  };

  for (auto const text : texts) {
    g_autoptr(GError) error = nullptr;
    g_autoptr(GKeyFile) file = terminal_profile_file_load(text, strlen(text), &error);
    g_assert_null(file);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  }

  // A file without a usable name still loads
  auto const nameless = "[Pano Terminal Profile]\nvisible-name=''\n";
  g_autoptr(GKeyFile) file = terminal_profile_file_load(nameless, strlen(nameless), nullptr);
  g_assert_nonnull(file);
  g_autofree char* name = terminal_profile_file_dup_name(file);
  g_assert_null(name);
}

int
main(int argc,
     char* argv[])
{
  g_test_init(&argc, &argv, nullptr);

  auto const schema_dir = g_getenv("TEST_SCHEMA_DIR");
  g_assert_nonnull(schema_dir);
  g_autoptr(GError) error = nullptr;
  g_autoptr(GSettingsSchemaSource) source =
    g_settings_schema_source_new_from_directory(schema_dir, nullptr, true, &error);
  g_assert_no_error(error);

  profile_schema = g_settings_schema_source_lookup(source, TERMINAL_PROFILE_SCHEMA, false);
  g_assert_nonnull(profile_schema);
  backend = g_memory_settings_backend_new();

  g_test_add_func("/profile-file/round-trip", test_round_trip);
  g_test_add_func("/profile-file/skip-invalid", test_skip_invalid);
  g_test_add_func("/profile-file/not-a-profile", test_not_a_profile);

  auto const status = g_test_run();

  g_object_unref(backend);
  g_settings_schema_unref(profile_schema);
  return status;
}
