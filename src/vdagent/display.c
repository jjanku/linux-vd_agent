/*  display.c vdagent display source code

 Copyright 2020 Red Hat, Inc.

 Red Hat Authors:
 Julien Ropé <jrope@redhat.com>

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

#include <config.h>

#include <glib.h>
#ifdef WITH_GTK
#include <gdk/gdk.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif
#endif
#include <syslog.h>
#include "x11.h"
#include "x11-priv.h"

#include "display.h"

/**
 * VDAgentDisplay and the vdagent_display_*() functions are used as wrappers for display-related
 * operations.
 * They allow vdagent code to call generic display functions that are independent from the underlying
 * API (X11/GTK/etc).
 *
 * The display.c file contains the actual implementation and chooses what API will be called.
 * The x11.c and x11-randr.c files contains the x11-specific functions.
 */
struct VDAgentDisplay {
    struct vdagent_x11 *x11;
    GIOChannel *x11_channel;
};


static gchar *vdagent_display_get_wm_name(VDAgentDisplay *display)
{
#ifdef GDK_WINDOWING_X11
    GdkDisplay *gdk_display = gdk_display_get_default();
    if (GDK_IS_X11_DISPLAY(gdk_display))
        return g_strdup(gdk_x11_screen_get_window_manager_name(
            gdk_display_get_default_screen(gdk_display)));
    return g_strdup("unsupported");
#else
    return vdagent_x11_get_wm_name(display->x11);
#endif
}


struct vdagent_x11* vdagent_display_get_x11(VDAgentDisplay *display)
{
    return display->x11;
}

static gboolean x11_io_channel_cb(GIOChannel *source, GIOCondition condition, gpointer data)
{
    VDAgentDisplay *display = data;
    vdagent_x11_do_read(display->x11);

    return G_SOURCE_CONTINUE;
}

VDAgentDisplay* vdagent_display_create(UdscsConnection *vdagentd, int debug, int sync)
{
    VDAgentDisplay *display;
    gchar *net_wm_name = NULL;

    display = g_new0(VDAgentDisplay, 1);
    display->x11 = vdagent_x11_create(vdagentd, debug, sync);
    if (display->x11 == NULL) {
        g_free(display);
        return NULL;
    }

    display->x11_channel = g_io_channel_unix_new(vdagent_x11_get_fd(display->x11));
    if (display->x11_channel == NULL) {
        vdagent_x11_destroy(display->x11, TRUE);
        g_free(display);
        return NULL;
    }

    g_io_add_watch(display->x11_channel, G_IO_IN, x11_io_channel_cb, display);


    /* Since we are started at the same time as the wm,
       sometimes we need to wait a bit for the _NET_WM_NAME to show up. */
    for (int i = 0; i < 9; i++) {
        g_free(net_wm_name);
        net_wm_name = vdagent_display_get_wm_name(display);
        if (strcmp(net_wm_name, "unknown"))
            break;
        usleep(100000);
    }
    if (display->x11->debug)
        syslog(LOG_DEBUG, "%s: net_wm_name=\"%s\", has icons=%d",
               __func__, net_wm_name, vdagent_display_has_icons_on_desktop(display));
    g_free(net_wm_name);

    return display;
}

void vdagent_display_destroy(VDAgentDisplay *display, int vdagentd_disconnected)
{
    if (!display) {
        return;
    }

    g_clear_pointer(&display->x11_channel, g_io_channel_unref);
    vdagent_x11_destroy(display->x11, vdagentd_disconnected);
}

/* Function used to determine the default location to save file-xfers,
   xdg desktop dir or xdg download dir. We err on the safe side and use a
   whitelist approach, so any unknown desktop will end up with saving
   file-xfers to the xdg download dir, and opening the xdg download dir with
   xdg-open when the file-xfer completes. */
gboolean vdagent_display_has_icons_on_desktop(VDAgentDisplay *display)
{
    static const char * const wms_with_icons_on_desktop[] = {
        "Metacity", /* GNOME-2 or GNOME-3 fallback */
        "Xfwm4",    /* Xfce */
        "Marco",    /* Mate */
        "Metacity (Marco)", /* Mate, newer */
        NULL
    };
    gchar *net_wm_name = vdagent_display_get_wm_name(display);
    int i;

    for (i = 0; wms_with_icons_on_desktop[i]; i++)
        if (!strcmp(net_wm_name, wms_with_icons_on_desktop[i])) {
            g_free(net_wm_name);
            return TRUE;
        }

    g_free(net_wm_name);
    return FALSE;
}

// handle the device info message from the server. This will allow us to
// maintain a mapping from spice display id to xrandr output
void vdagent_display_handle_graphics_device_info(VDAgentDisplay *display, uint8_t *data,
        size_t size)
{
    VDAgentGraphicsDeviceInfo *graphics_device_info = (VDAgentGraphicsDeviceInfo *)data;
    VDAgentDeviceDisplayInfo *device_display_info = graphics_device_info->display_info;

    void *buffer_end = data + size;

    syslog(LOG_INFO, "Received Graphics Device Info:");

    for (size_t i = 0; i < graphics_device_info->count; ++i) {
        if ((void*) device_display_info > buffer_end ||
                (void*) (&device_display_info->device_address +
                    device_display_info->device_address_len) > buffer_end) {
            syslog(LOG_ERR, "Malformed graphics_display_info message, "
                   "extends beyond the end of the buffer");
            break;
        }

        // make sure the string is terminated:
        if (device_display_info->device_address_len > 0) {
            device_display_info->device_address[device_display_info->device_address_len - 1] = '\0';
        } else {
            syslog(LOG_WARNING, "Zero length device_address received for channel_id: %u, monitor_id: %u",
                   device_display_info->channel_id, device_display_info->monitor_id);
        }

        vdagent_x11_handle_device_display_info(display->x11, device_display_info);

        device_display_info = (VDAgentDeviceDisplayInfo*) ((char*) device_display_info +
            sizeof(VDAgentDeviceDisplayInfo) + device_display_info->device_address_len);
    }

    // make sure daemon is up-to-date with (possibly updated) device IDs
    vdagent_x11_send_daemon_guest_xorg_res(display->x11, 1);
}

/*
 * Set monitor configuration according to client request.
 *
 * On exit send current configuration to client, regardless of error.
 *
 * Errors:
 *  screen size too large for driver to handle. (we set the largest/smallest possible)
 *  no randr support in X server.
 *  invalid configuration request from client.
 */
void vdagent_display_set_monitor_config(VDAgentDisplay *display, VDAgentMonitorsConfig *mon_config,
        int fallback)
{
    vdagent_x11_set_monitor_config(display->x11, mon_config, fallback);
}
