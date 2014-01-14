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

#ifndef __KDBUS_EP_H
#define __KDBUS_EP_H

#include "defaults.h"
#include "util.h"

/*
 * struct kdbus_endpoint - enpoint to access a bus
 * @kref		reference count
 * @disconnected	invalidated data
 * @bus			bus behind this endpoint
 * @name		name of the endpoint
 * @id			id of this endpoint on the bus
 * @minor		minor of this endpoint in the namespace major
 * @dev			device node of this endpoint
 * @mode		file mode of this endpoint device node
 * @uid			uid owning this endpoint
 * @gid			gid owning this endpoint
 * @bus_entry		bus' endpoints
 * @wait		wake up this endpoint
 * @lock		endpoint data lock
 * @policy_db		uploaded policy
 * @policy_open		default endpoint policy
 *
 * An enpoint offers access to a bus; the default device node name is "bus".
 * Additional custom endpoints to the same bus can be created and they can
 * carry their own policies/filters.
 */
struct kdbus_ep {
	struct kref kref;
	bool disconnected;
	struct kdbus_bus *bus;
	const char *name;
	u64 id;
	unsigned int minor;
	struct device *dev;
	umode_t mode;
	kuid_t uid;
	kgid_t gid;
	struct list_head bus_entry;
	wait_queue_head_t wait;
	struct mutex lock;
	struct kdbus_policy_db *policy_db;
	bool policy_open:1;
};

struct kdbus_ns;

int kdbus_ep_new(struct kdbus_bus *bus, struct kdbus_ns *ns, const char *name,
		 umode_t mode, kuid_t uid, kgid_t gid, bool policy);
struct kdbus_ep *kdbus_ep_ref(struct kdbus_ep *ep);
struct kdbus_ep *kdbus_ep_unref(struct kdbus_ep *ep);
void kdbus_ep_disconnect(struct kdbus_ep *ep);
int kdbus_ep_make_user(void __user *buf,
		       struct kdbus_cmd_make **make, char **name);
#endif
