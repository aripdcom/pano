/*
 * Copyright © 2001 Havoc Pennington
 * Copyright © 2007, 2008, 2010, 2011 Christian Persch
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

#include "config.h"

#include "terminal-pcre2.hh"
#include "terminal-regex.hh"
#include "terminal-screen.hh"
#include "terminal-client-utils.hh"
#include "terminal-notebook.hh"

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <uuid.h>

#include <algorithm>
#include <cmath>

#if defined(__FreeBSD__) || defined(__DragonFly__) || defined(__OpenBSD__)
#include <sys/sysctl.h>
#endif

#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include <adwaita.h>
#include <gtk/gtk.h>

#ifdef GDK_WINDOWING_X11
#include <gdk/x11/gdkx.h>
#endif

#include <adwaita.h>

#include "terminal-accels.hh"
#include "terminal-app.hh"
#include "terminal-debug.hh"
#include "terminal-defines.hh"
#include "terminal-enums.hh"
#include "terminal-flatpak.hh"
#include "terminal-intl.hh"
#include "terminal-marshal.h"
#include "terminal-schemas.hh"
#include "terminal-schemes.hh"
#include "terminal-quick-select.hh"
#include "terminal-tab.hh"
#include "terminal-util.hh"
#include "terminal-window.hh"
#include "terminal-info-bar.hh"
#include "terminal-libgsystem.hh"

#include "eggshell.hh"

/* Links open with Ctrl+click, so they only look clickable while Ctrl is held */
#define URL_MATCH_CURSOR_NAME "pointer"
#define URL_MATCH_IDLE_CURSOR_NAME "text"
#define SIZE_DISMISS_TIMEOUT_MSEC 1000
#define VISUAL_BELL_DURATION_MS 300

#define DROP_REQUEST_PRIORITY               G_PRIORITY_DEFAULT
#define APPLICATION_VND_PORTAL_FILETRANSFER "application/vnd.portal.filetransfer"
#define APPLICATION_VND_PORTAL_FILES        "application/vnd.portal.files"
#define TEXT_X_MOZ_URL                      "text/x-moz-url"
#define TEXT_URI_LIST                       "text/uri-list"
#define X_SPECIAL_GNOME_RESET_BACKGROUND    "x-special/gnome-reset-background"

namespace {

typedef struct {
  volatile int refcount;
  char **argv; /* as passed */
  char **exec_argv; /* as processed */
  char **envv;
  char *cwd;
  gboolean cwd_requested; /* asked for, so not subject to preserve-working-directory */
  gboolean as_shell;

  VtePtyFlags pty_flags;
  GSpawnFlags spawn_flags;

  /* FD passing */
  GUnixFDList *fd_list;
  int n_fd_map;
  int* fd_map;

  /* async exec callback */
  TerminalScreenExecCallback callback;
  gpointer callback_data;
  GDestroyNotify callback_data_destroy_notify;

  /* Cancellable */
  GCancellable *cancellable;
} ExecData;

} // anon namespace

typedef struct
{
  int tag;
  TerminalURLFlavor flavor;
} TagData;

typedef struct {
  TerminalScreen *screen;
  GdkDrop *drop;
  GList *files;
  const char *mime_type;
} TextUriList;

struct _TerminalScreenPrivate
{
  gboolean link_cursor; /* whether links show the pointer cursor, see terminal_screen_modifiers_cb() */
  double pointer_x, pointer_y; /* -1 while the pointer is elsewhere */
  char *saved_word_chars; /* while a double click selects a link, see below */
  char *uuid;
  gboolean registered; /* D-Bus interface is registered */

  GSettings *profile; /* never nullptr */
  char* user_title; /* set with Set Title, or nullptr */
  char* initial_title; /* from --title, or nullptr */
  char* directory_title; /* the working directory, when no title is set */
  /* Monitoring for activity and silence, see terminal_screen_set_monitoring() */
  gboolean monitor_activity;
  gboolean monitor_silence;
  guint silence_source;

  /* The colors in use, if not those of the system style */
  gboolean has_custom_colors;
  GdkRGBA custom_foreground;
  GdkRGBA custom_background;

  GtkWidget* bell_flash; /* the visual bell's overlay, while it shows */
  guint bell_flash_source;

  /* Quick select, while it runs: the labelled matches in the rows from
   * quick_select_first_row on, and the letters typed so far */
  GArray* quick_select; /* TerminalQuickSelectMatch */
  long quick_select_first_row;
  char quick_select_typed[3];
  GtkWidget* quick_select_hint;
  guint profile_changed_id;
  guint profile_forgotten_id;
  int child_pid;
  GSList *match_tags;
  gboolean exec_on_realize;
  guint idle_exec_source;
  ExecData *exec_data;

  GtkRevealer *size_revealer;
  GtkLabel *size_label;
  guint size_dismiss_source;

  GtkDropTargetAsync *drop_target;
  GtkWidget *drop_highlight;

  GIcon* icon_color;
  GIcon* icon_image;

  bool has_progress;
  bool icon_progress_set;
  VteProgressHint progress_hint;
  double progress_fraction;
  GIcon* icon_progress;

  /* The command running at the prompt, from shell integration */
  gint64 command_start; /* monotonic time, 0 if none */
  char* command;
  guint command_capture_source;

  /* Rows of the shell prompts, from shell integration; see PromptMark */
  GArray* prompts;
  long prompts_columns; /* the column count the rows are valid for */
  GArray* prompt_anchors; /* see terminal_screen_anchor_prompts() */
  int allocated_width; /* with this width and character width */
  int allocated_char_width;
  long command_mark_row; /* the prompt of the running command, or -1 */
  long row_origin; /* cache for terminal_screen_get_row_origin() */
  bool row_origin_valid;
  bool precmd_in_batch; /* a prompt was reported in the current batch */
  guint precmd_batch_source;

  /* The container to start the shell in, see terminal_screen_set_container() */
  char* container_runtime;
  char* container_name;

  /* See terminal_screen_update_process_kind() */
  TerminalProcessKind process_kind;
  guint process_kind_source;

  /* In the sandbox: the foreground process on the host, as last seen, and
   * the ongoing query for it */
  TerminalFlatpakProcessInfo* host_process;
  GCancellable* host_process_cancellable;
  bool host_process_query_again;

  /* Input broadcasting, see terminal_screen_is_user_input() */
  bool in_key_event;
  guint key_event_source;
  bool broadcast_excluded;

  /* Search matches to highlight, see terminal_screen_set_search_matches() */
  GArray* search_matches; /* TerminalSearchMatch */
  int search_current;
  long search_columns; /* the column count the matches are valid for */
};

/* VTE keeps the OSC 133 prompt marks to itself, so remember where the
 * prompts are. The shell reports a new prompt (vte.shell.precmd) just
 * before printing it; VTE emits the termprop change after processing the
 * data it has, so the cursor may already be past the prompt, but it is on
 * the row of the prompt either way. The rows of the command output are
 * those between the prompt where the command was entered and the next one.
 *
 * VTE reports the termprops of one batch of data in the order of their IDs
 * (precmd, preexec, postexec), not in the order they arrived. When a quick
 * command starts and ends within one batch, preexec comes after the precmd
 * of the next prompt, but belongs to the prompt before.
 */
struct PromptMark {
  long row; /* absolute row of the prompt */
  bool ran_command; /* a command entered at this prompt was run */
  int status; /* the exit status of that command, or -1 while unknown */
};

#define MAX_PROMPTS 10000

/* Where a prompt mark is in the text, which stays the same when VTE
 * rewraps the lines: the line breaks between the start of its row and the
 * cursor. */
struct PromptAnchor {
  long newlines; /* -1 if not known */
};

/* Marks further up are dropped when the lines rewrap */
#define PROMPT_ANCHOR_ROWS 5000

enum
{
  PROFILE_SET,
  SHOW_POPUP_MENU,
  MATCH_CLICKED,
  CLOSE_SCREEN,
  COMMAND_FINISHED,
  PROMPTS_CHANGED,
  COLUMNS_CHANGED,
  QUICK_SELECT_CHOSEN,
  COLORS_CHANGED,
  ACTIVITY,
  SILENCE,
  LAST_SIGNAL
};

enum {
  PROP_PROFILE = 1,
  PROP_ICON,
  PROP_ICON_PROGRESS,
  PROP_TITLE,
  PROP_PROCESS_KIND,
  N_PROPS
};

static GParamSpec* pspecs[N_PROPS];

static void terminal_screen_constructed (GObject             *object);
static void terminal_screen_dispose     (GObject             *object);
static void terminal_screen_finalize    (GObject             *object);
static void terminal_screen_profile_changed_cb (GSettings     *profile,
                                                const char    *prop_name,
                                                TerminalScreen *screen);
static gboolean terminal_screen_drop_target_drop (TerminalScreen     *screen,
                                                  GdkDrop            *drop,
                                                  double              x,
                                                  double              y,
                                                  GtkDropTargetAsync *drop_target);
static GdkDragAction terminal_screen_drop_target_drag_enter (TerminalScreen     *screen,
                                                             GdkDrop            *drop,
                                                             double              x,
                                                             double              y,
                                                             GtkDropTargetAsync *drop_target);
static void terminal_screen_drop_target_drag_leave (TerminalScreen     *screen,
                                                    GdkDrop            *drop,
                                                    GtkDropTargetAsync *drop_target);
static void terminal_screen_set_font (TerminalScreen *screen);
static void terminal_screen_system_font_changed_cb (GSettings *,
                                                    const char*,
                                                    TerminalScreen *screen);
static void terminal_screen_capture_click_pressed_cb (TerminalScreen  *screen,
                                                      int              n_press,
                                                      double           x,
                                                      double           y,
                                                      GtkGestureClick *click);
static void terminal_screen_long_pressed_cb (GtkGestureLongPress *gesture,
                                             double               x,
                                             double               y,
                                             TerminalScreen      *screen);
static void terminal_screen_bubble_click_pressed_cb (TerminalScreen  *screen,
                                                     int              n_press,
                                                     double           x,
                                                     double           y,
                                                     GtkGestureClick *click);
static void terminal_screen_child_exited  (VteTerminal *terminal,
                                           int status);

static void terminal_screen_window_title_changed      (VteTerminal *vte_terminal,
                                                       TerminalScreen *screen);

static void update_color_scheme                      (TerminalScreen *screen);

static void terminal_screen_check_extra (TerminalScreen *screen,
                                         double          x,
                                         double          y,
                                         char           **number_info,
                                         char           **timestamp_info);
static char* terminal_screen_check_match (TerminalScreen *screen,
                                          double          x,
                                          double          y,
                                          int            *flavor);

static void terminal_screen_show_info_bar (TerminalScreen *screen,
                                           GError *error,
                                           gboolean show_relaunch);


static char**terminal_screen_get_child_environment (TerminalScreen *screen,
                                                    char **initial_envv,
                                                    char **path,
                                                    char **shell);

static void terminal_screen_snapshot_quick_select (TerminalScreen *screen,
                                                   GtkSnapshot *snapshot);
static gboolean terminal_screen_quick_select_key (TerminalScreen *screen,
                                                  guint keyval,
                                                  GdkModifierType state);
static void terminal_screen_add_shell_integration (TerminalScreen* screen,
                                                   char** argv,
                                                   gboolean as_shell,
                                                   char*** exec_argv_p,
                                                   char*** envv_p);
static gboolean terminal_screen_get_child_command (TerminalScreen *screen,
                                                   char          **exec_argv,
                                                   const char     *path_env,
                                                   const char     *shell_env,
                                                   gboolean        shell,
                                                   gboolean       *preserve_cwd_p,
                                                   GSpawnFlags    *spawn_flags_p,
                                                   char         ***argv_p,
                                                   GError        **err);

static void terminal_screen_menu_popup_action (GtkWidget  *widget,
                                               const char *action_name,
                                               GVariant   *param);

static void terminal_screen_queue_idle_exec (TerminalScreen *screen);

static void _terminal_screen_update_scrollbar (TerminalScreen *screen);
static bool terminal_screen_validate_prompts (TerminalScreen *screen);

static void _terminal_screen_update_kinetic_scrolling (TerminalScreen *screen);

static void text_uri_list_free (TextUriList *uri_list);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (TextUriList, text_uri_list_free)

static guint signals[LAST_SIGNAL];

typedef struct {
  const char *pattern;
  TerminalURLFlavor flavor;
} TerminalRegexPattern;

static const TerminalRegexPattern url_regex_patterns[] = {
  { REGEX_URL_AS_IS, FLAVOR_AS_IS },
  { REGEX_URL_HTTP,  FLAVOR_DEFAULT_TO_HTTP },
  { REGEX_URL_FILE,  FLAVOR_AS_IS },
  { REGEX_URL_VOIP,  FLAVOR_VOIP_CALL },
  { REGEX_EMAIL,     FLAVOR_EMAIL },
  { REGEX_XMPP,      FLAVOR_AS_IS },
  { REGEX_NEWS_MAN,  FLAVOR_AS_IS },
};

static const TerminalRegexPattern extra_regex_patterns[] = {
  { "(0[Xx][[:xdigit:]]+|[[:digit:]]+)", FLAVOR_NUMBER },
};

static VteRegex **url_regexes;
static VteRegex **extra_regexes;
static TerminalURLFlavor *url_regex_flavors;
static TerminalURLFlavor *extra_regex_flavors;
static guint n_url_regexes;
static guint n_extra_regexes;

/* See bug #697024 */
#ifndef __linux__

#undef dup3
#define dup3 fake_dup3

static int
fake_dup3 (int fd, int fd2, int flags)
{
  if (dup2 (fd, fd2) == -1)
    return -1;

  return fcntl (fd2, F_SETFD, flags);
}
#endif /* !__linux__ */

static char*
strv_to_string (char **strv)
{
  return strv ? g_strjoinv (" ", strv) : g_strdup ("(null)");
}

static char*
exec_data_to_string (ExecData *data)
{
  gs_free char *str1 = nullptr;
  gs_free char *str2 = nullptr;
  return data ? g_strdup_printf ("data %p argv:[%s] exec-argv:[%s] envv:%p(%u) as-shell:%s cwd:%s",
                                 data,
                                 (str1 = strv_to_string (data->argv)),
                                 (str2 = strv_to_string (data->exec_argv)),
                                 data->envv, data->envv ? g_strv_length (data->envv) : 0,
                                 data->as_shell ? "true" : "false",
                                 data->cwd)
  : g_strdup ("(null)");
}

static ExecData*
exec_data_new (void)
{
  ExecData *data = g_new0 (ExecData, 1);
  data->refcount = 1;

  return data;
}

static ExecData *
exec_data_clone (ExecData *data)
{
  if (data == nullptr)
    return nullptr;

  ExecData *clone = exec_data_new ();
  clone->envv = g_strdupv (data->envv);
  clone->cwd = g_strdup (data->cwd);
  clone->cwd_requested = data->cwd_requested;

  /* If FDs were passed, cannot repeat argv. Return data only for env and cwd */
  if (data->fd_list != nullptr) {
    clone->as_shell = TRUE;
    return clone;
  }

  clone->argv = g_strdupv (data->argv);
  clone->as_shell = data->as_shell;

  return clone;
}

static void
exec_data_callback (ExecData *data,
                    GError *error,
                    TerminalScreen *screen)
{
  if (data->callback)
    data->callback (screen, error, data->callback_data);
}

static ExecData*
exec_data_ref (ExecData *data)
{
  data->refcount++;
  return data;
}

static void
exec_data_unref (ExecData *data)
{
  if (data == nullptr)
    return;

  if (--data->refcount > 0)
    return;

  g_strfreev (data->argv);
  g_strfreev (data->exec_argv);
  g_strfreev (data->envv);
  g_free (data->cwd);
  g_clear_object (&data->fd_list);
  g_free (data->fd_map);

  if (data->callback_data_destroy_notify && data->callback_data)
    data->callback_data_destroy_notify (data->callback_data);

  g_clear_object (&data->cancellable);

  g_free (data);
}

GS_DEFINE_CLEANUP_FUNCTION0(ExecData*, _terminal_local_unref_exec_data, exec_data_unref)
#define terminal_unref_exec_data __attribute__((__cleanup__(_terminal_local_unref_exec_data)))

static void
terminal_screen_clear_exec_data (TerminalScreen *screen,
                                 gboolean cancelled)
{
  TerminalScreenPrivate *priv = screen->priv;

  if (priv->exec_data == nullptr)
    return;

  if (cancelled) {
    gs_free_error GError *err = nullptr;
    g_set_error_literal (&err, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                         "Spawning was cancelled");
    exec_data_callback (priv->exec_data, err, screen);
  }

  exec_data_unref (priv->exec_data);
  priv->exec_data = nullptr;
}

G_DEFINE_TYPE_WITH_PRIVATE (TerminalScreen, terminal_screen, VTE_TYPE_TERMINAL)

static void
free_tag_data (TagData *tagdata)
{
  g_slice_free (TagData, tagdata);
}

static void
precompile_regexes (const TerminalRegexPattern *regex_patterns,
                    guint n_regexes,
                    VteRegex ***regexes,
                    TerminalURLFlavor **regex_flavors)
{
  guint i;

  *regexes = g_new0 (VteRegex*, n_regexes);
  *regex_flavors = g_new0 (TerminalURLFlavor, n_regexes);

  for (i = 0; i < n_regexes; ++i)
    {
      GError *error = nullptr;

      (*regexes)[i] = vte_regex_new_for_match (regex_patterns[i].pattern, -1,
                                               PCRE2_UTF | PCRE2_NO_UTF_CHECK | PCRE2_UCP | PCRE2_MULTILINE,
                                               &error);
      terminal_assert_no_error (error);

      if (!vte_regex_jit ((*regexes)[i], PCRE2_JIT_COMPLETE, &error) ||
          !vte_regex_jit ((*regexes)[i], PCRE2_JIT_PARTIAL_SOFT, &error)) {
        g_printerr ("Failed to JIT regex '%s': %s\n", regex_patterns[i].pattern, error->message);
        g_clear_error (&error);
      }

      (*regex_flavors)[i] = regex_patterns[i].flavor;
    }
}

static void
terminal_screen_enable_menu_bar_accel_notify_cb (GSettings *settings,
                                                 const char *key,
                                                 TerminalScreen *screen)
{
  g_assert (G_IS_SETTINGS (settings));
  g_assert (key != nullptr);
  g_assert (TERMINAL_IS_SCREEN (screen));

  gtk_widget_action_set_enabled (GTK_WIDGET (screen),
                                 "menu.popup",
                                 g_settings_get_boolean (settings, key));
}

static GdkTexture*
texture_from_surface(cairo_surface_t* surface)
{
  gs_unref_bytes auto bytes =
    g_bytes_new_with_free_func(cairo_image_surface_get_data(surface),
                               size_t(cairo_image_surface_get_height(surface)) *
                               size_t(cairo_image_surface_get_stride(surface)),
                               GDestroyNotify(cairo_surface_destroy),
                               cairo_surface_reference(surface));

  return gdk_memory_texture_new(cairo_image_surface_get_width(surface),
                                cairo_image_surface_get_height(surface),
                                GDK_MEMORY_DEFAULT,
                                bytes,
                                cairo_image_surface_get_stride(surface));
}

static void
terminal_screen_icon_color_changed_cb(TerminalScreen* screen,
                                      char const* prop,
                                      VteTerminal* terminal)
{
  auto const priv = screen->priv;

  g_clear_object(&priv->icon_color);

  auto color = GdkRGBA{};
  if (vte_terminal_get_termprop_rgba_by_id(terminal,
                                           VTE_PROPERTY_ID_ICON_COLOR,
                                           &color)) {
    auto const scale = gtk_widget_get_scale_factor(GTK_WIDGET(screen));
    auto const w = 32 * scale, h = 32 * scale;
    auto const xc = w / 2, yc = h / 2;
    auto const radius = w / 2 - 1;

    auto surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    auto cr = cairo_create(surface);
    cairo_set_source_rgb(cr, color.red, color.green, color.blue);
    cairo_new_sub_path(cr);
    cairo_arc(cr, xc, yc, radius, 0., G_PI * 2);
    cairo_close_path(cr);
    cairo_fill(cr);

    cairo_destroy(cr);

    priv->icon_color = G_ICON(texture_from_surface(surface));
    cairo_surface_destroy(surface);
  }

  g_object_notify_by_pspec(G_OBJECT(terminal), pspecs[PROP_ICON]);
}

static void
terminal_screen_icon_image_changed_cb(TerminalScreen* screen,
                                      char const* prop,
                                      VteTerminal* terminal)
{
  auto const priv = screen->priv;

  g_clear_object(&priv->icon_image);
  priv->icon_image =
    G_ICON(vte_terminal_ref_termprop_image_texture_by_id(terminal,
                                                         VTE_PROPERTY_ID_ICON_IMAGE));

  g_object_notify_by_pspec(G_OBJECT(terminal), pspecs[PROP_ICON]);
}

static GIcon*
terminal_screen_ensure_icon_progress(TerminalScreen* screen)
{
  auto const priv = screen->priv;

  if (priv->icon_progress_set)
    return priv->icon_progress;

  if (priv->has_progress) {
    GIcon* icon = nullptr;

    switch (priv->progress_hint) {
    case VTE_PROGRESS_HINT_ERROR:
      icon = g_themed_icon_new("dialog-error-symbolic");
      break;

    case VTE_PROGRESS_HINT_INDETERMINATE:
      icon = nullptr;
      break;

    case VTE_PROGRESS_HINT_PAUSED:
    case VTE_PROGRESS_HINT_ACTIVE: {
      auto const scale = gtk_widget_get_scale_factor(GTK_WIDGET(screen));
      auto const w = 16 * scale, h = 16 * scale;
      auto const xc = w / 2, yc = h / 2;
      auto const radius = w / 2 - 1;

      auto color = GdkRGBA{};
      G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
      auto style_context = gtk_widget_get_style_context(GTK_WIDGET(screen));
      gtk_style_context_get_color(style_context, &color);
      G_GNUC_END_IGNORE_DEPRECATIONS;

      auto surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
      auto cr = cairo_create(surface);

      // First draw a shadow filled circle
      cairo_set_source_rgba(cr, color.red, color.green, color.blue, 0.25);
      cairo_arc(cr, xc, yc, radius, 0., 2 * G_PI);
      cairo_close_path(cr);
      cairo_fill(cr);

      // Now draw progress filled circle
      auto const fraction = priv->progress_fraction;
      if (fraction > 0.) {
        cairo_set_line_width(cr, 1.);
        cairo_set_source_rgb(cr, color.red, color.green, color.blue);
        cairo_new_sub_path(cr);

        if (fraction < 1.) {
          cairo_move_to(cr, xc, yc);
          cairo_line_to(cr, xc, yc - radius);
          cairo_arc(cr, xc, yc, radius, - G_PI / 2.0, 2.0 * G_PI * (fraction - 0.25));
          cairo_line_to(cr, xc, yc);
        } else {
          cairo_arc(cr, xc, yc, radius, 0, 2 * G_PI);
        }

        cairo_close_path(cr);
        cairo_fill(cr);
      }
      cairo_destroy(cr);

      icon = G_ICON(texture_from_surface(surface));
      cairo_surface_destroy(surface);
      break;
    }

    case VTE_PROGRESS_HINT_INACTIVE:
    default:
      icon = nullptr;
      break;
    }

    g_set_object(&priv->icon_progress, icon);

  } else {
    // Remove progress
    g_clear_object(&priv->icon_progress);
  }

  priv->icon_progress_set = true;

  return priv->icon_progress;
}

static void
terminal_screen_clear_icon_progress(TerminalScreen* screen)
{
  auto const priv = screen->priv;

  g_clear_object(&priv->icon_progress);
  priv->icon_progress_set = false;

  g_object_notify_by_pspec(G_OBJECT(screen), pspecs[PROP_ICON_PROGRESS]);
}

static void
terminal_screen_progress_value_changed_cb(TerminalScreen* screen,
                                          char const* prop,
                                          VteTerminal* terminal)
{
  auto const priv = screen->priv;

  auto fraction = 0.;
  uint64_t value = 0;
  auto const has_progress = vte_terminal_get_termprop_uint_by_id(terminal,
                                                                 VTE_PROPERTY_ID_PROGRESS_VALUE,
                                                                 &value);
  if (has_progress)
    fraction = std::clamp(double(value) / 100., 0., 1.);
  else
    fraction = 0.;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfloat-equal"
  if (priv->has_progress == has_progress &&
      priv->progress_fraction == fraction)
    return;
#pragma GCC diagnostic pop

  priv->has_progress = has_progress;
  priv->progress_fraction = fraction;

  terminal_screen_clear_icon_progress(screen);
}

static gboolean
terminal_screen_capture_command_cb(void* user_data)
{
  auto const screen = TERMINAL_SCREEN(user_data);
  auto const priv = screen->priv;

  priv->command_capture_source = 0;

  gs_free char* cmdline = nullptr;
  if (terminal_screen_has_foreground_process(screen, nullptr, &cmdline) && cmdline) {
    g_free(priv->command);
    priv->command = g_steal_pointer(&cmdline);
  }

  return G_SOURCE_REMOVE;
}

/* Drops prompt marks that are no longer valid: when the column count
 * changed, VTE has rewrapped the lines and the rows moved. Normally
 * terminal_screen_reanchor_prompts() moves them along; this is for the
 * changes it did not see. Returns whether marks were dropped.
 */
static bool
terminal_screen_validate_prompts(TerminalScreen* screen)
{
  auto const priv = screen->priv;
  auto const columns = vte_terminal_get_column_count(VTE_TERMINAL(screen));

  if (columns == priv->prompts_columns)
    return false;

  priv->prompts_columns = columns;
  if (priv->prompts->len == 0)
    return false;

  g_array_set_size(priv->prompts, 0);
  return true;
}

static GtkAdjustment*
terminal_screen_get_vadjustment(TerminalScreen* screen)
{
  return gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(screen));
}

/* The scroll adjustment counts pixels (see vte_terminal_set_scroll_unit_is_pixels)
 * or rows; these convert between its values and absolute rows. */
static double
terminal_screen_scroll_unit(TerminalScreen* screen)
{
  auto const terminal = VTE_TERMINAL(screen);
  if (!vte_terminal_get_scroll_unit_is_pixels(terminal))
    return 1.0;

  return double(MAX(vte_terminal_get_char_height(terminal), 1));
}

/* VTE fills the scroll adjustment relative to the first row it keeps
 * (Terminal::scroll_limit_lower()), which is not public, while the
 * cursor position and text ranges use absolute rows. This finds the
 * absolute row of adjustment value 0.
 *
 * The cursor is on the bottom screen, so the origin is one of row-count
 * candidates; the right one is where the text of the displayed rows
 * matches the text of the same absolute rows. The last result is tried
 * first, and a cursor on the last row is preferred.
 */
static long
terminal_screen_get_row_origin(TerminalScreen* screen)
{
  auto const priv = screen->priv;
  auto const terminal = VTE_TERMINAL(screen);
  auto const adjustment = terminal_screen_get_vadjustment(screen);
  auto const unit = terminal_screen_scroll_unit(screen);
  auto const rows = vte_terminal_get_row_count(terminal);
  auto const span = long(gtk_adjustment_get_upper(adjustment) / unit + 0.5);
  auto const top = long(gtk_adjustment_get_value(adjustment) / unit);

  long cursor_row = 0;
  vte_terminal_get_cursor_position(terminal, nullptr, &cursor_row);

  gs_free char* displayed = vte_terminal_get_text_format(terminal, VTE_FORMAT_TEXT);
  auto matches = [&](long origin) -> bool {
    if (origin < 0)
      return false;
    gs_free char* text = vte_terminal_get_text_range_format(terminal, VTE_FORMAT_TEXT,
                                                            origin + top, 0,
                                                            origin + top + rows, 0,
                                                            nullptr);
    return g_strcmp0(text, displayed) == 0;
  };

  if (priv->row_origin_valid && matches(priv->row_origin))
    return priv->row_origin;

  for (auto k = rows - 1; k >= 0; --k) {
    auto const origin = cursor_row - k + rows - span;
    if (matches(origin)) {
      priv->row_origin = origin;
      priv->row_origin_valid = true;
      return origin;
    }
  }

  // No candidate fits; assume the cursor is on the last row
  return MAX(0l, cursor_row + 1 - span);
}

static long
terminal_screen_adjustment_to_row(TerminalScreen* screen,
                                  double value)
{
  return terminal_screen_get_row_origin(screen) +
    long(value / terminal_screen_scroll_unit(screen) + 0.5);
}

static double
terminal_screen_row_to_adjustment(TerminalScreen* screen,
                                  long row)
{
  return double(row - terminal_screen_get_row_origin(screen)) * terminal_screen_scroll_unit(screen);
}

static long
count_newlines(char const* text)
{
  long n = 0;
  for (auto p = text; p && *p; ++p)
    n += *p == '\n';
  return n;
}

/* Before VTE gets another width, and rewraps the lines: where the prompt
 * marks are in the text, see terminal_screen_reanchor_prompts(). */
static void
terminal_screen_anchor_prompts(TerminalScreen* screen)
{
  auto const priv = screen->priv;
  auto const terminal = VTE_TERMINAL(screen);

  g_clear_pointer(&priv->prompt_anchors, g_array_unref);
  if (priv->prompts->len == 0 ||
      vte_terminal_get_column_count(terminal) != priv->prompts_columns)
    return;

  long cursor_row = 0, cursor_col = 0;
  vte_terminal_get_cursor_position(terminal, &cursor_col, &cursor_row);

  auto const n = priv->prompts->len;
  priv->prompt_anchors = g_array_sized_new(false, false, sizeof(PromptAnchor), n);
  g_array_set_size(priv->prompt_anchors, n);

  // From the bottom up, one piece of text between two marks at a time
  long newlines = 0;
  auto end_row = cursor_row, end_col = cursor_col;
  auto known = true;
  for (auto i = n; i > 0; --i) {
    auto const& mark = g_array_index(priv->prompts, PromptMark, i - 1);
    auto& anchor = g_array_index(priv->prompt_anchors, PromptAnchor, i - 1);
    known = known && mark.row <= end_row && cursor_row - mark.row <= PROMPT_ANCHOR_ROWS;
    if (!known) {
      anchor.newlines = -1;
      continue;
    }

    gs_free char* text = vte_terminal_get_text_range_format(terminal, VTE_FORMAT_TEXT,
                                                           mark.row, 0, end_row, end_col,
                                                           nullptr);
    newlines += count_newlines(text);
    anchor.newlines = newlines;
    end_row = mark.row;
    end_col = 0;
  }
}

/* The line breaks between the start of @row and the end of @end_row */
static long
terminal_screen_count_newlines(TerminalScreen* screen,
                               long row,
                               long end_row)
{
  gs_free char* text = vte_terminal_get_text_range_format(VTE_TERMINAL(screen), VTE_FORMAT_TEXT,
                                                         row, 0, end_row + 1, 0,
                                                         nullptr);
  return count_newlines(text);
}

/* After VTE rewrapped the lines: finds the new rows of the prompt marks
 * from their anchors. A mark goes to the first row of the line that has
 * as many line breaks below it as before. Returns false if there were no
 * anchors for the new width, and the marks were dropped. */
static bool
terminal_screen_reanchor_prompts(TerminalScreen* screen,
                                 long old_columns)
{
  auto const priv = screen->priv;
  auto const terminal = VTE_TERMINAL(screen);
  gs_unref_array GArray* anchors = g_steal_pointer(&priv->prompt_anchors);

  if (!anchors || anchors->len != priv->prompts->len ||
      old_columns != priv->prompts_columns)
    return false;

  priv->prompts_columns = vte_terminal_get_column_count(terminal);

  long cursor_row = 0, cursor_col = 0;
  vte_terminal_get_cursor_position(terminal, &cursor_col, &cursor_row);
  auto const first_row = terminal_screen_adjustment_to_row(screen,
                                                           gtk_adjustment_get_lower(terminal_screen_get_vadjustment(screen)));

  /* Walk up from the cursor. @newlines counts the line breaks between the
   * start of @row and the cursor; whole chunks of rows are skipped while
   * the next mark is further up. */
  auto row = cursor_row;
  long newlines = 0;
  auto const old_command_row = priv->command_mark_row;
  priv->command_mark_row = -1;
  guint n_dropped = 0;

  for (auto i = priv->prompts->len; i > 0; --i) {
    auto& mark = g_array_index(priv->prompts, PromptMark, i - 1);
    auto const target = g_array_index(anchors, PromptAnchor, i - 1).newlines;
    auto const old_row = mark.row;
    if (target < 0 || target < newlines) {
      n_dropped = i;
      break;
    }

    // Skip chunks that end below the mark's line
    long constexpr chunk = 256;
    while (row - chunk >= first_row) {
      auto const n = terminal_screen_count_newlines(screen, row - chunk, row - 1);
      if (newlines + n > target)
        break;
      newlines += n;
      row -= chunk;
    }

    // Then row by row, to the first row of the line
    while (row > first_row) {
      auto const n = terminal_screen_count_newlines(screen, row - 1, row - 1);
      if (newlines + n > target)
        break;
      newlines += n;
      --row;
    }

    if (newlines != target) {
      n_dropped = i;
      break;
    }

    mark.row = row;
    if (old_row == old_command_row)
      priv->command_mark_row = row;
  }

  // The marks further up, which could not be placed, go
  if (n_dropped > 0)
    g_array_remove_range(priv->prompts, 0, n_dropped);
  return true;
}

/* Root and remote sessions */

static TerminalProcessKind
terminal_screen_find_process_kind(TerminalScreen* screen)
{
  auto const priv = screen->priv;

  if (priv->child_pid <= 0)
    return TERMINAL_PROCESS_KIND_NORMAL;

  auto const settings = terminal_app_get_global_settings(terminal_app_get());
  if (!g_settings_get_boolean(settings, TERMINAL_SETTING_HIGHLIGHT_ROOT_AND_REMOTE_KEY))
    return TERMINAL_PROCESS_KIND_NORMAL;

  static char const* const remote_programs[] = {
    "ssh", "mosh-client", "autossh", "sshpass", "et", "telnet", nullptr
  };

  // The sandbox does not see the processes on the host; ask there
  if (terminal_flatpak_is_sandboxed()) {
    auto const info = priv->host_process;
    if (!info)
      return TERMINAL_PROCESS_KIND_NORMAL;
    if (info->foreground_euid == 0)
      return TERMINAL_PROCESS_KIND_ROOT;
    if (info->foreground_comm && g_strv_contains(remote_programs, info->foreground_comm))
      return TERMINAL_PROCESS_KIND_REMOTE;
    return TERMINAL_PROCESS_KIND_NORMAL;
  }

  auto const pty = vte_terminal_get_pty(VTE_TERMINAL(screen));
  auto pid = pty ? int(tcgetpgrp(vte_pty_get_fd(pty))) : -1;
  if (pid <= 0)
    pid = priv->child_pid;

  // The effective user of the foreground program, from Linux's /proc
  gs_free char* status_path = g_strdup_printf("/proc/%d/status", pid);
  gs_free char* status = nullptr;
  if (g_file_get_contents(status_path, &status, nullptr, nullptr)) {
    if (auto const uid_line = strstr(status, "\nUid:")) {
      unsigned real_uid, effective_uid;
      if (sscanf(uid_line + 5, "%u %u", &real_uid, &effective_uid) == 2 &&
          effective_uid == 0)
        return TERMINAL_PROCESS_KIND_ROOT;
    }
  }

  gs_free char* comm_path = g_strdup_printf("/proc/%d/comm", pid);
  gs_free char* comm = nullptr;
  if (g_file_get_contents(comm_path, &comm, nullptr, nullptr)) {
    g_strstrip(comm);
    if (g_strv_contains(remote_programs, comm))
      return TERMINAL_PROCESS_KIND_REMOTE;
  }

  return TERMINAL_PROCESS_KIND_NORMAL;
}

static void terminal_screen_update_directory_title(TerminalScreen* screen);
static void terminal_screen_ignore_program_titles_changed_cb(TerminalScreen* screen);
static void terminal_screen_cwd_changed_cb(TerminalScreen* screen,
                                           const char* prop,
                                           void* user_data);

static void terminal_screen_query_host_process(TerminalScreen* screen);

static void
terminal_screen_apply_process_kind(TerminalScreen* screen)
{
  auto const priv = screen->priv;

  // Without shell integration, the working directory has no signal either
  terminal_screen_update_directory_title(screen);

  auto const kind = terminal_screen_find_process_kind(screen);
  if (kind == priv->process_kind)
    return;

  priv->process_kind = kind;
  g_object_notify_by_pspec(G_OBJECT(screen), pspecs[PROP_PROCESS_KIND]);
  // The icon falls back to one for the kind
  g_object_notify_by_pspec(G_OBJECT(screen), pspecs[PROP_ICON]);
}

static void
terminal_screen_host_process_done_cb(GObject* source,
                                     GAsyncResult* result,
                                     void* user_data)
{
  gs_free_error GError* error = nullptr;
  auto const info = terminal_flatpak_query_process_finish(result, &error);
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;

  auto const screen = TERMINAL_SCREEN(user_data);
  auto const priv = screen->priv;
  g_clear_object(&priv->host_process_cancellable);
  g_clear_pointer(&priv->host_process, terminal_flatpak_process_info_free);
  if (priv->child_pid > 0)
    priv->host_process = info;
  else
    terminal_flatpak_process_info_free(info);

  terminal_screen_apply_process_kind(screen);

  if (priv->host_process_query_again) {
    priv->host_process_query_again = false;
    terminal_screen_query_host_process(screen);
  }
}

static void
terminal_screen_query_host_process(TerminalScreen* screen)
{
  auto const priv = screen->priv;
  if (priv->host_process_cancellable) {
    priv->host_process_query_again = true;
    return;
  }

  // The callback holds no reference; disposing cancels the query
  priv->host_process_cancellable = g_cancellable_new();
  terminal_flatpak_query_process_async(priv->child_pid,
                                       priv->host_process_cancellable,
                                       terminal_screen_host_process_done_cb,
                                       screen);
}

static void
terminal_screen_update_process_kind(TerminalScreen* screen)
{
  auto const priv = screen->priv;

  if (terminal_flatpak_is_sandboxed()) {
    if (priv->child_pid > 0) {
      terminal_screen_query_host_process(screen);
      return;
    }
    g_clear_pointer(&priv->host_process, terminal_flatpak_process_info_free);
  }

  terminal_screen_apply_process_kind(screen);
}

static gboolean
terminal_screen_process_kind_timeout_cb(void* user_data)
{
  auto const screen = TERMINAL_SCREEN(user_data);

  screen->priv->process_kind_source = 0;
  terminal_screen_update_process_kind(screen);
  return G_SOURCE_REMOVE;
}

static void
terminal_screen_queue_process_kind_update(TerminalScreen* screen)
{
  auto const priv = screen->priv;
  if (priv->process_kind_source == 0)
    priv->process_kind_source = g_timeout_add(300,
                                              terminal_screen_process_kind_timeout_cb,
                                              screen);
}

/**
 * terminal_screen_get_process_kind:
 * @screen: a #TerminalScreen
 *
 * Returns: whether the foreground program runs as root or connects to
 *   another computer, as far as Pano Terminal can tell
 */
TerminalProcessKind
terminal_screen_get_process_kind(TerminalScreen* screen)
{
  g_return_val_if_fail(TERMINAL_IS_SCREEN(screen), TERMINAL_PROCESS_KIND_NORMAL);

  return screen->priv->process_kind;
}

static gboolean
terminal_screen_key_event_done_cb(void* user_data)
{
  auto const priv = TERMINAL_SCREEN(user_data)->priv;

  priv->in_key_event = false;
  priv->key_event_source = 0;
  return G_SOURCE_REMOVE;
}

/* Shows the pointer cursor over links only while Ctrl, which opens them,
 * is held */
static void
terminal_screen_set_link_cursor(TerminalScreen* screen,
                                bool ctrl)
{
  if (ctrl == bool(screen->priv->link_cursor))
    return;

  screen->priv->link_cursor = ctrl;
  for (auto l = screen->priv->match_tags; l; l = l->next) {
    auto const tag_data = reinterpret_cast<TagData*>(l->data);
    vte_terminal_match_set_cursor_name(VTE_TERMINAL(screen), tag_data->tag,
                                       ctrl ? URL_MATCH_CURSOR_NAME : URL_MATCH_IDLE_CURSOR_NAME);
  }

  /* VTE only sets the cursor when the pointer moves onto a link, so set it
   * for the link under the pointer */
  auto const priv = screen->priv;
  if (priv->pointer_x >= 0) {
    int tag;
    gs_free char *match = vte_terminal_check_match_at(VTE_TERMINAL(screen),
                                                      priv->pointer_x, priv->pointer_y, &tag);
    if (match)
      gtk_widget_set_cursor_from_name(GTK_WIDGET(screen),
                                      ctrl ? URL_MATCH_CURSOR_NAME : URL_MATCH_IDLE_CURSOR_NAME);
  }
}

static bool
is_ctrl_key(guint keyval)
{
  return keyval == GDK_KEY_Control_L || keyval == GDK_KEY_Control_R;
}

/* The state of a key event is the one from before it, so the press and
 * release of Ctrl itself are told by its key; see the key handlers */
static gboolean
terminal_screen_modifiers_cb(GtkEventControllerKey* controller,
                             GdkModifierType state,
                             TerminalScreen* screen)
{
  terminal_screen_set_link_cursor(screen, (state & GDK_CONTROL_MASK) != 0);
  return FALSE;
}

static void
terminal_screen_key_released_cb(GtkEventControllerKey* controller,
                                guint keyval,
                                guint keycode,
                                GdkModifierType state,
                                TerminalScreen* screen)
{
  if (is_ctrl_key(keyval))
    terminal_screen_set_link_cursor(screen, false);
}

static void
terminal_screen_pointer_motion_cb(GtkEventControllerMotion* controller,
                                  double x,
                                  double y,
                                  TerminalScreen* screen)
{
  screen->priv->pointer_x = x;
  screen->priv->pointer_y = y;
}

static void
terminal_screen_pointer_leave_cb(GtkEventControllerMotion* controller,
                                 TerminalScreen* screen)
{
  screen->priv->pointer_x = screen->priv->pointer_y = -1;
}

/* Marks the time until the main loop runs again as handling a key press */
static gboolean
terminal_screen_key_pressed_cb(GtkEventControllerKey* controller,
                               guint keyval,
                               guint keycode,
                               GdkModifierType state,
                               TerminalScreen* screen)
{
  auto const priv = screen->priv;

  if (is_ctrl_key(keyval))
    terminal_screen_set_link_cursor(screen, true);

  if (priv->quick_select)
    return terminal_screen_quick_select_key (screen, keyval, state);

  priv->in_key_event = true;
  if (priv->key_event_source == 0)
    priv->key_event_source = g_idle_add_full(G_PRIORITY_HIGH,
                                             terminal_screen_key_event_done_cb,
                                             screen, nullptr);
  return GDK_EVENT_PROPAGATE;
}

static gboolean
terminal_screen_precmd_batch_done_cb(void* user_data)
{
  auto const priv = TERMINAL_SCREEN(user_data)->priv;

  priv->precmd_in_batch = false;
  priv->precmd_batch_source = 0;
  return G_SOURCE_REMOVE;
}

/* The shell is about to print a new prompt */
static void
terminal_screen_shell_precmd_cb(TerminalScreen* screen,
                                char const* prop,
                                VteTerminal* terminal)
{
  auto const priv = screen->priv;

  terminal_screen_validate_prompts(screen);

  long row = 0;
  vte_terminal_get_cursor_position(terminal, nullptr, &row);

  // After a reset or a clear, rows can go back; forget marks at or after
  // the new prompt, and those that left the scrollback.
  auto const first_row = terminal_screen_adjustment_to_row(screen,
                                                           gtk_adjustment_get_lower(terminal_screen_get_vadjustment(screen)));
  auto n_valid = 0u;
  for (auto i = 0u; i < priv->prompts->len; ++i) {
    auto const& mark = g_array_index(priv->prompts, PromptMark, i);
    if (mark.row >= first_row && mark.row < row)
      g_array_index(priv->prompts, PromptMark, n_valid++) = mark;
  }
  g_array_set_size(priv->prompts, n_valid);

  if (priv->prompts->len >= MAX_PROMPTS)
    g_array_remove_range(priv->prompts, 0, priv->prompts->len - MAX_PROMPTS + 1);

  auto const mark = PromptMark{row, false, -1};
  g_array_append_val(priv->prompts, mark);

  // Until the main loop runs again, we are in the same batch
  priv->precmd_in_batch = true;
  if (priv->precmd_batch_source == 0)
    priv->precmd_batch_source = g_idle_add_full(G_PRIORITY_HIGH,
                                                terminal_screen_precmd_batch_done_cb,
                                                screen, nullptr);

  g_signal_emit(screen, signals[PROMPTS_CHANGED], 0);
}

/* The shell is about to run the command entered at the prompt */
static void
terminal_screen_shell_preexec_cb(TerminalScreen* screen,
                                 char const* prop,
                                 VteTerminal* terminal)
{
  auto const priv = screen->priv;

  // The command was entered at the last prompt, or at the one before if
  // the next prompt was reported in the same batch.
  auto const index = int(priv->prompts->len) - (priv->precmd_in_batch ? 2 : 1);
  priv->command_mark_row = -1;
  if (index >= 0) {
    auto& mark = g_array_index(priv->prompts, PromptMark, index);
    mark.ran_command = true;
    // Its exit status comes with postexec, maybe after the next prompt
    priv->command_mark_row = mark.row;
    g_signal_emit(screen, signals[PROMPTS_CHANGED], 0);
  }

  priv->command_start = g_get_monotonic_time();
  g_clear_pointer(&priv->command, g_free);

  // The shell has not started the command yet; look a bit later. Commands
  // that end before do not run long enough to matter.
  g_clear_handle_id(&priv->command_capture_source, g_source_remove);
  priv->command_capture_source = g_timeout_add(500,
                                               terminal_screen_capture_command_cb,
                                               screen);
}

/* The command has returned; the termprop value is its exit status */
static void
terminal_screen_shell_postexec_cb(TerminalScreen* screen,
                                  char const* prop,
                                  VteTerminal* terminal)
{
  auto const priv = screen->priv;

  if (priv->command_start == 0)
    return;

  g_clear_handle_id(&priv->command_capture_source, g_source_remove);

  uint64_t status = 0;
  vte_terminal_get_termprop_uint(terminal, VTE_TERMPROP_SHELL_POSTEXEC, &status);

  for (auto i = priv->prompts->len; i > 0; --i) {
    auto& mark = g_array_index(priv->prompts, PromptMark, i - 1);
    if (mark.row == priv->command_mark_row) {
      mark.status = int(MIN(status, guint64(G_MAXINT)));
      g_signal_emit(screen, signals[PROMPTS_CHANGED], 0);
      break;
    }
  }
  priv->command_mark_row = -1;

  auto const duration_ms = guint64(g_get_monotonic_time() - priv->command_start) / 1000;
  priv->command_start = 0;

  gs_free char* command = g_steal_pointer(&priv->command);
  g_signal_emit(screen, signals[COMMAND_FINISHED], 0,
                command, guint(status), duration_ms);
}

static void
terminal_screen_progress_hint_changed_cb(TerminalScreen* screen,
                                         char const* prop,
                                         VteTerminal* terminal)
{
  auto const priv = screen->priv;

  VteProgressHint hint;
  int64_t value = 0;
  if (vte_terminal_get_termprop_int_by_id(terminal,
                                          VTE_PROPERTY_ID_PROGRESS_HINT,
                                          &value))
    hint = VteProgressHint(value);
  else
    hint = VTE_PROGRESS_HINT_INACTIVE;

  if (priv->progress_hint == hint)
    return;

  priv->progress_hint = hint;

  terminal_screen_clear_icon_progress(screen);
}

static TerminalWindow *
terminal_screen_get_window (TerminalScreen *screen)
{
  GtkWidget *widget = GTK_WIDGET (screen);
  GtkRoot *toplevel = gtk_widget_get_root (widget);

  return TERMINAL_WINDOW (toplevel);
}

static void
terminal_screen_realize (GtkWidget *widget)
{
  TerminalScreen *screen = TERMINAL_SCREEN (widget);

  GTK_WIDGET_CLASS (terminal_screen_parent_class)->realize (widget);

  terminal_screen_set_font (screen);

  TerminalScreenPrivate *priv = screen->priv;
  if (priv->exec_on_realize)
    terminal_screen_queue_idle_exec (screen);

  priv->exec_on_realize = FALSE;

}

static void
terminal_screen_update_style (TerminalScreen *screen)
{
  update_color_scheme (screen);
  terminal_screen_set_font (screen);
}

static void
terminal_screen_css_changed (GtkWidget *widget,
                             GtkCssStyleChange *change)
{
  TerminalScreen *screen = TERMINAL_SCREEN (widget);

  GTK_WIDGET_CLASS (terminal_screen_parent_class)->css_changed (widget, change);

  terminal_screen_update_style (screen);
}

static void
terminal_screen_root (GtkWidget *widget)
{
  TerminalScreen *screen = TERMINAL_SCREEN (widget);
  TerminalScreenPrivate *priv = screen->priv;

  GTK_WIDGET_CLASS (terminal_screen_parent_class)->root (widget);

  if (priv->profile != nullptr)
    terminal_screen_profile_changed_cb (priv->profile, nullptr, screen);
}

static void
terminal_screen_measure (GtkWidget      *widget,
                         GtkOrientation  orientation,
                         int             for_size,
                         int            *minimum,
                         int            *natural,
                         int            *minimum_baseline,
                         int            *natural_baseline)
{
  TerminalScreen *screen = TERMINAL_SCREEN (widget);
  TerminalScreenPrivate *priv = screen->priv;
  int min_revealer;
  int nat_revealer;

  GTK_WIDGET_CLASS (terminal_screen_parent_class)->measure (widget,
                                                            orientation,
                                                            for_size,
                                                            minimum, natural,
                                                            minimum_baseline, natural_baseline);

  gtk_widget_measure (GTK_WIDGET (priv->size_revealer),
                      orientation, for_size,
                      &min_revealer, &nat_revealer, nullptr, nullptr);

  *minimum = MAX (*minimum, min_revealer);
  *natural = MAX (*natural, nat_revealer);
}

static gboolean
dismiss_size_label_cb (gpointer user_data)
{
  TerminalScreen *screen = TERMINAL_SCREEN (user_data);
  TerminalScreenPrivate *priv = screen->priv;

  gtk_revealer_set_reveal_child (priv->size_revealer, FALSE);
  priv->size_dismiss_source = 0;

  return G_SOURCE_REMOVE;
}

static void
terminal_screen_size_allocate (GtkWidget *widget,
                               int        width,
                               int        height,
                               int        baseline)
{
  TerminalScreen *screen = TERMINAL_SCREEN (widget);
  TerminalScreenPrivate *priv = screen->priv;
  GtkRequisition min;
  GtkAllocation revealer_alloc;
  GtkRoot *root;
  int prev_column_count, column_count;
  int prev_row_count, row_count;

  prev_column_count = vte_terminal_get_column_count (VTE_TERMINAL (screen));
  prev_row_count = vte_terminal_get_row_count (VTE_TERMINAL (screen));

  /* Another width or font may change the column count, and VTE rewraps the
   * lines then; note where the prompt marks are in the text */
  auto const char_width = int (vte_terminal_get_char_width (VTE_TERMINAL (screen)));
  if (width != priv->allocated_width || char_width != priv->allocated_char_width) {
    priv->allocated_width = width;
    priv->allocated_char_width = char_width;
    terminal_screen_anchor_prompts (screen);
  }

  GTK_WIDGET_CLASS (terminal_screen_parent_class)->size_allocate (widget, width, height, baseline);

  column_count = vte_terminal_get_column_count (VTE_TERMINAL (screen));
  row_count = vte_terminal_get_row_count (VTE_TERMINAL (screen));

  /* Rewrapped lines move the prompt rows, and invalidate search matches */
  if (column_count != prev_column_count) {
    if (terminal_screen_reanchor_prompts (screen, prev_column_count) ||
        terminal_screen_validate_prompts (screen))
      g_signal_emit (screen, signals[PROMPTS_CHANGED], 0);
    terminal_screen_stop_quick_select (screen);
    g_signal_emit (screen, signals[COLUMNS_CHANGED], 0);
  } else {
    // The text goes on; anchors are only good for this allocation
    g_clear_pointer (&priv->prompt_anchors, g_array_unref);
  }

  root = gtk_widget_get_root (widget);

  if (terminal_screen_is_active (screen) &&
      GTK_IS_WINDOW (root) &&
      !gtk_window_is_maximized (GTK_WINDOW (root)) &&
      !gtk_window_is_fullscreen (GTK_WINDOW (root)) &&
      (prev_column_count != column_count || prev_row_count != row_count)) {
    char format[32];

    g_snprintf (format, sizeof format, "%ld × %ld",
                vte_terminal_get_column_count (VTE_TERMINAL (screen)),
                vte_terminal_get_row_count (VTE_TERMINAL (screen)));
    gtk_label_set_label (priv->size_label, format);

    gtk_revealer_set_reveal_child (priv->size_revealer, TRUE);

    g_clear_handle_id (&priv->size_dismiss_source, g_source_remove);
    priv->size_dismiss_source = g_timeout_add (SIZE_DISMISS_TIMEOUT_MSEC,
                                               dismiss_size_label_cb,
                                               screen);
  } else if (gtk_window_is_maximized (GTK_WINDOW (root)) ||
             gtk_window_is_fullscreen (GTK_WINDOW (root)) ||
             terminal_window_in_fullscreen_transition (TERMINAL_WINDOW (root))) {
    g_clear_handle_id (&priv->size_dismiss_source, g_source_remove);
    gtk_revealer_set_reveal_child (priv->size_revealer, FALSE);
  }

  gtk_widget_get_preferred_size (GTK_WIDGET (priv->size_revealer), &min, nullptr);
  revealer_alloc.x = width - min.width;
  revealer_alloc.y = height - min.height;
  revealer_alloc.width = min.width;
  revealer_alloc.height = min.height;
  gtk_widget_size_allocate (GTK_WIDGET (priv->size_revealer), &revealer_alloc, -1);

  gtk_widget_get_preferred_size (GTK_WIDGET (priv->drop_highlight), &min, nullptr);
  gtk_widget_allocate (priv->drop_highlight, width, height, baseline, nullptr);
}

/* Draws the search matches over the text: all of them faintly, the current
 * one stronger. */
static void
terminal_screen_snapshot_search_matches (TerminalScreen *screen,
                                         GtkSnapshot *snapshot)
{
  auto const priv = screen->priv;
  auto const terminal = VTE_TERMINAL (screen);

  if (!priv->search_matches || priv->search_matches->len == 0 ||
      priv->search_columns != vte_terminal_get_column_count (terminal))
    return;

  auto const cell_width = double (vte_terminal_get_char_width (terminal));
  auto const cell_height = double (vte_terminal_get_char_height (terminal));
  if (cell_width <= 0 || cell_height <= 0)
    return;

  GtkBorder padding;
  gtk_style_context_get_padding (gtk_widget_get_style_context (GTK_WIDGET (screen)), &padding);

  // The adjustment counts pixels or rows, from the row origin
  auto const adjustment = gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (screen));
  auto const origin = terminal_screen_get_row_origin (screen);
  auto const top_px = (vte_terminal_get_scroll_unit_is_pixels (terminal)
                       ? gtk_adjustment_get_value (adjustment)
                       : gtk_adjustment_get_value (adjustment) * cell_height) + origin * cell_height;
  auto const first_row = long (top_px / cell_height);
  auto const last_row = first_row + vte_terminal_get_row_count (terminal) + 1;
  auto const columns = priv->search_columns;

  static GdkRGBA const match_color = {1.0f, 0.8f, 0.0f, 0.35f};
  static GdkRGBA const current_color = {1.0f, 0.45f, 0.0f, 0.6f};

  for (auto i = 0u; i < priv->search_matches->len; ++i) {
    auto const& match = g_array_index (priv->search_matches, TerminalSearchMatch, i);
    if (match.end_row < first_row || match.start_row > last_row)
      continue;

    auto const color = int (i) == priv->search_current ? &current_color : &match_color;
    for (auto row = MAX (match.start_row, first_row); row <= MIN (match.end_row, last_row); ++row) {
      auto const start_col = row == match.start_row ? match.start_col : 0;
      auto const end_col = row == match.end_row ? match.end_col : columns;
      if (end_col <= start_col)
        continue;

      auto const rect = GRAPHENE_RECT_INIT (float (padding.left + start_col * cell_width),
                                            float (padding.top + row * cell_height - top_px),
                                            float ((end_col - start_col) * cell_width),
                                            float (cell_height));
      gtk_snapshot_append_color (snapshot, color, &rect);
    }
  }
}

static void
terminal_screen_snapshot (GtkWidget   *widget,
                          GtkSnapshot *snapshot)
{
  TerminalScreen *screen = TERMINAL_SCREEN (widget);
  TerminalScreenPrivate *priv = screen->priv;

  GTK_WIDGET_CLASS (terminal_screen_parent_class)->snapshot (widget, snapshot);

  terminal_screen_snapshot_search_matches (screen, snapshot);
  terminal_screen_snapshot_quick_select (screen, snapshot);

  gtk_widget_snapshot_child (widget, GTK_WIDGET (priv->drop_highlight), snapshot);
  gtk_widget_snapshot_child (widget, GTK_WIDGET (priv->size_revealer), snapshot);
}

/* Quick select */

static void
terminal_screen_snapshot_quick_select (TerminalScreen *screen,
                                       GtkSnapshot *snapshot)
{
  auto const priv = screen->priv;
  auto const terminal = VTE_TERMINAL (screen);
  if (!priv->quick_select)
    return;

  auto const cell_width = double (vte_terminal_get_char_width (terminal));
  auto const cell_height = double (vte_terminal_get_char_height (terminal));
  if (cell_width <= 0 || cell_height <= 0)
    return;

  GtkBorder padding;
  gtk_style_context_get_padding (gtk_widget_get_style_context (GTK_WIDGET (screen)), &padding);

  long first_row, last_row;
  terminal_screen_get_visible_rows (screen, &first_row, &last_row);
  // The rows may have scrolled since the matches were found
  auto const top = double (padding.top) - double (first_row - priv->quick_select_first_row) * cell_height;

  static GdkRGBA const match_color = {1.0f, 0.8f, 0.0f, 0.25f};
  static GdkRGBA const label_color = {1.0f, 0.8f, 0.0f, 1.0f};
  static GdkRGBA const text_color = {0.0f, 0.0f, 0.0f, 1.0f};

  auto font = pango_font_description_copy (vte_terminal_get_font (terminal));
  pango_font_description_set_weight (font, PANGO_WEIGHT_BOLD);
  gs_unref_object auto layout = gtk_widget_create_pango_layout (GTK_WIDGET (screen), nullptr);
  pango_layout_set_font_description (layout, font);
  pango_font_description_free (font);

  auto const typed_len = strlen (priv->quick_select_typed);
  for (auto i = 0u; i < priv->quick_select->len; ++i) {
    auto const& match = g_array_index (priv->quick_select, TerminalQuickSelectMatch, i);
    if (!g_str_has_prefix (match.label, priv->quick_select_typed))
      continue;

    auto const y = top + double (match.line) * cell_height;
    auto const x = double (padding.left) + double (match.start_col) * cell_width;
    auto const match_rect = GRAPHENE_RECT_INIT (float (x), float (y),
                                                float ((match.end_col - match.start_col) * cell_width),
                                                float (cell_height));
    gtk_snapshot_append_color (snapshot, &match_color, &match_rect);

    // The letters still to type, over the start of the match
    auto const rest = match.label + typed_len;
    auto const label_rect = GRAPHENE_RECT_INIT (float (x), float (y),
                                                float (strlen (rest) * cell_width),
                                                float (cell_height));
    gtk_snapshot_append_color (snapshot, &label_color, &label_rect);

    pango_layout_set_text (layout, rest, -1);
    gtk_snapshot_save (snapshot);
    auto const point = GRAPHENE_POINT_INIT (float (x), float (y));
    gtk_snapshot_translate (snapshot, &point);
    gtk_snapshot_append_layout (snapshot, layout, &text_color);
    gtk_snapshot_restore (snapshot);
  }
}

/**
 * terminal_screen_stop_quick_select:
 * @screen: a #TerminalScreen
 *
 * Ends quick select, if it runs.
 */
void
terminal_screen_stop_quick_select (TerminalScreen *screen)
{
  auto const priv = screen->priv;
  if (!priv->quick_select)
    return;

  g_clear_pointer (&priv->quick_select, g_array_unref);
  if (priv->quick_select_hint) {
    terminal_tab_remove_overlay (priv->quick_select_hint);
    g_clear_object (&priv->quick_select_hint);
  }
  gtk_widget_queue_draw (GTK_WIDGET (screen));
}

/**
 * terminal_screen_start_quick_select:
 * @screen: a #TerminalScreen
 *
 * Labels what is worth copying in the visible text, and lets the user
 * pick one by typing its label.
 *
 * Returns: whether there was something to label
 */
gboolean
terminal_screen_start_quick_select (TerminalScreen *screen)
{
  g_return_val_if_fail (TERMINAL_IS_SCREEN (screen), FALSE);

  auto const priv = screen->priv;
  auto const terminal = VTE_TERMINAL (screen);
  terminal_screen_stop_quick_select (screen);

  long first_row, last_row;
  terminal_screen_get_visible_rows (screen, &first_row, &last_row);
  auto const columns = vte_terminal_get_column_count (terminal);

  // One line per row, so that rows and lines correspond
  gs_unref_ptrarray auto lines = g_ptr_array_new_with_free_func (g_free);
  for (auto row = first_row; row <= last_row; ++row) {
    auto text = vte_terminal_get_text_range_format (terminal, VTE_FORMAT_TEXT,
                                                    row, 0, row, columns, nullptr);
    if (!text)
      text = g_strdup ("");
    g_strdelimit (text, "\r\n", ' ');
    g_ptr_array_add (lines, text);
  }
  g_ptr_array_add (lines, nullptr);

  auto const settings = terminal_app_get_global_settings (terminal_app_get ());
  gs_strfreev char** patterns = g_settings_get_strv (settings, TERMINAL_SETTING_QUICK_SELECT_PATTERNS_KEY);
  auto const matches = terminal_quick_select_find (reinterpret_cast<char const* const*> (lines->pdata),
                                                   patterns);
  if (matches->len == 0) {
    g_array_unref (matches);
    return FALSE;
  }

  terminal_quick_select_assign_labels (matches);
  priv->quick_select = matches;
  priv->quick_select_first_row = first_row;
  priv->quick_select_typed[0] = '\0';

  auto hint = gtk_label_new (_("Type a label to copy it · with Shift to paste it · with Ctrl to open it · Esc to cancel"));
  gtk_label_set_wrap (GTK_LABEL (hint), true);
  gtk_widget_add_css_class (hint, "quick-select-hint");
  gtk_widget_add_css_class (hint, "osd");
  gtk_widget_set_halign (hint, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (hint, GTK_ALIGN_END);
  gtk_widget_set_can_target (hint, false);
  priv->quick_select_hint = GTK_WIDGET (g_object_ref_sink (hint));
  terminal_tab_add_screen_overlay (screen, hint);

  gtk_widget_queue_draw (GTK_WIDGET (screen));
  gtk_widget_grab_focus (GTK_WIDGET (screen));
  return TRUE;
}

static gboolean
terminal_screen_quick_select_key (TerminalScreen *screen,
                                  guint keyval,
                                  GdkModifierType state)
{
  auto const priv = screen->priv;

  switch (keyval) {
  case GDK_KEY_Shift_L: case GDK_KEY_Shift_R:
  case GDK_KEY_Control_L: case GDK_KEY_Control_R:
  case GDK_KEY_Caps_Lock:
    return GDK_EVENT_STOP;
  case GDK_KEY_BackSpace: {
    auto const len = strlen (priv->quick_select_typed);
    if (len > 0)
      priv->quick_select_typed[len - 1] = '\0';
    gtk_widget_queue_draw (GTK_WIDGET (screen));
    return GDK_EVENT_STOP;
  }
  default:
    break;
  }

  auto const c = gdk_keyval_to_unicode (gdk_keyval_to_lower (keyval));
  if (c < 'a' || c > 'z') {
    // Anything else ends it, without reaching the terminal
    terminal_screen_stop_quick_select (screen);
    return GDK_EVENT_STOP;
  }

  auto const len = strlen (priv->quick_select_typed);
  if (len >= 2) {
    terminal_screen_stop_quick_select (screen);
    return GDK_EVENT_STOP;
  }
  priv->quick_select_typed[len] = char (c);
  priv->quick_select_typed[len + 1] = '\0';

  gboolean is_prefix;
  auto const match = terminal_quick_select_lookup (priv->quick_select, priv->quick_select_typed, &is_prefix);
  if (match) {
    auto const action = (state & GDK_CONTROL_MASK) ? TERMINAL_QUICK_SELECT_OPEN
      : (state & GDK_SHIFT_MASK) || keyval != gdk_keyval_to_lower (keyval) ? TERMINAL_QUICK_SELECT_PASTE
      : TERMINAL_QUICK_SELECT_COPY;
    gs_free char* text = g_strdup (match->text);
    auto const openable = match->openable;
    terminal_screen_stop_quick_select (screen);
    g_signal_emit (screen, signals[QUICK_SELECT_CHOSEN], 0, text, int (action), openable);
  } else if (!is_prefix) {
    // Not a label; start over
    priv->quick_select_typed[0] = '\0';
  }

  gtk_widget_queue_draw (GTK_WIDGET (screen));
  return GDK_EVENT_STOP;
}

static void
terminal_screen_quick_select_focus_cb (TerminalScreen* screen,
                                       GParamSpec* pspec,
                                       void* user_data)
{
  if (!gtk_widget_has_focus (GTK_WIDGET (screen)))
    terminal_screen_stop_quick_select (screen);
}

/* Monitoring for activity and silence */

static gboolean
terminal_screen_silence_cb (void* user_data)
{
  auto const screen = TERMINAL_SCREEN (user_data);

  // Once per quiet spell; the next output starts the count again
  screen->priv->silence_source = 0;
  g_signal_emit (screen, signals[SILENCE], 0);
  return G_SOURCE_REMOVE;
}

static void
terminal_screen_restart_silence_count (TerminalScreen* screen)
{
  auto const priv = screen->priv;

  g_clear_handle_id (&priv->silence_source, g_source_remove);
  if (!priv->monitor_silence)
    return;

  auto const settings = terminal_app_get_global_settings (terminal_app_get ());
  auto const seconds = g_settings_get_uint (settings, TERMINAL_SETTING_SILENCE_DURATION_KEY);
  priv->silence_source = g_timeout_add_seconds (seconds, terminal_screen_silence_cb, screen);
}

static void
terminal_screen_monitor_contents_changed_cb (TerminalScreen* screen,
                                             void* user_data)
{
  auto const priv = screen->priv;

  if (priv->monitor_activity)
    g_signal_emit (screen, signals[ACTIVITY], 0);
  if (priv->monitor_silence)
    terminal_screen_restart_silence_count (screen);
}

/**
 * terminal_screen_set_monitoring:
 * @screen: a #TerminalScreen
 * @activity: whether to emit ::activity when the terminal prints something
 * @silence: whether to emit ::silence when it has not printed anything
 *   for the silence-duration setting
 */
void
terminal_screen_set_monitoring (TerminalScreen* screen,
                                gboolean activity,
                                gboolean silence)
{
  g_return_if_fail (TERMINAL_IS_SCREEN (screen));

  auto const priv = screen->priv;
  priv->monitor_activity = activity;
  if (priv->monitor_silence != silence) {
    priv->monitor_silence = silence;
    // A terminal that is quiet already counts from now
    terminal_screen_restart_silence_count (screen);
  }
}

void
terminal_screen_get_monitoring (TerminalScreen* screen,
                                gboolean* activity,
                                gboolean* silence)
{
  g_return_if_fail (TERMINAL_IS_SCREEN (screen));

  *activity = screen->priv->monitor_activity;
  *silence = screen->priv->monitor_silence;
}

/* Visual bell */

static void
terminal_screen_stop_bell_flash(TerminalScreen* screen)
{
  auto const priv = screen->priv;

  g_clear_handle_id(&priv->bell_flash_source, g_source_remove);
  if (priv->bell_flash) {
    terminal_tab_remove_overlay(priv->bell_flash);
    g_clear_object(&priv->bell_flash);
  }
}

static gboolean
terminal_screen_bell_flash_done_cb(void* user_data)
{
  auto const screen = TERMINAL_SCREEN(user_data);

  screen->priv->bell_flash_source = 0;
  terminal_screen_stop_bell_flash(screen);
  return G_SOURCE_REMOVE;
}

/* Flashes the terminal: a layer over it, which fades out with CSS, and
 * which lets clicks through */
static void
terminal_screen_bell_cb(TerminalScreen* screen,
                        void* user_data)
{
  auto const priv = screen->priv;
  if (!g_settings_get_boolean(priv->profile, TERMINAL_PROFILE_VISUAL_BELL_KEY))
    return;

  // Programs may ring many times in a row; restart the flash
  terminal_screen_stop_bell_flash(screen);

  if (!gtk_widget_get_mapped(GTK_WIDGET(screen)))
    return;

  auto flash = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(flash, "visual-bell");
  gtk_widget_set_can_target(flash, false);
  gtk_widget_set_can_focus(flash, false);
  priv->bell_flash = GTK_WIDGET(g_object_ref_sink(flash));
  terminal_tab_add_screen_overlay(screen, flash);

  priv->bell_flash_source = g_timeout_add(VISUAL_BELL_DURATION_MS,
                                          terminal_screen_bell_flash_done_cb,
                                          screen);
}

static void
terminal_screen_init (TerminalScreen *screen)
{
  VteTerminal *terminal = VTE_TERMINAL (screen);
  TerminalScreenPrivate *priv;
  TerminalApp *app;
  guint i;
  uuid_t u;
  char uuidstr[37];

  priv = screen->priv = (TerminalScreenPrivate*)terminal_screen_get_instance_private (screen);

  uuid_generate (u);
  uuid_unparse (u, uuidstr);
  priv->uuid = g_strdup (uuidstr);

  priv->prompts = g_array_new (false, false, sizeof (PromptMark));
  priv->command_mark_row = -1;

  /* Notice key presses before VTE handles them; see terminal_screen_is_user_input() */
  auto const key_controller = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (key_controller, GTK_PHASE_CAPTURE);
  g_signal_connect (key_controller, "key-pressed",
                    G_CALLBACK (terminal_screen_key_pressed_cb), screen);
  g_signal_connect (key_controller, "modifiers",
                    G_CALLBACK (terminal_screen_modifiers_cb), screen);
  g_signal_connect (key_controller, "key-released",
                    G_CALLBACK (terminal_screen_key_released_cb), screen);
  priv->pointer_x = priv->pointer_y = -1;
  auto const long_press = gtk_gesture_long_press_new ();
  gtk_gesture_single_set_touch_only (GTK_GESTURE_SINGLE (long_press), TRUE);
  g_signal_connect (long_press, "pressed",
                    G_CALLBACK (terminal_screen_long_pressed_cb), screen);
  gtk_widget_add_controller (GTK_WIDGET (screen), GTK_EVENT_CONTROLLER (long_press));
  auto const motion_controller = gtk_event_controller_motion_new ();
  g_signal_connect (motion_controller, "enter",
                    G_CALLBACK (terminal_screen_pointer_motion_cb), screen);
  g_signal_connect (motion_controller, "motion",
                    G_CALLBACK (terminal_screen_pointer_motion_cb), screen);
  g_signal_connect (motion_controller, "leave",
                    G_CALLBACK (terminal_screen_pointer_leave_cb), screen);
  gtk_widget_add_controller (GTK_WIDGET (screen), motion_controller);
  gtk_widget_add_controller (GTK_WIDGET (screen), key_controller);
  priv->search_current = -1;

  priv->child_pid = -1;

  priv->has_progress = false;
  priv->progress_hint = VTE_PROGRESS_HINT_INACTIVE;
  priv->progress_fraction = 0.;

  gtk_widget_init_template (GTK_WIDGET (screen));

  vte_terminal_set_mouse_autohide (terminal, TRUE);
  vte_terminal_set_allow_hyperlink (terminal, TRUE);
  vte_terminal_set_scroll_unit_is_pixels (terminal, TRUE);
  vte_terminal_set_enable_fallback_scrolling (terminal, FALSE);
  /* When the height is not a multiple of the line height, leave the rest
   * blank at the bottom instead of showing part of a line there */
  vte_terminal_set_yfill (terminal, FALSE);

  for (i = 0; i < n_url_regexes; ++i)
    {
      TagData *tag_data;

      tag_data = g_slice_new (TagData);
      tag_data->flavor = url_regex_flavors[i];
      tag_data->tag = vte_terminal_match_add_regex (terminal, url_regexes[i], 0);
      vte_terminal_match_set_cursor_name (terminal, tag_data->tag, URL_MATCH_IDLE_CURSOR_NAME);

      priv->match_tags = g_slist_prepend (priv->match_tags, tag_data);
    }

  GdkContentFormatsBuilder *builder = gdk_content_formats_builder_new ();
  gdk_content_formats_builder_add_gtype (builder, G_TYPE_STRING);
  gdk_content_formats_builder_add_gtype (builder, GDK_TYPE_FILE_LIST);
  gdk_content_formats_builder_add_gtype (builder, GDK_TYPE_RGBA);
  gdk_content_formats_builder_add_mime_type (builder, APPLICATION_VND_PORTAL_FILES);
  gdk_content_formats_builder_add_mime_type (builder, APPLICATION_VND_PORTAL_FILETRANSFER);
  gdk_content_formats_builder_add_mime_type (builder, TEXT_URI_LIST);
  gdk_content_formats_builder_add_mime_type (builder, TEXT_X_MOZ_URL);
  gdk_content_formats_builder_add_mime_type (builder, X_SPECIAL_GNOME_RESET_BACKGROUND);
  g_autoptr(GdkContentFormats) formats = gdk_content_formats_builder_free_to_formats (builder);

  gtk_drop_target_async_set_actions (priv->drop_target,
                                     GdkDragAction(GDK_ACTION_COPY|GDK_ACTION_MOVE));
  gtk_drop_target_async_set_formats (priv->drop_target, formats);

  g_signal_connect (screen, "window-title-changed",
                    G_CALLBACK (terminal_screen_window_title_changed),
                    screen);

  g_signal_connect(screen, "termprop-changed::" VTE_TERMPROP_ICON_COLOR,
                   G_CALLBACK(terminal_screen_icon_color_changed_cb), screen);
  g_signal_connect(screen, "termprop-changed::" VTE_TERMPROP_ICON_IMAGE,
                   G_CALLBACK(terminal_screen_icon_image_changed_cb), screen);

  g_signal_connect(screen, "termprop-changed::" VTE_TERMPROP_PROGRESS_VALUE,
                   G_CALLBACK(terminal_screen_progress_value_changed_cb), screen);
  g_signal_connect(screen, "termprop-changed::" VTE_TERMPROP_PROGRESS_HINT,
                   G_CALLBACK(terminal_screen_progress_hint_changed_cb), screen);

  g_signal_connect(screen, "termprop-changed::" VTE_TERMPROP_SHELL_PREEXEC,
                   G_CALLBACK(terminal_screen_shell_preexec_cb), screen);
  g_signal_connect(screen, "termprop-changed::" VTE_TERMPROP_SHELL_POSTEXEC,
                   G_CALLBACK(terminal_screen_shell_postexec_cb), screen);
  g_signal_connect(screen, "termprop-changed::" VTE_TERMPROP_SHELL_PRECMD,
                   G_CALLBACK(terminal_screen_shell_precmd_cb), screen);

  g_signal_connect(screen, "bell",
                   G_CALLBACK(terminal_screen_bell_cb), nullptr);
  g_signal_connect(screen, "notify::has-focus",
                   G_CALLBACK(terminal_screen_quick_select_focus_cb), nullptr);
  g_signal_connect(screen, "contents-changed",
                   G_CALLBACK(terminal_screen_monitor_contents_changed_cb), nullptr);

  g_signal_connect(screen, "termprop-changed::" VTE_TERMPROP_CURRENT_DIRECTORY_URI,
                   G_CALLBACK(terminal_screen_cwd_changed_cb), nullptr);
  g_signal_connect_object(terminal_app_get_global_settings(terminal_app_get()),
                          "changed::" TERMINAL_SETTING_IGNORE_PROGRAM_TITLES_KEY,
                          G_CALLBACK(terminal_screen_ignore_program_titles_changed_cb),
                          screen, G_CONNECT_SWAPPED);

  // A new foreground program usually prints something; look then
  g_signal_connect(screen, "contents-changed",
                   G_CALLBACK(terminal_screen_queue_process_kind_update), nullptr);
  g_signal_connect_object(terminal_app_get_global_settings(terminal_app_get()),
                          "changed::" TERMINAL_SETTING_HIGHLIGHT_ROOT_AND_REMOTE_KEY,
                          G_CALLBACK(terminal_screen_queue_process_kind_update),
                          screen, G_CONNECT_SWAPPED);

  app = terminal_app_get ();
  // Scheme files may change on disk
  g_signal_connect_object (terminal_schemes_get_default (), "changed",
                           G_CALLBACK (update_color_scheme), screen, G_CONNECT_SWAPPED);

  g_signal_connect (terminal_app_get_desktop_interface_settings (app), "changed::" MONOSPACE_FONT_KEY_NAME,
                    G_CALLBACK (terminal_screen_system_font_changed_cb), screen);

}

static void
terminal_screen_get_property (GObject *object,
                              guint prop_id,
                              GValue *value,
                              GParamSpec *pspec)
{
  TerminalScreen *screen = TERMINAL_SCREEN (object);

  switch (prop_id)
    {
      case PROP_PROFILE:
        g_value_set_object (value, terminal_screen_get_profile (screen));
        break;
      case PROP_ICON:
        g_value_set_object(value, terminal_screen_get_icon(screen));
        break;
      case PROP_PROCESS_KIND:
        g_value_set_int(value, int(screen->priv->process_kind));
        break;
      case PROP_ICON_PROGRESS:
        g_value_set_object(value, terminal_screen_get_icon_progress(screen));
        break;
      case PROP_TITLE:
        g_value_set_string (value, terminal_screen_get_title (screen));
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
terminal_screen_set_property (GObject *object,
                              guint prop_id,
                              const GValue *value,
                              GParamSpec *pspec)
{
  TerminalScreen *screen = TERMINAL_SCREEN (object);

  switch (prop_id)
    {
      case PROP_PROFILE:
        terminal_screen_set_profile (screen, (GSettings*)g_value_get_object (value));
        break;
      case PROP_ICON:
      case PROP_ICON_PROGRESS:
      case PROP_TITLE:
      case PROP_PROCESS_KIND:
        /* not writable */
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
terminal_screen_class_init (TerminalScreenClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  VteTerminalClass *terminal_class = VTE_TERMINAL_CLASS (klass);

  object_class->constructed = terminal_screen_constructed;
  object_class->dispose = terminal_screen_dispose;
  object_class->finalize = terminal_screen_finalize;
  object_class->get_property = terminal_screen_get_property;
  object_class->set_property = terminal_screen_set_property;

  widget_class->css_changed = terminal_screen_css_changed;
  widget_class->measure = terminal_screen_measure;
  widget_class->realize = terminal_screen_realize;
  widget_class->root = terminal_screen_root;
  widget_class->size_allocate = terminal_screen_size_allocate;
  widget_class->snapshot = terminal_screen_snapshot;

  terminal_class->child_exited = terminal_screen_child_exited;

  signals[PROFILE_SET] =
    g_signal_new (I_("profile-set"),
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (TerminalScreenClass, profile_set),
                  nullptr, nullptr,
                  g_cclosure_marshal_VOID__OBJECT,
                  G_TYPE_NONE,
                  1, G_TYPE_SETTINGS);
  
  signals[SHOW_POPUP_MENU] =
    g_signal_new (I_("show-popup-menu"),
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (TerminalScreenClass, show_popup_menu),
                  nullptr, nullptr,
                  g_cclosure_marshal_VOID__POINTER,
                  G_TYPE_NONE,
                  1,
                  G_TYPE_POINTER);

  signals[MATCH_CLICKED] =
    g_signal_new (I_("match-clicked"),
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (TerminalScreenClass, match_clicked),
                  g_signal_accumulator_true_handled, nullptr,
                  _terminal_marshal_BOOLEAN__STRING_INT_UINT,
                  G_TYPE_BOOLEAN,
                  3, G_TYPE_STRING, G_TYPE_INT, G_TYPE_UINT);
  
  /* Emitted when a command entered at the shell prompt has finished, if the
   * shell reports this (see vte.sh). The command line is nullptr if the
   * command ended too quickly to see it. */
  signals[COMMAND_FINISHED] =
    g_signal_new (I_("command-finished"),
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  0,
                  nullptr, nullptr,
                  nullptr /* generic marshaller */,
                  G_TYPE_NONE,
                  3, G_TYPE_STRING, G_TYPE_UINT, G_TYPE_UINT64);

  /* The column count changed, so VTE has rewrapped the lines */
  signals[COLUMNS_CHANGED] =
    g_signal_new (I_("columns-changed"),
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  0,
                  nullptr, nullptr,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE,
                  0);

  /* Quick select picked a match: its text, and what to do with it,
   * a TerminalQuickSelectAction */
  signals[QUICK_SELECT_CHOSEN] =
    g_signal_new (I_("quick-select-chosen"),
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  0,
                  nullptr, nullptr,
                  nullptr /* generic marshaller */,
                  G_TYPE_NONE,
                  3, G_TYPE_STRING, G_TYPE_INT, G_TYPE_BOOLEAN);

  /* The terminal's colors changed, see terminal_screen_get_custom_colors() */
  signals[COLORS_CHANGED] =
    g_signal_new (I_("colors-changed"),
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  0,
                  nullptr, nullptr,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE,
                  0);

  /* The terminal printed something while monitored for activity */
  signals[ACTIVITY] =
    g_signal_new (I_("activity"),
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  0,
                  nullptr, nullptr,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE,
                  0);

  /* The terminal has been quiet while monitored for silence */
  signals[SILENCE] =
    g_signal_new (I_("silence"),
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  0,
                  nullptr, nullptr,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE,
                  0);

  signals[PROMPTS_CHANGED] =
    g_signal_new (I_("prompts-changed"),
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  0,
                  nullptr, nullptr,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE,
                  0);

  signals[CLOSE_SCREEN] =
    g_signal_new (I_("close-screen"),
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (TerminalScreenClass, close_screen),
                  nullptr, nullptr,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE,
                  0);

  pspecs[PROP_PROFILE] =
     g_param_spec_object ("profile", nullptr, nullptr,
                          G_TYPE_SETTINGS,
                          GParamFlags(G_PARAM_READWRITE |
				      G_PARAM_STATIC_NAME |
				      G_PARAM_STATIC_NICK |
				      G_PARAM_STATIC_BLURB));

  pspecs[PROP_ICON] =
     g_param_spec_object ("icon", nullptr, nullptr,
                          G_TYPE_ICON,
                          GParamFlags(G_PARAM_READABLE |
				      G_PARAM_STATIC_STRINGS |
                                      G_PARAM_EXPLICIT_NOTIFY));

  pspecs[PROP_ICON_PROGRESS] =
     g_param_spec_object ("icon-progress", nullptr, nullptr,
                          G_TYPE_ICON,
                          GParamFlags(G_PARAM_READABLE |
				      G_PARAM_STATIC_STRINGS |
                                      G_PARAM_EXPLICIT_NOTIFY));

  pspecs[PROP_TITLE] =
     g_param_spec_string ("title", nullptr, nullptr,
                          nullptr,
                          GParamFlags(G_PARAM_READABLE |
				      G_PARAM_STATIC_STRINGS |
                                      G_PARAM_EXPLICIT_NOTIFY));

  pspecs[PROP_PROCESS_KIND] =
     g_param_spec_int ("process-kind", nullptr, nullptr,
                       TERMINAL_PROCESS_KIND_NORMAL, TERMINAL_PROCESS_KIND_REMOTE,
                       TERMINAL_PROCESS_KIND_NORMAL,
                       GParamFlags(G_PARAM_READABLE |
                                   G_PARAM_STATIC_STRINGS |
                                   G_PARAM_EXPLICIT_NOTIFY));

  g_object_class_install_properties(object_class, N_PROPS, pspecs);

  gtk_widget_class_install_action (widget_class,
                                   "menu.popup",
                                   nullptr,
                                   terminal_screen_menu_popup_action);
  /* GTK 4 does not bind the keys that open a context menu by itself */
  gtk_widget_class_add_binding_action (widget_class, GDK_KEY_Menu, GdkModifierType(0),
                                       "menu.popup", nullptr);
  gtk_widget_class_add_binding_action (widget_class, GDK_KEY_F10, GDK_SHIFT_MASK,
                                       "menu.popup", nullptr);

  gtk_widget_class_set_template_from_resource (widget_class, "/com/aripd/pano/ui/screen.ui");

  gtk_widget_class_bind_template_child_private (widget_class, TerminalScreen, drop_target);
  gtk_widget_class_bind_template_child_private (widget_class, TerminalScreen, drop_highlight);
  gtk_widget_class_bind_template_child_private (widget_class, TerminalScreen, size_label);
  gtk_widget_class_bind_template_child_private (widget_class, TerminalScreen, size_revealer);

  gtk_widget_class_bind_template_callback (widget_class, terminal_screen_bubble_click_pressed_cb);
  gtk_widget_class_bind_template_callback (widget_class, terminal_screen_capture_click_pressed_cb);
  gtk_widget_class_bind_template_callback (widget_class, terminal_screen_drop_target_drag_enter);
  gtk_widget_class_bind_template_callback (widget_class, terminal_screen_drop_target_drag_leave);
  gtk_widget_class_bind_template_callback (widget_class, terminal_screen_drop_target_drop);

  n_url_regexes = G_N_ELEMENTS (url_regex_patterns);
  precompile_regexes (url_regex_patterns, n_url_regexes, &url_regexes, &url_regex_flavors);
  n_extra_regexes = G_N_ELEMENTS (extra_regex_patterns);
  precompile_regexes (extra_regex_patterns, n_extra_regexes, &extra_regexes, &extra_regex_flavors);

  g_type_ensure (ADW_TYPE_BIN);
}

static void
terminal_screen_constructed (GObject *object)
{
  TerminalScreen *screen = TERMINAL_SCREEN (object);
  TerminalScreenPrivate *priv = screen->priv;
  GSettings *settings;

  G_OBJECT_CLASS (terminal_screen_parent_class)->constructed (object);

  terminal_app_register_screen (terminal_app_get (), screen);
  priv->registered = TRUE;

  /* This fixes bug #329827 */
  settings = terminal_app_get_global_settings (terminal_app_get ());
  g_signal_connect_object (settings,
                           "changed::" TERMINAL_SETTING_ENABLE_MENU_BAR_ACCEL_KEY,
                           G_CALLBACK (terminal_screen_enable_menu_bar_accel_notify_cb),
                           screen,
                           GConnectFlags(0));
  terminal_screen_enable_menu_bar_accel_notify_cb (settings, TERMINAL_SETTING_ENABLE_MENU_BAR_ACCEL_KEY, screen);
}

static void
terminal_screen_dispose (GObject *object)
{
  TerminalScreen *screen = TERMINAL_SCREEN (object);
  TerminalScreenPrivate *priv = screen->priv;
  GtkSettings *settings;

  g_clear_handle_id (&priv->size_dismiss_source, g_source_remove);
  terminal_screen_stop_bell_flash (screen);
  terminal_screen_stop_quick_select (screen);
  g_clear_handle_id (&priv->silence_source, g_source_remove);

  gtk_widget_dispose_template (GTK_WIDGET (object), TERMINAL_TYPE_SCREEN);

  /* Unset child PID so that when an eventual child-exited signal arrives,
   * we don't emit "close".
   */
  priv->child_pid = -1;

  settings = gtk_widget_get_settings (GTK_WIDGET (screen));
  g_signal_handlers_disconnect_matched (settings, G_SIGNAL_MATCH_DATA,
                                        0, 0, nullptr, nullptr,
                                        screen);

  if (priv->idle_exec_source != 0)
    {
      g_source_remove (priv->idle_exec_source);
      priv->idle_exec_source = 0;
    }

  terminal_screen_clear_exec_data (screen, TRUE);

  g_clear_object(&screen->priv->icon_color);
  g_clear_object(&screen->priv->icon_image);
  g_clear_handle_id(&priv->command_capture_source, g_source_remove);
  g_clear_handle_id(&priv->precmd_batch_source, g_source_remove);
  g_clear_handle_id(&priv->key_event_source, g_source_remove);
  g_clear_handle_id(&priv->process_kind_source, g_source_remove);
  g_cancellable_cancel(priv->host_process_cancellable);
  g_clear_object(&priv->host_process_cancellable);
  g_clear_pointer(&priv->host_process, terminal_flatpak_process_info_free);
  g_clear_pointer(&priv->container_runtime, g_free);
  g_clear_pointer(&priv->container_name, g_free);
  g_clear_pointer(&priv->command, g_free);

  G_OBJECT_CLASS (terminal_screen_parent_class)->dispose (object);

  /* Unregister *after* chaining up to the parent's dispose,
   * since that will terminate the child process if there still
   * is any, and we need to get the dbus signal out
   * from the TerminalReceiver.
   */
  if (priv->registered) {
    terminal_app_unregister_screen (terminal_app_get (), screen);
    priv->registered = FALSE;
  }
}

static void
terminal_screen_finalize (GObject *object)
{
  TerminalScreen *screen = TERMINAL_SCREEN (object);
  TerminalScreenPrivate *priv = screen->priv;

  g_signal_handlers_disconnect_by_func (terminal_app_get_desktop_interface_settings (terminal_app_get ()),
                                        (void*)terminal_screen_system_font_changed_cb,
                                        screen);

  terminal_screen_set_profile (screen, nullptr);

  g_slist_free_full (priv->match_tags, (GDestroyNotify) free_tag_data);
  g_clear_pointer (&priv->prompts, g_array_unref);
  g_clear_pointer (&priv->prompt_anchors, g_array_unref);
  g_clear_pointer (&priv->search_matches, g_array_unref);
  g_clear_pointer (&priv->saved_word_chars, g_free);

  g_free (priv->uuid);
  g_free (priv->user_title);
  g_free (priv->initial_title);
  g_free (priv->directory_title);

  G_OBJECT_CLASS (terminal_screen_parent_class)->finalize (object);
}

TerminalScreen *
terminal_screen_new (GSettings       *profile,
                     const char      *title,
                     double           zoom)
{
  g_return_val_if_fail (G_IS_SETTINGS (profile), nullptr);

  TerminalScreen *screen = (TerminalScreen*)g_object_new (TERMINAL_TYPE_SCREEN, nullptr);

  terminal_screen_set_profile (screen, profile);

  vte_terminal_set_size (VTE_TERMINAL (screen),
                         g_settings_get_int (profile, TERMINAL_PROFILE_DEFAULT_SIZE_COLUMNS_KEY),
                         g_settings_get_int (profile, TERMINAL_PROFILE_DEFAULT_SIZE_ROWS_KEY));

  /* If given an initial title, strip it of control characters and
   * feed it to the terminal. It is also kept for when the titles that
   * programs set are ignored.
   */
  if (title != nullptr && title[0])
    screen->priv->initial_title = g_strdup (title);
  if (title != nullptr) {
    GString *seq;
    const char *p;

    seq = g_string_new ("\033]0;");
    for (p = title; *p; p = g_utf8_next_char (p)) {
      gunichar c = g_utf8_get_char (p);
      if (c < 0x20 || (c >= 0x7f && c <= 0x9f))
        continue;
      else if (c == ';')
        break;

      g_string_append_unichar (seq, c);
    }
    g_string_append (seq, "\033\\");

    vte_terminal_feed (VTE_TERMINAL (screen), seq->str, seq->len);
    g_string_free (seq, TRUE);
  }

  vte_terminal_set_font_scale (VTE_TERMINAL (screen), zoom);
  terminal_screen_set_font (screen);

  return screen;
}

static gboolean
terminal_screen_reexec_from_exec_data (TerminalScreen *screen,
                                       ExecData *data,
                                       char **envv,
                                       const char *cwd,
                                       GCancellable *cancellable,
                                       GError **error)
{
  _TERMINAL_DEBUG_IF (TERMINAL_DEBUG_PROCESSES) {
    gs_free char *str = exec_data_to_string (data);
    _terminal_debug_print (TERMINAL_DEBUG_PROCESSES,
                           "[screen %p] reexec_from_data: envv:%p(%u) cwd:%s data:[%s]\n",
                           screen,
                           envv, envv ? g_strv_length (envv) : 0,
                           cwd,
                           str);
  }

  /* If we have command line args, must always pass the cwd from the command line, too */
  auto const data_cwd = data && (data->argv || !cwd);
  return terminal_screen_exec (screen,
                               data ? data->argv : nullptr,
                               envv ? envv : data ? data->envv : nullptr,
                               data ? data->as_shell : TRUE,
                               data_cwd ? data->cwd : cwd,
                               data_cwd ? data->cwd_requested : FALSE,
                               nullptr /* fd list */, nullptr /* fd array */,
                               nullptr, nullptr, nullptr, /* callback + data + destroy notify */
                               cancellable,
                               error);
}

gboolean
terminal_screen_reexec_from_screen (TerminalScreen *screen,
                                    TerminalScreen *parent_screen,
                                    GCancellable *cancellable,
                                    GError **error)
{
  g_return_val_if_fail (TERMINAL_IS_SCREEN (screen), FALSE);

  if (parent_screen == nullptr)
    return TRUE;

  g_return_val_if_fail (TERMINAL_IS_SCREEN (parent_screen), FALSE);

  gs_free char* cwd = terminal_screen_get_current_dir (parent_screen);

  _terminal_debug_print (TERMINAL_DEBUG_PROCESSES,
                         "[screen %p] reexec_from_screen: parent:%p cwd:%s\n",
                         screen,
                         parent_screen,
                         cwd);

  return terminal_screen_reexec_from_exec_data (screen,
                                                nullptr /* exec data */,
                                                nullptr /* envv */,
                                                cwd,
                                                cancellable,
                                                error);
}

gboolean
terminal_screen_reexec (TerminalScreen *screen,
                        char **envv,
                        const char *cwd,
                        GCancellable *cancellable,
                        GError **error)
{
  g_return_val_if_fail (TERMINAL_IS_SCREEN (screen), FALSE);


  _terminal_debug_print (TERMINAL_DEBUG_PROCESSES,
                         "[screen %p] reexec: envv:%p(%u) cwd:%s\n",
                         screen,
                         envv, envv ? g_strv_length (envv) : 0,
                         cwd);

  return terminal_screen_reexec_from_exec_data (screen,
                                                screen->priv->exec_data,
                                                envv,
                                                cwd,
                                                cancellable,
                                                error);
}

gboolean
terminal_screen_exec (TerminalScreen *screen,
                      char **argv,
                      char **initial_envv,
                      gboolean as_shell,
                      const char *cwd,
                      gboolean cwd_requested,
                      GUnixFDList *fd_list,
                      GVariant *fd_array,
                      TerminalScreenExecCallback callback,
                      gpointer user_data,
                      GDestroyNotify destroy_notify,
                      GCancellable *cancellable,
                      GError **error)
{
  g_return_val_if_fail (TERMINAL_IS_SCREEN (screen), FALSE);
  g_return_val_if_fail (cancellable == nullptr || G_IS_CANCELLABLE (cancellable), FALSE);
  g_return_val_if_fail (error == nullptr || *error == nullptr, FALSE);
  g_return_val_if_fail (gtk_widget_get_parent (GTK_WIDGET (screen)) != nullptr, FALSE);

  _TERMINAL_DEBUG_IF (TERMINAL_DEBUG_PROCESSES) {
    gs_free char *argv_str = nullptr;
    _terminal_debug_print (TERMINAL_DEBUG_PROCESSES,
                           "[screen %p] exec: argv:[%s] envv:%p(%u) as-shell:%s cwd:%s\n",
                           screen,
                           (argv_str = strv_to_string(argv)),
                           initial_envv, initial_envv ? g_strv_length (initial_envv) : 0,
                           as_shell ? "true":"false",
                           cwd);
  }

  TerminalScreenPrivate *priv = screen->priv;

  /* The cursor stops blinking while a terminal is held after its command
   * exited; blink as the profile says again */
  vte_terminal_set_cursor_blink_mode (VTE_TERMINAL (screen),
                                      VteCursorBlinkMode(g_settings_get_enum (priv->profile, TERMINAL_PROFILE_CURSOR_BLINK_MODE_KEY)));

  ExecData *data = exec_data_new ();
  data->callback = callback;
  data->callback_data = user_data;
  data->callback_data_destroy_notify = destroy_notify;

  GError *err = nullptr;
  if (priv->child_pid != -1) {
    g_set_error_literal (&err, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                         "Cannot launch a new child process while the terminal is still running another child process");

    terminal_screen_show_info_bar (screen, err, FALSE);
    g_propagate_error (error, err);
    exec_data_unref (data); /* frees the callback data */
    return FALSE;
  }

  gs_free char *path = nullptr;
  gs_free char *shell = nullptr;
  gs_strfreev char **envv = terminal_screen_get_child_environment (screen,
                                                                  initial_envv,
                                                                  &path,
                                                                  &shell);

  gboolean preserve_cwd = FALSE;
  GSpawnFlags spawn_flags = GSpawnFlags(G_SPAWN_SEARCH_PATH_FROM_ENVP |
					VTE_SPAWN_NO_PARENT_ENVV);
  gs_strfreev char **exec_argv = nullptr;
  if (!terminal_screen_get_child_command (screen,
                                          argv,
                                          path,
                                          shell,
                                          as_shell,
                                          &preserve_cwd,
                                          &spawn_flags,
                                          &exec_argv,
                                          &err)) {
    terminal_screen_show_info_bar (screen, err, FALSE);
    g_propagate_error (error, err);
    exec_data_unref (data); /* frees the callback data */
    return FALSE;
  }

  terminal_screen_add_shell_integration (screen, argv, as_shell, &exec_argv, &envv);

  /* A working directory that was asked for, for example with
   * --working-directory or by the file manager, is always used */
  if (!preserve_cwd && !(cwd && cwd_requested)) {
    cwd = g_get_home_dir ();
    envv = g_environ_unsetenv (envv, "PWD");
  }

  data->fd_list = (GUnixFDList*)(fd_list ? g_object_ref(fd_list) : nullptr);

  if (fd_array) {
    terminal_assert_nonnull(fd_list);
    G_GNUC_UNUSED int n_fds = g_unix_fd_list_get_length(fd_list);

    gsize fd_array_data_len;
    const int *fd_array_data = (int const*)g_variant_get_fixed_array (fd_array, &fd_array_data_len, 2 * sizeof (int));

    data->n_fd_map = fd_array_data_len;
    data->fd_map = g_new (int, data->n_fd_map);
    for (gsize i = 0; i < fd_array_data_len; i++) {
      const int fd = fd_array_data[2 * i];
      const int idx = fd_array_data[2 * i + 1];
      if (idx < 0 || idx > n_fds) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "FD index out of bounds");
        exec_data_unref(data);
	return FALSE;
      }

      data->fd_map[idx] = fd;
    }
  } else {
    data->n_fd_map = 0;
    data->fd_map = nullptr;
  }

  data->argv = g_strdupv (argv);
  data->exec_argv = g_strdupv (exec_argv);
  data->cwd = g_strdup (cwd);
  data->cwd_requested = cwd_requested;
  data->envv = g_strdupv (envv);
  data->as_shell = as_shell;
  data->pty_flags = VTE_PTY_DEFAULT;
  data->spawn_flags = spawn_flags;
  data->cancellable = (GCancellable*)(cancellable ? g_object_ref (cancellable) : nullptr);

  terminal_screen_clear_exec_data (screen, TRUE);
  priv->exec_data = data;

  terminal_screen_queue_idle_exec (screen);

  return TRUE;
}

/**
 * terminal_screen_get_title:
 * @screen: a #TerminalScreen
 *
 * Returns: the title: the one set by the user, else the one the program
 *   set (unless those are ignored), else the one from --title, else the
 *   working directory
 */
const char*
terminal_screen_get_title (TerminalScreen *screen)
{
  auto const priv = screen->priv;
  if (priv->user_title)
    return priv->user_title;

  auto const settings = terminal_app_get_global_settings (terminal_app_get ());
  if (!g_settings_get_boolean (settings, TERMINAL_SETTING_IGNORE_PROGRAM_TITLES_KEY)) {
    auto const title = vte_terminal_get_window_title (VTE_TERMINAL (screen));
    if (title && title[0])
      return title;
  }

  if (priv->initial_title)
    return priv->initial_title;

  return priv->directory_title ? priv->directory_title : "";
}

/**
 * terminal_screen_get_user_title:
 * @screen: a #TerminalScreen
 *
 * Returns: (nullable): the title set by the user
 */
const char*
terminal_screen_get_user_title (TerminalScreen* screen)
{
  g_return_val_if_fail (TERMINAL_IS_SCREEN (screen), nullptr);

  return screen->priv->user_title;
}

/**
 * terminal_screen_set_user_title:
 * @screen: a #TerminalScreen
 * @title: (nullable): the title, or %NULL or "" to go back to the others
 *
 * Sets a title that programs cannot change.
 */
void
terminal_screen_set_user_title (TerminalScreen* screen,
                                const char* title)
{
  g_return_if_fail (TERMINAL_IS_SCREEN (screen));

  auto const priv = screen->priv;
  gs_free char* stripped = title ? g_strstrip (g_strdup (title)) : nullptr;
  auto const value = stripped && stripped[0] ? stripped : nullptr;
  if (g_strcmp0 (value, priv->user_title) == 0)
    return;

  g_free (priv->user_title);
  priv->user_title = g_strdup (value);
  g_object_notify_by_pspec (G_OBJECT (screen), pspecs[PROP_TITLE]);
}

/* The working directory, with ~ for the home folder */
static void
terminal_screen_update_directory_title (TerminalScreen* screen)
{
  auto const priv = screen->priv;
  gs_free char* dir = terminal_screen_get_current_dir (screen);
  gs_free char* title = nullptr;
  if (dir) {
    auto const home = g_get_home_dir ();
    auto const len = strlen (home);
    if (g_str_equal (dir, home))
      title = g_strdup ("~");
    else if (len > 1 && g_str_has_prefix (dir, home) && dir[len] == '/')
      title = g_strconcat ("~", dir + len, nullptr);
    else
      title = g_strdup (dir);
  }

  if (g_strcmp0 (title, priv->directory_title) == 0)
    return;

  g_free (priv->directory_title);
  priv->directory_title = static_cast<char*>(g_steal_pointer (&title));
  g_object_notify_by_pspec (G_OBJECT (screen), pspecs[PROP_TITLE]);
}

static void
terminal_screen_ignore_program_titles_changed_cb (TerminalScreen* screen)
{
  g_object_notify_by_pspec (G_OBJECT (screen), pspecs[PROP_TITLE]);
}

static void
terminal_screen_cwd_changed_cb (TerminalScreen* screen,
                                const char* prop,
                                void* user_data)
{
  terminal_screen_update_directory_title (screen);
}

static void
terminal_screen_profile_changed_cb (GSettings     *profile,
                                    const char    *prop_name,
                                    TerminalScreen *screen)
{
  TerminalScreenPrivate *priv = screen->priv;
  GObject *object = G_OBJECT (screen);
  VteTerminal *vte_terminal = VTE_TERMINAL (screen);
  TerminalWindow *window;

  g_object_freeze_notify (object);

  if ((window = terminal_screen_get_window (screen)))
    {
      /* We need these in line for the set_size in
       * update_on_realize
       */
      terminal_window_update_geometry (window);
    }

  if (!prop_name || prop_name == I_(TERMINAL_PROFILE_SCROLLBAR_POLICY_KEY))
    _terminal_screen_update_scrollbar (screen);

  if (!prop_name || prop_name == I_(TERMINAL_PROFILE_KINETIC_SCROLLING_KEY))
    _terminal_screen_update_kinetic_scrolling (screen);

  if (!prop_name || prop_name == I_(TERMINAL_PROFILE_ENCODING_KEY))
    {
      gs_free char *charset = g_settings_get_string (profile, TERMINAL_PROFILE_ENCODING_KEY);
      const char *encoding = terminal_util_translate_encoding (charset);
      if (encoding != nullptr)
        vte_terminal_set_encoding (vte_terminal, encoding, nullptr);
    }

  if (!prop_name || prop_name == I_(TERMINAL_PROFILE_CJK_UTF8_AMBIGUOUS_WIDTH_KEY))
    {
      int width;

      width = g_settings_get_enum (profile, TERMINAL_PROFILE_CJK_UTF8_AMBIGUOUS_WIDTH_KEY);
      vte_terminal_set_cjk_ambiguous_width (vte_terminal, width);
    }

  if (gtk_widget_get_realized (GTK_WIDGET (screen)) &&
      (!prop_name ||
       prop_name == I_(TERMINAL_PROFILE_USE_SYSTEM_FONT_KEY) ||
       prop_name == I_(TERMINAL_PROFILE_FONT_KEY) ||
       prop_name == I_(TERMINAL_PROFILE_CELL_WIDTH_SCALE_KEY) ||
       prop_name == I_(TERMINAL_PROFILE_CELL_HEIGHT_SCALE_KEY)))
    terminal_screen_set_font (screen);

  if (!prop_name ||
      prop_name == I_(TERMINAL_PROFILE_USE_THEME_COLORS_KEY) ||
      prop_name == I_(TERMINAL_PROFILE_FOREGROUND_COLOR_KEY) ||
      prop_name == I_(TERMINAL_PROFILE_BACKGROUND_COLOR_KEY) ||
      prop_name == I_(TERMINAL_PROFILE_BACKGROUND_OPACITY_KEY) ||
      prop_name == I_(TERMINAL_PROFILE_COLOR_SCHEME_KEY) ||
      prop_name == I_(TERMINAL_PROFILE_BOLD_COLOR_SAME_AS_FG_KEY) ||
      prop_name == I_(TERMINAL_PROFILE_BOLD_COLOR_KEY) ||
      prop_name == I_(TERMINAL_PROFILE_CURSOR_COLORS_SET_KEY) ||
      prop_name == I_(TERMINAL_PROFILE_CURSOR_BACKGROUND_COLOR_KEY) ||
      prop_name == I_(TERMINAL_PROFILE_CURSOR_FOREGROUND_COLOR_KEY) ||
      prop_name == I_(TERMINAL_PROFILE_HIGHLIGHT_COLORS_SET_KEY) ||
      prop_name == I_(TERMINAL_PROFILE_HIGHLIGHT_BACKGROUND_COLOR_KEY) ||
      prop_name == I_(TERMINAL_PROFILE_HIGHLIGHT_FOREGROUND_COLOR_KEY) ||
      prop_name == I_(TERMINAL_PROFILE_PALETTE_KEY))
    update_color_scheme (screen);

  if (!prop_name || prop_name == I_(TERMINAL_PROFILE_AUDIBLE_BELL_KEY))
      vte_terminal_set_audible_bell (vte_terminal, g_settings_get_boolean (profile, TERMINAL_PROFILE_AUDIBLE_BELL_KEY));

  if (!prop_name || prop_name == I_(TERMINAL_PROFILE_SCROLL_ON_INSERT_KEY))
    vte_terminal_set_scroll_on_insert(vte_terminal,
                                      g_settings_get_boolean(profile, TERMINAL_PROFILE_SCROLL_ON_INSERT_KEY));
  if (!prop_name || prop_name == I_(TERMINAL_PROFILE_SCROLL_ON_KEYSTROKE_KEY))
    vte_terminal_set_scroll_on_keystroke (vte_terminal,
                                          g_settings_get_boolean (profile, TERMINAL_PROFILE_SCROLL_ON_KEYSTROKE_KEY));
  if (!prop_name || prop_name == I_(TERMINAL_PROFILE_SCROLL_ON_OUTPUT_KEY))
    vte_terminal_set_scroll_on_output (vte_terminal,
                                       g_settings_get_boolean (profile, TERMINAL_PROFILE_SCROLL_ON_OUTPUT_KEY));
  if (!prop_name ||
      prop_name == I_(TERMINAL_PROFILE_SCROLLBACK_LINES_KEY) ||
      prop_name == I_(TERMINAL_PROFILE_SCROLLBACK_UNLIMITED_KEY))
    {
      glong lines = g_settings_get_boolean (profile, TERMINAL_PROFILE_SCROLLBACK_UNLIMITED_KEY) ?
		    -1 : g_settings_get_int (profile, TERMINAL_PROFILE_SCROLLBACK_LINES_KEY);
      vte_terminal_set_scrollback_lines (vte_terminal, lines);
    }

  if (!prop_name || prop_name == I_(TERMINAL_PROFILE_BACKSPACE_BINDING_KEY))
  vte_terminal_set_backspace_binding (vte_terminal,
                                      VteEraseBinding(g_settings_get_enum (profile, TERMINAL_PROFILE_BACKSPACE_BINDING_KEY)));
  
  if (!prop_name || prop_name == I_(TERMINAL_PROFILE_DELETE_BINDING_KEY))
  vte_terminal_set_delete_binding (vte_terminal,
                                   VteEraseBinding(g_settings_get_enum (profile, TERMINAL_PROFILE_DELETE_BINDING_KEY)));

  if (!prop_name || prop_name == I_(TERMINAL_PROFILE_ENABLE_BIDI_KEY))
    vte_terminal_set_enable_bidi (vte_terminal,
                                  g_settings_get_boolean (profile, TERMINAL_PROFILE_ENABLE_BIDI_KEY));
  if (!prop_name || prop_name == I_(TERMINAL_PROFILE_ENABLE_SHAPING_KEY))
    vte_terminal_set_enable_shaping (vte_terminal,
                                     g_settings_get_boolean (profile, TERMINAL_PROFILE_ENABLE_SHAPING_KEY));

  if (!prop_name || prop_name == I_(TERMINAL_PROFILE_ENABLE_SIXEL_KEY))
    vte_terminal_set_enable_sixel (vte_terminal,
                                   g_settings_get_boolean (profile, TERMINAL_PROFILE_ENABLE_SIXEL_KEY));

  if (!prop_name || prop_name == I_(TERMINAL_PROFILE_BOLD_IS_BRIGHT_KEY))
    vte_terminal_set_bold_is_bright (vte_terminal,
                                     g_settings_get_boolean (profile, TERMINAL_PROFILE_BOLD_IS_BRIGHT_KEY));

  if (!prop_name || prop_name == I_(TERMINAL_PROFILE_CURSOR_BLINK_MODE_KEY))
    vte_terminal_set_cursor_blink_mode (vte_terminal,
                                        VteCursorBlinkMode(g_settings_get_enum (priv->profile, TERMINAL_PROFILE_CURSOR_BLINK_MODE_KEY)));

  if (!prop_name || prop_name == I_(TERMINAL_PROFILE_CURSOR_SHAPE_KEY))
    vte_terminal_set_cursor_shape (vte_terminal,
                                   VteCursorShape(g_settings_get_enum (priv->profile, TERMINAL_PROFILE_CURSOR_SHAPE_KEY)));

  if (!prop_name || prop_name == I_(TERMINAL_PROFILE_REWRAP_ON_RESIZE_KEY))
    vte_terminal_set_rewrap_on_resize (vte_terminal,
                                       g_settings_get_boolean (profile, TERMINAL_PROFILE_REWRAP_ON_RESIZE_KEY));

  if (!prop_name || prop_name == I_(TERMINAL_PROFILE_TEXT_BLINK_MODE_KEY))
    vte_terminal_set_text_blink_mode (vte_terminal,
                                      VteTextBlinkMode(g_settings_get_enum (profile, TERMINAL_PROFILE_TEXT_BLINK_MODE_KEY)));

  if (!prop_name || prop_name == I_(TERMINAL_PROFILE_WORD_CHAR_EXCEPTIONS_KEY))
    {
      gs_free char *word_char_exceptions;
      g_settings_get (profile, TERMINAL_PROFILE_WORD_CHAR_EXCEPTIONS_KEY, "ms", &word_char_exceptions);
      vte_terminal_set_word_char_exceptions (vte_terminal, word_char_exceptions);
    }

  g_object_thaw_notify (object);
}

static void
update_color_scheme (TerminalScreen *screen)
{
  TerminalScreenPrivate *priv = screen->priv;
  GSettings *profile = priv->profile;
  gs_free GdkRGBA *colors;
  gsize n_colors = 0;
  GdkRGBA fg, bg, bold;
  GdkRGBA cursor_bg, cursor_fg;
  GdkRGBA highlight_bg, highlight_fg;
  GdkRGBA *boldp;
  GdkRGBA *cursor_bgp = nullptr, *cursor_fgp = nullptr;
  GdkRGBA *highlight_bgp = nullptr, *highlight_fgp = nullptr;
  gboolean use_theme_colors;

  colors = terminal_g_settings_get_rgba_palette (priv->profile, TERMINAL_PROFILE_PALETTE_KEY, &n_colors);

  use_theme_colors = g_settings_get_boolean (profile, TERMINAL_PROFILE_USE_THEME_COLORS_KEY);
  if (!terminal_g_settings_get_rgba (profile, TERMINAL_PROFILE_FOREGROUND_COLOR_KEY, &fg) ||
      !terminal_g_settings_get_rgba (profile, TERMINAL_PROFILE_BACKGROUND_COLOR_KEY, &bg)) {
    use_theme_colors = true;
  }

  if (use_theme_colors) {
    auto const app = terminal_app_get();
    auto const style_manager = reinterpret_cast<AdwStyleManager*>(terminal_app_get_adw_style_manager(app));

    // Use the style that is actually in effect. With the "system" theme
    // variant the requested color scheme is PREFER_LIGHT, but the system
    // may still have selected the dark style.
    if (!adw_style_manager_get_dark(style_manager)) {
      if (n_colors >= 16) {
        fg = colors[0];
        bg = colors[15];
      } else if (n_colors >= 8) {
        fg = colors[0];
        bg = colors[7];
      } else {
        fg = {0, 0, 0, 1};
        bg = {1, 1, 1, 1};
      }
    } else {
      if (n_colors >= 16) {
        fg = colors[15];
        bg = colors[0];
      } else if (n_colors >= 8) {
        fg = colors[7];
        bg = colors[0];
      } else {
        fg = {1, 1, 1, 1};
        bg = {0, 0, 0, 1};
      }
    }
  }

  if (!g_settings_get_boolean (profile, TERMINAL_PROFILE_BOLD_COLOR_SAME_AS_FG_KEY) &&
      !use_theme_colors &&
      terminal_g_settings_get_rgba (profile, TERMINAL_PROFILE_BOLD_COLOR_KEY, &bold))
    boldp = &bold;
  else
    boldp = nullptr;

  if (g_settings_get_boolean (profile, TERMINAL_PROFILE_CURSOR_COLORS_SET_KEY) &&
      !use_theme_colors)
    {
      if (terminal_g_settings_get_rgba (profile, TERMINAL_PROFILE_CURSOR_BACKGROUND_COLOR_KEY, &cursor_bg))
        cursor_bgp = &cursor_bg;
      if (terminal_g_settings_get_rgba (profile, TERMINAL_PROFILE_CURSOR_FOREGROUND_COLOR_KEY, &cursor_fg))
        cursor_fgp = &cursor_fg;
    }

  if (g_settings_get_boolean (profile, TERMINAL_PROFILE_HIGHLIGHT_COLORS_SET_KEY) &&
      !use_theme_colors)
    {
      if (terminal_g_settings_get_rgba (profile, TERMINAL_PROFILE_HIGHLIGHT_BACKGROUND_COLOR_KEY, &highlight_bg))
        highlight_bgp = &highlight_bg;
      if (terminal_g_settings_get_rgba (profile, TERMINAL_PROFILE_HIGHLIGHT_FOREGROUND_COLOR_KEY, &highlight_fg))
        highlight_fgp = &highlight_fg;
    }

  /* A color scheme replaces the profile's own colors, in the variant for
   * the style in use */
  gs_free char* scheme_id = g_settings_get_string (profile, TERMINAL_PROFILE_COLOR_SCHEME_KEY);
  auto const scheme = terminal_schemes_lookup (terminal_schemes_get_default (), scheme_id);
  GdkRGBA scheme_cursor;
  if (scheme) {
    auto const style_manager = reinterpret_cast<AdwStyleManager*>(terminal_app_get_adw_style_manager(terminal_app_get()));
    auto const scheme_colors = terminal_scheme_get_colors (scheme, adw_style_manager_get_dark (style_manager));
    fg = scheme_colors->foreground;
    bg = scheme_colors->background;
    g_free (colors);
    colors = (GdkRGBA*) g_memdup2 (scheme_colors->palette, sizeof (scheme_colors->palette));
    n_colors = TERMINAL_SCHEME_PALETTE_SIZE;
    boldp = nullptr;
    if (scheme_colors->has_cursor && !cursor_bgp) {
      scheme_cursor = scheme_colors->cursor;
      cursor_bgp = &scheme_cursor;
    }
  }

  /* Only the background is translucent; the window lets the desktop show
   * through the terminal area, see window_sync_translucency(). */
  bg.alpha = float(g_settings_get_double (profile, TERMINAL_PROFILE_BACKGROUND_OPACITY_KEY));

  vte_terminal_set_colors (VTE_TERMINAL (screen), &fg, &bg,
                           colors, n_colors);
  vte_terminal_set_color_bold (VTE_TERMINAL (screen), boldp);
  vte_terminal_set_color_cursor (VTE_TERMINAL (screen), cursor_bgp);
  vte_terminal_set_color_cursor_foreground (VTE_TERMINAL (screen), cursor_fgp);
  vte_terminal_set_color_highlight (VTE_TERMINAL (screen), highlight_bgp);
  vte_terminal_set_color_highlight_foreground (VTE_TERMINAL (screen), highlight_fgp);

  // The window may color its header bar alike
  priv->has_custom_colors = scheme != nullptr || !use_theme_colors;
  priv->custom_foreground = fg;
  priv->custom_background = bg;
  priv->custom_background.alpha = 1.0f;
  g_signal_emit (screen, signals[COLORS_CHANGED], 0);
}

/**
 * terminal_screen_get_custom_colors:
 * @screen: a #TerminalScreen
 * @foreground: (out): the foreground color
 * @background: (out): the background color, opaque
 *
 * Returns: whether @screen uses colors of its own, from a color scheme or
 *   its profile, rather than those of the system style
 */
gboolean
terminal_screen_get_custom_colors (TerminalScreen *screen,
                                   GdkRGBA *foreground,
                                   GdkRGBA *background)
{
  g_return_val_if_fail (TERMINAL_IS_SCREEN (screen), FALSE);

  auto const priv = screen->priv;
  *foreground = priv->custom_foreground;
  *background = priv->custom_background;
  return priv->has_custom_colors;
}

static void
terminal_screen_set_font (TerminalScreen *screen)
{
  TerminalScreenPrivate *priv = screen->priv;
  GSettings *profile = priv->profile;
  PangoFontDescription *desc;
  int size;

  if (g_settings_get_boolean (profile, TERMINAL_PROFILE_USE_SYSTEM_FONT_KEY))
    {
      desc = terminal_app_get_system_font (terminal_app_get ());
    }
  else
    {
      gs_free char *font;
      font = g_settings_get_string (profile, TERMINAL_PROFILE_FONT_KEY);
      desc = pango_font_description_from_string (font);
    }

  size = pango_font_description_get_size (desc);
  /* Sanity check */
  if (size == 0) {
    if (pango_font_description_get_size_is_absolute (desc))
      pango_font_description_set_absolute_size (desc, 10);
    else
      pango_font_description_set_size (desc, 10);
  }

  vte_terminal_set_font (VTE_TERMINAL (screen), desc);

  pango_font_description_free (desc);

  vte_terminal_set_cell_width_scale (VTE_TERMINAL (screen),
                                     g_settings_get_double (profile, TERMINAL_PROFILE_CELL_WIDTH_SCALE_KEY));
  vte_terminal_set_cell_height_scale (VTE_TERMINAL (screen),
                                      g_settings_get_double (profile, TERMINAL_PROFILE_CELL_HEIGHT_SCALE_KEY));
}

static void
terminal_screen_system_font_changed_cb (GSettings      *settings,
                                        const char     *key,
                                        TerminalScreen *screen)
{
  TerminalScreenPrivate *priv = screen->priv;

  if (!gtk_widget_get_realized (GTK_WIDGET (screen)))
    return;

  if (!g_settings_get_boolean (priv->profile, TERMINAL_PROFILE_USE_SYSTEM_FONT_KEY))
    return;

  terminal_screen_set_font (screen);
}

void
terminal_screen_set_profile (TerminalScreen *screen,
                             GSettings *profile)
{
  TerminalScreenPrivate *priv = screen->priv;
  GSettings*old_profile;

  old_profile = priv->profile;
  if (profile == old_profile)
    return;

  if (priv->profile_changed_id)
    {
      g_signal_handler_disconnect (G_OBJECT (priv->profile),
                                   priv->profile_changed_id);
      priv->profile_changed_id = 0;
    }

  priv->profile = profile;
  if (profile)
    {
      g_object_ref (profile);
      priv->profile_changed_id =
        g_signal_connect (profile, "changed",
                          G_CALLBACK (terminal_screen_profile_changed_cb),
                          screen);
      terminal_screen_profile_changed_cb (profile, nullptr, screen);

      g_signal_emit (G_OBJECT (screen), signals[PROFILE_SET], 0, old_profile);
    }

  if (old_profile)
    g_object_unref (old_profile);

  g_object_notify_by_pspec (G_OBJECT (screen), pspecs[PROP_PROFILE]);
}

GSettings*
terminal_screen_get_profile (TerminalScreen *screen)
{
  TerminalScreenPrivate *priv = screen->priv;

  return priv->profile;
}

GSettings*
terminal_screen_ref_profile (TerminalScreen *screen)
{
  TerminalScreenPrivate *priv = screen->priv;

  if (priv->profile != nullptr)
    return (GSettings*)g_object_ref (priv->profile);
  return nullptr;
}

/* Whether @arg0 names a shell of the host, for the sandbox */
static bool
is_host_shell (char const* arg0)
{
  gs_free char* name = g_path_get_basename (arg0);
  gs_free char* contents = nullptr;
  if (g_file_get_contents ("/run/host/etc/shells", &contents, nullptr, nullptr)) {
    gs_strfreev char** lines = g_strsplit (contents, "\n", -1);
    for (auto line = lines; *line; ++line) {
      g_strstrip (*line);
      if (**line == '/' && g_str_equal (strrchr (*line, '/') + 1, name))
        return true;
    }
  }

  static char const* const shells[] = {
    "sh", "bash", "zsh", "fish", "dash", "ksh", "mksh", "tcsh", "csh",
    "nu", "xonsh", "elvish", nullptr
  };
  return g_strv_contains (shells, name);
}

static gboolean
should_preserve_cwd (TerminalPreserveWorkingDirectory preserve_cwd,
                     const char *path,
                     const char *arg0)
{
  switch (preserve_cwd) {
  case TERMINAL_PRESERVE_WORKING_DIRECTORY_SAFE: {
    // The host's programs are not in the sandbox
    if (terminal_flatpak_is_sandboxed ())
      return is_host_shell (arg0);

    gs_free char *resolved_arg0 = terminal_util_find_program_in_path (path, arg0);
    return resolved_arg0 != nullptr &&
      terminal_util_get_is_shell (resolved_arg0);
  }

  case TERMINAL_PRESERVE_WORKING_DIRECTORY_ALWAYS:
    return TRUE;

  case TERMINAL_PRESERVE_WORKING_DIRECTORY_NEVER:
  default:
    return FALSE;
  }
}

/* The custom command of @profile, or %NULL when it has none: an empty or
 * blank custom command runs the shell */
static char *
profile_dup_custom_command (GSettings *profile)
{
  if (!g_settings_get_boolean (profile, TERMINAL_PROFILE_USE_CUSTOM_COMMAND_KEY))
    return nullptr;

  char *command = g_settings_get_string (profile, TERMINAL_PROFILE_CUSTOM_COMMAND_KEY);
  g_strstrip (command);
  if (!command[0])
    g_clear_pointer (&command, g_free);
  return command;
}

static gboolean
terminal_screen_get_child_command (TerminalScreen *screen,
                                   char          **argv,
                                   const char     *path_env,
                                   const char     *shell_env,
                                   gboolean        as_shell,
                                   gboolean       *preserve_cwd_p,
                                   GSpawnFlags    *spawn_flags_p,
                                   char         ***exec_argv_p,
                                   GError        **err)
{
  TerminalScreenPrivate *priv = screen->priv;
  GSettings *profile = priv->profile;
  TerminalPreserveWorkingDirectory preserve_cwd;
  char **exec_argv;

  g_assert (spawn_flags_p != nullptr && exec_argv_p != nullptr && preserve_cwd_p != nullptr);

  *exec_argv_p = exec_argv = nullptr;

  preserve_cwd = TerminalPreserveWorkingDirectory
    (g_settings_get_enum (profile, TERMINAL_PROFILE_PRESERVE_WORKING_DIRECTORY_KEY));

  gs_free char *custom_command = profile_dup_custom_command (profile);

  if (argv)
    {
      exec_argv = g_strdupv (argv);

      /* argv and cwd come from the command line client, so it must always be used */
      *preserve_cwd_p = TRUE;
      *spawn_flags_p = GSpawnFlags(*spawn_flags_p | G_SPAWN_SEARCH_PATH_FROM_ENVP);
    }
  else if (custom_command)
    {
      if (!g_shell_parse_argv (custom_command, nullptr, &exec_argv, err))
        return FALSE;

      *preserve_cwd_p = should_preserve_cwd (preserve_cwd, path_env, exec_argv[0]);
      *spawn_flags_p = GSpawnFlags(*spawn_flags_p | G_SPAWN_SEARCH_PATH_FROM_ENVP);
    }
  else if (as_shell && priv->container_name)
    {
      /* The shell of a container, in the same working directory */
      auto const tool = g_str_equal (priv->container_runtime, "distrobox") ? "distrobox" : "toolbox";
      char const* const container_argv[] = { tool, "enter", priv->container_name, nullptr };
      exec_argv = g_strdupv ((char**) container_argv);

      *preserve_cwd_p = TRUE;
      *spawn_flags_p = GSpawnFlags(*spawn_flags_p | G_SPAWN_SEARCH_PATH_FROM_ENVP);
    }
  else if (as_shell)
    {
      const char *only_name;
      char *shell;
      int argc = 0;

      // In the sandbox, the user's shell on the host
      if (terminal_flatpak_is_sandboxed ())
        shell = g_strdup (terminal_flatpak_get_host_shell () ? terminal_flatpak_get_host_shell ()
                          : shell_env ? shell_env : "/bin/sh");
      else
        shell = egg_shell (shell_env);

      only_name = strrchr (shell, '/');
      if (only_name != nullptr)
        only_name++;
      else {
        only_name = shell;
        *spawn_flags_p = GSpawnFlags(*spawn_flags_p | G_SPAWN_SEARCH_PATH_FROM_ENVP);
      }

      exec_argv = g_new (char*, 3);

      exec_argv[argc++] = shell;

      if (g_settings_get_boolean (profile, TERMINAL_PROFILE_LOGIN_SHELL_KEY))
        exec_argv[argc++] = g_strconcat ("-", only_name, nullptr);
      else
        exec_argv[argc++] = g_strdup (only_name);

      exec_argv[argc++] = nullptr;

      *preserve_cwd_p = should_preserve_cwd (preserve_cwd, path_env, shell);
      *spawn_flags_p = GSpawnFlags(*spawn_flags_p | G_SPAWN_FILE_AND_ARGV_ZERO);
    }

  else
    {
      g_set_error_literal (err, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                           _("No command supplied nor shell requested"));
      return FALSE;
    }

  *exec_argv_p = exec_argv;

  return TRUE;
}

/* Makes the default shell report its prompts and commands, see
 * data/shell-integration/README.md. @exec_argv is the shell's file and its
 * argv[0], for G_SPAWN_FILE_AND_ARGV_ZERO. */
static void
terminal_screen_add_shell_integration(TerminalScreen* screen,
                                      char** argv,
                                      gboolean as_shell,
                                      char*** exec_argv_p,
                                      char*** envv_p)
{
  auto const priv = screen->priv;
  auto const settings = terminal_app_get_global_settings(terminal_app_get());
  gs_free char* custom_command = profile_dup_custom_command(priv->profile);
  if (argv || !as_shell || priv->container_name || custom_command ||
      !g_settings_get_boolean(settings, TERMINAL_SETTING_INJECT_SHELL_INTEGRATION_KEY))
    return;

  auto const exec_argv = *exec_argv_p;
  if (!exec_argv || !exec_argv[0] || !exec_argv[1] || exec_argv[2])
    return;

  gs_free char* dir = g_build_filename(TERM_PKGDATADIR, "shell-integration", nullptr);
  if (!g_file_test(dir, G_FILE_TEST_IS_DIR))
    return;

  // The shell may run on the host, which has the files elsewhere
  gs_free char* sandbox_dir = g_steal_pointer(&dir);
  dir = terminal_flatpak_host_path(sandbox_dir);

  gs_free char* name = g_path_get_basename(exec_argv[0]);
  auto const login = g_settings_get_boolean(priv->profile, TERMINAL_PROFILE_LOGIN_SHELL_KEY);

  if (g_str_equal(name, "bash")) {
    // A login shell reads other files than the one we replace
    if (login)
      return;

    char* new_argv[] = {
      exec_argv[0], exec_argv[1],
      g_strdup("--rcfile"),
      g_build_filename(dir, "bash", "pano.bashrc", nullptr),
      nullptr
    };
    g_free(exec_argv);
    *exec_argv_p = static_cast<char**>(g_memdup2(new_argv, sizeof(new_argv)));
  } else if (g_str_equal(name, "zsh")) {
    auto envv = *envv_p;
    if (auto const zdotdir = g_environ_getenv(envv, "ZDOTDIR"))
      envv = g_environ_setenv(envv, "PANO_ZDOTDIR", zdotdir, true);
    gs_free char* zsh_dir = g_build_filename(dir, "zsh", nullptr);
    *envv_p = g_environ_setenv(envv, "ZDOTDIR", zsh_dir, true);
  } else if (g_str_equal(name, "fish")) {
    auto envv = *envv_p;
    auto const data_dirs = g_environ_getenv(envv, "XDG_DATA_DIRS");
    if (data_dirs)
      envv = g_environ_setenv(envv, "PANO_XDG_DATA_DIRS", data_dirs, true);
    gs_free char* value = g_strconcat(dir, ":",
                                      data_dirs && data_dirs[0] ? data_dirs : "/usr/local/share:/usr/share",
                                      nullptr);
    envv = g_environ_setenv(envv, "XDG_DATA_DIRS", value, true);
    *envv_p = g_environ_setenv(envv, "PANO_SHELL_INTEGRATION", "1", true);
  }
}

static gboolean
remove_prefixed_cb(void* key,
                   void* value,
                   void* user_data)
{
  auto const env = reinterpret_cast<char const*>(key);
  auto const prefix = reinterpret_cast<char const*>(user_data);

  if (terminal_client_get_environment_prefix_filters_is_excluded(env))
    return false;

  return g_str_has_prefix(env, prefix);
}

static char**
terminal_screen_get_child_environment (TerminalScreen *screen,
                                       char **initial_envv,
                                       char **path,
                                       char **shell)
{
  TerminalApp *app = terminal_app_get ();
  char **env;
  gs_strfreev char** current_environ = nullptr;
  char *e, *v;
  GHashTable *env_table;
  GHashTableIter iter;
  GPtrArray *retval;
  guint i;

  env_table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  /* In the sandbox, the command runs on the host, so it gets the host's
   * environment; the one of the client is the sandbox's too */
  gs_strfreev char** host_environ = terminal_flatpak_is_sandboxed ()
    ? terminal_flatpak_dup_host_environ () : nullptr;
  if (host_environ)
    env = host_environ;
  else if (initial_envv)
    env = initial_envv;
  else {
    env = current_environ = g_get_environ ();
    /* Remove this variable which we set in server.c:main() */
    env = g_environ_unsetenv (env, "G_ENABLE_DIAGNOSTIC");
  }

  for (i = 0; env[i]; ++i)
    {
      v = strchr (env[i], '=');
      if (v)
          g_hash_table_replace (env_table, g_strndup (env[i], v - env[i]), g_strdup (v + 1));
        else
          g_hash_table_replace (env_table, g_strdup (env[i]), nullptr);
    }

  /* Remove unwanted env variables */
  auto const filters = terminal_client_get_environment_filters ();
  for (i = 0; filters[i]; ++i)
    g_hash_table_remove (env_table, filters[i]);

  auto const pfilters = terminal_client_get_environment_prefix_filters ();
  for (i = 0; pfilters[i]; ++i) {
    g_hash_table_foreach_remove (env_table,
                                 GHRFunc(remove_prefixed_cb),
                                 (void*)pfilters[i]);
  }

  terminal_util_add_proxy_env (env_table);

  // Force ncurses to not use the dec graphics charset
  g_hash_table_replace (env_table, g_strdup("NCURSES_NO_UTF8_ACS"), g_strdup("1"));

  // VTE adds these when it starts the command itself, which it does not on the host
  if (terminal_flatpak_is_sandboxed ()) {
    g_hash_table_replace (env_table, g_strdup ("TERM"), g_strdup ("xterm-256color"));
    g_hash_table_replace (env_table, g_strdup ("COLORTERM"), g_strdup ("truecolor"));
    g_hash_table_replace (env_table, g_strdup ("VTE_VERSION"),
                          g_strdup_printf ("%u", vte_get_major_version () * 10000 +
                                           vte_get_minor_version () * 100 +
                                           vte_get_micro_version ()));
  }

  /* Add Pano private env vars used to communicate back to pano-terminal-server */
  GDBusConnection *connection = g_application_get_dbus_connection (G_APPLICATION (app));
  g_hash_table_replace (env_table, g_strdup (TERMINAL_ENV_SERVICE_NAME),
                        g_strdup (g_dbus_connection_get_unique_name (connection)));

  g_hash_table_replace (env_table, g_strdup (TERMINAL_ENV_SCREEN),
                        terminal_app_dup_screen_object_path (app, screen));

  /* Convert to strv */
  retval = g_ptr_array_sized_new (g_hash_table_size (env_table));
  g_hash_table_iter_init (&iter, env_table);
  while (g_hash_table_iter_next (&iter, (gpointer *) &e, (gpointer *) &v))
    g_ptr_array_add (retval, g_strdup_printf ("%s=%s", e, v ? v : ""));
  g_ptr_array_add (retval, nullptr);

  *path = g_strdup ((char const*)g_hash_table_lookup (env_table, "PATH"));
  *shell = g_strdup ((char const*)g_hash_table_lookup (env_table, "SHELL"));

  g_hash_table_destroy (env_table);
  return (char **) g_ptr_array_free (retval, FALSE);
}

enum {
  RESPONSE_RELAUNCH,
  RESPONSE_EDIT_PREFERENCES,
  RESPONSE_RUN_SHELL
};

static char* profile_dup_custom_command (GSettings *profile);

/* Starts the user's shell in the terminal of a command that has exited,
 * in the command's working directory */
static void
terminal_screen_run_shell (TerminalScreen *screen)
{
  auto const data = screen->priv->exec_data;
  char *argv[] = { vte_get_user_shell (), nullptr };
  if (!argv[0])
    argv[0] = g_strdup ("/bin/sh");

  gs_free_error GError *error = nullptr;
  if (!terminal_screen_exec (screen, argv,
                             data ? data->envv : nullptr,
                             FALSE,
                             data ? data->cwd : nullptr,
                             TRUE,
                             nullptr, nullptr, /* fd list and array */
                             nullptr, nullptr, nullptr, /* callback */
                             nullptr, &error))
    g_warning ("Failed to start the shell: %s", error->message);
  g_free (argv[0]);
}

static void
info_bar_response_cb (GtkWidget *info_bar,
                      int response,
                      TerminalScreen *screen)
{
  gtk_widget_grab_focus (GTK_WIDGET (screen));

  switch (response) {
    case GTK_RESPONSE_CANCEL:
      terminal_tab_remove_overlay (info_bar);
      g_signal_emit (screen, signals[CLOSE_SCREEN], 0);
      break;
    case RESPONSE_RELAUNCH:
      terminal_tab_remove_overlay (info_bar);
      terminal_screen_reexec (screen, nullptr, nullptr, nullptr, nullptr);
      break;
    case RESPONSE_RUN_SHELL:
      terminal_tab_remove_overlay (info_bar);
      terminal_screen_run_shell (screen);
      break;
    case RESPONSE_EDIT_PREFERENCES:
      terminal_app_edit_preferences (terminal_app_get (),
                                     terminal_screen_get_profile (screen),
                                     "custom-command-entry");
      break;
    default:
      terminal_tab_remove_overlay (info_bar);
      break;
  }
}

static void
terminal_screen_show_info_bar (TerminalScreen *screen,
                               GError *error,
                               gboolean show_relaunch)
{
  GtkWidget *info_bar;

  if (!gtk_widget_get_parent (GTK_WIDGET (screen)))
    return;

  info_bar = terminal_info_bar_new (GTK_MESSAGE_ERROR,
                                    _("_Preferences"), RESPONSE_EDIT_PREFERENCES,
                                    !show_relaunch ? nullptr : _("_Relaunch"), RESPONSE_RELAUNCH,
                                    nullptr);
  terminal_info_bar_format_text (TERMINAL_INFO_BAR (info_bar),
                                 _("There was an error creating the child process for this terminal"));
  terminal_info_bar_format_text (TERMINAL_INFO_BAR (info_bar),
                                 "%s", error->message);
  g_signal_connect (info_bar, "response",
                    G_CALLBACK (info_bar_response_cb), screen);

  gtk_widget_set_halign (info_bar, GTK_ALIGN_FILL);
  gtk_widget_set_valign (info_bar, GTK_ALIGN_START);
  terminal_tab_add_screen_overlay(screen, info_bar);
  terminal_info_bar_set_default_response (TERMINAL_INFO_BAR (info_bar), GTK_RESPONSE_CANCEL);
  gtk_widget_show (info_bar);
}

static void
spawn_result_cb (VteTerminal *terminal,
                 GPid pid,
                 GError *error,
                 gpointer user_data)
{
  TerminalScreen *screen = TERMINAL_SCREEN (terminal);
  ExecData *exec_data = (ExecData*)user_data;

  /* Terminal was destroyed while the spawn operation was in progress; nothing to do. */
  if (terminal == nullptr)
    goto out;

  {
  TerminalScreenPrivate *priv = screen->priv;

  priv->child_pid = pid;

  if (error) {
     // FIXMEchpe should be unnecessary, vte already does this internally
    vte_terminal_set_pty (terminal, nullptr);

    gboolean can_reexec = TRUE; /* FIXME */
    terminal_screen_show_info_bar (screen, error, can_reexec);
  }

  /* Retain info for reexec, if possible */
  ExecData *new_exec_data = exec_data_clone (exec_data);
  terminal_screen_clear_exec_data (screen, FALSE);
  priv->exec_data = new_exec_data;
  }

out:

  /* Must do this even if the terminal was destroyed */
  exec_data_callback (exec_data, error, screen);

  exec_data_unref (exec_data);
}

static gboolean
idle_exec_cb (TerminalScreen *screen)
{
  TerminalScreenPrivate *priv = screen->priv;

  priv->idle_exec_source = 0;

  ExecData *data = priv->exec_data;
  _TERMINAL_DEBUG_IF (TERMINAL_DEBUG_PROCESSES) {
    gs_free char *str = exec_data_to_string (data);
    _terminal_debug_print (TERMINAL_DEBUG_PROCESSES,
                           "[screen %p] now launching the child process: %s\n",
                           screen, str);
  }

  int n_fds;
  int *fds;
  if (data->fd_list) {
    fds = g_unix_fd_list_steal_fds(data->fd_list, &n_fds);
  } else {
    fds = nullptr;
    n_fds = 0;
  }

  VteTerminal *terminal = VTE_TERMINAL (screen);
  if (terminal_flatpak_is_sandboxed ()) {
    terminal_flatpak_spawn_async (terminal,
                                  data->pty_flags,
                                  data->cwd,
                                  (char const* const*)data->exec_argv,
                                  (char const* const*)data->envv,
                                  fds, n_fds,
                                  data->fd_map, data->n_fd_map,
                                  data->spawn_flags,
                                  data->cancellable,
                                  spawn_result_cb,
                                  exec_data_ref (data));
    return FALSE;
  }

  vte_terminal_spawn_with_fds_async (terminal,
                                     data->pty_flags,
                                     data->cwd,
                                     (char const* const*)data->exec_argv,
                                     (char const* const*)data->envv,
                                     fds, n_fds,
                                     data->fd_map, data->n_fd_map,
                                     data->spawn_flags,
                                     nullptr, nullptr, nullptr, /* child setup, data, destroy */
                                     -1,
                                     data->cancellable,
                                     spawn_result_cb,
                                     exec_data_ref (data));

  return FALSE; /* don't run again */
}

static void
terminal_screen_queue_idle_exec (TerminalScreen *screen)
{
  TerminalScreenPrivate *priv = screen->priv;

  if (priv->idle_exec_source != 0)
    return;

  if (!gtk_widget_get_realized (GTK_WIDGET (screen))) {
    priv->exec_on_realize = TRUE;
    return;
  }

  _terminal_debug_print (TERMINAL_DEBUG_PROCESSES,
                         "[screen %p] scheduling launching the child process on idle\n",
                         screen);

  priv->idle_exec_source = g_idle_add ((GSourceFunc) idle_exec_cb, screen);
}

static TerminalScreenPopupInfo *
terminal_screen_popup_info_new (TerminalScreen *screen)
{
  TerminalScreenPopupInfo *info;

  info = g_slice_new0 (TerminalScreenPopupInfo);
  info->ref_count = 1;

  return info;
}

TerminalScreenPopupInfo *
terminal_screen_popup_info_ref (TerminalScreenPopupInfo *info)
{
  g_return_val_if_fail (info != nullptr, nullptr);

  info->ref_count++;
  return info;
}

void
terminal_screen_popup_info_unref (TerminalScreenPopupInfo *info)
{
  g_return_if_fail (info != nullptr);

  if (--info->ref_count > 0)
    return;

  g_free (info->hyperlink);
  g_free (info->url);
  g_free (info->number_info);
  g_free (info->timestamp_info);
  g_slice_free (TerminalScreenPopupInfo, info);
}

static void
terminal_screen_do_popup (TerminalScreen *screen,
                          int state,
                          guint button,
                          guint time,
                          double x,
                          double y,
                          char *hyperlink,
                          char *url,
                          int url_flavor,
                          char *number_info,
                          char *timestamp_info)
{
  TerminalScreenPopupInfo *info;

  info = terminal_screen_popup_info_new (screen);
  info->button = button;
  info->state = state;
  info->timestamp = time;
  info->x = x;
  info->y = y;
  info->hyperlink = hyperlink; /* adopted */
  info->url = url; /* adopted */
  info->url_flavor = TerminalURLFlavor(url_flavor);
  info->number_info = number_info; /* adopted */
  info->timestamp_info = timestamp_info; /* adopted */
  // A menu opened from the keyboard has no pointer position
  info->row = button != 0 ? terminal_screen_get_row_at (screen, y) : -1;

  g_signal_emit (screen, signals[SHOW_POPUP_MENU], 0, info);
  terminal_screen_popup_info_unref (info);
}

static void
terminal_screen_menu_popup_action (GtkWidget *widget,
                                   const char *action_name,
                                   GVariant *param)
{
  TerminalScreen *screen = TERMINAL_SCREEN (widget);

  terminal_screen_do_popup (screen, 0, 0, GDK_CURRENT_TIME, 0, 0,
                            nullptr, nullptr, 0, nullptr, nullptr);
}

static void
terminal_screen_bubble_click_pressed_cb (TerminalScreen  *screen,
                                         int              n_press,
                                         double           x,
                                         double           y,
                                         GtkGestureClick *click)
{
  if (n_press == 1) {
    gs_free char *hyperlink = nullptr;
    gs_free char *url = nullptr;
    int url_flavor = 0;
    gs_free char *number_info = nullptr;
    gs_free char *timestamp_info = nullptr;

    auto event = gtk_event_controller_get_current_event (GTK_EVENT_CONTROLLER (click));
    auto state = gdk_event_get_modifier_state (event) & gtk_accelerator_get_default_mod_mask ();
    auto button = gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (click));
    auto time = gdk_event_get_time (event);

    hyperlink = vte_terminal_check_hyperlink_at (VTE_TERMINAL (screen), x, y);
    url = terminal_screen_check_match (screen, x, y, &url_flavor);
    terminal_screen_check_extra (screen, x, y, &number_info, &timestamp_info);

    if (button == 3)
      {
        if (!(state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK | GDK_ALT_MASK)) ||
            !(state & (GDK_CONTROL_MASK | GDK_ALT_MASK)))
          {
            terminal_screen_do_popup (screen, state, button, time, x, y,
                                      g_steal_pointer (&hyperlink),
                                      g_steal_pointer (&url),
                                      url_flavor,
                                      g_steal_pointer (&number_info),
                                      g_steal_pointer (&timestamp_info));
            gtk_gesture_set_state (GTK_GESTURE (click), GTK_EVENT_SEQUENCE_CLAIMED);
            return;
          }
      }
  }

  gtk_gesture_set_state (GTK_GESTURE (click), GTK_EVENT_SEQUENCE_DENIED);
}

/* A long press on a touchscreen opens the context menu, as a right click
 * does; without it, a touchscreen could not, for example, leave fullscreen */
static void
terminal_screen_long_pressed_cb (GtkGestureLongPress *gesture,
                                 double               x,
                                 double               y,
                                 TerminalScreen      *screen)
{
  int url_flavor = 0;
  gs_free char *number_info = nullptr;
  gs_free char *timestamp_info = nullptr;
  auto hyperlink = vte_terminal_check_hyperlink_at (VTE_TERMINAL (screen), x, y);
  auto url = terminal_screen_check_match (screen, x, y, &url_flavor);
  terminal_screen_check_extra (screen, x, y, &number_info, &timestamp_info);

  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  terminal_screen_do_popup (screen, 0, 3, GDK_CURRENT_TIME, x, y,
                            hyperlink, url, url_flavor,
                            g_steal_pointer (&number_info),
                            g_steal_pointer (&timestamp_info));
}

/* A double click selects a word, and VTE does not count ":" as part of
 * words, so a double click on "https://example.com" would leave out
 * "https:". While a double click on a link selects, count it in. */
static void
terminal_screen_link_selection_released_cb (TerminalScreen  *screen,
                                            int              n_press,
                                            double           x,
                                            double           y,
                                            GtkGestureClick *click)
{
  auto const priv = screen->priv;
  g_signal_handlers_disconnect_by_func (click, (void*)terminal_screen_link_selection_released_cb, screen);

  if (priv->saved_word_chars) {
    vte_terminal_set_word_char_exceptions (VTE_TERMINAL (screen),
                                           priv->saved_word_chars[0] ? priv->saved_word_chars : nullptr);
    g_clear_pointer (&priv->saved_word_chars, g_free);
  }
}

static void
terminal_screen_select_whole_link (TerminalScreen  *screen,
                                   GtkGestureClick *click)
{
  auto const priv = screen->priv;
  if (priv->saved_word_chars)
    return;

  auto const current = vte_terminal_get_word_char_exceptions (VTE_TERMINAL (screen));
  priv->saved_word_chars = g_strdup (current ? current : "");
  gs_free char *with_colon = g_strconcat (current ? current : "-#%&+,./=?@\\_~\302\267", ":", nullptr);
  vte_terminal_set_word_char_exceptions (VTE_TERMINAL (screen), with_colon);

  g_signal_connect_swapped (click, "released",
                            G_CALLBACK (terminal_screen_link_selection_released_cb), screen);
}

static void
terminal_screen_capture_click_pressed_cb (TerminalScreen  *screen,
                                          int              n_press,
                                          double           x,
                                          double           y,
                                          GtkGestureClick *click)
{
  gs_free char *hyperlink = nullptr;
  gs_free char *url = nullptr;
  int url_flavor = 0;
  gs_free char *number_info = nullptr;
  gs_free char *timestamp_info = nullptr;
  gboolean handled = FALSE;

  auto event = gtk_event_controller_get_current_event (GTK_EVENT_CONTROLLER (click));
  auto state = gdk_event_get_modifier_state (event) & gtk_accelerator_get_default_mod_mask ();
  auto button = gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (click));

  hyperlink = vte_terminal_check_hyperlink_at (VTE_TERMINAL (screen), x, y);
  url = terminal_screen_check_match (screen, x, y, &url_flavor);
  terminal_screen_check_extra (screen, x, y, &number_info, &timestamp_info);

  if (n_press == 2 && button == 1 && url != nullptr && !(state & GDK_CONTROL_MASK))
    terminal_screen_select_whole_link (screen, click);

  if (n_press == 1 &&
      !handled &&
      hyperlink != nullptr &&
      (button == 1 || button == 2) &&
      (state & GDK_CONTROL_MASK))
    {
      g_signal_emit (screen, signals[MATCH_CLICKED], 0,
                     hyperlink,
                     FLAVOR_AS_IS,
                     state,
                     &handled);
    }

  if (n_press == 1 &&
      !handled &&
      url != nullptr &&
      (button == 1 || button == 2) &&
      (state & GDK_CONTROL_MASK))
    {
      g_signal_emit (screen, signals[MATCH_CLICKED], 0,
                     url,
                     url_flavor,
                     state,
                     &handled);
    }

  if (handled)
    gtk_gesture_set_state (GTK_GESTURE (click), GTK_EVENT_SEQUENCE_CLAIMED);
  else
    gtk_gesture_set_state (GTK_GESTURE (click), GTK_EVENT_SEQUENCE_DENIED);
}

/**
 * terminal_screen_get_current_dir:
 * @screen:
 *
 * Tries to determine the current working directory of the foreground process
 * in @screen's PTY.
 *
 * Returns: a newly allocated string containing the current working directory,
 *   or %nullptr on failure
 */
/* Whether @host, from an OSC 7 URI, names this computer. Shells report
 * $HOSTNAME, which may or may not include the domain. */
static gboolean
is_local_host (const char *host)
{
  if (host == nullptr || host[0] == '\0' ||
      g_ascii_strcasecmp (host, "localhost") == 0)
    return TRUE;

  auto const local = g_get_host_name ();
  if (g_ascii_strcasecmp (host, local) == 0)
    return TRUE;

  /* Compare the first labels: "pc" and "pc.example.com" */
  auto const host_len = strcspn (host, ".");
  auto const local_len = strcspn (local, ".");
  return host_len == local_len && g_ascii_strncasecmp (host, local, host_len) == 0;
}

char *
terminal_screen_get_current_dir (TerminalScreen *screen)
{
  const char *uri;

  /* The folder that the shell reports with OSC 7, unless it is on another
   * computer, for example after ssh. Terminals in a container report the
   * container's host name, but share the folders. */
  uri = vte_terminal_get_current_directory_uri (VTE_TERMINAL (screen));
  if (uri != nullptr) {
    gs_free char *host = nullptr;
    gs_free char *path = g_filename_from_uri (uri, &host, nullptr);
    if (path && (is_local_host (host) || screen->priv->container_name))
      return g_steal_pointer (&path);
  }

  /* Without shell integration (OSC 7), ask the system for the working
   * directory of the shell. This only works on Linux. */
  if (terminal_flatpak_is_sandboxed ()) {
    if (screen->priv->host_process && screen->priv->host_process->cwd)
      return g_strdup (screen->priv->host_process->cwd);
  } else if (screen->priv->child_pid > 0) {
    gs_free char *link = g_strdup_printf ("/proc/%d/cwd", int(screen->priv->child_pid));
    if (auto const cwd = g_file_read_link (link, nullptr))
      return cwd;
  }

  ExecData *data = screen->priv->exec_data;
  if (data && data->cwd)
    return g_strdup (data->cwd);

  return nullptr;
}

static void
terminal_screen_window_title_changed (VteTerminal *vte_terminal,
                                      TerminalScreen *screen)
{
  g_object_notify_by_pspec (G_OBJECT (screen), pspecs[PROP_TITLE]);
}

static void
terminal_screen_child_exited (VteTerminal *terminal,
                              int status)
{
  TerminalScreen *screen = TERMINAL_SCREEN (terminal);
  TerminalScreenPrivate *priv = screen->priv;
  TerminalExitAction action;

  /* Don't do anything if we don't have a child */
  if (priv->child_pid == -1)
    return;

  /* No need to chain up to VteTerminalClass::child_exited since it's nullptr */

  _terminal_debug_print (TERMINAL_DEBUG_PROCESSES,
                         "[screen %p] child process exited\n",
                         screen);

  priv->child_pid = -1;
  terminal_screen_update_process_kind (screen);

  action = TerminalExitAction(g_settings_get_enum (priv->profile, TERMINAL_PROFILE_EXIT_ACTION_KEY));
  // A pinned tab stays, so that it can be restarted
  auto const tab = terminal_tab_get_from_screen(screen);
  if (tab && terminal_tab_get_pinned(tab))
    action = TERMINAL_EXIT_HOLD;

  switch (action)
    {
    case TERMINAL_EXIT_CLOSE:
      g_signal_emit (screen, signals[CLOSE_SCREEN], 0);
      break;
    case TERMINAL_EXIT_RESTART:
      terminal_screen_reexec (screen, nullptr, nullptr, nullptr, nullptr);
      break;
    case TERMINAL_EXIT_HOLD: {
      GtkWidget *info_bar;

      /* Offer the shell, unless that is what exited */
      /* Nothing can be typed now, so the cursor does not blink */
      vte_terminal_set_cursor_blink_mode (VTE_TERMINAL (screen), VTE_CURSOR_BLINK_OFF);

      auto const data = priv->exec_data;
      gs_free char *custom_command = profile_dup_custom_command (priv->profile);
      auto const ran_command = data && (data->argv || custom_command);

      info_bar = terminal_info_bar_new (GTK_MESSAGE_INFO,
                                        _("_Relaunch"), RESPONSE_RELAUNCH,
                                        ran_command ? _("Run _Shell") : nullptr, RESPONSE_RUN_SHELL,
                                        nullptr);
      if (WIFEXITED (status)) {
        terminal_info_bar_format_text (TERMINAL_INFO_BAR (info_bar),
                                      _("The child process exited normally with status %d."), WEXITSTATUS (status));
      } else if (WIFSIGNALED (status)) {
        terminal_info_bar_format_text (TERMINAL_INFO_BAR (info_bar),
                                      _("The child process was aborted by signal %d."), WTERMSIG (status));
      } else {
        terminal_info_bar_format_text (TERMINAL_INFO_BAR (info_bar),
                                      _("The child process was aborted."));
      }
      g_signal_connect (info_bar, "response",
                        G_CALLBACK (info_bar_response_cb), screen);

      gtk_widget_set_halign (info_bar, GTK_ALIGN_FILL);
      gtk_widget_set_valign (info_bar, GTK_ALIGN_START);
      terminal_tab_add_screen_overlay(screen, info_bar);
      terminal_info_bar_set_default_response (TERMINAL_INFO_BAR (info_bar), RESPONSE_RELAUNCH);
      gtk_widget_show (info_bar);
      break;
    }

    default:
      break;
    }
}

static void
text_uri_list_free (TextUriList *uri_list)
{
  g_clear_object (&uri_list->screen);
  g_clear_object (&uri_list->drop);
  g_clear_list (&uri_list->files, g_object_unref);
  uri_list->mime_type = nullptr;
  g_free (uri_list);
}

static void
terminal_screen_drop_file_list (TerminalScreen *screen,
                                const GList    *files)
{
  g_assert (TERMINAL_IS_SCREEN (screen));
  g_assert (files == nullptr || G_IS_FILE (files->data));

  g_autoptr(GString) string = g_string_new (nullptr);

  for (const GList *iter = files; iter; iter = iter->next) {
    GFile *file = G_FILE (iter->data);

    if (g_file_is_native (file)) {
      g_autofree char *quoted = g_shell_quote (g_file_peek_path (file));

      g_string_append (string, quoted);
      g_string_append_c (string, ' ');
    } else {
      g_autofree char *uri = g_file_get_uri (file);
      g_autofree char *quoted = g_shell_quote (uri);

      g_string_append (string, quoted);
      g_string_append_c (string, ' ');
    }
  }

  if (string->len > 0)
    terminal_screen_paste_text (screen, string->str, string->len);
}

static void
terminal_screen_drop_uri_list_line_cb (GObject      *object,
                                       GAsyncResult *result,
                                       gpointer      user_data)
{
  GDataInputStream *line_reader = G_DATA_INPUT_STREAM (object);
  g_autoptr(TextUriList) state = (TextUriList*)user_data;
  g_autoptr(GError) error = nullptr;
  g_autofree char *line = nullptr;
  gsize len = 0;

  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (state != nullptr);
  g_assert (TERMINAL_IS_SCREEN (state->screen));
  g_assert (GDK_IS_DROP (state->drop));

  line = g_data_input_stream_read_line_finish_utf8 (line_reader, result, &len, &error);

  if (error != nullptr) {
    g_debug ("Failed to receive '%s': %s", state->mime_type, error->message);
    gdk_drop_finish (state->drop, GdkDragAction(0));
    return;
  }

  if (line != nullptr && line[0] != 0 && line[0] != '#') {
    GFile *file = g_file_new_for_uri (line);

    if (file != nullptr) {
      state->files = g_list_append (state->files, file);
    }
  }

  if (line == nullptr || g_strcmp0 (state->mime_type, TEXT_X_MOZ_URL) == 0) {
    terminal_screen_drop_file_list (state->screen, state->files);
    gdk_drop_finish (state->drop, GDK_ACTION_COPY);
    return;
  }

  g_data_input_stream_read_line_async (line_reader,
                                       DROP_REQUEST_PRIORITY,
                                       nullptr,
                                       terminal_screen_drop_uri_list_line_cb,
                                       g_steal_pointer (&state));
}

static void
terminal_screen_drop_uri_list_cb (GObject      *object,
                                  GAsyncResult *result,
                                  gpointer      user_data)
{
  GdkDrop *drop = (GdkDrop *)object;
  g_autoptr(TerminalScreen) screen = TERMINAL_SCREEN (user_data);
  g_autoptr(GError) error = nullptr;
  g_autoptr(GInputStream) stream = nullptr;
  g_autoptr(GDataInputStream) line_reader = nullptr;
  g_autoptr(TextUriList) state = nullptr;
  const char *mime_type = nullptr;

  g_assert (GDK_IS_DROP (drop));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (TERMINAL_IS_SCREEN (screen));

  if (!(stream = gdk_drop_read_finish (drop, result, &mime_type, &error))) {
    g_debug ("Failed to receive text/uri-list offer: %s", error->message);
    gdk_drop_finish (drop, GdkDragAction(0));
    return;
  }

  g_assert (g_strcmp0 (mime_type, TEXT_URI_LIST) == 0);
  g_assert (G_IS_INPUT_STREAM (stream));

  line_reader = g_data_input_stream_new (stream);
  g_data_input_stream_set_newline_type (line_reader,
                                        G_DATA_STREAM_NEWLINE_TYPE_CR_LF);

  state = g_new0 (TextUriList, 1);
  state->screen = g_object_ref (screen);
  state->drop = g_object_ref (drop);
  state->mime_type = g_intern_string (mime_type);

  g_data_input_stream_read_line_async (line_reader,
                                       DROP_REQUEST_PRIORITY,
                                       nullptr,
                                       terminal_screen_drop_uri_list_line_cb,
                                       g_steal_pointer (&state));
}

static void
terminal_screen_drop_file_list_cb (GObject      *object,
                                   GAsyncResult *result,
                                   gpointer      user_data)
{
  GdkDrop *drop = (GdkDrop *)object;
  g_autoptr(TerminalScreen) screen = TERMINAL_SCREEN (user_data);
  g_autoptr(GError) error = nullptr;
  const GValue *value;
  const GList *file_list;

  g_assert (GDK_IS_DROP (drop));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (TERMINAL_IS_SCREEN (screen));

  if (!(value = gdk_drop_read_value_finish (drop, result, &error))) {
    g_debug ("Failed to receive file-list offer: %s", error->message);

    /* If the user dragged a directory from Nautilus or another
     * new-style application, a portal request would be made. But
     * GTK won't be able to open the directory so the request for
     * APPLICATION_VND_PORTAL_FILETRANSFER will fail. Fallback to
     * opening the request via TEXT_URI_LIST gracefully.
     */
    if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) ||
        g_error_matches (error, G_IO_ERROR, G_DBUS_ERROR_ACCESS_DENIED))
      gdk_drop_read_async (drop,
                           (const char **)(const char * const[]){TEXT_URI_LIST, nullptr},
                           DROP_REQUEST_PRIORITY,
                           nullptr,
                           terminal_screen_drop_uri_list_cb,
                           g_object_ref (screen));
    else
      gdk_drop_finish (drop, GdkDragAction(0));

    return;
  }

  g_assert (value != nullptr);
  g_assert (G_VALUE_HOLDS (value, GDK_TYPE_FILE_LIST));

  file_list = (const GList *)g_value_get_boxed (value);
  terminal_screen_drop_file_list (screen, file_list);
  gdk_drop_finish (drop, GDK_ACTION_COPY);
}

static void
terminal_screen_drop_string_cb (GObject      *object,
                                GAsyncResult *result,
                                gpointer      user_data)
{
  GdkDrop *drop = (GdkDrop *)object;
  g_autoptr(TerminalScreen) screen = TERMINAL_SCREEN (user_data);
  g_autoptr(GError) error = nullptr;
  const GValue *value;
  const char *string;

  g_assert (GDK_IS_DROP (drop));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (TERMINAL_IS_SCREEN (screen));

  if (!(value = gdk_drop_read_value_finish (drop, result, &error))) {
    gdk_drop_finish (drop, GdkDragAction(0));
    return;
  }

  g_assert (value != nullptr);
  g_assert (G_VALUE_HOLDS_STRING (value));

  string = g_value_get_string (value);

  if (string != nullptr && string[0] != 0)
    terminal_screen_paste_text (screen, string, -1);

  gdk_drop_finish (drop, GDK_ACTION_COPY);
}

static void
terminal_screen_drop_color_cb (GObject      *object,
                               GAsyncResult *result,
                               gpointer      user_data)
{
  GdkDrop *drop = (GdkDrop *)object;
  g_autoptr(TerminalScreen) screen = TERMINAL_SCREEN (user_data);
  TerminalScreenPrivate *priv = screen->priv;
  g_autoptr(GError) error = nullptr;
  const GValue *value;
  const GdkRGBA *color;

  g_assert (GDK_IS_DROP (drop));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (TERMINAL_IS_SCREEN (screen));

  if (!(value = gdk_drop_read_value_finish (drop, result, &error))) {
    gdk_drop_finish (drop, GdkDragAction(0));
    return;
  }

  g_assert (value != nullptr);
  g_assert (G_VALUE_HOLDS (value, GDK_TYPE_RGBA));

  color = (const GdkRGBA *)g_value_get_boxed (value);
  terminal_g_settings_set_rgba (priv->profile,
                                TERMINAL_PROFILE_BACKGROUND_COLOR_KEY,
                                color);
  g_settings_set_boolean (priv->profile, TERMINAL_PROFILE_USE_THEME_COLORS_KEY, FALSE);

  gdk_drop_finish (drop, GDK_ACTION_COPY);
}

static void
terminal_screen_drop_moz_url_cb (GObject      *object,
                                 GAsyncResult *result,
                                 gpointer      user_data)
{
  GdkDrop *drop = (GdkDrop *)object;
  g_autoptr(TerminalScreen) screen = TERMINAL_SCREEN (user_data);
  g_autoptr(GCharsetConverter) converter = nullptr;
  g_autoptr(GDataInputStream) line_reader = nullptr;
  g_autoptr(GInputStream) converter_stream = nullptr;
  g_autoptr(GInputStream) stream = nullptr;
  g_autoptr(GError) error = nullptr;
  const char *mime_type = nullptr;
  TextUriList *state;

  g_assert (GDK_IS_DROP (drop));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (TERMINAL_IS_SCREEN (screen));

  if (!(stream = gdk_drop_read_finish (drop, result, &mime_type, &error))) {
    gdk_drop_finish (drop, GdkDragAction(0));
    return;
  }

  g_assert (G_IS_INPUT_STREAM (stream));

  if (!(converter = g_charset_converter_new ("UTF-8", "UCS-2", &error))) {
    g_debug ("Failed to create UTF-8 decoder: %s", error->message);
    gdk_drop_finish (drop, GdkDragAction(0));
    return;
  }

  /* TEXT_X_MOZ_URL is in UCS-2 so convert it to UTF-8.
   *
   * The data is expected to be URL, a \n, then the title of the web page.
   *
   * However, some applications (e.g. dolphin) delimit with a \r\n (see
   * issue#293) so handle that generically with the line reader.
   */
  converter_stream = g_converter_input_stream_new (stream, G_CONVERTER (converter));
  line_reader = g_data_input_stream_new (converter_stream);
  g_data_input_stream_set_newline_type (line_reader,
                                        G_DATA_STREAM_NEWLINE_TYPE_ANY);

  state = g_new0 (TextUriList, 1);
  state->screen = g_object_ref (screen);
  state->drop = g_object_ref (drop);
  state->mime_type = g_intern_string (TEXT_X_MOZ_URL);

  g_data_input_stream_read_line_async (line_reader,
                                       DROP_REQUEST_PRIORITY,
                                       nullptr,
                                       terminal_screen_drop_uri_list_line_cb,
                                       g_steal_pointer (&state));
}

static gboolean
terminal_screen_drop_target_drop (TerminalScreen     *screen,
                                  GdkDrop            *drop,
                                  double              x,
                                  double              y,
                                  GtkDropTargetAsync *drop_target)
{
  TerminalScreenPrivate *priv = screen->priv;
  GdkContentFormats *formats;

  g_assert (TERMINAL_IS_SCREEN (screen));
  g_assert (GDK_IS_DROP (drop));
  g_assert (GTK_IS_DROP_TARGET_ASYNC (drop_target));

  gtk_widget_hide (priv->drop_highlight);

  formats = gdk_drop_get_formats (drop);

  if (gdk_content_formats_contain_gtype (formats, GDK_TYPE_FILE_LIST) ||
      gdk_content_formats_contain_gtype (formats, G_TYPE_FILE) ||
      gdk_content_formats_contain_mime_type (formats, TEXT_URI_LIST) ||
      gdk_content_formats_contain_mime_type (formats, APPLICATION_VND_PORTAL_FILETRANSFER) ||
      gdk_content_formats_contain_mime_type (formats, APPLICATION_VND_PORTAL_FILES)) {
    gdk_drop_read_value_async (drop,
                               GDK_TYPE_FILE_LIST,
                               DROP_REQUEST_PRIORITY,
                               nullptr,
                               terminal_screen_drop_file_list_cb,
                               g_object_ref (screen));
    return TRUE;
  } else if (gdk_content_formats_contain_gtype (formats, GDK_TYPE_RGBA)) {
    gdk_drop_read_value_async (drop,
                               GDK_TYPE_RGBA,
                               DROP_REQUEST_PRIORITY,
                               nullptr,
                               terminal_screen_drop_color_cb,
                               g_object_ref (screen));
    return TRUE;
  } else if (gdk_content_formats_contain_mime_type (formats, TEXT_X_MOZ_URL)) {
    gdk_drop_read_async (drop,
                         (const char **)((const char * const []){TEXT_X_MOZ_URL, nullptr}),
                         DROP_REQUEST_PRIORITY,
                         nullptr,
                         terminal_screen_drop_moz_url_cb,
                         g_object_ref (screen));
    return TRUE;
  } else if (gdk_content_formats_contain_mime_type (formats, X_SPECIAL_GNOME_RESET_BACKGROUND)) {
      g_settings_reset (priv->profile, TERMINAL_PROFILE_BACKGROUND_COLOR_KEY);
      gdk_drop_finish (drop, GDK_ACTION_COPY);
      return TRUE;
  } else if (gdk_content_formats_contain_gtype (formats, G_TYPE_STRING)) {
    gdk_drop_read_value_async (drop,
                               G_TYPE_STRING,
                               DROP_REQUEST_PRIORITY,
                               nullptr,
                               terminal_screen_drop_string_cb,
                               g_object_ref (screen));
    return TRUE;
  }

  return FALSE;
}

static GdkDragAction
terminal_screen_drop_target_drag_enter (TerminalScreen     *screen,
                                        GdkDrop            *drop,
                                        double              x,
                                        double              y,
                                        GtkDropTargetAsync *drop_target)
{
  TerminalScreenPrivate *priv = screen->priv;

  g_assert (TERMINAL_IS_SCREEN (screen));
  g_assert (GTK_IS_DROP_TARGET_ASYNC (drop_target));

  gtk_widget_show (priv->drop_highlight);

  return GDK_ACTION_COPY;
}

static void
terminal_screen_drop_target_drag_leave (TerminalScreen     *screen,
                                        GdkDrop            *drop,
                                        GtkDropTargetAsync *drop_target)
{
  TerminalScreenPrivate *priv = screen->priv;

  g_assert (TERMINAL_IS_SCREEN (screen));
  g_assert (GTK_IS_DROP_TARGET_ASYNC (drop_target));

  gtk_widget_hide (priv->drop_highlight);
}

static void
_terminal_screen_update_scrollbar (TerminalScreen *screen)
{
  TerminalScreenPrivate *priv = screen->priv;

  auto const vpolicy = TerminalScrollbarPolicy(g_settings_get_enum (priv->profile, TERMINAL_PROFILE_SCROLLBAR_POLICY_KEY));

  terminal_tab_set_screen_policy (screen, TERMINAL_SCROLLBAR_POLICY_NEVER, vpolicy);
}

void
_terminal_screen_update_kinetic_scrolling(TerminalScreen* screen)
{
  auto const priv = screen->priv;
  auto const value = g_settings_get_boolean(priv->profile, TERMINAL_PROFILE_KINETIC_SCROLLING_KEY);
  terminal_tab_set_screen_kinetic_scrolling(screen, value);
}

void
terminal_screen_get_size (TerminalScreen *screen,
			  int       *width_chars,
			  int       *height_chars)
{
  VteTerminal *terminal = VTE_TERMINAL (screen);

  *width_chars = vte_terminal_get_column_count (terminal);
  *height_chars = vte_terminal_get_row_count (terminal);
}

void
terminal_screen_get_cell_size (TerminalScreen *screen,
			       int                  *cell_width_pixels,
			       int                  *cell_height_pixels)
{
  VteTerminal *terminal = VTE_TERMINAL (screen);

  *cell_width_pixels = vte_terminal_get_char_width (terminal);
  *cell_height_pixels = vte_terminal_get_char_height (terminal);
}

static char*
terminal_screen_check_match (TerminalScreen *screen,
                             double          x,
                             double          y,
                             int            *flavor)
{
  TerminalScreenPrivate *priv = screen->priv;
  GSList *tags;
  int tag;
  char *match;

  match = vte_terminal_check_match_at (VTE_TERMINAL (screen), x, y, &tag);
  for (tags = priv->match_tags; tags != nullptr; tags = tags->next)
    {
      TagData *tag_data = (TagData*) tags->data;
      if (tag_data->tag == tag)
	{
	  if (flavor)
	    *flavor = tag_data->flavor;
	  return match;
	}
    }

  g_free (match);
  return nullptr;
}

static void
terminal_screen_check_extra (TerminalScreen  *screen,
                             double           x,
                             double           y,
                             char           **number_info,
                             char           **timestamp_info)
{
  guint i;
  char **matches;
  gboolean flavor_number_found = FALSE;

  matches = g_newa (char *, n_extra_regexes);
  memset(matches, 0, sizeof(char*) * n_extra_regexes);

  if (
      vte_terminal_check_regex_simple_at (VTE_TERMINAL (screen),
                                          x, y,
                                          extra_regexes,
                                          n_extra_regexes,
                                          0,
                                          matches))
    {
      for (i = 0; i < n_extra_regexes; i++)
        {
          if (matches[i] != nullptr)
            {
              /* Store the first match for each flavor, free all the others */
              switch (extra_regex_flavors[i])
                {
                case FLAVOR_NUMBER:
                  if (!flavor_number_found)
                    {
                      *number_info = terminal_util_number_info (matches[i]);
                      *timestamp_info = terminal_util_timestamp_info (matches[i]);
                      flavor_number_found = TRUE;
                    }
                  g_free (matches[i]);
                  break;
                default:
                  g_free (matches[i]);
                }
            }
        }
    }
}

/* Whether a child of the shell is stopped, as jobs suspended with Ctrl+Z
 * are, which closing the terminal would kill. Running background jobs are
 * not counted, as shells such as zsh run helpers for their prompts in the
 * background. */
static gboolean
terminal_screen_has_stopped_jobs (TerminalScreen *screen)
{
#ifdef __linux__
  auto const pid = screen->priv->child_pid;
  if (pid <= 0 || terminal_flatpak_is_sandboxed ())
    return FALSE;

  char filename[64];
  g_snprintf (filename, sizeof (filename), "/proc/%d/task/%d/children", int(pid), int(pid));
  gs_free char *children = nullptr;
  if (!g_file_get_contents (filename, &children, nullptr, nullptr))
    return FALSE;

  gs_strfreev char **pids = g_strsplit_set (children, " \n", -1);
  for (auto p = pids; *p; ++p) {
    if (!**p)
      continue;

    g_snprintf (filename, sizeof (filename), "/proc/%s/stat", *p);
    gs_free char *stat = nullptr;
    if (!g_file_get_contents (filename, &stat, nullptr, nullptr))
      continue;

    /* "PID (COMM) STATE ...", where COMM may contain spaces and parentheses */
    auto const end = strrchr (stat, ')');
    if (end && end[1] == ' ' && (end[2] == 'T' || end[2] == 't'))
      return TRUE;
  }
#endif
  return FALSE;
}

/**
 * terminal_screen_has_running_jobs:
 * @screen:
 *
 * Returns: %TRUE if closing @screen would kill something besides its idle
 *   shell: a program in the foreground or a stopped job
 */
gboolean
terminal_screen_has_running_jobs (TerminalScreen *screen)
{
  return terminal_screen_has_foreground_process (screen, nullptr, nullptr) ||
    terminal_screen_has_stopped_jobs (screen);
}

/**
 * terminal_screen_has_foreground_process:
 * @screen:
 * @process_name: (out) (allow-none): the basename of the program, or %nullptr
 * @cmdline: (out) (allow-none): the full command line, or %nullptr
 *
 * Checks whether there's a foreground process running in
 * this terminal.
 * 
 * Returns: %TRUE iff there's a foreground process running in @screen
 */
gboolean
terminal_screen_has_foreground_process (TerminalScreen *screen,
                                        char           **process_name,
                                        char           **cmdline)
{
  TerminalScreenPrivate *priv = screen->priv;
  gs_free char *command = nullptr;
  gs_free char *data_buf = nullptr;
  gs_free char *basename = nullptr;
  gs_free char *name = nullptr;
  VtePty *pty;
  int fd;
#if defined(__FreeBSD__) || defined(__DragonFly__) || defined(__OpenBSD__)
  int mib[4];
#else
  char filename[64];
#endif
  char *data;
  gsize i;
  gsize len;
  int fgpid;

  if (priv->child_pid == -1)
    return FALSE;

  // In the sandbox, as last seen on the host
  if (terminal_flatpak_is_sandboxed ()) {
    auto const info = priv->host_process;
    if (!info || info->foreground_pid == priv->child_pid)
      return FALSE;
    if (process_name)
      *process_name = g_strdup (info->foreground_name);
    if (cmdline)
      *cmdline = g_strdup (info->foreground_cmdline);
    return TRUE;
  }

  pty = vte_terminal_get_pty (VTE_TERMINAL (screen));
  if (pty == nullptr)
    return FALSE;

  fd = vte_pty_get_fd (pty);
  if (fd == -1)
    return FALSE;

  fgpid = tcgetpgrp (fd);
  if (fgpid == -1 || fgpid == priv->child_pid)
    return FALSE;

#if defined(__FreeBSD__) || defined(__DragonFly__)
  mib[0] = CTL_KERN;
  mib[1] = KERN_PROC;
  mib[2] = KERN_PROC_ARGS;
  mib[3] = fgpid;
  if (sysctl (mib, G_N_ELEMENTS (mib), nullptr, &len, nullptr, 0) == -1)
      return TRUE;

  data_buf = (char*)g_malloc0 (len);
  if (sysctl (mib, G_N_ELEMENTS (mib), data_buf, &len, nullptr, 0) == -1)
      return TRUE;
  data = data_buf;
#elif defined(__OpenBSD__)
  mib[0] = CTL_KERN;
  mib[1] = KERN_PROC_ARGS;
  mib[2] = fgpid;
  mib[3] = KERN_PROC_ARGV;
  if (sysctl (mib, G_N_ELEMENTS (mib), nullptr, &len, nullptr, 0) == -1)
      return TRUE;

  data_buf = (char*)g_malloc0 (len);
  if (sysctl (mib, G_N_ELEMENTS (mib), data_buf, &len, nullptr, 0) == -1)
      return TRUE;
  data = ((char**)data_buf)[0];
#else
  g_snprintf (filename, sizeof (filename), "/proc/%d/cmdline", fgpid);
  if (!g_file_get_contents (filename, &data_buf, &len, nullptr))
    return TRUE;
  data = data_buf;
#endif

  basename = g_path_get_basename (data);
  if (!basename)
    return TRUE;

  name = g_filename_to_utf8 (basename, -1, nullptr, nullptr, nullptr);
  if (!name)
    return TRUE;

  if (!process_name && !cmdline)
    return TRUE;

  gs_transfer_out_value (process_name, &name);

  if (len > 0 && data[len - 1] == '\0')
    len--;
  for (i = 0; i < len; i++)
    {
      if (data[i] == '\0')
        data[i] = ' ';
    }

  command = g_filename_to_utf8 (data, -1, nullptr, nullptr, nullptr);
  if (!command)
    return TRUE;

  gs_transfer_out_value (cmdline, &command);

  return TRUE;
}

const char *
terminal_screen_get_uuid (TerminalScreen *screen)
{
  g_return_val_if_fail (TERMINAL_IS_SCREEN (screen), nullptr);

  return screen->priv->uuid;
}

/**
 * terminal_screen_paste_text:
 * @screen:
 * @text: a NUL-terminated string
 * @len: length of @text, or -1
 *
 * Inserts @text to @terminal as if pasted.
 */
void
terminal_screen_paste_text (TerminalScreen* screen,
                            char const* text,
                            gssize len)
{
  g_return_if_fail (text != nullptr);
  g_return_if_fail (len >= -1);

  /* This is just an API hack until vte 0.69 adds vte_terminal_paste_text_len() */
  /* Note that @text MUST be NUL-terminated */

  vte_terminal_paste_text (VTE_TERMINAL (screen), text);
}

gboolean
terminal_screen_is_active (TerminalScreen *screen)
{
  TerminalNotebook *notebook;

  g_return_val_if_fail (TERMINAL_IS_SCREEN (screen), FALSE);

  notebook = TERMINAL_NOTEBOOK (gtk_widget_get_ancestor (GTK_WIDGET (screen),
                                                         TERMINAL_TYPE_NOTEBOOK));
  if (notebook == nullptr)
    return FALSE;

  return terminal_notebook_get_active_screen (notebook) == screen;
}

GIcon*
terminal_screen_get_icon(TerminalScreen* screen)
{
  g_return_val_if_fail(TERMINAL_IS_SCREEN(screen), nullptr);

  if (screen->priv->icon_image)
    return screen->priv->icon_image;
  else if (screen->priv->icon_color)
    return screen->priv->icon_color;

  // Without an icon set by the program, mark root and remote sessions
  static GIcon* root_icon = g_themed_icon_new("dialog-warning-symbolic");
  static GIcon* remote_icon = g_themed_icon_new("network-server-symbolic");
  switch (screen->priv->process_kind) {
  case TERMINAL_PROCESS_KIND_ROOT: return root_icon;
  case TERMINAL_PROCESS_KIND_REMOTE: return remote_icon;
  default: return nullptr;
  }
}

GIcon*
terminal_screen_get_icon_progress(TerminalScreen* screen)
{
  g_return_val_if_fail(TERMINAL_IS_SCREEN(screen), nullptr);

  return terminal_screen_ensure_icon_progress(screen);
}

/* Prompts */

/**
 * terminal_screen_has_prompts:
 * @screen: a #TerminalScreen
 *
 * Returns: whether the shell has reported prompts, so that
 *   terminal_screen_scroll_to_prompt() can work
 */
gboolean
terminal_screen_has_prompts(TerminalScreen* screen)
{
  g_return_val_if_fail(TERMINAL_IS_SCREEN(screen), FALSE);

  terminal_screen_validate_prompts(screen);
  return screen->priv->prompts->len > 0;
}

/**
 * terminal_screen_scroll_to_prompt:
 * @screen: a #TerminalScreen
 * @direction: -1 for the previous prompt, 1 for the next one
 *
 * Scrolls so that the previous or next prompt is the first visible row.
 * Going past the last prompt scrolls to the bottom.
 *
 * Returns: whether the terminal scrolled
 */
gboolean
terminal_screen_scroll_to_prompt(TerminalScreen* screen,
                                 int direction)
{
  g_return_val_if_fail(TERMINAL_IS_SCREEN(screen), FALSE);

  if (!terminal_screen_has_prompts(screen))
    return FALSE;

  auto const priv = screen->priv;
  auto const adjustment = terminal_screen_get_vadjustment(screen);
  auto const top = terminal_screen_adjustment_to_row(screen, gtk_adjustment_get_value(adjustment));
  auto const bottom = terminal_screen_adjustment_to_row(screen,
                                                        gtk_adjustment_get_upper(adjustment) -
                                                        gtk_adjustment_get_page_size(adjustment));

  double target = -1;
  if (direction < 0) {
    // At the bottom, start from the current prompt rather than from the top
    // of the view, so that the previous command shows first.
    long anchor = top;
    if (top >= bottom)
      vte_terminal_get_cursor_position(VTE_TERMINAL(screen), nullptr, &anchor);

    for (auto i = priv->prompts->len; i > 0; --i) {
      auto const& mark = g_array_index(priv->prompts, PromptMark, i - 1);
      if (mark.row < anchor) {
        target = double(mark.row);
        break;
      }
    }
  } else {
    for (auto i = 0u; i < priv->prompts->len; ++i) {
      auto const& mark = g_array_index(priv->prompts, PromptMark, i);
      if (mark.row > top) {
        target = double(MIN(mark.row, bottom));
        break;
      }
    }
    if (target < 0 && top < bottom)
      target = double(bottom);
  }

  if (target < 0)
    return FALSE;

  gtk_adjustment_set_value(adjustment, terminal_screen_row_to_adjustment(screen, long(target)));
  return TRUE;
}

/* Finds the rows of the output of the finished command whose prompt or
 * output contains @row, or of the last finished command if @row is -1 */
static bool
terminal_screen_find_command_output(TerminalScreen* screen,
                                    long row,
                                    long* start_row,
                                    long* end_row)
{
  auto const priv = screen->priv;

  terminal_screen_validate_prompts(screen);

  // The last prompt is the current one; its command, if any, is still running
  for (auto i = priv->prompts->len; i > 1; --i) {
    auto const& mark = g_array_index(priv->prompts, PromptMark, i - 2);
    auto const next_row = g_array_index(priv->prompts, PromptMark, i - 1).row;
    if (row >= 0 && (row < mark.row || row >= next_row))
      continue;
    if (!mark.ran_command) {
      if (row >= 0)
        return false;
      continue;
    }

    *start_row = mark.row + 1;
    *end_row = next_row - 1;
    return *end_row >= *start_row;
  }

  return false;
}

/**
 * terminal_screen_get_row_at:
 * @screen: a #TerminalScreen
 * @y: a vertical position in @screen
 *
 * Returns: the absolute row at @y
 */
long
terminal_screen_get_row_at(TerminalScreen* screen,
                           double y)
{
  g_return_val_if_fail(TERMINAL_IS_SCREEN(screen), -1);

  GtkBorder padding;
  gtk_style_context_get_padding(gtk_widget_get_style_context(GTK_WIDGET(screen)), &padding);
  auto const cell_height = vte_terminal_get_char_height(VTE_TERMINAL(screen));
  if (cell_height <= 0)
    return -1;

  long first_row, last_row;
  terminal_screen_get_visible_rows(screen, &first_row, &last_row);
  auto const row = first_row + long(floor((y - padding.top) / double(cell_height)));
  return CLAMP(row, first_row, last_row);
}

/**
 * terminal_screen_dup_scroll_marks:
 * @screen: a #TerminalScreen
 *
 * Returns: (transfer full): a #TerminalScrollMark for each prompt at which
 *   a command was run, with its position in the scrollable range
 */
GArray*
terminal_screen_dup_scroll_marks(TerminalScreen* screen)
{
  g_return_val_if_fail(TERMINAL_IS_SCREEN(screen), nullptr);

  auto const priv = screen->priv;
  auto const marks = g_array_new(false, false, sizeof(TerminalScrollMark));

  terminal_screen_validate_prompts(screen);
  if (priv->prompts->len == 0)
    return marks;

  auto const adjustment = terminal_screen_get_vadjustment(screen);
  auto const first_row = terminal_screen_adjustment_to_row(screen, gtk_adjustment_get_lower(adjustment));
  auto const end_row = terminal_screen_adjustment_to_row(screen, gtk_adjustment_get_upper(adjustment));
  if (end_row <= first_row)
    return marks;

  for (auto i = 0u; i < priv->prompts->len; ++i) {
    auto const& prompt = g_array_index(priv->prompts, PromptMark, i);
    if (!prompt.ran_command || prompt.row < first_row || prompt.row >= end_row)
      continue;

    auto const mark = TerminalScrollMark{double(prompt.row - first_row) / double(end_row - first_row),
                                         prompt.status > 0};
    g_array_append_val(marks, mark);
  }

  return marks;
}

/**
 * terminal_screen_has_command_output:
 * @screen: a #TerminalScreen
 *
 * Returns: whether terminal_screen_dup_command_output() has something to
 *   return
 */
gboolean
terminal_screen_has_command_output(TerminalScreen* screen)
{
  g_return_val_if_fail(TERMINAL_IS_SCREEN(screen), FALSE);

  long start_row, end_row;
  return terminal_screen_find_command_output(screen, -1, &start_row, &end_row);
}

/**
 * terminal_screen_has_command_output_at:
 * @screen: a #TerminalScreen
 * @row: an absolute row
 *
 * Returns: whether @row is in the prompt or the output of a finished command
 */
gboolean
terminal_screen_has_command_output_at(TerminalScreen* screen,
                                      long row)
{
  g_return_val_if_fail(TERMINAL_IS_SCREEN(screen), FALSE);

  long start_row, end_row;
  return row >= 0 && terminal_screen_find_command_output(screen, row, &start_row, &end_row);
}

/**
 * terminal_screen_dup_command_output:
 * @screen: a #TerminalScreen
 *
 * Returns: (transfer full) (nullable): the output of the last finished
 *   command, without trailing white space
 */
char*
terminal_screen_dup_command_output(TerminalScreen* screen)
{
  return terminal_screen_dup_command_output_at(screen, -1);
}

/**
 * terminal_screen_dup_command_output_at:
 * @screen: a #TerminalScreen
 * @row: an absolute row, or -1 for the last finished command
 *
 * Returns: (transfer full) (nullable): the output of the finished command
 *   whose prompt or output contains @row, without trailing white space
 */
char*
terminal_screen_dup_command_output_at(TerminalScreen* screen,
                                      long row)
{
  g_return_val_if_fail(TERMINAL_IS_SCREEN(screen), nullptr);

  long start_row, end_row;
  if (!terminal_screen_find_command_output(screen, row, &start_row, &end_row))
    return nullptr;

  auto const text = vte_terminal_get_text_range_format(VTE_TERMINAL(screen),
                                                       VTE_FORMAT_TEXT,
                                                       start_row, 0,
                                                       end_row,
                                                       vte_terminal_get_column_count(VTE_TERMINAL(screen)),
                                                       nullptr);
  return text ? g_strchomp(text) : nullptr;
}

/* Search */

/* The number of cells VTE gives to @c; close to VTE's own tables */
static int
character_width(gunichar c,
                bool ambiguous_wide)
{
  if (g_unichar_iszerowidth(c))
    return 0;
  if (g_unichar_iswide(c) || (ambiguous_wide && g_unichar_iswide_cjk(c)))
    return 2;
  return 1;
}

/* Maps a byte offset in a line of text to a cell */
struct CellPosition {
  long row;
  long col;
};

/**
 * terminal_screen_find_matches:
 * @screen: a #TerminalScreen
 * @regex: the pattern
 * @max_matches: stop after this many matches
 *
 * Finds all matches of @regex in the scrollback and on the screen. Matches
 * may span soft-wrapped rows, but not hard line breaks.
 *
 * Returns: (transfer full): a #GArray of #TerminalSearchMatch, from top to
 *   bottom
 */
GArray*
terminal_screen_find_matches(TerminalScreen* screen,
                             GRegex* regex,
                             guint max_matches)
{
  auto const matches = g_array_new(false, false, sizeof(TerminalSearchMatch));

  g_return_val_if_fail(TERMINAL_IS_SCREEN(screen), matches);
  g_return_val_if_fail(regex != nullptr, matches);

  auto const terminal = VTE_TERMINAL(screen);
  auto const columns = vte_terminal_get_column_count(terminal);
  auto const ambiguous_wide = vte_terminal_get_cjk_ambiguous_width(terminal) == 2;

  // Search at most the last 100000 rows
  auto const adjustment = terminal_screen_get_vadjustment(screen);
  auto first_row = terminal_screen_adjustment_to_row(screen, gtk_adjustment_get_lower(adjustment));
  auto const last_row = terminal_screen_adjustment_to_row(screen, gtk_adjustment_get_upper(adjustment)) - 1;
  first_row = MAX(first_row, last_row - 100000);
  if (last_row < first_row || columns <= 0)
    return matches;

  gsize length = 0;
  gs_free char* text = vte_terminal_get_text_range_format(terminal, VTE_FORMAT_TEXT,
                                                          first_row, 0, last_row, columns,
                                                          &length);
  if (!text)
    return matches;

  gs_unref_array GArray* positions = g_array_new(false, false, sizeof(CellPosition));

  auto row = first_row;
  for (auto line = text; line && *line && matches->len < max_matches; ) {
    auto const newline = strchr(line, '\n');
    auto const line_length = newline ? gsize(newline - line) : strlen(line);

    // Where each byte of the line is on the grid; rows wrap like VTE's
    g_array_set_size(positions, line_length + 1);
    auto col = 0l;
    for (auto p = line; p < line + line_length; p = g_utf8_next_char(p)) {
      auto const c = g_utf8_get_char(p);
      auto width = c == '\t' ? int(8 - col % 8) : character_width(c, ambiguous_wide);
      if (width > 0 && col + width > columns && col > 0) {
        ++row;
        col = 0;
        if (c == '\t')
          width = 8;
      }
      for (auto q = p; q < g_utf8_next_char(p); ++q)
        g_array_index(positions, CellPosition, q - line) = CellPosition{row, col};
      col += width;
    }
    g_array_index(positions, CellPosition, line_length) = CellPosition{row, col};

    gs_free char* line_copy = g_strndup(line, line_length);
    GMatchInfo* info = nullptr;
    g_regex_match(regex, line_copy, G_REGEX_MATCH_NOTEMPTY, &info);
    while (g_match_info_matches(info) && matches->len < max_matches) {
      int start = 0, end = 0;
      if (g_match_info_fetch_pos(info, 0, &start, &end) && end > start) {
        auto const& from = g_array_index(positions, CellPosition, start);
        // The end is right after the last character of the match
        auto const last_char = g_utf8_find_prev_char(line_copy, line_copy + end);
        auto const& last = g_array_index(positions, CellPosition, last_char - line_copy);
        auto const& after = g_array_index(positions, CellPosition, end);
        auto const to = after.row == last.row ? after
          : CellPosition{last.row, last.col + character_width(g_utf8_get_char(last_char), ambiguous_wide)};
        auto const match = TerminalSearchMatch{from.row, from.col, to.row, to.col};
        g_array_append_val(matches, match);
      }
      g_match_info_next(info, nullptr);
    }
    g_match_info_free(info);

    ++row;
    line = newline ? newline + 1 : nullptr;
  }

  return matches;
}

/**
 * terminal_screen_set_search_matches:
 * @screen: a #TerminalScreen
 * @matches: (nullable): a #GArray of #TerminalSearchMatch, or %NULL
 * @current: the index of the current match, or -1
 *
 * Sets the matches to highlight. They are dropped when the column count
 * changes.
 */
void
terminal_screen_set_search_matches(TerminalScreen* screen,
                                   GArray* matches,
                                   int current)
{
  g_return_if_fail(TERMINAL_IS_SCREEN(screen));

  auto const priv = screen->priv;
  if (matches)
    g_array_ref(matches);
  g_clear_pointer(&priv->search_matches, g_array_unref);
  priv->search_matches = matches;
  priv->search_current = current;
  priv->search_columns = vte_terminal_get_column_count(VTE_TERMINAL(screen));

  gtk_widget_queue_draw(GTK_WIDGET(screen));
}

/**
 * terminal_screen_scroll_to_row:
 * @screen: a #TerminalScreen
 * @row: an absolute row
 *
 * Scrolls @row into view, to the middle of the screen if it is not visible.
 */
void
terminal_screen_scroll_to_row(TerminalScreen* screen,
                              long row)
{
  g_return_if_fail(TERMINAL_IS_SCREEN(screen));

  auto const adjustment = terminal_screen_get_vadjustment(screen);
  auto const rows = vte_terminal_get_row_count(VTE_TERMINAL(screen));
  auto const top = terminal_screen_adjustment_to_row(screen, gtk_adjustment_get_value(adjustment));

  if (row >= top && row < top + rows)
    return;

  auto const value = CLAMP(terminal_screen_row_to_adjustment(screen, row - rows / 2),
                           gtk_adjustment_get_lower(adjustment),
                           gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment));
  gtk_adjustment_set_value(adjustment, value);
}

/**
 * terminal_screen_get_visible_rows:
 * @screen: a #TerminalScreen
 * @first_row: (out): the first visible row
 * @last_row: (out): the last visible row
 */
void
terminal_screen_get_visible_rows(TerminalScreen* screen,
                                 long* first_row,
                                 long* last_row)
{
  g_return_if_fail(TERMINAL_IS_SCREEN(screen));

  auto const adjustment = terminal_screen_get_vadjustment(screen);
  *first_row = terminal_screen_adjustment_to_row(screen, gtk_adjustment_get_value(adjustment));
  *last_row = *first_row + vte_terminal_get_row_count(VTE_TERMINAL(screen)) - 1;
}

/* Input broadcasting */

/**
 * terminal_screen_is_user_input:
 * @screen: a #TerminalScreen
 * @text: data from the #VteTerminal::commit signal
 * @size: the length of @text
 *
 * VTE emits ::commit for everything it sends to the child: typed text,
 * keys, pastes, mouse reports, and its own replies to queries from the
 * program. Only what the user typed should be sent to other terminals.
 *
 * Text without escape sequences is typed (directly or through an input
 * method, which may commit it later); replies and reports always start
 * with an escape. Escape sequences count only while a key press is being
 * handled, for cursor and function keys.
 *
 * Returns: whether @text is input from the user
 */
gboolean
terminal_screen_is_user_input(TerminalScreen* screen,
                              char const* text,
                              gsize size)
{
  g_return_val_if_fail(TERMINAL_IS_SCREEN(screen), FALSE);

  if (!memchr(text, '\x1b', size))
    return TRUE;

  return screen->priv->in_key_event;
}

void
terminal_screen_set_broadcast_excluded(TerminalScreen* screen,
                                       gboolean excluded)
{
  g_return_if_fail(TERMINAL_IS_SCREEN(screen));

  screen->priv->broadcast_excluded = excluded != FALSE;
}

/**
 * terminal_screen_get_broadcast_excluded:
 * @screen: a #TerminalScreen
 *
 * Returns: whether @screen neither sends nor receives broadcast input
 */
gboolean
terminal_screen_get_broadcast_excluded(TerminalScreen* screen)
{
  g_return_val_if_fail(TERMINAL_IS_SCREEN(screen), FALSE);

  return screen->priv->broadcast_excluded;
}

/* Containers */

/**
 * terminal_screen_set_container:
 * @screen: a #TerminalScreen
 * @runtime: (nullable): "toolbox" or "distrobox"
 * @name: (nullable): the container, or %NULL for the host
 *
 * Makes the shell of @screen start in a container when it is started
 * next. Explicit commands still run on the host.
 */
void
terminal_screen_set_container(TerminalScreen* screen,
                              char const* runtime,
                              char const* name)
{
  g_return_if_fail(TERMINAL_IS_SCREEN(screen));

  auto const priv = screen->priv;
  g_set_str(&priv->container_runtime, name && name[0] ? runtime : nullptr);
  g_set_str(&priv->container_name, name && name[0] ? name : nullptr);
}

/**
 * terminal_screen_get_container:
 * @screen: a #TerminalScreen
 * @runtime: (out) (transfer full) (nullable): the runtime
 * @name: (out) (transfer full) (nullable): the container
 *
 * Finds the container the shell of @screen is in: the one it reports
 * (toolbox does, with OSC 777), else the one it was started in.
 *
 * Returns: whether it is in a container
 */
gboolean
terminal_screen_get_container(TerminalScreen* screen,
                              char** runtime,
                              char** name)
{
  g_return_val_if_fail(TERMINAL_IS_SCREEN(screen), FALSE);

  auto const terminal = VTE_TERMINAL(screen);
  gs_free char* reported_name = vte_terminal_dup_termprop_string(terminal, VTE_TERMPROP_CONTAINER_NAME, nullptr);
  if (reported_name && reported_name[0]) {
    gs_free char* reported_runtime = vte_terminal_dup_termprop_string(terminal, VTE_TERMPROP_CONTAINER_RUNTIME, nullptr);
    *runtime = g_strdup(reported_runtime && g_str_equal(reported_runtime, "distrobox") ? "distrobox" : "toolbox");
    *name = g_steal_pointer(&reported_name);
    return TRUE;
  }

  auto const priv = screen->priv;
  if (priv->container_name && priv->child_pid > 0) {
    *runtime = g_strdup(priv->container_runtime);
    *name = g_strdup(priv->container_name);
    return TRUE;
  }

  *runtime = *name = nullptr;
  return FALSE;
}
