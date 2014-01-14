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
#include <linux/hash.h>
#include <linux/init.h>
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

/**
 * struct kdbus_match_db - message filters
 * @entries_list:	List of matches
 * @entries_lock:	Match data lock
 */
struct kdbus_match_db {
	struct list_head	entries_list;
	struct mutex		entries_lock;
};

/**
 * struct kdbus_match_entry - a match database entry
 * @cookie:		User-supplied cookie to lookup the entry
 * @list_entry:		The list entry element for the db list
 * @rules_list:		The list head for tracking rules of this entry
 */
struct kdbus_match_entry {
	u64			cookie;
	struct list_head	list_entry;
	struct list_head	rules_list;
};

/**
 * struct kdbus_match_rule - a rule appended to a match entry
 * @type:		An item type to match agains
 * @name:		Name to match against
 * @bloom:		Bloom filter to match against
 * @old_id:		For KDBUS_ITEM_ID_REMOVE and KDBUS_ITEM_NAME_REMOVE or
 *			KDBUS_ITEM_NAME_CHANGE, stores a connection ID
 * @src_id:		For KDBUS_ITEM_ID, stores a connection ID
 * @new_id:		For KDBUS_ITEM_ID_ADD, KDBUS_ITEM_NAME_ADD or
 *			KDBUS_ITEM_NAME_CHANGE, stores a connection ID
 * @rules_entry:	List entry to the entry's rules list
 */
struct kdbus_match_rule {
	u64			type;
	union {
		char		*name;
		u64		*bloom;
	};
	union {
		u64		old_id;
		u64		src_id;
	};
	u64			new_id;

	struct list_head	rules_entry;
};

static void kdbus_match_rule_free(struct kdbus_match_rule *rule)
{
	switch (rule->type) {
	case KDBUS_ITEM_BLOOM:
		kfree(rule->bloom);
		break;

	case KDBUS_ITEM_NAME:
	case KDBUS_ITEM_NAME_ADD:
	case KDBUS_ITEM_NAME_REMOVE:
	case KDBUS_ITEM_NAME_CHANGE:
		kfree(rule->name);
		break;

	case KDBUS_ITEM_ID:
	case KDBUS_ITEM_ID_ADD:
	case KDBUS_ITEM_ID_REMOVE:
		break;

	default:
		BUG();
	}

	list_del(&rule->rules_entry);
	kfree(rule);
}

static void kdbus_match_entry_free(struct kdbus_match_entry *entry)
{
	struct kdbus_match_rule *r, *tmp;

	list_for_each_entry_safe(r, tmp, &entry->rules_list, rules_entry)
		kdbus_match_rule_free(r);
}

/**
 * kdbus_match_db_free() - free match db resources
 * @db:			The match database
 */
void kdbus_match_db_free(struct kdbus_match_db *db)
{
	struct kdbus_match_entry *entry, *tmp;

	mutex_lock(&db->entries_lock);
	list_for_each_entry_safe(entry, tmp, &db->entries_list, list_entry)
		kdbus_match_entry_free(entry);
	mutex_unlock(&db->entries_lock);

	kfree(db);
}

/**
 * kdbus_match_db_new() - create a new match database
 * @db:			Pointer location for the returned database
 *
 * Returns 0 on success, any other value in case of errors.
 */
int kdbus_match_db_new(struct kdbus_match_db **db)
{
	struct kdbus_match_db *d;

	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	mutex_init(&d->entries_lock);
	INIT_LIST_HEAD(&d->entries_list);

	*db = d;
	return 0;
}

static bool kdbus_match_bloom(const u64 *filter, const u64 *mask,
		       const struct kdbus_conn *conn)
{
	unsigned int i;

	for (i = 0; i < conn->ep->bus->bloom_size / sizeof(u64); i++)
		if ((filter[i] & mask[i]) != mask[i])
			return false;

	return true;
}

static bool kdbus_match_rules(const struct kdbus_match_entry *entry,
			      struct kdbus_conn *conn_src,
			      struct kdbus_kmsg *kmsg)
{
	struct kdbus_match_rule *r;

	/*
	 * Walk all the rules and bail out immediately
	 * if any of them is unsatisfied.
	 */

	list_for_each_entry(r, &entry->rules_list, rules_entry) {

		if (conn_src == NULL) {
			/* kernel notifications */

			if (kmsg->notify_type != r->type)
				return false;

			switch (r->type) {
			case KDBUS_ITEM_ID_ADD:
				if (r->new_id != KDBUS_MATCH_ID_ANY &&
				    r->new_id != kmsg->notify_new_id)
					return false;

				break;

			case KDBUS_ITEM_ID_REMOVE:
				if (r->old_id != KDBUS_MATCH_ID_ANY &&
				    r->old_id != kmsg->notify_old_id)
					return false;

				break;

			case KDBUS_ITEM_NAME_ADD:
			case KDBUS_ITEM_NAME_CHANGE:
			case KDBUS_ITEM_NAME_REMOVE:
				if ((r->old_id != KDBUS_MATCH_ID_ANY &&
				     r->old_id != kmsg->notify_old_id) ||
				    (r->new_id != KDBUS_MATCH_ID_ANY &&
				     r->new_id != kmsg->notify_new_id) ||
				    (r->name && kmsg->notify_name &&
				     strcmp(r->name, kmsg->notify_name) != 0))
					return false;

				break;

			default:
				return false;
			}
		} else {
			/* messages from userspace */

			switch (r->type) {
			case KDBUS_ITEM_BLOOM:
				if (!kdbus_match_bloom(kmsg->bloom,
						       r->bloom, conn_src))
					return false;
				break;

			case KDBUS_ITEM_ID:
				if (r->src_id != conn_src->id &&
				    r->src_id != KDBUS_MATCH_ID_ANY)
					return false;

				break;

			case KDBUS_ITEM_NAME:
				if (!kdbus_conn_has_name(conn_src, r->name))
					return false;

				break;

			default:
				return false;
			}
		}
	}

	return true;
}

/**
 * kdbus_match_db_match_kmsg() - match a kmsg object agains the database entries
 * @db:			The match database
 * @conn_src:		The connection object originating the message
 * @kmsg:		The kmsg to perform the match on
 *
 * This function will walk through all the database entries previously uploaded
 * with kdbus_match_db_add(). As soon as any of them has an all-satisfied rule
 * set, this function will return true.
 *
 * Returns true in if there was a matching database entry, false otherwise.
 */
bool kdbus_match_db_match_kmsg(struct kdbus_match_db *db,
			       struct kdbus_conn *conn_src,
			       struct kdbus_kmsg *kmsg)
{
	struct kdbus_match_entry *entry;
	bool matched = false;

	mutex_lock(&db->entries_lock);
	list_for_each_entry(entry, &db->entries_list, list_entry) {
		matched = kdbus_match_rules(entry, conn_src, kmsg);
		if (matched)
			break;
	}
	mutex_unlock(&db->entries_lock);

	return matched;
}

static int cmd_match_from_user(const struct kdbus_conn *conn,
			       void __user *buf, bool items,
			       struct kdbus_cmd_match **m)
{
	struct kdbus_cmd_match *cmd_match;
	u64 size;

	if (kdbus_size_get_user(&size, buf, struct kdbus_cmd_match))
		return -EFAULT;

	if (size < sizeof(*cmd_match) || size > KDBUS_MATCH_MAX_SIZE)
		return -EMSGSIZE;

	/* remove does not accept any items */
	if (!items && size != sizeof(*cmd_match))
		return -EMSGSIZE;

	cmd_match = memdup_user(buf, size);
	if (IS_ERR(cmd_match))
		return PTR_ERR(cmd_match);

	/* privileged users can act on behalf of someone else */
	if (cmd_match->owner_id == 0)
		cmd_match->owner_id = conn->id;
	else if (cmd_match->owner_id != conn->id &&
		 !kdbus_bus_uid_is_privileged(conn->ep->bus)) {
		kfree(cmd_match);
		return -EPERM;
	}

	*m = cmd_match;

	return 0;
}

/**
 * kdbus_match_db_add() - add an entry to the match database
 * @conn:		The connection that was used in the ioctl call
 * @buf:		The __user buffer that was provided by the ioctl call
 *
 * Returns 0 in success, any other value in case of errors.
 * This function is used in the context of the KDBUS_CMD_MATCH_ADD ioctl
 * interface.
 *
 * One call to this function (or one ioctl(KDBUS_CMD_MATCH_ADD), respectively,
 * adds one new database entry with n rules attached to it. Each rule is
 * described with an kdbus_item, and an entry is considered matching if all
 * its rules are satisfied.
 *
 * The items attached to a kdbus_cmd_match struct have the following mapping:
 *
 * KDBUS_ITEM_BLOOM:		Denotes a bloom mask
 * KDBUS_ITEM_NAME:		Denotes a connection's source name
 * KDBUS_ITEM_ID:		Denotes a connection's ID
 * KDBUS_ITEM_NAME_ADD:
 * KDBUS_ITEM_NAME_REMOVE:
 * KDBUS_ITEM_NAME_CHANGE:	Describe kdbus_notify_name_change prototypes
 * KDBUS_ITEM_ID_ADD:
 * KDBUS_ITEM_ID_REMOVE:	Describe kdbus_notify_id_change prototypes
 *
 * For kdbus_notify_{id,name}_change structs, only the ID and name fields
 * are looked at at when adding an entry. The flags are unused.
 *
 * Also note that KDBUS_ITEM_BLOOM, KDBUS_ITEM_NAME and KDBUS_ITEM_ID are
 * used to match messages from userspace, while the others apply to kernel-
 * generated notifications.
 */
int kdbus_match_db_add(struct kdbus_conn *conn, void __user *buf)
{
	struct kdbus_conn *target_conn = NULL;
	struct kdbus_match_entry *entry = NULL;
	struct kdbus_cmd_match *cmd_match;
	struct kdbus_match_db *db;
	struct kdbus_item *item;
	LIST_HEAD(list);
	int ret;

	ret = cmd_match_from_user(conn, buf, true, &cmd_match);
	if (ret < 0)
		return ret;

	if (cmd_match->owner_id != 0 && cmd_match->owner_id != conn->id) {
		struct kdbus_bus *bus = conn->ep->bus;

		mutex_lock(&bus->lock);
		target_conn = kdbus_bus_find_conn_by_id(bus,
							cmd_match->owner_id);
		mutex_unlock(&bus->lock);

		if (!target_conn) {
			ret = -ENXIO;
			goto exit_free;
		}

		db = target_conn->match_db;
	} else {
		db = conn->match_db;
	}

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry) {
		ret = -ENOMEM;
		goto exit_free;
	}

	entry->cookie = cmd_match->cookie;

	INIT_LIST_HEAD(&entry->rules_list);

	KDBUS_ITEM_FOREACH(item, cmd_match, items) {
		struct kdbus_match_rule *rule;
		size_t size = item->size - offsetof(struct kdbus_item, data);

		if (!KDBUS_ITEM_VALID(item, cmd_match)) {
			ret = -EINVAL;
			break;
		}

		rule = kzalloc(sizeof(*rule), GFP_KERNEL);
		if (!rule) {
			ret = -ENOMEM;
			break;
		}

		rule->type = item->type;

		switch (item->type) {
		case KDBUS_ITEM_BLOOM:
			if (size != conn->ep->bus->bloom_size) {
				ret = -EBADMSG;
				break;
			}

			rule->bloom = kmemdup(item->data, size, GFP_KERNEL);
			if (!rule->bloom) {
				ret = -ENOMEM;
				break;
			}

			break;

		case KDBUS_ITEM_NAME:
			if (size == 0) {
				ret = -EINVAL;
				break;
			}

			rule->name = kstrdup(item->str, GFP_KERNEL);
			if (!rule->name)
				ret = -ENOMEM;

			break;

		case KDBUS_ITEM_ID:
			if (size < sizeof(u64)) {
				ret = -EINVAL;
				break;
			}

			rule->src_id = item->id;
			break;

		case KDBUS_ITEM_NAME_ADD:
		case KDBUS_ITEM_NAME_REMOVE:
		case KDBUS_ITEM_NAME_CHANGE: {
			if (size < sizeof(struct kdbus_notify_name_change)) {
				ret = -EINVAL;
				break;
			}

			rule->old_id = item->name_change.old.id;
			rule->new_id = item->name_change.new.id;

			if (size > sizeof(struct kdbus_notify_name_change)) {
				rule->name = kstrdup(item->name_change.name,
						     GFP_KERNEL);
				if (!rule->name)
					ret = -ENOMEM;
			}

			break;
		}

		case KDBUS_ITEM_ID_ADD:
		case KDBUS_ITEM_ID_REMOVE:
			if (size < sizeof(struct kdbus_notify_id_change)) {
				ret = -EINVAL;
				break;
			}

			if (item->type == KDBUS_ITEM_ID_ADD)
				rule->new_id = item->id_change.id;
			else
				rule->old_id = item->id_change.id;

			break;

		default:
			ret = -EINVAL;
			break;
		}

		if (ret < 0)
			break;

		list_add_tail(&rule->rules_entry, &entry->rules_list);
	}

	if (ret == 0 && !KDBUS_ITEM_END(item, cmd_match))
		ret = -EINVAL;

	if (ret == 0)
		list_add_tail(&entry->list_entry, &db->entries_list);
	else
		kdbus_match_entry_free(entry);

exit_free:
	kdbus_conn_unref(target_conn);
	kfree(cmd_match);

	return ret;
}

/**
 * kdbus_match_db_remove() - remove an entry from the match database
 * @conn:		The connection that was used in the ioctl call
 * @buf:		The __user buffer that was provided by the ioctl call
 *
 * Returns 0 in success, any other value in case of errors.
 * This function is used in the context of the KDBUS_CMD_MATCH_REMOVE
 * ioctl interface.
 */
int kdbus_match_db_remove(struct kdbus_conn *conn, void __user *buf)
{
	struct kdbus_conn *target_conn = NULL;
	struct kdbus_match_db *db;
	struct kdbus_cmd_match *cmd_match = NULL;
	struct kdbus_match_entry *entry, *tmp;
	int ret;

	ret = cmd_match_from_user(conn, buf, false, &cmd_match);
	if (ret < 0)
		return ret;

	if (cmd_match->owner_id != 0 && cmd_match->owner_id != conn->id) {
		struct kdbus_bus *bus = conn->ep->bus;

		mutex_lock(&bus->lock);
		target_conn = kdbus_bus_find_conn_by_id(bus,
							cmd_match->owner_id);
		mutex_unlock(&bus->lock);

		if (!target_conn) {
			kfree(cmd_match);
			return -ENXIO;
		}

		db = target_conn->match_db;
	} else {
		db = conn->match_db;
	}

	mutex_lock(&db->entries_lock);
	list_for_each_entry_safe(entry, tmp, &db->entries_list, list_entry)
		if (entry->cookie == cmd_match->cookie)
			kdbus_match_entry_free(entry);
	mutex_unlock(&db->entries_lock);

	kdbus_conn_unref(target_conn);
	kfree(cmd_match);

	return 0;
}
