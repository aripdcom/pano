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

/*
 * TerminalTabSidebar: a vertical list of the pages of an AdwTabView,
 * used when the tabs are placed at the left or right of the window.
 * AdwTabBar only supports a horizontal layout.
 */

#include "config.h"

#include "terminal-tab-sidebar.hh"
#include "terminal-icon-button.hh"
#include "terminal-intl.hh"

#include <glib/gi18n.h>
#include "terminal-libgsystem.hh"
#include "terminal-tab.hh"

#define DEFAULT_WIDTH 200
#define MIN_WIDTH 120
#define MAX_WIDTH 600
#define HANDLE_WIDTH 6

#define ROW_DATA_KEY "terminal-tab-sidebar-row"

struct _TerminalTabSidebar
{
  GtkWidget parent_instance;

  AdwTabView* view; // owned
  GtkWidget* scrolled_window;
  GtkWidget* list_view;
  GtkWidget* handle; // for resizing
  GtkPopoverMenu* context_menu;
  GPtrArray* rows; // the row widgets, unowned

  guint reset_menu_source;
  gboolean autohide;
  GtkPositionType position;
  int width;
  int drag_start_width;
  double drag_start_x; // in the window
};

enum {
  PROP_0,
  PROP_VIEW,
  PROP_AUTOHIDE,
  PROP_WIDTH,
  N_PROPS
};

static GParamSpec* pspecs[N_PROPS];

enum {
  PAGE_MIDDLE_CLICKED,
  SCREEN_DROPPED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(TerminalTabSidebar, terminal_tab_sidebar, GTK_TYPE_WIDGET)

/* Per-row state, attached to the row widget */

typedef struct {
  TerminalTabSidebar* sidebar; // unowned
  AdwTabPage* page; // unowned, only set while the row is bound
  GtkWidget* icon;
  GtkWidget* label;
  GtkWidget* indicator;
  GtkWidget* pin_icon;
  GtkWidget* close_button;
  GPtrArray* bindings;
  gulong attention_handler;
  gulong pinned_handler;
} RowData;

static void
binding_free(void* data)
{
  auto const binding = G_BINDING(data);
  g_binding_unbind(binding);
  g_object_unref(binding);
}

static void
row_data_free(void* user_data)
{
  auto const data = reinterpret_cast<RowData*>(user_data);

  if (data->page) {
    g_clear_signal_handler(&data->attention_handler, data->page);
    g_clear_signal_handler(&data->pinned_handler, data->page);
  }
  g_ptr_array_unref(data->bindings);
  g_free(data);
}

static inline RowData*
row_data_get(GtkWidget* row)
{
  return reinterpret_cast<RowData*>(g_object_get_data(G_OBJECT(row), ROW_DATA_KEY));
}

static void
row_add_binding(RowData* data,
                GBinding* binding)
{
  g_ptr_array_add(data->bindings, g_object_ref(binding));
}

static gboolean
object_to_visible(GBinding* binding,
                  GValue const* from_value,
                  GValue* to_value,
                  void* user_data)
{
  g_value_set_boolean(to_value, g_value_get_object(from_value) != nullptr);
  return TRUE;
}

/* Context menu */

static gboolean
reset_setup_menu_cb(void* user_data)
{
  auto const sidebar = TERMINAL_TAB_SIDEBAR(user_data);

  sidebar->reset_menu_source = 0;

  if (sidebar->view)
    g_signal_emit_by_name(sidebar->view, "setup-menu", (AdwTabPage*)nullptr);

  return G_SOURCE_REMOVE;
}

static void
context_menu_closed_cb(GtkPopover* popover,
                       TerminalTabSidebar* sidebar)
{
  // The menu item's action is activated only after the popover
  // has closed, so defer resetting the context page until then.
  g_clear_handle_id(&sidebar->reset_menu_source, g_source_remove);
  sidebar->reset_menu_source = g_idle_add(reset_setup_menu_cb, sidebar);
}

static void
show_context_menu(TerminalTabSidebar* sidebar,
                  AdwTabPage* page,
                  GtkWidget* widget,
                  double x,
                  double y)
{
  if (!sidebar->view)
    return;

  auto const menu_model = adw_tab_view_get_menu_model(sidebar->view);
  if (!menu_model)
    return;

  g_clear_handle_id(&sidebar->reset_menu_source, g_source_remove);
  g_signal_emit_by_name(sidebar->view, "setup-menu", page);

  if (!sidebar->context_menu) {
    sidebar->context_menu = GTK_POPOVER_MENU(gtk_popover_menu_new_from_model(menu_model));
    gtk_widget_set_parent(GTK_WIDGET(sidebar->context_menu), GTK_WIDGET(sidebar));
    gtk_popover_set_has_arrow(GTK_POPOVER(sidebar->context_menu), false);
    gtk_popover_set_position(GTK_POPOVER(sidebar->context_menu), GTK_POS_BOTTOM);
    gtk_widget_set_halign(GTK_WIDGET(sidebar->context_menu), GTK_ALIGN_START);
    g_signal_connect(sidebar->context_menu, "closed",
                     G_CALLBACK(context_menu_closed_cb), sidebar);
  } else {
    gtk_popover_menu_set_menu_model(sidebar->context_menu, menu_model);
  }

  graphene_point_t point = {float(x), float(y)};
  if (!gtk_widget_compute_point(widget, GTK_WIDGET(sidebar), &point, &point))
    point = {float(x), float(y)};

  GdkRectangle const rect = {int(point.x), int(point.y), 1, 1};
  gtk_popover_set_pointing_to(GTK_POPOVER(sidebar->context_menu), &rect);
  gtk_popover_popup(GTK_POPOVER(sidebar->context_menu));
}

/* Rows */

static void
row_update_attention(GtkWidget* row,
                     AdwTabPage* page)
{
  if (page && adw_tab_page_get_needs_attention(page))
    gtk_widget_add_css_class(row, "needs-attention");
  else
    gtk_widget_remove_css_class(row, "needs-attention");
}

static void
row_attention_notify_cb(AdwTabPage* page,
                        GParamSpec* pspec,
                        GtkWidget* row)
{
  row_update_attention(row, page);
}

static void
row_close_clicked_cb(GtkButton* button,
                     RowData* data)
{
  if (data->page && data->sidebar->view)
    adw_tab_view_close_page(data->sidebar->view, data->page);
}

static void
row_middle_click_pressed_cb(GtkGestureClick* gesture,
                            int n_press,
                            double x,
                            double y,
                            RowData* data)
{
  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void
row_middle_click_released_cb(GtkGestureClick* gesture,
                             int n_press,
                             double x,
                             double y,
                             RowData* data)
{
  if (!data->page)
    return;

  // Keep the page alive while handlers may close it
  gs_unref_object auto page = ADW_TAB_PAGE(g_object_ref(data->page));
  g_signal_emit(data->sidebar, signals[PAGE_MIDDLE_CLICKED], 0, page);
}

static void
row_click_pressed_cb(GtkGestureClick* gesture,
                     int n_press,
                     double x,
                     double y,
                     RowData* data)
{
  if (!data->page)
    return;

  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);

  auto const widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  show_context_menu(data->sidebar, data->page, widget, x, y);
}

/* Pinned tabs come first; mark them, and draw a line below the last one */
static void
terminal_tab_sidebar_update_pinned_rows(TerminalTabSidebar* sidebar)
{
  if (!sidebar->view)
    return;

  auto const n_pinned = adw_tab_view_get_n_pinned_pages(sidebar->view);
  for (auto i = 0u; i < sidebar->rows->len; ++i) {
    auto const row = GTK_WIDGET(g_ptr_array_index(sidebar->rows, i));
    auto const data = row_data_get(row);
    auto const pinned = data->page && adw_tab_page_get_pinned(data->page);
    auto const last = pinned &&
      adw_tab_view_get_page_position(sidebar->view, data->page) == n_pinned - 1;

    gtk_widget_set_visible(data->pin_icon, pinned);

    // On the list row around our widget, so that the line spans the row
    if (auto const list_row = gtk_widget_get_parent(row)) {
      if (last)
        gtk_widget_add_css_class(list_row, "last-pinned");
      else
        gtk_widget_remove_css_class(list_row, "last-pinned");
    }
  }
}

/* Drag and drop */

/* What the rows and the list take: tabs, and panes to make tabs of */
static void
set_drop_types(GtkDropTarget* target)
{
  GType types[] = { ADW_TYPE_TAB_PAGE, TERMINAL_TYPE_SCREEN };
  gtk_drop_target_set_gtypes(target, types, G_N_ELEMENTS(types));
}

static void
row_clear_drop_marks(GtkWidget* row)
{
  gtk_widget_remove_css_class(row, "drop-before");
  gtk_widget_remove_css_class(row, "drop-after");
}

static GdkContentProvider*
row_drag_prepare_cb(GtkDragSource* source,
                    double x,
                    double y,
                    RowData* data)
{
  if (!data->page)
    return nullptr;

  return gdk_content_provider_new_typed(ADW_TYPE_TAB_PAGE, data->page);
}

static void
row_drag_begin_cb(GtkDragSource* source,
                  GdkDrag* drag,
                  RowData* data)
{
  auto const row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(source));
  gs_unref_object auto paintable = gtk_widget_paintable_new(row);
  gtk_drag_source_set_icon(source, paintable, 0, 0);
}

/* A tab dropped where nothing takes it gets its own window, like with
 * AdwTabBar. */
static gboolean
row_drag_cancel_cb(GtkDragSource* source,
                   GdkDrag* drag,
                   GdkDragCancelReason reason,
                   RowData* data)
{
  auto const view = data->sidebar->view;
  if (reason != GDK_DRAG_CANCEL_NO_TARGET || !data->page || !view ||
      adw_tab_view_get_n_pages(view) < 2)
    return false;

  AdwTabView* new_view = nullptr;
  g_signal_emit_by_name(view, "create-window", &new_view);
  if (!new_view)
    return false;

  adw_tab_view_transfer_page(view, data->page, new_view, 0);
  return true;
}

/* The AdwTabView a page is in: the page's child is inside it */
static AdwTabView*
page_get_view(AdwTabPage* page)
{
  auto const child = adw_tab_page_get_child(page);
  return child ? ADW_TAB_VIEW(gtk_widget_get_ancestor(child, ADW_TYPE_TAB_VIEW)) : nullptr;
}

/* Where a drop at @y on @row puts the tab: before or after it */
static int
row_drop_position(RowData* data,
                  double y)
{
  auto const row = gtk_widget_get_parent(data->label);
  auto const position = adw_tab_view_get_page_position(data->sidebar->view, data->page);
  return y < gtk_widget_get_height(row) / 2.0 ? position : position + 1;
}

static GdkDragAction
row_drop_motion_cb(GtkDropTarget* target,
                   double x,
                   double y,
                   RowData* data)
{
  auto const row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
  if (!data->page)
    return GdkDragAction(0);

  auto const before = y < gtk_widget_get_height(row) / 2.0;
  gtk_widget_add_css_class(row, before ? "drop-before" : "drop-after");
  gtk_widget_remove_css_class(row, before ? "drop-after" : "drop-before");
  return GDK_ACTION_MOVE;
}

static void
row_drop_leave_cb(GtkDropTarget* target,
                  RowData* data)
{
  row_clear_drop_marks(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target)));
}

/* Moves @page to @position in the sidebar's view, from this or another
 * window. Pinned and other tabs each keep to their own part. */
static bool
terminal_tab_sidebar_move_page(TerminalTabSidebar* sidebar,
                               AdwTabPage* page,
                               int position)
{
  auto const view = sidebar->view;
  auto const source_view = page_get_view(page);
  if (!view || !source_view)
    return false;

  auto const n_pinned = adw_tab_view_get_n_pinned_pages(view);
  auto const n_pages = adw_tab_view_get_n_pages(view);
  auto const pinned = adw_tab_page_get_pinned(page);

  if (source_view == view) {
    // Moving down, the page leaves its old place first
    auto const old_position = adw_tab_view_get_page_position(view, page);
    if (position > old_position)
      --position;
    position = pinned ? CLAMP(position, 0, n_pinned - 1)
                      : CLAMP(position, n_pinned, n_pages - 1);
    return adw_tab_view_reorder_page(view, page, position);
  }

  position = pinned ? CLAMP(position, 0, n_pinned)
                    : CLAMP(position, n_pinned, n_pages);
  adw_tab_view_transfer_page(source_view, page, view, position);
  return true;
}

static gboolean
row_drop_cb(GtkDropTarget* target,
            GValue const* value,
            double x,
            double y,
            RowData* data)
{
  row_clear_drop_marks(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target)));

  if (!data->page || !data->sidebar->view)
    return false;

  // A pane, which becomes a tab here
  auto const position = row_drop_position(data, y);
  if (G_VALUE_HOLDS(value, TERMINAL_TYPE_SCREEN)) {
    gboolean handled = false;
    g_signal_emit(data->sidebar, signals[SCREEN_DROPPED], 0,
                  g_value_get_object(value), position, &handled);
    return handled;
  }

  auto const page = ADW_TAB_PAGE(g_value_get_object(value));
  if (!page || page == data->page)
    return false;

  return terminal_tab_sidebar_move_page(data->sidebar, page, position);
}

/* Dropping below the last row moves the tab to the end */
static gboolean
list_drop_cb(GtkDropTarget* target,
             GValue const* value,
             double x,
             double y,
             TerminalTabSidebar* sidebar)
{
  if (!sidebar->view)
    return false;

  auto const position = adw_tab_view_get_n_pages(sidebar->view);
  if (G_VALUE_HOLDS(value, TERMINAL_TYPE_SCREEN)) {
    gboolean handled = false;
    g_signal_emit(sidebar, signals[SCREEN_DROPPED], 0,
                  g_value_get_object(value), position, &handled);
    return handled;
  }

  auto const page = ADW_TAB_PAGE(g_value_get_object(value));
  if (!page)
    return false;

  return terminal_tab_sidebar_move_page(sidebar, page, position);
}

/* The list view destroys rows on its own; forget them then */
static void
row_finalized_cb(void* user_data,
                 GObject* row)
{
  auto const sidebar = TERMINAL_TAB_SIDEBAR(user_data);
  g_ptr_array_remove(sidebar->rows, row);
}

static void
factory_setup_cb(GtkSignalListItemFactory* factory,
                 GObject* object,
                 TerminalTabSidebar* sidebar)
{
  auto const item = GTK_LIST_ITEM(object);

  auto const data = g_new0(RowData, 1);
  data->sidebar = sidebar;
  data->bindings = g_ptr_array_new_with_free_func(binding_free);

  auto const row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(row, "tab-row");

  data->icon = gtk_image_new();
  gtk_widget_add_css_class(data->icon, "tab-icon");
  gtk_box_append(GTK_BOX(row), data->icon);

  data->label = gtk_label_new(nullptr);
  gtk_label_set_ellipsize(GTK_LABEL(data->label), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(data->label), 0.0f);
  gtk_widget_set_hexpand(data->label, true);
  gtk_box_append(GTK_BOX(row), data->label);

  data->indicator = gtk_image_new();
  gtk_widget_add_css_class(data->indicator, "tab-indicator");
  gtk_box_append(GTK_BOX(row), data->indicator);

  data->pin_icon = gtk_image_new_from_icon_name("view-pin-symbolic");
  gtk_widget_add_css_class(data->pin_icon, "pin-icon");
  gtk_widget_set_tooltip_text(data->pin_icon, _("Pinned"));
  gtk_widget_set_visible(data->pin_icon, false);
  gtk_box_append(GTK_BOX(row), data->pin_icon);

  data->close_button = terminal_close_button_new();
  gtk_widget_add_css_class(data->close_button, "close-button");
  gtk_widget_set_valign(data->close_button, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(row), data->close_button);
  g_signal_connect(data->close_button, "clicked",
                   G_CALLBACK(row_close_clicked_cb), data);

  auto const gesture = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture), GDK_BUTTON_SECONDARY);
  g_signal_connect(gesture, "pressed",
                   G_CALLBACK(row_click_pressed_cb), data);
  gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(gesture)); // adopts

  auto const middle_click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(middle_click), GDK_BUTTON_MIDDLE);
  g_signal_connect(middle_click, "pressed",
                   G_CALLBACK(row_middle_click_pressed_cb), data);
  g_signal_connect(middle_click, "released",
                   G_CALLBACK(row_middle_click_released_cb), data);
  gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(middle_click)); // adopts

  auto const drag_source = gtk_drag_source_new();
  gtk_drag_source_set_actions(drag_source, GDK_ACTION_MOVE);
  g_signal_connect(drag_source, "prepare", G_CALLBACK(row_drag_prepare_cb), data);
  g_signal_connect(drag_source, "drag-begin", G_CALLBACK(row_drag_begin_cb), data);
  g_signal_connect(drag_source, "drag-cancel", G_CALLBACK(row_drag_cancel_cb), data);
  gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(drag_source));

  auto const drop_target = gtk_drop_target_new(G_TYPE_INVALID, GDK_ACTION_MOVE);
  set_drop_types(drop_target);
  g_signal_connect(drop_target, "motion", G_CALLBACK(row_drop_motion_cb), data);
  g_signal_connect(drop_target, "enter", G_CALLBACK(row_drop_motion_cb), data);
  g_signal_connect(drop_target, "leave", G_CALLBACK(row_drop_leave_cb), data);
  g_signal_connect(drop_target, "drop", G_CALLBACK(row_drop_cb), data);
  gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(drop_target));

  g_object_set_data_full(G_OBJECT(row), ROW_DATA_KEY, data, row_data_free);
  gtk_list_item_set_child(item, row);
  g_ptr_array_add(sidebar->rows, row);
  g_object_weak_ref(G_OBJECT(row), row_finalized_cb, sidebar);
}

static void
factory_bind_cb(GtkSignalListItemFactory* factory,
                GObject* object,
                TerminalTabSidebar* sidebar)
{
  auto const item = GTK_LIST_ITEM(object);
  auto const row = gtk_list_item_get_child(item);
  auto const data = row_data_get(row);
  auto const page = ADW_TAB_PAGE(gtk_list_item_get_item(item));

  data->page = page;

  row_add_binding(data, g_object_bind_property(page, "title",
                                               data->label, "label",
                                               G_BINDING_SYNC_CREATE));
  // The label is ellipsized, so show the full title in a tooltip
  row_add_binding(data, g_object_bind_property(page, "title",
                                               row, "tooltip-text",
                                               G_BINDING_SYNC_CREATE));
  row_add_binding(data, g_object_bind_property(page, "icon",
                                               data->icon, "gicon",
                                               G_BINDING_SYNC_CREATE));
  row_add_binding(data, g_object_bind_property_full(page, "icon",
                                                    data->icon, "visible",
                                                    G_BINDING_SYNC_CREATE,
                                                    object_to_visible,
                                                    nullptr, nullptr, nullptr));
  row_add_binding(data, g_object_bind_property(page, "indicator-icon",
                                               data->indicator, "gicon",
                                               G_BINDING_SYNC_CREATE));
  row_add_binding(data, g_object_bind_property_full(page, "indicator-icon",
                                                    data->indicator, "visible",
                                                    G_BINDING_SYNC_CREATE,
                                                    object_to_visible,
                                                    nullptr, nullptr, nullptr));
  // Like AdwTabBar, don't offer to close pinned tabs from the tab itself
  row_add_binding(data, g_object_bind_property(page, "pinned",
                                               data->close_button, "visible",
                                               GBindingFlags(G_BINDING_SYNC_CREATE |
                                                             G_BINDING_INVERT_BOOLEAN)));

  data->attention_handler = g_signal_connect(page, "notify::needs-attention",
                                             G_CALLBACK(row_attention_notify_cb), row);
  row_update_attention(row, page);

  // The view may report the new number of pinned tabs before the page
  // itself is pinned, so watch both.
  data->pinned_handler = g_signal_connect_swapped(page, "notify::pinned",
                                                  G_CALLBACK(terminal_tab_sidebar_update_pinned_rows),
                                                  sidebar);
  terminal_tab_sidebar_update_pinned_rows(sidebar);
}

static void
factory_unbind_cb(GtkSignalListItemFactory* factory,
                  GObject* object,
                  TerminalTabSidebar* sidebar)
{
  auto const item = GTK_LIST_ITEM(object);
  auto const row = gtk_list_item_get_child(item);
  auto const data = row_data_get(row);

  g_ptr_array_set_size(data->bindings, 0);

  if (data->page) {
    g_clear_signal_handler(&data->attention_handler, data->page);
    g_clear_signal_handler(&data->pinned_handler, data->page);
  }
  data->page = nullptr;

  row_update_attention(row, nullptr);
  row_clear_drop_marks(row);
  gtk_widget_set_visible(data->pin_icon, false);
  if (auto const list_row = gtk_widget_get_parent(row))
    gtk_widget_remove_css_class(list_row, "last-pinned");
}

/* Sidebar */

static void
terminal_tab_sidebar_update_visibility(TerminalTabSidebar* sidebar)
{
  auto visible = false;

  if (sidebar->view) {
    // Same rule as AdwTabBar: when autohiding, hide the tabs if
    // there is at most one tab and no pinned tabs.
    auto const n_pages = adw_tab_view_get_n_pages(sidebar->view);
    auto const n_pinned = adw_tab_view_get_n_pinned_pages(sidebar->view);
    visible = !sidebar->autohide || n_pages > 1 || n_pinned > 0;
  }

  gtk_widget_set_visible(sidebar->scrolled_window, visible);
  gtk_widget_set_visible(sidebar->handle, visible);
}

static void
terminal_tab_sidebar_scroll_to_selected(TerminalTabSidebar* sidebar)
{
  if (!sidebar->view)
    return;

  auto const page = adw_tab_view_get_selected_page(sidebar->view);
  if (!page)
    return;

  auto const position = adw_tab_view_get_page_position(sidebar->view, page);
  if (position < 0)
    return;

  gtk_list_view_scroll_to(GTK_LIST_VIEW(sidebar->list_view),
                          guint(position),
                          GTK_LIST_SCROLL_NONE,
                          nullptr);
}

static void
view_notify_pages_cb(AdwTabView* view,
                     GParamSpec* pspec,
                     TerminalTabSidebar* sidebar)
{
  terminal_tab_sidebar_update_visibility(sidebar);
  terminal_tab_sidebar_update_pinned_rows(sidebar);
}

/* Keyboard */

static RowData*
terminal_tab_sidebar_get_selected_row(TerminalTabSidebar* sidebar)
{
  auto const page = sidebar->view ? adw_tab_view_get_selected_page(sidebar->view) : nullptr;
  for (auto i = 0u; page && i < sidebar->rows->len; ++i) {
    auto const data = row_data_get(GTK_WIDGET(g_ptr_array_index(sidebar->rows, i)));
    if (data->page == page)
      return data;
  }
  return nullptr;
}

/* Gives the focus back to the terminal of the selected tab */
static void
terminal_tab_sidebar_focus_terminal(TerminalTabSidebar* sidebar)
{
  auto const page = sidebar->view ? adw_tab_view_get_selected_page(sidebar->view) : nullptr;
  auto const child = page ? adw_tab_page_get_child(page) : nullptr;
  if (child && TERMINAL_IS_TAB(child))
    if (auto const screen = terminal_tab_get_screen(TERMINAL_TAB(child)))
      gtk_widget_grab_focus(GTK_WIDGET(screen));
}

static gboolean
list_shortcut_menu_cb(GtkWidget* widget,
                      GVariant* args,
                      void* user_data)
{
  auto const sidebar = TERMINAL_TAB_SIDEBAR(user_data);
  auto const data = terminal_tab_sidebar_get_selected_row(sidebar);
  if (!data)
    return false;

  auto const row = gtk_widget_get_parent(data->label);
  show_context_menu(sidebar, data->page, row,
                    gtk_widget_get_width(row) / 2.0, gtk_widget_get_height(row) / 2.0);
  return true;
}

static gboolean
list_shortcut_close_cb(GtkWidget* widget,
                       GVariant* args,
                       void* user_data)
{
  auto const sidebar = TERMINAL_TAB_SIDEBAR(user_data);
  auto const page = sidebar->view ? adw_tab_view_get_selected_page(sidebar->view) : nullptr;
  if (!page || adw_tab_page_get_pinned(page))
    return false;

  adw_tab_view_close_page(sidebar->view, page);
  return true;
}

static gboolean
list_shortcut_leave_cb(GtkWidget* widget,
                       GVariant* args,
                       void* user_data)
{
  terminal_tab_sidebar_focus_terminal(TERMINAL_TAB_SIDEBAR(user_data));
  return true;
}

/* The list only takes the focus while it is used with the keyboard, see
 * terminal_tab_sidebar_focus_list(); a click on a tab keeps the focus in
 * the terminal. */
static void
list_focus_leave_cb(GtkEventControllerFocus* controller,
                    TerminalTabSidebar* sidebar)
{
  gtk_widget_set_can_focus(sidebar->list_view, false);
}

/* Resizing */

/* The x position of the pointer in the window. The handle moves while it
 * is dragged, so its own coordinates would drift. */
static bool
handle_get_pointer_x(GtkGesture* gesture,
                     double* x)
{
  auto const event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture));
  double y;
  return event && gdk_event_get_position(event, x, &y);
}

static void
handle_drag_begin_cb(GtkGestureDrag* gesture,
                     double x,
                     double y,
                     TerminalTabSidebar* sidebar)
{
  sidebar->drag_start_width = sidebar->width;
  if (!handle_get_pointer_x(GTK_GESTURE(gesture), &sidebar->drag_start_x))
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_DENIED);
}

static void
handle_drag_update_cb(GtkGestureDrag* gesture,
                      double offset_x,
                      double offset_y,
                      TerminalTabSidebar* sidebar)
{
  double x;
  if (!handle_get_pointer_x(GTK_GESTURE(gesture), &x))
    return;

  // The handle is on the inner edge, towards the terminal
  auto const delta = x - sidebar->drag_start_x;
  terminal_tab_sidebar_set_width(sidebar,
                                 sidebar->drag_start_width +
                                 int(sidebar->position == GTK_POS_LEFT ? delta : -delta));
}

static void
view_notify_selected_page_cb(AdwTabView* view,
                             GParamSpec* pspec,
                             TerminalTabSidebar* sidebar)
{
  terminal_tab_sidebar_scroll_to_selected(sidebar);
}

/* GtkWidgetClass impl */

static void
terminal_tab_sidebar_measure(GtkWidget* widget,
                             GtkOrientation orientation,
                             int for_size,
                             int* minimum,
                             int* natural,
                             int* minimum_baseline,
                             int* natural_baseline)
{
  auto const sidebar = TERMINAL_TAB_SIDEBAR(widget);

  *minimum = *natural = 0;
  if (sidebar->scrolled_window && gtk_widget_get_visible(sidebar->scrolled_window)) {
    gtk_widget_measure(sidebar->scrolled_window, orientation, for_size,
                       minimum, natural, nullptr, nullptr);
    if (orientation == GTK_ORIENTATION_HORIZONTAL)
      *minimum = *natural = MAX(*minimum, sidebar->width);
  }

  *minimum_baseline = *natural_baseline = -1;
}

static void
terminal_tab_sidebar_size_allocate(GtkWidget* widget,
                                   int width,
                                   int height,
                                   int baseline)
{
  auto const sidebar = TERMINAL_TAB_SIDEBAR(widget);

  if (sidebar->scrolled_window &&
      gtk_widget_should_layout(sidebar->scrolled_window))
    gtk_widget_allocate(sidebar->scrolled_window, width, height, baseline, nullptr);

  if (sidebar->handle && gtk_widget_should_layout(sidebar->handle)) {
    auto const x = sidebar->position == GTK_POS_LEFT ? width - HANDLE_WIDTH : 0;
    GtkAllocation allocation = {x, 0, HANDLE_WIDTH, height};
    gtk_widget_size_allocate(sidebar->handle, &allocation, -1);
  }

  if (sidebar->context_menu)
    gtk_popover_present(GTK_POPOVER(sidebar->context_menu));
}

/* GObjectClass impl */

static void
terminal_tab_sidebar_init(TerminalTabSidebar* sidebar)
{
  sidebar->autohide = TRUE;
  sidebar->position = GTK_POS_LEFT;
  sidebar->width = DEFAULT_WIDTH;
  sidebar->rows = g_ptr_array_new();

  auto const factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(factory_setup_cb), sidebar);
  g_signal_connect(factory, "bind", G_CALLBACK(factory_bind_cb), sidebar);
  g_signal_connect(factory, "unbind", G_CALLBACK(factory_unbind_cb), sidebar);

  sidebar->list_view = gtk_list_view_new(nullptr, factory); // adopts factory
  gtk_widget_add_css_class(sidebar->list_view, "navigation-sidebar");
  // Keep the keyboard focus in the terminal when clicking on a tab
  gtk_widget_set_can_focus(sidebar->list_view, false);

  auto const focus_controller = gtk_event_controller_focus_new();
  g_signal_connect(focus_controller, "leave", G_CALLBACK(list_focus_leave_cb), sidebar);
  gtk_widget_add_controller(sidebar->list_view, focus_controller);

  auto const shortcuts = gtk_shortcut_controller_new();
  auto add_shortcut = [&](char const* trigger, GtkShortcutFunc func) {
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(shortcuts),
                                         gtk_shortcut_new(gtk_shortcut_trigger_parse_string(trigger),
                                                          gtk_callback_action_new(func, sidebar, nullptr)));
  };
  add_shortcut("Menu|<Shift>F10", list_shortcut_menu_cb);
  add_shortcut("Delete", list_shortcut_close_cb);
  add_shortcut("Escape|Return|KP_Enter", list_shortcut_leave_cb);
  gtk_widget_add_controller(sidebar->list_view, shortcuts);

  auto const list_drop = gtk_drop_target_new(G_TYPE_INVALID, GDK_ACTION_MOVE);
  set_drop_types(list_drop);
  g_signal_connect(list_drop, "drop", G_CALLBACK(list_drop_cb), sidebar);
  gtk_widget_add_controller(sidebar->list_view, GTK_EVENT_CONTROLLER(list_drop));

  sidebar->scrolled_window = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sidebar->scrolled_window),
                                 GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(sidebar->scrolled_window, true);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sidebar->scrolled_window),
                                sidebar->list_view);
  gtk_widget_set_parent(sidebar->scrolled_window, GTK_WIDGET(sidebar));

  // Drag the inner edge to resize; it is on top of the list
  sidebar->handle = GTK_WIDGET(g_object_new(GTK_TYPE_BOX, "css-name", "handle", nullptr));
  gtk_widget_set_cursor_from_name(sidebar->handle, "col-resize");
  auto const drag = gtk_gesture_drag_new();
  g_signal_connect(drag, "drag-begin", G_CALLBACK(handle_drag_begin_cb), sidebar);
  g_signal_connect(drag, "drag-update", G_CALLBACK(handle_drag_update_cb), sidebar);
  gtk_widget_add_controller(sidebar->handle, GTK_EVENT_CONTROLLER(drag));
  gtk_widget_set_parent(sidebar->handle, GTK_WIDGET(sidebar));

  terminal_tab_sidebar_set_position(sidebar, GTK_POS_LEFT);
  terminal_tab_sidebar_update_visibility(sidebar);
}

static void
terminal_tab_sidebar_dispose(GObject* object)
{
  auto const sidebar = TERMINAL_TAB_SIDEBAR(object);

  g_clear_handle_id(&sidebar->reset_menu_source, g_source_remove);

  if (sidebar->list_view)
    terminal_tab_sidebar_set_view(sidebar, nullptr);

  g_clear_pointer(reinterpret_cast<GtkWidget**>(&sidebar->context_menu), gtk_widget_unparent);
  g_clear_pointer(&sidebar->scrolled_window, gtk_widget_unparent);
  g_clear_pointer(&sidebar->handle, gtk_widget_unparent);
  sidebar->list_view = nullptr;
  if (sidebar->rows) {
    for (auto i = 0u; i < sidebar->rows->len; ++i)
      g_object_weak_unref(G_OBJECT(g_ptr_array_index(sidebar->rows, i)), row_finalized_cb, sidebar);
    g_clear_pointer(&sidebar->rows, g_ptr_array_unref);
  }

  G_OBJECT_CLASS(terminal_tab_sidebar_parent_class)->dispose(object);
}

static void
terminal_tab_sidebar_get_property(GObject* object,
                                  guint prop_id,
                                  GValue* value,
                                  GParamSpec* pspec)
{
  auto const sidebar = TERMINAL_TAB_SIDEBAR(object);

  switch (prop_id) {
  case PROP_VIEW:
    g_value_set_object(value, sidebar->view);
    break;
  case PROP_AUTOHIDE:
    g_value_set_boolean(value, sidebar->autohide);
    break;
  case PROP_WIDTH:
    g_value_set_int(value, sidebar->width);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    break;
  }
}

static void
terminal_tab_sidebar_set_property(GObject* object,
                                  guint prop_id,
                                  GValue const* value,
                                  GParamSpec* pspec)
{
  auto const sidebar = TERMINAL_TAB_SIDEBAR(object);

  switch (prop_id) {
  case PROP_VIEW:
    terminal_tab_sidebar_set_view(sidebar, ADW_TAB_VIEW(g_value_get_object(value)));
    break;
  case PROP_AUTOHIDE:
    terminal_tab_sidebar_set_autohide(sidebar, g_value_get_boolean(value));
    break;
  case PROP_WIDTH:
    terminal_tab_sidebar_set_width(sidebar, g_value_get_int(value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    break;
  }
}

static void
terminal_tab_sidebar_class_init(TerminalTabSidebarClass* klass)
{
  auto const object_class = G_OBJECT_CLASS(klass);
  auto const widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = terminal_tab_sidebar_dispose;
  object_class->get_property = terminal_tab_sidebar_get_property;
  object_class->set_property = terminal_tab_sidebar_set_property;

  widget_class->measure = terminal_tab_sidebar_measure;
  widget_class->size_allocate = terminal_tab_sidebar_size_allocate;

  pspecs[PROP_VIEW] =
    g_param_spec_object("view", nullptr, nullptr,
                        ADW_TYPE_TAB_VIEW,
                        GParamFlags(G_PARAM_READWRITE |
                                    G_PARAM_STATIC_STRINGS |
                                    G_PARAM_EXPLICIT_NOTIFY));

  pspecs[PROP_AUTOHIDE] =
    g_param_spec_boolean("autohide", nullptr, nullptr,
                         TRUE,
                         GParamFlags(G_PARAM_READWRITE |
                                     G_PARAM_STATIC_STRINGS |
                                     G_PARAM_EXPLICIT_NOTIFY));

  pspecs[PROP_WIDTH] =
    g_param_spec_int("width", nullptr, nullptr,
                     MIN_WIDTH, MAX_WIDTH, DEFAULT_WIDTH,
                     GParamFlags(G_PARAM_READWRITE |
                                 G_PARAM_STATIC_STRINGS |
                                 G_PARAM_EXPLICIT_NOTIFY));

  g_object_class_install_properties(object_class, N_PROPS, pspecs);

  /* A tab's row was clicked with the middle button */
  signals[PAGE_MIDDLE_CLICKED] =
    g_signal_new("page-middle-clicked",
                 G_OBJECT_CLASS_TYPE(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 nullptr, nullptr,
                 g_cclosure_marshal_VOID__OBJECT,
                 G_TYPE_NONE,
                 1, ADW_TYPE_TAB_PAGE);

  /* A pane was dropped at a position in the list, to become a tab there.
   * Returns whether it was taken. */
  signals[SCREEN_DROPPED] =
    g_signal_new("screen-dropped",
                 G_OBJECT_CLASS_TYPE(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 g_signal_accumulator_true_handled, nullptr,
                 nullptr,
                 G_TYPE_BOOLEAN,
                 2, TERMINAL_TYPE_SCREEN, G_TYPE_INT);

  gtk_widget_class_set_css_name(widget_class, "tabsidebar");
}

/* Public API */

AdwTabView*
terminal_tab_sidebar_get_view(TerminalTabSidebar* sidebar)
{
  g_return_val_if_fail(TERMINAL_IS_TAB_SIDEBAR(sidebar), nullptr);

  return sidebar->view;
}

void
terminal_tab_sidebar_set_view(TerminalTabSidebar* sidebar,
                              AdwTabView* view)
{
  g_return_if_fail(TERMINAL_IS_TAB_SIDEBAR(sidebar));
  g_return_if_fail(view == nullptr || ADW_IS_TAB_VIEW(view));

  if (sidebar->view == view)
    return;

  if (sidebar->view) {
    g_signal_handlers_disconnect_by_data(sidebar->view, sidebar);
    gtk_list_view_set_model(GTK_LIST_VIEW(sidebar->list_view), nullptr);
    g_clear_object(&sidebar->view);
  }

  if (view) {
    sidebar->view = ADW_TAB_VIEW(g_object_ref(view));

    gs_unref_object auto pages = adw_tab_view_get_pages(view);
    gtk_list_view_set_model(GTK_LIST_VIEW(sidebar->list_view), pages);

    g_signal_connect(view, "notify::n-pages",
                     G_CALLBACK(view_notify_pages_cb), sidebar);
    g_signal_connect(view, "notify::n-pinned-pages",
                     G_CALLBACK(view_notify_pages_cb), sidebar);
    g_signal_connect(view, "notify::selected-page",
                     G_CALLBACK(view_notify_selected_page_cb), sidebar);
  }

  terminal_tab_sidebar_update_visibility(sidebar);

  g_object_notify_by_pspec(G_OBJECT(sidebar), pspecs[PROP_VIEW]);
}

gboolean
terminal_tab_sidebar_get_autohide(TerminalTabSidebar* sidebar)
{
  g_return_val_if_fail(TERMINAL_IS_TAB_SIDEBAR(sidebar), FALSE);

  return sidebar->autohide;
}

void
terminal_tab_sidebar_set_autohide(TerminalTabSidebar* sidebar,
                                  gboolean autohide)
{
  g_return_if_fail(TERMINAL_IS_TAB_SIDEBAR(sidebar));

  autohide = autohide != FALSE;
  if (sidebar->autohide == autohide)
    return;

  sidebar->autohide = autohide;
  terminal_tab_sidebar_update_visibility(sidebar);

  g_object_notify_by_pspec(G_OBJECT(sidebar), pspecs[PROP_AUTOHIDE]);
}

void
terminal_tab_sidebar_set_position(TerminalTabSidebar* sidebar,
                                  GtkPositionType position)
{
  g_return_if_fail(TERMINAL_IS_TAB_SIDEBAR(sidebar));
  g_return_if_fail(position == GTK_POS_LEFT || position == GTK_POS_RIGHT);

  sidebar->position = position;
  gtk_widget_queue_allocate(GTK_WIDGET(sidebar));

  auto const widget = GTK_WIDGET(sidebar);
  if (position == GTK_POS_LEFT) {
    gtk_widget_remove_css_class(widget, "right");
    gtk_widget_add_css_class(widget, "left");
  } else {
    gtk_widget_remove_css_class(widget, "left");
    gtk_widget_add_css_class(widget, "right");
  }
}

/**
 * terminal_tab_sidebar_set_width:
 * @sidebar: a #TerminalTabSidebar
 * @width: the width in pixels
 */
void
terminal_tab_sidebar_set_width(TerminalTabSidebar* sidebar,
                               int width)
{
  g_return_if_fail(TERMINAL_IS_TAB_SIDEBAR(sidebar));

  width = CLAMP(width, MIN_WIDTH, MAX_WIDTH);
  if (sidebar->width == width)
    return;

  sidebar->width = width;
  gtk_widget_queue_resize(GTK_WIDGET(sidebar));
  g_object_notify_by_pspec(G_OBJECT(sidebar), pspecs[PROP_WIDTH]);
}

/**
 * terminal_tab_sidebar_focus_list:
 * @sidebar: a #TerminalTabSidebar
 *
 * Moves the keyboard focus to the selected tab. The arrow keys then switch
 * tabs, the menu key opens the tab menu, Delete closes the tab, and Return
 * or Escape go back to the terminal.
 *
 * Returns: whether the list took the focus
 */
gboolean
terminal_tab_sidebar_focus_list(TerminalTabSidebar* sidebar)
{
  g_return_val_if_fail(TERMINAL_IS_TAB_SIDEBAR(sidebar), FALSE);

  if (!sidebar->view || !gtk_widget_is_visible(sidebar->scrolled_window))
    return FALSE;

  auto const page = adw_tab_view_get_selected_page(sidebar->view);
  if (!page)
    return FALSE;

  gtk_widget_set_can_focus(sidebar->list_view, true);
  gtk_list_view_scroll_to(GTK_LIST_VIEW(sidebar->list_view),
                          guint(adw_tab_view_get_page_position(sidebar->view, page)),
                          GTK_LIST_SCROLL_FOCUS,
                          nullptr);
  // That makes the selected tab the focus item; now move the focus there
  gtk_widget_grab_focus(sidebar->list_view);

  return gtk_widget_has_focus(sidebar->list_view) ||
    gtk_widget_is_ancestor(gtk_root_get_focus(gtk_widget_get_root(GTK_WIDGET(sidebar))),
                           sidebar->list_view);
}
