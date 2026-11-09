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

#include "config.h"

#include "terminal-command-palette.hh"
#include "terminal-accels.hh"
#include "terminal-app.hh"
#include "terminal-libgsystem.hh"
#include "terminal-profiles-list.hh"
#include "terminal-schemas.hh"
#include "terminal-tab.hh"

#include <glib/gi18n.h>

#define WIDTH 520
#define LIST_HEIGHT 360

/* An entry of the palette: an action with its target, or a terminal to
 * switch to */
typedef struct {
  char* title;
  char* subtitle; // nullable
  char* action_name; // "win.…", or nullptr for a terminal
  GVariant* target; // nullable
  GWeakRef screen;
  char* search_text; // casefolded title and subtitle
  int score; // of the last search, -1 if it does not match
  guint index; // the order in which the entries were added
} PaletteItem;

static void
palette_item_free(PaletteItem* item)
{
  g_free(item->title);
  g_free(item->subtitle);
  g_free(item->action_name);
  g_clear_pointer(&item->target, g_variant_unref);
  g_weak_ref_clear(&item->screen);
  g_free(item->search_text);
  g_free(item);
}

typedef struct {
  TerminalWindow* window; // unowned; the popover goes with it
  GtkPopover* popover;
  GtkSearchEntry* entry;
  GtkListBox* list;
  guint n_items;
} Palette;

static PaletteItem*
row_get_item(GtkListBoxRow* row)
{
  return reinterpret_cast<PaletteItem*>(g_object_get_data(G_OBJECT(row), "palette-item"));
}

static bool
is_word_start(char const* text,
              char const* p)
{
  if (p == text)
    return true;

  auto const prev = g_utf8_get_char(g_utf8_prev_char(p));
  return !g_unichar_isalnum(prev);
}

/**
 * terminal_command_palette_match:
 * @pattern: casefolded text to look for
 * @text: casefolded text to look in
 *
 * Fuzzy matching: the characters of @pattern must appear in @text in the
 * same order. Matches at the start of words and runs of characters score
 * higher.
 *
 * Returns: the score, or -1 if @text does not match
 */
int
terminal_command_palette_match(char const* pattern,
                               char const* text)
{
  auto score = 0;
  auto p = pattern;
  auto t = text;
  auto run = 0;

  while (*p) {
    auto const wanted = g_utf8_get_char(p);
    if (g_unichar_isspace(wanted)) {
      p = g_utf8_next_char(p);
      run = 0;
      continue;
    }

    auto found = false;
    while (*t) {
      auto const c = g_utf8_get_char(t);
      if (c == wanted) {
        found = true;
        score += 1;
        if (is_word_start(text, t))
          score += 8;
        if (run > 0)
          score += 4 * run;
        ++run;
        t = g_utf8_next_char(t);
        break;
      }
      run = 0;
      t = g_utf8_next_char(t);
    }

    if (!found)
      return -1;

    p = g_utf8_next_char(p);
  }

  return score;
}

static void
palette_add(Palette* palette,
            char const* title,
            char const* subtitle,
            char const* accel,
            char const* action_name,
            GVariant* target,
            TerminalScreen* screen)
{
  auto const item = g_new0(PaletteItem, 1);
  item->title = g_strdup(title);
  item->subtitle = g_strdup(subtitle);
  item->action_name = g_strdup(action_name);
  item->target = target ? g_variant_ref_sink(target) : nullptr;
  g_weak_ref_init(&item->screen, screen);
  gs_free char* text = g_strconcat(title, " ", subtitle ? subtitle : "", nullptr);
  item->search_text = g_utf8_casefold(text, -1);
  item->index = palette->n_items++;

  auto const box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_start(box, 6);
  gtk_widget_set_margin_end(box, 6);
  gtk_widget_set_margin_top(box, 4);
  gtk_widget_set_margin_bottom(box, 4);

  auto const labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(labels, true);
  auto const title_label = gtk_label_new(title);
  gtk_label_set_xalign(GTK_LABEL(title_label), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(title_label), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(labels), title_label);
  if (subtitle) {
    auto const subtitle_label = gtk_label_new(subtitle);
    gtk_label_set_xalign(GTK_LABEL(subtitle_label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(subtitle_label), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(subtitle_label, "dim-label");
    gtk_widget_add_css_class(subtitle_label, "caption");
    gtk_box_append(GTK_BOX(labels), subtitle_label);
  }
  gtk_box_append(GTK_BOX(box), labels);

  guint key = 0;
  GdkModifierType mods = GdkModifierType(0);
  if (accel && gtk_accelerator_parse(accel, &key, &mods) && key != 0) {
    gs_free char* label = gtk_accelerator_get_label(key, mods);
    auto const accel_label = gtk_label_new(label);
    gtk_widget_add_css_class(accel_label, "dim-label");
    gtk_widget_set_valign(accel_label, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), accel_label);
  }

  auto const row = gtk_list_box_row_new();
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  g_object_set_data_full(G_OBJECT(row), "palette-item", item, GDestroyNotify(palette_item_free));
  gtk_list_box_append(palette->list, row);
}

/* Actions with keyboard shortcuts, as far as the window can run them now */
static void
add_accel_action_cb(char const* name,
                    char const* action_name,
                    GVariant* target,
                    char const* accel,
                    void* user_data)
{
  auto const palette = reinterpret_cast<Palette*>(user_data);
  if (!g_str_has_prefix(action_name, "win."))
    return;

  auto const group = G_ACTION_GROUP(palette->window);
  auto const short_name = action_name + strlen("win.");
  if (!g_action_group_has_action(group, short_name) ||
      !g_action_group_get_action_enabled(group, short_name))
    return;

  // Opening the palette again from itself is pointless
  if (g_str_equal(short_name, "command-palette"))
    return;

  palette_add(palette, name, nullptr, accel, action_name, target, nullptr);
}

static void
palette_add_profiles(Palette* palette)
{
  auto const profiles = terminal_app_get_profiles_list(terminal_app_get());
  gs_free_list GList* list = terminal_profiles_list_ref_children_sorted(profiles);
  auto const several = list && list->next;

  for (auto l = list; l; l = l->next) {
    auto const settings = G_SETTINGS(l->data);
    gs_free char* uuid = terminal_settings_list_dup_uuid_from_child(profiles, settings);
    gs_free char* name = g_settings_get_string(settings, TERMINAL_PROFILE_VISIBLE_NAME_KEY);
    if (!uuid)
      continue;

    /* Translators: %s is the name of a profile */
    gs_free char* new_tab = g_strdup_printf(_("New Tab with Profile “%s”"), name);
    palette_add(palette, new_tab, nullptr, nullptr, "win.new-terminal",
                g_variant_new("(ss)", "tab", uuid), nullptr);
    /* Translators: %s is the name of a profile */
    gs_free char* new_window = g_strdup_printf(_("New Window with Profile “%s”"), name);
    palette_add(palette, new_window, nullptr, nullptr, "win.new-terminal",
                g_variant_new("(ss)", "window", uuid), nullptr);
    if (several) {
      /* Translators: %s is the name of a profile */
      gs_free char* use = g_strdup_printf(_("Use Profile “%s” in This Terminal"), name);
      palette_add(palette, use, nullptr, nullptr, "win.profile",
                  g_variant_new_string(uuid), nullptr);
    }
  }
  g_list_free_full(g_steal_pointer(&list), g_object_unref);
}

static void
palette_add_tabs(Palette* palette)
{
  auto const active = terminal_window_get_active(palette->window);
  gs_free_list GList* tabs = terminal_window_list_tabs(palette->window);

  auto n = 0;
  for (auto l = tabs; l; l = l->next) {
    ++n;
    gs_free_list GList* screens = terminal_tab_list_screens(TERMINAL_TAB(l->data));
    for (auto s = screens; s; s = s->next) {
      auto const screen = TERMINAL_SCREEN(s->data);
      if (screen == active)
        continue;

      auto const title = terminal_screen_get_title(screen);
      /* Translators: %s is the title of a terminal */
      gs_free char* text = g_strdup_printf(_("Go to “%s”"), title && title[0] ? title : _("Terminal"));
      /* Translators: %d is the number of a tab */
      gs_free char* where = screens->next ? g_strdup_printf(_("A pane in tab %d"), n)
                                          : g_strdup_printf(_("Tab %d"), n);
      palette_add(palette, text, where, nullptr, nullptr, nullptr, screen);
    }
  }
}

static int
palette_sort_cb(GtkListBoxRow* a,
                GtkListBoxRow* b,
                void* user_data)
{
  auto const ia = row_get_item(a);
  auto const ib = row_get_item(b);
  if (ia->score != ib->score)
    return ib->score - ia->score;
  return int(ia->index) - int(ib->index);
}

static gboolean
palette_filter_cb(GtkListBoxRow* row,
                  void* user_data)
{
  return row_get_item(row)->score >= 0;
}

static GtkListBoxRow*
palette_next_row(Palette* palette,
                 GtkListBoxRow* row,
                 int direction)
{
  auto widget = row ? GTK_WIDGET(row) : nullptr;
  if (!widget)
    widget = direction > 0 ? gtk_widget_get_first_child(GTK_WIDGET(palette->list)) : nullptr;
  else
    widget = direction > 0 ? gtk_widget_get_next_sibling(widget) : gtk_widget_get_prev_sibling(widget);

  while (widget && !gtk_widget_get_child_visible(widget))
    widget = direction > 0 ? gtk_widget_get_next_sibling(widget) : gtk_widget_get_prev_sibling(widget);

  return widget ? GTK_LIST_BOX_ROW(widget) : nullptr;
}

static void
palette_select(Palette* palette,
               GtkListBoxRow* row)
{
  gtk_list_box_select_row(palette->list, row);
  if (!row)
    return;

  // Keep the row in view
  auto const adjustment = gtk_list_box_get_adjustment(palette->list);
  graphene_rect_t bounds;
  if (adjustment &&
      gtk_widget_compute_bounds(GTK_WIDGET(row), GTK_WIDGET(palette->list), &bounds)) {
    auto const value = gtk_adjustment_get_value(adjustment);
    auto const page = gtk_adjustment_get_page_size(adjustment);
    if (bounds.origin.y < value)
      gtk_adjustment_set_value(adjustment, bounds.origin.y);
    else if (bounds.origin.y + bounds.size.height > value + page)
      gtk_adjustment_set_value(adjustment, bounds.origin.y + bounds.size.height - page);
  }
}

static void
palette_search_changed_cb(GtkSearchEntry* entry,
                          Palette* palette)
{
  gs_free char* pattern = g_utf8_casefold(gtk_editable_get_text(GTK_EDITABLE(entry)), -1);

  for (auto child = gtk_widget_get_first_child(GTK_WIDGET(palette->list));
       child;
       child = gtk_widget_get_next_sibling(child)) {
    auto const item = row_get_item(GTK_LIST_BOX_ROW(child));
    item->score = terminal_command_palette_match(pattern, item->search_text);
  }

  gtk_list_box_invalidate_sort(palette->list);
  gtk_list_box_invalidate_filter(palette->list);
  palette_select(palette, palette_next_row(palette, nullptr, 1));
}

static void
palette_run(Palette* palette,
            GtkListBoxRow* row)
{
  if (!row)
    return;

  auto const item = row_get_item(row);
  auto const window = palette->window;
  gs_free char* action_name = g_strdup(item->action_name);
  gs_unref_variant auto target = item->target ? g_variant_ref(item->target) : nullptr;
  gs_unref_object auto screen = reinterpret_cast<TerminalScreen*>(g_weak_ref_get(&item->screen));

  // Close first, so that the focus is back in the window
  gtk_popover_popdown(palette->popover);

  if (action_name) {
    gtk_widget_activate_action_variant(GTK_WIDGET(window), action_name, target);
  } else if (screen && gtk_widget_get_root(GTK_WIDGET(screen))) {
    auto const screen_window = TERMINAL_WINDOW(gtk_widget_get_root(GTK_WIDGET(screen)));
    terminal_window_switch_screen(screen_window, screen);
    gtk_widget_grab_focus(GTK_WIDGET(screen));
  }
}

static void
palette_activate_cb(GtkSearchEntry* entry,
                    Palette* palette)
{
  palette_run(palette, gtk_list_box_get_selected_row(palette->list));
}

static void
palette_row_activated_cb(GtkListBox* list,
                         GtkListBoxRow* row,
                         Palette* palette)
{
  palette_run(palette, row);
}

static void
palette_stop_search_cb(GtkSearchEntry* entry,
                       Palette* palette)
{
  gtk_popover_popdown(palette->popover);
}

static gboolean
palette_key_pressed_cb(GtkEventControllerKey* controller,
                       guint keyval,
                       guint keycode,
                       GdkModifierType state,
                       Palette* palette)
{
  int direction;
  switch (keyval) {
  case GDK_KEY_Up:
  case GDK_KEY_KP_Up:
    direction = -1;
    break;
  case GDK_KEY_Down:
  case GDK_KEY_KP_Down:
    direction = 1;
    break;
  default:
    return GDK_EVENT_PROPAGATE;
  }

  auto const current = gtk_list_box_get_selected_row(palette->list);
  if (auto const next = palette_next_row(palette, current, direction))
    palette_select(palette, next);
  return GDK_EVENT_STOP;
}

static gboolean
palette_unparent_cb(void* user_data)
{
  auto const popover = GTK_WIDGET(user_data);
  gtk_widget_unparent(popover);
  g_object_unref(popover);
  return G_SOURCE_REMOVE;
}

static void
palette_closed_cb(GtkPopover* popover,
                  Palette* palette)
{
  // Not while the popover is still handling its signals
  g_idle_add(palette_unparent_cb, g_object_ref(popover));
}

/**
 * terminal_command_palette_show:
 * @window: a #TerminalWindow
 * @parent: the widget at whose top the palette shows
 *
 * Shows the command palette for @window.
 */
void
terminal_command_palette_show(TerminalWindow* window,
                              GtkWidget* parent)
{
  g_return_if_fail(TERMINAL_IS_WINDOW(window));

  auto const palette = g_new0(Palette, 1);
  palette->window = window;

  auto const box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_size_request(box, WIDTH, -1);

  palette->entry = GTK_SEARCH_ENTRY(gtk_search_entry_new());
  gtk_search_entry_set_placeholder_text(palette->entry, _("Search actions, profiles and tabs"));
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(palette->entry));

  palette->list = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(palette->list, GTK_SELECTION_BROWSE);
  gtk_list_box_set_activate_on_single_click(palette->list, true);
  gtk_widget_add_css_class(GTK_WIDGET(palette->list), "navigation-sidebar");
  gtk_list_box_set_sort_func(palette->list, palette_sort_cb, nullptr, nullptr);
  gtk_list_box_set_filter_func(palette->list, palette_filter_cb, nullptr, nullptr);

  auto const scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scrolled), LIST_HEIGHT);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scrolled), LIST_HEIGHT);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), GTK_WIDGET(palette->list));
  gtk_list_box_set_adjustment(palette->list,
                              gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled)));
  gtk_box_append(GTK_BOX(box), scrolled);

  // The entries: the open terminals first, then the actions, then profiles
  palette_add_tabs(palette);
  terminal_accels_foreach_action(add_accel_action_cb, palette);
  palette_add_profiles(palette);

  palette->popover = GTK_POPOVER(gtk_popover_new());
  gtk_popover_set_child(palette->popover, box);
  gtk_popover_set_has_arrow(palette->popover, false);
  gtk_popover_set_position(palette->popover, GTK_POS_BOTTOM);
  gtk_widget_add_css_class(GTK_WIDGET(palette->popover), "command-palette");
  gtk_widget_set_parent(GTK_WIDGET(palette->popover), parent);

  auto const rect = GdkRectangle{gtk_widget_get_width(parent) / 2, 0, 1, 1};
  gtk_popover_set_pointing_to(palette->popover, &rect);

  g_signal_connect(palette->entry, "search-changed", G_CALLBACK(palette_search_changed_cb), palette);
  g_signal_connect(palette->entry, "activate", G_CALLBACK(palette_activate_cb), palette);
  g_signal_connect(palette->entry, "stop-search", G_CALLBACK(palette_stop_search_cb), palette);
  g_signal_connect(palette->list, "row-activated", G_CALLBACK(palette_row_activated_cb), palette);
  auto const keys = gtk_event_controller_key_new();
  g_signal_connect(keys, "key-pressed", G_CALLBACK(palette_key_pressed_cb), palette);
  gtk_widget_add_controller(GTK_WIDGET(palette->entry), keys);
  g_signal_connect(palette->popover, "closed", G_CALLBACK(palette_closed_cb), palette);
  g_object_set_data_full(G_OBJECT(palette->popover), "palette", palette, g_free);

  palette_search_changed_cb(palette->entry, palette);
  gtk_popover_popup(palette->popover);
  gtk_widget_grab_focus(GTK_WIDGET(palette->entry));
}
