/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2005-2008 Marcel Holtmann <marcel@holtmann.org>
 * Copyright (C) 2006-2009 Bastien Nocera <hadess@hadess.net>
 * Copyright (C) 2010-2011 Gary Ching-Pang Lin <glin@suse.com>
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
#include <config.h>
#endif

#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>

#include <glib.h>

#include <linux/rfkill.h>

#ifndef RFKILL_EVENT_SIZE_V1
#define RFKILL_EVENT_SIZE_V1    8
#endif

#include "urf-killswitch.h"
#include "urf-utils.h"

enum {
	DEVICE_ADDED,
	DEVICE_REMOVED,
	DEVICE_CHANGED,
	LAST_SIGNAL
};

static int signals[LAST_SIGNAL] = { 0 };

#define URF_KILLSWITCH_GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE((obj), \
                                URF_TYPE_KILLSWITCH, UrfKillswitchPrivate))

struct UrfKillswitchPrivate {
	int		 fd;
	gboolean	 force_sync;
	GIOChannel	*channel;
	guint		 watch_id;
	GList		*devices; /* a GList of UrfDevice */
	UrfDevice	*type_pivot[NUM_RFKILL_TYPES];
};

G_DEFINE_TYPE(UrfKillswitch, urf_killswitch, G_TYPE_OBJECT)

static KillswitchState
event_to_state (gboolean soft,
		gboolean hard)
{
	if (hard)
		return KILLSWITCH_STATE_HARD_BLOCKED;
	else if (soft)
		return KILLSWITCH_STATE_SOFT_BLOCKED;
	else
		return KILLSWITCH_STATE_UNBLOCKED;
}

static const char *
state_to_string (KillswitchState state)
{
	switch (state) {
	case KILLSWITCH_STATE_NO_ADAPTER:
		return "KILLSWITCH_STATE_NO_ADAPTER";
	case KILLSWITCH_STATE_SOFT_BLOCKED:
		return "KILLSWITCH_STATE_SOFT_BLOCKED";
	case KILLSWITCH_STATE_UNBLOCKED:
		return "KILLSWITCH_STATE_UNBLOCKED";
	case KILLSWITCH_STATE_HARD_BLOCKED:
		return "KILLSWITCH_STATE_HARD_BLOCKED";
	default:
		g_assert_not_reached ();
	}
}

static const char *
type_to_string (unsigned int type)
{
	switch (type) {
	case RFKILL_TYPE_ALL:
		return "ALL";
	case RFKILL_TYPE_WLAN:
		return "WLAN";
	case RFKILL_TYPE_BLUETOOTH:
		return "BLUETOOTH";
	case RFKILL_TYPE_UWB:
		return "UWB";
	case RFKILL_TYPE_WIMAX:
		return "WIMAX";
	case RFKILL_TYPE_WWAN:
		return "WWAN";
	case RFKILL_TYPE_GPS:
		return "GPS";
	case RFKILL_TYPE_FM:
		return "FM";
	default:
		g_assert_not_reached ();
	}
}

/**
 * urf_killswitch_find_device:
 **/
static UrfDevice *
urf_killswitch_find_device (UrfKillswitch *killswitch,
			    guint          index)
{
	UrfKillswitchPrivate *priv = killswitch->priv;
	UrfDevice *device;
	GList *item;

	for (item = priv->devices; item != NULL; item = item->next) {
		device = (UrfDevice *)item->data;
		if (urf_device_get_index (device) == index)
			return device;
	}

	return NULL;
}

/**
 * urf_killswitch_set_block:
 **/
gboolean
urf_killswitch_set_block (UrfKillswitch  *killswitch,
			  const guint     type,
			  const gboolean  block)
{
	UrfKillswitchPrivate *priv = killswitch->priv;
	struct rfkill_event event;
	ssize_t len;

	g_return_val_if_fail (type < NUM_RFKILL_TYPES, FALSE);

	memset (&event, 0, sizeof(event));
	event.op = RFKILL_OP_CHANGE_ALL;
	event.type = type;
	event.soft = block;

	g_debug ("Set %s to %s", type_to_string (type), block?"block":"unblock");
	len = write (priv->fd, &event, sizeof(event));
	if (len < 0) {
		g_warning ("Failed to change RFKILL state: %s",
			   g_strerror (errno));
		return FALSE;
	}
	return TRUE;
}

/**
 * urf_killswitch_set_block_idx:
 **/
gboolean
urf_killswitch_set_block_idx (UrfKillswitch  *killswitch,
			      const guint     index,
			      const gboolean  block)
{
	UrfKillswitchPrivate *priv = killswitch->priv;
	UrfDevice *device;
	struct rfkill_event event;
	ssize_t len;

	device = urf_killswitch_find_device (killswitch, index);
	if (device == NULL) {
		g_warning ("Block index: No device with index %u", index);
		return FALSE;
	}

	memset (&event, 0, sizeof(event));
	event.op = RFKILL_OP_CHANGE;
	event.idx = index;
	event.soft = block;

	g_debug ("Set device %u to %s", index, block?"block":"unblock");
	len = write (priv->fd, &event, sizeof(event));
	if (len < 0) {
		g_warning ("Failed to change RFKILL state: %s",
			   g_strerror (errno));
		return FALSE;
	}
	return TRUE;
}

static KillswitchState
aggregate_pivot_state (UrfKillswitch *killswitch)
{
	UrfKillswitchPrivate *priv = killswitch->priv;
	UrfDevice *device;
	int state = KILLSWITCH_STATE_NO_ADAPTER;
	int i;
	gboolean soft, hard;

	for (i = 0; i < NUM_RFKILL_TYPES; i++) {
		if (!priv->type_pivot[i])
			continue;

		device = priv->type_pivot[i];
		soft = urf_device_get_soft (device);
		hard = urf_device_get_hard (device);
		switch (event_to_state (soft, hard)) {
		case KILLSWITCH_STATE_UNBLOCKED:
			if (state == KILLSWITCH_STATE_NO_ADAPTER)
				state = KILLSWITCH_STATE_UNBLOCKED;
			break;
		case KILLSWITCH_STATE_SOFT_BLOCKED:
			state = KILLSWITCH_STATE_SOFT_BLOCKED;
			break;
		case KILLSWITCH_STATE_HARD_BLOCKED:
			return KILLSWITCH_STATE_HARD_BLOCKED;
		default:
			break;
		}
	}

	return state;
}

/**
 * urf_killswitch_get_state:
 **/
KillswitchState
urf_killswitch_get_state (UrfKillswitch *killswitch,
			  guint          type)
{
	UrfKillswitchPrivate *priv;
	UrfDevice *device;
	int state = KILLSWITCH_STATE_NO_ADAPTER;

	g_return_val_if_fail (URF_IS_KILLSWITCH (killswitch), state);
	g_return_val_if_fail (type < NUM_RFKILL_TYPES, state);

	priv = killswitch->priv;

	if (priv->devices == NULL)
		return KILLSWITCH_STATE_NO_ADAPTER;

	if (type == RFKILL_TYPE_ALL)
		return aggregate_pivot_state (killswitch);

	if (priv->type_pivot[type]) {
		device = priv->type_pivot[type];
		state = event_to_state (urf_device_get_soft (device),
					urf_device_get_hard (device));
	}

	g_debug ("devices %s state %s",
		 type_to_string (type), state_to_string (state));

	return state;
}

/**
 * urf_killswitch_get_state_idx:
 **/
KillswitchState
urf_killswitch_get_state_idx (UrfKillswitch *killswitch,
			      guint          index)
{
	UrfKillswitchPrivate *priv;
	UrfDevice *device;
	int state = KILLSWITCH_STATE_NO_ADAPTER;
	gboolean soft, hard;

	g_return_val_if_fail (URF_IS_KILLSWITCH (killswitch), state);

	priv = killswitch->priv;

	if (priv->devices == NULL)
		return state;

	device = urf_killswitch_find_device (killswitch, index);
	if (device) {
		soft = urf_device_get_soft (device);
		hard = urf_device_get_hard (device);
		state = event_to_state (soft, hard);
		g_debug ("killswitch %d is %s", index, state_to_string (state));
	}

	return state;
}

/**
 * urf_killswitch_has_devices:
 **/
gboolean
urf_killswitch_has_devices (UrfKillswitch *killswitch)
{
	g_return_val_if_fail (URF_IS_KILLSWITCH (killswitch), FALSE);

	return (killswitch->priv->devices != NULL);
}

/**
 * urf_killswitch_get_devices:
 **/
GList*
urf_killswitch_get_devices (UrfKillswitch *killswitch)
{
	g_return_val_if_fail (URF_IS_KILLSWITCH (killswitch), NULL);

	return killswitch->priv->devices;
}

/**
 * urf_killswitch_get_killswitch:
 **/
UrfDevice *
urf_killswitch_get_device (UrfKillswitch *killswitch,
			   const guint    index)
{
	UrfDevice *device;

	g_return_val_if_fail (URF_IS_KILLSWITCH (killswitch), NULL);

	device = urf_killswitch_find_device (killswitch, index);
	if (device)
		return URF_DEVICE (g_object_ref (device));

	return NULL;
}

/**
 * update_killswitch:
 **/
static void
update_killswitch (UrfKillswitch *killswitch,
		   guint          index,
		   gboolean       soft,
		   gboolean       hard)
{
	UrfKillswitchPrivate *priv = killswitch->priv;
	UrfDevice *device;
	gboolean changed, old_hard;
	char *object_path;

	device = urf_killswitch_find_device (killswitch, index);
	if (device == NULL) {
		g_warning ("No device with index %u in the list", index);
		return;
	}

	old_hard = urf_device_get_hard (device);
	changed = urf_device_update_states (device, soft, hard);

	if (changed == TRUE) {
		g_debug ("updating killswitch status %d to soft %d hard %d",
			 index, soft, hard);
		object_path = g_strdup (urf_device_get_object_path (device));
		g_signal_emit (G_OBJECT (killswitch), signals[DEVICE_CHANGED], 0, object_path);
		g_free (object_path);

		if (priv->force_sync) {
			/* Sync soft and hard blocks */
			if (hard == TRUE && soft == FALSE)
				urf_killswitch_set_block_idx (killswitch, index, TRUE);
			else if (hard != old_hard && hard == FALSE)
				urf_killswitch_set_block_idx (killswitch, index, FALSE);
		}
	}
}

static void
assign_new_pivot (UrfKillswitch *killswitch,
		  const guint    type)
{
	UrfKillswitchPrivate *priv = killswitch->priv;
	UrfDevice *device;
	const char *name;
	GList *item;

	for (item = priv->devices; item != NULL; item = item->next) {
		device = (UrfDevice *)item->data;
		name = urf_device_get_name (device);
		if (urf_device_get_rf_type (device) == type &&
		    (priv->type_pivot[type] == NULL ||
		     urf_device_is_platform (device))) {
			priv->type_pivot[type] = device;
			g_debug ("assign killswitch idx %d %s as a pivot",
				 urf_device_get_index (device), name);
		}
	}
}

/**
 * remove_killswitch:
 **/
static void
remove_killswitch (UrfKillswitch *killswitch,
		   guint          index)
{
	UrfKillswitchPrivate *priv = killswitch->priv;
	UrfDevice *device;
	guint type;
	const char *name;
	gboolean pivot_changed = FALSE;
	char *object_path = NULL;

	device = urf_killswitch_find_device (killswitch, index);
	if (device == NULL) {
		g_warning ("No device with index %u in the list", index);
		return;
	}

	priv->devices = g_list_remove (priv->devices, device);
	type = urf_device_get_rf_type (device);
	object_path = g_strdup (urf_device_get_object_path(device));

	name = urf_device_get_name (device);
	g_debug ("removing killswitch idx %d %s", index, name);

	if (priv->type_pivot[type] == device) {
		priv->type_pivot[type] = NULL;
		pivot_changed = TRUE;
	}

	g_object_unref (device);

	/* Find the next pivot */
	if (pivot_changed) {
		assign_new_pivot (killswitch, type);
	}
	g_signal_emit (G_OBJECT (killswitch), signals[DEVICE_REMOVED], 0, object_path);
	g_free (object_path);
}

/**
 * add_killswitch:
 **/
static void
add_killswitch (UrfKillswitch *killswitch,
		guint          index,
		guint          type,
		gboolean       soft,
		gboolean       hard)

{
	UrfKillswitchPrivate *priv = killswitch->priv;
	UrfDevice *device;
	const char *name;

	device = urf_killswitch_find_device (killswitch, index);
	if (device != NULL) {
		g_warning ("device with index %u already in the list", index);
		return;
	}

	g_debug ("adding killswitch idx %d soft %d hard %d", index, soft, hard);

	device = urf_device_new (index, type, soft, hard);
	priv->devices = g_list_append (priv->devices, device);

	/* Assume that only one platform vendor in a machine */
	name = urf_device_get_name (device);
	if (priv->type_pivot[type] == NULL || urf_device_is_platform (device)) {
		priv->type_pivot[type] = device;
		g_debug ("assign killswitch idx %d %s as a pivot", index, name);
	}

	g_signal_emit (G_OBJECT (killswitch), signals[DEVICE_ADDED], 0,
		       urf_device_get_object_path (device));
	if (priv->force_sync && priv->type_pivot[type] != device) {
		urf_killswitch_set_block_idx (killswitch, index, soft);
	}
}

static const char *
op_to_string (unsigned int op)
{
	switch (op) {
	case RFKILL_OP_ADD:
		return "ADD";
	case RFKILL_OP_DEL:
		return "DEL";
	case RFKILL_OP_CHANGE:
		return "CHANGE";
	case RFKILL_OP_CHANGE_ALL:
		return "CHANGE_ALL";
	default:
		g_assert_not_reached ();
	}
}

static void
print_event (struct rfkill_event *event)
{
	g_debug ("RFKILL event: idx %u type %u (%s) op %u (%s) soft %u hard %u",
		 event->idx,
		 event->type, type_to_string (event->type),
		 event->op, op_to_string (event->op),
		 event->soft, event->hard);
}

/**
 * event_cb:
 **/
static gboolean
event_cb (GIOChannel    *source,
	  GIOCondition   condition,
	  UrfKillswitch *killswitch)
{
	if (condition & G_IO_IN) {
		GIOStatus status;
		struct rfkill_event event;
		gsize read;
		gboolean soft, hard;

		status = g_io_channel_read_chars (source,
						  (char *) &event,
						  sizeof(event),
						  &read,
						  NULL);

		while (status == G_IO_STATUS_NORMAL && read == sizeof(event)) {
			print_event (&event);

			soft = (event.soft > 0)?TRUE:FALSE;
			hard = (event.hard > 0)?TRUE:FALSE;

			if (event.op == RFKILL_OP_CHANGE) {
				update_killswitch (killswitch, event.idx, soft, hard);
			} else if (event.op == RFKILL_OP_DEL) {
				remove_killswitch (killswitch, event.idx);
			} else if (event.op == RFKILL_OP_ADD) {
				add_killswitch (killswitch, event.idx, event.type, soft, hard);
			}

			status = g_io_channel_read_chars (source,
							  (char *) &event,
							  sizeof(event),
							  &read,
							  NULL);
		}
	} else {
		g_debug ("something else happened");
		return FALSE;
	}

	return TRUE;
}

/**
 * urf_killswitch_startup
 **/
gboolean
urf_killswitch_startup (UrfKillswitch *killswitch,
			UrfConfig     *config)
{
	UrfKillswitchPrivate *priv = killswitch->priv;
	struct rfkill_event event;
	int fd;

	priv->force_sync = urf_config_get_force_sync (config);

	fd = open("/dev/rfkill", O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		if (errno == EACCES)
			g_warning ("Could not open RFKILL control device, please verify your installation");
		return FALSE;
	}

	/* Disable rfkill input */
	ioctl(fd, RFKILL_IOCTL_NOINPUT);

	priv->fd = fd;

	while (1) {
		ssize_t len;

		len = read(fd, &event, sizeof(event));
		if (len < 0) {
			if (errno == EAGAIN)
				break;
			g_debug ("Reading of RFKILL events failed");
			break;
		}

		if (len != RFKILL_EVENT_SIZE_V1) {
			g_warning("Wrong size of RFKILL event\n");
			continue;
		}

		if (event.op != RFKILL_OP_ADD)
			continue;
		if (event.type >= NUM_RFKILL_TYPES)
			continue;

		add_killswitch (killswitch, event.idx, event.type, event.soft, event.hard);
	}

	/* Setup monitoring */
	priv->channel = g_io_channel_unix_new (priv->fd);
	g_io_channel_set_encoding (priv->channel, NULL, NULL);
	priv->watch_id = g_io_add_watch (priv->channel,
					 G_IO_IN | G_IO_HUP | G_IO_ERR,
					 (GIOFunc) event_cb,
					 killswitch);
	return TRUE;
}

/**
 * urf_killswitch_init:
 **/
static void
urf_killswitch_init (UrfKillswitch *killswitch)
{
	UrfKillswitchPrivate *priv = URF_KILLSWITCH_GET_PRIVATE (killswitch);
	int i;

	killswitch->priv = priv;
	priv->devices = NULL;
	priv->fd = -1;

	for (i = 0; i < NUM_RFKILL_TYPES; i++)
		priv->type_pivot[i] = NULL;
}

/**
 * urf_killswitch_finalize:
 **/
static void
urf_killswitch_finalize (GObject *object)
{
	UrfKillswitchPrivate *priv = URF_KILLSWITCH_GET_PRIVATE (object);

	/* cleanup monitoring */
	if (priv->watch_id > 0) {
		g_source_remove (priv->watch_id);
		priv->watch_id = 0;
		g_io_channel_shutdown (priv->channel, FALSE, NULL);
		g_io_channel_unref (priv->channel);
	}
	close(priv->fd);

	g_list_foreach (priv->devices, (GFunc) g_object_unref, NULL);
	g_list_free (priv->devices);
	priv->devices = NULL;

	G_OBJECT_CLASS(urf_killswitch_parent_class)->finalize(object);
}

/**
 * urf_killswitch_class_init:
 **/
static void
urf_killswitch_class_init(UrfKillswitchClass *klass)
{
	GObjectClass *object_class = (GObjectClass *) klass;

	g_type_class_add_private(klass, sizeof(UrfKillswitchPrivate));
	object_class->finalize = urf_killswitch_finalize;

	signals[DEVICE_ADDED] =
		g_signal_new ("device-added",
			      G_TYPE_FROM_CLASS (klass),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (UrfKillswitchClass, device_added),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__STRING,
			      G_TYPE_NONE, 1, G_TYPE_STRING);

	signals[DEVICE_REMOVED] =
		g_signal_new ("device-removed",
			      G_TYPE_FROM_CLASS (klass),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (UrfKillswitchClass, device_removed),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__STRING,
			      G_TYPE_NONE, 1, G_TYPE_STRING);

	signals[DEVICE_CHANGED] =
		g_signal_new ("device-changed",
			      G_TYPE_FROM_CLASS (klass),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (UrfKillswitchClass, device_changed),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__STRING,
			      G_TYPE_NONE, 1, G_TYPE_STRING);

}

/**
 * urf_killswitch_new:
 **/
UrfKillswitch *
urf_killswitch_new (void)
{
	UrfKillswitch *killswitch;
	killswitch = URF_KILLSWITCH(g_object_new (URF_TYPE_KILLSWITCH, NULL));
	return killswitch;
}

