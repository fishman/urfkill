/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2010 Gary Ching-Pang Lin <glin@novell.com>
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <string.h>
#include <stdlib.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <glib-object.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>

#include "egg-debug.h"

#include "urf-polkit.h"
#include "urf-daemon.h"
#include "urf-killswitch.h"
#include "urf-utils.h"

#include "urf-daemon-glue.h"
#include "urf-marshal.h"

enum
{
	PROP_0,
	PROP_DAEMON_VERSION,
	PROP_LAST
};

enum
{
	SIGNAL_RFKILL_ADDED,
	SIGNAL_RFKILL_REMOVED,
	SIGNAL_RFKILL_CHANGED,
	SIGNAL_LAST,
};

static guint signals[SIGNAL_LAST] = { 0 };

struct UrfDaemonPrivate
{
	DBusGConnection	*connection;
	DBusGProxy	*proxy;
	UrfPolkit	*polkit;
	UrfKillswitch   *killswitch;
};

static void urf_daemon_finalize (GObject *object);

G_DEFINE_TYPE (UrfDaemon, urf_daemon, G_TYPE_OBJECT)

#define URF_DAEMON_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), \
				URF_TYPE_DAEMON, UrfDaemonPrivate))

#define URF_DAEMON_STATES_STRUCT_TYPE (dbus_g_type_get_struct ("GValueArray",	\
								G_TYPE_UINT,	\
								G_TYPE_UINT,	\
								G_TYPE_INT,	\
								G_TYPE_UINT,	\
								G_TYPE_UINT,	\
								G_TYPE_STRING,	\
								G_TYPE_INVALID))

/**
 * urf_daemon_register_rfkill_daemon:
 **/
static gboolean
urf_daemon_register_rfkill_daemon (UrfDaemon *daemon)
{
	GError *error = NULL;
	gboolean ret = FALSE;
	UrfDaemonPrivate *priv = URF_DAEMON_GET_PRIVATE (daemon);

	priv->connection = dbus_g_bus_get (DBUS_BUS_SYSTEM, &error);
	if (priv->connection == NULL) {
		if (error != NULL) {
			g_critical ("error getting system bus: %s", error->message);
			g_error_free (error);
		}
		goto out;
	}

	/* connect to DBUS */
	priv->proxy = dbus_g_proxy_new_for_name (priv->connection,
						 DBUS_SERVICE_DBUS,
						 DBUS_PATH_DBUS,
						 DBUS_INTERFACE_DBUS);

	/* register GObject */
	dbus_g_connection_register_g_object (priv->connection,
					     "/org/freedesktop/URfkill",
					     G_OBJECT (daemon));

	/* success */
	ret = TRUE;
out:
	return ret;
}

/**
 * urf_daemon_startup:
 **/
gboolean
urf_daemon_startup (UrfDaemon *daemon)
{
	gboolean ret;
	UrfDaemonPrivate *priv = URF_DAEMON_GET_PRIVATE (daemon);

	/* register on bus */
	ret = urf_daemon_register_rfkill_daemon (daemon);
	if (!ret) {
		egg_warning ("failed to register");
		goto out;
	}

out:
	return ret;
}

/**
 * urf_daemon_block:
 **/
gboolean
urf_daemon_block (UrfDaemon *daemon, const char *type_name, DBusGMethodInvocation *context)
{
	UrfDaemonPrivate *priv = URF_DAEMON_GET_PRIVATE (daemon);
	int type;
	gchar *sender = NULL;
	UrfPolkitCaller *caller;
	GError *error = NULL;
	gboolean ret;

	if (!urf_killswitch_has_killswitches (priv->killswitch))
		return FALSE;

	type = urf_killswitch_rf_type (priv->killswitch, type_name);
	if (type < 0)
		return FALSE;

	sender = dbus_g_method_get_sender (context);
	caller = urf_polkit_caller_new_from_sender (priv->polkit, sender);
	if (caller == NULL) {
		error = g_error_new (URF_DAEMON_ERROR, URF_DAEMON_ERROR_GENERAL,
				     "caller %s not found", sender);
		dbus_g_method_return_error (context, error);
		goto out;
	}

	if (!urf_polkit_check_auth (priv->polkit, caller, "org.freedesktop.urfkill.block", context))
		goto out;

	ret = urf_killswitch_set_state (priv->killswitch, type, KILLSWITCH_STATE_SOFT_BLOCKED);

	dbus_g_method_return (context, ret);
out:
	if (caller != NULL)
		urf_polkit_caller_unref (caller);	

	return TRUE;
}

/**
 * urf_daemon_unblock:
 **/
gboolean
urf_daemon_unblock (UrfDaemon *daemon, const char *type_name, DBusGMethodInvocation *context)
{
	UrfDaemonPrivate *priv = URF_DAEMON_GET_PRIVATE (daemon);
	int type;
	gchar *sender = NULL;
	UrfPolkitCaller *caller;
	GError *error = NULL;
	gboolean ret;

	if (!urf_killswitch_has_killswitches (priv->killswitch))
		return FALSE;

	type = urf_killswitch_rf_type (priv->killswitch, type_name);
	if (type < 0)
		return FALSE;

	sender = dbus_g_method_get_sender (context);
	caller = urf_polkit_caller_new_from_sender (priv->polkit, sender);
	if (caller == NULL) {
		error = g_error_new (URF_DAEMON_ERROR, URF_DAEMON_ERROR_GENERAL,
				     "caller %s not found", sender);
		dbus_g_method_return_error (context, error);
		goto out;
	}

	if (!urf_polkit_check_auth (priv->polkit, caller, "org.freedesktop.urfkill.unblock", context))
		goto out;

	ret = urf_killswitch_set_state (priv->killswitch, type, KILLSWITCH_STATE_UNBLOCKED);

	dbus_g_method_return (context, ret);
out:
	if (caller != NULL)
		urf_polkit_caller_unref (caller);	

	return TRUE;
}

/**
 * urf_daemon_get_all:
 **/
gboolean
urf_daemon_get_all (UrfDaemon *daemon, DBusGMethodInvocation *context)
{
	UrfDaemonPrivate *priv = URF_DAEMON_GET_PRIVATE (daemon);
	GError *error;
	GPtrArray *array;
	GList *killswitches = NULL, *item = NULL;
	GValue *value;
	UrfIndKillswitch *ind;
	char *device_name;

	g_return_val_if_fail (URF_IS_DAEMON (daemon), FALSE);

	killswitches = urf_killswitch_get_killswitches (priv->killswitch);

	array = g_ptr_array_sized_new (g_list_length(killswitches));
	for (item = killswitches; item; item = g_list_next (item)) {
		ind = (UrfIndKillswitch *)item->data;

		device_name = get_rfkill_name_by_index (ind->index);

		value = g_new0 (GValue, 1);
		g_value_init (value, URF_DAEMON_STATES_STRUCT_TYPE);
		g_value_take_boxed (value, dbus_g_type_specialized_construct (URF_DAEMON_STATES_STRUCT_TYPE));
		dbus_g_type_struct_set (value,
					0, ind->index,
					1, ind->type,
					2, ind->state,
					3, ind->soft,
					4, ind->hard,
					5, device_name, -1);
		g_ptr_array_add (array, g_value_get_boxed (value));
		g_free (value);
	}

	dbus_g_method_return (context, array);

	return TRUE;
}


/**
 * urf_daemon_get_killswitch:
 **/
gboolean
urf_daemon_get_killswitch (UrfDaemon *daemon, const guint index, DBusGMethodInvocation *context)
{
	UrfDaemonPrivate *priv = URF_DAEMON_GET_PRIVATE (daemon);
	GError *error;
	UrfKillswitch *killswitch;
	UrfIndKillswitch *ind;
	char *device_name;
	int type, state;
	guint soft, hard;

	g_return_val_if_fail (URF_IS_DAEMON (daemon), FALSE);

	killswitch = priv->killswitch;

	ind = urf_killswitch_get_killswitch (killswitch, index);
	
	if (ind == NULL) {
		type = -1;
		state = -1;
		soft = 1;
		hard = 1;
		device_name = NULL;
	} else {
		type = ind->type;
		state = ind->state;
		soft = ind->soft;
		hard = ind->hard;
		device_name = get_rfkill_name_by_index (index);
	}
	
	dbus_g_method_return (context, type, state, soft, hard, device_name);

	return TRUE;
}

/**
 * urf_daemon_killswitch_added_cb:
 **/
static void
urf_daemon_killswitch_added_cb (UrfKillswitch *killswitch,
				guint index,
				UrfDaemon *daemon)
{
	UrfIndKillswitch *ind;
	char *device_name;

	g_return_if_fail (URF_IS_DAEMON (daemon));
	g_return_if_fail (URF_IS_KILLSWITCH (killswitch));

	ind = urf_killswitch_get_killswitch (killswitch, index);

	device_name = get_rfkill_name_by_index (index);

	if (!ind)
		return;

	g_signal_emit (daemon, signals[SIGNAL_RFKILL_ADDED], 0,
		       index, ind->type, ind->state, ind->soft, ind->hard, device_name);
}

/**
 * urf_daemon_killswitch_removed_cb:
 **/
static void
urf_daemon_killswitch_removed_cb (UrfKillswitch *killswitch,
				  guint index,
				  UrfDaemon *daemon)
{
	g_return_if_fail (URF_IS_DAEMON (daemon));
	g_return_if_fail (URF_IS_KILLSWITCH (killswitch));

	g_signal_emit (daemon, signals[SIGNAL_RFKILL_REMOVED], 0, index);
}

/**
 * urf_daemon_killswitch_changed_cb:
 **/
static void
urf_daemon_killswitch_changed_cb (UrfKillswitch *killswitch,
				  guint index,
				  UrfDaemon *daemon)
{
	UrfIndKillswitch *ind;
	char *device_name;

	g_return_if_fail (URF_IS_DAEMON (daemon));
	g_return_if_fail (URF_IS_KILLSWITCH (killswitch));

	ind = urf_killswitch_get_killswitch (killswitch, index);

	if (!ind)
		return;

	device_name = get_rfkill_name_by_index (index);

	g_signal_emit (daemon, signals[SIGNAL_RFKILL_CHANGED], 0,
		       index, ind->type, ind->state, ind->soft, ind->hard, device_name);
}

/**
 * urf_daemon_init:
 **/
static void
urf_daemon_init (UrfDaemon *daemon)
{
	gboolean ret;
	GError *error = NULL;

	daemon->priv = URF_DAEMON_GET_PRIVATE (daemon);
	daemon->priv->polkit = urf_polkit_new ();

	daemon->priv->killswitch = urf_killswitch_new ();

	g_signal_connect (daemon->priv->killswitch, "rfkill-added",
			  G_CALLBACK (urf_daemon_killswitch_added_cb), daemon);
	g_signal_connect (daemon->priv->killswitch, "rfkill-removed",
			  G_CALLBACK (urf_daemon_killswitch_removed_cb), daemon);
	g_signal_connect (daemon->priv->killswitch, "rfkill-changed",
			  G_CALLBACK (urf_daemon_killswitch_changed_cb), daemon);
}

/**
 * urf_daemon_error_quark:
 **/
GQuark
urf_daemon_error_quark (void)
{
	static GQuark ret = 0;
	if (ret == 0)
		ret = g_quark_from_static_string ("urf_daemon_error");
	return ret;
}

/**
 * urf_daemon_get_property:
 **/
static void
urf_daemon_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
	UrfDaemon *daemon = URF_DAEMON (object);
	UrfDaemonPrivate *priv = URF_DAEMON_GET_PRIVATE (daemon);

	switch (prop_id) {
	case PROP_DAEMON_VERSION:
		g_value_set_string (value, PACKAGE_VERSION);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

/**
 * urf_daemon_set_property:
 **/
static void
urf_daemon_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
	switch (prop_id) {
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

#define ENUM_ENTRY(NAME, DESC) { NAME, "" #NAME "", DESC }
/**
 * urf_daemon_error_get_type:
 **/
GType
urf_daemon_error_get_type (void)
{
	static GType etype = 0;

	if (etype == 0) {
		static const GEnumValue values[] = {
			ENUM_ENTRY (URF_DAEMON_ERROR_GENERAL, "GeneralError"),
			ENUM_ENTRY (URF_DAEMON_ERROR_NOT_SUPPORTED, "NotSupported"),
			ENUM_ENTRY (URF_DAEMON_ERROR_NO_SUCH_DEVICE, "NoSuchDevice"),
			{ 0, 0, 0 }
		};
		g_assert (URF_DAEMON_NUM_ERRORS == G_N_ELEMENTS (values) - 1);
		etype = g_enum_register_static ("UrfDaemonError", values);
	}
	return etype;
}

/**
 * urf_daemon_class_init:
 **/
static void
urf_daemon_class_init (UrfDaemonClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = urf_daemon_finalize;
	object_class->get_property = urf_daemon_get_property;
	object_class->set_property = urf_daemon_set_property;

	g_type_class_add_private (klass, sizeof (UrfDaemonPrivate));

	signals[SIGNAL_RFKILL_ADDED] =
		g_signal_new ("rfkill-added",
			      G_OBJECT_CLASS_TYPE (klass),
			      G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
			      0, NULL, NULL,
			      urf_marshal_VOID__UINT_UINT_INT_UINT_UINT_STRING,
			      G_TYPE_NONE, 6, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_INT, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_STRING);

	signals[SIGNAL_RFKILL_REMOVED] =
		g_signal_new ("rfkill-removed",
			      G_OBJECT_CLASS_TYPE (klass),
			      G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
			      0, NULL, NULL,
			      g_cclosure_marshal_VOID__UINT,
			      G_TYPE_NONE, 1, G_TYPE_UINT);

	signals[SIGNAL_RFKILL_CHANGED] =
		g_signal_new ("rfkill-changed",
			      G_OBJECT_CLASS_TYPE (klass),
			      G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
			      0, NULL, NULL,
			      urf_marshal_VOID__UINT_UINT_INT_UINT_UINT_STRING,
			      G_TYPE_NONE, 6, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_INT, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_STRING);

	g_object_class_install_property (object_class,
					 PROP_DAEMON_VERSION,
					 g_param_spec_string ("daemon-version",
							      "Daemon Version",
							      "The version of the running daemon",
							      NULL,
							      G_PARAM_READABLE));

	dbus_g_object_type_install_info (URF_TYPE_DAEMON, &dbus_glib_urf_daemon_object_info);
	dbus_g_error_domain_register (URF_DAEMON_ERROR, NULL, URF_DAEMON_TYPE_ERROR);
}

/**
 * urf_daemon_finalize:
 **/
static void
urf_daemon_finalize (GObject *object)
{
	UrfDaemon *daemon = URF_DAEMON (object);
	UrfDaemonPrivate *priv = URF_DAEMON_GET_PRIVATE (daemon);

	if (priv->proxy != NULL)
		g_object_unref (priv->proxy);
	if (priv->connection != NULL)
		dbus_g_connection_unref (priv->connection);

	g_object_unref (priv->polkit);
	g_object_unref (priv->killswitch);
	G_OBJECT_CLASS (urf_daemon_parent_class)->finalize (object);
}

/**
 * urf_daemon_new:
 **/
UrfDaemon *
urf_daemon_new (void)
{
	UrfDaemon *daemon;
	daemon = URF_DAEMON (g_object_new (URF_TYPE_DAEMON, NULL));
	return daemon;
}
