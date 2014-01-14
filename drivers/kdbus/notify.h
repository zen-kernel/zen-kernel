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

#ifndef __KDBUS_NOTIFY_H
#define __KDBUS_NOTIFY_H

int kdbus_notify_id_change(u64 type, u64 id, u64 flags,
			   struct list_head *queue_list);
int kdbus_notify_reply_timeout(u64 id, u64 cookie,
			       struct list_head *queue_list);
int kdbus_notify_reply_dead(u64 id, u64 cookie,
			    struct list_head *queue_list);
int kdbus_notify_name_change(u64 type,
			     u64 old_id, u64 new_id,
			     u64 old_flags, u64 new_flags,
			     const char *name,
			     struct list_head *queue_list);
#endif
