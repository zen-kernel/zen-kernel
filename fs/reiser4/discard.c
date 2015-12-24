/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

/* TRIM/discard interoperation subsystem for reiser4. */

/*
 * This subsystem is responsible for populating an atom's ->discard_set and
 * (later) converting it into a series of discard calls to the kernel.
 *
 * The discard is an in-kernel interface for notifying the storage
 * hardware about blocks that are being logically freed by the filesystem.
 * This is done via calling the blkdev_issue_discard() function. There are
 * restrictions on block ranges: they should constitute at least one erase unit
 * in length and be correspondingly aligned. Otherwise a discard request will
 * be ignored.
 *
 * The erase unit size is kept in struct queue_limits as discard_granularity.
 * The offset from the partition start to the first erase unit is kept in
 * struct queue_limits as discard_alignment.
 *
 * At atom level, we record numbers of all blocks that happen to be deallocated
 * during the transaction. Then we read the generated set, filter out any blocks
 * that have since been allocated again and issue discards for everything still
 * valid. This is what discard.[ch] is here for.
 *
 * However, simply iterating through the recorded extents is not enough:
 * - if a single extent is smaller than the erase unit, then this particular
 *   extent won't be discarded even if it is surrounded by enough free blocks
 *   to constitute a whole erase unit;
 * - we won't be able to merge small adjacent extents forming an extent long
 *   enough to be discarded.
 *
 * MECHANISM:
 *
 * During the transaction deallocated extents are recorded in atom's delete
 * set. In reiser4, there are two methods to deallocate a block:
 * 1. deferred deallocation, enabled by BA_DEFER flag to reiser4_dealloc_block().
 *    In this mode, blocks are stored to delete set instead of being marked free
 *    immediately. After committing the transaction, the delete set is "applied"
 *    by the block allocator and all these blocks are marked free in memory
 *    (see reiser4_post_write_back_hook()).
 *    Space management plugins also read the delete set to update on-disk
 *    allocation records (see reiser4_pre_commit_hook()).
 * 2. immediate deallocation (the opposite).
 *    In this mode, blocks are marked free immediately. This is used by the
 *    journal subsystem to manage space used by the journal records, so these
 *    allocations are not visible to the space management plugins and never hit
 *    the disk.
 *
 * When discard is enabled, all immediate deallocations become deferred. This
 * is OK because journal's allocations happen after reiser4_pre_commit_hook()
 * where the on-disk space allocation records are updated. So, in this mode
 * the atom's delete set becomes "the discard set" -- list of blocks that have
 * to be considered for discarding.
 *
 * Discarding is performed before completing deferred deallocations, hence all
 * extents in the discard set are still marked as allocated and cannot contain
 * any data. Thus we can avoid any checks for blocks directly present in the
 * discard set.
 *
 * For now, we don't perform "padding" of extents to erase unit boundaries.
 * This means if extents are not aligned with the device's erase unit lattice,
 * the partial erase units at head and tail of extents are truncated by kernel
 * (in blkdev_issue_discard()).
 *
 * So, at commit time the following actions take place:
 * - delete sets are merged to form the discard set;
 * - elements of the discard set are sorted;
 * - the discard set is iterated, joining any adjacent extents;
 * - for each extent, a single call to blkdev_issue_discard() is done.
 */

#include "discard.h"
#include "context.h"
#include "debug.h"
#include "txnmgr.h"
#include "super.h"

#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/blkdev.h>

static int __discard_extent(struct block_device *bdev, sector_t start,
                            sector_t len)
{
	assert("intelfx-21", bdev != NULL);

	return blkdev_issue_discard(bdev, start, len, reiser4_ctx_gfp_mask_get(),
	                            0);
}

static int discard_extent(txn_atom *atom UNUSED_ARG,
                          const reiser4_block_nr* start,
                          const reiser4_block_nr* len,
                          void *data UNUSED_ARG)
{
	struct super_block *sb = reiser4_get_current_sb();
	struct block_device *bdev = sb->s_bdev;

	sector_t extent_start_sec, extent_len_sec;

	const int sec_per_blk = sb->s_blocksize >> 9;

	/* we assume block = N * sector */
	assert("intelfx-7", sec_per_blk > 0);

	/* convert extent to sectors */
	extent_start_sec = *start * sec_per_blk;
	extent_len_sec = *len * sec_per_blk;

	/* discard the extent, don't pad it to erase unit boundaries for now */
	return __discard_extent(bdev, extent_start_sec, extent_len_sec);
}

int discard_atom(txn_atom *atom, struct list_head *processed_set)
{
	int ret;
	struct list_head discard_set;

	if (!reiser4_is_set(reiser4_get_current_sb(), REISER4_DISCARD)) {
		spin_unlock_atom(atom);
		return 0;
	}

	assert("intelfx-28", atom != NULL);
	assert("intelfx-59", processed_set != NULL);

	if (list_empty(&atom->discard.delete_set)) {
		/* Nothing left to discard. */
		spin_unlock_atom(atom);
		return 0;
	}

	/* Take the delete sets from the atom in order to release atom spinlock. */
	blocknr_list_init(&discard_set);
	blocknr_list_merge(&atom->discard.delete_set, &discard_set);
	spin_unlock_atom(atom);

	/* Sort the discard list, joining adjacent and overlapping extents. */
	blocknr_list_sort_and_join(&discard_set);

	/* Perform actual dirty work. */
	ret = blocknr_list_iterator(NULL, &discard_set, &discard_extent, NULL, 0);

	/* Add processed extents to the temporary list. */
	blocknr_list_merge(&discard_set, processed_set);

	if (ret != 0) {
		return ret;
	}

	/* Let's do this again for any new extents in the atom's discard set. */
	return -E_REPEAT;
}

void discard_atom_post(txn_atom *atom, struct list_head *processed_set)
{
	assert("intelfx-60", atom != NULL);
	assert("intelfx-61", processed_set != NULL);

	if (!reiser4_is_set(reiser4_get_current_sb(), REISER4_DISCARD)) {
		spin_unlock_atom(atom);
		return;
	}

	blocknr_list_merge(processed_set, &atom->discard.delete_set);
	spin_unlock_atom(atom);
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
