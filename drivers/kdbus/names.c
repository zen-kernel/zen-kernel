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

#include <linux/ctype.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/hash.h>
#include <linux/idr.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "bus.h"
#include "connection.h"
#include "endpoint.h"
#include "names.h"
#include "notify.h"
#include "policy.h"

/**
 * struct kdbus_name_queue_item - a queue item for a name
 * @conn:		The associated connection
 * @entry:		Name entry queuing up for
 * @flags:		The queuing flags
 * @entry_entry:	List element for the list in @entry
 * @conn_entry:		List element for the list in @conn
 */
struct kdbus_name_queue_item {
	struct kdbus_conn	*conn;
	struct kdbus_name_entry	*entry;
	u64			flags;
	struct list_head	entry_entry;
	struct list_head	conn_entry;
};

static void kdbus_name_entry_free(struct kdbus_name_entry *e)
{
	hash_del(&e->hentry);
	kfree(e->name);
	kfree(e);
}

/**
 * kdbus_name_registry_free() - drop a name reg's reference
 * @reg:		The name registry
 *
 * Cleanup the name registry's internal structures.
 */
void kdbus_name_registry_free(struct kdbus_name_registry *reg)
{
	struct kdbus_name_entry *e;
	struct hlist_node *tmp;
	unsigned int i;

	mutex_lock(&reg->entries_lock);
	hash_for_each_safe(reg->entries_hash, i, tmp, e, hentry)
		kdbus_name_entry_free(e);
	mutex_unlock(&reg->entries_lock);

	kfree(reg);
}

/**
 * kdbus_name_registry_new() - create a new name registry
 * @reg:		The returned name registry
 *
 * Returns 0 on success, -ENOMEM if memory allocation failed.
 */
int kdbus_name_registry_new(struct kdbus_name_registry **reg)
{
	struct kdbus_name_registry *r;

	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r)
		return -ENOMEM;

	hash_init(r->entries_hash);
	mutex_init(&r->entries_lock);
	r->name_id_next = 1;

	*reg = r;

	return 0;
}

static struct kdbus_name_entry *
__kdbus_name_lookup(struct kdbus_name_registry *reg,
		    u32 hash, const char *name)
{
	struct kdbus_name_entry *e;

	hash_for_each_possible(reg->entries_hash, e, hentry, hash)
		if (strcmp(e->name, name) == 0)
			return e;

	return NULL;
}

static void kdbus_name_queue_item_free(struct kdbus_name_queue_item *q)
{
	list_del(&q->entry_entry);
	list_del(&q->conn_entry);
	kfree(q);
}

static void kdbus_name_entry_remove_owner(struct kdbus_name_entry *e)
{
	struct kdbus_conn *conn = e->conn;

	BUG_ON(!e->conn);

	mutex_lock(&conn->lock);
	conn->names--;
	list_del(&e->conn_entry);
	mutex_unlock(&conn->lock);

	kdbus_conn_unref(conn);
	e->conn = NULL;
}

static void kdbus_name_entry_set_owner(struct kdbus_name_entry *e,
				       struct kdbus_conn *conn)
{
	BUG_ON(e->conn);

	mutex_lock(&conn->lock);
	e->conn = kdbus_conn_ref(conn);
	list_add_tail(&e->conn_entry, &e->conn->names_list);
	conn->names++;
	mutex_unlock(&conn->lock);
}

static int kdbus_name_entry_release(struct kdbus_name_entry *e,
				     struct list_head *notify_list)
{
	struct kdbus_name_queue_item *q;

	if (list_empty(&e->queue_list)) {
		/* if the name has an activator connection, hand it back */
		if (e->activator && e->activator != e->conn) {
			u64 flags = KDBUS_NAME_ACTIVATOR;
			int ret;

			kdbus_notify_name_change(KDBUS_ITEM_NAME_CHANGE,
						 e->conn->id, e->activator->id,
						 e->flags, flags,
						 e->name, notify_list);

			/*
			 * Move messages still queued in the old connection
			 * and addressed to that name to the new connection.
			 * This allows a race and loss-free name and message
			 * takeover and exit-on-idle services.
			 */
			ret = kdbus_conn_move_messages(e->activator, e->conn,
						       e->name_id);
			if (ret < 0)
				return ret;

			e->flags = flags;
			kdbus_name_entry_remove_owner(e);
			kdbus_name_entry_set_owner(e, e->activator);

			return 0;
		}

		/* release the name */
		kdbus_notify_name_change(KDBUS_ITEM_NAME_REMOVE,
					 e->conn->id, 0,
					 e->flags, 0, e->name,
					 notify_list);
		kdbus_name_entry_remove_owner(e);
		kdbus_name_entry_free(e);

		return 0;
	}

	/* give it to first waiter in the queue */
	q = list_first_entry(&e->queue_list,
			     struct kdbus_name_queue_item,
			     entry_entry);
	kdbus_notify_name_change(KDBUS_ITEM_NAME_CHANGE,
				 e->conn->id, q->conn->id,
				 e->flags, q->flags, e->name, notify_list);
	e->flags = q->flags;
	kdbus_name_entry_remove_owner(e);
	kdbus_name_entry_set_owner(e, q->conn);
	kdbus_name_queue_item_free(q);

	return 0;
}

static int kdbus_name_release(struct kdbus_name_entry *e,
			      struct kdbus_conn *conn,
			      struct list_head *notify_list)
{
	struct kdbus_name_queue_item *q_tmp, *q;

	/* Is the connection already the real owner of the name? */
	if (e->conn == conn)
		return kdbus_name_entry_release(e, notify_list);

	/*
	 * Otherwise, walk the list of queued entries and search for
	 * items for the connection.
	 */
	list_for_each_entry_safe(q, q_tmp, &e->queue_list, entry_entry) {
		if (q->conn != conn)
			continue;
		kdbus_name_queue_item_free(q);
		return 0;
	}

	/* the name belongs to somebody else */
	return -EADDRINUSE;
}

/**
 * kdbus_name_remove_by_conn() - remove all name entries of a given connection
 * @reg:		The name registry
 * @conn:		The connection which entries to remove
 *
 * This function removes all name entry held by a given connection.
 */
void kdbus_name_remove_by_conn(struct kdbus_name_registry *reg,
			       struct kdbus_conn *conn)
{
	struct kdbus_name_entry *e_tmp, *e;
	struct kdbus_name_queue_item *q_tmp, *q;
	LIST_HEAD(notify_list);
	LIST_HEAD(names_queue_list);
	LIST_HEAD(names_list);

	mutex_lock(&conn->lock);
	list_splice_init(&conn->names_list, &names_list);
	list_splice_init(&conn->names_queue_list, &names_queue_list);
	mutex_unlock(&conn->lock);

	mutex_lock(&reg->entries_lock);
	list_for_each_entry_safe(q, q_tmp, &names_queue_list, conn_entry)
		kdbus_name_queue_item_free(q);
	list_for_each_entry_safe(e, e_tmp, &names_list, conn_entry)
		kdbus_name_entry_release(e, &notify_list);
	mutex_unlock(&reg->entries_lock);

	kdbus_conn_kmsg_list_send(conn->ep, &notify_list);
}

/**
 * kdbus_name_lookup() - look up a name in a name registry
 * @reg:		The name registry
 * @name:		The name to look up
 *
 * Returns the name entry, if found. Otherwise, NULL is returned.
 */
struct kdbus_name_entry *kdbus_name_lookup(struct kdbus_name_registry *reg,
					   const char *name)
{
	struct kdbus_name_entry *e = NULL;
	u32 hash = kdbus_str_hash(name);

	mutex_lock(&reg->entries_lock);
	e = __kdbus_name_lookup(reg, hash, name);
	mutex_unlock(&reg->entries_lock);

	return e;
}

static int kdbus_name_queue_conn(struct kdbus_conn *conn, u64 flags,
				  struct kdbus_name_entry *e)
{
	struct kdbus_name_queue_item *q;

	q = kzalloc(sizeof(*q), GFP_KERNEL);
	if (!q)
		return -ENOMEM;

	q->conn = conn;
	q->flags = flags;
	q->entry = e;

	list_add_tail(&q->entry_entry, &e->queue_list);
	list_add_tail(&q->conn_entry, &conn->names_queue_list);

	return 0;
}

/* called with entries_lock held */
static int kdbus_name_replace_owner(struct kdbus_name_registry *reg,
				    struct kdbus_conn *conn,
				    struct kdbus_name_entry *e, u64 flags,
				    struct list_head *notify_list)
{
	int ret;

	ret = kdbus_notify_name_change(KDBUS_ITEM_NAME_CHANGE,
				       e->conn->id, conn->id,
				       e->flags, flags,
				       e->name, notify_list);
	if (ret < 0)
		return ret;

	/* hand over ownership */
	kdbus_name_entry_remove_owner(e);
	kdbus_name_entry_set_owner(e, conn);
	e->flags = flags;

	return 0;
}

/**
 * kdbus_name_is_valid() - check if a name is value
 * @p:			The name to check
 *
 * A name is valid if all of the following criterias are met:
 *
 *  - The name has one or more elements separated by a period ('.') character.
 *    All elements must contain at least one character.
 *  - Each element must only contain the ASCII characters "[A-Z][a-z][0-9]_"
 *    and must not begin with a digit.
 *  - The name must contain at least one '.' (period) character
 *    (and thus at least two elements).
 *  - The name must not begin with a '.' (period) character.
 *  - The name must not exceed KDBUS_NAME_MAX_LEN.
 */
bool kdbus_name_is_valid(const char *p)
{
	const char *q;
	bool dot, found_dot;

	for (dot = true, q = p; *q; q++) {
		if (*q == '.') {
			if (dot)
				return false;

			found_dot = dot = true;
		} else {
			bool good;

			good = isalpha(*q) || (!dot && isdigit(*q)) ||
				*q == '_' || *q == '-';

			if (!good)
				return false;

			dot = false;
		}
	}

	if (q - p > KDBUS_NAME_MAX_LEN)
		return false;

	if (dot)
		return false;

	if (!found_dot)
		return false;

	return true;
}

/**
 * kdbus_name_acquire() - acquire a name
 * @reg:		The name registry
 * @conn:		The connection to pin this entry to
 * @name:		The name to acquire
 * @flags:		Acquisition flags (KDBUS_NAME_*)
 * @entry:		Return pointer for the entry (may be NULL)
 *
 * Returns 0 on success, other values on error.
 */
int kdbus_name_acquire(struct kdbus_name_registry *reg,
		       struct kdbus_conn *conn,
		       const char *name, u64 *flags,
		       struct kdbus_name_entry **entry)
{
	struct kdbus_name_entry *e = NULL;
	LIST_HEAD(notify_list);
	int ret = 0;
	u32 hash;

	hash = kdbus_str_hash(name);

	mutex_lock(&reg->entries_lock);
	e = __kdbus_name_lookup(reg, hash, name);
	if (e) {
		/* connection already owns that name */
		if (e->conn == conn) {
			ret = -EALREADY;
			goto exit_unlock;
		}

		/* an activator can only own a single name */
		if (conn->flags & KDBUS_HELLO_ACTIVATOR) {
			ret = -EALREADY;
			goto exit_unlock;
		}

		/* take over the name of an activator connection */
		if (e->flags & KDBUS_NAME_ACTIVATOR) {

			/*
			 * Take over the messages queued in the activator
			 * connection, the activator itself never reads them.
			 */
			ret = kdbus_conn_move_messages(conn, e->activator, 0);
			if (ret < 0)
				goto exit_unlock;

			ret = kdbus_name_replace_owner(reg, conn, e, *flags,
						       &notify_list);
			goto exit_unlock;
		}

		/* take over the name if both parties agree */
		if ((*flags & KDBUS_NAME_REPLACE_EXISTING) &&
		    (e->flags & KDBUS_NAME_ALLOW_REPLACEMENT)) {

			/*
			 * Move name back to the queue, in case we take it away
			 * from a connection which asked for queuing.
			 */
			if (e->flags & KDBUS_NAME_QUEUE) {
				ret = kdbus_name_queue_conn(e->conn,
							    e->flags, e);
				if (ret < 0)
					goto exit_unlock;
			}

			ret = kdbus_name_replace_owner(reg, conn, e, *flags,
						       &notify_list);
			goto exit_unlock;
		}

		/* add it to the queue waiting for the name */
		if (*flags & KDBUS_NAME_QUEUE) {
			ret = kdbus_name_queue_conn(conn, *flags, e);

			/* tell the caller that we queued it */
			*flags |= KDBUS_NAME_IN_QUEUE;

			goto exit_unlock;
		}

		/* the name is busy, return a failure */
		ret = -EEXIST;
		goto exit_unlock;
	}

	/* new name entry */
	e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (!e) {
		ret = -ENOMEM;
		goto exit_unlock;
	}

	e->name = kstrdup(name, GFP_KERNEL);
	if (!e->name) {
		kfree(e);
		ret = -ENOMEM;
		goto exit_unlock;
	}

	if (conn->flags & KDBUS_HELLO_ACTIVATOR)
		e->activator = kdbus_conn_ref(conn);

	e->flags = *flags;
	INIT_LIST_HEAD(&e->queue_list);
	e->name_id = reg->name_id_next++;
	hash_add(reg->entries_hash, &e->hentry, hash);
	kdbus_name_entry_set_owner(e, conn);

	kdbus_notify_name_change(KDBUS_ITEM_NAME_ADD,
				 0, e->conn->id,
				 0, e->flags, e->name,
				 &notify_list);

	if (entry)
		*entry = e;

exit_unlock:
	mutex_unlock(&reg->entries_lock);
	kdbus_conn_kmsg_list_send(conn->ep, &notify_list);

	return ret;
}

/**
 * kdbus_cmd_name_acquire() - acquire a name from a ioctl command buffer
 * @reg:		The name registry
 * @conn:		The connection to pin this entry to
 * @buf:		The __user buffer as passed in by the ioctl
 *
 * Returns 0 on success, other values on error.
 */
int kdbus_cmd_name_acquire(struct kdbus_name_registry *reg,
			   struct kdbus_conn *conn,
			   void __user *buf)
{
	u64 allowed;
	struct kdbus_name_entry *e = NULL;
	struct kdbus_cmd_name *cmd_name;
	LIST_HEAD(notify_list);
	u64 size;
	u32 hash;
	int ret = 0;

	if (kdbus_size_get_user(&size, buf, struct kdbus_cmd_name))
		return -EFAULT;

	/* monitor connection may not own names */
	if (conn->flags & KDBUS_HELLO_MONITOR)
		return -EPERM;

	if ((size < sizeof(struct kdbus_cmd_name)) ||
	    (size > (sizeof(struct kdbus_cmd_name) + KDBUS_NAME_MAX_LEN + 1)))
		return -EMSGSIZE;

	if (conn->names > KDBUS_CONN_MAX_NAMES)
		return -E2BIG;

	cmd_name = memdup_user(buf, size);
	if (IS_ERR(cmd_name))
		return PTR_ERR(cmd_name);

	/* refuse improper flags when requesting */
	allowed = KDBUS_NAME_REPLACE_EXISTING|
		  KDBUS_NAME_ALLOW_REPLACEMENT|
		  KDBUS_NAME_QUEUE;
	if ((cmd_name->flags & ~allowed) != 0)
		return -EINVAL;

	if (!kdbus_check_strlen(cmd_name, name) ||
	    !kdbus_name_is_valid(cmd_name->name)) {
		ret = -EINVAL;
		goto exit_free;
	}

	/* privileged users can act on behalf of someone else */
	if (cmd_name->owner_id != 0) {
		struct kdbus_conn *new_conn;
		struct kdbus_bus *bus = conn->ep->bus;

		if (!kdbus_bus_uid_is_privileged(bus)) {
			ret = -EPERM;
			goto exit_free;
		}

		mutex_lock(&bus->lock);
		new_conn = kdbus_bus_find_conn_by_id(bus, cmd_name->owner_id);
		mutex_unlock(&bus->lock);

		if (!new_conn) {
			ret = -ENXIO;
			goto exit_free;
		}

		conn = new_conn;
	} else {
		kdbus_conn_ref(conn);
	}

	hash = kdbus_str_hash(cmd_name->name);

	if (conn->ep->policy_db) {
		if (!kdbus_policy_db_check_own_access(conn->ep->policy_db,
						      conn, cmd_name->name)) {
			ret = -EPERM;
			goto exit_unref_conn;
		}
	}

	ret = kdbus_name_acquire(reg, conn, cmd_name->name,
				 &cmd_name->flags, &e);
	if (ret < 0)
		goto exit_unref_conn;

	/* return flags to the caller */
	if (copy_to_user(buf, cmd_name, size)) {
		ret = -EFAULT;
		kdbus_conn_kmsg_list_free(&notify_list);
		kdbus_name_entry_release(e, NULL);
	}

exit_unref_conn:
	kdbus_conn_kmsg_list_send(conn->ep, &notify_list);
	kdbus_conn_unref(conn);

exit_free:
	kfree(cmd_name);
	return ret;
}

/**
 * kdbus_cmd_name_release() - release a name entry from a ioctl command buffer
 * @reg:		The name registry
 * @conn:		The connection that holds the name
 * @buf:		The __user buffer as passed in by the ioctl
 *
 * Returns 0 on success, other values on error.
 */
int kdbus_cmd_name_release(struct kdbus_name_registry *reg,
			   struct kdbus_conn *conn,
			   void __user *buf)
{
	struct kdbus_name_entry *e;
	struct kdbus_cmd_name *cmd_name;
	LIST_HEAD(notify_list);
	u64 size;
	u32 hash;
	int ret = 0;

	if (kdbus_size_get_user(&size, buf, struct kdbus_cmd_name))
		return -EFAULT;

	if ((size < sizeof(struct kdbus_cmd_name)) ||
	    (size > (sizeof(struct kdbus_cmd_name) + KDBUS_NAME_MAX_LEN + 1)))
		return -EMSGSIZE;

	cmd_name = memdup_user(buf, size);
	if (IS_ERR(cmd_name))
		return PTR_ERR(cmd_name);

	if (!kdbus_name_is_valid(cmd_name->name)) {
		ret = -EINVAL;
		goto exit_free;
	}

	hash = kdbus_str_hash(cmd_name->name);

	mutex_lock(&reg->entries_lock);
	e = __kdbus_name_lookup(reg, hash, cmd_name->name);
	if (!e) {
		ret = -ESRCH;
		conn = NULL;
		goto exit_unlock;
	}

	/* privileged users can act on behalf of someone else */
	if (cmd_name->owner_id > 0) {
		struct kdbus_bus *bus = conn->ep->bus;

		if (!kdbus_bus_uid_is_privileged(bus)) {
			ret = -EPERM;
			goto exit_unlock;
		}

		mutex_lock(&bus->lock);
		conn = kdbus_bus_find_conn_by_id(bus, cmd_name->owner_id);
		mutex_unlock(&bus->lock);

		if (!conn) {
			ret = -ENXIO;
			goto exit_unlock;
		}
	} else {
		kdbus_conn_ref(conn);
	}

	ret = kdbus_name_release(e, conn, &notify_list);

exit_unlock:
	mutex_unlock(&reg->entries_lock);

	if (conn) {
		kdbus_conn_kmsg_list_send(conn->ep, &notify_list);
		kdbus_conn_unref(conn);
	}

exit_free:
	kfree(cmd_name);
	return ret;
}

static int kdbus_name_list_write(struct kdbus_conn *conn,
				 struct kdbus_conn *c,
				 size_t *pos,
				 struct kdbus_name_entry *e,
				 bool write)
{
	size_t p = *pos;
	const size_t len = sizeof(struct kdbus_cmd_name);
	size_t nlen = 0;
	int ret = 0;

	if (e)
		nlen = strlen(e->name) + 1;

	if (write) {
		struct kdbus_cmd_name n = {
			.size = len + nlen,
			.owner_id = c->id,
			.flags = e ? e->flags : 0,
			.conn_flags = c->flags,
		};

		/* write record */
		ret = kdbus_pool_write(conn->pool, p, &n, len);
		if (ret < 0)
			return ret;
		p += len;

		/* append name */
		if (e) {
			ret = kdbus_pool_write(conn->pool, p, e->name, nlen);
			if (ret < 0)
				return ret;
			p += KDBUS_ALIGN8(nlen);
		}
	} else {
		p += len + KDBUS_ALIGN8(nlen);
	}

	*pos = p;
	return 0;
}

static int kdbus_name_list_all(struct kdbus_conn *conn, u64 flags,
			       size_t *pos, bool write)
{
	struct kdbus_conn *c;
	size_t p = *pos;
	int ret, i;

	hash_for_each(conn->ep->bus->conn_hash, i, c, hentry) {
		bool added = false;

		/* skip activators */
		if (!(flags & KDBUS_NAME_LIST_ACTIVATORS) &&
		    c->flags & KDBUS_HELLO_ACTIVATOR)
			continue;

		/* all names the connection owns */
		if (flags & (KDBUS_NAME_LIST_NAMES |
			     KDBUS_NAME_LIST_ACTIVATORS)) {
			struct kdbus_name_entry *e;

			list_for_each_entry(e, &c->names_list, conn_entry) {
				struct kdbus_conn *a = e->activator;

				if ((flags & KDBUS_NAME_LIST_ACTIVATORS) &&
				    a && a != c) {
					ret = kdbus_name_list_write(conn, a, &p,
								    e, write);
					if (ret < 0)
						return ret;

					added = true;
				}

				if (flags & KDBUS_NAME_LIST_NAMES ||
				    c->flags & KDBUS_HELLO_ACTIVATOR) {
					ret = kdbus_name_list_write(conn, c, &p,
								    e, write);
					if (ret < 0)
						return ret;

					added = true;
				}
			}
		}

		/* queue of names the connection is currently waiting for */
		if (flags & KDBUS_NAME_LIST_QUEUED) {
			struct kdbus_name_queue_item *q;

			list_for_each_entry(q, &c->names_queue_list, conn_entry) {
				ret = kdbus_name_list_write(conn, c, &p,
							    q->entry, write);
				if (ret < 0)
					return ret;

				added = true;
			}
		}

		/* nothing added so far, just add the unique ID */
		if (!added && flags & KDBUS_NAME_LIST_UNIQUE) {
			ret = kdbus_name_list_write(conn, c, &p, NULL, write);
			if (ret < 0)
				return ret;
		}
	}

	*pos = p;
	return 0;
}

/**
 * kdbus_cmd_name_list() - list names of a connection
 * @reg:		The name registry
 * @conn:		The connection holding the name entries
 * @buf:		The __user buffer as passed in by the ioctl
 *
 * Returns 0 on success, other values on error.
 */
int kdbus_cmd_name_list(struct kdbus_name_registry *reg,
			struct kdbus_conn *conn,
			void __user *buf)
{
	struct kdbus_cmd_name_list *cmd_list;
	struct kdbus_name_list list = {};
	size_t size;
	size_t off, pos;
	int ret;

	cmd_list = memdup_user(buf, sizeof(struct kdbus_cmd_name_list));
	if (IS_ERR(cmd_list))
		return PTR_ERR(cmd_list);

	mutex_lock(&reg->entries_lock);
	mutex_lock(&conn->ep->bus->lock);

	/* size of header */
	size = sizeof(struct kdbus_name_list);

	/* size of records */
	ret = kdbus_name_list_all(conn, cmd_list->flags, &size, false);
	if (ret < 0)
		goto exit_unlock;

	ret = kdbus_pool_alloc_range(conn->pool, size, &off);
	if (ret < 0)
		goto exit_unlock;

	/* copy header */
	pos = off;
	list.size = size;

	ret = kdbus_pool_write(conn->pool, pos,
			       &list, sizeof(struct kdbus_name_list));
	if (ret < 0)
		goto exit_pool_free;
	pos += sizeof(struct kdbus_name_list);

	/* copy data */
	ret = kdbus_name_list_all(conn, cmd_list->flags, &pos, true);
	if (ret < 0)
		goto exit_pool_free;

	/* return allocated data */
	if (kdbus_offset_set_user(&off, buf, struct kdbus_cmd_name_list))
		ret = -EFAULT;

exit_pool_free:
	if (ret < 0)
		kdbus_pool_free_range(conn->pool, off);
exit_unlock:
	mutex_unlock(&conn->ep->bus->lock);
	mutex_unlock(&reg->entries_lock);
	kfree(cmd_list);

	return ret;
}
