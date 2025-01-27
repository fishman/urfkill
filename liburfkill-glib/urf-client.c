/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2010 Gary Ching-Pang Lin <glin@suse.com>
 *
 * Licensed under the GNU Lesser General Public License Version 2.1
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

/**
 * SECTION:urf-client
 * @short_description: Main client object for accessing the urfkill daemon
 * @title: UrfClient
 * @include: urfkill.h
 * @see_also: #UrfDevice
 *
 * A helper GObject to use for accessing urfkill information, and to be
 * notified when it is changed.
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <glib.h>
#include <dbus/dbus-glib.h>

#include "urf-client.h"

static void	urf_client_class_init	(UrfClientClass	*klass);
static void	urf_client_init		(UrfClient	*client);
static void	urf_client_dispose	(GObject	*object);
static void	urf_client_finalize	(GObject	*object);

#define URF_CLIENT_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), URF_TYPE_CLIENT, UrfClientPrivate))

struct _UrfClientPrivate
{
	DBusGConnection	*bus;
	DBusGProxy	*proxy;
	DBusGProxy	*prop_proxy;
	GList		*devices;
	char		*daemon_version;
	gboolean	 key_control;
	gboolean	 have_properties;
};

enum {
	URF_CLIENT_DEVICE_ADDED,
	URF_CLIENT_DEVICE_REMOVED,
	URF_CLIENT_DEVICE_CHANGED,
	URF_CLIENT_RF_KEY_PRESSED,
	URF_CLIENT_LAST_SIGNAL
};

enum {
	PROP_0,
	PROP_DAEMON_VERSION,
	PROP_KEY_CONTROL,
	PROP_LAST
};

static guint signals [URF_CLIENT_LAST_SIGNAL] = { 0 };
static gpointer urf_client_object = NULL;

G_DEFINE_TYPE (UrfClient, urf_client, G_TYPE_OBJECT)

/**
 * urf_client_find_device:
 **/
static UrfDevice *
urf_client_find_device (UrfClient   *client,
			const char  *object_path)
{
	UrfClientPrivate *priv = client->priv;
	UrfDevice *device = NULL;
	GList *item;

	if (priv->devices == NULL)
		return NULL;

	for (item = priv->devices; item; item = item->next) {
		device = (UrfDevice *) item->data;
		if (g_strcmp0 (urf_device_get_object_path (device), object_path) == 0)
			return device;
	}

	return NULL;
}

/**
 * urf_client_get_devices:
 * @client: a #UrfClient instance
 *
 * Get a list of the device objects.
 *
 * Return value: (element-type UrfDevice) (transfer none): a list of #UrfDevice objects
 *
 * Since: 0.2.0
 **/
GList *
urf_client_get_devices (UrfClient *client)
{
	g_return_val_if_fail (URF_IS_CLIENT (client), NULL);

	return client->priv->devices;
}

/**
 * urf_client_set_block:
 * @client: a #UrfClient instance
 * @type: the type of the devices
 * @block: %TRUE to block the devices or %FALSE to unblock
 * @cancellable: a #GCancellable or %NULL
 * @error: a #GError, or %NULL
 *
 * Block or unblock the devices belonging to the type.
 *
 * <note>
 *   <para>
 *     This function only changes soft block. Hard block is controlled
 *     by BIOS or the hardware and there is no way to change the
 *     state of hard block through kernel functions.
 *   </para>
 * </note>
 *
 * Return value: #TRUE for success, else #FALSE and @error is used
 *
 * Since: 0.2.0
 **/
gboolean
urf_client_set_block (UrfClient      *client,
		      UrfDeviceType   type,
		      const gboolean  block,
		      GCancellable   *cancellable,
		      GError         **error)
{
	gboolean ret, status = FALSE;
	GError *error_local = NULL;

	g_return_val_if_fail (URF_IS_CLIENT (client), FALSE);
	g_return_val_if_fail (client->priv->proxy != NULL, FALSE);
	g_return_val_if_fail (type < NUM_URFDEVICE_TYPES, FALSE);

	ret = dbus_g_proxy_call (client->priv->proxy, "Block", &error_local,
				 G_TYPE_UINT, type,
				 G_TYPE_BOOLEAN, block,
				 G_TYPE_INVALID,
				 G_TYPE_BOOLEAN, &status,
				 G_TYPE_INVALID);
	if (!ret) {
		g_warning ("Couldn't sent BLOCK: %s", error_local->message);
		g_set_error (error, 1, 0, "%s", error_local->message);
		status = FALSE;
	}
	if (error_local != NULL)
		g_error_free (error_local);
	return status;
}

/**
 * urf_client_set_block_idx:
 * @client: a #UrfClient instance
 * @index: the index of the device
 * @block: %TRUE to block the device or %FALSE to unblock
 * @cancellable: a #GCancellable or %NULL
 * @error: a #GError, or %NULL
 *
 * Block or unblock the device by the index.
 *
 * <note>
 *   <para>
 *     This function only changes soft block. Hard block is controlled
 *     by BIOS or the hardware and there is no way to change the
 *     state of hard block through kernel functions.
 *   </para>
 * </note>
 *
 * Return value: #TRUE for success, else #FALSE and @error is used
 *
 * Since: 0.2.0
 **/
gboolean
urf_client_set_block_idx (UrfClient      *client,
			  const guint     index,
			  const gboolean  block,
			  GCancellable   *cancellable,
			  GError         **error)
{
	gboolean ret, status;
	GError *error_local = NULL;

	g_return_val_if_fail (URF_IS_CLIENT (client), FALSE);
	g_return_val_if_fail (client->priv->proxy != NULL, FALSE);

	ret = dbus_g_proxy_call (client->priv->proxy, "BlockIdx", &error_local,
				 G_TYPE_UINT, index,
				 G_TYPE_BOOLEAN, block,
				 G_TYPE_INVALID,
				 G_TYPE_BOOLEAN, &status,
				 G_TYPE_INVALID);
	if (!ret) {
		g_warning ("Couldn't sent BLOCKIDX: %s", error_local->message);
		g_set_error (error, 1, 0, "%s", error_local->message);
		status = FALSE;
	}
	if (error_local != NULL)
		g_error_free (error_local);
	return status;
}

/**
 * urf_client_is_inhibited:
 * @client: a #UrfClient instance
 * @error: a #GError, or %NULL
 *
 * Get whether the key control is inhibited or not,
 *
 * Return value: #TRUE if the key control is inhibited
 *
 * Since: 0.2.0
 **/
gboolean
urf_client_is_inhibited (UrfClient *client,
			 GError    **error)
{
	gboolean ret, is_inhibited;
	GError *error_local = NULL;

	g_return_val_if_fail (URF_IS_CLIENT (client), FALSE);
	g_return_val_if_fail (client->priv->proxy != NULL, FALSE);

	ret = dbus_g_proxy_call (client->priv->proxy, "IsInhibited", &error_local,
				 G_TYPE_INVALID,
				 G_TYPE_BOOLEAN, &is_inhibited,
				 G_TYPE_INVALID);
	if (!ret) {
		g_warning ("Couldn't sent IsInhibited: %s", error_local->message);
		g_set_error (error, 1, 0, "%s", error_local->message);
		is_inhibited = FALSE;
	}
	if (error_local != NULL)
		g_error_free (error_local);
	return is_inhibited;

}

/**
 * urf_client_inhibit:
 * @client: a #UrfClient instance
 * @reason: the reason to inhibit the key control
 * @error: a #GError, or %NULL
 *
 * Inhibit the rfkill key handling function for this session.
 *
 * Return value: the cookie and @error is used
 *
 * Since: 0.2.0
 **/
guint
urf_client_inhibit (UrfClient  *client,
		    const char *reason,
		    GError     **error)
{
	GError *error_local = NULL;
	gboolean ret;
	guint cookie = 0;

	if (!URF_IS_CLIENT (client) || client->priv->proxy == NULL) {
		error_local = g_error_new (URF_CLIENT_ERROR,
					   URF_CLIENT_ERROR_GENERAL,
					   "Not a vaild UrfClient instance");
		g_warning ("Inhibit: %s", error_local->message);
		g_set_error (error, 1, 0, "%s", error_local->message);
		goto out;
	}

	ret = dbus_g_proxy_call (client->priv->proxy, "Inhibit", &error_local,
				 G_TYPE_STRING, reason,
				 G_TYPE_INVALID,
				 G_TYPE_UINT, &cookie,
				 G_TYPE_INVALID);
	if (!ret) {
		g_warning ("Couldn't sent INHIBIT: %s", error_local->message);
		g_set_error (error, 1, 0, "%s", error_local->message);
		goto out;
	}
out:
	if (error_local != NULL)
		g_error_free (error_local);
	return cookie;
}

/**
 * urf_client_uninhibit:
 * @client: a #UrfClient instance
 * @cookie: the cookie
 *
 * Cancel a previous call to #urf_client_inhibit identified by the cookie.
 *
 * Since: 0.2.0
 **/
void
urf_client_uninhibit (UrfClient   *client,
		      const guint  cookie)
{
	g_return_if_fail (URF_IS_CLIENT (client));
	g_return_if_fail (client->priv->proxy != NULL);

	dbus_g_proxy_call_no_reply (client->priv->proxy, "Uninhibit",
				    G_TYPE_UINT, cookie,
				    G_TYPE_INVALID,
				    G_TYPE_INVALID);
}


/**
 * urf_client_set_wlan_block:
 * @client: a #UrfClient instance
 * @block: %TRUE to block the WLAN devices or %FALSE to unblock
 *
 * Block or unblock the WLAN devices. This is a convenient function
 * and the underlying function is #urf_client_set_block.
 *
 * Return value: #TRUE for success, else #FALSE
 *
 * Since: 0.2.0
 **/
gboolean
urf_client_set_wlan_block (UrfClient     *client,
			   const gboolean block)
{
	return urf_client_set_block (client, URFDEVICE_TYPE_WLAN, block, NULL, NULL);
}

/**
 * urf_client_set_bluetooth_block:
 * @client: a #UrfClient instance
 * @block: %TRUE to block the bluetooth devices or %FALSE to unblock
 *
 * Block or unblock the bluetooth devices. This is a convenient function
 * and the underlying function is #urf_client_set_block.
 *
 * Return value: #TRUE for success, else #FALSE
 *
 * Since: 0.2.0
 **/
gboolean
urf_client_set_bluetooth_block (UrfClient     *client,
				const gboolean block)
{
	return urf_client_set_block (client, URFDEVICE_TYPE_BLUETOOTH, block, NULL, NULL);
}

/**
 * urf_client_set_wwan_block:
 * @client: a #UrfClient instance
 * @block: %TRUE to block the WWAN devices or %FALSE to unblock
 *
 * Block or unblock the wireless WAN devices. This is a convenient function
 * and the underlying function is #urf_client_set_block.
 *
 * Return value: #TRUE for success, else #FALSE
 *
 * Since: 0.2.0
 **/
gboolean
urf_client_set_wwan_block (UrfClient     *client,
			   const gboolean block)
{
	return urf_client_set_block (client, URFDEVICE_TYPE_WWAN, block, NULL, NULL);
}

/**
 * urf_client_get_properties_sync:
 **/
static gboolean
urf_client_get_properties_sync (UrfClient    *client,
				GCancellable *cancellable,
				GError       **error)
{
	gboolean ret = TRUE;
	GHashTable *props;
	GValue *value;

	props = NULL;

	if (client->priv->have_properties)
		goto out;
	if (!client->priv->prop_proxy)
		goto out;

	error = NULL;
	ret = dbus_g_proxy_call (client->priv->prop_proxy, "GetAll", error,
				 G_TYPE_STRING, "org.freedesktop.URfkill",
				 G_TYPE_INVALID,
				 dbus_g_type_get_map ("GHashTable", G_TYPE_STRING, G_TYPE_VALUE), &props,
				 G_TYPE_INVALID);

	if (!ret)
		goto out;

	value = g_hash_table_lookup (props, "DaemonVersion");
	if (value == NULL) {
		g_warning ("No 'DaemonVersion' property");
		goto out;
	}
	client->priv->daemon_version = g_strdup (g_value_get_string (value));

	value = g_hash_table_lookup (props, "KeyControl");
	if (value == NULL) {
		g_warning ("No 'KeyControl' property");
		goto out;
	}
	client->priv->key_control = g_value_get_boolean (value);

	/* All done */
	client->priv->have_properties = TRUE;
out:
	if (props != NULL)
		g_hash_table_unref (props);
	return ret;
}

/**
 * urf_client_get_daemon_version:
 * @client: a #UrfClient instance
 *
 * Get urfkill daemon version
 *
 * Return value: string containing the daemon version, e.g. 0.2.0
 *
 * Since: 0.2.0
 **/
const char *
urf_client_get_daemon_version (UrfClient *client)
{
	g_return_val_if_fail (URF_IS_CLIENT (client), NULL);
	urf_client_get_properties_sync (client, NULL, NULL);
	return client->priv->daemon_version;
}

/**
 * urf_client_get_key_control:
 **/
static gboolean
urf_client_get_key_control (UrfClient *client)
{
	g_return_val_if_fail (URF_IS_CLIENT (client), FALSE);
	urf_client_get_properties_sync (client, NULL, NULL);
	return client->priv->key_control;
}

/**
 * urf_client_add:
 **/
static UrfDevice *
urf_client_add (UrfClient  *client,
		const char *object_path)
{
	UrfDevice *device;

	device = urf_device_new ();
	urf_device_set_object_path_sync (device, object_path, NULL, NULL);

	client->priv->devices = g_list_append (client->priv->devices, device);

	return device;
}

/**
 * urf_client_device_added_cb:
 **/
static void
urf_client_device_added_cb (DBusGProxy  *proxy,
			    const gchar *object_path,
			    UrfClient   *client)
{
	UrfDevice *device;

	device = urf_client_find_device (client, object_path);
	if (device != NULL) {
		g_warning ("already added: %s", object_path);
		return;
	}

	device = urf_client_add (client, object_path);

	g_signal_emit (client, signals [URF_CLIENT_DEVICE_ADDED], 0, device);
}

/**
 * urf_client_device_removed_cb:
 **/
static void
urf_client_device_removed_cb (DBusGProxy *proxy,
			      const char *object_path,
			      UrfClient  *client)
{
	UrfClientPrivate *priv = client->priv;
	UrfDevice *device;

	device = urf_client_find_device (client, object_path);

	if (device == NULL) {
		g_warning ("no such device to be removed: %s", object_path);
		return;
	}

	client->priv->devices = g_list_remove (priv->devices, device);

	g_signal_emit (client, signals [URF_CLIENT_DEVICE_REMOVED], 0, device);

	g_object_unref (device);
}

/**
 * urf_client_device_changed_cb:
 **/
static void
urf_client_device_changed_cb (DBusGProxy     *proxy,
			      const char     *object_path,
			      UrfClient      *client)
{
	UrfDevice *device;

	device = urf_client_find_device (client, object_path);

	if (device == NULL) {
		g_warning ("no device to be changed: %s", object_path);
		return;
	}

	g_signal_emit (client, signals [URF_CLIENT_DEVICE_CHANGED], 0, device);
}

/**
 * urf_client_rf_key_pressed_cb:
 **/
static void
urf_client_rf_key_pressed_cb (DBusGProxy *proxy,
			      const int   keycode,
			      UrfClient  *client)
{
	g_signal_emit (client, signals [URF_CLIENT_RF_KEY_PRESSED], 0, keycode);
}

/**
 * urf_client_get_devices_private:
 **/
static void
urf_client_get_devices_private (UrfClient *client,
				GError    **error)
{
	GError *error_local = NULL;
	GType g_type_array;
	GPtrArray *devices = NULL;
	const char *object_path;
	gboolean ret;
	guint i;

	g_return_if_fail (URF_IS_CLIENT (client));
	g_return_if_fail (client->priv->proxy != NULL);

	g_type_array = dbus_g_type_get_collection ("GPtrArray", DBUS_TYPE_G_OBJECT_PATH);
	ret = dbus_g_proxy_call (client->priv->proxy, "EnumerateDevices", &error_local,
				 G_TYPE_INVALID,
				 g_type_array, &devices,
				 G_TYPE_INVALID);
	if (!ret) {
		g_set_error (error, 1, 0, "%s", error_local->message);
		g_error_free (error_local);
		return;
	}

	/* no data */
	if (devices == NULL) {
		g_set_error_literal (error, 1, 0, "no data");
		return;
	}

	/* convert */
	for (i=0; i < devices->len; i++) {
		object_path = (const char *) g_ptr_array_index (devices, i);
		urf_client_add (client, object_path);
	}
}

/**
 * urf_client_get_property:
 **/
static void
urf_client_get_property (GObject    *object,
			 guint       prop_id,
			 GValue     *value,
			 GParamSpec *pspec)
{
	UrfClient *client = URF_CLIENT (object);

	urf_client_get_properties_sync (client, NULL, NULL);

	switch (prop_id) {
	case PROP_DAEMON_VERSION:
		g_value_set_string (value, urf_client_get_daemon_version (client));
		break;
	case PROP_KEY_CONTROL:
		g_value_set_boolean (value, urf_client_get_key_control (client));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

/**
 * urf_client_error_quark:
 **/
GQuark
urf_client_error_quark (void)
{
	static GQuark ret = 0;
	if (ret == 0)
		ret = g_quark_from_static_string ("urf_client_error");
	return ret;
}

#define ENUM_ENTRY(NAME, DESC) { NAME, "" #NAME "", DESC }
/**
 * urf_client_error_get_type:
 **/
GType
urf_client_error_get_type (void)
{
	static GType etype = 0;

	if (etype == 0) {
		static const GEnumValue values[] = {
			ENUM_ENTRY (URF_CLIENT_ERROR_GENERAL, "GeneralError"),
			{ 0, 0, 0 }
		};
		g_assert (URF_CLIENT_NUM_ERRORS == G_N_ELEMENTS (values) - 1);
		etype = g_enum_register_static ("UrfClientError", values);
	}
	return etype;
}

/**
 * urf_client_class_init:
 * @klass: The UrfClientClass
 **/
static void
urf_client_class_init (UrfClientClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->get_property = urf_client_get_property;
	object_class->dispose = urf_client_dispose;
	object_class->finalize = urf_client_finalize;

	/**
	 * UrfClient:daemon-version:
	 *
	 * The running daemon version.
	 *
	 * Since: 0.2.0
	 **/
	g_object_class_install_property (object_class,
					 PROP_DAEMON_VERSION,
					 g_param_spec_string ("daemon-version",
							      "Daemon version",
							      "The running daemon version",
							      NULL,
							      G_PARAM_READABLE));

	/**
	 * UrfClient:key-control:
	 *
	 * Whether the key control in the daemon is enabled or not
	 *
	 * Since: 0.2.0
	 **/
	g_object_class_install_property (object_class,
					 PROP_KEY_CONTROL,
					 g_param_spec_boolean ("key-control",
							       "Key Control",
							       "The key control state",
							       FALSE,
							       G_PARAM_READABLE));

	/* install signals */
	/**
	 * UrfClient::device-added:
	 * @client: the #UrfClient instance that emitted the signa
	 * @device: the #UrfDevice that was added.
	 *
	 * The device-added signal is emitted when a rfkill device is added.
	 *
	 * Since 0.2.0
	 **/
        signals[URF_CLIENT_DEVICE_ADDED] =
                g_signal_new ("device-added",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (UrfClientClass, device_added),
			      NULL, NULL, g_cclosure_marshal_VOID__OBJECT,
			      G_TYPE_NONE, 1, URF_TYPE_DEVICE);

	/**
	 * UrfClient::device-removed:
	 * @client: the #UrfClient instance that emitted the signa
	 * @device: the #UrfDevice that was removed.
	 *
	 * The device-removed signal is emitted when a rfkill device is removed.
	 *
	 * Since 0.2.0
	 **/
        signals[URF_CLIENT_DEVICE_REMOVED] =
                g_signal_new ("device-removed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (UrfClientClass, device_removed),
			      NULL, NULL, g_cclosure_marshal_VOID__OBJECT,
			      G_TYPE_NONE, 1, URF_TYPE_DEVICE);

	/**
	 * UrfClient::device-changed:
	 * @client: the #UrfClient instance that emitted the signa
	 * @device: the #UrfDevice that was changed.
	 *
	 * The device-changed signal is emitted when a rfkill device is changed.
	 *
	 * Since 0.2.0
	 **/
        signals[URF_CLIENT_DEVICE_CHANGED] =
                g_signal_new ("device-changed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (UrfClientClass, device_changed),
			      NULL, NULL, g_cclosure_marshal_VOID__OBJECT,
			      G_TYPE_NONE, 1, URF_TYPE_DEVICE);

	/**
	 * UrfClient::rf-key-pressed:
	 * @client: the #UrfClient instance that emitted the signa
	 * @keycode: the keycode from the input device
	 *
	 * The rf-key-pressed signal is emitted when a rfkill key is pressed.
	 *
	 * Since 0.2.0
	 **/
        signals[URF_CLIENT_RF_KEY_PRESSED] =
                g_signal_new ("rf-key-pressed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (UrfClientClass, rf_key_pressed),
			      NULL, NULL, g_cclosure_marshal_VOID__INT,
			      G_TYPE_NONE, 1, G_TYPE_INT);

	g_type_class_add_private (klass, sizeof (UrfClientPrivate));
}

/**
 * urf_client_init:
 * @client: This class instance
 **/
static void
urf_client_init (UrfClient *client)
{
	GError *error = NULL;

	client->priv = URF_CLIENT_GET_PRIVATE (client);
	client->priv->daemon_version = NULL;
	client->priv->key_control = FALSE;
	client->priv->have_properties = FALSE;

	/* get on the bus */
	client->priv->bus = dbus_g_bus_get (DBUS_BUS_SYSTEM, &error);
	if (client->priv->bus == NULL) {
		g_warning ("Couldn't connect to system bus: %s", error->message);
		g_error_free (error);
		goto out;
	}

	/* connect to main interface */
	client->priv->proxy = dbus_g_proxy_new_for_name (client->priv->bus,
							 "org.freedesktop.URfkill",
							 "/org/freedesktop/URfkill",
							 "org.freedesktop.URfkill");
	if (client->priv->proxy == NULL) {
		g_warning ("Couldn't connect to proxy");
		goto out;
	}

	/* connect to main interface */
	client->priv->prop_proxy = dbus_g_proxy_new_for_name (client->priv->bus,
							      "org.freedesktop.URfkill",
							      "/org/freedesktop/URfkill",
							      "org.freedesktop.DBus.Properties");
	if (client->priv->prop_proxy == NULL) {
		g_warning ("Couldn't connect to proxy");
		goto out;
	}

	client->priv->devices = NULL;

	urf_client_get_devices_private (client, &error);
	if (error) {
		g_warning ("Failed to enumerate devices: %s", error->message);
		g_error_free (error);
		goto out;
	}

	/* connect signals */
	dbus_g_proxy_add_signal (client->priv->proxy, "DeviceAdded",
				 G_TYPE_STRING,
				 G_TYPE_INVALID);
	dbus_g_proxy_add_signal (client->priv->proxy, "DeviceRemoved",
				 G_TYPE_STRING,
				 G_TYPE_INVALID);
	dbus_g_proxy_add_signal (client->priv->proxy, "DeviceChanged",
				 G_TYPE_STRING,
				 G_TYPE_INVALID);
	dbus_g_proxy_add_signal (client->priv->proxy, "UrfkeyPressed",
				 G_TYPE_INT,
				 G_TYPE_INVALID);
	/* callbacks */
	dbus_g_proxy_connect_signal (client->priv->proxy, "DeviceAdded",
				     G_CALLBACK (urf_client_device_added_cb), client, NULL);
	dbus_g_proxy_connect_signal (client->priv->proxy, "DeviceRemoved",
				     G_CALLBACK (urf_client_device_removed_cb), client, NULL);
	dbus_g_proxy_connect_signal (client->priv->proxy, "DeviceChanged",
				     G_CALLBACK (urf_client_device_changed_cb), client, NULL);
	dbus_g_proxy_connect_signal (client->priv->proxy, "UrfkeyPressed",
				     G_CALLBACK (urf_client_rf_key_pressed_cb), client, NULL);

out:
	return;
}

/**
 * urf_client_dispose:
 **/
static void
urf_client_dispose (GObject *object)
{
	UrfClient *client;

	g_return_if_fail (URF_IS_CLIENT (object));

	client = URF_CLIENT (object);

	if (client->priv->bus) {
		dbus_g_connection_unref (client->priv->bus);
		client->priv->bus = NULL;
	}

	if (client->priv->proxy) {
		g_object_unref (client->priv->proxy);
		client->priv->proxy = NULL;
	}

	if (client->priv->prop_proxy) {
		g_object_unref (client->priv->prop_proxy);
		client->priv->prop_proxy = NULL;
	}

	G_OBJECT_CLASS (urf_client_parent_class)->dispose (object);
}

/**
 * urf_client_finalize:
 **/
static void
urf_client_finalize (GObject *object)
{
	UrfClient *client;
	GList *item;

	g_return_if_fail (URF_IS_CLIENT (object));

	client = URF_CLIENT (object);

	g_free (client->priv->daemon_version);

	if (client->priv->devices) {
		for (item = client->priv->devices; item; item = item->next)
			g_object_unref (item->data);
		g_list_free (client->priv->devices);
		client->priv->devices = NULL;
	}

	G_OBJECT_CLASS (urf_client_parent_class)->finalize (object);
}

/**
 * urf_client_new:
 *
 * Creates a new #UrfClient object.
 *
 * Return value: a new #UrfClient object.
 *
 **/
UrfClient *
urf_client_new (void)
{
	if (urf_client_object != NULL) {
		g_object_ref (urf_client_object);
	} else {
		urf_client_object = g_object_new (URF_TYPE_CLIENT, NULL);
		g_object_add_weak_pointer (urf_client_object, &urf_client_object);
	}
	return URF_CLIENT (urf_client_object);
}

