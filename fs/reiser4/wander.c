/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

/* Reiser4 Wandering Log */

/* You should read http://www.namesys.com/txn-doc.html

   That describes how filesystem operations are performed as atomic
   transactions, and how we try to arrange it so that we can write most of the
   data only once while performing the operation atomically.

   For the purposes of this code, it is enough for it to understand that it
   has been told a given block should be written either once, or twice (if
   twice then once to the wandered location and once to the real location).

   This code guarantees that those blocks that are defined to be part of an
   atom either all take effect or none of them take effect.

   The "relocate set" of nodes are submitted to write by the jnode_flush()
   routine, and the "overwrite set" is submitted by reiser4_write_log().
   This is because with the overwrite set we seek to optimize writes, and
   with the relocate set we seek to cause disk order to correlate with the
   "parent first order" (preorder).

   reiser4_write_log() allocates and writes wandered blocks and maintains
   additional on-disk structures of the atom as wander records (each wander
   record occupies one block) for storing of the "wandered map" (a table which
   contains a relation between wandered and real block numbers) and other
   information which might be needed at transaction recovery time.

   The wander records are unidirectionally linked into a circle: each wander
   record contains a block number of the next wander record, the last wander
   record points to the first one.

   One wander record (named "tx head" in this file) has a format which is
   different from the other wander records. The "tx head" has a reference to the
   "tx head" block of the previously committed atom.  Also, "tx head" contains
   fs information (the free blocks counter, and the oid allocator state) which
   is logged in a special way .

   There are two journal control blocks, named journal header and journal
   footer which have fixed on-disk locations.  The journal header has a
   reference to the "tx head" block of the last committed atom.  The journal
   footer points to the "tx head" of the last flushed atom.  The atom is
   "played" when all blocks from its overwrite set are written to disk the
   second time (i.e. written to their real locations).

   NOTE: People who know reiserfs internals and its journal structure might be
   confused with these terms journal footer and journal header. There is a table
   with terms of similar semantics in reiserfs (reiser3) and reiser4:

   REISER3 TERM        |  REISER4 TERM         | DESCRIPTION
   --------------------+-----------------------+----------------------------
   commit record       |  journal header       | atomic write of this record
                       |                       | ends transaction commit
   --------------------+-----------------------+----------------------------
   journal header      |  journal footer       | atomic write of this record
                       |                       | ends post-commit writes.
                       |                       | After successful
                       |                       | writing of this journal
                       |                       | blocks (in reiser3) or
                       |                       | wandered blocks/records are
                       |                       | free for re-use.
   --------------------+-----------------------+----------------------------

   The atom commit process is the following:

   1. The overwrite set is taken from atom's clean list, and its size is
      counted.

   2. The number of necessary wander records (including tx head) is calculated,
      and the wander record blocks are allocated.

   3. Allocate wandered blocks and populate wander records by wandered map.

   4. submit write requests for wander records and wandered blocks.

   5. wait until submitted write requests complete.

   6. update journal header: change the pointer to the block number of just
   written tx head, submit an i/o for modified journal header block and wait
   for i/o completion.

   NOTE: The special logging for bitmap blocks and some reiser4 super block
   fields makes processes of atom commit, flush and recovering a bit more
   complex (see comments in the source code for details).

   The atom playing process is the following:

   1. Write atom's overwrite set in-place.

   2. Wait on i/o.

   3. Update journal footer: change the pointer to block number of tx head
   block of the atom we currently flushing, submit an i/o, wait on i/o
   completion.

   4. Free disk space which was used for wandered blocks and wander records.

   After the freeing of wandered blocks and wander records we have that journal
   footer points to the on-disk structure which might be overwritten soon.
   Neither the log writer nor the journal recovery procedure use that pointer
   for accessing the data.  When the journal recovery procedure finds the oldest
   transaction it compares the journal footer pointer value with the "prev_tx"
   pointer value in tx head, if values are equal the oldest not flushed
   transaction is found.

   NOTE on disk space leakage: the information about of what blocks and how many
   blocks are allocated for wandered blocks, wandered records is not written to
   the disk because of special logging for bitmaps and some super blocks
   counters.  After a system crash we the reiser4 does not remember those
   objects allocation, thus we have no such a kind of disk space leakage.
*/

/* Special logging of reiser4 super block fields. */

/* There are some reiser4 super block fields (free block count and OID allocator
   state (number of files and next free OID) which are logged separately from
   super block to avoid unnecessary atom fusion.

   So, the reiser4 super block can be not captured by a transaction with
   allocates/deallocates disk blocks or create/delete file objects.  Moreover,
   the reiser4 on-disk super block is not touched when such a transaction is
   committed and flushed.  Those "counters logged specially" are logged in "tx
   head" blocks and in the journal footer block.

   A step-by-step description of special logging:

   0. The per-atom information about deleted or created files and allocated or
   freed blocks is collected during the transaction.  The atom's
   ->nr_objects_created and ->nr_objects_deleted are for object
   deletion/creation tracking, the numbers of allocated and freed blocks are
   calculated using atom's delete set and atom's capture list -- all new and
   relocated nodes should be on atom's clean list and should have JNODE_RELOC
   bit set.

   1. The "logged specially" reiser4 super block fields have their "committed"
   versions in the reiser4 in-memory super block.  They get modified only at
   atom commit time.  The atom's commit thread has an exclusive access to those
   "committed" fields because the log writer implementation supports only one
   atom commit a time (there is a per-fs "commit" mutex).  At
   that time "committed" counters are modified using per-atom information
   collected during the transaction. These counters are stored on disk as a
   part of tx head block when atom is committed.

   2. When the atom is flushed the value of the free block counter and the OID
   allocator state get written to the journal footer block.  A special journal
   procedure (journal_recover_sb_data()) takes those values from the journal
   footer and updates the reiser4 in-memory super block.

   NOTE: That means free block count and OID allocator state are logged
   separately from the reiser4 super block regardless of the fact that the
   reiser4 super block has fields to store both the free block counter and the
   OID allocator.

   Writing the whole super block at commit time requires knowing true values of
   all its fields without changes made by not yet committed transactions. It is
   possible by having their "committed" version of the super block like the
   reiser4 bitmap blocks have "committed" and "working" versions.  However,
   another scheme was implemented which stores special logged values in the
   unused free space inside transaction head block.  In my opinion it has an
   advantage of not writing whole super block when only part of it was
   modified. */

#include "debug.h"
#include "dformat.h"
#include "txnmgr.h"
#include "jnode.h"
#include "znode.h"
#include "block_alloc.h"
#include "page_cache.h"
#include "wander.h"
#include "reiser4.h"
#include "super.h"
#include "vfs_ops.h"
#include "writeout.h"
#include "inode.h"
#include "entd.h"

#include <linux/types.h>
#include <linux/fs.h>		/* for struct super_block  */
#include <linux/mm.h>		/* for struct page */
#include <linux/pagemap.h>
#include <linux/bio.h>		/* for struct bio */
#include <linux/blkdev.h>

static int write_jnodes_to_disk_extent(
	jnode *, int, const reiser4_block_nr *, flush_queue_t *, int);

/* The commit_handle is a container for objects needed at atom commit time  */
struct commit_handle {
	/* A pointer to atom's list of OVRWR nodes */
	struct list_head *overwrite_set;
	/* atom's overwrite set size */
	int overwrite_set_size;
	/* jnodes for wander record blocks */
	struct list_head tx_list;
	/* number of wander records */
	__u32 tx_size;
	/* 'committed' sb counters are saved here until atom is completely
	   flushed  */
	__u64 free_blocks;
	__u64 nr_files;
	__u64 next_oid;
	/* A pointer to the atom which is being committed */
	txn_atom *atom;
	/* A pointer to current super block */
	struct super_block *super;
	/* The counter of modified bitmaps */
	reiser4_block_nr nr_bitmap;
};

static void init_commit_handle(struct commit_handle *ch, txn_atom *atom)
{
	memset(ch, 0, sizeof(struct commit_handle));
	INIT_LIST_HEAD(&ch->tx_list);

	ch->atom = atom;
	ch->super = reiser4_get_current_sb();
}

static void done_commit_handle(struct commit_handle *ch)
{
	assert("zam-690", list_empty(&ch->tx_list));
}

static inline int reiser4_use_write_barrier(struct super_block * s)
{
	return !reiser4_is_set(s, REISER4_NO_WRITE_BARRIER);
}

/* fill journal header block data  */
static void format_journal_header(struct commit_handle *ch)
{
	struct reiser4_super_info_data *sbinfo;
	struct journal_header *header;
	jnode *txhead;

	sbinfo = get_super_private(ch->super);
	assert("zam-479", sbinfo != NULL);
	assert("zam-480", sbinfo->journal_header != NULL);

	txhead = list_entry(ch->tx_list.next, jnode, capture_link);

	jload(sbinfo->journal_header);

	header = (struct journal_header *)jdata(sbinfo->journal_header);
	assert("zam-484", header != NULL);

	put_unaligned(cpu_to_le64(*jnode_get_block(txhead)),
		      &header->last_committed_tx);

	jrelse(sbinfo->journal_header);
}

/* fill journal footer block data */
static void format_journal_footer(struct commit_handle *ch)
{
	struct reiser4_super_info_data *sbinfo;
	struct journal_footer *footer;
	jnode *tx_head;

	sbinfo = get_super_private(ch->super);

	tx_head = list_entry(ch->tx_list.next, jnode, capture_link);

	assert("zam-493", sbinfo != NULL);
	assert("zam-494", sbinfo->journal_header != NULL);

	check_me("zam-691", jload(sbinfo->journal_footer) == 0);

	footer = (struct journal_footer *)jdata(sbinfo->journal_footer);
	assert("zam-495", footer != NULL);

	put_unaligned(cpu_to_le64(*jnode_get_block(tx_head)),
		      &footer->last_flushed_tx);
	put_unaligned(cpu_to_le64(ch->free_blocks), &footer->free_blocks);

	put_unaligned(cpu_to_le64(ch->nr_files), &footer->nr_files);
	put_unaligned(cpu_to_le64(ch->next_oid), &footer->next_oid);

	jrelse(sbinfo->journal_footer);
}

/* wander record capacity depends on current block size */
static int wander_record_capacity(const struct super_block *super)
{
	return (super->s_blocksize -
		sizeof(struct wander_record_header)) /
	    sizeof(struct wander_entry);
}

/* Fill first wander record (tx head) in accordance with supplied given data */
static void format_tx_head(struct commit_handle *ch)
{
	jnode *tx_head;
	jnode *next;
	struct tx_header *header;

	tx_head = list_entry(ch->tx_list.next, jnode, capture_link);
	assert("zam-692", &ch->tx_list != &tx_head->capture_link);

	next = list_entry(tx_head->capture_link.next, jnode, capture_link);
	if (&ch->tx_list == &next->capture_link)
		next = tx_head;

	header = (struct tx_header *)jdata(tx_head);

	assert("zam-460", header != NULL);
	assert("zam-462", ch->super->s_blocksize >= sizeof(struct tx_header));

	memset(jdata(tx_head), 0, (size_t) ch->super->s_blocksize);
	memcpy(jdata(tx_head), TX_HEADER_MAGIC, TX_HEADER_MAGIC_SIZE);

	put_unaligned(cpu_to_le32(ch->tx_size), &header->total);
	put_unaligned(cpu_to_le64(get_super_private(ch->super)->last_committed_tx),
		      &header->prev_tx);
	put_unaligned(cpu_to_le64(*jnode_get_block(next)), &header->next_block);
	put_unaligned(cpu_to_le64(ch->free_blocks), &header->free_blocks);
	put_unaligned(cpu_to_le64(ch->nr_files), &header->nr_files);
	put_unaligned(cpu_to_le64(ch->next_oid), &header->next_oid);
}

/* prepare ordinary wander record block (fill all service fields) */
static void
format_wander_record(struct commit_handle *ch, jnode *node, __u32 serial)
{
	struct wander_record_header *LRH;
	jnode *next;

	assert("zam-464", node != NULL);

	LRH = (struct wander_record_header *)jdata(node);
	next = list_entry(node->capture_link.next, jnode, capture_link);

	if (&ch->tx_list == &next->capture_link)
		next = list_entry(ch->tx_list.next, jnode, capture_link);

	assert("zam-465", LRH != NULL);
	assert("zam-463",
	       ch->super->s_blocksize > sizeof(struct wander_record_header));

	memset(jdata(node), 0, (size_t) ch->super->s_blocksize);
	memcpy(jdata(node), WANDER_RECORD_MAGIC, WANDER_RECORD_MAGIC_SIZE);

	put_unaligned(cpu_to_le32(ch->tx_size), &LRH->total);
	put_unaligned(cpu_to_le32(serial), &LRH->serial);
	put_unaligned(cpu_to_le64(*jnode_get_block(next)), &LRH->next_block);
}

/* add one wandered map entry to formatted wander record */
static void
store_entry(jnode * node, int index, const reiser4_block_nr * a,
	    const reiser4_block_nr * b)
{
	char *data;
	struct wander_entry *pairs;

	data = jdata(node);
	assert("zam-451", data != NULL);

	pairs =
	    (struct wander_entry *)(data + sizeof(struct wander_record_header));

	put_unaligned(cpu_to_le64(*a), &pairs[index].original);
	put_unaligned(cpu_to_le64(*b), &pairs[index].wandered);
}

/* currently, wander records contains contain only wandered map, which depend on
   overwrite set size */
static void get_tx_size(struct commit_handle *ch)
{
	assert("zam-440", ch->overwrite_set_size != 0);
	assert("zam-695", ch->tx_size == 0);

	/* count all ordinary wander records
	   (<overwrite_set_size> - 1) / <wander_record_capacity> + 1 and add one
	   for tx head block */
	ch->tx_size =
	    (ch->overwrite_set_size - 1) / wander_record_capacity(ch->super) +
	    2;
}

/* A special structure for using in store_wmap_actor() for saving its state
   between calls */
struct store_wmap_params {
	jnode *cur;		/* jnode of current wander record to fill */
	int idx;		/* free element index in wander record  */
	int capacity;		/* capacity  */

#if REISER4_DEBUG
	struct list_head *tx_list;
#endif
};

/* an actor for use in blocknr_set_iterator routine which populates the list
   of pre-formatted wander records by wandered map info */
static int
store_wmap_actor(txn_atom * atom UNUSED_ARG, const reiser4_block_nr * a,
		 const reiser4_block_nr * b, void *data)
{
	struct store_wmap_params *params = data;

	if (params->idx >= params->capacity) {
		/* a new wander record should be taken from the tx_list */
		params->cur = list_entry(params->cur->capture_link.next, jnode, capture_link);
		assert("zam-454",
		       params->tx_list != &params->cur->capture_link);

		params->idx = 0;
	}

	store_entry(params->cur, params->idx, a, b);
	params->idx++;

	return 0;
}

/* This function is called after Relocate set gets written to disk, Overwrite
   set is written to wandered locations and all wander records are written
   also. Updated journal header blocks contains a pointer (block number) to
   first wander record of the just written transaction */
static int update_journal_header(struct commit_handle *ch, int use_barrier)
{
	struct reiser4_super_info_data *sbinfo = get_super_private(ch->super);
	jnode *jh = sbinfo->journal_header;
	jnode *head = list_entry(ch->tx_list.next, jnode, capture_link);
	int ret;

	format_journal_header(ch);

	ret = write_jnodes_to_disk_extent(jh, 1, jnode_get_block(jh), NULL,
					  use_barrier ? WRITEOUT_BARRIER : 0);
	if (ret)
		return ret;

	/* blk_run_address_space(sbinfo->fake->i_mapping);
	 * blk_run_queues(); */

	ret = jwait_io(jh, WRITE);

	if (ret)
		return ret;

	sbinfo->last_committed_tx = *jnode_get_block(head);

	return 0;
}

/* This function is called after write-back is finished. We update journal
   footer block and free blocks which were occupied by wandered blocks and
   transaction wander records */
static int update_journal_footer(struct commit_handle *ch, int use_barrier)
{
	reiser4_super_info_data *sbinfo = get_super_private(ch->super);

	jnode *jf = sbinfo->journal_footer;

	int ret;

	format_journal_footer(ch);

	ret = write_jnodes_to_disk_extent(jf, 1, jnode_get_block(jf), NULL,
					  use_barrier ? WRITEOUT_BARRIER : 0);
	if (ret)
		return ret;

	/* blk_run_address_space(sbinfo->fake->i_mapping);
	 * blk_run_queue(); */

	ret = jwait_io(jf, WRITE);
	if (ret)
		return ret;

	return 0;
}

/* free block numbers of wander records of already written in place transaction */
static void dealloc_tx_list(struct commit_handle *ch)
{
	while (!list_empty(&ch->tx_list)) {
		jnode *cur = list_entry(ch->tx_list.next, jnode, capture_link);
		list_del(&cur->capture_link);
		ON_DEBUG(INIT_LIST_HEAD(&cur->capture_link));
		reiser4_dealloc_block(jnode_get_block(cur), BLOCK_NOT_COUNTED,
				      BA_FORMATTED);

		unpin_jnode_data(cur);
		reiser4_drop_io_head(cur);
	}
}

/* An actor for use in block_nr_iterator() routine which frees wandered blocks
   from atom's overwrite set. */
static int
dealloc_wmap_actor(txn_atom * atom UNUSED_ARG,
		   const reiser4_block_nr * a UNUSED_ARG,
		   const reiser4_block_nr * b, void *data UNUSED_ARG)
{

	assert("zam-499", b != NULL);
	assert("zam-500", *b != 0);
	assert("zam-501", !reiser4_blocknr_is_fake(b));

	reiser4_dealloc_block(b, BLOCK_NOT_COUNTED, BA_FORMATTED);
	return 0;
}

/* free wandered block locations of already written in place transaction */
static void dealloc_wmap(struct commit_handle *ch)
{
	assert("zam-696", ch->atom != NULL);

	blocknr_set_iterator(ch->atom, &ch->atom->wandered_map,
			     dealloc_wmap_actor, NULL, 1);
}

/* helper function for alloc wandered blocks, which refill set of block
   numbers needed for wandered blocks  */
static int
get_more_wandered_blocks(int count, reiser4_block_nr * start, int *len)
{
	reiser4_blocknr_hint hint;
	int ret;

	reiser4_block_nr wide_len = count;

	/* FIXME-ZAM: A special policy needed for allocation of wandered blocks
	   ZAM-FIXME-HANS: yes, what happened to our discussion of using a fixed
	   reserved allocation area so as to get the best qualities of fixed
	   journals? */
	reiser4_blocknr_hint_init(&hint);
	hint.block_stage = BLOCK_GRABBED;

	ret = reiser4_alloc_blocks(&hint, start, &wide_len,
				   BA_FORMATTED | BA_USE_DEFAULT_SEARCH_START);
	*len = (int)wide_len;

	return ret;
}

/*
 * roll back changes made before issuing BIO in the case of IO error.
 */
static void undo_bio(struct bio *bio)
{
	int i;

	for (i = 0; i < bio->bi_vcnt; ++i) {
		struct page *pg;
		jnode *node;

		pg = bio->bi_io_vec[i].bv_page;
		end_page_writeback(pg);
		node = jprivate(pg);
		spin_lock_jnode(node);
		JF_CLR(node, JNODE_WRITEBACK);
		JF_SET(node, JNODE_DIRTY);
		spin_unlock_jnode(node);
	}
	bio_put(bio);
}

/* put overwrite set back to atom's clean list */
static void put_overwrite_set(struct commit_handle *ch)
{
	jnode *cur;

	list_for_each_entry(cur, ch->overwrite_set, capture_link)
		jrelse_tail(cur);
}

/* Count overwrite set size, grab disk space for wandered blocks allocation.
   Since we have a separate list for atom's overwrite set we just scan the list,
   count bitmap and other not leaf nodes which wandered blocks allocation we
   have to grab space for. */
static int get_overwrite_set(struct commit_handle *ch)
{
	int ret;
	jnode *cur;
	__u64 nr_not_leaves = 0;
#if REISER4_DEBUG
	__u64 nr_formatted_leaves = 0;
	__u64 nr_unformatted_leaves = 0;
#endif

	assert("zam-697", ch->overwrite_set_size == 0);

	ch->overwrite_set = ATOM_OVRWR_LIST(ch->atom);
	cur = list_entry(ch->overwrite_set->next, jnode, capture_link);

	while (ch->overwrite_set != &cur->capture_link) {
		jnode *next = list_entry(cur->capture_link.next, jnode, capture_link);

		/* Count bitmap locks for getting correct statistics what number
		 * of blocks were cleared by the transaction commit. */
		if (jnode_get_type(cur) == JNODE_BITMAP)
			ch->nr_bitmap++;

		assert("zam-939", JF_ISSET(cur, JNODE_OVRWR)
		       || jnode_get_type(cur) == JNODE_BITMAP);

		if (jnode_is_znode(cur) && znode_above_root(JZNODE(cur))) {
			/* we replace fake znode by another (real)
			   znode which is suggested by disk_layout
			   plugin */

			/* FIXME: it looks like fake znode should be
			   replaced by jnode supplied by
			   disk_layout. */

			struct super_block *s = reiser4_get_current_sb();
			reiser4_super_info_data *sbinfo =
			    get_current_super_private();

			if (sbinfo->df_plug->log_super) {
				jnode *sj = sbinfo->df_plug->log_super(s);

				assert("zam-593", sj != NULL);

				if (IS_ERR(sj))
					return PTR_ERR(sj);

				spin_lock_jnode(sj);
				JF_SET(sj, JNODE_OVRWR);
				insert_into_atom_ovrwr_list(ch->atom, sj);
				spin_unlock_jnode(sj);

				/* jload it as the rest of overwrite set */
				jload_gfp(sj, reiser4_ctx_gfp_mask_get(), 0);

				ch->overwrite_set_size++;
			}
			spin_lock_jnode(cur);
			reiser4_uncapture_block(cur);
			jput(cur);

		} else {
			int ret;
			ch->overwrite_set_size++;
			ret = jload_gfp(cur, reiser4_ctx_gfp_mask_get(), 0);
			if (ret)
				reiser4_panic("zam-783",
					      "cannot load e-flushed jnode back (ret = %d)\n",
					      ret);
		}

		/* Count not leaves here because we have to grab disk space
		 * for wandered blocks. They were not counted as "flush
		 * reserved". Counting should be done _after_ nodes are pinned
		 * into memory by jload(). */
		if (!jnode_is_leaf(cur))
			nr_not_leaves++;
		else {
#if REISER4_DEBUG
			/* at this point @cur either has JNODE_FLUSH_RESERVED
			 * or is eflushed. Locking is not strong enough to
			 * write an assertion checking for this. */
			if (jnode_is_znode(cur))
				nr_formatted_leaves++;
			else
				nr_unformatted_leaves++;
#endif
			JF_CLR(cur, JNODE_FLUSH_RESERVED);
		}

		cur = next;
	}

	/* Grab space for writing (wandered blocks) of not leaves found in
	 * overwrite set. */
	ret = reiser4_grab_space_force(nr_not_leaves, BA_RESERVED);
	if (ret)
		return ret;

	/* Disk space for allocation of wandered blocks of leaf nodes already
	 * reserved as "flush reserved", move it to grabbed space counter. */
	spin_lock_atom(ch->atom);
	assert("zam-940",
	       nr_formatted_leaves + nr_unformatted_leaves <=
	       ch->atom->flush_reserved);
	flush_reserved2grabbed(ch->atom, ch->atom->flush_reserved);
	spin_unlock_atom(ch->atom);

	return ch->overwrite_set_size;
}

/**
 * write_jnodes_to_disk_extent - submit write request
 * @head:
 * @first: first jnode of the list
 * @nr: number of jnodes on the list
 * @block_p:
 * @fq:
 * @flags: used to decide whether page is to get PG_reclaim flag
 *
 * Submits a write request for @nr jnodes beginning from the @first, other
 * jnodes are after the @first on the double-linked "capture" list.  All jnodes
 * will be written to the disk region of @nr blocks starting with @block_p block
 * number.  If @fq is not NULL it means that waiting for i/o completion will be
 * done more efficiently by using flush_queue_t objects.
 * This function is the one which writes list of jnodes in batch mode. It does
 * all low-level things as bio construction and page states manipulation.
 *
 * ZAM-FIXME-HANS: brief me on why this function exists, and why bios are
 * aggregated in this function instead of being left to the layers below
 *
 * FIXME: ZAM->HANS: What layer are you talking about? Can you point me to that?
 * Why that layer needed? Why BIOs cannot be constructed here?
 */
static int write_jnodes_to_disk_extent(
	jnode *first, int nr, const reiser4_block_nr *block_p,
	flush_queue_t *fq, int flags)
{
	struct super_block *super = reiser4_get_current_sb();
	int write_op = ( flags & WRITEOUT_BARRIER ) ? WRITE_FLUSH_FUA : WRITE;
	int max_blocks;
	jnode *cur = first;
	reiser4_block_nr block;

	assert("zam-571", first != NULL);
	assert("zam-572", block_p != NULL);
	assert("zam-570", nr > 0);

	block = *block_p;
	max_blocks = min(bio_get_nr_vecs(super->s_bdev), BIO_MAX_PAGES);

	while (nr > 0) {
		struct bio *bio;
		int nr_blocks = min(nr, max_blocks);
		int i;
		int nr_used;

		bio = bio_alloc(GFP_NOIO, nr_blocks);
		if (!bio)
			return RETERR(-ENOMEM);

		bio->bi_bdev = super->s_bdev;
		bio->bi_sector = block * (super->s_blocksize >> 9);
		for (nr_used = 0, i = 0; i < nr_blocks; i++) {
			struct page *pg;

			pg = jnode_page(cur);
			assert("zam-573", pg != NULL);

			page_cache_get(pg);

			lock_and_wait_page_writeback(pg);

			if (!bio_add_page(bio, pg, super->s_blocksize, 0)) {
				/*
				 * underlying device is satiated. Stop adding
				 * pages to the bio.
				 */
				unlock_page(pg);
				page_cache_release(pg);
				break;
			}

			spin_lock_jnode(cur);
			assert("nikita-3166",
			       pg->mapping == jnode_get_mapping(cur));
			assert("zam-912", !JF_ISSET(cur, JNODE_WRITEBACK));
#if REISER4_DEBUG
			spin_lock(&cur->load);
			assert("nikita-3165", !jnode_is_releasable(cur));
			spin_unlock(&cur->load);
#endif
			JF_SET(cur, JNODE_WRITEBACK);
			JF_CLR(cur, JNODE_DIRTY);
			ON_DEBUG(cur->written++);
			spin_unlock_jnode(cur);

			ClearPageError(pg);
			set_page_writeback(pg);

			if (get_current_context()->entd) {
				/* this is ent thread */
				entd_context *ent = get_entd_context(super);
				struct wbq *rq, *next;

				spin_lock(&ent->guard);

				if (pg == ent->cur_request->page) {
					/*
					 * entd is called for this page. This
					 * request is not in th etodo list
					 */
					ent->cur_request->written = 1;
				} else {
					/*
					 * if we have written a page for which writepage
					 * is called for - move request to another list.
					 */
					list_for_each_entry_safe(rq, next, &ent->todo_list, link) {
						assert("", rq->magic == WBQ_MAGIC);
						if (pg == rq->page) {
							/*
							 * remove request from
							 * entd's queue, but do
							 * not wake up a thread
							 * which put this
							 * request
							 */
							list_del_init(&rq->link);
							ent->nr_todo_reqs --;
							list_add_tail(&rq->link, &ent->done_list);
							ent->nr_done_reqs ++;
							rq->written = 1;
							break;
						}
					}
				}
				spin_unlock(&ent->guard);
			}

			clear_page_dirty_for_io(pg);

			unlock_page(pg);

			cur = list_entry(cur->capture_link.next, jnode, capture_link);
			nr_used++;
		}
		if (nr_used > 0) {
			assert("nikita-3453",
			       bio->bi_size == super->s_blocksize * nr_used);
			assert("nikita-3454", bio->bi_vcnt == nr_used);

			/* Check if we are allowed to write at all */
			if (super->s_flags & MS_RDONLY)
				undo_bio(bio);
			else {
				int not_supported;

				add_fq_to_bio(fq, bio);
				bio_get(bio);
				reiser4_submit_bio(write_op, bio);
				not_supported = bio_flagged(bio, BIO_EOPNOTSUPP);
				bio_put(bio);
				if (not_supported)
					return -EOPNOTSUPP;
			}

			block += nr_used - 1;
			update_blocknr_hint_default(super, &block);
			block += 1;
		} else {
			bio_put(bio);
		}
		nr -= nr_used;
	}

	return 0;
}

/* This is a procedure which recovers a contiguous sequences of disk block
   numbers in the given list of j-nodes and submits write requests on this
   per-sequence basis */
int
write_jnode_list(struct list_head *head, flush_queue_t *fq,
		 long *nr_submitted, int flags)
{
	int ret;
	jnode *beg = list_entry(head->next, jnode, capture_link);

	while (head != &beg->capture_link) {
		int nr = 1;
		jnode *cur = list_entry(beg->capture_link.next, jnode, capture_link);

		while (head != &cur->capture_link) {
			if (*jnode_get_block(cur) != *jnode_get_block(beg) + nr)
				break;
			++nr;
			cur = list_entry(cur->capture_link.next, jnode, capture_link);
		}

		ret = write_jnodes_to_disk_extent(
			beg, nr, jnode_get_block(beg), fq, flags);
		if (ret)
			return ret;

		if (nr_submitted)
			*nr_submitted += nr;

		beg = cur;
	}

	return 0;
}

/* add given wandered mapping to atom's wandered map */
static int
add_region_to_wmap(jnode * cur, int len, const reiser4_block_nr * block_p)
{
	int ret;
	blocknr_set_entry *new_bsep = NULL;
	reiser4_block_nr block;

	txn_atom *atom;

	assert("zam-568", block_p != NULL);
	block = *block_p;
	assert("zam-569", len > 0);

	while ((len--) > 0) {
		do {
			atom = get_current_atom_locked();
			assert("zam-536",
			       !reiser4_blocknr_is_fake(jnode_get_block(cur)));
			ret =
			    blocknr_set_add_pair(atom, &atom->wandered_map,
						 &new_bsep,
						 jnode_get_block(cur), &block);
		} while (ret == -E_REPEAT);

		if (ret) {
			/* deallocate blocks which were not added to wandered
			   map */
			reiser4_block_nr wide_len = len;

			reiser4_dealloc_blocks(&block, &wide_len,
					       BLOCK_NOT_COUNTED,
					       BA_FORMATTED
					       /* formatted, without defer */ );

			return ret;
		}

		spin_unlock_atom(atom);

		cur = list_entry(cur->capture_link.next, jnode, capture_link);
		++block;
	}

	return 0;
}

/* Allocate wandered blocks for current atom's OVERWRITE SET and immediately
   submit IO for allocated blocks.  We assume that current atom is in a stage
   when any atom fusion is impossible and atom is unlocked and it is safe. */
static int alloc_wandered_blocks(struct commit_handle *ch, flush_queue_t *fq)
{
	reiser4_block_nr block;

	int rest;
	int len;
	int ret;

	jnode *cur;

	assert("zam-534", ch->overwrite_set_size > 0);

	rest = ch->overwrite_set_size;

	cur = list_entry(ch->overwrite_set->next, jnode, capture_link);
	while (ch->overwrite_set != &cur->capture_link) {
		assert("zam-567", JF_ISSET(cur, JNODE_OVRWR));

		ret = get_more_wandered_blocks(rest, &block, &len);
		if (ret)
			return ret;

		rest -= len;

		ret = add_region_to_wmap(cur, len, &block);
		if (ret)
			return ret;

		ret = write_jnodes_to_disk_extent(cur, len, &block, fq, 0);
		if (ret)
			return ret;

		while ((len--) > 0) {
			assert("zam-604",
			       ch->overwrite_set != &cur->capture_link);
			cur = list_entry(cur->capture_link.next, jnode, capture_link);
		}
	}

	return 0;
}

/* allocate given number of nodes over the journal area and link them into a
   list, return pointer to the first jnode in the list */
static int alloc_tx(struct commit_handle *ch, flush_queue_t * fq)
{
	reiser4_blocknr_hint hint;
	reiser4_block_nr allocated = 0;
	reiser4_block_nr first, len;
	jnode *cur;
	jnode *txhead;
	int ret;
	reiser4_context *ctx;
	reiser4_super_info_data *sbinfo;

	assert("zam-698", ch->tx_size > 0);
	assert("zam-699", list_empty_careful(&ch->tx_list));

	ctx = get_current_context();
	sbinfo = get_super_private(ctx->super);

	while (allocated < (unsigned)ch->tx_size) {
		len = (ch->tx_size - allocated);

		reiser4_blocknr_hint_init(&hint);

		hint.block_stage = BLOCK_GRABBED;

		/* FIXME: there should be some block allocation policy for
		   nodes which contain wander records */

		/* We assume that disk space for wandered record blocks can be
		 * taken from reserved area. */
		ret = reiser4_alloc_blocks(&hint, &first, &len,
					   BA_FORMATTED | BA_RESERVED |
					   BA_USE_DEFAULT_SEARCH_START);
		reiser4_blocknr_hint_done(&hint);

		if (ret)
			return ret;

		allocated += len;

		/* create jnodes for all wander records */
		while (len--) {
			cur = reiser4_alloc_io_head(&first);

			if (cur == NULL) {
				ret = RETERR(-ENOMEM);
				goto free_not_assigned;
			}

			ret = jinit_new(cur, reiser4_ctx_gfp_mask_get());

			if (ret != 0) {
				jfree(cur);
				goto free_not_assigned;
			}

			pin_jnode_data(cur);

			list_add_tail(&cur->capture_link, &ch->tx_list);

			first++;
		}
	}

	{ /* format a on-disk linked list of wander records */
		int serial = 1;

		txhead = list_entry(ch->tx_list.next, jnode, capture_link);
		format_tx_head(ch);

		cur = list_entry(txhead->capture_link.next, jnode, capture_link);
		while (&ch->tx_list != &cur->capture_link) {
			format_wander_record(ch, cur, serial++);
			cur = list_entry(cur->capture_link.next, jnode, capture_link);
		}
	}

	{ /* Fill wander records with Wandered Set */
		struct store_wmap_params params;
		txn_atom *atom;

		params.cur = list_entry(txhead->capture_link.next, jnode, capture_link);

		params.idx = 0;
		params.capacity =
		    wander_record_capacity(reiser4_get_current_sb());

		atom = get_current_atom_locked();
		blocknr_set_iterator(atom, &atom->wandered_map,
				     &store_wmap_actor, &params, 0);
		spin_unlock_atom(atom);
	}

	{ /* relse all jnodes from tx_list */
		cur = list_entry(ch->tx_list.next, jnode, capture_link);
		while (&ch->tx_list != &cur->capture_link) {
			jrelse(cur);
			cur = list_entry(cur->capture_link.next, jnode, capture_link);
		}
	}

	ret = write_jnode_list(&ch->tx_list, fq, NULL, 0);

	return ret;

      free_not_assigned:
	/* We deallocate blocks not yet assigned to jnodes on tx_list. The
	   caller takes care about invalidating of tx list  */
	reiser4_dealloc_blocks(&first, &len, BLOCK_NOT_COUNTED, BA_FORMATTED);

	return ret;
}

static int commit_tx(struct commit_handle *ch)
{
	flush_queue_t *fq;
	int barrier;
	int ret;

	/* Grab more space for wandered records. */
	ret = reiser4_grab_space_force((__u64) (ch->tx_size), BA_RESERVED);
	if (ret)
		return ret;

	fq = get_fq_for_current_atom();
	if (IS_ERR(fq))
		return PTR_ERR(fq);

	spin_unlock_atom(fq->atom);
	do {
		ret = alloc_wandered_blocks(ch, fq);
		if (ret)
			break;
		ret = alloc_tx(ch, fq);
		if (ret)
			break;
	} while (0);

	reiser4_fq_put(fq);
	if (ret)
		return ret;
 	barrier = reiser4_use_write_barrier(ch->super);
	if (!barrier) {
		ret = current_atom_finish_all_fq();
		if (ret)
			return ret;
	}
	ret = update_journal_header(ch, barrier);
	if (!barrier || ret)
		return ret;
	return current_atom_finish_all_fq();
}

static int write_tx_back(struct commit_handle * ch)
{
	flush_queue_t *fq;
	int ret;
	int barrier;

	reiser4_post_commit_hook();
	fq = get_fq_for_current_atom();
	if (IS_ERR(fq))
		return  PTR_ERR(fq);
	spin_unlock_atom(fq->atom);
	ret = write_jnode_list(
		ch->overwrite_set, fq, NULL, WRITEOUT_FOR_PAGE_RECLAIM);
	reiser4_fq_put(fq);
	if (ret)
		return ret;

	barrier = reiser4_use_write_barrier(ch->super);
	if (!barrier) {
		ret = current_atom_finish_all_fq();
		if (ret)
			return ret;
	}
	ret = update_journal_footer(ch, barrier);
	if (ret)
		return ret;
	if (barrier) {
		ret = current_atom_finish_all_fq();
		if (ret)
			return ret;
	}
	reiser4_post_write_back_hook();
	return 0;
}

/* We assume that at this moment all captured blocks are marked as RELOC or
   WANDER (belong to Relocate o Overwrite set), all nodes from Relocate set
   are submitted to write.
*/

int reiser4_write_logs(long *nr_submitted)
{
	txn_atom *atom;
	struct super_block *super = reiser4_get_current_sb();
	reiser4_super_info_data *sbinfo = get_super_private(super);
	struct commit_handle ch;
	int ret;

	writeout_mode_enable();

	/* block allocator may add j-nodes to the clean_list */
	ret = reiser4_pre_commit_hook();
	if (ret)
		return ret;

	/* No locks are required if we take atom which stage >=
	 * ASTAGE_PRE_COMMIT */
	atom = get_current_context()->trans->atom;
	assert("zam-965", atom != NULL);

	/* relocate set is on the atom->clean_nodes list after
	 * current_atom_complete_writes() finishes. It can be safely
	 * uncaptured after commit_mutex is locked, because any atom that
	 * captures these nodes is guaranteed to commit after current one.
	 *
	 * This can only be done after reiser4_pre_commit_hook(), because it is where
	 * early flushed jnodes with CREATED bit are transferred to the
	 * overwrite list. */
	reiser4_invalidate_list(ATOM_CLEAN_LIST(atom));
	spin_lock_atom(atom);
	/* There might be waiters for the relocate nodes which we have
	 * released, wake them up. */
	reiser4_atom_send_event(atom);
	spin_unlock_atom(atom);

	if (REISER4_DEBUG) {
		int level;

		for (level = 0; level < REAL_MAX_ZTREE_HEIGHT + 1; ++level)
			assert("nikita-3352",
			       list_empty_careful(ATOM_DIRTY_LIST(atom, level)));
	}

	sbinfo->nr_files_committed += (unsigned)atom->nr_objects_created;
	sbinfo->nr_files_committed -= (unsigned)atom->nr_objects_deleted;

	init_commit_handle(&ch, atom);

	ch.free_blocks = sbinfo->blocks_free_committed;
	ch.nr_files = sbinfo->nr_files_committed;
	/* ZAM-FIXME-HANS: email me what the contention level is for the super
	 * lock. */
	ch.next_oid = oid_next(super);

	/* count overwrite set and place it in a separate list */
	ret = get_overwrite_set(&ch);

	if (ret <= 0) {
		/* It is possible that overwrite set is empty here, it means
		   all captured nodes are clean */
		goto up_and_ret;
	}

	/* Inform the caller about what number of dirty pages will be
	 * submitted to disk. */
	*nr_submitted += ch.overwrite_set_size - ch.nr_bitmap;

	/* count all records needed for storing of the wandered set */
	get_tx_size(&ch);

	ret = commit_tx(&ch);
	if (ret)
		goto up_and_ret;

	spin_lock_atom(atom);
	reiser4_atom_set_stage(atom, ASTAGE_POST_COMMIT);
	spin_unlock_atom(atom);

	ret = write_tx_back(&ch);
	reiser4_post_write_back_hook();

      up_and_ret:
	if (ret) {
		/* there could be fq attached to current atom; the only way to
		   remove them is: */
		current_atom_finish_all_fq();
	}

	/* free blocks of flushed transaction */
	dealloc_tx_list(&ch);
	dealloc_wmap(&ch);

	put_overwrite_set(&ch);

	done_commit_handle(&ch);

	writeout_mode_disable();

	return ret;
}

/* consistency checks for journal data/control blocks: header, footer, log
   records, transactions head blocks. All functions return zero on success. */

static int check_journal_header(const jnode * node UNUSED_ARG)
{
	/* FIXME: journal header has no magic field yet. */
	return 0;
}

/* wait for write completion for all jnodes from given list */
static int wait_on_jnode_list(struct list_head *head)
{
	jnode *scan;
	int ret = 0;

	list_for_each_entry(scan, head, capture_link) {
		struct page *pg = jnode_page(scan);

		if (pg) {
			if (PageWriteback(pg))
				wait_on_page_writeback(pg);

			if (PageError(pg))
				ret++;
		}
	}

	return ret;
}

static int check_journal_footer(const jnode * node UNUSED_ARG)
{
	/* FIXME: journal footer has no magic field yet. */
	return 0;
}

static int check_tx_head(const jnode * node)
{
	struct tx_header *header = (struct tx_header *)jdata(node);

	if (memcmp(&header->magic, TX_HEADER_MAGIC, TX_HEADER_MAGIC_SIZE) != 0) {
		warning("zam-627", "tx head at block %s corrupted\n",
			sprint_address(jnode_get_block(node)));
		return RETERR(-EIO);
	}

	return 0;
}

static int check_wander_record(const jnode * node)
{
	struct wander_record_header *RH =
	    (struct wander_record_header *)jdata(node);

	if (memcmp(&RH->magic, WANDER_RECORD_MAGIC, WANDER_RECORD_MAGIC_SIZE) !=
	    0) {
		warning("zam-628", "wander record at block %s corrupted\n",
			sprint_address(jnode_get_block(node)));
		return RETERR(-EIO);
	}

	return 0;
}

/* fill commit_handler structure by everything what is needed for update_journal_footer */
static int restore_commit_handle(struct commit_handle *ch, jnode *tx_head)
{
	struct tx_header *TXH;
	int ret;

	ret = jload(tx_head);
	if (ret)
		return ret;

	TXH = (struct tx_header *)jdata(tx_head);

	ch->free_blocks = le64_to_cpu(get_unaligned(&TXH->free_blocks));
	ch->nr_files = le64_to_cpu(get_unaligned(&TXH->nr_files));
	ch->next_oid = le64_to_cpu(get_unaligned(&TXH->next_oid));

	jrelse(tx_head);

	list_add(&tx_head->capture_link, &ch->tx_list);

	return 0;
}

/* replay one transaction: restore and write overwrite set in place */
static int replay_transaction(const struct super_block *s,
			      jnode * tx_head,
			      const reiser4_block_nr * log_rec_block_p,
			      const reiser4_block_nr * end_block,
			      unsigned int nr_wander_records)
{
	reiser4_block_nr log_rec_block = *log_rec_block_p;
	struct commit_handle ch;
	LIST_HEAD(overwrite_set);
	jnode *log;
	int ret;

	init_commit_handle(&ch, NULL);
	ch.overwrite_set = &overwrite_set;

	restore_commit_handle(&ch, tx_head);

	while (log_rec_block != *end_block) {
		struct wander_record_header *header;
		struct wander_entry *entry;

		int i;

		if (nr_wander_records == 0) {
			warning("zam-631",
				"number of wander records in the linked list"
				" greater than number stored in tx head.\n");
			ret = RETERR(-EIO);
			goto free_ow_set;
		}

		log = reiser4_alloc_io_head(&log_rec_block);
		if (log == NULL)
			return RETERR(-ENOMEM);

		ret = jload(log);
		if (ret < 0) {
			reiser4_drop_io_head(log);
			return ret;
		}

		ret = check_wander_record(log);
		if (ret) {
			jrelse(log);
			reiser4_drop_io_head(log);
			return ret;
		}

		header = (struct wander_record_header *)jdata(log);
		log_rec_block = le64_to_cpu(get_unaligned(&header->next_block));

		entry = (struct wander_entry *)(header + 1);

		/* restore overwrite set from wander record content */
		for (i = 0; i < wander_record_capacity(s); i++) {
			reiser4_block_nr block;
			jnode *node;

			block = le64_to_cpu(get_unaligned(&entry->wandered));
			if (block == 0)
				break;

			node = reiser4_alloc_io_head(&block);
			if (node == NULL) {
				ret = RETERR(-ENOMEM);
				/*
				 * FIXME-VS:???
				 */
				jrelse(log);
				reiser4_drop_io_head(log);
				goto free_ow_set;
			}

			ret = jload(node);

			if (ret < 0) {
				reiser4_drop_io_head(node);
				/*
				 * FIXME-VS:???
				 */
				jrelse(log);
				reiser4_drop_io_head(log);
				goto free_ow_set;
			}

			block = le64_to_cpu(get_unaligned(&entry->original));

			assert("zam-603", block != 0);

			jnode_set_block(node, &block);

			list_add_tail(&node->capture_link, ch.overwrite_set);

			++entry;
		}

		jrelse(log);
		reiser4_drop_io_head(log);

		--nr_wander_records;
	}

	if (nr_wander_records != 0) {
		warning("zam-632", "number of wander records in the linked list"
			" less than number stored in tx head.\n");
		ret = RETERR(-EIO);
		goto free_ow_set;
	}

	{			/* write wandered set in place */
		write_jnode_list(ch.overwrite_set, NULL, NULL, 0);
		ret = wait_on_jnode_list(ch.overwrite_set);

		if (ret) {
			ret = RETERR(-EIO);
			goto free_ow_set;
		}
	}

	ret = update_journal_footer(&ch, 0);

      free_ow_set:

	while (!list_empty(ch.overwrite_set)) {
		jnode *cur = list_entry(ch.overwrite_set->next, jnode, capture_link);
		list_del_init(&cur->capture_link);
		jrelse(cur);
		reiser4_drop_io_head(cur);
	}

	list_del_init(&tx_head->capture_link);

	done_commit_handle(&ch);

	return ret;
}

/* find oldest committed and not played transaction and play it. The transaction
 * was committed and journal header block was updated but the blocks from the
 * process of writing the atom's overwrite set in-place and updating of journal
 * footer block were not completed. This function completes the process by
 * recovering the atom's overwrite set from their wandered locations and writes
 * them in-place and updating the journal footer. */
static int replay_oldest_transaction(struct super_block *s)
{
	reiser4_super_info_data *sbinfo = get_super_private(s);
	jnode *jf = sbinfo->journal_footer;
	unsigned int total;
	struct journal_footer *F;
	struct tx_header *T;

	reiser4_block_nr prev_tx;
	reiser4_block_nr last_flushed_tx;
	reiser4_block_nr log_rec_block = 0;

	jnode *tx_head;

	int ret;

	if ((ret = jload(jf)) < 0)
		return ret;

	F = (struct journal_footer *)jdata(jf);

	last_flushed_tx = le64_to_cpu(get_unaligned(&F->last_flushed_tx));

	jrelse(jf);

	if (sbinfo->last_committed_tx == last_flushed_tx) {
		/* all transactions are replayed */
		return 0;
	}

	prev_tx = sbinfo->last_committed_tx;

	/* searching for oldest not flushed transaction */
	while (1) {
		tx_head = reiser4_alloc_io_head(&prev_tx);
		if (!tx_head)
			return RETERR(-ENOMEM);

		ret = jload(tx_head);
		if (ret < 0) {
			reiser4_drop_io_head(tx_head);
			return ret;
		}

		ret = check_tx_head(tx_head);
		if (ret) {
			jrelse(tx_head);
			reiser4_drop_io_head(tx_head);
			return ret;
		}

		T = (struct tx_header *)jdata(tx_head);

		prev_tx = le64_to_cpu(get_unaligned(&T->prev_tx));

		if (prev_tx == last_flushed_tx)
			break;

		jrelse(tx_head);
		reiser4_drop_io_head(tx_head);
	}

	total = le32_to_cpu(get_unaligned(&T->total));
	log_rec_block = le64_to_cpu(get_unaligned(&T->next_block));

	pin_jnode_data(tx_head);
	jrelse(tx_head);

	ret =
	    replay_transaction(s, tx_head, &log_rec_block,
			       jnode_get_block(tx_head), total - 1);

	unpin_jnode_data(tx_head);
	reiser4_drop_io_head(tx_head);

	if (ret)
		return ret;
	return -E_REPEAT;
}

/* The reiser4 journal current implementation was optimized to not to capture
   super block if certain super blocks fields are modified. Currently, the set
   is (<free block count>, <OID allocator>). These fields are logged by
   special way which includes storing them in each transaction head block at
   atom commit time and writing that information to journal footer block at
   atom flush time.  For getting info from journal footer block to the
   in-memory super block there is a special function
   reiser4_journal_recover_sb_data() which should be called after disk format
   plugin re-reads super block after journal replaying.
*/

/* get the information from journal footer in-memory super block */
int reiser4_journal_recover_sb_data(struct super_block *s)
{
	reiser4_super_info_data *sbinfo = get_super_private(s);
	struct journal_footer *jf;
	int ret;

	assert("zam-673", sbinfo->journal_footer != NULL);

	ret = jload(sbinfo->journal_footer);
	if (ret != 0)
		return ret;

	ret = check_journal_footer(sbinfo->journal_footer);
	if (ret != 0)
		goto out;

	jf = (struct journal_footer *)jdata(sbinfo->journal_footer);

	/* was there at least one flushed transaction?  */
	if (jf->last_flushed_tx) {

		/* restore free block counter logged in this transaction */
		reiser4_set_free_blocks(s, le64_to_cpu(get_unaligned(&jf->free_blocks)));

		/* restore oid allocator state */
		oid_init_allocator(s,
				   le64_to_cpu(get_unaligned(&jf->nr_files)),
				   le64_to_cpu(get_unaligned(&jf->next_oid)));
	}
      out:
	jrelse(sbinfo->journal_footer);
	return ret;
}

/* reiser4 replay journal procedure */
int reiser4_journal_replay(struct super_block *s)
{
	reiser4_super_info_data *sbinfo = get_super_private(s);
	jnode *jh, *jf;
	struct journal_header *header;
	int nr_tx_replayed = 0;
	int ret;

	assert("zam-582", sbinfo != NULL);

	jh = sbinfo->journal_header;
	jf = sbinfo->journal_footer;

	if (!jh || !jf) {
		/* it is possible that disk layout does not support journal
		   structures, we just warn about this */
		warning("zam-583",
			"journal control blocks were not loaded by disk layout plugin.  "
			"journal replaying is not possible.\n");
		return 0;
	}

	/* Take free block count from journal footer block. The free block
	   counter value corresponds the last flushed transaction state */
	ret = jload(jf);
	if (ret < 0)
		return ret;

	ret = check_journal_footer(jf);
	if (ret) {
		jrelse(jf);
		return ret;
	}

	jrelse(jf);

	/* store last committed transaction info in reiser4 in-memory super
	   block */
	ret = jload(jh);
	if (ret < 0)
		return ret;

	ret = check_journal_header(jh);
	if (ret) {
		jrelse(jh);
		return ret;
	}

	header = (struct journal_header *)jdata(jh);
	sbinfo->last_committed_tx = le64_to_cpu(get_unaligned(&header->last_committed_tx));

	jrelse(jh);

	/* replay committed transactions */
	while ((ret = replay_oldest_transaction(s)) == -E_REPEAT)
		nr_tx_replayed++;

	return ret;
}

/* load journal control block (either journal header or journal footer block) */
static int
load_journal_control_block(jnode ** node, const reiser4_block_nr * block)
{
	int ret;

	*node = reiser4_alloc_io_head(block);
	if (!(*node))
		return RETERR(-ENOMEM);

	ret = jload(*node);

	if (ret) {
		reiser4_drop_io_head(*node);
		*node = NULL;
		return ret;
	}

	pin_jnode_data(*node);
	jrelse(*node);

	return 0;
}

/* unload journal header or footer and free jnode */
static void unload_journal_control_block(jnode ** node)
{
	if (*node) {
		unpin_jnode_data(*node);
		reiser4_drop_io_head(*node);
		*node = NULL;
	}
}

/* release journal control blocks */
void reiser4_done_journal_info(struct super_block *s)
{
	reiser4_super_info_data *sbinfo = get_super_private(s);

	assert("zam-476", sbinfo != NULL);

	unload_journal_control_block(&sbinfo->journal_header);
	unload_journal_control_block(&sbinfo->journal_footer);
	rcu_barrier();
}

/* load journal control blocks */
int reiser4_init_journal_info(struct super_block *s)
{
	reiser4_super_info_data *sbinfo = get_super_private(s);
	journal_location *loc;
	int ret;

	loc = &sbinfo->jloc;

	assert("zam-651", loc != NULL);
	assert("zam-652", loc->header != 0);
	assert("zam-653", loc->footer != 0);

	ret = load_journal_control_block(&sbinfo->journal_header, &loc->header);

	if (ret)
		return ret;

	ret = load_journal_control_block(&sbinfo->journal_footer, &loc->footer);

	if (ret) {
		unload_journal_control_block(&sbinfo->journal_header);
	}

	return ret;
}

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 80
   End:
*/
