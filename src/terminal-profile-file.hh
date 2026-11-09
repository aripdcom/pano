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
 * Profile files, for exporting a profile and importing it elsewhere.
 *
 * A profile file is a key file with one group, [Pano Terminal Profile],
 * that holds the settings the profile changed from their defaults, in
 * the GVariant text format, like `dconf dump` prints them:
 *
 *   [Pano Terminal Profile]
 *   visible-name='Solarized'
 *   color-scheme='solarized'
 *   font='Monospace 12'
 *   use-system-font=false
 */

#define TERMINAL_PROFILE_FILE_SUFFIX ".pano-profile"

char* terminal_profile_file_export(GSettings* profile,
                                   gsize* length);

GKeyFile* terminal_profile_file_load(char const* data,
                                     gsize length,
                                     GError** error);

char* terminal_profile_file_dup_name(GKeyFile* file);

guint terminal_profile_file_apply(GKeyFile* file,
                                  GSettings* profile);

G_END_DECLS
