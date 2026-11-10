/*
 * Copyright © 2018 Christian Persch
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

#include <glib/gi18n.h>

#include <adwaita.h>

#include "terminal-headerbar.hh"
#include "terminal-app.hh"
#include "terminal-libgsystem.hh"

struct _TerminalHeaderbar
{
  GtkWidget parent_instance;
  AdwHeaderBar *headerbar;
  GtkWidget *profilebutton;
  GtkWidget *new_tab_button;
  GtkWidget *menubutton;
  GtkWidget *zoom_label;
  GtkPopoverMenu *profiles_popover_menu;
  gulong items_changed_handler;
};

G_DEFINE_FINAL_TYPE (TerminalHeaderbar, terminal_headerbar, GTK_TYPE_WIDGET)

static void
profilemenu_items_changed_cb (GMenuModel *menu,
                              int position G_GNUC_UNUSED,
                              int removed G_GNUC_UNUSED,
                              int added G_GNUC_UNUSED,
                              TerminalHeaderbar *headerbar)
{
  gtk_widget_set_visible (headerbar->new_tab_button,
                          g_menu_model_get_n_items (menu) == 0);
  gtk_widget_set_visible (headerbar->profilebutton,
                          g_menu_model_get_n_items (menu) > 0);
}

static GtkWidget *
zoom_button_new (const char *icon_name,
                 const char *tooltip,
                 const char *action)
{
  auto const button = gtk_button_new_from_icon_name (icon_name);
  gtk_widget_set_tooltip_text (button, tooltip);
  gtk_actionable_set_action_name (GTK_ACTIONABLE (button), action);
  gtk_widget_add_css_class (button, "flat");
  return button;
}

/* Zoom out, the zoom level, which resets the zoom when clicked, and zoom in,
 * at the top of the main menu */
static void
terminal_headerbar_add_zoom_controls (TerminalHeaderbar *headerbar)
{
  auto const box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_set_homogeneous (GTK_BOX (box), TRUE);
  gtk_widget_set_hexpand (box, TRUE);

  auto const reset = gtk_button_new ();
  gtk_widget_set_tooltip_text (reset, _("Reset Zoom"));
  gtk_actionable_set_action_name (GTK_ACTIONABLE (reset), "win.zoom-normal");
  gtk_widget_add_css_class (reset, "flat");
  headerbar->zoom_label = gtk_label_new (nullptr);
  gtk_button_set_child (GTK_BUTTON (reset), headerbar->zoom_label);

  gtk_box_append (GTK_BOX (box), zoom_button_new ("zoom-out-symbolic", _("Zoom Out"), "win.zoom-out"));
  gtk_box_append (GTK_BOX (box), reset);
  gtk_box_append (GTK_BOX (box), zoom_button_new ("zoom-in-symbolic", _("Zoom In"), "win.zoom-in"));

  terminal_headerbar_set_zoom (headerbar, 1.0);

  auto const popover = gtk_menu_button_get_popover (GTK_MENU_BUTTON (headerbar->menubutton));
  gtk_popover_menu_add_child (GTK_POPOVER_MENU (popover), box, "zoom");
}

/* Class implementation */

static void
terminal_headerbar_init (TerminalHeaderbar *headerbar)
{

  GtkWidget *widget = GTK_WIDGET (headerbar);
  TerminalApp *app = terminal_app_get ();
  GMenuModel *profilemenu;

  gtk_widget_init_template (widget);

  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (headerbar->menubutton),
                                  terminal_app_get_headermenu (app));
  terminal_headerbar_add_zoom_controls (headerbar);

  profilemenu = terminal_app_get_profilemenu (app);
  adw_split_button_set_menu_model (ADW_SPLIT_BUTTON (headerbar->profilebutton),
                                  profilemenu);

  headerbar->items_changed_handler =
    g_signal_connect_object (profilemenu,
                             "items-changed",
                             G_CALLBACK (profilemenu_items_changed_cb),
                             headerbar,
                             G_CONNECT_DEFAULT);
  profilemenu_items_changed_cb (profilemenu, 0, 0, 0, headerbar);
}

static void
terminal_headerbar_dispose (GObject *object)
{
  TerminalHeaderbar *headerbar = TERMINAL_HEADERBAR (object);
  TerminalApp *app = terminal_app_get ();

  gtk_widget_dispose_template (GTK_WIDGET (headerbar), TERMINAL_TYPE_HEADERBAR);

  GMenuModel *profilemenu = terminal_app_get_profilemenu (app);
  g_clear_signal_handler (&headerbar->items_changed_handler, profilemenu);

  G_OBJECT_CLASS (terminal_headerbar_parent_class)->dispose (object);
}

static void
terminal_headerbar_class_init (TerminalHeaderbarClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  gobject_class->dispose = terminal_headerbar_dispose;

  gtk_widget_class_set_template_from_resource (widget_class, "/com/aripd/pano/ui/headerbar.ui");
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_bind_template_child (widget_class, TerminalHeaderbar, headerbar);
  gtk_widget_class_bind_template_child (widget_class, TerminalHeaderbar, menubutton);
  gtk_widget_class_bind_template_child (widget_class, TerminalHeaderbar, profilebutton);
  gtk_widget_class_bind_template_child (widget_class, TerminalHeaderbar, new_tab_button);
}

/**
 * terminal_headerbar_new:
 *
 * Returns: a new #TerminalHeaderbar
 */
GtkWidget *
terminal_headerbar_new (void)
{
  return reinterpret_cast<GtkWidget*>
    (g_object_new (TERMINAL_TYPE_HEADERBAR, nullptr));
}

/* Opens the main menu, if the header bar is shown */
gboolean
terminal_headerbar_popup_menu (TerminalHeaderbar *headerbar)
{
  g_return_val_if_fail (TERMINAL_IS_HEADERBAR (headerbar), FALSE);

  if (!gtk_widget_is_drawable (headerbar->menubutton))
    return FALSE;

  gtk_menu_button_popup (GTK_MENU_BUTTON (headerbar->menubutton));
  return TRUE;
}

/* Calls @callback (GtkPopover*, @user_data) when the main menu or the menu of
 * profiles closes
 */
void
terminal_headerbar_connect_menus_closed (TerminalHeaderbar *headerbar,
                                         GCallback callback,
                                         gpointer user_data)
{
  g_return_if_fail (TERMINAL_IS_HEADERBAR (headerbar));

  GtkPopover *popovers[] = {
    gtk_menu_button_get_popover (GTK_MENU_BUTTON (headerbar->menubutton)),
    adw_split_button_get_popover (ADW_SPLIT_BUTTON (headerbar->profilebutton)),
  };
  for (auto popover : popovers) {
    if (popover)
      g_signal_connect_object (popover, "closed", callback, user_data, G_CONNECT_DEFAULT);
  }
}

/* Shows @scale, the zoom of the active terminal, in the main menu */
void
terminal_headerbar_set_zoom (TerminalHeaderbar *headerbar,
                             double scale)
{
  g_return_if_fail (TERMINAL_IS_HEADERBAR (headerbar));

  /* Translators: the zoom level in percent, such as "110%" */
  gs_free char *label = g_strdup_printf (_("%d%%"), int(scale * 100 + 0.5));
  gtk_label_set_label (GTK_LABEL (headerbar->zoom_label), label);
}
