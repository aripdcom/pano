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
#include "terminal-libgsystem.hh"

#define SIDEBAR_WIDTH 200

#define ROW_DATA_KEY "terminal-tab-sidebar-row"

struct _TerminalTabSidebar
{
  GtkWidget parent_instance;

  AdwTabView* view; // owned
  GtkWidget* scrolled_window;
  GtkWidget* list_view;
  GtkPopoverMenu* context_menu;

  guint reset_menu_source;
  gboolean autohide;
};

enum {
  PROP_0,
  PROP_VIEW,
  PROP_AUTOHIDE,
  N_PROPS
};

static GParamSpec* pspecs[N_PROPS];

G_DEFINE_FINAL_TYPE(TerminalTabSidebar, terminal_tab_sidebar, GTK_TYPE_WIDGET)

/* Per-row state, attached to the row widget */

typedef struct {
  TerminalTabSidebar* sidebar; // unowned
  AdwTabPage* page; // unowned, only set while the row is bound
  GtkWidget* icon;
  GtkWidget* label;
  GtkWidget* indicator;
  GtkWidget* close_button;
  GPtrArray* bindings;
  gulong attention_handler;
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

  if (data->page)
    g_clear_signal_handler(&data->attention_handler, data->page);
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

  g_object_set_data_full(G_OBJECT(row), ROW_DATA_KEY, data, row_data_free);
  gtk_list_item_set_child(item, row);
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

  if (data->page)
    g_clear_signal_handler(&data->attention_handler, data->page);
  data->page = nullptr;

  row_update_attention(row, nullptr);
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
  if (sidebar->scrolled_window)
    gtk_widget_measure(sidebar->scrolled_window, orientation, for_size,
                       minimum, natural, nullptr, nullptr);

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

  if (sidebar->context_menu)
    gtk_popover_present(GTK_POPOVER(sidebar->context_menu));
}

/* GObjectClass impl */

static void
terminal_tab_sidebar_init(TerminalTabSidebar* sidebar)
{
  sidebar->autohide = TRUE;

  auto const factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(factory_setup_cb), sidebar);
  g_signal_connect(factory, "bind", G_CALLBACK(factory_bind_cb), sidebar);
  g_signal_connect(factory, "unbind", G_CALLBACK(factory_unbind_cb), sidebar);

  sidebar->list_view = gtk_list_view_new(nullptr, factory); // adopts factory
  gtk_widget_add_css_class(sidebar->list_view, "navigation-sidebar");
  // Keep the keyboard focus in the terminal when clicking on a tab
  gtk_widget_set_can_focus(sidebar->list_view, false);

  sidebar->scrolled_window = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sidebar->scrolled_window),
                                 GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  gtk_widget_set_size_request(sidebar->scrolled_window, SIDEBAR_WIDTH, -1);
  gtk_widget_set_vexpand(sidebar->scrolled_window, true);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sidebar->scrolled_window),
                                sidebar->list_view);
  gtk_widget_set_parent(sidebar->scrolled_window, GTK_WIDGET(sidebar));

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
  sidebar->list_view = nullptr;

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

  g_object_class_install_properties(object_class, N_PROPS, pspecs);

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

  auto const widget = GTK_WIDGET(sidebar);
  if (position == GTK_POS_LEFT) {
    gtk_widget_remove_css_class(widget, "right");
    gtk_widget_add_css_class(widget, "left");
  } else {
    gtk_widget_remove_css_class(widget, "left");
    gtk_widget_add_css_class(widget, "right");
  }
}
