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
#include <linux/hashtable.h>
#include <linux/idr.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "bus.h"
#include "connection.h"
#include "endpoint.h"
#include "names.h"
#include "namespace.h"

bool kdbus_bus_uid_is_privileged(const struct kdbus_bus *bus)
{
	if (capable(CAP_IPC_OWNER))
		return true;

	if (uid_eq(bus->uid_owner, current_fsuid()))
		return true;

	return false;
}

/**
 * kdbus_bus_ref() - increase the reference counter of a kdbus_bus
 * @bus:		The bus to unref
 *
 * Every user of a bus, except for its creator, must add a reference to the
 * kdbus_bus using this function.
 */
struct kdbus_bus *kdbus_bus_ref(struct kdbus_bus *bus)
{
	kref_get(&bus->kref);
	return bus;
}

static void __kdbus_bus_free(struct kref *kref)
{
	struct kdbus_bus *bus = container_of(kref, struct kdbus_bus, kref);

	kdbus_bus_disconnect(bus);
	if (bus->name_registry)
		kdbus_name_registry_free(bus->name_registry);
	kdbus_ns_unref(bus->ns);
	kfree(bus->name);
	kfree(bus);
}

/**
 * kdbus_bus_unref() - decrease the reference counter of a kdbus_bus
 * @bus:		The bus to unref
 *
 * Release a reference. If the reference count drops to 0, the bus will be
 * freed.
 *
 * Returns: NULL
 */
struct kdbus_bus *kdbus_bus_unref(struct kdbus_bus *bus)
{
	if (!bus)
		return NULL;

	kref_put(&bus->kref, __kdbus_bus_free);
	return NULL;
}

/**
 * kdbus_bus_find_conn_by_id() - find a connection with a given id
 * @bus:		The bus to look for the connection
 * @id:			The 64-bit connection id
 *
 * Looks up a connection with a given id. The returned connection
 * is ref'ed, and needs to be unref'ed by the user. Returns NULL if
 * the connection can't be found.
 *
 * This function must be called with bus->lock held.
 */
struct kdbus_conn *kdbus_bus_find_conn_by_id(struct kdbus_bus *bus, u64 id)
{
	struct kdbus_conn *conn, *found = NULL;

	hash_for_each_possible(bus->conn_hash, conn, hentry, id)
		if (conn->id == id) {
			found = kdbus_conn_ref(conn);
			break;
		}

	return found;
}

/**
 * kdbus_bus_disconnect() - disconnect a bus
 * @bus:		The kdbus reference
 *
 * The passed bus will be disconnected and the associated endpoint will be
 * unref'ed.
 */
void kdbus_bus_disconnect(struct kdbus_bus *bus)
{
	struct kdbus_ep *ep, *tmp;

	mutex_lock(&bus->lock);
	if (bus->disconnected) {
		mutex_unlock(&bus->lock);
		return;
	}

	bus->disconnected = true;
	mutex_unlock(&bus->lock);

	/* disconnect from namespace */
	mutex_lock(&bus->ns->lock);
	if (bus->ns)
		list_del(&bus->ns_entry);
	mutex_unlock(&bus->ns->lock);

	/* remove all endpoints attached to this bus */
	list_for_each_entry_safe(ep, tmp, &bus->ep_list, bus_entry) {
		kdbus_ep_disconnect(ep);
		kdbus_ep_unref(ep);
	}
}

static struct kdbus_bus *kdbus_bus_find(struct kdbus_ns *ns, const char *name)
{
	struct kdbus_bus *bus = NULL;
	struct kdbus_bus *b;

	mutex_lock(&ns->lock);
	list_for_each_entry(b, &ns->bus_list, ns_entry) {
		if (strcmp(b->name, name))
			continue;

		bus = kdbus_bus_ref(b);
		break;
	}

	mutex_unlock(&ns->lock);
	return bus;
}

/**
 * kdbus_bus_new() - create a new bus
 * @ns:			The namespace to work on
 * @make:		Pointer to a struct kdbus_cmd_make containing the
 *			details for the bus creation
 * @name:		Name of the bus
 * @bloom_size:		Size of the bloom filter on this bus
 * @mode:		The access mode for the device node
 * @uid:		The uid of the device node
 * @gid:		The gid of the device node
 * @bus:		Pointer to a reference where the new bus is stored
 *
 * This function will allocate a new kdbus_bus and link it to the given
 * namespace.
 *
 * Returns: 0 on success, negative errno on failure.
 */
int kdbus_bus_new(struct kdbus_ns *ns,
		  struct kdbus_cmd_make *make, const char *name,
		  size_t bloom_size, umode_t mode, kuid_t uid,
		  kgid_t gid, struct kdbus_bus **bus)
{
	char prefix[16];
	struct kdbus_bus *b;
	int ret;

	BUG_ON(*bus);

	/* enforce "$UID-" prefix */
	snprintf(prefix, sizeof(prefix), "%u-",
		 from_kuid(current_user_ns(), uid));
	if (strncmp(name, prefix, strlen(prefix) != 0))
		return -EINVAL;

	b = kdbus_bus_find(ns, name);
	if (b) {
		kdbus_bus_unref(b);
		return -EEXIST;
	}

	b = kzalloc(sizeof(struct kdbus_bus), GFP_KERNEL);
	if (!b)
		return -ENOMEM;

	kref_init(&b->kref);
	b->uid_owner = uid;
	b->bus_flags = make->flags;
	b->bloom_size = bloom_size;
	b->conn_id_next = 1; /* connection 0 == kernel */
	mutex_init(&b->lock);
	hash_init(b->conn_hash);
	INIT_LIST_HEAD(&b->ep_list);
	INIT_LIST_HEAD(&b->monitors_list);

	/* generate unique ID for this bus */
	get_random_bytes(b->id128, sizeof(b->id128));

	/* Set UUID version to 4 --- truly random generation */
	b->id128[6] &= 0x0f;
	b->id128[6] |= 0x40;

	/* Set the UUID variant to DCE */
	b->id128[8] &= 0x3f;
	b->id128[8] |= 0x80;

	b->name = kstrdup(name, GFP_KERNEL);
	if (!b->name) {
		ret = -ENOMEM;
		goto exit;
	}

	ret = kdbus_name_registry_new(&b->name_registry);
	if (ret < 0)
		goto exit;

	ret = kdbus_ep_new(b, ns, "bus", mode, uid, gid,
			   b->bus_flags & KDBUS_MAKE_POLICY_OPEN);
	if (ret < 0)
		goto exit;

	/* link into namespace */
	mutex_lock(&ns->lock);
	b->id = ns->bus_id_next++;
	list_add_tail(&b->ns_entry, &ns->bus_list);
	b->ns = kdbus_ns_ref(ns);
	mutex_unlock(&ns->lock);

	*bus = b;
	return 0;

exit:
	kdbus_bus_unref(b);
	return ret;
}

/**
 * kdbus_bus_make_user() - create a kdbus_cmd_make from user-supplied data
 * @buf:		The user supplied data from the ioctl() call
 * @make:		Reference to the location where to store the result
 * @name:		Shortcut to the requested name
 * @bloom_size:		The bloom filter size as denoted in the make items
 *
 * This function is part of the connection ioctl() interface and will parse
 * the user-supplied data.
 *
 * Returns: 0 on success, negative errno on failure.
 */
int kdbus_bus_make_user(void __user *buf, struct kdbus_cmd_make **make,
			char **name, size_t *bloom_size)
{
	u64 size;
	struct kdbus_cmd_make *m;
	const char *n = NULL;
	const struct kdbus_item *item;
	u64 bsize = 0;
	int ret;

	if (kdbus_size_get_user(&size, buf, struct kdbus_cmd_make))
		return -EFAULT;

	if (size < sizeof(struct kdbus_cmd_make) || size > KDBUS_MAKE_MAX_SIZE)
		return -EMSGSIZE;

	m = memdup_user(buf, size);
	if (IS_ERR(m))
		return PTR_ERR(m);

	KDBUS_ITEM_FOREACH(item, m, items) {
		if (!KDBUS_ITEM_VALID(item, m)) {
			ret = -EINVAL;
			goto exit;
		}

		switch (item->type) {
		case KDBUS_ITEM_MAKE_NAME:
			if (n) {
				ret = -EEXIST;
				goto exit;
			}

			if (item->size < KDBUS_ITEM_HEADER_SIZE + 2) {
				ret = -EINVAL;
				goto exit;
			}

			if (item->size > KDBUS_ITEM_HEADER_SIZE +
					 KDBUS_MAKE_MAX_LEN + 1) {
				ret = -ENAMETOOLONG;
				goto exit;
			}

			if (!kdbus_validate_nul(item->str,
					item->size - KDBUS_ITEM_HEADER_SIZE)) {
				ret = -EINVAL;
				goto exit;
			}

			ret = kdbus_devname_valid(item->str);
			if (ret < 0)
				goto exit;

			n = item->str;
			break;

		case KDBUS_ITEM_BLOOM_SIZE:
			if (item->size < KDBUS_ITEM_HEADER_SIZE + sizeof(u64)) {
				ret = -EINVAL;
				goto exit;
			}

			bsize = item->data64[0];
			break;

		default:
			ret = -ENOTSUPP;
			goto exit;
		}
	}

	if (!KDBUS_ITEM_END(item, m)) {
		ret = -EINVAL;
		goto exit;
	}

	if (!n) {
		ret = -EBADMSG;
		goto exit;
	}

	if (!KDBUS_IS_ALIGNED8(bsize)) {
		ret = -EINVAL;
		goto exit;
	}

	if (bsize < 8 || bsize > SZ_16K) {
		ret = -EINVAL;
		goto exit;
	}

	*make = m;
	*name = (char *)n;
	*bloom_size = (size_t)bsize;
	return 0;

exit:
	kfree(m);
	return ret;
}
