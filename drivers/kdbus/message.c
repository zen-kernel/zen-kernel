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

#include <linux/capability.h>
#include <linux/cgroup.h>
#include <linux/cred.h>
#include <linux/device.h>
#include <linux/file.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "bus.h"
#include "connection.h"
#include "endpoint.h"
#include "match.h"
#include "message.h"
#include "names.h"
#include "policy.h"

#define KDBUS_KMSG_HEADER_SIZE offsetof(struct kdbus_kmsg, msg)

/**
 * kdbus_kmsg_free() - free allocated message
 * @kmsg:		Message
 */
void kdbus_kmsg_free(struct kdbus_kmsg *kmsg)
{
	kdbus_meta_free(kmsg->meta);
	kfree(kmsg);
}

/**
 * kdbus_kmsg_new() - allocate message
 * @extra_size:		additional size to reserve for data
 * @kmsg:			Returned Message
 *
 * Returns: 0 on success, negative errno on failure.
 */
int kdbus_kmsg_new(size_t extra_size, struct kdbus_kmsg **kmsg)
{
	struct kdbus_kmsg *m;
	size_t size;

	BUG_ON(*kmsg);

	size = sizeof(struct kdbus_kmsg) + KDBUS_ITEM_SIZE(extra_size);
	m = kzalloc(size, GFP_KERNEL);
	if (!m)
		return -ENOMEM;

	m->msg.size = size - KDBUS_KMSG_HEADER_SIZE;
	m->msg.items[0].size = KDBUS_ITEM_SIZE(extra_size);

	*kmsg = m;
	return 0;
}

/*
 * kdbus_msg_scan_items - validate incoming data and prepare parsing
 * @conn:		Connection
 * @kmsg:		Message
 *
 * Returns: 0 on success, negative errno on failure.
 */
static int kdbus_msg_scan_items(struct kdbus_conn *conn,
				struct kdbus_kmsg *kmsg)
{
	const struct kdbus_msg *msg = &kmsg->msg;
	const struct kdbus_item *item;
	size_t vecs_size = 0;
	unsigned int items_count = 0;
	bool has_fds = false;
	bool has_name = false;
	bool has_bloom = false;

	KDBUS_ITEM_FOREACH(item, msg, items) {
		size_t payload_size;

		if (!KDBUS_ITEM_VALID(item, msg))
			return -EINVAL;

		if (++items_count > KDBUS_MSG_MAX_ITEMS)
			return -E2BIG;

		payload_size = item->size - KDBUS_ITEM_HEADER_SIZE;

		switch (item->type) {
		case KDBUS_ITEM_PAYLOAD_VEC:
			if (payload_size != sizeof(struct kdbus_vec))
				return -EINVAL;

			/* empty payload is invalid */
			if (item->vec.size == 0)
				return -EINVAL;

			vecs_size += item->vec.size;
			if (vecs_size > KDBUS_MSG_MAX_PAYLOAD_VEC_SIZE &&
			    !kdbus_bus_uid_is_privileged(conn->ep->bus))
				return -EMSGSIZE;

			/* \0-bytes records store only the alignment bytes */
			if (KDBUS_PTR(item->vec.address))
				kmsg->vecs_size += item->vec.size;
			else
				kmsg->vecs_size += item->vec.size % 8;
			kmsg->vecs_count++;
			break;

		case KDBUS_ITEM_PAYLOAD_MEMFD:
			if (payload_size != sizeof(struct kdbus_memfd))
				return -EINVAL;

			/* do not allow to broadcast file descriptors */
			if (msg->dst_id == KDBUS_DST_ID_BROADCAST)
				return -ENOTUNIQ;

			if (item->memfd.fd < 0)
				return -EBADF;

			/* empty payload is invalid */
			if (item->memfd.size == 0)
				return -EINVAL;

			kmsg->memfds_count++;
			break;

		case KDBUS_ITEM_FDS: {
			unsigned int n;

			/* do not allow multiple fd arrays */
			if (has_fds)
				return -EEXIST;
			has_fds = true;

			/* do not allow to broadcast file descriptors */
			if (msg->dst_id == KDBUS_DST_ID_BROADCAST)
				return -ENOTUNIQ;

			n = payload_size / sizeof(int);
			if (n > KDBUS_MSG_MAX_FDS)
				return -EMFILE;

			kmsg->fds = item->fds;
			kmsg->fds_count = n;
			break;
		}

		case KDBUS_ITEM_BLOOM:
			/* do not allow multiple bloom filters */
			if (has_bloom)
				return -EEXIST;
			has_bloom = true;

			/* bloom filters are only for broadcast messages */
			if (msg->dst_id != KDBUS_DST_ID_BROADCAST)
				return -EBADMSG;

			/* allow only bloom sizes of a multiple of 64bit */
			if (!KDBUS_IS_ALIGNED8(payload_size))
				return -EFAULT;

			/* do not allow mismatching bloom filter sizes */
			if (payload_size != conn->ep->bus->bloom_size)
				return -EDOM;

			kmsg->bloom = item->data64;
			break;

		case KDBUS_ITEM_DST_NAME:
			/* do not allow multiple names */
			if (has_name)
				return -EEXIST;
			has_name = true;

			/* enforce NUL-terminated strings */
			if (!kdbus_validate_nul(item->str, payload_size))
				return -EINVAL;

			if (!kdbus_name_is_valid(item->str))
				return -EINVAL;

			kmsg->dst_name = item->str;
			break;

		default:
			return -ENOTSUPP;
		}
	}

	if (!KDBUS_ITEM_END(item, msg))
		return -EINVAL;

	/* name is needed if no ID is given */
	if (msg->dst_id == KDBUS_DST_ID_NAME && !has_name)
		return -EDESTADDRREQ;

	/* name and ID should not be given at the same time */
	if (msg->dst_id > KDBUS_DST_ID_NAME &&
	    msg->dst_id < KDBUS_DST_ID_BROADCAST && has_name)
		return -EBADMSG;

	if (msg->dst_id == KDBUS_DST_ID_BROADCAST) {
		/* broadcast messages require a bloom filter */
		if (!has_bloom)
			return -EBADMSG;

		/* timeouts are not allowed for broadcasts */
		if (msg->timeout_ns > 0)
			return -ENOTUNIQ;
	}

	/* bloom filters are for undirected messages only */
	if (has_name && has_bloom)
		return -EBADMSG;

	return 0;
}

/**
 * kdbus_kmsg_new_from_user() - copy message from user memory
 * @conn:		Connection
 * @msg:		User-provided message
 * @kmsg:		Copy of message
 *
 * Returns: 0 on success, negative errno on failure.
 */
int kdbus_kmsg_new_from_user(struct kdbus_conn *conn,
			     struct kdbus_msg __user *msg,
			     struct kdbus_kmsg **kmsg)
{
	struct kdbus_kmsg *m;
	u64 size, alloc_size;
	int ret;

	BUG_ON(*kmsg);

	if (!KDBUS_IS_ALIGNED8((unsigned long)msg))
		return -EFAULT;

	if (kdbus_size_get_user(&size, msg, struct kdbus_msg))
		return -EFAULT;

	if (size < sizeof(struct kdbus_msg) || size > KDBUS_MSG_MAX_SIZE)
		return -EMSGSIZE;

	alloc_size = size + KDBUS_KMSG_HEADER_SIZE;

	m = kmalloc(alloc_size, GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	memset(m, 0, KDBUS_KMSG_HEADER_SIZE);

	if (copy_from_user(&m->msg, msg, size)) {
		ret = -EFAULT;
		goto exit_free;
	}

	/* do not accept kernel-generated messages */
	if (m->msg.payload_type == 0) {
		ret = -EINVAL;
		goto exit_free;
	}

	/* requests for replies need a timeout */
	if (m->msg.flags & KDBUS_MSG_FLAGS_EXPECT_REPLY &&
	    m->msg.timeout_ns == 0) {
		ret = -EINVAL;
		goto exit_free;
	}

	ret = kdbus_msg_scan_items(conn, m);
	if (ret < 0)
		goto exit_free;

	/* patch-in the source of this message */
	m->msg.src_id = conn->id;

	*kmsg = m;
	return 0;

exit_free:
	kdbus_kmsg_free(m);
	return ret;
}
