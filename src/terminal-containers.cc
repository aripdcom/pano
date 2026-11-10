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
#include "terminal-flatpak.hh"
#include "terminal-intl.hh"
#include "terminal-libgsystem.hh"

#include <glib/gi18n.h>

/* The tools that are installed, then an empty line, then one line per
 * container: its name, its toolbox label, its manager label. Missing labels
 * print as "<no value>". In the sandbox, this runs on the host. */
static char const list_script[] =
  "command -v podman >/dev/null || exit 0\n"
  "command -v toolbox >/dev/null && echo toolbox\n"
  "command -v distrobox >/dev/null && echo distrobox\n"
  "echo\n"
  "exec podman ps --all --format "
  "'{{.Names}}|{{index .Labels \"com.github.containers.toolbox\"}}|{{index .Labels \"manager\"}}'\n";

static void
list_done_cb(GObject* source,
             GAsyncResult* result,
             void* user_data)
{
  gs_unref_object auto section = G_MENU(user_data);
  gs_free char* output = terminal_flatpak_run_finish(result, nullptr, nullptr);
  if (!output)
    return;

  gs_strfreev char** lines = g_strsplit(output, "\n", -1);
  auto line = lines;
  auto show_toolbox = false, show_distrobox = false;
  for (; *line && **line; ++line) {
    show_toolbox |= g_str_equal(*line, "toolbox");
    show_distrobox |= g_str_equal(*line, "distrobox");
  }

  gs_unref_object auto submenu = g_menu_new();
  for (; *line; ++line) {
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

  char const* const argv[] = { "sh", "-c", list_script, nullptr };
  terminal_flatpak_run_async(argv, nullptr, list_done_cb, g_object_ref(section));
}
