/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
reiser4/README */

/* This file contains code for various block number sets used by the atom to
   track the deleted set and wandered block mappings. */

#include "debug.h"
#include "dformat.h"
#include "txnmgr.h"
#include "context.h"
#include "super.h"

#include <linux/slab.h>

/* The proposed data structure for storing unordered block number sets is a
   list of elements, each of which contains an array of block number or/and
   array of block number pairs. That element called blocknr_set_entry is used
   to store block numbers from the beginning and for extents from the end of
   the data field (char data[...]). The ->nr_blocks and ->nr_pairs fields
   count numbers of blocks and extents.

   +------------------- blocknr_set_entry->data ------------------+
   |block1|block2| ... <free space> ... |pair3|pair2|pair1|
   +------------------------------------------------------------+

   When current blocknr_set_entry is full, allocate a new one. */

/* Usage examples: blocknr sets are used in reiser4 for storing atom's delete
 * set (single blocks and block extents), in that case blocknr pair represent an
 * extent; atom's wandered map is also stored as a blocknr set, blocknr pairs
 * there represent a (real block) -> (wandered block) mapping. */

/* Protection: blocknr sets belong to reiser4 atom, and
 * their modifications are performed with the atom lock held */

/* The total size of a blocknr_set_entry. */
#define BLOCKNR_SET_ENTRY_SIZE 128

/* The number of blocks that can fit the blocknr data area. */
#define BLOCKNR_SET_ENTRIES_NUMBER		\
       ((BLOCKNR_SET_ENTRY_SIZE -		\
	2 * sizeof(unsigned) -			\
	sizeof(struct list_head)) /		\
	sizeof(reiser4_block_nr))

static struct kmem_cache *blocknr_set_slab = NULL;

/* An entry of the blocknr_set */
struct blocknr_set_entry {
	unsigned nr_singles;
	unsigned nr_pairs;
	struct list_head link;
	reiser4_block_nr entries[BLOCKNR_SET_ENTRIES_NUMBER];
};

/* A pair of blocks as recorded in the blocknr_set_entry data. */
struct blocknr_pair {
	reiser4_block_nr a;
	reiser4_block_nr b;
};

/* Return the number of blocknr slots available in a blocknr_set_entry. */
/* Audited by: green(2002.06.11) */
static unsigned bse_avail(blocknr_set_entry * bse)
{
	unsigned used = bse->nr_singles + 2 * bse->nr_pairs;

	assert("jmacd-5088", BLOCKNR_SET_ENTRIES_NUMBER >= used);
	cassert(sizeof(blocknr_set_entry) == BLOCKNR_SET_ENTRY_SIZE);

	return BLOCKNR_SET_ENTRIES_NUMBER - used;
}

/* Initialize a blocknr_set_entry. */
static void bse_init(blocknr_set_entry *bse)
{
	bse->nr_singles = 0;
	bse->nr_pairs = 0;
	INIT_LIST_HEAD(&bse->link);
}

/* Allocate and initialize a blocknr_set_entry. */
/* Audited by: green(2002.06.11) */
static blocknr_set_entry *bse_alloc(void)
{
	blocknr_set_entry *e;

	if ((e = (blocknr_set_entry *) kmem_cache_alloc(blocknr_set_slab,
							reiser4_ctx_gfp_mask_get())) == NULL)
		return NULL;

	bse_init(e);

	return e;
}

/* Free a blocknr_set_entry. */
/* Audited by: green(2002.06.11) */
static void bse_free(blocknr_set_entry * bse)
{
	kmem_cache_free(blocknr_set_slab, bse);
}

/* Add a block number to a blocknr_set_entry */
/* Audited by: green(2002.06.11) */
static void
bse_put_single(blocknr_set_entry * bse, const reiser4_block_nr * block)
{
	assert("jmacd-5099", bse_avail(bse) >= 1);

	bse->entries[bse->nr_singles++] = *block;
}

/* Get a pair of block numbers */
/* Audited by: green(2002.06.11) */
static inline struct blocknr_pair *bse_get_pair(blocknr_set_entry * bse,
						unsigned pno)
{
	assert("green-1", BLOCKNR_SET_ENTRIES_NUMBER >= 2 * (pno + 1));

	return (struct blocknr_pair *) (bse->entries +
					BLOCKNR_SET_ENTRIES_NUMBER -
					2 * (pno + 1));
}

/* Add a pair of block numbers to a blocknr_set_entry */
/* Audited by: green(2002.06.11) */
static void
bse_put_pair(blocknr_set_entry * bse, const reiser4_block_nr * a,
	     const reiser4_block_nr * b)
{
	struct blocknr_pair *pair;

	assert("jmacd-5100", bse_avail(bse) >= 2 && a != NULL && b != NULL);

	pair = bse_get_pair(bse, bse->nr_pairs++);

	pair->a = *a;
	pair->b = *b;
}

/* Add either a block or pair of blocks to the block number set.  The first
   blocknr (@a) must be non-NULL.  If @b is NULL a single blocknr is added, if
   @b is non-NULL a pair is added.  The block number set belongs to atom, and
   the call is made with the atom lock held.  There may not be enough space in
   the current blocknr_set_entry.  If new_bsep points to a non-NULL
   blocknr_set_entry then it will be added to the blocknr_set and new_bsep
   will be set to NULL.  If new_bsep contains NULL then the atom lock will be
   released and a new bse will be allocated in new_bsep.  E_REPEAT will be
   returned with the atom unlocked for the operation to be tried again.  If
   the operation succeeds, 0 is returned.  If new_bsep is non-NULL and not
   used during the call, it will be freed automatically. */
static int blocknr_set_add(txn_atom *atom, struct list_head *bset,
			   blocknr_set_entry **new_bsep, const reiser4_block_nr *a,
			   const reiser4_block_nr *b)
{
	blocknr_set_entry *bse;
	unsigned entries_needed;

	assert("jmacd-5101", a != NULL);

	entries_needed = (b == NULL) ? 1 : 2;
	if (list_empty(bset) ||
	    bse_avail(list_entry(bset->next, blocknr_set_entry, link)) < entries_needed) {
		/* See if a bse was previously allocated. */
		if (*new_bsep == NULL) {
			spin_unlock_atom(atom);
			*new_bsep = bse_alloc();
			return (*new_bsep != NULL) ? -E_REPEAT :
				RETERR(-ENOMEM);
		}

		/* Put it on the head of the list. */
		list_add(&((*new_bsep)->link), bset);

		*new_bsep = NULL;
	}

	/* Add the single or pair. */
	bse = list_entry(bset->next, blocknr_set_entry, link);
	if (b == NULL) {
		bse_put_single(bse, a);
	} else {
		bse_put_pair(bse, a, b);
	}

	/* If new_bsep is non-NULL then there was an allocation race, free this
	   copy. */
	if (*new_bsep != NULL) {
		bse_free(*new_bsep);
		*new_bsep = NULL;
	}

	return 0;
}

/* Add an extent to the block set.  If the length is 1, it is treated as a
   single block (e.g., reiser4_set_add_block). */
/* Audited by: green(2002.06.11) */
/* Auditor note: Entire call chain cannot hold any spinlocks, because
   kmalloc might schedule. The only exception is atom spinlock, which is
   properly freed. */
int
blocknr_set_add_extent(txn_atom * atom,
		       struct list_head *bset,
		       blocknr_set_entry ** new_bsep,
		       const reiser4_block_nr * start,
		       const reiser4_block_nr * len)
{
	assert("jmacd-5102", start != NULL && len != NULL && *len > 0);
	return blocknr_set_add(atom, bset, new_bsep, start,
			       *len == 1 ? NULL : len);
}

/* Add a block pair to the block set. It adds exactly a pair, which is checked
 * by an assertion that both arguments are not null.*/
/* Audited by: green(2002.06.11) */
/* Auditor note: Entire call chain cannot hold any spinlocks, because
   kmalloc might schedule. The only exception is atom spinlock, which is
   properly freed. */
int
blocknr_set_add_pair(txn_atom * atom,
		     struct list_head *bset,
		     blocknr_set_entry ** new_bsep, const reiser4_block_nr * a,
		     const reiser4_block_nr * b)
{
	assert("jmacd-5103", a != NULL && b != NULL);
	return blocknr_set_add(atom, bset, new_bsep, a, b);
}

/* Initialize slab cache of blocknr_set_entry objects. */
int blocknr_set_init_static(void)
{
	assert("intelfx-55", blocknr_set_slab == NULL);

	blocknr_set_slab = kmem_cache_create("blocknr_set_entry",
					     sizeof(blocknr_set_entry),
					     0,
					     SLAB_HWCACHE_ALIGN |
					     SLAB_RECLAIM_ACCOUNT,
					     NULL);

	if (blocknr_set_slab == NULL) {
		return RETERR(-ENOMEM);
	}

	return 0;
}

/* Destroy slab cache of blocknr_set_entry objects. */
void blocknr_set_done_static(void)
{
	destroy_reiser4_cache(&blocknr_set_slab);
}

/* Initialize a blocknr_set. */
void blocknr_set_init(struct list_head *bset)
{
	INIT_LIST_HEAD(bset);
}

/* Release the entries of a blocknr_set. */
void blocknr_set_destroy(struct list_head *bset)
{
	blocknr_set_entry *bse;

	while (!list_empty(bset)) {
		bse = list_entry(bset->next, blocknr_set_entry, link);
		list_del_init(&bse->link);
		bse_free(bse);
	}
}

/* Merge blocknr_set entries out of @from into @into. */
/* Audited by: green(2002.06.11) */
/* Auditor comments: This merge does not know if merged sets contain
   blocks pairs (As for wandered sets) or extents, so it cannot really merge
   overlapping ranges if there is some. So I believe it may lead to
   some blocks being presented several times in one blocknr_set. To help
   debugging such problems it might help to check for duplicate entries on
   actual processing of this set. Testing this kind of stuff right here is
   also complicated by the fact that these sets are not sorted and going
   through whole set on each element addition is going to be CPU-heavy task */
void blocknr_set_merge(struct list_head *from, struct list_head *into)
{
	blocknr_set_entry *bse_into = NULL;

	/* If @from is empty, no work to perform. */
	if (list_empty(from))
		return;
	/* If @into is not empty, try merging partial-entries. */
	if (!list_empty(into)) {

		/* Neither set is empty, pop the front to members and try to
		   combine them. */
		blocknr_set_entry *bse_from;
		unsigned into_avail;

		bse_into = list_entry(into->next, blocknr_set_entry, link);
		list_del_init(&bse_into->link);
		bse_from = list_entry(from->next, blocknr_set_entry, link);
		list_del_init(&bse_from->link);

		/* Combine singles. */
		for (into_avail = bse_avail(bse_into);
		     into_avail != 0 && bse_from->nr_singles != 0;
		     into_avail -= 1) {
			bse_put_single(bse_into,
				       &bse_from->entries[--bse_from->
							  nr_singles]);
		}

		/* Combine pairs. */
		for (; into_avail > 1 && bse_from->nr_pairs != 0;
		     into_avail -= 2) {
			struct blocknr_pair *pair =
				bse_get_pair(bse_from, --bse_from->nr_pairs);
			bse_put_pair(bse_into, &pair->a, &pair->b);
		}

		/* If bse_from is empty, delete it now. */
		if (bse_avail(bse_from) == BLOCKNR_SET_ENTRIES_NUMBER) {
			bse_free(bse_from);
		} else {
			/* Otherwise, bse_into is full or nearly full (e.g.,
			   it could have one slot avail and bse_from has one
			   pair left).  Push it back onto the list.  bse_from
			   becomes bse_into, which will be the new partial. */
			list_add(&bse_into->link, into);
			bse_into = bse_from;
		}
	}

	/* Splice lists together. */
	list_splice_init(from, into->prev);

	/* Add the partial entry back to the head of the list. */
	if (bse_into != NULL)
		list_add(&bse_into->link, into);
}

/* Iterate over all blocknr set elements. */
int blocknr_set_iterator(txn_atom *atom, struct list_head *bset,
			 blocknr_set_actor_f actor, void *data, int delete)
{

	blocknr_set_entry *entry;

	assert("zam-429", atom != NULL);
	assert("zam-430", atom_is_protected(atom));
	assert("zam-431", bset != 0);
	assert("zam-432", actor != NULL);

	entry = list_entry(bset->next, blocknr_set_entry, link);
	while (bset != &entry->link) {
		blocknr_set_entry *tmp = list_entry(entry->link.next, blocknr_set_entry, link);
		unsigned int i;
		int ret;

		for (i = 0; i < entry->nr_singles; i++) {
			ret = actor(atom, &entry->entries[i], NULL, data);

			/* We can't break a loop if delete flag is set. */
			if (ret != 0 && !delete)
				return ret;
		}

		for (i = 0; i < entry->nr_pairs; i++) {
			struct blocknr_pair *ab;

			ab = bse_get_pair(entry, i);

			ret = actor(atom, &ab->a, &ab->b, data);

			if (ret != 0 && !delete)
				return ret;
		}

		if (delete) {
			list_del(&entry->link);
			bse_free(entry);
		}

		entry = tmp;
	}

	return 0;
}

/*
 * Local variables:
 * c-indentation-style: "K&R"
 * mode-name: "LC"
 * c-basic-offset: 8
 * tab-width: 8
 * fill-column: 79
 * scroll-step: 1
 * End:
 */
