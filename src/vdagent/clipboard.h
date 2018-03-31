/*  clipboard.h - vdagent clipboard handling header

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

#ifndef __VDAGENT_CLIPBOARD_H
#define __VDAGENT_CLIPBOARD_H

#include <glib.h>

#include "udscs.h"

typedef struct VDAgentClipboards VDAgentClipboards;

VDAgentClipboards *vdagent_clipboards_init(struct udscs_connection *conn);
void vdagent_clipboards_finalize(VDAgentClipboards *c, gboolean conn_alive);

void vdagent_clipboard_request(VDAgentClipboards *c, guint sel_id, guint type);

void vdagent_clipboard_release(VDAgentClipboards *c, guint sel_id);

void vdagent_clipboards_release_all(VDAgentClipboards *c);

void vdagent_clipboard_data(VDAgentClipboards *c, guint sel_id,
                            guint type, guchar *data, guint size);

void vdagent_clipboard_grab(VDAgentClipboards *c, guint sel_id,
                            guint32 *types, guint n_types);

#endif
