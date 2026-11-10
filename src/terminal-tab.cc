/*
 * Copyright © 2008, 2010, 2011 Christian Persch
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

#include "config.h"

#include "terminal-tab.hh"
#include "terminal-app.hh"
#include "terminal-schemas.hh"
#include "terminal-debug.hh"
#include "terminal-intl.hh"
#include "terminal-libgsystem.hh"
#include "terminal-type-builtins.hh"
#include "terminal-window.hh"

#include <adwaita.h>
#include <gtk/gtk.h>

#include <algorithm>
#include <cmath>

/* TerminalPane: a single terminal with its scrolled window and overlays,
 * below an optional title bar */

#define TERMINAL_TYPE_PANE (terminal_pane_get_type())

G_DECLARE_FINAL_TYPE (TerminalPane, terminal_pane, TERMINAL, PANE, GtkWidget)

struct _TerminalPane
{
  GtkWidget parent_instance;

  TerminalScreen* screen; // unowned, child of scrolled_window
  GtkWidget* title_bar;
  GtkWidget* overlay;
  GtkWidget* scrolled_window;
  GtkWidget* scroll_marks; // over the scrollbar
  GtkWidget* drop_indicator; // where a dropped pane or tab would go
};

G_DEFINE_FINAL_TYPE (TerminalPane, terminal_pane, GTK_TYPE_WIDGET)

/* Marks next to the scrollbar for the commands that ran, from shell
 * integration; red for those that failed */
static void
scroll_marks_draw_cb(GtkDrawingArea* area,
                     cairo_t* cr,
                     int width,
                     int height,
                     TerminalPane* pane)
{
  if (!pane->screen)
    return;

  gs_unref_array auto marks = terminal_screen_dup_scroll_marks(pane->screen);
  if (marks->len == 0)
    return;

  GdkRGBA color;
  gtk_widget_get_color(GTK_WIDGET(area), &color);
  static GdkRGBA const failed_color = {0.88f, 0.11f, 0.14f, 1.0f};

  for (auto i = 0u; i < marks->len; ++i) {
    auto const& mark = g_array_index(marks, TerminalScrollMark, i);
    auto const y = std::clamp(mark.position * height, 0.0, double(height - 2));

    if (mark.failed)
      gdk_cairo_set_source_rgba(cr, &failed_color);
    else
      cairo_set_source_rgba(cr, color.red, color.green, color.blue, 0.45);
    cairo_rectangle(cr, 0, floor(y), width, 2);
    cairo_fill(cr);
  }
}

/* Drag and drop: a pane is dragged by its title bar, which shows while the
 * tab is split, onto another pane, the tab bar or out of the window. A pane
 * also takes tabs, which then become panes. */

static GdkContentProvider*
pane_drag_prepare_cb(GtkDragSource* source,
                     double x,
                     double y,
                     TerminalPane* pane)
{
  auto const tab = terminal_tab_get_from_screen(pane->screen);
  if (!tab || !terminal_tab_is_split(tab))
    return nullptr;

  return gdk_content_provider_new_typed(TERMINAL_TYPE_SCREEN, pane->screen);
}

static void
pane_drag_begin_cb(GtkDragSource* source,
                   GdkDrag* drag,
                   TerminalPane* pane)
{
  gs_unref_object auto paintable = gtk_widget_paintable_new(pane->title_bar);
  gtk_drag_source_set_icon(source, paintable, 0, 0);
}

static gboolean
pane_move_to_window_cb(void* data)
{
  auto const ref = reinterpret_cast<GWeakRef*>(data);
  if (auto const screen = reinterpret_cast<TerminalScreen*>(g_weak_ref_get(ref))) {
    if (auto const window = gtk_widget_get_root(GTK_WIDGET(screen)); TERMINAL_IS_WINDOW(window))
      terminal_window_move_pane(TERMINAL_WINDOW(window), screen, true);
    g_object_unref(screen);
  }
  g_weak_ref_clear(ref);
  g_free(ref);
  return G_SOURCE_REMOVE;
}

/* A pane dropped where nothing takes it gets its own window, like a tab */
static gboolean
pane_drag_cancel_cb(GtkDragSource* source,
                    GdkDrag* drag,
                    GdkDragCancelReason reason,
                    TerminalPane* pane)
{
  if (reason != GDK_DRAG_CANCEL_NO_TARGET || !pane->screen)
    return false;

  // Not while the drag source is still busy with the pane
  auto const ref = g_new0(GWeakRef, 1);
  g_weak_ref_init(ref, pane->screen);
  g_idle_add(pane_move_to_window_cb, ref);
  return true;
}

/* Where @object, a pane's #TerminalScreen or an #AdwTabPage, dropped at
 * @x, @y, would go, or -1 if it cannot go here: next to the nearest side,
 * or in the middle of a pane of the same tab in place of the pane */
static int
pane_drop_zone(TerminalPane* pane,
               GObject* object,
               double x,
               double y)
{
  if (!pane->screen)
    return -1;

  auto const tab = terminal_tab_get_from_screen(pane->screen);
  auto same_tab = false;
  if (TERMINAL_IS_SCREEN(object)) {
    auto const screen = TERMINAL_SCREEN(object);
    if (screen == pane->screen)
      return -1;
    same_tab = terminal_tab_get_from_screen(screen) == tab;
  } else if (ADW_IS_TAB_PAGE(object)) {
    if (adw_tab_page_get_child(ADW_TAB_PAGE(object)) == GTK_WIDGET(tab))
      return -1;
  }

  auto const width = double(std::max(gtk_widget_get_width(GTK_WIDGET(pane)), 1));
  auto const height = double(std::max(gtk_widget_get_height(GTK_WIDGET(pane)), 1));
  auto const fx = x / width, fy = y / height;

  if (same_tab && fx > 1. / 3 && fx < 2. / 3 && fy > 1. / 3 && fy < 2. / 3)
    return TERMINAL_PANE_DROP_SWAP;

  auto zone = TERMINAL_PANE_DROP_LEFT;
  auto distance = fx;
  if (1 - fx < distance) {
    zone = TERMINAL_PANE_DROP_RIGHT;
    distance = 1 - fx;
  }
  if (fy < distance) {
    zone = TERMINAL_PANE_DROP_TOP;
    distance = fy;
  }
  if (1 - fy < distance)
    zone = TERMINAL_PANE_DROP_BOTTOM;
  return zone;
}

static void
pane_show_drop_indicator(TerminalPane* pane,
                         int zone)
{
  auto const indicator = pane->drop_indicator;
  if (zone < 0) {
    gtk_widget_set_visible(indicator, false);
    return;
  }

  auto const width = gtk_widget_get_width(pane->overlay);
  auto const height = gtk_widget_get_height(pane->overlay);
  auto halign = GTK_ALIGN_FILL, valign = GTK_ALIGN_FILL;
  int w = -1, h = -1;
  switch (zone) {
  case TERMINAL_PANE_DROP_LEFT:   halign = GTK_ALIGN_START; w = width / 2;  break;
  case TERMINAL_PANE_DROP_RIGHT:  halign = GTK_ALIGN_END;   w = width / 2;  break;
  case TERMINAL_PANE_DROP_TOP:    valign = GTK_ALIGN_START; h = height / 2; break;
  case TERMINAL_PANE_DROP_BOTTOM: valign = GTK_ALIGN_END;   h = height / 2; break;
  default: break;
  }
  gtk_widget_set_halign(indicator, halign);
  gtk_widget_set_valign(indicator, valign);
  gtk_widget_set_size_request(indicator, w, h);
  gtk_widget_set_visible(indicator, true);
}

static GdkDragAction
pane_drop_motion_cb(GtkDropTarget* target,
                    double x,
                    double y,
                    TerminalPane* pane)
{
  auto const value = gtk_drop_target_get_value(target);
  auto const zone = pane_drop_zone(pane, value ? G_OBJECT(g_value_get_object(value)) : nullptr, x, y);
  pane_show_drop_indicator(pane, zone);
  return zone < 0 ? GdkDragAction(0) : GDK_ACTION_MOVE;
}

static void
pane_drop_leave_cb(GtkDropTarget* target,
                   TerminalPane* pane)
{
  pane_show_drop_indicator(pane, -1);
}

static gboolean
pane_drop_cb(GtkDropTarget* target,
             GValue const* value,
             double x,
             double y,
             TerminalPane* pane)
{
  pane_show_drop_indicator(pane, -1);

  auto const zone = pane_drop_zone(pane, G_OBJECT(g_value_get_object(value)), x, y);
  auto const window = gtk_widget_get_root(GTK_WIDGET(pane));
  if (zone < 0 || !TERMINAL_IS_WINDOW(window))
    return false;

  return terminal_window_drop_on_pane(TERMINAL_WINDOW(window),
                                      pane->screen,
                                      G_OBJECT(g_value_get_object(value)),
                                      TerminalPaneDrop(zone));
}

static void
terminal_pane_init(TerminalPane* pane)
{
  pane->title_bar = gtk_label_new(nullptr);
  gtk_widget_add_css_class(pane->title_bar, "pane-title");
  gtk_label_set_ellipsize(GTK_LABEL(pane->title_bar), PANGO_ELLIPSIZE_END);
  gtk_label_set_single_line_mode(GTK_LABEL(pane->title_bar), true);
  gtk_widget_set_visible(pane->title_bar, false);
  gtk_widget_set_parent(pane->title_bar, GTK_WIDGET(pane));

  pane->overlay = gtk_overlay_new();
  gtk_widget_set_vexpand(pane->overlay, true);
  gtk_widget_set_parent(pane->overlay, GTK_WIDGET(pane));

  pane->scrolled_window =
    GTK_WIDGET(g_object_new(GTK_TYPE_SCROLLED_WINDOW,
                            "propagate-natural-width", TRUE,
                            "propagate-natural-height", TRUE,
                            nullptr));
  gtk_overlay_set_child(GTK_OVERLAY(pane->overlay), pane->scrolled_window);

  pane->scroll_marks = gtk_drawing_area_new();
  gtk_widget_add_css_class(pane->scroll_marks, "scroll-marks");
  gtk_widget_set_halign(pane->scroll_marks, GTK_ALIGN_END);
  gtk_widget_set_size_request(pane->scroll_marks, 4, -1);
  gtk_widget_set_can_target(pane->scroll_marks, false);
  gtk_widget_set_can_focus(pane->scroll_marks, false);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(pane->scroll_marks),
                                 GtkDrawingAreaDrawFunc(scroll_marks_draw_cb),
                                 pane, nullptr);
  gtk_overlay_add_overlay(GTK_OVERLAY(pane->overlay), pane->scroll_marks);

  pane->drop_indicator = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(pane->drop_indicator, "pane-drop-indicator");
  gtk_widget_set_can_target(pane->drop_indicator, false);
  gtk_widget_set_visible(pane->drop_indicator, false);
  gtk_overlay_add_overlay(GTK_OVERLAY(pane->overlay), pane->drop_indicator);

  auto const drag_source = gtk_drag_source_new();
  gtk_drag_source_set_actions(drag_source, GDK_ACTION_MOVE);
  g_signal_connect(drag_source, "prepare", G_CALLBACK(pane_drag_prepare_cb), pane);
  g_signal_connect(drag_source, "drag-begin", G_CALLBACK(pane_drag_begin_cb), pane);
  g_signal_connect(drag_source, "drag-cancel", G_CALLBACK(pane_drag_cancel_cb), pane);
  gtk_widget_add_controller(pane->title_bar, GTK_EVENT_CONTROLLER(drag_source));
  gtk_widget_set_cursor_from_name(pane->title_bar, "grab");

  GType types[] = { TERMINAL_TYPE_SCREEN, ADW_TYPE_TAB_PAGE };
  auto const drop_target = gtk_drop_target_new(G_TYPE_INVALID, GDK_ACTION_MOVE);
  gtk_drop_target_set_gtypes(drop_target, types, G_N_ELEMENTS(types));
  gtk_drop_target_set_preload(drop_target, true);
  g_signal_connect(drop_target, "enter", G_CALLBACK(pane_drop_motion_cb), pane);
  g_signal_connect(drop_target, "motion", G_CALLBACK(pane_drop_motion_cb), pane);
  g_signal_connect(drop_target, "leave", G_CALLBACK(pane_drop_leave_cb), pane);
  g_signal_connect(drop_target, "drop", G_CALLBACK(pane_drop_cb), pane);
  gtk_widget_add_controller(GTK_WIDGET(pane), GTK_EVENT_CONTROLLER(drop_target));
}

static void
terminal_pane_dispose(GObject* object)
{
  auto const pane = TERMINAL_PANE(object);

  pane->screen = nullptr;
  g_clear_pointer(&pane->title_bar, gtk_widget_unparent);
  g_clear_pointer(&pane->overlay, gtk_widget_unparent);

  G_OBJECT_CLASS(terminal_pane_parent_class)->dispose(object);
}

static void
terminal_pane_class_init(TerminalPaneClass* klass)
{
  auto const gobject_class = G_OBJECT_CLASS(klass);
  auto const widget_class = GTK_WIDGET_CLASS(klass);

  gobject_class->dispose = terminal_pane_dispose;

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BOX_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "terminal-pane");
}

static void
terminal_pane_set_policy(TerminalPane* pane,
                         TerminalScrollbarPolicy vpolicy)
{
  auto vpolicy_to_gtk = [](TerminalScrollbarPolicy policy) constexpr noexcept -> auto
  {
    switch (policy) {
    case TERMINAL_SCROLLBAR_POLICY_NEVER: return GTK_POLICY_EXTERNAL;
    case TERMINAL_SCROLLBAR_POLICY_OVERLAY: return GTK_POLICY_AUTOMATIC;
    default: terminal_assert_not_reached();
      [[fallthrough]];
    case TERMINAL_SCROLLBAR_POLICY_ALWAYS: return GTK_POLICY_ALWAYS;
    }
  };

  auto const scrolled_window = GTK_SCROLLED_WINDOW(pane->scrolled_window);
  // The terminal never scrolls horizontally
  gtk_scrolled_window_set_policy(scrolled_window,
                                 GTK_POLICY_NEVER,
                                 vpolicy_to_gtk(vpolicy));
  gtk_scrolled_window_set_overlay_scrolling(scrolled_window,
                                            vpolicy == TERMINAL_SCROLLBAR_POLICY_OVERLAY);

  // The marks go with the scrollbar
  gtk_widget_set_visible(pane->scroll_marks, vpolicy != TERMINAL_SCROLLBAR_POLICY_NEVER);
}

static TerminalPane*
terminal_pane_new(TerminalScreen* screen,
                  bool visible)
{
  auto const pane = TERMINAL_PANE(g_object_new(TERMINAL_TYPE_PANE, nullptr));

  pane->screen = screen;
  gtk_orientable_set_orientation(GTK_ORIENTABLE(gtk_widget_get_layout_manager(GTK_WIDGET(pane))),
                                 GTK_ORIENTATION_VERTICAL);
  g_object_bind_property(screen, "title", pane->title_bar, "label", G_BINDING_SYNC_CREATE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(pane->scrolled_window),
                                GTK_WIDGET(screen));

  // Redraw the marks when they change, and when the scrollback grows
  g_signal_connect_object(screen, "prompts-changed",
                          G_CALLBACK(gtk_widget_queue_draw), pane->scroll_marks,
                          G_CONNECT_SWAPPED);
  if (auto const adjustment = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(screen)))
    g_signal_connect_object(adjustment, "changed",
                            G_CALLBACK(gtk_widget_queue_draw), pane->scroll_marks,
                            G_CONNECT_SWAPPED);
  // The real policy is applied from the profile once the screen is rooted
  terminal_pane_set_policy(pane, TERMINAL_SCROLLBAR_POLICY_NEVER);
  gtk_widget_set_visible(pane->scrolled_window, visible);

  return pane;
}

static TerminalPane*
pane_from_screen(TerminalScreen* screen)
{
  if (screen == nullptr)
    return nullptr;

  return TERMINAL_PANE(gtk_widget_get_ancestor(GTK_WIDGET(screen), TERMINAL_TYPE_PANE));
}

/* TerminalTab */

struct _TerminalTab
{
  GtkWidget parent_instance;

  GtkWidget* root; // a TerminalPane, or a GtkPaned of panes
  TerminalScreen* active_screen; // unowned
  GQueue screens; // TerminalScreen*, unowned, most recently focused first
  GBindingGroup* bindings;

  char* title;
  GIcon* icon;
  GIcon* icon_progress;

  bool pinned;
  bool active;
  bool zoomed;
};

enum
{
  PROP_0,
  PROP_SCREEN,
  PROP_TITLE,
  PROP_ICON,
  PROP_ICON_PROGRESS,
  PROP_ZOOMED,
  N_PROPS
};

enum
{
  SCREEN_ADDED,
  SCREEN_REMOVED,
  LAST_SIGNAL
};

static GParamSpec* pspecs[N_PROPS];
static guint signals[LAST_SIGNAL];

G_DEFINE_FINAL_TYPE (TerminalTab, terminal_tab, GTK_TYPE_WIDGET)

#define TERMINAL_TAB_CSS_NAME "terminal-tab"

static void tab_update_panes(TerminalTab* tab);

static void
tab_set_active_screen(TerminalTab* tab,
                      TerminalScreen* screen)
{
  if (screen) {
    // Keep the list in order of focus
    g_queue_remove(&tab->screens, screen);
    g_queue_push_head(&tab->screens, screen);
  }

  if (tab->active_screen == screen)
    return;

  tab->active_screen = screen;
  g_binding_group_set_source(tab->bindings, screen);
  g_object_notify_by_pspec(G_OBJECT(tab), pspecs[PROP_SCREEN]);
  tab_update_panes(tab);
}

static void
screen_has_focus_cb(TerminalScreen* screen,
                    GParamSpec* pspec,
                    TerminalTab* tab)
{
  if (gtk_widget_has_focus(GTK_WIDGET(screen)))
    tab_set_active_screen(tab, screen);
}

static void
tab_track_screen(TerminalTab* tab,
                 TerminalScreen* screen)
{
  g_queue_push_tail(&tab->screens, screen);
  g_signal_connect(screen, "notify::has-focus",
                   G_CALLBACK(screen_has_focus_cb), tab);
}

static void
tab_untrack_screen(TerminalTab* tab,
                   TerminalScreen* screen)
{
  g_signal_handlers_disconnect_by_func(screen,
                                       (void*)screen_has_focus_cb,
                                       tab);
  g_queue_remove(&tab->screens, screen);
}

/* Moving a widget that has the focus in it leaves GtkPaned with a stale
 * focus child. So the focus goes away while widgets in @subtree move, and
 * comes back afterwards with tab_restore_focus(). */
static GtkWidget*
tab_release_focus(TerminalTab* tab,
                  GtkWidget* subtree)
{
  auto const root = gtk_widget_get_root(GTK_WIDGET(tab));
  GtkWidget* focus = root ? gtk_root_get_focus(root) : nullptr;
  if (!focus || !(focus == subtree || gtk_widget_is_ancestor(focus, subtree)))
    return nullptr;

  g_object_ref(focus);
  gtk_root_set_focus(root, nullptr);
  return focus;
}

static void
tab_restore_focus(GtkWidget* focus)
{
  if (!focus)
    return;

  if (gtk_widget_get_root(focus))
    gtk_widget_grab_focus(focus);
  g_object_unref(focus);
}

/* Puts @new_child where @old_child is in the tree. Returns a reference to
 * @old_child, which is no longer parented.
 */
static GtkWidget*
tab_replace_child(TerminalTab* tab,
                  GtkWidget* old_child,
                  GtkWidget* new_child)
{
  auto const parent = gtk_widget_get_parent(old_child);

  g_object_ref(old_child);

  if (parent == GTK_WIDGET(tab)) {
    gtk_widget_unparent(old_child);
    tab->root = new_child;
    gtk_widget_set_parent(new_child, GTK_WIDGET(tab));
  } else {
    auto const paned = GTK_PANED(parent);
    if (gtk_paned_get_start_child(paned) == old_child)
      gtk_paned_set_start_child(paned, new_child);
    else
      gtk_paned_set_end_child(paned, new_child);
  }

  return old_child;
}

static void
collect_screens(GtkWidget* widget,
                GList** list)
{
  if (widget == nullptr)
    return;

  if (TERMINAL_IS_PANE(widget)) {
    auto const pane = TERMINAL_PANE(widget);
    if (pane->screen)
      *list = g_list_prepend(*list, pane->screen);
  } else if (GTK_IS_PANED(widget)) {
    collect_screens(gtk_paned_get_start_child(GTK_PANED(widget)), list);
    collect_screens(gtk_paned_get_end_child(GTK_PANED(widget)), list);
  }
}

static void
show_all_panes(GtkWidget* widget)
{
  if (!GTK_IS_PANED(widget))
    return;

  auto const paned = GTK_PANED(widget);
  for (auto const child : {gtk_paned_get_start_child(paned),
                           gtk_paned_get_end_child(paned)}) {
    if (!child)
      continue;
    gtk_widget_set_visible(child, true);
    show_all_panes(child);
  }
}

/* Marks the panes other than the active one, which the CSS may dim, and
 * shows the panes' title bars if wanted. Both only while the tab is split. */
static void
tab_update_panes(TerminalTab* tab)
{
  if (!tab->root)
    return;

  auto const split = g_queue_get_length(&tab->screens) > 1;
  auto const settings = terminal_app_get_global_settings(terminal_app_get());
  auto const titles = split && tab->active &&
    g_settings_get_boolean(settings, TERMINAL_SETTING_SHOW_PANE_TITLES_KEY);

  GList* screens = nullptr;
  collect_screens(tab->root, &screens);
  for (auto l = screens; l; l = l->next) {
    auto const screen = reinterpret_cast<TerminalScreen*>(l->data);
    auto const pane = pane_from_screen(screen);
    if (!pane)
      continue;

    if (split && screen != tab->active_screen)
      gtk_widget_add_css_class(GTK_WIDGET(pane), "inactive");
    else
      gtk_widget_remove_css_class(GTK_WIDGET(pane), "inactive");
    gtk_widget_set_visible(pane->title_bar, titles);
  }
  g_list_free(screens);
}

static void
terminal_tab_init (TerminalTab *tab)
{
  tab->pinned = false;
  tab->active = true;
  tab->zoomed = false;
  g_queue_init(&tab->screens);

  tab->bindings = g_binding_group_new();
  g_binding_group_bind(tab->bindings, "title", tab, "title", G_BINDING_SYNC_CREATE);
  g_binding_group_bind(tab->bindings, "icon", tab, "icon", G_BINDING_SYNC_CREATE);
  g_binding_group_bind(tab->bindings, "icon-progress", tab, "icon-progress", G_BINDING_SYNC_CREATE);
}

static void
terminal_tab_constructed (GObject *object)
{
  TerminalTab *tab = TERMINAL_TAB (object);

  G_OBJECT_CLASS (terminal_tab_parent_class)->constructed (object);

  g_assert (tab->active_screen != nullptr);

  auto const screen = tab->active_screen;
  tab->active_screen = nullptr;

  auto const pane = terminal_pane_new(screen, tab->active);
  tab->root = GTK_WIDGET(pane);
  gtk_widget_set_parent(tab->root, GTK_WIDGET(tab));

  tab_track_screen(tab, screen);
  tab_set_active_screen(tab, screen);

  g_signal_connect_object(terminal_app_get_global_settings(terminal_app_get()),
                          "changed::" TERMINAL_SETTING_SHOW_PANE_TITLES_KEY,
                          G_CALLBACK(tab_update_panes), tab,
                          G_CONNECT_SWAPPED);
}

static void
terminal_tab_dispose (GObject *object)
{
  TerminalTab *tab = TERMINAL_TAB (object);

  while (!g_queue_is_empty(&tab->screens))
    tab_untrack_screen(tab, reinterpret_cast<TerminalScreen*>(g_queue_peek_head(&tab->screens)));

  tab->active_screen = nullptr;
  if (tab->bindings)
    g_binding_group_set_source(tab->bindings, nullptr);
  g_clear_object(&tab->bindings);

  g_clear_pointer (&tab->root, gtk_widget_unparent);

  G_OBJECT_CLASS (terminal_tab_parent_class)->dispose (object);
}

static void
terminal_tab_finalize(GObject* object)
{
  auto const tab = TERMINAL_TAB(object);

  g_free(tab->title);
  g_clear_object(&tab->icon);
  g_clear_object(&tab->icon_progress);

  G_OBJECT_CLASS(terminal_tab_parent_class)->finalize(object);
}

static void
terminal_tab_get_property (GObject *object,
                           guint prop_id,
                           GValue *value,
                           GParamSpec *pspec)
{
  TerminalTab *tab = TERMINAL_TAB (object);

  switch (prop_id) {
    case PROP_SCREEN:
      g_value_set_object (value, tab->active_screen);
      break;
    case PROP_TITLE:
      g_value_set_string(value, tab->title);
      break;
    case PROP_ICON:
      g_value_set_object(value, tab->icon);
      break;
    case PROP_ICON_PROGRESS:
      g_value_set_object(value, tab->icon_progress);
      break;
    case PROP_ZOOMED:
      g_value_set_boolean(value, tab->zoomed);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
set_icon(TerminalTab* tab,
         GIcon** icon,
         GIcon* value,
         GParamSpec* pspec)
{
  if (g_set_object(icon, value))
    g_object_notify_by_pspec(G_OBJECT(tab), pspec);
}

static void
terminal_tab_set_property (GObject *object,
                           guint prop_id,
                           const GValue *value,
                           GParamSpec *pspec)
{
  TerminalTab *tab = TERMINAL_TAB (object);

  switch (prop_id) {
    case PROP_SCREEN:
      // Construct-only; the screen is put into the tab in constructed()
      tab->active_screen = reinterpret_cast<TerminalScreen*>(g_value_get_object(value));
      break;
    case PROP_TITLE:
      if (g_set_str(&tab->title, g_value_get_string(value)))
        g_object_notify_by_pspec(object, pspec);
      break;
    case PROP_ICON:
      set_icon(tab, &tab->icon, G_ICON(g_value_get_object(value)), pspec);
      break;
    case PROP_ICON_PROGRESS:
      set_icon(tab, &tab->icon_progress, G_ICON(g_value_get_object(value)), pspec);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
terminal_tab_class_init (TerminalTabClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  gobject_class->constructed = terminal_tab_constructed;
  gobject_class->dispose = terminal_tab_dispose;
  gobject_class->finalize = terminal_tab_finalize;
  gobject_class->get_property = terminal_tab_get_property;
  gobject_class->set_property = terminal_tab_set_property;

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, TERMINAL_TAB_CSS_NAME);

  pspecs[PROP_SCREEN] =
    g_param_spec_object("screen", nullptr, nullptr,
                        TERMINAL_TYPE_SCREEN,
                        GParamFlags(G_PARAM_READWRITE |
                                    G_PARAM_CONSTRUCT_ONLY |
                                    G_PARAM_STATIC_STRINGS));

  // These follow the active screen
  pspecs[PROP_TITLE] =
    g_param_spec_string("title", nullptr, nullptr,
                        nullptr,
                        GParamFlags(G_PARAM_READWRITE |
                                    G_PARAM_STATIC_STRINGS |
                                    G_PARAM_EXPLICIT_NOTIFY));
  pspecs[PROP_ICON] =
    g_param_spec_object("icon", nullptr, nullptr,
                        G_TYPE_ICON,
                        GParamFlags(G_PARAM_READWRITE |
                                    G_PARAM_STATIC_STRINGS |
                                    G_PARAM_EXPLICIT_NOTIFY));
  pspecs[PROP_ICON_PROGRESS] =
    g_param_spec_object("icon-progress", nullptr, nullptr,
                        G_TYPE_ICON,
                        GParamFlags(G_PARAM_READWRITE |
                                    G_PARAM_STATIC_STRINGS |
                                    G_PARAM_EXPLICIT_NOTIFY));

  pspecs[PROP_ZOOMED] =
    g_param_spec_boolean("zoomed", nullptr, nullptr,
                         false,
                         GParamFlags(G_PARAM_READABLE |
                                     G_PARAM_STATIC_STRINGS |
                                     G_PARAM_EXPLICIT_NOTIFY));

  g_object_class_install_properties(gobject_class, G_N_ELEMENTS(pspecs), pspecs);

  signals[SCREEN_ADDED] =
    g_signal_new(I_("screen-added"),
                 G_OBJECT_CLASS_TYPE(gobject_class),
                 G_SIGNAL_RUN_LAST,
                 0,
                 nullptr, nullptr,
                 g_cclosure_marshal_VOID__OBJECT,
                 G_TYPE_NONE,
                 1, TERMINAL_TYPE_SCREEN);

  signals[SCREEN_REMOVED] =
    g_signal_new(I_("screen-removed"),
                 G_OBJECT_CLASS_TYPE(gobject_class),
                 G_SIGNAL_RUN_LAST,
                 0,
                 nullptr, nullptr,
                 g_cclosure_marshal_VOID__OBJECT,
                 G_TYPE_NONE,
                 1, TERMINAL_TYPE_SCREEN);

  g_type_ensure(TERMINAL_TYPE_PANE);
}

/* public API */

/**
 * terminal_tab_new:
 * @screen: a #TerminalScreen
 *
 * Returns: a new #TerminalTab for @screen
 */
TerminalTab*
terminal_tab_new (TerminalScreen *screen)
{
  return reinterpret_cast<TerminalTab*>
    (g_object_new (TERMINAL_TYPE_TAB,
		   "screen", screen,
		   nullptr));
}

/**
 * terminal_tab_get_screen:
 * @tab: a #TerminalTab
 *
 * Returns: the active #TerminalScreen of @tab, that is the pane that had
 *   the focus last
 */
TerminalScreen *
terminal_tab_get_screen (TerminalTab *tab)
{
  if (tab == nullptr)
    return nullptr;

  g_return_val_if_fail (TERMINAL_IS_TAB (tab), nullptr);

  return tab->active_screen;
}

/**
 * terminal_tab_set_active_screen:
 * @tab: a #TerminalTab
 * @screen: a #TerminalScreen in @tab
 *
 * Makes @screen the active screen of @tab. This does not move the focus.
 */
void
terminal_tab_set_active_screen(TerminalTab* tab,
                               TerminalScreen* screen)
{
  g_return_if_fail(TERMINAL_IS_TAB(tab));
  g_return_if_fail(TERMINAL_IS_SCREEN(screen));
  g_return_if_fail(terminal_tab_get_from_screen(screen) == tab);

  if (tab->zoomed && screen != tab->active_screen)
    terminal_tab_set_zoomed(tab, false);

  tab_set_active_screen(tab, screen);
}

/**
 * terminal_tab_list_screens:
 * @tab: a #TerminalTab
 *
 * Returns: (transfer container): all screens of @tab, from top left to
 *   bottom right
 */
GList*
terminal_tab_list_screens(TerminalTab* tab)
{
  g_return_val_if_fail(TERMINAL_IS_TAB(tab), nullptr);

  GList* list = nullptr;
  collect_screens(tab->root, &list);
  return g_list_reverse(list);
}

guint
terminal_tab_get_n_screens(TerminalTab* tab)
{
  g_return_val_if_fail(TERMINAL_IS_TAB(tab), 0);

  return g_queue_get_length(&tab->screens);
}

bool
terminal_tab_is_split(TerminalTab* tab)
{
  return terminal_tab_get_n_screens(tab) > 1;
}

/**
 * terminal_tab_split:
 * @tab: a #TerminalTab
 * @screen: a #TerminalScreen in @tab
 * @new_screen: a new #TerminalScreen
 * @orientation: %GTK_ORIENTATION_HORIZONTAL to put @new_screen to the
 *   right of @screen, %GTK_ORIENTATION_VERTICAL to put it below
 * @position: the share of the space that @screen keeps, for example 0.5
 *
 * Splits the pane of @screen in two and puts @new_screen into the new
 * half. The caller should move the focus to @new_screen.
 */
/* The size of @paned along its orientation. Its max-position is less than
 * that, by the minimum size of its end child. */
static int
paned_get_size(GtkPaned* paned)
{
  auto const widget = GTK_WIDGET(paned);
  return gtk_orientable_get_orientation(GTK_ORIENTABLE(paned)) == GTK_ORIENTATION_HORIZONTAL
    ? gtk_widget_get_width(widget)
    : gtk_widget_get_height(widget);
}

static void
paned_apply_position_cb(GtkPaned* paned,
                        GParamSpec* pspec,
                        void* user_data)
{
  auto const size = paned_get_size(paned);
  if (size <= 0)
    return;

  auto const fraction = *reinterpret_cast<double*>(g_object_get_data(G_OBJECT(paned), "terminal-position"));
  g_signal_handlers_disconnect_by_func(paned, (void*)paned_apply_position_cb, user_data);
  gtk_paned_set_position(paned, int(fraction * size + 0.5));
}

/* Sets the position as a share of the available space, now or as soon as
 * the paned has been allocated.
 */
static void
paned_set_fraction(GtkPaned* paned,
                   double fraction,
                   int size)
{
  fraction = std::clamp(fraction, 0.05, 0.95);

  if (size > 0) {
    gtk_paned_set_position(paned, int(fraction * size + 0.5));
    return;
  }

  auto const data = g_new(double, 1);
  *data = fraction;
  g_object_set_data_full(G_OBJECT(paned), "terminal-position", data, g_free);
  g_signal_connect(paned, "notify::max-position",
                   G_CALLBACK(paned_apply_position_cb), nullptr);
}

static double
paned_get_fraction(GtkPaned* paned)
{
  auto const size = paned_get_size(paned);
  if (size <= 0)
    return 0.5;

  return std::clamp(double(gtk_paned_get_position(paned)) / size, 0.0, 1.0);
}

void
terminal_tab_split(TerminalTab* tab,
                   TerminalScreen* screen,
                   TerminalScreen* new_screen,
                   GtkOrientation orientation,
                   double position)
{
  g_return_if_fail(TERMINAL_IS_TAB(tab));
  g_return_if_fail(TERMINAL_IS_SCREEN(new_screen));
  g_return_if_fail(gtk_widget_get_parent(GTK_WIDGET(new_screen)) == nullptr);

  auto const pane = pane_from_screen(screen);
  g_return_if_fail(pane != nullptr);
  g_return_if_fail(gtk_widget_is_ancestor(GTK_WIDGET(pane), GTK_WIDGET(tab)));

  terminal_tab_set_zoomed(tab, false);

  auto const size = orientation == GTK_ORIENTATION_HORIZONTAL
    ? gtk_widget_get_width(GTK_WIDGET(pane))
    : gtk_widget_get_height(GTK_WIDGET(pane));

  auto const new_pane = terminal_pane_new(new_screen, tab->active);

  auto const paned = GTK_PANED(gtk_paned_new(orientation));
  gtk_paned_set_resize_start_child(paned, true);
  gtk_paned_set_resize_end_child(paned, true);
  gtk_paned_set_shrink_start_child(paned, false);
  gtk_paned_set_shrink_end_child(paned, false);

  auto const old_pane = tab_replace_child(tab, GTK_WIDGET(pane), GTK_WIDGET(paned));
  gtk_paned_set_start_child(paned, old_pane);
  g_object_unref(old_pane);
  gtk_paned_set_end_child(paned, GTK_WIDGET(new_pane));

  paned_set_fraction(paned, position, size);

  tab_track_screen(tab, new_screen);
  tab_update_panes(tab);
  g_signal_emit(tab, signals[SCREEN_ADDED], 0, new_screen);
}

/* Takes the pane of @screen out of the tree; its space goes to the
 * neighbouring pane. Returns a reference to @screen, no longer parented,
 * if @keep_screen, else drops it together with the pane.
 */
static TerminalScreen*
tab_remove_pane(TerminalTab* tab,
                TerminalScreen* screen,
                bool keep_screen)
{
  auto const pane = pane_from_screen(screen);
  g_return_val_if_fail(pane != nullptr, nullptr);

  auto const parent = gtk_widget_get_parent(GTK_WIDGET(pane));
  g_return_val_if_fail(GTK_IS_PANED(parent), nullptr);
  g_return_val_if_fail(gtk_widget_is_ancestor(parent, GTK_WIDGET(tab)), nullptr);

  terminal_tab_set_zoomed(tab, false);

  // Make the most recently focused other pane active first, so that the
  // removed screen is never the active one while it goes away.
  if (tab->active_screen == screen) {
    for (auto l = tab->screens.head; l; l = l->next) {
      auto const other = reinterpret_cast<TerminalScreen*>(l->data);
      if (other != screen) {
        tab_set_active_screen(tab, other);
        break;
      }
    }
  }

  // Emit before changing the tree, while the tab is still split
  g_signal_emit(tab, signals[SCREEN_REMOVED], 0, screen);

  tab_untrack_screen(tab, screen);

  if (keep_screen) {
    g_object_ref(screen);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(pane->scrolled_window), nullptr);
    pane->screen = nullptr;
  }

  auto const paned = GTK_PANED(parent);
  GtkWidget* sibling = gtk_paned_get_start_child(paned) == GTK_WIDGET(pane)
    ? gtk_paned_get_end_child(paned)
    : gtk_paned_get_start_child(paned);

  auto const focus = tab_release_focus(tab, GTK_WIDGET(paned));

  g_object_ref(sibling);
  if (gtk_paned_get_start_child(paned) == sibling)
    gtk_paned_set_start_child(paned, nullptr);
  else
    gtk_paned_set_end_child(paned, nullptr);

  // This drops the paned, and with it the pane (and its screen)
  auto const old_paned = tab_replace_child(tab, GTK_WIDGET(paned), sibling);
  g_object_unref(sibling);
  g_object_unref(old_paned);

  tab_restore_focus(focus);
  tab_update_panes(tab);

  return keep_screen ? screen : nullptr;
}

/**
 * terminal_tab_remove_screen:
 * @tab: a #TerminalTab
 * @screen: a #TerminalScreen in @tab
 *
 * Closes the pane of @screen. Its space goes to the neighbouring pane.
 * The last pane of a tab cannot be removed; close the tab instead.
 */
void
terminal_tab_remove_screen(TerminalTab* tab,
                           TerminalScreen* screen)
{
  g_return_if_fail(TERMINAL_IS_TAB(tab));
  g_return_if_fail(TERMINAL_IS_SCREEN(screen));

  tab_remove_pane(tab, screen, false);
}

/**
 * terminal_tab_take_screen:
 * @tab: a #TerminalTab
 * @screen: a #TerminalScreen in @tab, which is split
 *
 * Takes the pane of @screen out of @tab, keeping the terminal and what
 * runs in it, for example to put it into a tab of its own.
 *
 * Returns: (transfer full): @screen, without a parent
 */
TerminalScreen*
terminal_tab_take_screen(TerminalTab* tab,
                         TerminalScreen* screen)
{
  g_return_val_if_fail(TERMINAL_IS_TAB(tab), nullptr);
  g_return_val_if_fail(TERMINAL_IS_SCREEN(screen), nullptr);

  return tab_remove_pane(tab, screen, true);
}

/* The number of panes side by side in @orientation within @widget */
static guint
count_panes_along(GtkWidget* widget,
                  GtkOrientation orientation)
{
  if (GTK_IS_PANED(widget) &&
      gtk_orientable_get_orientation(GTK_ORIENTABLE(widget)) == orientation)
    return count_panes_along(gtk_paned_get_start_child(GTK_PANED(widget)), orientation) +
      count_panes_along(gtk_paned_get_end_child(GTK_PANED(widget)), orientation);

  return 1;
}

static void
equalize_node(GtkWidget* widget)
{
  if (!GTK_IS_PANED(widget))
    return;

  auto const paned = GTK_PANED(widget);
  auto const orientation = gtk_orientable_get_orientation(GTK_ORIENTABLE(widget));
  auto const start = gtk_paned_get_start_child(paned);
  auto const end = gtk_paned_get_end_child(paned);
  auto const n_start = count_panes_along(start, orientation);
  auto const n_end = count_panes_along(end, orientation);

  paned_set_fraction(paned, double(n_start) / double(n_start + n_end), paned_get_size(paned));

  equalize_node(start);
  equalize_node(end);
}

/**
 * terminal_tab_equalize:
 * @tab: a #TerminalTab
 *
 * Gives the panes in each row and column of @tab the same size.
 */
void
terminal_tab_equalize(TerminalTab* tab)
{
  g_return_if_fail(TERMINAL_IS_TAB(tab));

  terminal_tab_set_zoomed(tab, false);
  equalize_node(tab->root);
}

/* The pane next to the active one in @direction. When several panes border
 * it, the most recently used one wins. */
static TerminalScreen*
tab_find_neighbor(TerminalTab* tab,
                  GtkDirectionType direction)
{
  if (!terminal_tab_is_split(tab) || tab->zoomed)
    return nullptr;

  auto const current = pane_from_screen(tab->active_screen);
  graphene_rect_t cr;
  if (!current ||
      !gtk_widget_compute_bounds(GTK_WIDGET(current), GTK_WIDGET(tab), &cr))
    return nullptr;

  auto const cx0 = cr.origin.x, cx1 = cr.origin.x + cr.size.width;
  auto const cy0 = cr.origin.y, cy1 = cr.origin.y + cr.size.height;
  auto constexpr slack = 2.0f; // the paned handle

  TerminalScreen* best = nullptr;
  auto best_distance = G_MAXFLOAT;

  for (auto l = tab->screens.head; l; l = l->next) {
    auto const screen = reinterpret_cast<TerminalScreen*>(l->data);
    auto const pane = pane_from_screen(screen);
    graphene_rect_t r;
    if (pane == current ||
        !gtk_widget_get_visible(GTK_WIDGET(pane)) ||
        !gtk_widget_compute_bounds(GTK_WIDGET(pane), GTK_WIDGET(tab), &r))
      continue;

    auto const x0 = r.origin.x, x1 = r.origin.x + r.size.width;
    auto const y0 = r.origin.y, y1 = r.origin.y + r.size.height;

    float distance, overlap;
    switch (direction) {
    case GTK_DIR_LEFT:
      distance = cx0 - x1;
      overlap = std::min(y1, cy1) - std::max(y0, cy0);
      break;
    case GTK_DIR_RIGHT:
      distance = x0 - cx1;
      overlap = std::min(y1, cy1) - std::max(y0, cy0);
      break;
    case GTK_DIR_UP:
      distance = cy0 - y1;
      overlap = std::min(x1, cx1) - std::max(x0, cx0);
      break;
    case GTK_DIR_DOWN:
      distance = y0 - cy1;
      overlap = std::min(x1, cx1) - std::max(x0, cx0);
      break;
    default:
      return nullptr;
    }

    if (distance < -slack || overlap <= 0)
      continue;

    // Strictly smaller, so that ties go to the most recently used pane
    if (distance < best_distance - slack) {
      best = screen;
      best_distance = distance;
    }
  }

  return best;
}

/**
 * terminal_tab_focus_pane:
 * @tab: a #TerminalTab
 * @direction: %GTK_DIR_LEFT, %GTK_DIR_RIGHT, %GTK_DIR_UP or %GTK_DIR_DOWN
 *
 * Moves the focus to the neighbouring pane in @direction. When several
 * panes border the active one, the most recently used one wins.
 *
 * Returns: %true if the focus was moved
 */
bool
terminal_tab_focus_pane(TerminalTab* tab,
                        GtkDirectionType direction)
{
  g_return_val_if_fail(TERMINAL_IS_TAB(tab), false);

  auto const best = tab_find_neighbor(tab, direction);
  if (!best)
    return false;

  tab_set_active_screen(tab, best);
  gtk_widget_grab_focus(GTK_WIDGET(best));
  return true;
}

/**
 * terminal_tab_has_neighbor:
 * @tab: a #TerminalTab
 * @direction: %GTK_DIR_LEFT, %GTK_DIR_RIGHT, %GTK_DIR_UP or %GTK_DIR_DOWN
 *
 * Returns: whether there is a pane next to the active one in @direction
 */
bool
terminal_tab_has_neighbor(TerminalTab* tab,
                          GtkDirectionType direction)
{
  g_return_val_if_fail(TERMINAL_IS_TAB(tab), false);

  return tab_find_neighbor(tab, direction) != nullptr;
}

/* Exchanges the places of two panes in the tree; the dividers stay */
static void
tab_swap_panes(TerminalTab* tab,
               GtkWidget* a,
               GtkWidget* b)
{
  auto const placeholder = gtk_label_new(nullptr);
  auto const old_a = tab_replace_child(tab, a, placeholder);
  auto const old_b = tab_replace_child(tab, b, a);
  auto const old_placeholder = tab_replace_child(tab, placeholder, b);
  g_object_unref(old_a);
  g_object_unref(old_b);
  g_object_unref(old_placeholder);
}

/**
 * terminal_tab_swap_screens:
 * @tab: a #TerminalTab
 * @screen: a #TerminalScreen in @tab
 * @other: another #TerminalScreen in @tab
 *
 * Exchanges the places of the panes of @screen and @other. The focus
 * stays with the active pane.
 */
void
terminal_tab_swap_screens(TerminalTab* tab,
                          TerminalScreen* screen,
                          TerminalScreen* other)
{
  g_return_if_fail(TERMINAL_IS_TAB(tab));

  auto const pane = pane_from_screen(screen);
  auto const other_pane = pane_from_screen(other);
  g_return_if_fail(pane && other_pane && pane != other_pane);
  g_return_if_fail(terminal_tab_get_from_screen(screen) == tab &&
                   terminal_tab_get_from_screen(other) == tab);

  terminal_tab_set_zoomed(tab, false);

  auto const focus = tab_release_focus(tab, tab->root);
  tab_swap_panes(tab, GTK_WIDGET(pane), GTK_WIDGET(other_pane));
  tab_restore_focus(focus);
  tab_update_panes(tab);
}

/**
 * terminal_tab_swap_pane:
 * @tab: a #TerminalTab
 * @direction: %GTK_DIR_LEFT, %GTK_DIR_RIGHT, %GTK_DIR_UP or %GTK_DIR_DOWN
 *
 * Exchanges the active pane with its neighbour in @direction, see
 * terminal_tab_focus_pane(). The active pane keeps the focus.
 *
 * Returns: %true if the panes were swapped
 */
bool
terminal_tab_swap_pane(TerminalTab* tab,
                       GtkDirectionType direction)
{
  g_return_val_if_fail(TERMINAL_IS_TAB(tab), false);

  auto const neighbor = tab_find_neighbor(tab, direction);
  if (!neighbor)
    return false;

  terminal_tab_swap_screens(tab, tab->active_screen, neighbor);
  return true;
}

/* Puts @widget, a pane or a tree of panes without a parent, next to the
 * pane of @target, on @side of it; the two share its space. */
static void
tab_insert_beside(TerminalTab* tab,
                  TerminalScreen* target,
                  GtkWidget* widget,
                  GtkPositionType side)
{
  auto const pane = pane_from_screen(target);
  g_return_if_fail(pane != nullptr);

  terminal_tab_set_zoomed(tab, false);

  auto const orientation = side == GTK_POS_LEFT || side == GTK_POS_RIGHT
    ? GTK_ORIENTATION_HORIZONTAL : GTK_ORIENTATION_VERTICAL;
  auto const size = orientation == GTK_ORIENTATION_HORIZONTAL
    ? gtk_widget_get_width(GTK_WIDGET(pane))
    : gtk_widget_get_height(GTK_WIDGET(pane));

  auto const paned = GTK_PANED(gtk_paned_new(orientation));
  gtk_paned_set_resize_start_child(paned, true);
  gtk_paned_set_resize_end_child(paned, true);
  gtk_paned_set_shrink_start_child(paned, false);
  gtk_paned_set_shrink_end_child(paned, false);

  auto const focus = tab_release_focus(tab, GTK_WIDGET(pane));
  auto const old_pane = tab_replace_child(tab, GTK_WIDGET(pane), GTK_WIDGET(paned));
  auto const before = side == GTK_POS_LEFT || side == GTK_POS_TOP;
  gtk_paned_set_start_child(paned, before ? widget : old_pane);
  gtk_paned_set_end_child(paned, before ? old_pane : widget);
  g_object_unref(old_pane);
  tab_restore_focus(focus);

  paned_set_fraction(paned, 0.5, size);
}

/* Shows or hides the terminals of the panes in @widget with the tab, see
 * terminal_tab_set_active() */
static void
tab_sync_pane_visibility(TerminalTab* tab,
                         GtkWidget* widget)
{
  GList* screens = nullptr;
  collect_screens(widget, &screens);
  for (auto l = screens; l; l = l->next) {
    if (auto const pane = pane_from_screen(reinterpret_cast<TerminalScreen*>(l->data)))
      gtk_widget_set_visible(pane->scrolled_window, tab->active);
  }
  g_list_free(screens);
}

/**
 * terminal_tab_insert_screen:
 * @tab: a #TerminalTab
 * @target: a #TerminalScreen in @tab
 * @screen: a #TerminalScreen without a parent, for example from
 *   terminal_tab_take_screen()
 * @side: where to put @screen
 *
 * Splits the pane of @target and puts @screen on @side of it.
 */
void
terminal_tab_insert_screen(TerminalTab* tab,
                           TerminalScreen* target,
                           TerminalScreen* screen,
                           GtkPositionType side)
{
  g_return_if_fail(TERMINAL_IS_TAB(tab));
  g_return_if_fail(TERMINAL_IS_SCREEN(screen));
  g_return_if_fail(gtk_widget_get_parent(GTK_WIDGET(screen)) == nullptr);
  g_return_if_fail(terminal_tab_get_from_screen(target) == tab);

  auto const pane = terminal_pane_new(screen, tab->active);
  tab_insert_beside(tab, target, GTK_WIDGET(pane), side);

  tab_track_screen(tab, screen);
  tab_update_panes(tab);
  g_signal_emit(tab, signals[SCREEN_ADDED], 0, screen);
}

/**
 * terminal_tab_pick_drop:
 * @tab: a #TerminalTab
 * @widget: the widget of @x, @y
 * @where: (out): where a dropped tab would go
 *
 * For drops that the panes do not get themselves: libadwaita keeps a tab
 * dragged from a tab bar away from the content of the tab view.
 *
 * Returns: (nullable): the screen of the pane at @x, @y
 */
TerminalScreen*
terminal_tab_pick_drop(TerminalTab* tab,
                       GtkWidget* widget,
                       double x,
                       double y,
                       TerminalPaneDrop* where)
{
  g_return_val_if_fail(TERMINAL_IS_TAB(tab), nullptr);

  auto const widget_point = GRAPHENE_POINT_INIT(float(x), float(y));
  graphene_point_t point;
  if (!gtk_widget_compute_point(widget, GTK_WIDGET(tab), &widget_point, &point))
    return nullptr;

  // The children do not take pointer input during the drag
  auto const picked = gtk_widget_pick(GTK_WIDGET(tab), point.x, point.y,
                                      GTK_PICK_NON_TARGETABLE);
  auto const pane = picked
    ? TERMINAL_PANE(gtk_widget_get_ancestor(picked, TERMINAL_TYPE_PANE))
    : nullptr;
  if (!pane || !pane->screen)
    return nullptr;

  graphene_point_t pane_point;
  if (!gtk_widget_compute_point(GTK_WIDGET(tab), GTK_WIDGET(pane), &point, &pane_point))
    return nullptr;

  auto const zone = pane_drop_zone(pane, nullptr, pane_point.x, pane_point.y);
  if (zone < 0)
    return nullptr;

  *where = TerminalPaneDrop(zone);
  return pane->screen;
}

/**
 * terminal_tab_show_drop_indicator:
 * @screen: a #TerminalScreen
 * @where: where a drop would go, or -1 to hide the indicator
 */
void
terminal_tab_show_drop_indicator(TerminalScreen* screen,
                                 int where)
{
  if (auto const pane = pane_from_screen(screen))
    pane_show_drop_indicator(pane, where);
}

/**
 * terminal_tab_merge:
 * @tab: a #TerminalTab
 * @target: a #TerminalScreen in @tab
 * @other: another #TerminalTab
 *
 * Moves all panes of @other, as they are arranged, next to the pane of
 * @target, on @side of it. @other is left without panes, to be closed.
 */
void
terminal_tab_merge(TerminalTab* tab,
                   TerminalScreen* target,
                   TerminalTab* other,
                   GtkPositionType side)
{
  g_return_if_fail(TERMINAL_IS_TAB(tab));
  g_return_if_fail(TERMINAL_IS_TAB(other));
  g_return_if_fail(tab != other && other->root != nullptr);
  g_return_if_fail(terminal_tab_get_from_screen(target) == tab);

  // The window of @other may close when its last screen goes
  g_object_ref(other);

  auto const screens = terminal_tab_list_screens(other);
  terminal_tab_set_zoomed(other, false);
  for (auto l = screens; l; l = l->next)
    tab_untrack_screen(other, reinterpret_cast<TerminalScreen*>(l->data));
  other->active_screen = nullptr;
  g_binding_group_set_source(other->bindings, nullptr);

  // The caller moves the focus to the moved panes
  if (auto const focus = tab_release_focus(other, other->root))
    g_object_unref(focus);

  auto const root = GTK_WIDGET(g_object_ref(other->root));
  gtk_widget_unparent(root);
  other->root = nullptr;

  // Announce the screens as gone only now that @other has none left
  for (auto l = screens; l; l = l->next)
    g_signal_emit(other, signals[SCREEN_REMOVED], 0, l->data);
  g_object_unref(other);

  tab_insert_beside(tab, target, root, side);
  g_object_unref(root);
  tab_sync_pane_visibility(tab, root);

  for (auto l = screens; l; l = l->next) {
    tab_track_screen(tab, reinterpret_cast<TerminalScreen*>(l->data));
    g_signal_emit(tab, signals[SCREEN_ADDED], 0, l->data);
  }
  tab_update_panes(tab);
  g_list_free(screens);
}

/**
 * terminal_tab_resize_pane:
 * @tab: a #TerminalTab
 * @direction: %GTK_DIR_LEFT, %GTK_DIR_RIGHT, %GTK_DIR_UP or %GTK_DIR_DOWN
 *
 * Moves the nearest divider of the active pane in @direction by a few
 * character cells.
 *
 * Returns: %true if a divider was moved
 */
bool
terminal_tab_resize_pane(TerminalTab* tab,
                         GtkDirectionType direction)
{
  g_return_val_if_fail(TERMINAL_IS_TAB(tab), false);

  if (!terminal_tab_is_split(tab) || tab->zoomed)
    return false;

  GtkOrientation orientation;
  int sign;
  switch (direction) {
  case GTK_DIR_LEFT:  orientation = GTK_ORIENTATION_HORIZONTAL; sign = -1; break;
  case GTK_DIR_RIGHT: orientation = GTK_ORIENTATION_HORIZONTAL; sign = 1;  break;
  case GTK_DIR_UP:    orientation = GTK_ORIENTATION_VERTICAL;   sign = -1; break;
  case GTK_DIR_DOWN:  orientation = GTK_ORIENTATION_VERTICAL;   sign = 1;  break;
  default: return false;
  }

  GtkPaned* paned = nullptr;
  for (auto widget = GTK_WIDGET(pane_from_screen(tab->active_screen));
       widget && widget != GTK_WIDGET(tab);
       widget = gtk_widget_get_parent(widget)) {
    auto const parent = gtk_widget_get_parent(widget);
    if (GTK_IS_PANED(parent) &&
        gtk_orientable_get_orientation(GTK_ORIENTABLE(parent)) == orientation) {
      paned = GTK_PANED(parent);
      break;
    }
  }

  if (!paned)
    return false;

  int cell_width = 0, cell_height = 0;
  terminal_screen_get_cell_size(tab->active_screen, &cell_width, &cell_height);
  auto step = orientation == GTK_ORIENTATION_HORIZONTAL ? 2 * cell_width : cell_height;
  if (step <= 0)
    step = 16;

  int min_position = 0, max_position = 0;
  g_object_get(paned,
               "min-position", &min_position,
               "max-position", &max_position,
               nullptr);

  auto const position = std::clamp(gtk_paned_get_position(paned) + sign * step,
                                   min_position, max_position);
  gtk_paned_set_position(paned, position);
  return true;
}

/**
 * terminal_tab_set_zoomed:
 * @tab: a #TerminalTab
 * @zoomed: whether to zoom
 *
 * Shows only the active pane, using the space of the whole tab, or shows
 * all panes again.
 */
void
terminal_tab_set_zoomed(TerminalTab* tab,
                        bool zoomed)
{
  g_return_if_fail(TERMINAL_IS_TAB(tab));

  if (zoomed && !terminal_tab_is_split(tab))
    zoomed = false;

  if (tab->zoomed == zoomed)
    return;

  tab->zoomed = zoomed;

  if (zoomed) {
    // Hide the other side of every divider above the active pane
    for (auto widget = GTK_WIDGET(pane_from_screen(tab->active_screen));
         widget && widget != tab->root;
         widget = gtk_widget_get_parent(widget)) {
      auto const paned = GTK_PANED(gtk_widget_get_parent(widget));
      auto const other = gtk_paned_get_start_child(paned) == widget
        ? gtk_paned_get_end_child(paned)
        : gtk_paned_get_start_child(paned);
      gtk_widget_set_visible(other, false);
    }
    gtk_widget_add_css_class(GTK_WIDGET(tab), "zoomed");
  } else {
    show_all_panes(tab->root);
    gtk_widget_remove_css_class(GTK_WIDGET(tab), "zoomed");
  }

  g_object_notify_by_pspec(G_OBJECT(tab), pspecs[PROP_ZOOMED]);
}

bool
terminal_tab_get_zoomed(TerminalTab* tab)
{
  g_return_val_if_fail(TERMINAL_IS_TAB(tab), false);

  return tab->zoomed;
}

/**
 * terminal_tab_get_from_screen:
 * @screen: a #TerminalScreen
 *
 * Returns the #TerminalTab containing @screen.
 */
TerminalTab *
terminal_tab_get_from_screen (TerminalScreen *screen)
{
  if (screen == nullptr)
    return nullptr;

  g_return_val_if_fail (TERMINAL_IS_SCREEN (screen), nullptr);

  return TERMINAL_TAB (gtk_widget_get_ancestor (GTK_WIDGET (screen), TERMINAL_TYPE_TAB));
}

/**
 * terminal_tab_set_screen_policy:
 * @screen: a #TerminalScreen
 * @hpolicy: a #TerminalScrollbarPolicy (ignored; there is no horizontal
 *   scrolling)
 * @vpolicy: a #TerminalScrollbarPolicy
 *
 * Sets the scrollbar policy of the pane of @screen.
 */
void
terminal_tab_set_screen_policy(TerminalScreen* screen,
                               TerminalScrollbarPolicy hpolicy,
                               TerminalScrollbarPolicy vpolicy)
{
  g_return_if_fail(TERMINAL_IS_SCREEN(screen));

  if (auto const pane = pane_from_screen(screen))
    terminal_pane_set_policy(pane, vpolicy);
}

void
terminal_tab_set_screen_kinetic_scrolling(TerminalScreen* screen,
                                          bool enable)
{
  g_return_if_fail(TERMINAL_IS_SCREEN(screen));

  if (auto const pane = pane_from_screen(screen))
    gtk_scrolled_window_set_kinetic_scrolling(GTK_SCROLLED_WINDOW(pane->scrolled_window),
                                              enable);
}

/**
 * terminal_tab_add_screen_overlay:
 * @screen: a #TerminalScreen
 * @child: a widget, for example an info bar
 *
 * Shows @child on top of the pane of @screen.
 */
void
terminal_tab_add_screen_overlay(TerminalScreen* screen,
                                GtkWidget* child)
{
  g_return_if_fail(TERMINAL_IS_SCREEN(screen));

  if (auto const pane = pane_from_screen(screen))
    gtk_overlay_add_overlay(GTK_OVERLAY(pane->overlay), child);
}

/**
 * terminal_tab_set_screen_css_class:
 * @screen: a #TerminalScreen
 * @css_class: a style class
 * @set: whether to add or remove @css_class
 *
 * Adds a style class to the pane of @screen, for example to draw a frame.
 */
void
terminal_tab_set_screen_css_class(TerminalScreen* screen,
                                  char const* css_class,
                                  bool set)
{
  g_return_if_fail(TERMINAL_IS_SCREEN(screen));

  if (auto const pane = pane_from_screen(screen)) {
    if (set)
      gtk_widget_add_css_class(GTK_WIDGET(pane), css_class);
    else
      gtk_widget_remove_css_class(GTK_WIDGET(pane), css_class);
  }
}

void
terminal_tab_remove_overlay(GtkWidget* child)
{
  g_return_if_fail(GTK_IS_WIDGET(child));

  auto const parent = gtk_widget_get_parent(child);
  if (GTK_IS_OVERLAY(parent))
    gtk_overlay_remove_overlay(GTK_OVERLAY(parent), child);
}

void
terminal_tab_set_pinned(TerminalTab* tab,
                        bool pinned)
{
  g_return_if_fail(TERMINAL_IS_TAB(tab));

  tab->pinned = pinned;
}

bool
terminal_tab_get_pinned(TerminalTab* tab)
{
  g_return_val_if_fail(TERMINAL_IS_TAB(tab), false);

  return tab->pinned;
}

/**
 * terminal_tab_set_active:
 * @tab: a #TerminalTab
 * @active: whether @tab is the selected tab
 *
 * Hides the terminals of tabs that are not selected, so that they do not
 * take part in the size calculation of the window.
 */
void
terminal_tab_set_active(TerminalTab* tab,
                        bool active)
{
  g_return_if_fail(TERMINAL_IS_TAB(tab));

  tab->active = active;

  auto const screens = terminal_tab_list_screens(tab);
  for (auto l = screens; l; l = l->next) {
    if (auto const pane = pane_from_screen(reinterpret_cast<TerminalScreen*>(l->data)))
      gtk_widget_set_visible(pane->scrolled_window, active);
  }
  g_list_free(screens);
  tab_update_panes(tab);
}

/* Layouts */

#define LAYOUT_SPLIT "split"

static GVariant*
save_node(GtkWidget* widget,
          TerminalTabSaveFunc save_func,
          void* user_data)
{
  if (TERMINAL_IS_PANE(widget))
    return save_func(TERMINAL_PANE(widget)->screen, user_data);

  if (!GTK_IS_PANED(widget))
    return nullptr;

  auto const paned = GTK_PANED(widget);
  auto const start = save_node(gtk_paned_get_start_child(paned), save_func, user_data);
  auto const end = save_node(gtk_paned_get_end_child(paned), save_func, user_data);
  if (!start || !end)
    return start ? start : end;

  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&builder, "{sv}", LAYOUT_SPLIT,
                        g_variant_new_string(gtk_orientable_get_orientation(GTK_ORIENTABLE(paned)) == GTK_ORIENTATION_HORIZONTAL
                                             ? "horizontal" : "vertical"));
  g_variant_builder_add(&builder, "{sv}", "position",
                        g_variant_new_double(paned_get_fraction(paned)));
  g_variant_builder_add(&builder, "{sv}", "start", start);
  g_variant_builder_add(&builder, "{sv}", "end", end);
  return g_variant_builder_end(&builder);
}

/**
 * terminal_tab_save_layout:
 * @tab: a #TerminalTab
 * @save_func: returns an a{sv} dictionary for a screen
 * @user_data: data for @save_func
 *
 * Returns: (transfer floating): the pane layout of @tab
 */
GVariant*
terminal_tab_save_layout(TerminalTab* tab,
                         TerminalTabSaveFunc save_func,
                         void* user_data)
{
  g_return_val_if_fail(TERMINAL_IS_TAB(tab), nullptr);

  return save_node(tab->root, save_func, user_data);
}

static bool
layout_is_split(GVariant* node)
{
  return g_variant_lookup(node, LAYOUT_SPLIT, "&s", nullptr);
}

static GVariant*
layout_child(GVariant* node,
             char const* key)
{
  return g_variant_lookup_value(node, key, G_VARIANT_TYPE_VARDICT);
}

/* Returns: (transfer full): the first pane of @node */
static GVariant*
layout_first_pane(GVariant* node)
{
  auto pane = g_variant_ref(node);
  while (layout_is_split(pane)) {
    auto const start = layout_child(pane, "start");
    g_variant_unref(pane);
    if (!start)
      return nullptr;
    pane = start;
  }
  return pane;
}

static void
restore_node(TerminalTab* tab,
             TerminalScreen* anchor,
             GVariant* node,
             TerminalTabCreateFunc create_func,
             void* user_data)
{
  char const* orientation;
  if (!g_variant_lookup(node, LAYOUT_SPLIT, "&s", &orientation))
    return; // a pane; @anchor is its screen

  gs_unref_variant GVariant* start = layout_child(node, "start");
  gs_unref_variant GVariant* end = layout_child(node, "end");
  if (!start)
    return;

  gs_unref_variant GVariant* end_pane = end ? layout_first_pane(end) : nullptr;
  auto const new_screen = end_pane ? create_func(end_pane, user_data) : nullptr;
  if (!new_screen) {
    restore_node(tab, anchor, start, create_func, user_data);
    return;
  }

  double position;
  if (!g_variant_lookup(node, "position", "d", &position))
    position = 0.5;

  terminal_tab_split(tab, anchor, new_screen,
                     g_str_equal(orientation, "vertical") ? GTK_ORIENTATION_VERTICAL
                                                          : GTK_ORIENTATION_HORIZONTAL,
                     position);

  restore_node(tab, anchor, start, create_func, user_data);
  restore_node(tab, new_screen, end, create_func, user_data);
}

/**
 * terminal_tab_new_from_layout:
 * @layout: a layout from terminal_tab_save_layout()
 * @create_func: creates a new screen for a pane dictionary
 * @user_data: data for @create_func
 *
 * Returns: (transfer floating) (nullable): a new #TerminalTab with the
 *   panes of @layout
 */
TerminalTab*
terminal_tab_new_from_layout(GVariant* layout,
                             TerminalTabCreateFunc create_func,
                             void* user_data)
{
  g_return_val_if_fail(layout != nullptr, nullptr);
  g_return_val_if_fail(g_variant_is_of_type(layout, G_VARIANT_TYPE_VARDICT), nullptr);

  gs_unref_variant GVariant* first = layout_first_pane(layout);
  auto const screen = first ? create_func(first, user_data) : nullptr;
  if (!screen)
    return nullptr;

  auto const tab = terminal_tab_new(screen);
  restore_node(tab, screen, layout, create_func, user_data);
  return tab;
}
