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

#include "terminal-screen.hh"
#include "terminal-window.hh"

G_BEGIN_DECLS

/*
 * The session is the set of open windows, with their tabs, panes, working
 * directories and sizes. It is saved to a file in the user's state
 * directory while the server runs, and can be restored when Pano Terminal
 * is started again. Running commands are not restored; every pane gets a
 * new shell in its old working directory.
 */

void terminal_session_init(void);

void terminal_session_shutdown(void);

void terminal_session_window_closing(TerminalWindow* window);

bool terminal_session_restore(char const* activation_token,
                              char const* startup_id);

void terminal_session_apply_window_size(TerminalScreen* screen);

G_END_DECLS
