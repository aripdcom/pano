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

/* In a Flatpak sandbox, a shell would only see the runtime, not the
 * user's system. So the shells and the other commands run on the host,
 * through the HostCommand method of Flatpak's session helper (the one
 * that `flatpak-spawn --host` uses), which needs the
 * --talk-name=org.freedesktop.Flatpak permission. The terminal's PTY is
 * passed to the host process as its standard input and output.
 *
 * The sandbox has its own process namespace, so the host processes do not
 * show in its /proc. What a terminal runs, and where, is asked from the
 * host with a small shell script instead.
 */

#include "config.h"

#include "terminal-flatpak.hh"
#include "terminal-libgsystem.hh"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <gio/gunixfdlist.h>
#include <gio/gunixinputstream.h>
#include <glib-unix.h>

#define FLATPAK_BUS_NAME "org.freedesktop.Flatpak"
#define FLATPAK_OBJECT_PATH "/org/freedesktop/Flatpak/Development"
#define FLATPAK_INTERFACE "org.freedesktop.Flatpak.Development"

enum {
  FLATPAK_HOST_COMMAND_FLAGS_CLEAR_ENV = 1 << 0,
  FLATPAK_HOST_COMMAND_FLAGS_WATCH_BUS = 1 << 1,
};

/**
 * terminal_flatpak_is_sandboxed:
 *
 * Returns: whether Pano Terminal runs in a Flatpak sandbox
 */
gboolean
terminal_flatpak_is_sandboxed(void)
{
  static int sandboxed = -1;
  if (sandboxed == -1)
    sandboxed = g_file_test("/.flatpak-info", G_FILE_TEST_EXISTS);
  return sandboxed;
}

/**
 * terminal_flatpak_host_path:
 * @path: a path in the sandbox
 *
 * Returns: (transfer full): where the host finds @path: the files of the
 *   application under /app are elsewhere there
 */
char*
terminal_flatpak_host_path(char const* path)
{
  if (!terminal_flatpak_is_sandboxed() ||
      !(g_str_equal(path, "/app") || g_str_has_prefix(path, "/app/")))
    return g_strdup(path);

  static char* app_path = nullptr;
  static gsize loaded = 0;
  if (g_once_init_enter(&loaded)) {
    g_autoptr(GKeyFile) info = g_key_file_new();
    if (g_key_file_load_from_file(info, "/.flatpak-info", G_KEY_FILE_NONE, nullptr))
      app_path = g_key_file_get_string(info, "Instance", "app-path", nullptr);
    g_once_init_leave(&loaded, 1);
  }

  if (!app_path)
    return g_strdup(path);
  return g_strconcat(app_path, path + strlen("/app"), nullptr);
}

/* The host's environment and the user's shell there */

static char** host_environ = nullptr;
static char* host_shell = nullptr;

static void
load_host_info(void)
{
  static gsize loaded = 0;
  if (!g_once_init_enter(&loaded))
    return;

  // The shell from the user database, then the environment
  char const* const argv[] = {
    "flatpak-spawn", "--host", "sh", "-c",
    "u=$(id -un); "
    "s=$(getent passwd \"$u\" 2>/dev/null || grep \"^$u:\" /etc/passwd); "
    "printf '%s\\0' \"${s##*:}\"; "
    "env -0 2>/dev/null || env | tr '\\n' '\\0'",
    nullptr
  };
  gs_free_error GError* error = nullptr;
  gs_unref_bytes GBytes* output = nullptr;
  gs_unref_object GSubprocess* process =
    g_subprocess_newv(argv,
                      GSubprocessFlags(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                       G_SUBPROCESS_FLAGS_STDERR_SILENCE),
                      &error);
  if (!process || !g_subprocess_communicate(process, nullptr, nullptr, &output, nullptr, &error))
    g_warning("Failed to look up the environment of the host: %s", error->message);

  if (output) {
    gsize length = 0;
    auto const data = reinterpret_cast<char const*>(g_bytes_get_data(output, &length));
    auto const strv = g_ptr_array_new();
    auto first = true;
    for (gsize start = 0; start < length; ) {
      auto end = start;
      while (end < length && data[end])
        ++end;
      auto const entry = g_strndup(data + start, end - start);
      if (first)
        host_shell = entry;
      else if (strchr(entry, '='))
        g_ptr_array_add(strv, entry);
      else
        g_free(entry);
      first = false;
      start = end + 1;
    }
    g_ptr_array_add(strv, nullptr);
    host_environ = reinterpret_cast<char**>(g_ptr_array_free(strv, false));
  }

  if (host_shell && !host_shell[0])
    g_clear_pointer(&host_shell, g_free);
  if (!host_environ || !host_environ[0]) {
    g_strfreev(host_environ);
    host_environ = nullptr;
  }

  g_once_init_leave(&loaded, 1);
}

/**
 * terminal_flatpak_dup_host_environ:
 *
 * Returns: (transfer full) (nullable): the environment of the host's
 *   session, as the commands get it there
 */
char**
terminal_flatpak_dup_host_environ(void)
{
  load_host_info();
  return g_strdupv(host_environ);
}

/**
 * terminal_flatpak_get_host_shell:
 *
 * Returns: (nullable): the user's shell on the host
 */
char const*
terminal_flatpak_get_host_shell(void)
{
  load_host_info();
  return host_shell;
}

/* Exits of the host processes. The session helper reports them with a
 * signal, which may come before the reply to the call that started it. */

static GHashTable* watched_pids = nullptr; // pid → GWeakRef to the terminal
static GHashTable* early_exits = nullptr;  // pid → wait status
static unsigned n_pending_spawns = 0;

static void
weak_ref_free(void* data)
{
  auto const ref = reinterpret_cast<GWeakRef*>(data);
  g_weak_ref_clear(ref);
  g_free(ref);
}

static void
emit_child_exited(GWeakRef* ref,
                  int status)
{
  if (auto const terminal = g_weak_ref_get(ref)) {
    g_signal_emit_by_name(terminal, "child-exited", status);
    g_object_unref(terminal);
  }
}

static void
host_command_exited_cb(GDBusConnection* connection,
                       char const* sender_name,
                       char const* object_path,
                       char const* interface_name,
                       char const* signal_name,
                       GVariant* parameters,
                       void* user_data)
{
  guint32 pid = 0, status = 0;
  g_variant_get(parameters, "(uu)", &pid, &status);

  GWeakRef* ref = nullptr;
  if (g_hash_table_steal_extended(watched_pids, GUINT_TO_POINTER(pid), nullptr, (void**)&ref)) {
    emit_child_exited(ref, int(status));
    weak_ref_free(ref);
  } else if (n_pending_spawns > 0) {
    g_hash_table_insert(early_exits, GUINT_TO_POINTER(pid), GUINT_TO_POINTER(status));
  }
}

static GDBusConnection*
get_connection(GCancellable* cancellable,
               GError** error)
{
  auto const connection = g_bus_get_sync(G_BUS_TYPE_SESSION, cancellable, error);
  if (!connection)
    return nullptr;

  if (!watched_pids) {
    watched_pids = g_hash_table_new_full(nullptr, nullptr, nullptr, weak_ref_free);
    early_exits = g_hash_table_new(nullptr, nullptr);
    g_dbus_connection_signal_subscribe(connection,
                                       FLATPAK_BUS_NAME,
                                       FLATPAK_INTERFACE,
                                       "HostCommandExited",
                                       FLATPAK_OBJECT_PATH,
                                       nullptr,
                                       G_DBUS_SIGNAL_FLAGS_NONE,
                                       host_command_exited_cb,
                                       nullptr, nullptr);
  }
  return connection;
}

static GVariant*
strv_to_aay(char const* const* strv)
{
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("aay"));
  for (auto p = strv; p && *p; ++p)
    g_variant_builder_add_value(&builder, g_variant_new_bytestring(*p));
  return g_variant_builder_end(&builder);
}

static GVariant*
environ_to_variant(char const* const* envv)
{
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("a{ss}"));
  for (auto p = envv; p && *p; ++p) {
    auto const equal = strchr(*p, '=');
    if (!equal)
      continue;
    gs_free char* name = g_strndup(*p, equal - *p);
    g_variant_builder_add(&builder, "{ss}", name, equal + 1);
  }
  return g_variant_builder_end(&builder);
}

/* The other side of the PTY @master */
static int
open_pty_peer(int master,
              GError** error)
{
#ifdef TIOCGPTPEER
  auto const peer = ioctl(master, TIOCGPTPEER, O_RDWR | O_NOCTTY | O_CLOEXEC);
  if (peer != -1)
    return peer;
#endif

  char name[256];
  if (ptsname_r(master, name, sizeof(name)) == 0) {
    auto const fd = open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd != -1)
      return fd;
  }

  auto const errsv = errno;
  g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errsv),
              "Failed to open the PTY: %s", g_strerror(errsv));
  return -1;
}

struct SpawnData {
  GWeakRef terminal;
  VteTerminalSpawnAsyncCallback callback;
  void* user_data;
};

static void
spawn_done_cb(GObject* source,
              GAsyncResult* result,
              void* user_data)
{
  auto const data = reinterpret_cast<SpawnData*>(user_data);
  gs_free_error GError* error = nullptr;
  gs_unref_variant GVariant* reply =
    g_dbus_connection_call_with_unix_fd_list_finish(G_DBUS_CONNECTION(source),
                                                     nullptr, result, &error);
  auto terminal = reinterpret_cast<VteTerminal*>(g_weak_ref_get(&data->terminal));

  GPid pid = -1;
  if (reply) {
    guint32 host_pid = 0;
    g_variant_get(reply, "(u)", &host_pid);
    pid = GPid(host_pid);
  } else {
    g_dbus_error_strip_remote_error(error);
  }

  if (data->callback)
    data->callback(terminal, pid, error, data->user_data);

  if (pid > 0) {
    auto const ref = g_new0(GWeakRef, 1);
    g_weak_ref_init(ref, terminal);

    gpointer status;
    if (g_hash_table_steal_extended(early_exits, GINT_TO_POINTER(pid), nullptr, &status)) {
      emit_child_exited(ref, GPOINTER_TO_INT(status));
      weak_ref_free(ref);
    } else {
      g_hash_table_insert(watched_pids, GINT_TO_POINTER(pid), ref);
    }
  }

  if (--n_pending_spawns == 0)
    g_hash_table_remove_all(early_exits);

  g_clear_object(&terminal);
  g_weak_ref_clear(&data->terminal);
  g_free(data);
}

/**
 * terminal_flatpak_spawn_async:
 *
 * Like vte_terminal_spawn_with_fds_async(), but starts the command on the
 * host. @envv is the whole environment, see
 * terminal_flatpak_dup_host_environ(). Takes ownership of @fds.
 */
void
terminal_flatpak_spawn_async(VteTerminal* terminal,
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
                             void* user_data)
{
  gs_free_error GError* error = nullptr;
  gs_unref_object GUnixFDList* fd_list = g_unix_fd_list_new();
  gs_unref_object VtePty* pty = nullptr;
  gs_unref_object GDBusConnection* connection = nullptr;
  int peer = -1;

  GVariantBuilder fds_builder;
  g_variant_builder_init(&fds_builder, G_VARIANT_TYPE("a{uh}"));

  if (!(connection = get_connection(cancellable, &error)) ||
      !(pty = vte_terminal_pty_new_sync(terminal, pty_flags, cancellable, &error)) ||
      (peer = open_pty_peer(vte_pty_get_fd(pty), &error)) == -1) {
    for (auto i = 0; i < n_fds; ++i)
      close(fds[i]);
    g_free(fds);
    g_variant_builder_clear(&fds_builder);
    if (callback)
      callback(terminal, -1, error, user_data);
    return;
  }

  /* The terminal is the standard input, output and error. Each needs its
   * own handle: the session helper closes a handle once it moved it. */
  for (auto target = 0u; target < 3; ++target)
    g_variant_builder_add(&fds_builder, "{uh}", target,
                          g_unix_fd_list_append(fd_list, peer, nullptr));
  close(peer);

  // And the files that the command line client passed on
  for (auto i = 0; i < n_fds && i < n_fd_map; ++i) {
    if (fd_map[i] > 2) {
      auto const h = g_unix_fd_list_append(fd_list, fds[i], nullptr);
      if (h != -1)
        g_variant_builder_add(&fds_builder, "{uh}", guint32(fd_map[i]), h);
    }
    close(fds[i]);
  }
  for (auto i = n_fd_map; i < n_fds; ++i)
    close(fds[i]);
  g_free(fds);

  vte_terminal_set_pty(terminal, pty);

  /* The host cannot start a file with another argv[0]; a shell gets "-l"
   * for the "-" of a login shell instead */
  gs_unref_ptrarray GPtrArray* host_argv = g_ptr_array_new();
  if ((spawn_flags & G_SPAWN_FILE_AND_ARGV_ZERO) && argv[0] && argv[1]) {
    g_ptr_array_add(host_argv, (void*)argv[0]);
    if (argv[1][0] == '-')
      g_ptr_array_add(host_argv, (void*)"-l");
    for (auto p = argv + 2; *p; ++p)
      g_ptr_array_add(host_argv, (void*)*p);
  } else {
    for (auto p = argv; *p; ++p)
      g_ptr_array_add(host_argv, (void*)*p);
  }
  g_ptr_array_add(host_argv, nullptr);

  auto const data = g_new0(SpawnData, 1);
  g_weak_ref_init(&data->terminal, terminal);
  data->callback = callback;
  data->user_data = user_data;
  ++n_pending_spawns;

  g_dbus_connection_call_with_unix_fd_list(connection,
                                           FLATPAK_BUS_NAME,
                                           FLATPAK_OBJECT_PATH,
                                           FLATPAK_INTERFACE,
                                           "HostCommand",
                                           g_variant_new("(^ay@aay@a{uh}@a{ss}u)",
                                                         cwd ? cwd : g_get_home_dir(),
                                                         strv_to_aay((char const* const*)host_argv->pdata),
                                                         g_variant_builder_end(&fds_builder),
                                                         environ_to_variant(envv),
                                                         guint32(FLATPAK_HOST_COMMAND_FLAGS_CLEAR_ENV |
                                                                 FLATPAK_HOST_COMMAND_FLAGS_WATCH_BUS)),
                                           G_VARIANT_TYPE("(u)"),
                                           G_DBUS_CALL_FLAGS_NONE,
                                           -1,
                                           fd_list,
                                           cancellable,
                                           spawn_done_cb,
                                           data);
}

/* Commands with their output */

static void
run_splice_done_cb(GObject* source,
                   GAsyncResult* result,
                   void* user_data)
{
  gs_unref_object GTask* task = G_TASK(user_data);
  GError* error = nullptr;
  if (g_output_stream_splice_finish(G_OUTPUT_STREAM(source), result, &error) < 0) {
    g_task_return_error(task, error);
    return;
  }
  g_task_return_pointer(task,
                        g_memory_output_stream_steal_as_bytes(G_MEMORY_OUTPUT_STREAM(source)),
                        GDestroyNotify(g_bytes_unref));
}

static void
run_host_command_done_cb(GObject* source,
                         GAsyncResult* result,
                         void* user_data)
{
  gs_unref_object GTask* task = G_TASK(user_data);
  GError* error = nullptr;
  gs_unref_variant GVariant* reply =
    g_dbus_connection_call_with_unix_fd_list_finish(G_DBUS_CONNECTION(source),
                                                     nullptr, result, &error);
  auto const stream = G_INPUT_STREAM(g_task_get_task_data(task));
  if (!reply) {
    g_dbus_error_strip_remote_error(error);
    g_task_return_error(task, error);
    return;
  }

  gs_unref_object GOutputStream* output = g_memory_output_stream_new_resizable();
  g_output_stream_splice_async(output, stream,
                               GOutputStreamSpliceFlags(G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE |
                                                        G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET),
                               G_PRIORITY_DEFAULT,
                               g_task_get_cancellable(task),
                               run_splice_done_cb,
                               g_object_ref(task));
}

static void
run_communicate_done_cb(GObject* source,
                        GAsyncResult* result,
                        void* user_data)
{
  gs_unref_object GTask* task = G_TASK(user_data);
  GError* error = nullptr;
  GBytes* output = nullptr;
  if (!g_subprocess_communicate_finish(G_SUBPROCESS(source), result, &output, nullptr, &error)) {
    g_task_return_error(task, error);
    return;
  }
  g_task_return_pointer(task, output ? output : g_bytes_new(nullptr, 0),
                        GDestroyNotify(g_bytes_unref));
}

/**
 * terminal_flatpak_run_async:
 * @argv: the command, found in the PATH
 *
 * Runs @argv, on the host when in a sandbox, and collects its output.
 */
void
terminal_flatpak_run_async(char const* const* argv,
                           GCancellable* cancellable,
                           GAsyncReadyCallback callback,
                           void* user_data)
{
  auto const task = g_task_new(nullptr, cancellable, callback, user_data);
  g_task_set_source_tag(task, (void*)terminal_flatpak_run_async);
  GError* error = nullptr;

  if (!terminal_flatpak_is_sandboxed()) {
    gs_unref_object GSubprocess* process =
      g_subprocess_newv(argv,
                        GSubprocessFlags(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                         G_SUBPROCESS_FLAGS_STDERR_SILENCE),
                        &error);
    if (!process) {
      g_task_return_error(task, error);
      g_object_unref(task);
      return;
    }
    g_subprocess_communicate_async(process, nullptr, cancellable,
                                   run_communicate_done_cb, task);
    return;
  }

  gs_unref_object GDBusConnection* connection = get_connection(cancellable, &error);
  int pipe_fds[2];
  auto const null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
  if (!connection || null_fd == -1 || !g_unix_open_pipe(pipe_fds, O_CLOEXEC, &error)) {
    if (null_fd != -1)
      close(null_fd);
    if (!error)
      error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to open /dev/null");
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }

  gs_unref_object GUnixFDList* fd_list = g_unix_fd_list_new();
  // One handle each, see terminal_flatpak_spawn_async()
  GVariantBuilder fds_builder;
  g_variant_builder_init(&fds_builder, G_VARIANT_TYPE("a{uh}"));
  g_variant_builder_add(&fds_builder, "{uh}", 0u, g_unix_fd_list_append(fd_list, null_fd, nullptr));
  g_variant_builder_add(&fds_builder, "{uh}", 1u, g_unix_fd_list_append(fd_list, pipe_fds[1], nullptr));
  g_variant_builder_add(&fds_builder, "{uh}", 2u, g_unix_fd_list_append(fd_list, null_fd, nullptr));
  close(null_fd);
  close(pipe_fds[1]);
  g_task_set_task_data(task, g_unix_input_stream_new(pipe_fds[0], true), g_object_unref);

  char const* const no_env[] = { nullptr };
  g_dbus_connection_call_with_unix_fd_list(connection,
                                           FLATPAK_BUS_NAME,
                                           FLATPAK_OBJECT_PATH,
                                           FLATPAK_INTERFACE,
                                           "HostCommand",
                                           g_variant_new("(^ay@aay@a{uh}@a{ss}u)",
                                                         g_get_home_dir(),
                                                         strv_to_aay(argv),
                                                         g_variant_builder_end(&fds_builder),
                                                         environ_to_variant(no_env),
                                                         guint32(FLATPAK_HOST_COMMAND_FLAGS_WATCH_BUS)),
                                           G_VARIANT_TYPE("(u)"),
                                           G_DBUS_CALL_FLAGS_NONE,
                                           -1,
                                           fd_list,
                                           cancellable,
                                           run_host_command_done_cb,
                                           task);
}

/**
 * terminal_flatpak_run_finish:
 * @length: (out) (optional): the length of the output
 *
 * Returns: (transfer full): the output of the command, with a NUL at
 *   the end
 */
char*
terminal_flatpak_run_finish(GAsyncResult* result,
                            gsize* length,
                            GError** error)
{
  gs_unref_bytes GBytes* bytes = reinterpret_cast<GBytes*>(g_task_propagate_pointer(G_TASK(result), error));
  if (!bytes)
    return nullptr;

  gsize size = 0;
  auto const data = g_bytes_get_data(bytes, &size);
  if (length)
    *length = size;
  auto const text = reinterpret_cast<char*>(g_malloc(size + 1));
  if (size)
    memcpy(text, data, size);
  text[size] = '\0';
  return text;
}

/* Process information from the host */

void
terminal_flatpak_process_info_free(TerminalFlatpakProcessInfo* info)
{
  if (!info)
    return;
  g_free(info->foreground_name);
  g_free(info->foreground_comm);
  g_free(info->foreground_cmdline);
  g_free(info->cwd);
  g_free(info);
}

static void
query_done_cb(GObject* source,
              GAsyncResult* result,
              void* user_data)
{
  gs_unref_object GTask* task = G_TASK(user_data);
  GError* error = nullptr;
  gsize length = 0;
  gs_free char* output = terminal_flatpak_run_finish(result, &length, &error);
  if (!output) {
    g_task_return_error(task, error);
    return;
  }

  // NUL separated: the foreground process, its effective user, the working
  // directory of the shell, the name and command line of the process
  GPtrArray* fields = g_ptr_array_new();
  for (auto p = output; p < output + length; p += strlen(p) + 1)
    g_ptr_array_add(fields, p);

  if (fields->len < 5 || atoi((char const*)fields->pdata[0]) <= 0) {
    g_ptr_array_unref(fields);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "The process is gone");
    return;
  }

  auto const info = g_new0(TerminalFlatpakProcessInfo, 1);
  info->foreground_pid = atoi((char const*)fields->pdata[0]);
  auto const euid = (char const*)fields->pdata[1];
  info->foreground_euid = euid[0] ? atoi(euid) : -1;
  info->cwd = ((char const*)fields->pdata[2])[0] ? g_strdup((char const*)fields->pdata[2]) : nullptr;
  info->foreground_comm = g_strstrip(g_strdup((char const*)fields->pdata[3]));
  info->foreground_cmdline = g_strstrip(g_strdup((char const*)fields->pdata[4]));

  // Like the native code: the file name of the program, else its name
  gs_strfreev char** words = g_strsplit(info->foreground_cmdline, " ", 2);
  if (words[0] && words[0][0])
    info->foreground_name = g_path_get_basename(words[0]);
  else
    info->foreground_name = g_strdup(info->foreground_comm);

  g_ptr_array_unref(fields);
  g_task_return_pointer(task, info, GDestroyNotify(terminal_flatpak_process_info_free));
}

/**
 * terminal_flatpak_query_process_async:
 * @shell_pid: the host process ID of the shell of a terminal
 *
 * Finds out what runs in the foreground of a terminal, on the host.
 */
void
terminal_flatpak_query_process_async(int shell_pid,
                                     GCancellable* cancellable,
                                     GAsyncReadyCallback callback,
                                     void* user_data)
{
  /* The foreground process group of the terminal is field 8 of the stat
   * file; the name before it is in parentheses and may contain spaces */
  static char const script[] =
    "p=$1\n"
    "s=$(cat /proc/$p/stat 2>/dev/null) || exit 1\n"
    "s=${s##*) }\n"
    "set -- $s\n"
    "f=$6\n"
    "[ \"$f\" -gt 0 ] 2>/dev/null && [ -r /proc/$f/comm ] || f=$p\n"
    "u=\n"
    "while read -r k a b c; do [ \"$k\" = Uid: ] && u=$b && break; done < /proc/$f/status\n"
    "printf '%s\\0%s\\0%s\\0%s\\0' \"$f\" \"$u\" \"$(readlink /proc/$p/cwd)\" \"$(cat /proc/$f/comm)\"\n"
    "tr '\\0' ' ' < /proc/$f/cmdline\n"
    "printf '\\0'\n";

  gs_free char* pid = g_strdup_printf("%d", shell_pid);
  char const* const argv[] = { "sh", "-c", script, "sh", pid, nullptr };
  terminal_flatpak_run_async(argv, cancellable, query_done_cb,
                             g_task_new(nullptr, cancellable, callback, user_data));
}

/**
 * terminal_flatpak_query_process_finish:
 *
 * Returns: (transfer full): what runs in the terminal
 */
TerminalFlatpakProcessInfo*
terminal_flatpak_query_process_finish(GAsyncResult* result,
                                      GError** error)
{
  return reinterpret_cast<TerminalFlatpakProcessInfo*>(g_task_propagate_pointer(G_TASK(result), error));
}
