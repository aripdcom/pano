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

#include <glib.h>

G_BEGIN_DECLS

/*
 * Quick select: finding what is worth copying in the visible text (URLs,
 * paths, hashes, addresses, numbers and the user's own patterns), and
 * labelling each with one or two letters to pick it with.
 */

#define TERMINAL_QUICK_SELECT_MAX_MATCHES 676 /* two letters */

typedef struct {
  guint line; /* the index of the line the match is in */
  long start_col; /* the first column */
  long end_col; /* the column after the last one */
  char* text;
  char label[3];
  gboolean openable; /* a URL or a path */
} TerminalQuickSelectMatch;

GArray* terminal_quick_select_find(char const* const* lines,
                                   char const* const* user_patterns);

void terminal_quick_select_assign_labels(GArray* matches);

TerminalQuickSelectMatch const* terminal_quick_select_lookup(GArray* matches,
                                                             char const* typed,
                                                             gboolean* is_prefix);

G_END_DECLS
