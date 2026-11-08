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

#include <adwaita.h>

G_BEGIN_DECLS

#define TERMINAL_TYPE_TAB_SIDEBAR (terminal_tab_sidebar_get_type())

G_DECLARE_FINAL_TYPE(TerminalTabSidebar, terminal_tab_sidebar, TERMINAL, TAB_SIDEBAR, GtkWidget)

AdwTabView* terminal_tab_sidebar_get_view(TerminalTabSidebar* sidebar);

void terminal_tab_sidebar_set_view(TerminalTabSidebar* sidebar,
                                   AdwTabView* view);

gboolean terminal_tab_sidebar_get_autohide(TerminalTabSidebar* sidebar);

void terminal_tab_sidebar_set_autohide(TerminalTabSidebar* sidebar,
                                       gboolean autohide);

void terminal_tab_sidebar_set_position(TerminalTabSidebar* sidebar,
                                       GtkPositionType position);

G_END_DECLS
