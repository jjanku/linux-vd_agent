/*  clipboard.c - vdagent clipboard handling code

    Copyright 2017 Red Hat, Inc.

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
# include <config.h>
#endif

#include <gtk/gtk.h>
#include <syslog.h>
#include <string.h>

#include "vdagentd-proto.h"
#include "vdagentd-proto-strings.h"
#include "spice/vd_agent.h"

#include "clipboard.h"

/* 2 selections supported - _SELECTION_CLIPBOARD = 0, _SELECTION_PRIMARY = 1 */
#define SELECTION_COUNT (VD_AGENT_CLIPBOARD_SELECTION_PRIMARY + 1)
#define TYPE_COUNT      (VD_AGENT_CLIPBOARD_IMAGE_JPG + 1)

#define sel_id_from_clip(clipboard) \
    GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(clipboard), "vdagent-selection-id"))

enum {
    OWNER_NONE,
    OWNER_GUEST,
    OWNER_CLIENT
};

typedef struct {
    GMainLoop        *loop;
    GtkSelectionData *sel_data;
} AppRequest;

typedef struct {
    GtkClipboard *clipboard;
    guint         owner;

    GList        *requests_from_apps; /* VDAgent --> Client */
    GList        *requests_from_client; /* Client --> VDAgent */
    gpointer     *last_targets_req;

    GdkAtom       targets[TYPE_COUNT];
} Selection;

struct VDAgentClipboards {
    struct udscs_connection *conn;
    Selection                selections[SELECTION_COUNT];
    gboolean                 use_extended_selection;
};

static const struct {
    guint         type;
    const gchar  *atom_name;
} atom2agent[] = {
    {VD_AGENT_CLIPBOARD_UTF8_TEXT, "UTF8_STRING"},
    {VD_AGENT_CLIPBOARD_UTF8_TEXT, "text/plain;charset=utf-8"},
    {VD_AGENT_CLIPBOARD_UTF8_TEXT, "STRING"},
    {VD_AGENT_CLIPBOARD_UTF8_TEXT, "TEXT"},
    {VD_AGENT_CLIPBOARD_UTF8_TEXT, "text/plain"},
    {VD_AGENT_CLIPBOARD_IMAGE_PNG, "image/png"},
    {VD_AGENT_CLIPBOARD_IMAGE_BMP, "image/bmp"},
    {VD_AGENT_CLIPBOARD_IMAGE_BMP, "image/x-bmp"},
    {VD_AGENT_CLIPBOARD_IMAGE_BMP, "image/x-MS-bmp"},
    {VD_AGENT_CLIPBOARD_IMAGE_BMP, "image/x-win-bitmap"},
    {VD_AGENT_CLIPBOARD_IMAGE_TIFF,"image/tiff"},
    {VD_AGENT_CLIPBOARD_IMAGE_JPG, "image/jpeg"},
};

static guint get_type_from_atom(GdkAtom atom)
{
    gchar *name = gdk_atom_name(atom);
    int i;
    for (i = 0; i < G_N_ELEMENTS(atom2agent); i++) {
        if (!g_ascii_strcasecmp(name, atom2agent[i].atom_name)) {
            g_free(name);
            return atom2agent[i].type;
        }
    }
    g_free(name);
    return VD_AGENT_CLIPBOARD_NONE;
}

static gboolean filter_target(const gchar *target)
{
    /* FIXME: exclude anything else? */
    const gchar * const exclude[] = {
        "TARGETS",
        "SAVE_TARGETS",
        "AVAILABLE_TARGETS",
        "REQUESTED_TARGETS",
        "TIMESTAMP",
        "MULTIPLE",
        NULL
    };
    guint i;

    for (i = 0; exclude[i]; i++)
        if (!g_ascii_strcasecmp(target, exclude[i]))
            return TRUE;
    return FALSE;
}

static gboolean send_grab(VDAgentClipboards *c, guint sel_id,
                          GdkAtom *atoms, gint n_atoms)
{
    if (atoms == NULL)
        return FALSE;

    if (c->use_extended_selection)
    {
        gchar **names, *data, *ptr;
        gsize len;
        guint a;

        names = g_new(gchar *, n_atoms);
        len = 0;
        for (a = 0; a < n_atoms; a++) {
            names[a] = gdk_atom_name(atoms[a]);
            if (filter_target(names[a]))
                g_clear_pointer(&names[a], g_free);
            else
                len += strlen(names[a]) + 1;
        }
        if (len == 0) {
            g_free(names);
            return FALSE;
        }

        data = g_malloc(len);
        for (a = 0, ptr = data; a < n_atoms; a++)
            if (names[a])
                ptr = g_stpcpy(ptr, names[a]) + 1;

        udscs_write(c->conn, VDAGENTD_SELECTION_GRAB, sel_id, 0,
                    (guint8 *)data, len);

        for (a = 0; a < n_atoms; a++)
            g_free(names[a]);
        g_free(names);
        g_free(data);
    }
    else /* c->use_extended_selection = FALSE */
    {
        Selection *sel = &c->selections[sel_id];
        guint32 types[G_N_ELEMENTS(atom2agent)];
        guint type, n_types, a;

        for (type = 0; type < TYPE_COUNT; type++)
            sel->targets[type] = GDK_NONE;

        n_types = 0;
        for (a = 0; a < n_atoms; a++) {
            type = get_type_from_atom(atoms[a]);
            if (type == VD_AGENT_CLIPBOARD_NONE || sel->targets[type] != GDK_NONE)
                continue;

            sel->targets[type] = atoms[a];
            types[n_types] = type;
            n_types++;
        }

        if (n_types == 0) {
            syslog(LOG_WARNING, "%s: sel_id=%u: no target supported", __func__, sel_id);
            return FALSE;
        }

        udscs_write(c->conn, VDAGENTD_CLIPBOARD_GRAB, sel_id, 0,
                    (guint8 *)types, n_types * sizeof(guint32));
    }

    return TRUE;
}

static gboolean send_request(VDAgentClipboards *c, guint sel_id, GdkAtom target)
{
    if (c->use_extended_selection)
    {
        gchar *target_name = gdk_atom_name(target);
        udscs_write(c->conn, VDAGENTD_SELECTION_REQUEST, sel_id, 0,
                    (guint8 *)target_name, strlen(target_name) + 1);
        g_free(target_name);
    }
    else /* c->use_extended_selection = FALSE */
    {
        guint type = get_type_from_atom(target);
        g_return_val_if_fail(type != VD_AGENT_CLIPBOARD_NONE, FALSE);
        udscs_write(c->conn, VDAGENTD_CLIPBOARD_REQUEST, sel_id, type, NULL, 0);
    }

    return TRUE;
}

static void send_data(VDAgentClipboards *c, guint sel_id,
                      GdkAtom type, gint format,
                      const guchar *data, gint data_len)
{
    if (c->conn == NULL)
        return;

    if (c->use_extended_selection)
    {
        gchar *type_name = gdk_atom_name(type);
        gsize type_len = strlen(type_name) + 1;
        guchar *buff = g_malloc(type_len + data_len);

        memcpy(buff, type_name, type_len);
        memcpy(buff + type_len, data, data_len);

        udscs_write(c->conn, VDAGENTD_SELECTION_DATA, sel_id,
                    format, buff, type_len + data_len);

        g_free(buff);
        g_free(type_name);
    }
    else /* c->use_extended_selection = FALSE */
    {
        udscs_write(c->conn, VDAGENTD_CLIPBOARD_DATA, sel_id,
                    get_type_from_atom(type), data, data_len);
    }
}

static void send_release(VDAgentClipboards *c, guint sel_id)
{
    if (c->conn == NULL)
        return;

    if (c->use_extended_selection)
    {
        udscs_write(c->conn, VDAGENTD_SELECTION_RELEASE, sel_id, 0, NULL, 0);
    }
    else /* c->use_extended_selection = FALSE */
    {
        udscs_write(c->conn, VDAGENTD_CLIPBOARD_RELEASE, sel_id, 0, NULL, 0);
    }
}

/* gtk_clipboard_request_(, callback, user_data) cannot be cancelled.
   Instead, gpointer *ref = request_ref_new() is passed to the callback.
   Callback can check using request_ref_is_cancelled(ref)
   whether request_ref_cancel(ref) was called.
   This mechanism enables cancellation of the request
   as well as passing VDAgentClipboards reference to the desired callback.
 */
static gpointer *request_ref_new(gpointer data)
{
    gpointer *ref = g_new(gpointer, 1);
    *ref = data;
    return ref;
}

static gpointer request_ref_free(gpointer *ref)
{
    gpointer data = *ref;
    g_free(ref);
    return data;
}

static void request_ref_cancel(gpointer *ref)
{
    g_return_if_fail(ref != NULL);
    *ref = NULL;
}

static gboolean request_ref_is_cancelled(gpointer *ref)
{
    g_return_val_if_fail(ref != NULL, TRUE);
    return *ref == NULL;
}

static void clipboard_new_owner(VDAgentClipboards *c, guint sel_id, guint new_owner)
{
    Selection *sel = &c->selections[sel_id];
    GList *l;
    /* let the other apps know no data is coming */
    for (l = sel->requests_from_apps; l != NULL; l= l->next) {
        AppRequest *req = l->data;
        g_main_loop_quit(req->loop);
    }
    g_clear_pointer(&sel->requests_from_apps, g_list_free);

    /* respond to pending client's data requests */
    for (l = sel->requests_from_client; l != NULL; l = l->next) {
        request_ref_cancel(l->data);
        send_data(c, sel_id, GDK_NONE, 8, NULL, 0);
    }
    g_clear_pointer(&sel->requests_from_client, g_list_free);

    sel->owner = new_owner;
}

static void clipboard_targets_received_cb(GtkClipboard *clipboard,
                                          GdkAtom      *atoms,
                                          gint          n_atoms,
                                          gpointer      user_data)
{
    if (request_ref_is_cancelled(user_data))
        return;

    VDAgentClipboards *c = request_ref_free(user_data);
    guint sel_id;

    sel_id = sel_id_from_clip(clipboard);
    c->selections[sel_id].last_targets_req = NULL;

    if (send_grab(c, sel_id, atoms, n_atoms))
        clipboard_new_owner(c, sel_id, OWNER_GUEST);
}

static void clipboard_owner_change_cb(GtkClipboard        *clipboard,
                                      GdkEventOwnerChange *event,
                                      gpointer             user_data)
{
    VDAgentClipboards *c = user_data;
    guint sel_id = sel_id_from_clip(clipboard);
    Selection *sel = &c->selections[sel_id];

    /* if the event was caused by gtk_clipboard_set_with_data(), ignore it  */
    if (sel->owner == OWNER_CLIENT)
        return;

    if (sel->owner == OWNER_GUEST) {
        clipboard_new_owner(c, sel_id, OWNER_NONE);
        send_release(c, sel_id);
    }

    if (event->reason != GDK_OWNER_CHANGE_NEW_OWNER)
        return;

    /* if there's a pending request for clipboard targets, cancel it */
    if (sel->last_targets_req)
        request_ref_cancel(sel->last_targets_req);

    sel->last_targets_req = request_ref_new(c);
    gtk_clipboard_request_targets(clipboard, clipboard_targets_received_cb,
                                  sel->last_targets_req);
}

static void clipboard_contents_received_cb(GtkClipboard     *clipboard,
                                           GtkSelectionData *sel_data,
                                           gpointer          user_data)
{
    if (request_ref_is_cancelled(user_data))
        return;

    VDAgentClipboards *c = request_ref_free(user_data);
    guint sel_id;
    GdkAtom target, type;

    sel_id = sel_id_from_clip(clipboard);
    c->selections[sel_id].requests_from_client =
        g_list_remove(c->selections[sel_id].requests_from_client, user_data);

    target = gtk_selection_data_get_target(sel_data);
    type   = gtk_selection_data_get_data_type(sel_data);
    if (target == type) {
        send_data(c, sel_id, type,
                  gtk_selection_data_get_format(sel_data),
                  gtk_selection_data_get_data(sel_data),
                  gtk_selection_data_get_length(sel_data));
    } else {
        gchar *target_str, *type_str;
        target_str = gdk_atom_name(target);
        type_str   = gdk_atom_name(type);
        syslog(LOG_WARNING, "%s: sel_id=%u: expected type %s, recieved %s, "
                            "skipping", __func__, sel_id, target_str, type_str);
        g_free(target_str);
        g_free(type_str);
        send_data(c, sel_id, GDK_NONE, 8, NULL, 0);
    }
}

static void clipboard_get_cb(GtkClipboard     *clipboard,
                             GtkSelectionData *sel_data,
                             guint             info,
                             gpointer          user_data)
{
    AppRequest req;
    VDAgentClipboards *c = user_data;
    guint sel_id;

    sel_id = sel_id_from_clip(clipboard);
    g_return_if_fail(c->selections[sel_id].owner == OWNER_CLIENT);

    if (!send_request(c, sel_id, gtk_selection_data_get_target(sel_data)))
        return;

    req.sel_data = sel_data;
    req.loop = g_main_loop_new(NULL, FALSE);
    c->selections[sel_id].requests_from_apps =
        g_list_prepend(c->selections[sel_id].requests_from_apps, &req);

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gdk_threads_leave();
    g_main_loop_run(req.loop);
    gdk_threads_enter();
G_GNUC_END_IGNORE_DEPRECATIONS

    g_main_loop_unref(req.loop);
}

static void clipboard_clear_cb(GtkClipboard *clipboard, gpointer user_data)
{
    VDAgentClipboards *c = user_data;
    clipboard_new_owner(c, sel_id_from_clip(clipboard), OWNER_NONE);
}

void clipboard_grab(VDAgentClipboards *c, guint sel_id,
                    GtkTargetEntry *targets, guint n_targets)
{
    if (gtk_clipboard_set_with_data(c->selections[sel_id].clipboard,
                                    targets, n_targets,
                                    clipboard_get_cb, clipboard_clear_cb, c))
        clipboard_new_owner(c, sel_id, OWNER_CLIENT);
    else {
        syslog(LOG_ERR, "%s: sel_id=%u: clipboard grab failed", __func__, sel_id);
        clipboard_new_owner(c, sel_id, OWNER_NONE);
    }
}

void vdagent_clipboard_grab(VDAgentClipboards *c, guint sel_id,
                            guint32 *types, guint n_types)
{
    GtkTargetEntry targets[G_N_ELEMENTS(atom2agent)];
    guint n_targets, i, t;

    g_return_if_fail(sel_id < SELECTION_COUNT);

    n_targets = 0;
    for (i = 0; i < G_N_ELEMENTS(atom2agent); i++)
        for (t = 0; t < n_types; t++)
            if (atom2agent[i].type == types[t]) {
                targets[n_targets].target = (gchar *)atom2agent[i].atom_name;
                n_targets++;
                break;
            }

    if (n_targets == 0) {
        syslog(LOG_WARNING, "%s: sel_id=%u: no type supported", __func__, sel_id);
        return;
    }

    clipboard_grab(c, sel_id, targets, n_targets);
}

void vdagent_selection_grab(VDAgentClipboards *c, guint sel_id,
                            const gchar *data, guint size)
{
    GtkTargetEntry *targets;
    GQueue *q;
    gchar *p;
    guint n;

    g_return_if_fail(sel_id < SELECTION_COUNT);

    g_return_if_fail(data != NULL && size > 0);
    g_return_if_fail(data[size-1] == 0);

    q = g_queue_new();
    g_queue_push_tail(q, (gchar *)data);
    for (p = (gchar *)data; p < data+size-1; p++)
        if (*p == '\0')
            g_queue_push_tail(q, p+1);

    targets = g_new0(GtkTargetEntry, q->length);
    for (n = 0; !g_queue_is_empty(q); n++)
        targets[n].target = g_queue_pop_head(q);

    clipboard_grab(c, sel_id, targets, n);
    g_queue_free(q);
    g_free(targets);
}

void selection_data_set(VDAgentClipboards *c, guint sel_id,
                        GdkAtom type, gint type_vdagent,
                        gint format, const guchar *data, guint size)
{
    g_return_if_fail(sel_id < SELECTION_COUNT);
    Selection *sel = &c->selections[sel_id];
    AppRequest *req;
    GList *l;

    for (l = sel->requests_from_apps; l != NULL; l = l->next) {
        req = l->data;
        GdkAtom target = gtk_selection_data_get_target(req->sel_data);
        if (target == type || get_type_from_atom(target) == type_vdagent)
            break;
    }
    if (l == NULL) {
        gchar *type_name = gdk_atom_name(type);
        syslog(LOG_WARNING, "%s: sel_id=%u: no corresponding request found for "
                            "type=%s, type_vdagent=%u, skipping",
                            __func__, sel_id, type_name, type_vdagent);
        g_free(type_name);
        return;
    }
    sel->requests_from_apps = g_list_delete_link(sel->requests_from_apps, l);

    gtk_selection_data_set(req->sel_data,
                           gtk_selection_data_get_target(req->sel_data),
                           format, data, size);

    g_main_loop_quit(req->loop);
}

void vdagent_clipboard_data(VDAgentClipboards *c, guint sel_id,
                            guint type, guchar *data, guint size)
{
    selection_data_set(c, sel_id, GDK_NONE, type, 8, data, size);
}

void vdagent_selection_data(VDAgentClipboards *c, guint sel_id,
                            guint format, const guchar *data, guint size)
{
    GdkAtom type;
    guint offset;

    offset = strnlen((gchar *)data, size) + 1;
    g_return_if_fail(offset <= size);

    type = gdk_atom_intern((gchar *)data, FALSE);
    selection_data_set(c, sel_id, type, TYPE_COUNT, format,
                       data + offset, size - offset);
}

void vdagent_clipboard_release(VDAgentClipboards *c, guint sel_id)
{
    g_return_if_fail(sel_id < SELECTION_COUNT);
    if (c->selections[sel_id].owner != OWNER_CLIENT)
        return;

    clipboard_new_owner(c, sel_id, OWNER_NONE);
    gtk_clipboard_clear(c->selections[sel_id].clipboard);
}

void vdagent_selection_release(VDAgentClipboards *c, guint sel_id)
{
    vdagent_clipboard_release(c, sel_id);
}

void vdagent_clipboards_release_all(VDAgentClipboards *c)
{
    guint sel_id, owner;

    for (sel_id = 0; sel_id < SELECTION_COUNT; sel_id++) {
        owner = c->selections[sel_id].owner;
        clipboard_new_owner(c, sel_id, OWNER_NONE);
        if (owner == OWNER_CLIENT)
            gtk_clipboard_clear(c->selections[sel_id].clipboard);
        else if (owner == OWNER_GUEST)
            send_release(c, sel_id);
    }
}

static void clipboard_request(VDAgentClipboards *c, guint sel_id, GdkAtom target)
{
    Selection *sel;

    if (sel_id >= SELECTION_COUNT)
        goto err;
    sel = &c->selections[sel_id];
    if (sel->owner != OWNER_GUEST) {
        syslog(LOG_WARNING, "%s: sel_id=%d: received request "
                            "while not owning clipboard", __func__, sel_id);
        goto err;
    }
    if (target == GDK_NONE) {
        syslog(LOG_WARNING, "%s: sel_id=%d: invalid data type requested",
                            __func__, sel_id);
        goto err;
    }

    gpointer *ref = request_ref_new(c);
    sel->requests_from_client = g_list_prepend(sel->requests_from_client, ref);
    gtk_clipboard_request_contents(sel->clipboard, target,
                                   clipboard_contents_received_cb, ref);
    return;
err:
    send_data(c, sel_id, GDK_NONE, 8, NULL, 0);
}

void vdagent_clipboard_request(VDAgentClipboards *c, guint sel_id, guint type)
{
    GdkAtom target;
    if (sel_id < SELECTION_COUNT && type < TYPE_COUNT)
        target = c->selections[sel_id].targets[type];
    else
        target = GDK_NONE;
    clipboard_request(c, sel_id, target);
}

void vdagent_selection_request(VDAgentClipboards *c, guint sel_id,
                               const gchar *target_str, guint size)
{
    GdkAtom target;
    if (strnlen(target_str, size) == size - 1)
        target = gdk_atom_intern(target_str, FALSE);
    else
        target = GDK_NONE;
    clipboard_request(c, sel_id, target);
}

void vdagent_clipboards_set_protocol(VDAgentClipboards *c, guint protocol)
{
    g_return_if_fail(protocol <= CLIPBOARD_PROTOCOL_SELECTION);
    c->use_extended_selection = protocol == CLIPBOARD_PROTOCOL_SELECTION;
    syslog(LOG_DEBUG, "Clipboard protocol set to %s",
                      vdagentd_clipboard_protocols[protocol]);
}

VDAgentClipboards *vdagent_clipboards_init(struct udscs_connection *conn)
{
    guint sel_id;
    const GdkAtom sel_atom[SELECTION_COUNT] = {
        GDK_SELECTION_CLIPBOARD, /* VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD */
        GDK_SELECTION_PRIMARY,   /* VD_AGENT_CLIPBOARD_SELECTION_PRIMARY */
    };

    VDAgentClipboards *c;
    c = g_new0(VDAgentClipboards, 1);
    c->conn = conn;
    c->use_extended_selection = FALSE;

    for (sel_id = 0; sel_id < SELECTION_COUNT; sel_id++) {
        GtkClipboard *clipboard = gtk_clipboard_get(sel_atom[sel_id]);
        c->selections[sel_id].clipboard = clipboard;
        /* enables the use of sel_id_from_clipboard(clipboard) macro */
        g_object_set_data(G_OBJECT(clipboard), "vdagent-selection-id",
                          GUINT_TO_POINTER(sel_id));
        g_signal_connect(G_OBJECT(clipboard), "owner-change",
                         G_CALLBACK(clipboard_owner_change_cb), c);
    }

    return c;
}

void vdagent_clipboards_finalize(VDAgentClipboards *c, gboolean conn_alive)
{
    guint sel_id;
    for (sel_id = 0; sel_id < SELECTION_COUNT; sel_id++)
        g_signal_handlers_disconnect_by_func(c->selections[sel_id].clipboard,
            G_CALLBACK(clipboard_owner_change_cb), c);

    if (conn_alive == FALSE)
        c->conn = NULL;
    vdagent_clipboards_release_all(c);

    g_free(c);
}
