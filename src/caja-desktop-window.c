/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */

/*
 * Caja
 *
 * Copyright (C) 2000 Eazel, Inc.
 *
 * Caja is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * Caja is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Authors: Darin Adler <darin@bentspoon.com>
 */

#include <config.h>

#include <X11/Xatom.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <glib/gi18n.h>
#ifdef HAVE_WAYLAND
#include <gdk/gdkwayland.h>
#include <gtk-layer-shell/gtk-layer-shell.h>
#endif
#include <eel/eel-background.h>
#include <eel/eel-vfs-extensions.h>

#include <libcaja-private/caja-file-utilities.h>
#include <libcaja-private/caja-icon-names.h>

#include "caja-desktop-window.h"
#include "caja-window-private.h"
#include "caja-actions.h"

/* Tell screen readers that this is a desktop window */

static GType caja_desktop_window_accessible_get_type (void);

G_DEFINE_TYPE (CajaDesktopWindowAccessible, caja_desktop_window_accessible,
               GTK_TYPE_WINDOW_ACCESSIBLE);

static AtkAttributeSet *
desktop_get_attributes (AtkObject *accessible)
{
    AtkAttributeSet *attributes;
    AtkAttribute *is_desktop;

    attributes = ATK_OBJECT_CLASS (caja_desktop_window_accessible_parent_class)->get_attributes (accessible);

    is_desktop = g_malloc (sizeof (AtkAttribute));
    is_desktop->name = g_strdup ("is-desktop");
    is_desktop->value = g_strdup ("true");

    attributes = g_slist_append (attributes, is_desktop);

    return attributes;
}

static void
caja_desktop_window_accessible_init (CajaDesktopWindowAccessible *window)
{
}

static void
caja_desktop_window_accessible_class_init (CajaDesktopWindowAccessibleClass *klass)
{
    AtkObjectClass *aclass = ATK_OBJECT_CLASS (klass);

    aclass->get_attributes = desktop_get_attributes;
}

struct _CajaDesktopWindowPrivate
{
    gulong size_changed_id;

    gboolean loaded;
};

G_DEFINE_TYPE_WITH_PRIVATE (CajaDesktopWindow, caja_desktop_window,
               CAJA_TYPE_SPATIAL_WINDOW);

static void
caja_desktop_window_init (CajaDesktopWindow *window)
{
    GtkAction *action;
    AtkObject *accessible;

    window->details = caja_desktop_window_get_instance_private (window);

    GtkStyleContext *context;

    context = gtk_widget_get_style_context (GTK_WIDGET (window));
    gtk_style_context_add_class (context, "caja-desktop-window");

    gtk_window_move (GTK_WINDOW (window), 0, 0);

    /* shouldn't really be needed given our semantic type
     * of _NET_WM_TYPE_DESKTOP, but why not
     */
    gtk_window_set_resizable (GTK_WINDOW (window),
                              FALSE);

    g_object_set_data (G_OBJECT (window), "is_desktop_window",
                       GINT_TO_POINTER (1));

    gtk_widget_hide (CAJA_WINDOW (window)->details->statusbar);
    gtk_widget_hide (CAJA_WINDOW (window)->details->menubar);
    gtk_window_set_decorated (GTK_WINDOW (window), FALSE);

    /* Don't allow close action on desktop */
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
    action = gtk_action_group_get_action (CAJA_WINDOW (window)->details->main_action_group,
                                          CAJA_ACTION_CLOSE);
    gtk_action_set_sensitive (action, FALSE);
    G_GNUC_END_IGNORE_DEPRECATIONS;

    /* Set the accessible name so that it doesn't inherit the cryptic desktop URI. */
    accessible = gtk_widget_get_accessible (GTK_WIDGET (window));

    if (accessible) {
        atk_object_set_name (accessible, _("Desktop"));
    }
}

static gint
caja_desktop_window_delete_event (CajaDesktopWindow *window)
{
    /* Returning true tells GTK+ not to delete the window. */
    return TRUE;
}

void
caja_desktop_window_update_directory (CajaDesktopWindow *window)
{
    GFile *location;

    g_assert (CAJA_IS_DESKTOP_WINDOW (window));

    location = g_file_new_for_uri (EEL_DESKTOP_URI);
    caja_window_go_to (CAJA_WINDOW (window), location);
    window->details->loaded = TRUE;

    g_object_unref (location);
}

static void
caja_desktop_window_screen_size_changed (GdkScreen             *screen,
        CajaDesktopWindow *window)
{
    int width_request, height_request;

    GdkDisplay *display = gdk_screen_get_display (screen);
    if (GDK_IS_X11_DISPLAY (display))
    {
        GdkWindow *root_window;
        root_window = gdk_screen_get_root_window (screen);
        gdk_window_get_geometry (root_window, NULL, NULL, &width_request, &height_request);
    }
    else
    {
        /*No root window or primary monitor in wayland unless compositors add it back*/
        GdkRectangle geometry = {0};
        GdkMonitor *monitor;
        monitor = gdk_display_get_monitor (display, 0);
        gdk_monitor_get_geometry (monitor, &geometry);
        width_request = geometry.width;
        height_request = geometry.height;
    }

    g_object_set (window,
                  "width_request", width_request,
                  "height_request", height_request,
                  NULL);
}

CajaDesktopWindow *
caja_desktop_window_new (CajaApplication *application,
                         GdkScreen           *screen)
{
    CajaDesktopWindow *window;
    int width_request, height_request;
    int scale;

    GdkDisplay *display = gdk_screen_get_display (screen);
    if (GDK_IS_X11_DISPLAY (display))
    {
        scale = gdk_window_get_scale_factor (gdk_screen_get_root_window (screen));
        width_request = WidthOfScreen (gdk_x11_screen_get_xscreen (screen)) / scale;
        height_request = HeightOfScreen (gdk_x11_screen_get_xscreen (screen)) / scale;
    }
    else
    {
        /*FIXME: There is no primary monitor in wayland itself
        *compositors can implement this but as this is written
        *only a few wayland compositors allow setting a primary monitor
        *and they all do it differently. For now, use the first monitor
        */
        GdkRectangle geometry = {0};
        GdkMonitor *monitor;
        monitor = gdk_display_get_monitor (display, 0);
        gdk_monitor_get_geometry (monitor, &geometry);
        width_request = geometry.width;
        height_request = geometry.height;
    }

    window = CAJA_DESKTOP_WINDOW
             (gtk_widget_new (caja_desktop_window_get_type(),
                              "app", application,
                              "width_request", width_request,
                              "height_request", height_request,
                              "screen", screen,
                              NULL));
    /* Stop wrong desktop window size in GTK 3.20*/
    /* We don't want to set a default size, which the parent does, since this */
    /* will cause the desktop window to open at the wrong size in gtk 3.20 */
    gtk_window_set_default_size (GTK_WINDOW (window), -1, -1);

    /*For wayland only
     *Code taken from gtk-layer-shell simple-example.c
     */
#ifdef HAVE_WAYLAND
    if (GDK_IS_WAYLAND_DISPLAY (display))
    {
        GtkWindow *gtkwin;
        gtkwin = (GTK_WINDOW(window));

        /* Before the window is first realized, set it up to be a layer surface */
        gtk_layer_init_for_window (gtkwin);

        /* Order below normal windows */
        gtk_layer_set_layer (gtkwin, GTK_LAYER_SHELL_LAYER_BOTTOM);

        gtk_layer_set_namespace (gtkwin, "desktop");

        /*Anchor the desktop to all four corners
         *This is much simpler than on x11 and
         *should always render the desktop across
         *all of the screen
         */
        gtk_layer_set_anchor (gtkwin, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
        gtk_layer_set_anchor (gtkwin, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
        gtk_layer_set_anchor (gtkwin, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
        gtk_layer_set_anchor (gtkwin, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);

        /*Enable keyboard use on the desktop*/
        gtk_layer_set_keyboard_mode (gtkwin, GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
    }
#endif
    /* Special sawmill setting*/
    GdkWindow *gdkwin;
    if ((GDK_IS_X11_DISPLAY (display)))
        gtk_widget_realize (GTK_WIDGET (window));
    else
        gtk_widget_show (GTK_WIDGET (window));

    gdkwin = gtk_widget_get_window (GTK_WIDGET (window));
    if ((GDK_IS_X11_DISPLAY (display)) && (gdk_window_ensure_native (gdkwin)))
    {
        Display *disp = GDK_DISPLAY_XDISPLAY (gdk_window_get_display (gdkwin));
        XClassHint *xch = XAllocClassHint ();
        xch->res_name = "desktop_window";
        xch->res_class = "Caja";
        XSetClassHint (disp, GDK_WINDOW_XID(gdkwin), xch);
        XFree(xch);
    }

    gdk_window_set_title (gdkwin, _("Desktop"));

    g_signal_connect (window, "delete_event", G_CALLBACK (caja_desktop_window_delete_event), NULL);

    /* Point window at the desktop folder.
     * Note that caja_desktop_window_init is too early to do this.
     */
    caja_desktop_window_update_directory (window);

    return window;
}

static void
map (GtkWidget *widget)
{
    /* Chain up to realize our children */
    GTK_WIDGET_CLASS (caja_desktop_window_parent_class)->map (widget);
    gdk_window_lower (gtk_widget_get_window (widget));
}

static void
unrealize (GtkWidget *widget)
{
    CajaDesktopWindow *window;
    CajaDesktopWindowPrivate *details;
    GdkWindow *root_window;
    GdkDisplay *display;

    window = CAJA_DESKTOP_WINDOW (widget);
    details = window->details;
    display = gtk_widget_get_display (widget);

    /*Avoid root window on wayland-it's not supposed to work*/
    if (GDK_IS_X11_DISPLAY (display))
    {
        root_window = gdk_screen_get_root_window (
                      gtk_window_get_screen (GTK_WINDOW (window)));

        gdk_property_delete (root_window,
                             gdk_atom_intern ("CAJA_DESKTOP_WINDOW_ID", TRUE));
    }

    if (details->size_changed_id != 0) {
        g_signal_handler_disconnect (gtk_window_get_screen (GTK_WINDOW (window)),
                         details->size_changed_id);
        details->size_changed_id = 0;
    }

    GTK_WIDGET_CLASS (caja_desktop_window_parent_class)->unrealize (widget);
}

/*This should only be reached in x11*/
static void
set_wmspec_desktop_hint (GdkWindow *window)
{
    GdkAtom atom;

    atom = gdk_atom_intern ("_NET_WM_WINDOW_TYPE_DESKTOP", FALSE);

    gdk_property_change (window,
                         gdk_atom_intern ("_NET_WM_WINDOW_TYPE", FALSE),
                         gdk_x11_xatom_to_atom (XA_ATOM), 32,
                         GDK_PROP_MODE_REPLACE, (guchar *) &atom, 1);
}

/*This should only be reached in x11*/
static void
set_desktop_window_id (CajaDesktopWindow *window,
                       GdkWindow             *gdkwindow)
{
    /* Tuck the desktop windows xid in the root to indicate we own the desktop in on x11
     */
    Window window_xid;
    GdkWindow *root_window;

    root_window = gdk_screen_get_root_window (
                      gtk_window_get_screen (GTK_WINDOW (window)));
    window_xid = GDK_WINDOW_XID (gdkwindow);

    gdk_property_change (root_window,
                         gdk_atom_intern ("CAJA_DESKTOP_WINDOW_ID", FALSE),
                         gdk_x11_xatom_to_atom (XA_WINDOW), 32,
                         GDK_PROP_MODE_REPLACE, (guchar *) &window_xid, 1);
}

static void
realize (GtkWidget *widget)
{
    CajaDesktopWindow *window;
    CajaDesktopWindowPrivate *details;
    window = CAJA_DESKTOP_WINDOW (widget);
    details = window->details;
    GdkDisplay *display;

    /* Make sure we get keyboard events */
    display = gtk_widget_get_display (widget);
    if (GDK_IS_X11_DISPLAY (display))
        gtk_widget_set_events (widget, gtk_widget_get_events (widget)
                               | GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK);

    /*Do the work of realizing. */
    GTK_WIDGET_CLASS (caja_desktop_window_parent_class)->realize (widget);

    /* This is the new way to set up the desktop window in x11 but not for wayland */
    display = gtk_widget_get_display (widget);
    if (GDK_IS_X11_DISPLAY (display))
    {
        set_wmspec_desktop_hint (gtk_widget_get_window (widget));
        set_desktop_window_id (window, gtk_widget_get_window (widget));
    }

    details->size_changed_id =
        g_signal_connect (gtk_window_get_screen (GTK_WINDOW (window)), "size_changed",
                          G_CALLBACK (caja_desktop_window_screen_size_changed), window);
}

/* Should only reached in x11*/
static gboolean
draw (GtkWidget *widget,
      cairo_t   *cr)
{
    g_assert (GDK_IS_X11_DISPLAY (gdk_display_get_default()));
    eel_background_draw (widget, cr);
    return GTK_WIDGET_CLASS (caja_desktop_window_parent_class)->draw (widget, cr);
}

static CajaIconInfo *
real_get_icon (CajaWindow *window,
               CajaWindowSlot *slot)
{
    gint scale = gtk_widget_get_scale_factor (GTK_WIDGET (window));
    return caja_icon_info_lookup_from_name (CAJA_ICON_DESKTOP, 48, scale);
}

static void
caja_desktop_window_class_init (CajaDesktopWindowClass *klass)
{
    GtkWidgetClass *wclass = GTK_WIDGET_CLASS (klass);
    CajaWindowClass *nclass = CAJA_WINDOW_CLASS (klass);

    wclass->realize = realize;
    wclass->unrealize = unrealize;
    wclass->map = map;
   /*Drawing the desktop background from here gives a black background in wayland
    *So manage desktop background from the icon container as in navigation windows
    */
    if (GDK_IS_X11_DISPLAY (gdk_display_get_default()))
        wclass->draw = draw;

    gtk_widget_class_set_accessible_type (wclass, CAJA_TYPE_DESKTOP_WINDOW_ACCESSIBLE);

    nclass->window_type = CAJA_WINDOW_DESKTOP;
    nclass->get_icon = real_get_icon;
}

gboolean
caja_desktop_window_loaded (CajaDesktopWindow *window)
{
    return window->details->loaded;
}
