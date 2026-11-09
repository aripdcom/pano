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

#include "terminal-window.hh"

G_BEGIN_DECLS

/*
 * The command palette: a searchable list of the window's actions, the
 * profiles and the open tabs, which runs the chosen one.
 */

void terminal_command_palette_show(TerminalWindow* window,
                                   GtkWidget* parent);

int terminal_command_palette_match(char const* pattern,
                                   char const* text);

G_END_DECLS
