/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2004-2005 William Jon McCann <mccann@jhu.edu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Authors: William Jon McCann <mccann@jhu.edu>
 *
 */

#include "config.h"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <string.h>

#include <glib/gi18n.h>

#define DBUS_API_SUBJECT_TO_CHANGE
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>

#include "gs-listener-dbus.h"
#include "gs-marshal.h"
#include "gs-debug.h"

/* this is for dbus < 0.3 */
#if ((DBUS_VERSION_MAJOR == 0) && (DBUS_VERSION_MINOR < 30))
#define dbus_bus_name_has_owner(connection, name, err)      dbus_bus_service_exists(connection, name, err)
#define dbus_bus_request_name(connection, name, flags, err) dbus_bus_acquire_service(connection, name, flags, err)
#endif

static void              gs_listener_class_init         (GSListenerClass *klass);
static void              gs_listener_init               (GSListener      *listener);
static void              gs_listener_finalize           (GObject         *object);

static void              gs_listener_unregister_handler (DBusConnection  *connection,
                                                         void            *data);

static DBusHandlerResult gs_listener_message_handler    (DBusConnection  *connection,
                                                         DBusMessage     *message,
                                                         void            *user_data);

#define GS_LISTENER_SERVICE   "org.gnome.ScreenSaver"
#define GS_LISTENER_PATH      "/org/gnome/ScreenSaver"
#define GS_LISTENER_INTERFACE "org.gnome.ScreenSaver"

#define TYPE_MISMATCH_ERROR GS_LISTENER_INTERFACE ".TypeMismatch"

#define GS_LISTENER_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), GS_TYPE_LISTENER, GSListenerPrivate))

struct GSListenerPrivate
{
        DBusConnection *connection;

        guint           session_idle : 1;
        guint           active : 1;
        guint           activation_enabled : 1;
        guint           throttled : 1;
        GHashTable     *inhibitors;
        GHashTable     *throttlers;

        time_t          active_start;
        time_t          session_idle_start;
};

typedef struct
{
        int      entry_type;
        char    *application;
        char    *reason;
        char    *connection;
        guint32  cookie;
} GSListenerRefEntry;

enum {
        LOCK,
        CYCLE,
        QUIT,
        POKE,
        ACTIVE_CHANGED,
        THROTTLE_CHANGED,
        LAST_SIGNAL
};

enum {
        PROP_0,
        PROP_ACTIVE,
        PROP_SESSION_IDLE,
        PROP_ACTIVATION_ENABLED,
};

enum {
        REF_ENTRY_TYPE_INHIBIT,
        REF_ENTRY_TYPE_THROTTLE
};

static DBusObjectPathVTable
gs_listener_vtable = { &gs_listener_unregister_handler,
                       &gs_listener_message_handler,
                       NULL,
                       NULL,
                       NULL,
                       NULL };

static GObjectClass *parent_class = NULL;
static guint         signals [LAST_SIGNAL] = { 0, };

G_DEFINE_TYPE (GSListener, gs_listener, G_TYPE_OBJECT)

GQuark
gs_listener_error_quark (void)
{
        static GQuark quark = 0;
        if (!quark)
                quark = g_quark_from_static_string ("gs_listener_error");

        return quark;
}

static void
gs_listener_ref_entry_free (GSListenerRefEntry *entry)
{
	g_free (entry->connection);
	g_free (entry->application);
	g_free (entry->reason);
	g_free (entry);
	entry = NULL;
}

static void
gs_listener_unregister_handler (DBusConnection *connection,
                                void           *data)
{
}

static gboolean
send_dbus_message (DBusConnection *connection,
                   DBusMessage    *message)
{
        gboolean is_connected;
        gboolean sent;

        g_return_val_if_fail (message != NULL, FALSE);

        if (! connection) {
                g_warning ("There is no valid connection to the message bus");
                return FALSE;
        }

        is_connected = dbus_connection_get_is_connected (connection);
        if (! is_connected) {
                g_warning ("Not connected to the message bus");
                return FALSE;
        }

        sent = dbus_connection_send (connection, message, NULL);

        return sent;
}

static void
gs_listener_send_signal_active_changed (GSListener *listener)
{
        DBusMessage    *message;
        DBusMessageIter iter;
        dbus_bool_t     active;

        g_return_if_fail (listener != NULL);

        message = dbus_message_new_signal (GS_LISTENER_PATH,
                                           GS_LISTENER_SERVICE,
                                           "ActiveChanged");

        active = listener->priv->active;
        dbus_message_iter_init_append (message, &iter);
        dbus_message_iter_append_basic (&iter, DBUS_TYPE_BOOLEAN, &active);

        if (! send_dbus_message (listener->priv->connection, message)) {
                g_warning ("Could not send ActiveChanged signal");
        }

        dbus_message_unref (message);
}

static void
gs_listener_send_signal_session_idle_changed (GSListener *listener)
{
        DBusMessage    *message;
        DBusMessageIter iter;
        dbus_bool_t     idle;

        g_return_if_fail (listener != NULL);

        message = dbus_message_new_signal (GS_LISTENER_PATH,
                                           GS_LISTENER_SERVICE,
                                           "SessionIdleChanged");

        idle = listener->priv->session_idle;
        dbus_message_iter_init_append (message, &iter);
        dbus_message_iter_append_basic (&iter, DBUS_TYPE_BOOLEAN, &idle);

        if (! send_dbus_message (listener->priv->connection, message)) {
                g_warning ("Could not send SessionIdleChanged signal");
        }

        dbus_message_unref (message);
}

static const char *
get_name_for_entry_type (int entry_type)
{
        const char *name;

        switch (entry_type) {
        case REF_ENTRY_TYPE_INHIBIT:
                name = "inhibitor";
                break;
        case REF_ENTRY_TYPE_THROTTLE:
                name = "throttler";
                break;
        default:
                g_assert_not_reached ();
                break;
        }

        return name;
}

static GHashTable *
get_hash_for_entry_type (GSListener         *listener,
                         int                 entry_type)
{
        GHashTable *hash;

        switch (entry_type) {
        case REF_ENTRY_TYPE_INHIBIT:
                hash = listener->priv->inhibitors;
                break;
        case REF_ENTRY_TYPE_THROTTLE:
                hash = listener->priv->throttlers;
                break;
        default:
                g_assert_not_reached ();
                break;
        }

        return hash;
}

static void
list_ref_entry (gpointer key,
                gpointer value,
                gpointer user_data)
{
        GSListenerRefEntry *entry;

        entry =  (GSListenerRefEntry *)value;

        gs_debug ("%s: %s for reason: %s",
                  get_name_for_entry_type (entry->entry_type),
                  entry->application,
                  entry->reason);
}

static gboolean
listener_ref_entry_is_present (GSListener *listener,
                               int         entry_type)
{
        guint       n_entries;
        gboolean    is_set;
        GHashTable *hash;

        hash = get_hash_for_entry_type (listener, entry_type);

        /* if we aren't inhibited then activate */
        n_entries = 0;
        if (hash != NULL) {
                n_entries = g_hash_table_size (hash);

                g_hash_table_foreach (hash, list_ref_entry, NULL);
        }

        is_set = (n_entries > 0);

        return is_set;
}

static gboolean
listener_check_activation (GSListener *listener)
{
        gboolean inhibited;
        gboolean res;

        gs_debug ("Checking for activation");

        if (! listener->priv->activation_enabled) {
                return TRUE;
        }

        if (! listener->priv->session_idle) {
                return TRUE;
        }

        /* if we aren't inhibited then activate */
        inhibited = listener_ref_entry_is_present (listener, REF_ENTRY_TYPE_INHIBIT);

        res = FALSE;
        if (! inhibited) {
                gs_debug ("Trying to activate");
                res = gs_listener_set_active (listener, TRUE);
        }

        return res;
}

static void
gs_listener_set_throttle (GSListener *listener,
                          gboolean    throttled)
{
        g_return_if_fail (GS_IS_LISTENER (listener));

        if (listener->priv->throttled != throttled) {
                gs_debug ("Changine throttle status: %d", throttled);

                listener->priv->throttled = throttled;

                g_signal_emit (listener, signals [THROTTLE_CHANGED], 0, throttled);
        }
}

static gboolean
listener_check_throttle (GSListener *listener)
{
        gboolean throttled;

        gs_debug ("Checking for throttle");

        throttled = listener_ref_entry_is_present (listener, REF_ENTRY_TYPE_THROTTLE);

        if (throttled != listener->priv->throttled) {
                gs_listener_set_throttle (listener, throttled);
        }

        return TRUE;
}

static gboolean
listener_set_session_idle_internal (GSListener *listener,
                                    gboolean    idle)
{
        listener->priv->session_idle = idle;

        if (idle) {
                listener->priv->session_idle_start = time (NULL);
        } else {
                listener->priv->session_idle_start = 0;
        }

        gs_listener_send_signal_session_idle_changed (listener);

        return TRUE;
}

static gboolean
listener_set_active_internal (GSListener *listener,
                              gboolean    active)
{
        listener->priv->active = active;

        /* if idle not in sync with active, change it */
        if (listener->priv->session_idle != active) {
                listener_set_session_idle_internal (listener, active);
        }

        if (active) {
                listener->priv->active_start = time (NULL);
        } else {
                listener->priv->active_start = 0;
        }

        gs_listener_send_signal_active_changed (listener);

        return TRUE;
}

gboolean
gs_listener_set_active (GSListener *listener,
                        gboolean    active)
{
        gboolean res;

        g_return_val_if_fail (GS_IS_LISTENER (listener), FALSE);

        if (listener->priv->active == active) {
                gs_debug ("Trying to set active state when already: %s",
                          active ? "active" : "inactive");
                return FALSE;
        }

        res = FALSE;
        g_signal_emit (listener, signals [ACTIVE_CHANGED], 0, active, &res);
        if (! res) {
                /* if the signal is not handled then we haven't changed state */
                gs_debug ("Active-changed signal not handled");

                /* clear the idle state */
                if (active) {
                        listener_set_session_idle_internal (listener, FALSE);
                }

                return FALSE;
        }

        listener_set_active_internal (listener, active);

        return TRUE;
}

gboolean
gs_listener_set_session_idle (GSListener *listener,
                              gboolean    idle)
{
        gboolean res;

        g_return_val_if_fail (GS_IS_LISTENER (listener), FALSE);

        gs_debug ("Setting session idle: %d", idle);

        if (listener->priv->session_idle == idle) {
                gs_debug ("Trying to set idle state when already %s",
                          idle ? "idle" : "not idle");
                return FALSE;
        }

        if (idle) {
                gboolean inhibited;

                inhibited = listener_ref_entry_is_present (listener, REF_ENTRY_TYPE_INHIBIT);

                /* if we are inhibited then do nothing */
                if (inhibited) {
                        return FALSE;
                }
        }

        listener->priv->session_idle = idle;
        res = listener_check_activation (listener);

        /* if activation fails then don't set idle */
        if (res) {
                listener_set_session_idle_internal (listener, idle);
        } else {
                gs_debug ("Idle activation failed");
                listener->priv->session_idle = !idle;
        }

        return res;
}

gboolean
gs_listener_get_activation_enabled (GSListener *listener)
{
        g_return_val_if_fail (GS_IS_LISTENER (listener), FALSE);

        return listener->priv->activation_enabled;
}

gboolean
gs_listener_is_inhibited (GSListener *listener)
{
        gboolean inhibited;

        g_return_val_if_fail (GS_IS_LISTENER (listener), FALSE);

        inhibited = listener_ref_entry_is_present (listener, REF_ENTRY_TYPE_INHIBIT);

        return inhibited;
}

void
gs_listener_set_activation_enabled (GSListener *listener,
                                    gboolean    enabled)
{
        g_return_if_fail (GS_IS_LISTENER (listener));

        if (listener->priv->activation_enabled != enabled) {
                listener->priv->activation_enabled = enabled;
        }
}

static dbus_bool_t
listener_property_set_bool (GSListener *listener,
                            guint       prop_id,
                            dbus_bool_t value)
{
        dbus_bool_t ret;

        ret = FALSE;

        switch (prop_id) {
        case PROP_ACTIVE:
                gs_listener_set_active (listener, value);
                ret = TRUE;
                break;
        default:
                break;
        }

        return ret;
}

static void
raise_error (DBusConnection *connection,
             DBusMessage    *in_reply_to,
             const char     *error_name,
             char           *format, ...)
{
        char         buf[512];
        DBusMessage *reply;

        va_list args;
        va_start (args, format);
        vsnprintf (buf, sizeof (buf), format, args);
        va_end (args);

        g_warning (buf);
        reply = dbus_message_new_error (in_reply_to, error_name, buf);
        if (reply == NULL) {
                g_error ("No memory");
        }
        if (! dbus_connection_send (connection, reply, NULL)) {
                g_error ("No memory");
        }

        dbus_message_unref (reply);
}

static void
raise_syntax (DBusConnection *connection,
              DBusMessage    *in_reply_to,
              const char     *method_name)
{
        raise_error (connection, in_reply_to,
                     GS_LISTENER_SERVICE ".SyntaxError",
                     "There is a syntax error in the invocation of the method %s",
                     method_name);
}

static guint32
generate_cookie ()
{
        guint32 cookie;

        cookie = (guint32)g_random_int_range (1, G_MAXINT32);

        return cookie;
}

static guint32
listener_generate_unique_key (GSListener *listener,
                              int         entry_type)
{
        guint32     cookie;
        GHashTable *hash;

        hash = get_hash_for_entry_type (listener, entry_type);

        do {
                cookie = generate_cookie ();
        } while (g_hash_table_lookup (hash, &cookie) != NULL);

        return cookie;
}

static void
listener_ref_entry_check (GSListener *listener,
                          int         entry_type)
{
        switch (entry_type) {
        case REF_ENTRY_TYPE_INHIBIT:
                listener_check_activation (listener);
                break;
        case REF_ENTRY_TYPE_THROTTLE:
                listener_check_throttle (listener);
                break;
        default:
                g_assert_not_reached ();
                break;
        }
}

static void
listener_add_ref_entry (GSListener         *listener,
                        int                 entry_type,
                        GSListenerRefEntry *entry)
{
        GHashTable *hash;

        gs_debug ("adding %s from %s for reason '%s' on connection %s",
                  get_name_for_entry_type (entry_type),
                  entry->application,
                  entry->reason,
                  entry->connection);

        hash = get_hash_for_entry_type (listener, entry_type);
        g_hash_table_insert (hash, &entry->cookie, entry);

        listener_ref_entry_check (listener, entry_type);
}

static gboolean
listener_remove_ref_entry (GSListener *listener,
                           int         entry_type,
                           guint32     cookie)
{
        GHashTable         *hash;
        gboolean            removed;
        GSListenerRefEntry *entry;

        hash = get_hash_for_entry_type (listener, entry_type);

        entry = g_hash_table_lookup (hash, &cookie);
        if (entry != NULL) {
                gs_debug ("removing %s from %s for reason '%s' on connection %s",
                          get_name_for_entry_type (entry_type),
                          entry->application,
                          entry->reason,
                          entry->connection);
        }

        removed = g_hash_table_remove (hash, &cookie);

        if (removed) {
                listener_ref_entry_check (listener, entry_type);
        } else {
                gs_debug ("Cookie %u was not in the list!", cookie);
        }

        return removed;
}

static DBusHandlerResult
listener_dbus_add_ref_entry (GSListener     *listener,
                             int             entry_type,
                             DBusConnection *connection,
                             DBusMessage    *message)
{
        DBusMessage        *reply;
        DBusError           error;
        char               *application;
        char               *reason;
        GSListenerRefEntry *entry;
        DBusMessageIter     iter;

        dbus_error_init (&error);
        if (! dbus_message_get_args (message, &error,
                                     DBUS_TYPE_STRING, &application,
                                     DBUS_TYPE_STRING, &reason,
                                     DBUS_TYPE_INVALID)) {
                if (entry_type == REF_ENTRY_TYPE_INHIBIT) {
                        raise_syntax (connection, message, "Inhibit");
                } else if (entry_type == REF_ENTRY_TYPE_THROTTLE) {
                        raise_syntax (connection, message, "Throttle");
                } else {
                        g_assert_not_reached ();
                 }

                return DBUS_HANDLER_RESULT_HANDLED;
        }

        reply = dbus_message_new_method_return (message);
        if (reply == NULL) {
                g_error ("No memory");
        }

        entry = g_new0 (GSListenerRefEntry, 1);
        entry->entry_type = entry_type;
        entry->connection = g_strdup (dbus_message_get_sender (message));
        entry->cookie = listener_generate_unique_key (listener, entry_type);
        entry->application = g_strdup (application);
        entry->reason = g_strdup (reason);

        listener_add_ref_entry (listener, entry_type, entry);

        dbus_message_iter_init_append (reply, &iter);
        dbus_message_iter_append_basic (&iter, DBUS_TYPE_UINT32, &entry->cookie);

        if (! dbus_connection_send (connection, reply, NULL)) {
                g_error ("No memory");
        }

        dbus_message_unref (reply);

        return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult
listener_dbus_remove_ref_entry (GSListener     *listener,
                                int             entry_type,
                                DBusConnection *connection,
                                DBusMessage    *message)
{
        DBusMessage        *reply;
        DBusError           error;
        const char         *sender;
        guint32             cookie;

        dbus_error_init (&error);
        if (! dbus_message_get_args (message, &error,
                                     DBUS_TYPE_UINT32, &cookie,
                                     DBUS_TYPE_INVALID)) {
                if (entry_type == REF_ENTRY_TYPE_INHIBIT) {
                        raise_syntax (connection, message, "UnInhibit");
                } else if (entry_type == REF_ENTRY_TYPE_THROTTLE) {
                        raise_syntax (connection, message, "UnThrottle");
                } else {
                        g_assert_not_reached ();
                }

                return DBUS_HANDLER_RESULT_HANDLED;
        }

        reply = dbus_message_new_method_return (message);
        if (reply == NULL)
                g_error ("No memory");

        /* FIXME: check sender is from same connection as entry */
        sender = dbus_message_get_sender (message);

        listener_remove_ref_entry (listener, entry_type, cookie);

        /* FIXME:  Pointless? */
        if (! dbus_connection_send (connection, reply, NULL)) {
                g_error ("No memory");
        }

        dbus_message_unref (reply);

        return DBUS_HANDLER_RESULT_HANDLED;
}

static gboolean
listener_ref_entry_has_connection (gpointer key,
                                   gpointer value,
                                   gpointer user_data)
{
        GSListenerRefEntry *entry;
        const char         *connection;
        gboolean            matches;

        entry = (GSListenerRefEntry *)value;
        connection = (const char *) user_data;

        matches = FALSE;
        if (connection != NULL && entry->connection != NULL) {
                matches = (strcmp (connection, entry->connection) == 0);
                if (matches) {
                        gs_debug ("removing %s from %s for reason '%s' on connection %s",
                                  get_name_for_entry_type (entry->entry_type),
                                  entry->application,
                                  entry->reason,
                                  entry->connection);
                }
        }

        return matches;
}

static gboolean
listener_ref_entry_remove_for_connection (GSListener  *listener,
                                          int          entry_type,
                                          const char  *connection)
{
        gboolean    removed;
        guint       n_removed;
        GHashTable *hash;

        hash = get_hash_for_entry_type (listener, entry_type);

        removed = FALSE;
        n_removed = g_hash_table_foreach_remove (hash, listener_ref_entry_has_connection, (gpointer)connection);

        removed = (n_removed > 0);

        return removed;
}

static void
listener_service_deleted (GSListener  *listener,
                          DBusMessage *message)
{
        char    *old_service_name;
        char    *new_service_name;
        gboolean removed;

        if (! dbus_message_get_args (message, NULL,
                                     DBUS_TYPE_STRING, &old_service_name,
                                     DBUS_TYPE_STRING, &new_service_name,
                                     DBUS_TYPE_INVALID)) {
                g_error ("Invalid NameOwnerChanged signal from bus!");
                return;
        }

        gs_debug ("DBUS service deleted: %s", new_service_name);

        removed = listener_ref_entry_remove_for_connection (listener, REF_ENTRY_TYPE_THROTTLE, new_service_name);
        if (removed) {
                listener_ref_entry_check (listener, REF_ENTRY_TYPE_THROTTLE);
        }

        removed = listener_ref_entry_remove_for_connection (listener, REF_ENTRY_TYPE_INHIBIT, new_service_name);
        if (removed) {
                listener_ref_entry_check (listener, REF_ENTRY_TYPE_INHIBIT);
        }

}

static void
raise_property_type_error (DBusConnection *connection,
                           DBusMessage    *in_reply_to,
                           const char     *device_id)
{
        char         buf [512];
        DBusMessage *reply;

        snprintf (buf, 511,
                  "Type mismatch setting property with id %s",
                  device_id);
        g_warning (buf);

        reply = dbus_message_new_error (in_reply_to,
                                        TYPE_MISMATCH_ERROR,
                                        buf);
        if (reply == NULL) {
                g_error ("No memory");
        }
        if (! dbus_connection_send (connection, reply, NULL)) {
                g_error ("No memory");
        }

        dbus_message_unref (reply);
}

static DBusHandlerResult
listener_set_property (GSListener     *listener,
                       DBusConnection *connection,
                       DBusMessage    *message,
                       guint           prop_id)
{
        const char     *path;
        int             type;
        gboolean        rc;
        DBusMessageIter iter;
        DBusMessage    *reply;

        path = dbus_message_get_path (message);

        dbus_message_iter_init (message, &iter);
        type = dbus_message_iter_get_arg_type (&iter);
        rc = FALSE;

        switch (type) {
        case DBUS_TYPE_BOOLEAN:
                {
                        dbus_bool_t v;
                        dbus_message_iter_get_basic (&iter, &v);
                        rc = listener_property_set_bool (listener, prop_id, v);
                        break;
                }
        default:
                g_warning ("Unsupported property type %d", type);
                break;
        }

        if (! rc) {
                raise_property_type_error (connection, message, path);
                return DBUS_HANDLER_RESULT_HANDLED;
        }

        reply = dbus_message_new_method_return (message);

        if (reply == NULL) {
                g_error ("No memory");
        }

        if (! dbus_connection_send (connection, reply, NULL)) {
                g_error ("No memory");
        }

        dbus_message_unref (reply);

        return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult
listener_get_property (GSListener     *listener,
                       DBusConnection *connection,
                       DBusMessage    *message,
                       guint           prop_id)
{
        DBusMessageIter iter;
        DBusMessage    *reply;

        reply = dbus_message_new_method_return (message);

        dbus_message_iter_init_append (reply, &iter);

        if (reply == NULL)
                g_error ("No memory");

        switch (prop_id) {
        case PROP_ACTIVE:
                {
                        dbus_bool_t b;
                        b = listener->priv->active;
                        dbus_message_iter_append_basic (&iter, DBUS_TYPE_BOOLEAN, &b);
                }
                break;
        case PROP_SESSION_IDLE:
                {
                        dbus_bool_t b;
                        b = listener->priv->session_idle;
                        dbus_message_iter_append_basic (&iter, DBUS_TYPE_BOOLEAN, &b);
                }
                break;
        default:
                g_warning ("Unsupported property id %u", prop_id);
                break;
        }

        if (! dbus_connection_send (connection, reply, NULL)) {
                g_error ("No memory");
        }

        dbus_message_unref (reply);

        return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult
listener_get_active_time (GSListener     *listener,
                          DBusConnection *connection,
                          DBusMessage    *message)
{
        DBusMessageIter iter;
        DBusMessage    *reply;
        dbus_uint32_t    secs;

        reply = dbus_message_new_method_return (message);

        dbus_message_iter_init_append (reply, &iter);

        if (reply == NULL) {
                g_error ("No memory");
        }

        if (listener->priv->active) {
                time_t now = time (NULL);

                if (now < listener->priv->active_start) {
                        /* shouldn't happen */
                        g_warning ("Active start time is in the future");
                        secs = 0;
                } else if (listener->priv->active_start <= 0) {
                        /* shouldn't happen */
                        g_warning ("Active start time was not set");
                        secs = 0;
                } else {
                        secs = now - listener->priv->active_start;
                }
        } else {
                secs = 0;
        }

        gs_debug ("Returning screensaver active for %u seconds", secs);
        dbus_message_iter_append_basic (&iter, DBUS_TYPE_UINT32, &secs);

        if (! dbus_connection_send (connection, reply, NULL)) {
                g_error ("No memory");
        }

        dbus_message_unref (reply);

        return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult
listener_get_session_idle_time (GSListener     *listener,
                                DBusConnection *connection,
                                DBusMessage    *message)
{
        DBusMessageIter iter;
        DBusMessage    *reply;
        dbus_uint32_t    secs;

        reply = dbus_message_new_method_return (message);

        if (reply == NULL) {
                g_error ("No memory");
        }

        dbus_message_iter_init_append (reply, &iter);

        if (listener->priv->session_idle) {
                time_t now = time (NULL);

                if (now < listener->priv->session_idle_start) {
                        /* shouldn't happen */
                        g_warning ("Session idle start time is in the future");
                        secs = 0;
                } else if (listener->priv->session_idle_start <= 0) {
                        /* shouldn't happen */
                        g_warning ("Session idle start time was not set");
                        secs = 0;
                } else {
                        secs = now - listener->priv->session_idle_start;
                }
        } else {
                secs = 0;
        }

        gs_debug ("Returning session idle for %u seconds", secs);
        dbus_message_iter_append_basic (&iter, DBUS_TYPE_UINT32, &secs);

        if (! dbus_connection_send (connection, reply, NULL)) {
                g_error ("No memory");
        }

        dbus_message_unref (reply);

        return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult
listener_dbus_filter_handle_methods (DBusConnection *connection,
                                     DBusMessage    *message, 
                                     void           *user_data,
                                     dbus_bool_t     local_interface)
{
        GSListener *listener = GS_LISTENER (user_data);

#if 0
        g_message ("obj_path=%s interface=%s method=%s destination=%s", 
                   dbus_message_get_path (message), 
                   dbus_message_get_interface (message),
                   dbus_message_get_member (message),
                   dbus_message_get_destination (message));
#endif

        g_return_val_if_fail (connection != NULL, DBUS_HANDLER_RESULT_NOT_YET_HANDLED);
        g_return_val_if_fail (message != NULL, DBUS_HANDLER_RESULT_NOT_YET_HANDLED);

        if (dbus_message_is_method_call (message, GS_LISTENER_SERVICE, "Lock")) {
                g_signal_emit (listener, signals [LOCK], 0);
                return DBUS_HANDLER_RESULT_HANDLED;
        }
        if (dbus_message_is_method_call (message, GS_LISTENER_SERVICE, "Quit")) {
                g_signal_emit (listener, signals [QUIT], 0);
                return DBUS_HANDLER_RESULT_HANDLED;
        }
        if (dbus_message_is_method_call (message, GS_LISTENER_SERVICE, "Cycle")) {
                g_signal_emit (listener, signals [CYCLE], 0);
                return DBUS_HANDLER_RESULT_HANDLED;
        }
        if (dbus_message_is_method_call (message, GS_LISTENER_SERVICE, "Inhibit")) {
                return listener_dbus_add_ref_entry (listener, REF_ENTRY_TYPE_INHIBIT, connection, message);
        }
        if (dbus_message_is_method_call (message, GS_LISTENER_SERVICE, "UnInhibit")) {
                return listener_dbus_remove_ref_entry (listener, REF_ENTRY_TYPE_INHIBIT, connection, message);
        }
        if (dbus_message_is_method_call (message, GS_LISTENER_SERVICE, "Throttle")) {
                return listener_dbus_add_ref_entry (listener, REF_ENTRY_TYPE_THROTTLE, connection, message);
        }
        if (dbus_message_is_method_call (message, GS_LISTENER_SERVICE, "UnThrottle")) {
                return listener_dbus_remove_ref_entry (listener, REF_ENTRY_TYPE_THROTTLE, connection, message);
        }
        if (dbus_message_is_method_call (message, GS_LISTENER_SERVICE, "setActive")) {
                return listener_set_property (listener, connection, message, PROP_ACTIVE);
        }
        if (dbus_message_is_method_call (message, GS_LISTENER_SERVICE, "getActive")) {
                return listener_get_property (listener, connection, message, PROP_ACTIVE);
        }
        if (dbus_message_is_method_call (message, GS_LISTENER_SERVICE, "getActiveTime")) {
                return listener_get_active_time (listener, connection, message);
        }
        if (dbus_message_is_method_call (message, GS_LISTENER_SERVICE, "getSessionIdle")) {
                return listener_get_property (listener, connection, message, PROP_SESSION_IDLE);
        }
        if (dbus_message_is_method_call (message, GS_LISTENER_SERVICE, "getSessionIdleTime")) {
                return listener_get_session_idle_time (listener, connection, message);
        }
        if (dbus_message_is_method_call (message, GS_LISTENER_SERVICE, "Poke")) {
                g_signal_emit (listener, signals [POKE], 0);
                return DBUS_HANDLER_RESULT_HANDLED;
        }

        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusHandlerResult
gs_listener_message_handler (DBusConnection *connection,
                             DBusMessage    *message,
                             void           *user_data)
{
        g_return_val_if_fail (connection != NULL, DBUS_HANDLER_RESULT_NOT_YET_HANDLED);
        g_return_val_if_fail (message != NULL, DBUS_HANDLER_RESULT_NOT_YET_HANDLED);

#if 0
        g_message ("obj_path=%s interface=%s method=%s destination=%s", 
                   dbus_message_get_path (message), 
                   dbus_message_get_interface (message),
                   dbus_message_get_member (message),
                   dbus_message_get_destination (message));
#endif

        if (dbus_message_is_method_call (message, "org.freedesktop.DBus", "AddMatch")) {
                DBusMessage *reply;

                reply = dbus_message_new_method_return (message);

                if (reply == NULL) {
                        g_error ("No memory");
                }

                if (! dbus_connection_send (connection, reply, NULL)) {
                        g_error ("No memory");
                }

                dbus_message_unref (reply);

                return DBUS_HANDLER_RESULT_HANDLED;
        } else if (dbus_message_is_signal (message, DBUS_INTERFACE_LOCAL, "Disconnected") &&
                   strcmp (dbus_message_get_path (message), DBUS_PATH_LOCAL) == 0) {
                
                dbus_connection_unref (connection);

                return DBUS_HANDLER_RESULT_HANDLED;
        } else {
                return listener_dbus_filter_handle_methods (connection, message, user_data, TRUE);
        }
}

static gboolean
gs_listener_dbus_init (GSListener *listener)
{
        DBusError error;

        dbus_error_init (&error);

        listener->priv->connection = dbus_bus_get (DBUS_BUS_SESSION, &error);

        if (listener->priv->connection == NULL) {
                if (dbus_error_is_set (&error)) {
                        g_warning ("couldn't connect to session bus: %s",
                                   error.message);
                        dbus_error_free (&error);
                }
                return FALSE;
        }

        dbus_connection_setup_with_g_main (listener->priv->connection, NULL);
	dbus_connection_set_exit_on_disconnect (listener->priv->connection, FALSE);

        return TRUE;
}

static gboolean
reinit_dbus (GSListener *listener)
{
        gboolean initialized;
        gboolean try_again;

        initialized = gs_listener_dbus_init (listener);

        /* if we didn't initialize then try again */
        /* FIXME: Should we keep trying forever?  If we fail more than
           once or twice then the session bus may have died.  The
           problem is that if it is restarted it will likely have a
           different bus address and we won't be able to find it */
        try_again = !initialized;

        return try_again;
}

static DBusHandlerResult
listener_dbus_filter_function (DBusConnection *connection,
                               DBusMessage    *message,
                               void           *user_data)
{
        GSListener *listener = GS_LISTENER (user_data);
        const char *path;

        path = dbus_message_get_path (message);

        /*
        g_message ("obj_path=%s interface=%s method=%s", 
                   dbus_message_get_path (message), 
                   dbus_message_get_interface (message),
                   dbus_message_get_member (message));
        */

        if (dbus_message_is_signal (message, DBUS_INTERFACE_LOCAL, "Disconnected") &&
            strcmp (path, DBUS_PATH_LOCAL) == 0) {

                g_message ("Got disconnected from the system message bus; "
                           "retrying to reconnect every 10 seconds");

                dbus_connection_unref (connection);
                connection = NULL;

                g_timeout_add (10000, (GSourceFunc)reinit_dbus, listener);
        } else if (dbus_message_is_signal (message,
                                           DBUS_INTERFACE_DBUS,
                                           "NameOwnerChanged")) {

                if (listener->priv->inhibitors != NULL)
                        listener_service_deleted (listener, message);
        } else {
                return listener_dbus_filter_handle_methods (connection, message, user_data, FALSE);
        }

        return DBUS_HANDLER_RESULT_HANDLED;
}

static void
gs_listener_set_property (GObject            *object,
                          guint               prop_id,
                          const GValue       *value,
                          GParamSpec         *pspec)
{
        GSListener *self;

        self = GS_LISTENER (object);

        switch (prop_id) {
        case PROP_ACTIVE:
                gs_listener_set_active (self, g_value_get_boolean (value));
                break;
        case PROP_SESSION_IDLE:
                gs_listener_set_session_idle (self, g_value_get_boolean (value));
                break;
        case PROP_ACTIVATION_ENABLED:
                gs_listener_set_activation_enabled (self, g_value_get_boolean (value));
                break;
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static void
gs_listener_get_property (GObject            *object,
                          guint               prop_id,
                          GValue             *value,
                          GParamSpec         *pspec)
{
        GSListener *self;

        self = GS_LISTENER (object);

        switch (prop_id) {
        case PROP_ACTIVE:
                g_value_set_boolean (value, self->priv->active);
                break;
        case PROP_SESSION_IDLE:
                g_value_set_boolean (value, self->priv->session_idle);
                break;
        case PROP_ACTIVATION_ENABLED:
                g_value_set_boolean (value, self->priv->activation_enabled);
                break;
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static void
gs_listener_class_init (GSListenerClass *klass)
{
        GObjectClass   *object_class = G_OBJECT_CLASS (klass);

        parent_class = g_type_class_peek_parent (klass);

        object_class->finalize     = gs_listener_finalize;
        object_class->get_property = gs_listener_get_property;
        object_class->set_property = gs_listener_set_property;

        signals [LOCK] =
                g_signal_new ("lock",
                              G_TYPE_FROM_CLASS (object_class),
                              G_SIGNAL_RUN_LAST,
                              G_STRUCT_OFFSET (GSListenerClass, lock),
                              NULL,
                              NULL,
                              g_cclosure_marshal_VOID__VOID,
                              G_TYPE_NONE,
                              0);
        signals [QUIT] =
                g_signal_new ("quit",
                              G_TYPE_FROM_CLASS (object_class),
                              G_SIGNAL_RUN_LAST,
                              G_STRUCT_OFFSET (GSListenerClass, quit),
                              NULL,
                              NULL,
                              g_cclosure_marshal_VOID__VOID,
                              G_TYPE_NONE,
                              0);
        signals [CYCLE] =
                g_signal_new ("cycle",
                              G_TYPE_FROM_CLASS (object_class),
                              G_SIGNAL_RUN_LAST,
                              G_STRUCT_OFFSET (GSListenerClass, cycle),
                              NULL,
                              NULL,
                              g_cclosure_marshal_VOID__VOID,
                              G_TYPE_NONE,
                              0);
        signals [POKE] =
                g_signal_new ("poke",
                              G_TYPE_FROM_CLASS (object_class),
                              G_SIGNAL_RUN_LAST,
                              G_STRUCT_OFFSET (GSListenerClass, poke),
                              NULL,
                              NULL,
                              g_cclosure_marshal_VOID__VOID,
                              G_TYPE_NONE,
                              0);
        signals [ACTIVE_CHANGED] =
                g_signal_new ("active-changed",
                              G_TYPE_FROM_CLASS (object_class),
                              G_SIGNAL_RUN_LAST,
                              G_STRUCT_OFFSET (GSListenerClass, active_changed),
                              NULL,
                              NULL,
                              gs_marshal_BOOLEAN__BOOLEAN,
                              G_TYPE_BOOLEAN,
                              1,
                              G_TYPE_BOOLEAN);
        signals [THROTTLE_CHANGED] =
                g_signal_new ("throttle-changed",
                              G_TYPE_FROM_CLASS (object_class),
                              G_SIGNAL_RUN_LAST,
                              G_STRUCT_OFFSET (GSListenerClass, throttle_changed),
                              NULL,
                              NULL,
                              g_cclosure_marshal_VOID__BOOLEAN,
                              G_TYPE_NONE,
                              1,
                              G_TYPE_BOOLEAN);

        g_object_class_install_property (object_class,
                                         PROP_ACTIVE,
                                         g_param_spec_boolean ("active",
                                                               NULL,
                                                               NULL,
                                                               FALSE,
                                                               G_PARAM_READWRITE));
        g_object_class_install_property (object_class,
                                         PROP_ACTIVATION_ENABLED,
                                         g_param_spec_boolean ("activation-enabled",
                                                               NULL,
                                                               NULL,
                                                               TRUE,
                                                               G_PARAM_READWRITE));

        g_type_class_add_private (klass, sizeof (GSListenerPrivate));
}


static gboolean
screensaver_is_running (DBusConnection *connection)
{
        DBusError error;
        gboolean  exists;
              
        g_return_val_if_fail (connection != NULL, FALSE);

        dbus_error_init (&error);
        exists = dbus_bus_name_has_owner (connection, GS_LISTENER_SERVICE, &error);
        if (dbus_error_is_set (&error)) {
                dbus_error_free (&error);
        }

        return exists;
}

gboolean
gs_listener_acquire (GSListener *listener,
                     GError    **error)
{
        gboolean  acquired;
        DBusError buserror;
        gboolean  is_connected;

        g_return_val_if_fail (listener != NULL, FALSE);

        if (! listener->priv->connection) {
                g_set_error (error,
                             GS_LISTENER_ERROR,
                             GS_LISTENER_ERROR_ACQUISITION_FAILURE,
                             "%s",
                             _("failed to register with the message bus"));
                return FALSE;
        }

        is_connected = dbus_connection_get_is_connected (listener->priv->connection);
        if (! is_connected) {
                g_set_error (error,
                             GS_LISTENER_ERROR,
                             GS_LISTENER_ERROR_ACQUISITION_FAILURE,
                             "%s",
                             _("not connected to the message bus"));
                return FALSE;
        }

        if (screensaver_is_running (listener->priv->connection)) {
                g_set_error (error,
                             GS_LISTENER_ERROR,
                             GS_LISTENER_ERROR_ACQUISITION_FAILURE,
                             "%s",
                             _("screensaver already running in this session"));
                return FALSE;
        }

        dbus_error_init (&buserror);

        if (dbus_connection_register_object_path (listener->priv->connection,
                                                  GS_LISTENER_PATH,
                                                  &gs_listener_vtable,
                                                  listener) == FALSE) {
                g_critical ("out of memory registering object path");
                return FALSE;
        }

        acquired = dbus_bus_request_name (listener->priv->connection,
                                          GS_LISTENER_SERVICE,
                                          0, &buserror) != -1;
        if (dbus_error_is_set (&buserror)) {
                g_set_error (error,
                             GS_LISTENER_ERROR,
                             GS_LISTENER_ERROR_ACQUISITION_FAILURE,
                             "%s",
                             buserror.message);
        }

        dbus_error_free (&buserror);

        dbus_connection_add_filter (listener->priv->connection, listener_dbus_filter_function, listener, NULL);

        dbus_bus_add_match (listener->priv->connection,
                            "type='signal'"
                            ",interface='"DBUS_INTERFACE_DBUS"'"
                            ",sender='"DBUS_SERVICE_DBUS"'"
                            ",member='NameOwnerChanged'",
                            NULL);

        return acquired;
}

static void
gs_listener_init (GSListener *listener)
{
        listener->priv = GS_LISTENER_GET_PRIVATE (listener);

        gs_listener_dbus_init (listener);

        listener->priv->inhibitors = g_hash_table_new_full (g_int_hash,
                                                            g_int_equal,
                                                            NULL,
                                                            (GDestroyNotify)gs_listener_ref_entry_free);
        listener->priv->throttlers = g_hash_table_new_full (g_int_hash,
                                                            g_int_equal,
                                                            NULL,
                                                            (GDestroyNotify)gs_listener_ref_entry_free);

}

static void
gs_listener_finalize (GObject *object)
{
        GSListener *listener;

        g_return_if_fail (object != NULL);
        g_return_if_fail (GS_IS_LISTENER (object));

        listener = GS_LISTENER (object);

        g_return_if_fail (listener->priv != NULL);

        if (listener->priv->inhibitors) {
                g_hash_table_destroy (listener->priv->inhibitors);
        }

        if (listener->priv->throttlers) {
                g_hash_table_destroy (listener->priv->throttlers);
        }

        G_OBJECT_CLASS (parent_class)->finalize (object);
}

GSListener *
gs_listener_new (void)
{
        GSListener *listener;

        listener = g_object_new (GS_TYPE_LISTENER, NULL);

        return GS_LISTENER (listener);
}
