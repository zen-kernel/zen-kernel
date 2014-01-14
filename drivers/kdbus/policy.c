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
#include <linux/idr.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "connection.h"
#include "names.h"
#include "policy.h"

#define KDBUS_POLICY_HASH_SIZE	64

/**
 * struct kdbus_policy_db - policy database
 * @entries_hash:	Hashtable of entries
 * @send_access_hash:	Hashtable of send access elements
 * @entries_lock:	Mutex to protect the database's access entries
 * @cache_lock:		Mutex to protect the database's cache
 */
struct kdbus_policy_db {
	DECLARE_HASHTABLE(entries_hash, 6);
	DECLARE_HASHTABLE(send_access_hash, 6);
	struct mutex		entries_lock;
	struct mutex		cache_lock;
};

/**
 * struct kdbus_policy_db_cache_entry - a cached entry
 * @conn_a:		Connection A
 * @conn_b:		Connection B
 * @hentry:		The hash table entry for the database's entries_hash
 */
struct kdbus_policy_db_cache_entry {
	struct kdbus_conn	*conn_a;
	struct kdbus_conn	*conn_b;
	struct hlist_node	hentry;
};

/**
 * struct kdbus_policy_db_entry_access - a database entry access item
 * @type:		One of KDBUS_POLICY_ACCESS_* types
 * @bits:		Access to grant. One of KDBUS_POLICY_*
 * @id:			For KDBUS_POLICY_ACCESS_USER, the uid
 *			For KDBUS_POLICY_ACCESS_GROUP, the gid
 * @list:		List entry item for the entry's list
 *
 * This is the internal version of struct kdbus_policy_access.
 */
struct kdbus_policy_db_entry_access {
	u8			type;	/* USER, GROUP, WORLD */
	u8			bits;	/* RECV, SEND, OWN */
	u64			id;	/* uid, gid, 0 */
	struct list_head	list;
};

/**
 * struct kdbus_policy_db_entry - a policy database entry
 * @name:		The name to match the policy entry against
 * @hentry:		The hash entry for the database's entries_hash
 * @access_list:	List head for keeping tracks of the entry's
 *			access items.
 */
struct kdbus_policy_db_entry {
	char			*name;
	struct hlist_node	hentry;
	struct list_head	access_list;
};

/**
 * kdbus_policy_db_free - drop a policy database reference
 * @db:		The policy database
 */
void kdbus_policy_db_free(struct kdbus_policy_db *db)
{
	struct kdbus_policy_db_entry *e;
	struct kdbus_policy_db_cache_entry *ce;
	struct hlist_node *tmp;
	unsigned int i;

	/* purge entries */
	mutex_lock(&db->entries_lock);
	hash_for_each_safe(db->entries_hash, i, tmp, e, hentry) {
		struct kdbus_policy_db_entry_access *a, *tmp;

		list_for_each_entry_safe(a, tmp, &e->access_list, list) {
			list_del(&a->list);
			kfree(a);
		}

		hash_del(&e->hentry);
		kfree(e->name);
		kfree(e);
	}
	mutex_unlock(&db->entries_lock);

	/* purge cache */
	mutex_lock(&db->cache_lock);
	hash_for_each_safe(db->send_access_hash, i, tmp, ce, hentry) {
		hash_del(&ce->hentry);
		kfree(ce);
	}
	mutex_unlock(&db->cache_lock);

	kfree(db);
}

/**
 * kdbus_policy_db_new() - create a new policy database
 * @db:		The location where to store the new database
 *
 * Return 0 on success, or any other value in case of errors.
 */
int kdbus_policy_db_new(struct kdbus_policy_db **db)
{
	struct kdbus_policy_db *d;

	BUG_ON(*db);

	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	hash_init(d->entries_hash);
	hash_init(d->send_access_hash);
	mutex_init(&d->entries_lock);
	mutex_init(&d->cache_lock);

	*db = d;

	return 0;
}

static u64 kdbus_collect_entry_accesses(struct kdbus_policy_db_entry *db_entry,
					struct kdbus_conn *conn)
{
	struct kdbus_policy_db_entry_access *a;
	u64 uid = from_kuid(current_user_ns(), current_uid());
	u64 gid = from_kgid(current_user_ns(), current_gid());
	u64 access = 0;

	list_for_each_entry(a, &db_entry->access_list, list) {
		switch (a->type) {
		case KDBUS_POLICY_ACCESS_USER:
			if (uid == a->id)
				access |= a->bits;
			break;
		case KDBUS_POLICY_ACCESS_GROUP:
			if (gid == a->id)
				access |= a->bits;
			break;
		case KDBUS_POLICY_ACCESS_WORLD:
			access |= a->bits;
			break;
		}
	}

	return access;
}

static int __kdbus_policy_db_check_send_access(struct kdbus_policy_db *db,
					       struct kdbus_conn *conn_src,
					       struct kdbus_conn *conn_dst)
{
	struct kdbus_name_entry *name_entry;
	struct kdbus_policy_db_entry *db_entry;
	u64 access;
	u32 hash;
	int ret = -EPERM;

	/*
	 * Send access is granted if either the source connection has a
	 * matching SEND rule or the receiver connection has a matching
	 * RECV rule.
	 * Hence, we walk the list of the names registered for each
	 * connection.
	 */
	mutex_lock(&conn_src->lock);
	list_for_each_entry(name_entry, &conn_src->names_list, conn_entry) {
		hash = kdbus_str_hash(name_entry->name);
		hash_for_each_possible(db->entries_hash, db_entry, hentry, hash) {
			if (strcmp(db_entry->name, name_entry->name) != 0)
				continue;

			access = kdbus_collect_entry_accesses(db_entry, conn_src);
			if (access & KDBUS_POLICY_SEND) {
				ret = 0;
				break;
			}
		}
	}
	mutex_unlock(&conn_src->lock);

	if (ret == 0)
		return 0;

	mutex_lock(&conn_dst->lock);
	list_for_each_entry(name_entry, &conn_dst->names_list, conn_entry) {
		hash = kdbus_str_hash(name_entry->name);
		hash_for_each_possible(db->entries_hash, db_entry, hentry, hash) {
			if (strcmp(db_entry->name, name_entry->name) != 0)
				continue;

			access = kdbus_collect_entry_accesses(db_entry, conn_dst);
			if (access & KDBUS_POLICY_RECV) {
				ret = 0;
				break;
			}
		}
	}
	mutex_unlock(&conn_dst->lock);

	return ret;
}

static struct kdbus_policy_db_cache_entry *
kdbus_policy_cache_entry_new(struct kdbus_conn *conn_a,
			     struct kdbus_conn *conn_b)
{
	struct kdbus_policy_db_cache_entry *ce;

	ce = kzalloc(sizeof(*ce), GFP_KERNEL);
	if (!ce)
		return NULL;

	ce->conn_a = conn_a;
	ce->conn_b = conn_b;

	return ce;
}

/**
 * kdbus_policy_db_check_send_access() - check if one connection is allowed
 *				       to send a message to another connection
 * @db:			The policy database
 * @conn_src:		The source connection
 * @conn_dst:		The destination connection
 *
 * Returns 0 if access is granted, -EPERM in case it's not, any any other
 * value in case of errors during adding the cache item internally.
 */
int kdbus_policy_db_check_send_access(struct kdbus_policy_db *db,
				      struct kdbus_conn *conn_src,
				      struct kdbus_conn *conn_dst)
{
	int ret = 0;
	unsigned int hash = 0;
	struct kdbus_policy_db_cache_entry *ce;

	/*
	 * If there was a positive match for these two connections before,
	 * there's an entry in the hash table for them.
	 */
	hash ^= hash_ptr(conn_src, KDBUS_POLICY_HASH_SIZE);
	hash ^= hash_ptr(conn_dst, KDBUS_POLICY_HASH_SIZE);

	mutex_lock(&db->cache_lock);
	hash_for_each_possible(db->send_access_hash, ce, hentry, hash)
		if (ce->conn_a == conn_src && ce->conn_b == conn_dst) {
			mutex_unlock(&db->cache_lock);
			return ret;
		}
	mutex_unlock(&db->cache_lock);

	/*
	 * Otherwise, walk the connection list and store and add
	 * a hash table entry if send access is granted.
	 */
	mutex_lock(&db->entries_lock);
	ret = __kdbus_policy_db_check_send_access(db, conn_src, conn_dst);
	if (ret == 0) {
		ce = kdbus_policy_cache_entry_new(conn_src, conn_dst);
		if (!ce) {
			ret = -ENOMEM;
			goto exit_unlock_entries;
		}

		mutex_lock(&db->cache_lock);
		hash_add(db->send_access_hash, &ce->hentry, hash);
		mutex_unlock(&db->cache_lock);
	}

exit_unlock_entries:
	mutex_unlock(&db->entries_lock);

	return ret;
}

/**
 * kdbus_policy_db_remove_conn() - remove all entries related to a connection
 * @db:		The policy database
 * @conn:	The connection which items to remove
 */
void kdbus_policy_db_remove_conn(struct kdbus_policy_db *db,
				 struct kdbus_conn *conn)
{
	struct kdbus_policy_db_cache_entry *ce;
	struct hlist_node *tmp;
	int i;

	mutex_lock(&db->cache_lock);
	hash_for_each_safe(db->send_access_hash, i, tmp, ce, hentry)
		if (ce->conn_a == conn || ce->conn_b == conn) {
			hash_del(&ce->hentry);
			kfree(ce);
		}
	mutex_unlock(&db->cache_lock);
}

/**
 * kdbus_policy_db_check_own_access() - check whether a policy is allowed
 *					to own a name
 * @db:		The policy database
 * @conn:	The connection to check
 * @name:	The name to check
 *
 * Returns true if the connection is allowed to own the name, false otherwise.
 */
bool kdbus_policy_db_check_own_access(struct kdbus_policy_db *db,
				      struct kdbus_conn *conn,
				      const char *name)
{
	struct kdbus_policy_db_entry *db_entry;
	u32 hash = kdbus_str_hash(name);
	bool allowed = false;

	/* Walk the list of the names registered for a connection ... */
	mutex_lock(&db->entries_lock);
	hash_for_each_possible(db->entries_hash, db_entry,
			       hentry, hash) {
		u64 access;

		if (strcmp(db_entry->name, name) != 0)
			continue;

		access = kdbus_collect_entry_accesses(db_entry, conn);
		if (access & KDBUS_POLICY_OWN) {
			allowed = true;
			goto exit_unlock;
		}
	}

exit_unlock:
	mutex_unlock(&db->entries_lock);

	return allowed;
}

static int kdbus_policy_db_parse(struct kdbus_policy_db *db,
				 const struct kdbus_cmd_policy *cmd,
				 u64 size)
{
	const struct kdbus_item *item;
	struct kdbus_policy_db_entry *current_entry = NULL;

	KDBUS_ITEM_FOREACH(item, cmd, policies) {
		if (!KDBUS_ITEM_VALID(item, cmd))
			return -EINVAL;

		switch (item->type) {
		case KDBUS_ITEM_POLICY_NAME: {
			struct kdbus_policy_db_entry *e;
			u32 hash;

			e = kzalloc(sizeof(*e), GFP_KERNEL);
			if (!e)
				return -ENOMEM;

			hash = kdbus_str_hash(item->policy.name);
			e->name = kstrdup(item->policy.name, GFP_KERNEL);
			INIT_LIST_HEAD(&e->access_list);

			mutex_lock(&db->entries_lock);
			hash_add(db->entries_hash, &e->hentry, hash);
			mutex_unlock(&db->entries_lock);

			current_entry = e;
			break;
		}

		case KDBUS_ITEM_POLICY_ACCESS: {
			struct kdbus_policy_db_entry_access *a;

			/*
			 * A KDBUS_ITEM_POLICY_ACCESS item can only appear
			 * after a KDBUS_ITEM_POLICY_NAME item.
			 */
			if (!current_entry)
				return -EINVAL;

			a = kzalloc(sizeof(*a), GFP_KERNEL);
			if (!a)
				return -ENOMEM;

			a->type = item->policy.access.type;
			a->bits = item->policy.access.bits;
			a->id   = item->policy.access.id;
			INIT_LIST_HEAD(&a->list);

			mutex_lock(&db->entries_lock);
			list_add_tail(&a->list, &current_entry->access_list);
			mutex_unlock(&db->entries_lock);
			break;
		}

		default:
			return -EINVAL;
		}
	}

	if (!KDBUS_ITEM_END(item, cmd))
		return -EINVAL;

	return 0;
}

/**
 * kdbus_cmd_policy_set_from_user() - set a connection's policy rules
 * @db:		The policy database
 * @buf:	The __user buffer that was provided by the ioctl() call
 *
 * Returns 0 on success, or any other value in case of errors.
 * This function is used in the context of the KDBUS_CMD_EP_POLICY_SET
 * ioctl().
 */
int kdbus_cmd_policy_set_from_user(struct kdbus_policy_db *db, void __user *buf)
{
	struct kdbus_cmd_policy *cmd;
	u64 size;
	int ret;

	if (kdbus_size_get_user(&size, buf, struct kdbus_cmd_policy))
		return -EFAULT;

	if (size <= sizeof(struct kdbus_cmd_policy))
		return -EINVAL;

	if (size > KDBUS_POLICY_MAX_SIZE)
		return -EMSGSIZE;

	cmd = memdup_user(buf, size);
	if (IS_ERR(cmd))
		return PTR_ERR(cmd);

	ret = kdbus_policy_db_parse(db, cmd, size);
	kfree(cmd);

	return ret;
}
