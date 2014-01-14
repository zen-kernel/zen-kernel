/*
 * Copyright (C) 2013 Kay Sievers
 * Copyright (C) 2013 Greg Kroah-Hartman <gregkh@linuxfoundation.org>
 * Copyright (C) 2013 Daniel Mack <daniel@zonque.org>
 * Copyright (C) 2013 Linux Foundation
 *
 * kdbus is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/slab.h>

#include "message.h"
#include "notify.h"

static int kdbus_notify_reply(u64 id, u64 cookie, u64 msg_type,
			      struct list_head *queue_list)
{
	struct kdbus_kmsg *kmsg = NULL;
	int ret;

	BUG_ON(id == 0);

	ret = kdbus_kmsg_new(0, &kmsg);
	if (ret < 0)
		return ret;

	/*
	 * a kernel-generated notification can only contain one
	 * struct kdbus_item, so make a shortcut here for
	 * faster lookup in the match db.
	 */
	kmsg->notify_type = msg_type;
	kmsg->msg.dst_id = id;
	kmsg->msg.src_id = KDBUS_SRC_ID_KERNEL;
	kmsg->msg.payload_type = KDBUS_PAYLOAD_KERNEL;
	kmsg->msg.cookie_reply = cookie;
	kmsg->msg.items[0].type = msg_type;

	list_add_tail(&kmsg->queue_entry, queue_list);
	return ret;
}

/**
 * kdbus_notify_reply_timeout() - queue a timeout reply
 * @id:			The destination's connection ID
 * @cookie:		The cookie to set in the reply.
 * @queue_list:		A queue list for the newly generated kdbus_kmsg.
 *			The caller has to free all items in the list using
 *			kdbus_kmsg_free(). Maybe NULL, in which case this
 *			function does nothing.
 *
 * Queues a message that has a KDBUS_ITEM_REPLY_TIMEOUT item attached.
 *
 * Returns: 0 on success, negative errno on failure.
 */
int kdbus_notify_reply_timeout(u64 id, u64 cookie, struct list_head *queue_list)
{
	return kdbus_notify_reply(id, cookie, KDBUS_ITEM_REPLY_TIMEOUT,
				  queue_list);
}

/**
 * kdbus_notify_reply_dead() - queue a 'dead' reply
 * @id:			The destination's connection ID
 * @cookie:		The cookie to set in the reply.
 * @queue_list:		A queue list for the newly generated kdbus_kmsg.
 *			The caller has to free all items in the list using
 *			kdbus_kmsg_free(). Maybe NULL, in which case this
 *			function does nothing.
 *
 * Queues a message that has a KDBUS_ITEM_REPLY_DEAD item attached.
 *
 * Returns: 0 on success, negative errno on failure.
 */
int kdbus_notify_reply_dead(u64 id, u64 cookie, struct list_head *queue_list)
{
	return kdbus_notify_reply(id, cookie, KDBUS_ITEM_REPLY_DEAD,
				  queue_list);
}

/**
 * kdbus_notify_name_change() - queue a notification about a name owner change
 * @type:		The type if the notification; KDBUS_ITEM_NAME_ADD,
 *			KDBUS_ITEM_NAME_CHANGE or KDBUS_ITEM_NAME_REMOVE
 * @old_id:		The id of the connection that used to own the name
 * @new_id:		The id of the new owner connection
 * @old_flags:		The flags to pass in the KDBUS_ITEM flags field for
 *			the old owner
 * @new_flags:		The flags to pass in the KDBUS_ITEM flags field for
 *			the new owner
 * @name:		The name that was removed or assigned to a new owner
 * @queue_list:		A queue list for the newly generated kdbus_kmsg.
 *			The caller has to free all items in the list using
 *			kdbus_kmsg_free(). Maybe NULL, in which case this
 *			function does nothing.
 *
 * Returns: 0 on success, negative errno on failure.
 */
int kdbus_notify_name_change(u64 type,
			     u64 old_id, u64 new_id,
			     u64 old_flags, u64 new_flags,
			     const char *name,
			     struct list_head *queue_list)
{
	struct kdbus_kmsg *kmsg = NULL;
	size_t name_len;
	size_t extra_size;
	int ret;

	if (!queue_list)
		return 0;

	name_len = strlen(name) + 1;
	extra_size = sizeof(struct kdbus_notify_name_change) + name_len;
	ret = kdbus_kmsg_new(extra_size, &kmsg);
	if (ret < 0)
		return ret;

	kmsg->msg.dst_id = KDBUS_DST_ID_BROADCAST;
	kmsg->msg.src_id = KDBUS_SRC_ID_KERNEL;
	kmsg->notify_type = type;
	kmsg->notify_old_id = old_id;
	kmsg->notify_new_id = new_id;
	kmsg->msg.items[0].type = type;
	kmsg->msg.items[0].name_change.old.id = old_id;
	kmsg->msg.items[0].name_change.old.flags = old_flags;
	kmsg->msg.items[0].name_change.new.id = new_id;
	kmsg->msg.items[0].name_change.new.flags = new_flags;
	memcpy(kmsg->msg.items[0].name_change.name, name, name_len);
	kmsg->notify_name = kmsg->msg.items[0].name_change.name;

	list_add_tail(&kmsg->queue_entry, queue_list);
	return ret;
}

/**
 * kdbus_notify_id_change() - queue a notification about a unique ID change
 * @type:		The type if the notification; KDBUS_ITEM_ID_ADD or
 *			KDBUS_ITEM_ID_REMOVE
 * @id:			The id of the connection that was added or removed
 * @flags:		The flags to pass in the KDBUS_ITEM flags field
 * @queue_list:		A queue list for the newly generated kdbus_kmsg.
 *			The caller has to free all items in the list using
 *			kdbus_kmsg_free(). Maybe NULL, in which case this
 *			function does nothing.
 *
 * Returns: 0 on success, negative errno on failure.
 */
int kdbus_notify_id_change(u64 type, u64 id, u64 flags,
			   struct list_head *queue_list)
{
	struct kdbus_kmsg *kmsg = NULL;
	int ret;

	ret = kdbus_kmsg_new(sizeof(struct kdbus_notify_id_change), &kmsg);
	if (ret < 0)
		return ret;

	kmsg->msg.dst_id = KDBUS_DST_ID_BROADCAST;
	kmsg->msg.src_id = KDBUS_SRC_ID_KERNEL;
	kmsg->notify_type = type;

	switch (type) {
	case KDBUS_ITEM_ID_ADD:
		kmsg->notify_new_id = id;
		break;

	case KDBUS_ITEM_ID_REMOVE:
		kmsg->notify_old_id = id;
		break;

	default:
		BUG();
	}

	kmsg->msg.items[0].type = type;
	kmsg->msg.items[0].id_change.id = id;
	kmsg->msg.items[0].id_change.flags = flags;

	list_add_tail(&kmsg->queue_entry, queue_list);
	return ret;
}
