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

#include "terminal-containers.hh"
#include "terminal-intl.hh"
#include "terminal-libgsystem.hh"

#include <glib/gi18n.h>

/* One line per container: its name, its toolbox label, its manager label.
 * Missing labels print as "<no value>". */
#define PODMAN_FORMAT \
  "{{.Names}}|{{index .Labels \"com.github.containers.toolbox\"}}|{{index .Labels \"manager\"}}"

static void
podman_done_cb(GObject* source,
               GAsyncResult* result,
               void* user_data)
{
  gs_unref_object auto section = G_MENU(user_data);
  gs_free char* output = nullptr;
  if (!g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &output, nullptr, nullptr))
    return;

  auto const have_toolbox = g_find_program_in_path("toolbox");
  auto const have_distrobox = g_find_program_in_path("distrobox");
  auto const show_toolbox = have_toolbox != nullptr;
  auto const show_distrobox = have_distrobox != nullptr;
  g_free(have_toolbox);
  g_free(have_distrobox);

  gs_unref_object auto submenu = g_menu_new();
  gs_strfreev char** lines = g_strsplit(output ? output : "", "\n", -1);
  for (auto line = lines; *line; ++line) {
    gs_strfreev char** fields = g_strsplit(*line, "|", 3);
    if (g_strv_length(fields) != 3 || !fields[0][0])
      continue;

    char const* runtime = nullptr;
    gs_free char* text = nullptr;
    if (show_toolbox && g_str_equal(fields[1], "true")) {
      runtime = "toolbox";
      /* Translators: a container name, and the tool that made it */
      text = g_strdup_printf(_("%s (Toolbox)"), fields[0]);
    } else if (show_distrobox && g_str_equal(fields[2], "distrobox")) {
      runtime = "distrobox";
      /* Translators: a container name, and the tool that made it */
      text = g_strdup_printf(_("%s (Distrobox)"), fields[0]);
    } else {
      continue;
    }

    gs_unref_object auto item = g_menu_item_new(text, nullptr);
    g_menu_item_set_action_and_target(item, "win.new-terminal-in-container",
                                      "(ss)", runtime, fields[0]);
    g_menu_append_item(submenu, item);
  }

  g_menu_remove_all(section);
  if (g_menu_model_get_n_items(G_MENU_MODEL(submenu)) > 0)
    g_menu_append_submenu(section, _("New Tab in _Container"), G_MENU_MODEL(submenu));
}

/**
 * terminal_containers_update_menu:
 * @section: the menu section to fill
 *
 * Lists the containers in the background and puts them into @section.
 * Without podman, the section stays empty.
 */
void
terminal_containers_update_menu(GMenu* section)
{
  g_return_if_fail(G_IS_MENU(section));

  gs_free char* podman = g_find_program_in_path("podman");
  if (!podman) {
    g_menu_remove_all(section);
    return;
  }

  gs_free_error GError* error = nullptr;
  gs_unref_object auto process =
    g_subprocess_new(GSubprocessFlags(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                      G_SUBPROCESS_FLAGS_STDERR_SILENCE),
                     &error,
                     podman, "ps", "--all", "--format", PODMAN_FORMAT, nullptr);
  if (!process)
    return;

  g_subprocess_communicate_utf8_async(process, nullptr, nullptr,
                                      podman_done_cb, g_object_ref(section));
}
