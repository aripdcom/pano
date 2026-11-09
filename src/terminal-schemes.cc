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

#include "terminal-schemes.hh"
#include "terminal-libgsystem.hh"

#define SCHEMES_DIR "schemes"
#define SCHEME_SUFFIX ".palette"

struct _TerminalSchemes
{
  GObject parent_instance;

  GPtrArray* schemes; // TerminalScheme*, unowned, sorted
  GHashTable* by_id; // id → TerminalScheme*, owned
  GFileMonitor* user_monitor;
  guint reload_source;
};

enum {
  CHANGED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

G_DEFINE_FINAL_TYPE(TerminalSchemes, terminal_schemes, G_TYPE_OBJECT)

static void
scheme_free(void* data)
{
  auto const scheme = reinterpret_cast<TerminalScheme*>(data);
  g_free(scheme->id);
  g_free(scheme->name);
  g_free(scheme);
}

static bool
parse_color(GKeyFile* file,
            char const* group,
            char const* key,
            GdkRGBA* color)
{
  gs_free char* value = g_key_file_get_string(file, group, key, nullptr);
  return value && gdk_rgba_parse(color, g_strstrip(value));
}

static bool
parse_colors(GKeyFile* file,
             char const* group,
             TerminalSchemeColors* colors)
{
  if (!parse_color(file, group, "Foreground", &colors->foreground) ||
      !parse_color(file, group, "Background", &colors->background))
    return false;

  for (auto i = 0; i < TERMINAL_SCHEME_PALETTE_SIZE; ++i) {
    char key[16];
    g_snprintf(key, sizeof(key), "Color%d", i);
    if (!parse_color(file, group, key, &colors->palette[i]))
      return false;
  }

  colors->has_cursor = parse_color(file, group, "Cursor", &colors->cursor);
  return true;
}

/* Returns: (transfer full) (nullable): the scheme in @path */
static TerminalScheme*
scheme_load(char const* path,
            char const* id)
{
  g_autoptr(GKeyFile) file = g_key_file_new();
  if (!g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, nullptr))
    return nullptr;

  auto const scheme = g_new0(TerminalScheme, 1);
  auto const has_light = g_key_file_has_group(file, "Light");
  auto const has_dark = g_key_file_has_group(file, "Dark");

  auto ok = false;
  if (has_light || has_dark) {
    // A file may have only one of them; use it for both styles then
    ok = parse_colors(file, has_light ? "Light" : "Dark", &scheme->light) &&
      parse_colors(file, has_dark ? "Dark" : "Light", &scheme->dark);
    scheme->has_variants = has_light && has_dark;
  } else {
    ok = parse_colors(file, "Palette", &scheme->light);
    scheme->dark = scheme->light;
  }

  if (!ok) {
    g_warning("Ignoring the color scheme %s: it lacks colors", path);
    scheme_free(scheme);
    return nullptr;
  }

  scheme->id = g_strdup(id);
  scheme->name = g_key_file_get_locale_string(file, "Palette", "Name", nullptr, nullptr);
  if (!scheme->name)
    scheme->name = g_strdup(id);
  scheme->primary = g_key_file_get_boolean(file, "Palette", "Primary", nullptr);
  return scheme;
}

static void
load_dir(TerminalSchemes* self,
         char const* dir_path)
{
  gs_free_error GError* error = nullptr;
  gs_unref_object auto dir = g_file_new_for_path(dir_path);
  gs_unref_object auto enumerator =
    g_file_enumerate_children(dir, G_FILE_ATTRIBUTE_STANDARD_NAME,
                              G_FILE_QUERY_INFO_NONE, nullptr, &error);
  if (!enumerator)
    return;

  while (auto const info = g_file_enumerator_next_file(enumerator, nullptr, nullptr)) {
    auto const name = g_file_info_get_name(info);
    if (g_str_has_suffix(name, SCHEME_SUFFIX)) {
      gs_free char* id = g_strndup(name, strlen(name) - strlen(SCHEME_SUFFIX));
      gs_free char* path = g_build_filename(dir_path, name, nullptr);
      if (auto const scheme = scheme_load(path, id))
        // A later folder replaces schemes with the same name
        g_hash_table_replace(self->by_id, scheme->id, scheme);
    }
    g_object_unref(info);
  }
}

static int
scheme_compare(void const* a,
               void const* b)
{
  auto const sa = *reinterpret_cast<TerminalScheme* const*>(a);
  auto const sb = *reinterpret_cast<TerminalScheme* const*>(b);
  if (sa->primary != sb->primary)
    return sa->primary ? -1 : 1;
  return g_utf8_collate(sa->name, sb->name);
}

/**
 * terminal_schemes_dup_user_dir:
 *
 * Returns: (transfer full): the folder for the user's own schemes
 */
char*
terminal_schemes_dup_user_dir(void)
{
  return g_build_filename(g_get_user_data_dir(), TERMINAL_PROGRAM_NAME, SCHEMES_DIR, nullptr);
}

static void
terminal_schemes_load(TerminalSchemes* self)
{
  g_ptr_array_set_size(self->schemes, 0);
  g_hash_table_remove_all(self->by_id);

  // Lowest priority first: the installed schemes, the other system data
  // folders (the first in the list wins), then the user's own.
  gs_free char* pkg_dir = g_build_filename(TERM_PKGDATADIR, SCHEMES_DIR, nullptr);
  load_dir(self, pkg_dir);

  auto const system_dirs = g_get_system_data_dirs();
  for (auto n = g_strv_length(const_cast<char**>(system_dirs)); n > 0; --n) {
    gs_free char* dir = g_build_filename(system_dirs[n - 1], TERMINAL_PROGRAM_NAME, SCHEMES_DIR, nullptr);
    if (!g_str_equal(dir, pkg_dir))
      load_dir(self, dir);
  }

  gs_free char* user_dir = terminal_schemes_dup_user_dir();
  load_dir(self, user_dir);

  GHashTableIter iter;
  void* value;
  g_hash_table_iter_init(&iter, self->by_id);
  while (g_hash_table_iter_next(&iter, nullptr, &value))
    g_ptr_array_add(self->schemes, value);
  g_ptr_array_sort(self->schemes, scheme_compare);
}

static gboolean
reload_timeout_cb(void* user_data)
{
  auto const self = TERMINAL_SCHEMES(user_data);

  self->reload_source = 0;
  terminal_schemes_reload(self);
  return G_SOURCE_REMOVE;
}

/* The user's folder changed; files are often written in several steps */
static void
user_dir_changed_cb(GFileMonitor* monitor,
                    GFile* file,
                    GFile* other_file,
                    GFileMonitorEvent event,
                    TerminalSchemes* self)
{
  g_clear_handle_id(&self->reload_source, g_source_remove);
  self->reload_source = g_timeout_add(500, reload_timeout_cb, self);
}

static void
terminal_schemes_init(TerminalSchemes* self)
{
  self->schemes = g_ptr_array_new();
  // The key is the scheme's own id; replacing an entry frees the old scheme
  self->by_id = g_hash_table_new_full(g_str_hash, g_str_equal, nullptr, scheme_free);

  terminal_schemes_load(self);

  // Watch the user's folder, creating it so that it can be watched
  gs_free char* user_dir = terminal_schemes_dup_user_dir();
  g_mkdir_with_parents(user_dir, 0755);
  gs_unref_object auto dir = g_file_new_for_path(user_dir);
  self->user_monitor = g_file_monitor_directory(dir, G_FILE_MONITOR_NONE, nullptr, nullptr);
  if (self->user_monitor)
    g_signal_connect(self->user_monitor, "changed", G_CALLBACK(user_dir_changed_cb), self);
}

static void
terminal_schemes_finalize(GObject* object)
{
  auto const self = TERMINAL_SCHEMES(object);

  g_clear_handle_id(&self->reload_source, g_source_remove);
  g_clear_object(&self->user_monitor);
  g_ptr_array_unref(self->schemes);
  g_hash_table_unref(self->by_id);

  G_OBJECT_CLASS(terminal_schemes_parent_class)->finalize(object);
}

static void
terminal_schemes_class_init(TerminalSchemesClass* klass)
{
  G_OBJECT_CLASS(klass)->finalize = terminal_schemes_finalize;

  /* The list of schemes or their colors changed */
  signals[CHANGED] =
    g_signal_new("changed",
                 G_OBJECT_CLASS_TYPE(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 nullptr, nullptr,
                 g_cclosure_marshal_VOID__VOID,
                 G_TYPE_NONE,
                 0);
}

/**
 * terminal_schemes_get_default:
 *
 * Returns: (transfer none): the color schemes, loaded on first use
 */
TerminalSchemes*
terminal_schemes_get_default(void)
{
  static TerminalSchemes* schemes = nullptr;
  if (!schemes)
    schemes = TERMINAL_SCHEMES(g_object_new(TERMINAL_TYPE_SCHEMES, nullptr));
  return schemes;
}

/**
 * terminal_schemes_get_all:
 * @schemes: the #TerminalSchemes
 *
 * Returns: (transfer none) (element-type TerminalScheme): the schemes,
 *   the primary ones first, then by name
 */
GPtrArray*
terminal_schemes_get_all(TerminalSchemes* schemes)
{
  g_return_val_if_fail(TERMINAL_IS_SCHEMES(schemes), nullptr);

  return schemes->schemes;
}

TerminalScheme const*
terminal_schemes_lookup(TerminalSchemes* schemes,
                        char const* id)
{
  g_return_val_if_fail(TERMINAL_IS_SCHEMES(schemes), nullptr);

  if (!id || !id[0])
    return nullptr;

  return reinterpret_cast<TerminalScheme const*>(g_hash_table_lookup(schemes->by_id, id));
}

/**
 * terminal_schemes_reload:
 * @schemes: the #TerminalSchemes
 *
 * Reads the scheme files again. Pointers to schemes become invalid.
 */
void
terminal_schemes_reload(TerminalSchemes* schemes)
{
  g_return_if_fail(TERMINAL_IS_SCHEMES(schemes));

  terminal_schemes_load(schemes);
  g_signal_emit(schemes, signals[CHANGED], 0);
}

/**
 * terminal_scheme_get_colors:
 * @scheme: a #TerminalScheme
 * @dark: whether the dark style is in use
 *
 * Returns: the colors for the style
 */
TerminalSchemeColors const*
terminal_scheme_get_colors(TerminalScheme const* scheme,
                           gboolean dark)
{
  g_return_val_if_fail(scheme != nullptr, nullptr);

  return dark ? &scheme->dark : &scheme->light;
}
