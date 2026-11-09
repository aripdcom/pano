/*
 * terminal-find-bar.c
 *
 * Copyright 2023 Christian Hergert <chergert@redhat.com>
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

#include "terminal-find-bar.hh"

#include "terminal-libgsystem.hh"
#include "terminal-util.hh"

#include <glib/gi18n.h>

/* The find bar searches the scrollback itself rather than with VTE's
 * search, so that it can count and highlight all matches. */

#define MAX_MATCHES 10000
#define RESCAN_DELAY_MS 250

struct _TerminalFindBar
{
  GtkWidget        parent_instance;

  TerminalScreen  *screen;

  GtkEntry        *entry;
  GtkCheckButton  *use_regex;
  GtkCheckButton  *whole_words;
  GtkCheckButton  *match_case;
  GtkLabel        *status;

  GRegex          *regex; /* the valid pattern, or nullptr */
  GArray          *matches; /* TerminalSearchMatch, top to bottom */
  int              current; /* index of the current match, or -1 */
  bool             truncated; /* there are more than MAX_MATCHES */
  guint            rescan_source;
};

enum {
  PROP_0,
  PROP_SCREEN,
  PROP_HAS_SEARCH,
  N_PROPS
};

G_DEFINE_FINAL_TYPE (TerminalFindBar, terminal_find_bar, GTK_TYPE_WIDGET)

static GParamSpec *properties [N_PROPS];

static void
terminal_find_bar_dismiss (GtkWidget  *widget,
                         const char *action_name,
                         GVariant   *param)
{
  TerminalFindBar *self = (TerminalFindBar *)widget;
  GtkWidget *revealer;

  g_assert (TERMINAL_IS_FIND_BAR (self));

  if ((revealer = gtk_widget_get_ancestor (widget, GTK_TYPE_REVEALER)))
    gtk_revealer_set_reveal_child (GTK_REVEALER (revealer), FALSE);

  if (self->screen != nullptr)
    gtk_widget_grab_focus (GTK_WIDGET (self->screen));
}

static gboolean
terminal_find_bar_grab_focus (GtkWidget *widget)
{
  return gtk_widget_grab_focus (GTK_WIDGET (TERMINAL_FIND_BAR (widget)->entry));
}

/* Highlights only while the bar is shown */
static void
terminal_find_bar_push_highlights(TerminalFindBar* self)
{
  if (!self->screen)
    return;

  if (gtk_widget_get_mapped(GTK_WIDGET(self)) && self->regex)
    terminal_screen_set_search_matches(self->screen, self->matches, self->current);
  else
    terminal_screen_set_search_matches(self->screen, nullptr, -1);
}

static void
terminal_find_bar_update_status(TerminalFindBar* self)
{
  auto const text = gtk_editable_get_text(GTK_EDITABLE(self->entry));
  auto const label = GTK_WIDGET(self->status);

  if (terminal_str_empty0(text)) {
    gtk_widget_set_visible(label, false);
    return;
  }

  // An invalid pattern shows its error, set in terminal_find_bar_update_regex()
  if (!self->regex) {
    gtk_widget_set_visible(label, true);
    return;
  }

  gtk_widget_remove_css_class(label, "error");
  gtk_widget_set_tooltip_text(label, nullptr);

  gs_free char* status = nullptr;
  auto const n_matches = self->matches ? self->matches->len : 0;
  if (n_matches == 0)
    status = g_strdup(_("No results"));
  else if (self->truncated)
    /* Translators: the current search match, and the number of matches */
    status = g_strdup_printf(_("%d of more than %u"), self->current + 1, n_matches);
  else
    /* Translators: the current search match, and the number of matches */
    status = g_strdup_printf(_("%d of %u"), self->current + 1, n_matches);

  gtk_label_set_label(self->status, status);
  gtk_widget_set_visible(label, true);
}

static void
terminal_find_bar_show_current(TerminalFindBar* self)
{
  if (self->screen && self->current >= 0)
    terminal_screen_scroll_to_row(self->screen,
                                  g_array_index(self->matches, TerminalSearchMatch, self->current).start_row);
}

/* Finds the matches again. With @keep_current, the current match stays the
 * same if it is still there; otherwise it becomes the last match that is
 * not below the view, as searching goes upwards from the bottom. */
static void
terminal_find_bar_rescan(TerminalFindBar* self,
                         bool keep_current)
{
  g_clear_handle_id(&self->rescan_source, g_source_remove);

  if (!self->screen || !self->regex) {
    g_clear_pointer(&self->matches, g_array_unref);
    self->current = -1;
    self->truncated = false;
    terminal_find_bar_push_highlights(self);
    terminal_find_bar_update_status(self);
    return;
  }

  auto const matches = terminal_screen_find_matches(self->screen, self->regex, MAX_MATCHES + 1);
  self->truncated = matches->len > MAX_MATCHES;
  if (self->truncated)
    g_array_set_size(matches, MAX_MATCHES);

  auto current = -1;
  if (keep_current && self->matches && self->current >= 0) {
    auto const& old = g_array_index(self->matches, TerminalSearchMatch, self->current);
    for (auto i = 0u; i < matches->len; ++i) {
      auto const& match = g_array_index(matches, TerminalSearchMatch, i);
      current = int(i);
      if (match.start_row > old.start_row ||
          (match.start_row == old.start_row && match.start_col >= old.start_col))
        break;
    }
  } else if (matches->len > 0) {
    long first_row, last_row;
    terminal_screen_get_visible_rows(self->screen, &first_row, &last_row);
    current = 0;
    for (auto i = 0u; i < matches->len; ++i) {
      if (g_array_index(matches, TerminalSearchMatch, i).start_row <= last_row)
        current = int(i);
    }
  }

  g_clear_pointer(&self->matches, g_array_unref);
  self->matches = matches;
  self->current = current;

  if (!keep_current)
    terminal_find_bar_show_current(self);
  terminal_find_bar_push_highlights(self);
  terminal_find_bar_update_status(self);
}

static gboolean
terminal_find_bar_rescan_cb(void* user_data)
{
  auto const self = TERMINAL_FIND_BAR(user_data);

  self->rescan_source = 0;
  terminal_find_bar_rescan(self, true);
  return G_SOURCE_REMOVE;
}

/* The terminal contents changed; look again in a moment */
static void
terminal_find_bar_queue_rescan(TerminalFindBar* self)
{
  if (!self->regex || !gtk_widget_get_mapped(GTK_WIDGET(self)) || self->rescan_source)
    return;

  self->rescan_source = g_timeout_add(RESCAN_DELAY_MS, terminal_find_bar_rescan_cb, self);
}

static void
terminal_find_bar_update_regex(TerminalFindBar* self)
{
  g_assert (TERMINAL_IS_FIND_BAR (self));

  auto const had_search = self->regex != nullptr;
  g_clear_pointer(&self->regex, g_regex_unref);

  auto const text = gtk_editable_get_text (GTK_EDITABLE (self->entry));
  gs_free_error GError* error = nullptr;

  if (!terminal_str_empty0 (text)) {
    gs_free char* escaped = nullptr;
    auto pattern = text;
    if (!gtk_check_button_get_active (GTK_CHECK_BUTTON (self->use_regex)))
      pattern = escaped = g_regex_escape_string (text, -1);

    gs_free char* words = nullptr;
    if (gtk_check_button_get_active (GTK_CHECK_BUTTON (self->whole_words)))
      pattern = words = g_strdup_printf ("\\b(?:%s)\\b", pattern);

    auto flags = GRegexCompileFlags(G_REGEX_OPTIMIZE);
    if (!gtk_check_button_get_active (GTK_CHECK_BUTTON (self->match_case)))
      flags = GRegexCompileFlags(flags | G_REGEX_CASELESS);

    self->regex = g_regex_new (pattern, flags, GRegexMatchFlags(0), &error);
  }

  if (error) {
    gtk_widget_add_css_class (GTK_WIDGET (self->entry), "error");
    gtk_widget_set_tooltip_text (GTK_WIDGET (self->entry), error->message);

    // Keep it short; the tooltip has the details
    gtk_label_set_label (self->status, _("Invalid pattern"));
    gtk_widget_add_css_class (GTK_WIDGET (self->status), "error");
    gtk_widget_set_tooltip_text (GTK_WIDGET (self->status), error->message);
  } else {
    gtk_widget_remove_css_class (GTK_WIDGET (self->entry), "error");
    gtk_widget_set_tooltip_text (GTK_WIDGET (self->entry), nullptr);
  }

  terminal_find_bar_rescan (self, false);

  if (had_search != (self->regex != nullptr))
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_HAS_SEARCH]);
}

static void
terminal_find_bar_navigate(TerminalFindBar* self,
                           int direction)
{
  if (!self->matches || self->matches->len == 0)
    return;

  auto const n = int(self->matches->len);
  if (self->current < 0)
    self->current = direction < 0 ? n - 1 : 0;
  else
    self->current = (self->current + direction + n) % n;

  terminal_find_bar_show_current(self);
  terminal_find_bar_push_highlights(self);
  terminal_find_bar_update_status(self);
}

static void
terminal_find_bar_next (GtkWidget  *widget,
                      const char *action_name,
                      GVariant   *param)
{
  terminal_find_bar_navigate(TERMINAL_FIND_BAR(widget), 1);
}

static void
terminal_find_bar_previous (GtkWidget  *widget,
                            const char *action_name,
                            GVariant   *param)
{
  terminal_find_bar_navigate(TERMINAL_FIND_BAR(widget), -1);
}

static void
terminal_find_bar_entry_changed_cb (TerminalFindBar *self,
                                    GtkEntry      *entry)
{
  terminal_find_bar_update_regex(self);
}

static void
terminal_find_bar_option_toggled_cb(TerminalFindBar* self,
                                    GtkCheckButton* button)
{
  terminal_find_bar_update_regex(self);
}

static void
terminal_find_bar_map(GtkWidget* widget)
{
  auto const self = TERMINAL_FIND_BAR(widget);

  GTK_WIDGET_CLASS(terminal_find_bar_parent_class)->map(widget);

  // The contents may have changed while the bar was hidden
  if (self->regex)
    terminal_find_bar_rescan(self, true);
}

static void
terminal_find_bar_unmap(GtkWidget* widget)
{
  auto const self = TERMINAL_FIND_BAR(widget);

  GTK_WIDGET_CLASS(terminal_find_bar_parent_class)->unmap(widget);

  g_clear_handle_id(&self->rescan_source, g_source_remove);
  terminal_find_bar_push_highlights(self);
}

static void
terminal_find_bar_screen_changed_cb(TerminalFindBar* self)
{
  terminal_find_bar_queue_rescan(self);
}

static void
terminal_find_bar_disconnect_screen(TerminalFindBar* self)
{
  if (!self->screen)
    return;

  terminal_screen_set_search_matches(self->screen, nullptr, -1);
  g_signal_handlers_disconnect_by_func(self->screen,
                                       (void*)terminal_find_bar_screen_changed_cb,
                                       self);
}

static void
terminal_find_bar_dispose (GObject *object)
{
  TerminalFindBar *self = (TerminalFindBar *)object;
  GtkWidget *child;

  g_clear_handle_id (&self->rescan_source, g_source_remove);
  terminal_find_bar_disconnect_screen (self);
  g_clear_pointer (&self->regex, g_regex_unref);
  g_clear_pointer (&self->matches, g_array_unref);

  gtk_widget_dispose_template (GTK_WIDGET (self), TERMINAL_TYPE_FIND_BAR);

  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self))))
    gtk_widget_unparent (child);

  g_clear_object (&self->screen);

  G_OBJECT_CLASS (terminal_find_bar_parent_class)->dispose (object);
}

static void
terminal_find_bar_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
  TerminalFindBar *self = TERMINAL_FIND_BAR (object);

  switch (prop_id)
    {
    case PROP_SCREEN:
      g_value_set_object (value, self->screen);
      break;

    case PROP_HAS_SEARCH:
      g_value_set_boolean (value, self->regex != nullptr);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
terminal_find_bar_set_property (GObject      *object,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  TerminalFindBar *self = TERMINAL_FIND_BAR (object);

  switch (prop_id)
    {
    case PROP_SCREEN:
      terminal_find_bar_set_screen (self, reinterpret_cast<TerminalScreen*>(g_value_get_object(value)));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
terminal_find_bar_class_init (TerminalFindBarClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = terminal_find_bar_dispose;
  object_class->get_property = terminal_find_bar_get_property;
  object_class->set_property = terminal_find_bar_set_property;

  widget_class->grab_focus = terminal_find_bar_grab_focus;
  widget_class->map = terminal_find_bar_map;
  widget_class->unmap = terminal_find_bar_unmap;

  properties[PROP_SCREEN] =
    g_param_spec_object ("screen", nullptr, nullptr,
                         TERMINAL_TYPE_SCREEN,
                         GParamFlags(G_PARAM_READWRITE |
                                     G_PARAM_EXPLICIT_NOTIFY |
                                     G_PARAM_STATIC_STRINGS));

  properties[PROP_HAS_SEARCH] =
    g_param_spec_boolean ("has-search", nullptr, nullptr,
                          false,
                          GParamFlags(G_PARAM_READABLE |
                                      G_PARAM_EXPLICIT_NOTIFY |
                                      G_PARAM_STATIC_STRINGS));

  g_object_class_install_properties (object_class, N_PROPS, properties);

  gtk_widget_class_set_template_from_resource (widget_class, "/com/aripd/pano/ui/find-bar.ui");
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "findbar");

  gtk_widget_class_bind_template_child (widget_class, TerminalFindBar, entry);
  gtk_widget_class_bind_template_child (widget_class, TerminalFindBar, use_regex);
  gtk_widget_class_bind_template_child (widget_class, TerminalFindBar, whole_words);
  gtk_widget_class_bind_template_child (widget_class, TerminalFindBar, match_case);
  gtk_widget_class_bind_template_child (widget_class, TerminalFindBar, status);

  gtk_widget_class_bind_template_callback (widget_class, terminal_find_bar_entry_changed_cb);
  gtk_widget_class_bind_template_callback (widget_class, terminal_find_bar_option_toggled_cb);

  gtk_widget_class_install_action (widget_class, "search.dismiss", nullptr, terminal_find_bar_dismiss);
  gtk_widget_class_install_action (widget_class, "search.down", nullptr, terminal_find_bar_next);
  gtk_widget_class_install_action (widget_class, "search.up", nullptr, terminal_find_bar_previous);

  gtk_widget_class_add_binding_action (widget_class, GDK_KEY_Escape, GdkModifierType(0), "search.dismiss", nullptr);
  gtk_widget_class_add_binding_action (widget_class, GDK_KEY_g, GDK_CONTROL_MASK, "search.up", nullptr);
  gtk_widget_class_add_binding_action (widget_class, GDK_KEY_g, GdkModifierType(GDK_CONTROL_MASK|GDK_SHIFT_MASK), "search.down", nullptr);
}

static void
terminal_find_bar_init (TerminalFindBar *self)
{
  self->current = -1;
  gtk_widget_init_template (GTK_WIDGET (self));
}

TerminalScreen *
terminal_find_bar_get_screen (TerminalFindBar *self)
{
  g_return_val_if_fail (TERMINAL_IS_FIND_BAR (self), nullptr);

  return self->screen;
}

void
terminal_find_bar_set_screen (TerminalFindBar  *self,
                              TerminalScreen* screen)
{
  g_return_if_fail (TERMINAL_IS_FIND_BAR (self));
  g_return_if_fail (!screen || TERMINAL_IS_SCREEN (screen));

  if (self->screen == screen)
    return;

  terminal_find_bar_disconnect_screen (self);
  g_set_object (&self->screen, screen);

  if (screen) {
    g_signal_connect_swapped (screen, "contents-changed",
                              G_CALLBACK (terminal_find_bar_screen_changed_cb), self);
    g_signal_connect_swapped (screen, "columns-changed",
                              G_CALLBACK (terminal_find_bar_screen_changed_cb), self);
  }

  // Each terminal starts with an empty search
  gtk_editable_set_text (GTK_EDITABLE (self->entry), "");
  terminal_find_bar_update_regex (self);
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_SCREEN]);
}

/**
 * terminal_find_bar_find:
 * @self: a #TerminalFindBar
 * @direction: -1 for the previous match, 1 for the next one
 */
void
terminal_find_bar_find (TerminalFindBar* self,
                        int direction)
{
  g_return_if_fail (TERMINAL_IS_FIND_BAR (self));

  terminal_find_bar_navigate (self, direction);
}

gboolean
terminal_find_bar_has_search (TerminalFindBar* self)
{
  g_return_val_if_fail (TERMINAL_IS_FIND_BAR (self), FALSE);

  return self->regex != nullptr;
}

void
terminal_find_bar_clear (TerminalFindBar* self)
{
  g_return_if_fail (TERMINAL_IS_FIND_BAR (self));

  gtk_editable_set_text (GTK_EDITABLE (self->entry), "");
}
