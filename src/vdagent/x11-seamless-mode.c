/*  vdagent-x11-seamless-mode.c vdagent seamless mode integration code

    Copyright 2015 Red Hat, Inc.

    Red Hat Authors:
    Ondrej Holy <oholy@redhat.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <glib.h>
#include <X11/X.h>
#include <X11/Xproto.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <syslog.h>
#include <spice/vd_agent.h>

#include "vdagentd-proto.h"
#include "x11.h"
#include "x11-priv.h"

/* Get window property. */
static gulong
get_window_property(Display *display, Window window,
                    const gchar *property, Atom type, int format,
                    guchar **data_ret)
{
    Atom property_atom, type_ret;
    gulong nitems_ret, bytes_after_ret;
    guchar *data = NULL;
    gint format_ret, rc;

    property_atom = XInternAtom(display, property, TRUE);
    if (!property_atom)
        return 0;

    rc = XGetWindowProperty(display, window, property_atom,
                            0, LONG_MAX, False, type,
                            &type_ret, &format_ret, &nitems_ret,
                            &bytes_after_ret, &data);
    if (rc == Success && type_ret == type && format_ret == format) {
        *data_ret = data;

        return nitems_ret;
    }

    if (data) {
        syslog(LOG_WARNING, "vdagent-x11-seamless-mode: "
               "XGetWindowProperty(%s) returned data of unexpected format/type",
               property);
        XFree(data);
    }

    return 0;
}

/* Get window type, or None. */
static Atom
get_window_type(Display *display, Window window)
{
    guchar *data = NULL;

    if (get_window_property(display, window, "_NET_WM_WINDOW_TYPE",
                            XA_ATOM, 32, &data) == 1) {
        Atom type;

        type = *(Atom *)data;
        XFree (data);

        return type;
    }

    return None;
}

static gboolean
filter_window_type(Display *display, Atom atom)
{
    unsigned int list_size;
    Atom filter_atom;

    static const char *blacklist[] = {
        "_NET_WM_WINDOW_TYPE_DESKTOP"
        };

    list_size = sizeof(blacklist)/sizeof(blacklist[0]);

    for (int i = 0; i < list_size; i++) {
        filter_atom = XInternAtom(display, blacklist[i], 0);

        if (filter_atom == atom)
            return FALSE;
    }

    return TRUE;
}

static void
get_geometry(Display *display, Window window,
             VDAgentSeamlessModeWindow *geometry)
{
    guchar *data = NULL;
    gulong *extents;
    gint x_abs, y_abs;
    Window root, child;
    guint border, depth;
    XWindowAttributes attributes;

    XGetGeometry(display, window, &root, &geometry->x, &geometry->y, &geometry->w, &geometry->h, &border, &depth);
    XTranslateCoordinates(display, window, root, -border, -border,
                           &x_abs, &y_abs, &child);
    XGetWindowAttributes(display, window, &attributes);

    /* Change relative to absolute mapping (e.g. gnome-terminal, firefox). */
    if (x_abs != geometry->x || y_abs != geometry->y) {
        geometry->x = x_abs - geometry->x + attributes.x;
        geometry->y = y_abs - geometry->y + attributes.y;
    }

    /* Remove WM border (e.g. gnome-terminal, firefox). */
    if (get_window_property(display, window, "_NET_FRAME_EXTENTS",
                            XA_CARDINAL, 32, &data) == 4) {
        extents = (gulong *)data; /* left, right, top, bottom */
        geometry->x -= extents[0];
        geometry->y -= extents[2];
        geometry->w += extents[0] + extents[1];
        geometry->h += extents[2] + extents[3];

        XFree(data);
    }

    /* Remove GTK border (client-side decorations). */
    if (get_window_property(display, window, "_GTK_FRAME_EXTENTS",
                            XA_CARDINAL, 32, &data) == 4) {
        extents = (gulong *)data; /* left, right, top, bottom */
        geometry->x += extents[0];
        geometry->y += extents[2];
        geometry->w -= extents[0] + extents[1];
        geometry->h -= extents[2] + extents[3];

        XFree(data);
    }
}

/* Determine whether window is visible. */
static gboolean
is_visible(Display *display, Window window)
{
    Atom type;
    XWindowAttributes attributes;

    /* Visible window must have window type specified. */
    type = get_window_type(display, window);
    if (type == None)
        return FALSE;

    if (!filter_window_type(display, type))
        return FALSE;

    /* Window must be viewable. */
    XGetWindowAttributes(display, window, &attributes);
    if (attributes.map_state != IsViewable)
       return FALSE;

    return TRUE;
}

/* Get list of visible windows. */
static GList *
get_window_list(struct vdagent_x11 *x11, Window window)
{
    Window root, parent;
    Window *list;
    guint n;
    GList *window_list = NULL;

    vdagent_x11_set_error_handler(x11, vdagent_x11_ignore_bad_window_handler);

    if (XQueryTree(x11->display, window, &root, &parent, &list, &n)) {
        guint i;
        for (i = 0; i < n; ++i) {
            VDAgentSeamlessModeWindow *spice_window;

            if (is_visible(x11->display, list[i])) {
                spice_window = g_new0(VDAgentSeamlessModeWindow, 1);
                get_geometry(x11->display, list[i], spice_window);

                if (vdagent_x11_restore_error_handler(x11) != 0) {
                    vdagent_x11_set_error_handler(x11,
                                                  vdagent_x11_ignore_bad_window_handler);
                    g_free(spice_window);
                    continue;
                }

                window_list = g_list_append(window_list, spice_window);
            }

            window_list = g_list_concat(window_list,
                                        get_window_list(x11, list[i]));
        }

        XFree(list);
    }

    vdagent_x11_restore_error_handler(x11);

    return window_list;
}

void
vdagent_x11_seamless_mode_send_list(struct vdagent_x11 *x11)
{
    VDAgentSeamlessModeList *list;
    GList *window_list, *l;
    size_t size;

    if (!x11->seamless_mode)
      return;

    // TODO: Check if it is neccesary to send the list...
    window_list = get_window_list(x11, DefaultRootWindow(x11->display));

    size = sizeof(VDAgentSeamlessModeList) +
           sizeof(VDAgentSeamlessModeWindow) * g_list_length(window_list);
    list = g_malloc0(size);

    for (l = window_list; l != NULL; l = l->next) {
        VDAgentSeamlessModeWindow *window;

        window = l->data;
        list->windows[list->num_of_windows].x = window->x;
        list->windows[list->num_of_windows].y = window->y;
        list->windows[list->num_of_windows].w = window->w;
        list->windows[list->num_of_windows].h = window->h;

        list->num_of_windows++;
    }

    g_list_free_full(window_list, (GDestroyNotify)g_free);

    udscs_write(x11->vdagentd, VDAGENTD_SEAMLESS_MODE_LIST, 0, 0,
                (uint8_t *)list, size);
}
