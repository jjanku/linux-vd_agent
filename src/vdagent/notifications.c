/*  notifications.c

    Copyright 2018 Red Hat, Inc.

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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <glib.h>
#include <gio/gio.h>
#include <syslog.h>
#include <string.h>

#include "notifications.h"
#include "vdagentd-proto.h"

#define TAG                 "vdagent-notifications: "

#define BUS_NAME_DBUS       "org.freedesktop.DBus"
#define OBJ_PATH_DBUS       "/org/freedesktop/DBus"

#define VARIANT_TYPE_NOTIFY "(susssasa{sv}i)"

#define MATCH_RULE          "type='method_call',"                    \
                            "path='/org/freedesktop/Notifications'," \
                            "member='Notify'"

struct VDAgentNotifications {
    struct udscs_connection *udscs;
    GDBusConnection         *dbus;
    GAsyncQueue             *queue;
};

static gboolean send_notification_to_client(gpointer user_data)
{
    VDAgentNotifications *n = user_data;
    GDBusMessage *message;
    GVariant *msg_body;
    const gchar *app_name, *summary, *body;
    gchar *buff, *ptr;
    guint size;

    message = g_async_queue_pop(n->queue);

    /* DEBUG only */
    gchar *msg_log = g_dbus_message_print(message, 0);
    g_message("New Notify message:\n%s", msg_log);
    g_free(msg_log);
    /* ---------- */

    msg_body = g_dbus_message_get_body(message);

    if (!g_variant_is_of_type(msg_body, G_VARIANT_TYPE(VARIANT_TYPE_NOTIFY))) {
        syslog(LOG_WARNING, TAG "unexpected notification format, skipping");
        goto skip;
    }

    /* for specification details, see
       https://people.gnome.org/~mccann/docs/notification-spec/notification-spec-latest.html  */
    g_variant_get(msg_body,
                  "(&sus&s&sasa{sv}i)",
                  &app_name, /* STRING app_name */
                  NULL,      /* UINT32 replaces_id */
                  NULL,      /* STRING app_icon */
                  &summary,  /* STRING summary */
                  &body,     /* STRING body */
                  NULL,      /* ARRAY  actions */
                  NULL,      /* DICT   hints */
                  NULL);     /* INT32  expire_timeout */

    size = strlen(app_name) + strlen(summary) + strlen(body) + 3;
    buff = g_malloc(size);

    ptr = g_stpcpy(buff, app_name) + 1;
    ptr = g_stpcpy(ptr, summary) + 1;
    g_stpcpy(ptr, body);

    udscs_write(n->udscs, VDAGENTD_GUEST_NOTIFICATION,
                0, 0, (guint8 *)buff, size);

    g_free(buff);
skip:
    g_object_unref(message);
    return G_SOURCE_REMOVE;
}

static GDBusMessage *notify_invoked_cb(GDBusConnection *connection,
                                       GDBusMessage    *message,
                                       gboolean         incoming,
                                       gpointer         user_data)
{
    VDAgentNotifications *n = user_data;
    /* this function is called in GDBusWorkerThread,
     * but udscs_write() must be called from main thread */
    g_async_queue_push(n->queue, message);
    g_idle_add(send_notification_to_client, n);

    return NULL;
}

static gboolean dbus_call_method(GDBusConnection *dbus,
                                 const gchar     *method,
                                 GVariant        *msg_body)
{
    GDBusMessage *message, *reply;
    GError *err = NULL;

    message = g_dbus_message_new_method_call(BUS_NAME_DBUS,
                  OBJ_PATH_DBUS, NULL, method);
    g_dbus_message_set_body(message, msg_body);

    reply = g_dbus_connection_send_message_with_reply_sync(dbus, message,
                G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err);

    g_object_unref(message);

    if (reply) {
        g_dbus_message_to_gerror(reply, &err);
        g_object_unref(reply);
    }
    if (err) {
        syslog(LOG_ERR, TAG "%s call failed: %s", method, err->message);
        g_error_free(err);
        return FALSE;
    }
    return TRUE;
}

static gboolean become_monitor(GDBusConnection *dbus)
{
    GVariantBuilder *array_builder;
    GVariant *msg_body;

    array_builder = g_variant_builder_new(G_VARIANT_TYPE("as"));
    g_variant_builder_add(array_builder, "s", MATCH_RULE);
    msg_body = g_variant_new("(asu)", array_builder, 0);
    g_variant_builder_unref(array_builder);

    return dbus_call_method(dbus, "BecomeMonitor", msg_body);
}

static gboolean add_match_rule(GDBusConnection *dbus,
                               const gchar     *rule)
{
    return dbus_call_method(dbus, "AddMatch", g_variant_new("(s)", rule));
}

static gboolean start_monitoring(GDBusConnection *dbus)
{
    /* see https://dbus.freedesktop.org/doc/dbus-specification.html#message-bus-routing-eavesdropping for details */
    if (become_monitor(dbus))
        return TRUE;
    syslog(LOG_DEBUG, TAG "BecomeMonitor failed, trying eavesdropping");
    if (add_match_rule(dbus, "eavesdrop='true'," MATCH_RULE))
        return TRUE;
    syslog(LOG_DEBUG, TAG "AddMatch with eavesdrop='true' failed, trying without");
    if (add_match_rule(dbus, MATCH_RULE))
        return TRUE;
    syslog(LOG_DEBUG, TAG "AddMatch failed");

    syslog(LOG_ERR, TAG "unable to monitor DBus messages");
    return FALSE;
}

GDBusConnection *dbus_connection_private_new()
{
    GDBusConnection *dbus;
    GError *err = NULL;
    gchar *addr;
    GDBusConnectionFlags flags =
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
        G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION;

    addr = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (err) {
        syslog(LOG_ERR, TAG "%s: %s", __func__, err->message);
        g_error_free(err);
        return NULL;
    }

    /* unlike g_bus_get_sync(), this always opens a new connection */
    dbus = g_dbus_connection_new_for_address_sync(addr, flags, NULL, NULL, &err);
    g_free(addr);
    if (err) {
        syslog(LOG_ERR, TAG "%s: %s", __func__, err->message);
        g_error_free(err);
    }
    return dbus;
}

VDAgentNotifications *vdagent_notifications_init(struct udscs_connection *udscs)
{
    VDAgentNotifications *n;

    n = g_new(VDAgentNotifications, 1);
    n->udscs = udscs;
    n->queue = g_async_queue_new_full(g_object_unref);
    n->dbus = dbus_connection_private_new();

    if (n->dbus == NULL || start_monitoring(n->dbus) == FALSE) {
        vdagent_notifications_finalize(n);
        return NULL;
    }

    g_dbus_connection_add_filter(n->dbus, notify_invoked_cb, n, NULL);
    return n;
}

void vdagent_notifications_finalize(VDAgentNotifications *n)
{
    g_return_if_fail(n != NULL);
    if (n->dbus) {
        g_dbus_connection_close_sync(n->dbus, NULL, NULL);
        g_object_unref(n->dbus);
    }
    /* remove any idle source that was set up in notify_invoked_cb() */
    while (g_source_remove_by_user_data(n))
        continue;
    g_async_queue_unref(n->queue);
    g_free(n);
}
