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

#ifndef __KDBUS_BUS_H
#define __KDBUS_BUS_H

#include <linux/hashtable.h>
#include <linux/idr.h>

#include "internal.h"

/**
 * struct kdbus_bus - bus in a namespace
 * @kref:		Reference count
 * @disconnected:	Invalidated data
 * @uid_owner:		The uid of the owner of the bus
 * @ns:			Namespace of this bus
 * @name:		The bus name
 * @id:			ID of this bus in the namespace
 * @lock:		Bus data lock
 * @ep_id_next:		Next endpoint id sequence number
 * @conn_id_next:	Next connection id sequence number
 * @msg_id_next:	Next message id sequence number
 * @conn_idr:		Map of connection device minor nummbers
 * @conn_hash:		Map of connection IDs
 * @ep_list:		Endpoints on this bus
 * @bus_flags:		Simple pass-through flags from userspace to userspace
 * @bloom_size:		Bloom filter size
 * @name_registry:	Namespace's list of buses
 * @ns_entry:		Namespace's list of buses
 * @monitors_list:	Connections that monitor this bus
 * @id128:		Unique random 128 bit ID of this bus
 *
 * A bus provides a "bus" endpoint / device node.
 *
 * A bus is created by opening the control node and issuing the
 * KDBUS_CMD_BUS_MAKE iotcl. Closing this file immediately destroys
 * the bus.
 */
struct kdbus_bus {
	struct kref kref;
	bool disconnected;
	kuid_t uid_owner;
	struct kdbus_ns *ns;
	const char *name;
	u64 id;
	struct mutex lock;
	u64 ep_id_next;
	u64 conn_id_next;
	u64 msg_id_next;
	struct idr conn_idr;
	DECLARE_HASHTABLE(conn_hash, 6);
	struct list_head ep_list;
	u64 bus_flags;
	size_t bloom_size;
	struct kdbus_name_registry *name_registry;
	struct list_head ns_entry;
	struct list_head monitors_list;
	u8 id128[16];
};

int kdbus_bus_make_user(void __user *buf, struct kdbus_cmd_make **make,
			char **name, size_t *bsize);
int kdbus_bus_new(struct kdbus_ns *ns, struct kdbus_cmd_make *make,
		  const char *name, size_t bloom_size,
		  umode_t mode, kuid_t uid, kgid_t gid, struct kdbus_bus **bus);
struct kdbus_bus *kdbus_bus_ref(struct kdbus_bus *bus);
struct kdbus_bus *kdbus_bus_unref(struct kdbus_bus *bus);
void kdbus_bus_disconnect(struct kdbus_bus *bus);

bool kdbus_bus_uid_is_privileged(const struct kdbus_bus *bus);
struct kdbus_conn *kdbus_bus_find_conn_by_id(struct kdbus_bus *bus, u64 id);
#endif
