/*  log.c - log handling code

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

#include <syslog.h>
#include <glib.h>

static gint log_glib_level_to_priority(GLogLevelFlags level)
{
    if (level & G_LOG_LEVEL_ERROR)
        return LOG_ERR;
    if (level & G_LOG_LEVEL_CRITICAL)
        return LOG_ERR;
    if (level & G_LOG_LEVEL_WARNING)
        return LOG_WARNING;
    if (level & G_LOG_LEVEL_MESSAGE)
        return LOG_NOTICE;
    if (level & G_LOG_LEVEL_INFO)
        return LOG_INFO;
    if (level & G_LOG_LEVEL_DEBUG)
        return LOG_DEBUG;
    return LOG_NOTICE;
}

static void log_handler(const gchar   *log_domain,
                        GLogLevelFlags log_level,
                        const gchar   *message,
                        gpointer       user_data)
{
    gboolean debug = GPOINTER_TO_INT(user_data);
    gint priority = log_glib_level_to_priority(log_level);

    /* skip debug message when not requested */
    if (debug == FALSE && priority >= LOG_INFO)
        return;

    syslog(priority, "%s", message);
    g_log_default_handler(log_domain, log_level, message, NULL);
}

void vdagent_setup_log(const gchar *log_domain,
                       gboolean     with_pid,
                       gboolean     debug)
{
    gchar *debug_env;

    /* make sure debug and info messages
       get printed out by the default handler */
    if (debug) {
        debug_env = (gchar *)g_getenv("G_MESSAGES_DEBUG");
        if (debug_env == NULL) {
            g_setenv("G_MESSAGES_DEBUG", log_domain, FALSE);
        } else {
            debug_env = g_strconcat(debug_env, ",", log_domain, NULL);
            g_setenv("G_MESSAGES_DEBUG", debug_env, FALSE);
            g_free(debug_env);
        }
    }

    openlog(log_domain, with_pid ? LOG_PID : 0, LOG_USER);
    g_log_set_handler(log_domain, G_LOG_LEVEL_MASK, log_handler, GINT_TO_POINTER(debug));
}
