/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

/* This is a block list implementation, used to create ordered block sets
   (at the cost of being less memory efficient than blocknr_set).
   It is used by discard code. */

#include "debug.h"
#include "dformat.h"
#include "txnmgr.h"
#include "context.h"
#include "super.h"

#include <linux/slab.h>
#include <linux/list_sort.h>

static struct kmem_cache *blocknr_list_slab = NULL;

/**
 * Represents an extent range [@start; @end).
 */
struct blocknr_list_entry {
	reiser4_block_nr start, len;
	struct list_head link;
};

#define blocknr_list_entry(ptr) list_entry(ptr, blocknr_list_entry, link)

static void blocknr_list_entry_init(blocknr_list_entry *entry)
{
	assert("intelfx-11", entry != NULL);

	entry->start = 0;
	entry->len = 0;
	INIT_LIST_HEAD(&entry->link);
}

static blocknr_list_entry *blocknr_list_entry_alloc(void)
{
	blocknr_list_entry *entry;

	entry = (blocknr_list_entry *)kmem_cache_alloc(blocknr_list_slab,
	                                               reiser4_ctx_gfp_mask_get());
	if (entry == NULL) {
		return NULL;
	}

	blocknr_list_entry_init(entry);

	return entry;
}

static void blocknr_list_entry_free(blocknr_list_entry *entry)
{
	assert("intelfx-12", entry != NULL);

	kmem_cache_free(blocknr_list_slab, entry);
}

/**
 * Given ranges @to and [@start; @end), if they overlap, their union
 * is calculated and saved in @to.
 */
static int blocknr_list_entry_merge(blocknr_list_entry *to,
                                    reiser4_block_nr start,
                                    reiser4_block_nr len)
{
	reiser4_block_nr end, to_end;

	assert("intelfx-13", to != NULL);

	assert("intelfx-16", to->len > 0);
	assert("intelfx-17", len > 0);

	end = start + len;
	to_end = to->start + to->len;

	if ((to->start <= end) && (start <= to_end)) {
		if (start < to->start) {
			to->start = start;
		}

		if (end > to_end) {
			to_end = end;
		}

		to->len = to_end - to->start;

		return 0;
	}

	return -1;
}

static int blocknr_list_entry_merge_entry(blocknr_list_entry *to,
                                          blocknr_list_entry *from)
{
	assert("intelfx-18", from != NULL);

	return blocknr_list_entry_merge(to, from->start, from->len);
}

/**
 * A comparison function for list_sort().
 *
 * "The comparison function @cmp must return a negative value if @a
 * should sort before @b, and a positive value if @a should sort after
 * @b. If @a and @b are equivalent, and their original relative
 * ordering is to be preserved, @cmp must return 0."
 */
static int blocknr_list_entry_compare(void* priv UNUSED_ARG,
                                      struct list_head *a, struct list_head *b)
{
	blocknr_list_entry *entry_a, *entry_b;
	reiser4_block_nr entry_a_end, entry_b_end;

	assert("intelfx-19", a != NULL);
	assert("intelfx-20", b != NULL);

	entry_a = blocknr_list_entry(a);
	entry_b = blocknr_list_entry(b);

	entry_a_end = entry_a->start + entry_a->len;
	entry_b_end = entry_b->start + entry_b->len;

	/* First sort by starting block numbers... */
	if (entry_a->start < entry_b->start) {
		return -1;
	}

	if (entry_a->start > entry_b->start) {
		return 1;
	}

	/** Then by ending block numbers.
	 * If @a contains @b, it will be sorted before. */
	if (entry_a_end > entry_b_end) {
		return -1;
	}

	if (entry_a_end < entry_b_end) {
		return 1;
	}

	return 0;
}

int blocknr_list_init_static(void)
{
	assert("intelfx-54", blocknr_list_slab == NULL);

	blocknr_list_slab = kmem_cache_create("blocknr_list_entry",
	                                      sizeof(blocknr_list_entry),
	                                      0,
	                                      SLAB_HWCACHE_ALIGN |
	                                      SLAB_RECLAIM_ACCOUNT,
	                                      NULL);
	if (blocknr_list_slab == NULL) {
		return RETERR(-ENOMEM);
	}

	return 0;
}

void blocknr_list_done_static(void)
{
	destroy_reiser4_cache(&blocknr_list_slab);
}

void blocknr_list_init(struct list_head* blist)
{
	assert("intelfx-24", blist != NULL);

	INIT_LIST_HEAD(blist);
}

void blocknr_list_destroy(struct list_head* blist)
{
	struct list_head *pos, *tmp;
	blocknr_list_entry *entry;

	assert("intelfx-25", blist != NULL);

	list_for_each_safe(pos, tmp, blist) {
		entry = blocknr_list_entry(pos);
		list_del_init(pos);
		blocknr_list_entry_free(entry);
	}

	assert("intelfx-48", list_empty(blist));
}

void blocknr_list_merge(struct list_head *from, struct list_head *to)
{
	assert("intelfx-26", from != NULL);
	assert("intelfx-27", to != NULL);

	list_splice_tail_init(from, to);

	assert("intelfx-49", list_empty(from));
}

void blocknr_list_sort_and_join(struct list_head *blist)
{
	struct list_head *pos, *next;
	struct blocknr_list_entry *entry, *next_entry;

	assert("intelfx-50", blist != NULL);

	/* Step 1. Sort the extent list. */
	list_sort(NULL, blist, blocknr_list_entry_compare);

	/* Step 2. Join adjacent extents in the list. */
	pos = blist->next;
	next = pos->next;
	entry = blocknr_list_entry(pos);

	for (; next != blist; next = pos->next) {
		/** @next is a valid node at this point */
		next_entry = blocknr_list_entry(next);

		/** try to merge @next into @pos */
		if (!blocknr_list_entry_merge_entry(entry, next_entry)) {
			/** successful; delete the @next node.
			 * next merge will be attempted into the same node. */
			list_del_init(next);
			blocknr_list_entry_free(next_entry);
		} else {
			/** otherwise advance @pos. */
			pos = next;
			entry = next_entry;
		}
	}
}

int blocknr_list_add_extent(txn_atom *atom,
                            struct list_head *blist,
                            blocknr_list_entry **new_entry,
                            const reiser4_block_nr *start,
                            const reiser4_block_nr *len)
{
	assert("intelfx-29", atom != NULL);
	assert("intelfx-42", atom_is_protected(atom));
	assert("intelfx-43", blist != NULL);
	assert("intelfx-30", new_entry != NULL);
	assert("intelfx-31", start != NULL);
	assert("intelfx-32", len != NULL && *len > 0);

	if (*new_entry == NULL) {
		/*
		 * Optimization: try to merge new extent into the last one.
		 */
		if (!list_empty(blist)) {
			blocknr_list_entry *last_entry;
			last_entry = blocknr_list_entry(blist->prev);
			if (!blocknr_list_entry_merge(last_entry, *start, *len)) {
				return 0;
			}
		}

		/*
		 * Otherwise, allocate a new entry and tell -E_REPEAT.
		 * Next time we'll take the branch below.
		 */
		spin_unlock_atom(atom);
		*new_entry = blocknr_list_entry_alloc();
		return (*new_entry != NULL) ? -E_REPEAT : RETERR(-ENOMEM);
	}

	/*
	 * The entry has been allocated beforehand, fill it and link to the list.
	 */
	(*new_entry)->start = *start;
	(*new_entry)->len = *len;
	list_add_tail(&(*new_entry)->link, blist);

	return 0;
}

int blocknr_list_iterator(txn_atom *atom,
                          struct list_head *blist,
                          blocknr_set_actor_f actor,
                          void *data,
                          int delete)
{
	struct list_head *pos;
	blocknr_list_entry *entry;
	int ret = 0;

	assert("intelfx-46", blist != NULL);
	assert("intelfx-47", actor != NULL);

	if (delete) {
		struct list_head *tmp;

		list_for_each_safe(pos, tmp, blist) {
			entry = blocknr_list_entry(pos);

			/*
			 * Do not exit, delete flag is set. Instead, on the first error we
			 * downgrade from iterating to just deleting.
			 */
			if (ret == 0) {
				ret = actor(atom, &entry->start, &entry->len, data);
			}

			list_del_init(pos);
			blocknr_list_entry_free(entry);
		}

		assert("intelfx-44", list_empty(blist));
	} else {
		list_for_each(pos, blist) {
			entry = blocknr_list_entry(pos);

			ret = actor(atom, &entry->start, &entry->len, data);

			if (ret != 0) {
				return ret;
			}
		}
	}

	return ret;
}

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   scroll-step: 1
   End:
*/
