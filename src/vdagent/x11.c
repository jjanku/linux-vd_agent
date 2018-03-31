/*  vdagent-x11.c vdagent x11 code

    Copyright 2010-2011 Red Hat, Inc.

    Red Hat Authors:
    Hans de Goede <hdegoede@redhat.com>

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

/* Note: Our event loop is only called when there is data to be read from the
   X11 socket. If events have arrived and have already been read by libX11 from
   the socket triggered by other libX11 calls from this file, the select for
   read in the main loop, won't see these and our event loop won't get called!

   Thus we must make sure that all queued events have been consumed, whenever
   we return to the main loop. IOW all (externally callable) functions in this
   file must end with calling XPending and consuming all queued events.

   Calling XPending when-ever we return to the mainloop also ensures any
   pending writes are flushed. */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <glib.h>
#include <gdk/gdk.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <syslog.h>
#include <assert.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include "vdagentd-proto.h"
#include "x11.h"
#include "x11-priv.h"

/* Stupid X11 API, there goes our encapsulate all data in a struct design */
int (*vdagent_x11_prev_error_handler)(Display *, XErrorEvent *);
int vdagent_x11_caught_error;

static int vdagent_x11_debug_error_handler(
    Display *display, XErrorEvent *error)
{
    abort();
}

void vdagent_x11_set_error_handler(struct vdagent_x11 *x11,
    int (*handler)(Display *, XErrorEvent *))
{
    XSync(x11->display, False);
    vdagent_x11_caught_error = 0;
    vdagent_x11_prev_error_handler = XSetErrorHandler(handler);
}

int vdagent_x11_restore_error_handler(struct vdagent_x11 *x11)
{
    int error;

    XSync(x11->display, False);
    XSetErrorHandler(vdagent_x11_prev_error_handler);
    error = vdagent_x11_caught_error;
    vdagent_x11_caught_error = 0;

    return error;
}

static const gchar *vdagent_x11_get_wm_name()
{
#ifdef GDK_WINDOWING_X11
    GdkDisplay *display = gdk_display_get_default();
    if (GDK_IS_X11_DISPLAY(display))
        return gdk_x11_screen_get_window_manager_name(
            gdk_display_get_default_screen(display));
#endif
    return "unsupported";
}

struct vdagent_x11 *vdagent_x11_create(struct udscs_connection *vdagentd,
    int debug, int sync)
{
    struct vdagent_x11 *x11;
    XWindowAttributes attrib;
    int i;
    const gchar *net_wm_name;

    x11 = g_new0(struct vdagent_x11, 1);
    x11->vdagentd = vdagentd;
    x11->debug = debug;

    x11->display = XOpenDisplay(NULL);
    if (!x11->display) {
        syslog(LOG_ERR, "could not connect to X-server");
        g_free(x11);
        return NULL;
    }

    x11->screen_count = ScreenCount(x11->display);
    if (x11->screen_count > MAX_SCREENS) {
        syslog(LOG_ERR, "Error too much screens: %d > %d",
               x11->screen_count, MAX_SCREENS);
        XCloseDisplay(x11->display);
        g_free(x11);
        return NULL;
    }

    if (sync) {
        XSetErrorHandler(vdagent_x11_debug_error_handler);
        XSynchronize(x11->display, True);
    }

    for (i = 0; i < x11->screen_count; i++)
        x11->root_window[i] = RootWindow(x11->display, i);
    x11->fd = ConnectionNumber(x11->display);

    vdagent_x11_randr_init(x11);

    for (i = 0; i < x11->screen_count; i++) {
        /* Catch resolution changes */
        XSelectInput(x11->display, x11->root_window[i], StructureNotifyMask);

        /* Get the current resolution */
        XGetWindowAttributes(x11->display, x11->root_window[i], &attrib);
        x11->width[i]  = attrib.width;
        x11->height[i] = attrib.height;
    }
    vdagent_x11_send_daemon_guest_xorg_res(x11, 1);

    /* Since we are started at the same time as the wm,
       sometimes we need to wait a bit for the _NET_WM_NAME to show up. */
    for (i = 0; i < 9; i++) {
        net_wm_name = vdagent_x11_get_wm_name();
        if (strcmp(net_wm_name, "unknown"))
            break;
        usleep(100000);
    }
    if (x11->debug)
        syslog(LOG_DEBUG, "%s: net_wm_name=\"%s\", has icons=%d",
               __func__, net_wm_name, vdagent_x11_has_icons_on_desktop(x11));

    /* Flush output buffers and consume any pending events */
    vdagent_x11_do_read(x11);

    return x11;
}

void vdagent_x11_destroy(struct vdagent_x11 *x11, int vdagentd_disconnected)
{
    if (!x11)
        return;

    XCloseDisplay(x11->display);
    g_free(x11->randr.failed_conf);
    g_free(x11);
}

int vdagent_x11_get_fd(struct vdagent_x11 *x11)
{
    return x11->fd;
}

static void vdagent_x11_handle_event(struct vdagent_x11 *x11, XEvent event)
{
    int i, handled = 0;

    if (vdagent_x11_randr_handle_event(x11, event))
        return;

    switch (event.type) {
    case ConfigureNotify:
        for (i = 0; i < x11->screen_count; i++)
            if (event.xconfigure.window == x11->root_window[i])
                break;
        if (i == x11->screen_count)
            break;

        handled = 1;
        vdagent_x11_randr_handle_root_size_change(x11, i,
                event.xconfigure.width, event.xconfigure.height);
        break;
    case MappingNotify:
        /* These are uninteresting */
        handled = 1;
        break;
    }
    if (!handled && x11->debug)
        syslog(LOG_DEBUG, "unhandled x11 event, type %d, window %d",
               (int)event.type, (int)event.xany.window);
}

void vdagent_x11_do_read(struct vdagent_x11 *x11)
{
    XEvent event;

    while (XPending(x11->display)) {
        XNextEvent(x11->display, &event);
        vdagent_x11_handle_event(x11, event);
    }
}

/* Function used to determine the default location to save file-xfers,
   xdg desktop dir or xdg download dir. We err on the safe side and use a
   whitelist approach, so any unknown desktop will end up with saving
   file-xfers to the xdg download dir, and opening the xdg download dir with
   xdg-open when the file-xfer completes. */
int vdagent_x11_has_icons_on_desktop()
{
    const char * const wms_with_icons_on_desktop[] = {
        "Metacity", /* GNOME-2 or GNOME-3 fallback */
        "Xfwm4",    /* Xfce */
        "Marco",    /* Mate */
        "Metacity (Marco)", /* Mate, newer */
        NULL
    };
    const gchar *net_wm_name = vdagent_x11_get_wm_name();
    int i;

    for (i = 0; wms_with_icons_on_desktop[i]; i++)
        if (!strcmp(net_wm_name, wms_with_icons_on_desktop[i]))
            return 1;

    return 0;
}
