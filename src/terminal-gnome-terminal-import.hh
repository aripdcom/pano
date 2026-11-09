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

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/*
 * Copying the profiles, keyboard shortcuts and preferences of GNOME
 * Terminal, which keeps them under /org/gnome/terminal/legacy/ in dconf.
 * Its schemas may not be installed, but its keys have the same names and
 * types as ours, so they are read with our schemas.
 */

gboolean terminal_gnome_terminal_settings_exist(GSettingsBackend* backend);

guint terminal_gnome_terminal_import(void);

G_END_DECLS
