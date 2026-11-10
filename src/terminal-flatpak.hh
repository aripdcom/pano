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
#include <vte/vte.h>

G_BEGIN_DECLS

/* Running in a Flatpak sandbox, where the shell and other commands have
 * to run on the host instead. See terminal-flatpak.cc. */

gboolean terminal_flatpak_is_sandboxed(void);

char* terminal_flatpak_host_path(char const* path);

char** terminal_flatpak_dup_host_environ(void);

char const* terminal_flatpak_get_host_shell(void);

void terminal_flatpak_spawn_async(VteTerminal* terminal,
                                  VtePtyFlags pty_flags,
                                  char const* cwd,
                                  char const* const* argv,
                                  char const* const* envv,
                                  int* fds,
                                  int n_fds,
                                  int const* fd_map,
                                  int n_fd_map,
                                  GSpawnFlags spawn_flags,
                                  GCancellable* cancellable,
                                  VteTerminalSpawnAsyncCallback callback,
                                  void* user_data);

void terminal_flatpak_run_async(char const* const* argv,
                                GCancellable* cancellable,
                                GAsyncReadyCallback callback,
                                void* user_data);

char* terminal_flatpak_run_finish(GAsyncResult* result,
                                  gsize* length,
                                  GError** error);

/* What runs in a terminal, as seen on the host */
typedef struct {
  int foreground_pid;
  int foreground_euid;
  char* foreground_name;
  char* foreground_comm;
  char* foreground_cmdline;
  char* cwd;
} TerminalFlatpakProcessInfo;

void terminal_flatpak_process_info_free(TerminalFlatpakProcessInfo* info);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(TerminalFlatpakProcessInfo, terminal_flatpak_process_info_free)

void terminal_flatpak_query_process_async(int shell_pid,
                                          GCancellable* cancellable,
                                          GAsyncReadyCallback callback,
                                          void* user_data);

TerminalFlatpakProcessInfo* terminal_flatpak_query_process_finish(GAsyncResult* result,
                                                                  GError** error);

G_END_DECLS
