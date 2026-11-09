/*
 * Copyright © 2008, 2010 Christian Persch
 * Copyright © 2023 Christian Hergert
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

#include <gtk/gtk.h>

#include "terminal-enums.hh"
#include "terminal-screen.hh"

G_BEGIN_DECLS

#define TERMINAL_TYPE_TAB (terminal_tab_get_type())

G_DECLARE_FINAL_TYPE (TerminalTab, terminal_tab, TERMINAL, TAB, GtkWidget)

/*
 * A tab holds one or more terminals ("panes"), arranged in a tree of
 * GtkPaned widgets. The active screen is the pane that had the focus last;
 * the tab forwards its title and icons, and emits "screen-added" and
 * "screen-removed" when panes are split off or closed.
 */

TerminalTab* terminal_tab_new (TerminalScreen *screen);

TerminalScreen *terminal_tab_get_screen (TerminalTab *tab);

void terminal_tab_set_active_screen(TerminalTab* tab,
                                    TerminalScreen* screen);

GList* terminal_tab_list_screens(TerminalTab* tab);

guint terminal_tab_get_n_screens(TerminalTab* tab);

bool terminal_tab_is_split(TerminalTab* tab);

void terminal_tab_split(TerminalTab* tab,
                        TerminalScreen* screen,
                        TerminalScreen* new_screen,
                        GtkOrientation orientation);

void terminal_tab_remove_screen(TerminalTab* tab,
                                TerminalScreen* screen);

bool terminal_tab_focus_pane(TerminalTab* tab,
                             GtkDirectionType direction);

bool terminal_tab_resize_pane(TerminalTab* tab,
                              GtkDirectionType direction);

void terminal_tab_set_zoomed(TerminalTab* tab,
                             bool zoomed);

bool terminal_tab_get_zoomed(TerminalTab* tab);

TerminalTab *terminal_tab_get_from_screen (TerminalScreen *screen);

void terminal_tab_set_screen_policy(TerminalScreen* screen,
                                    TerminalScrollbarPolicy hpolicy,
                                    TerminalScrollbarPolicy vpolicy);

void terminal_tab_set_screen_kinetic_scrolling(TerminalScreen* screen,
                                               bool enable);

void terminal_tab_add_screen_overlay(TerminalScreen* screen,
                                     GtkWidget* child);

void terminal_tab_remove_overlay(GtkWidget* child);

void terminal_tab_set_pinned(TerminalTab* tab,
                             bool pinned);

bool terminal_tab_get_pinned(TerminalTab* tab);

void terminal_tab_set_active(TerminalTab* tab,
                             bool active);

G_END_DECLS
