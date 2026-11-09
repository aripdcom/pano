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

#include <gtk/gtk.h>

G_BEGIN_DECLS

/*
 * Color schemes from .palette files, with colors for the light and the
 * dark style; see data/schemes/README.md for the format and folders.
 */

#define TERMINAL_SCHEME_PALETTE_SIZE 16

typedef struct {
  GdkRGBA foreground;
  GdkRGBA background;
  GdkRGBA cursor;
  gboolean has_cursor;
  GdkRGBA palette[TERMINAL_SCHEME_PALETTE_SIZE];
} TerminalSchemeColors;

typedef struct {
  char* id; /* the file name without .palette */
  char* name;
  gboolean primary; /* listed first */
  gboolean has_variants; /* has separate light and dark colors */
  TerminalSchemeColors light;
  TerminalSchemeColors dark;
} TerminalScheme;

#define TERMINAL_TYPE_SCHEMES (terminal_schemes_get_type())

G_DECLARE_FINAL_TYPE(TerminalSchemes, terminal_schemes, TERMINAL, SCHEMES, GObject)

TerminalSchemes* terminal_schemes_get_default(void);

GPtrArray* terminal_schemes_get_all(TerminalSchemes* schemes);

TerminalScheme const* terminal_schemes_lookup(TerminalSchemes* schemes,
                                              char const* id);

void terminal_schemes_reload(TerminalSchemes* schemes);

char* terminal_schemes_dup_user_dir(void);

TerminalSchemeColors const* terminal_scheme_get_colors(TerminalScheme const* scheme,
                                                       gboolean dark);

G_END_DECLS
