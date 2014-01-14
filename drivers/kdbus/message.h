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

#ifndef __KDBUS_MESSAGE_H
#define __KDBUS_MESSAGE_H

#include "internal.h"
#include "metadata.h"

/**
 * struct kdbus_kmsg - internal message handling data
 * @notify_type:	Short-cut for faster lookup
 * @notify_old_id:	Short-cut for faster lookup
 * @notify_new_id:	Short-cut for faster lookup
 * @notify_name:	Short-cut for faster lookup
 * @dst_name:		Short-cut to msg for faster lookup
 * @dst_name_id:	Short-cut to msg for faster lookup
 * @bloom:		Short-cut to msg for faster lookup
 * @bloom_size:		Short-cut to msg for faster lookup
 * @fds:		Array of file descriptors to pass
 * @fds_count:		Number of file descriptors to pass
 * @meta:		Appended SCM-like metadata of the sending process
 * @vecs_size:		Size of PAYLOAD data
 * @vecs_count:		Number of PAYLOAD vectors
 * @memfds_count:	Number of memfds to pass
 * @queue_entry:	List of kernel-generated notifications
 * @msg:		Message from or to userspace
 */
struct kdbus_kmsg {
	u64 notify_type;
	u64 notify_old_id;
	u64 notify_new_id;
	const char *notify_name;

	const char *dst_name;
	u64 dst_name_id;
	const u64 *bloom;
	unsigned int bloom_size;
	const int *fds;
	unsigned int fds_count;
	struct kdbus_meta *meta;
	size_t vecs_size;
	unsigned int vecs_count;
	unsigned int memfds_count;
	struct list_head queue_entry;

	/* variable size, must be the last member */
	struct kdbus_msg msg;
};

struct kdbus_ep;
struct kdbus_conn;

int kdbus_kmsg_new(size_t extra_size, struct kdbus_kmsg **kmsg);
int kdbus_kmsg_new_from_user(struct kdbus_conn *conn,
			     struct kdbus_msg __user *msg,
			     struct kdbus_kmsg **kmsg);
void kdbus_kmsg_free(struct kdbus_kmsg *kmsg);
#endif
