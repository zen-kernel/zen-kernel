#include "../forward.h"
#include "../debug.h"
#include "../coord.h"
#include "../plugin/plugin.h"
#include "../jnode.h"
#include "../znode.h"
#include "../block_alloc.h"
#include "../reiser4.h"
#include "../flush.h"

/*
 * This file contains implementation of different transaction models.
 *
 * Transaction model is a high-level block allocator, which assigns block
 * numbers to dirty nodes, and, thereby, decides, how those nodes will be
 * committed.
 *
 * Every dirty node of reiser4 atom can be committed by either of the
 * following two ways:
 * 1) via journal;
 * 2) using "write-anywhere" technique.
 *
 * If the allocator doesn't change on-disk location of a node, then
 * this node will be committed using journalling technique (overwrite).
 * Otherwise, it will be comitted via write-anywhere technique (relocate):
 *
 *            relocate  <----  allocate  --- >  overwrite
 *
 * So, in our interpretation the 2 traditional "classic" strategies in
 * committing transactions (journalling and "write-anywhere") are just two
 * boundary cases: 1) when all nodes are overwritten, and 2) when all nodes
 * are relocated.
 *
 * Besides those 2 boundary cases we can implement in reiser4 the infinite
 * set of their various combinations, so that user can choose what is really
 * suitable for his needs.
 */

/* jnode_make_wander_nolock <- find_flush_start_jnode (special case for znode-above-root)
                            <- jnode_make_wander  */
void jnode_make_wander_nolock(jnode * node);

/* jnode_make_wander <- txmod.forward_alloc_formatted */
void jnode_make_wander(jnode * node);

/* jnode_make_reloc_nolock <- znode_make_reloc
                           <- unformatted_make_reloc */
static void jnode_make_reloc_nolock(flush_queue_t * fq, jnode * node);



                  /* Handle formatted nodes in forward context */


/**
 * txmod.forward_alloc_formatted <- allocate_znode <- alloc_pos_and_ancestors <- jnode_flush
 *                                                 <- alloc_one_ancestor <- alloc_pos_and_ancestors <- jnode_flush
 *                                                               <- alloc_one_ancestor (recursive)
 *                                                 <- lock_parent_and_allocate_znode <- squalloc_upper_levels <- check_parents_and_squalloc_upper_levels <- squalloc_upper_levels (recursive)
 *                                                                                                                                                       <- handle_pos_on_formatted
 *                                                                                   <- handle_pos_on_formatted
 *                                                                                   <- handle_pos_end_of_twig
 *                                                 <- handle_pos_to_leaf
 */
void znode_make_reloc(znode * z, flush_queue_t * fq);


                             /* Handle unformatted nodes */


/* unformatted_make_reloc <- assign_real_blocknrs <- txmod.forward_alloc_unformatted
                                                  <- txmod.squeeze_alloc_unformatted
*/
void unformatted_make_reloc(jnode *node, flush_queue_t *fq);

static void forward_overwrite_unformatted(flush_pos_t *flush_pos, oid_t oid,
				  unsigned long index, reiser4_block_nr width);

/* mark_jnode_overwrite <- forward_overwrite_unformatted <- txmod.forward_alloc_unformatted
                           squeeze_overwrite_unformatted <- txmod.squeeze_alloc_unformatted
*/
static void mark_jnode_overwrite(struct list_head *jnodes, jnode *node);

int split_allocated_extent(coord_t *coord, reiser4_block_nr pos_in_unit);
int allocated_extent_slum_size(flush_pos_t *flush_pos, oid_t oid,
			       unsigned long index, unsigned long count);
void allocate_blocks_unformatted(reiser4_blocknr_hint *preceder,
				 reiser4_block_nr wanted_count,
				 reiser4_block_nr *first_allocated,
				 reiser4_block_nr *allocated,
				 block_stage_t block_stage);
void assign_real_blocknrs(flush_pos_t *flush_pos, oid_t oid,
			  unsigned long index, reiser4_block_nr count,
			  reiser4_block_nr first);
int convert_extent(coord_t *coord, reiser4_extent *replace);
int put_unit_to_end(znode *node,
		    const reiser4_key *key, reiser4_extent *copy_ext);

/*
 * txmod.forward_alloc_unformatted <- handle_pos_on_twig
 * txmod.squeeze_alloc_unformatted <- squeeze_right_twig
 */

/* Common functions */

/**
 * Mark node JNODE_OVRWR and put it on atom->overwrite_nodes list.
 * Atom lock and jnode lock should be taken before calling this
 * function.
 */
void jnode_make_wander_nolock(jnode * node)
{
	txn_atom *atom;

	assert("nikita-2431", node != NULL);
	assert("nikita-2432", !JF_ISSET(node, JNODE_RELOC));
	assert("nikita-3153", JF_ISSET(node, JNODE_DIRTY));
	assert("zam-897", !JF_ISSET(node, JNODE_FLUSH_QUEUED));
	assert("nikita-3367", !reiser4_blocknr_is_fake(jnode_get_block(node)));

	atom = node->atom;

	assert("zam-895", atom != NULL);
	assert("zam-894", atom_is_protected(atom));

	JF_SET(node, JNODE_OVRWR);
	/* move node to atom's overwrite list */
	list_move_tail(&node->capture_link, ATOM_OVRWR_LIST(atom));
	ON_DEBUG(count_jnode(atom, node, DIRTY_LIST, OVRWR_LIST, 1));
}

/*
 * Same as jnode_make_wander_nolock, but all necessary locks
 * are taken inside this function.
 */
void jnode_make_wander(jnode * node)
{
	txn_atom *atom;

	spin_lock_jnode(node);
	atom = jnode_get_atom(node);
	assert("zam-913", atom != NULL);
	assert("zam-914", !JF_ISSET(node, JNODE_RELOC));

	jnode_make_wander_nolock(node);
	spin_unlock_atom(atom);
	spin_unlock_jnode(node);
}

/* this just sets RELOC bit  */
static void jnode_make_reloc_nolock(flush_queue_t * fq, jnode * node)
{
	assert_spin_locked(&(node->guard));
	assert("zam-916", JF_ISSET(node, JNODE_DIRTY));
	assert("zam-917", !JF_ISSET(node, JNODE_RELOC));
	assert("zam-918", !JF_ISSET(node, JNODE_OVRWR));
	assert("zam-920", !JF_ISSET(node, JNODE_FLUSH_QUEUED));
	assert("nikita-3367", !reiser4_blocknr_is_fake(jnode_get_block(node)));
	jnode_set_reloc(node);
}

/*
 * Mark znode RELOC and put it on flush queue
 */
void znode_make_reloc(znode * z, flush_queue_t * fq)
{
	jnode *node;
	txn_atom *atom;

	node = ZJNODE(z);
	spin_lock_jnode(node);

	atom = jnode_get_atom(node);
	assert("zam-919", atom != NULL);

	jnode_make_reloc_nolock(fq, node);
	queue_jnode(fq, node);

	spin_unlock_atom(atom);
	spin_unlock_jnode(node);
}

/* Mark unformatted node RELOC and put it on flush queue */
void unformatted_make_reloc(jnode *node, flush_queue_t *fq)
{
	assert("vs-1479", jnode_is_unformatted(node));

	jnode_make_reloc_nolock(fq, node);
	queue_jnode(fq, node);
}

/**
 * mark_jnode_overwrite - assign node to overwrite set
 * @jnodes: overwrite set list head
 * @node: jnode to belong to overwrite set
 *
 * Sets OVRWR jnode state bit and puts @node to the end of list head @jnodes
 * which is an accumulator for nodes before they get to overwrite set list of
 * atom.
 */
static void mark_jnode_overwrite(struct list_head *jnodes, jnode *node)
{
	spin_lock_jnode(node);

	assert("zam-917", !JF_ISSET(node, JNODE_RELOC));
	assert("zam-918", !JF_ISSET(node, JNODE_OVRWR));

	JF_SET(node, JNODE_OVRWR);
	list_move_tail(&node->capture_link, jnodes);
	ON_DEBUG(count_jnode(node->atom, node, DIRTY_LIST, OVRWR_LIST, 0));

	spin_unlock_jnode(node);
}

static int forward_relocate_unformatted(flush_pos_t *flush_pos,
					reiser4_extent *ext,
					extent_state state,
					oid_t oid, __u64 index,
					__u64 width, int *exit)
{
	int result;
	coord_t *coord;
	reiser4_extent replace_ext;
	reiser4_block_nr protected;
	reiser4_block_nr start;
	reiser4_block_nr first_allocated;
	__u64 allocated;
	block_stage_t block_stage;

	*exit = 0;
	coord = &flush_pos->coord;
	start = extent_get_start(ext);

	if (flush_pos->pos_in_unit) {
		/*
		 * split extent unit into two ones
		 */
		result = split_allocated_extent(coord,
						flush_pos->pos_in_unit);
		flush_pos->pos_in_unit = 0;
		*exit = 1;
		return result;
	}
	/*
	 * limit number of nodes to allocate
	 */
	if (flush_pos->nr_to_write < width)
		width = flush_pos->nr_to_write;

	if (state == ALLOCATED_EXTENT) {
		/*
		 * all protected nodes are not flushprepped, therefore
		 * they are counted as flush_reserved
		 */
		block_stage = BLOCK_FLUSH_RESERVED;
		protected = allocated_extent_slum_size(flush_pos, oid,
						       index, width);
		if (protected == 0) {
			flush_pos->state = POS_INVALID;
			flush_pos->pos_in_unit = 0;
			*exit = 1;
			return 0;
		}
	} else {
		block_stage = BLOCK_UNALLOCATED;
		protected = width;
	}
	/*
	 * look at previous unit if possible. If it is allocated, make
	 * preceder more precise
	 */
	if (coord->unit_pos &&
	    (state_of_extent(ext - 1) == ALLOCATED_EXTENT))
		reiser4_pos_hint(flush_pos)->blk =
			extent_get_start(ext - 1) +
			extent_get_width(ext - 1);
	/*
	 * allocate new block numbers for protected nodes
	 */
	allocate_blocks_unformatted(reiser4_pos_hint(flush_pos),
				    protected,
				    &first_allocated, &allocated,
				    block_stage);

	if (state == ALLOCATED_EXTENT)
		/*
		 * on relocating - free nodes which are going to be
		 * relocated
		 */
		reiser4_dealloc_blocks(&start, &allocated, 0, BA_DEFER);

	/* assign new block numbers to protected nodes */
	assign_real_blocknrs(flush_pos, oid, index, allocated, first_allocated);

	/* prepare extent which will replace current one */
	reiser4_set_extent(&replace_ext, first_allocated, allocated);

	/* adjust extent item */
	result = convert_extent(coord, &replace_ext);
	if (result != 0 && result != -ENOMEM) {
		warning("vs-1461",
			"Failed to allocate extent. Should not happen\n");
		*exit = 1;
		return result;
	}
	/*
	 * break flush: we prepared for flushing as many blocks as we
	 * were asked for
	 */
	if (flush_pos->nr_to_write == allocated)
		flush_pos->state = POS_INVALID;
	return 0;
}

static squeeze_result squeeze_relocate_unformatted(znode *left,
						   const coord_t *coord,
						   flush_pos_t *flush_pos,
						   reiser4_key *key,
						   reiser4_key *stop_key)
{
	int result;
	reiser4_extent *ext;
	__u64 index;
	__u64 width;
	reiser4_block_nr start;
	extent_state state;
	oid_t oid;
	reiser4_block_nr first_allocated;
	__u64 allocated;
	__u64 protected;
	reiser4_extent copy_extent;
	block_stage_t block_stage;

	assert("edward-1610", flush_pos->pos_in_unit == 0);
	assert("edward-1611", coord_is_leftmost_unit(coord));
	assert("edward-1612", item_is_extent(coord));

	ext = extent_by_coord(coord);
	index = extent_unit_index(coord);
	start = extent_get_start(ext);
	width = extent_get_width(ext);
	state = state_of_extent(ext);
	unit_key_by_coord(coord, key);
	oid = get_key_objectid(key);

	assert("edward-1613", state != HOLE_EXTENT);

	if (state == ALLOCATED_EXTENT) {
		/*
		 * all protected nodes are not flushprepped,
		 * therefore they are counted as flush_reserved
		 */
		block_stage = BLOCK_FLUSH_RESERVED;
		protected = allocated_extent_slum_size(flush_pos, oid,
						       index, width);
		if (protected == 0) {
			flush_pos->state = POS_INVALID;
			flush_pos->pos_in_unit = 0;
			return 0;
		}
	} else {
		block_stage = BLOCK_UNALLOCATED;
		protected = width;
	}
	/*
	 * look at previous unit if possible. If it is allocated, make
	 * preceder more precise
	 */
	if (coord->unit_pos &&
	    (state_of_extent(ext - 1) == ALLOCATED_EXTENT))
		reiser4_pos_hint(flush_pos)->blk =
			extent_get_start(ext - 1) +
			extent_get_width(ext - 1);
	/*
	 * allocate new block numbers for protected nodes
	 */
	allocate_blocks_unformatted(reiser4_pos_hint(flush_pos),
				    protected,
				    &first_allocated, &allocated,
				    block_stage);
	/*
	 * prepare extent which will be copied to left
	 */
	reiser4_set_extent(&copy_extent, first_allocated, allocated);
	result = put_unit_to_end(left, key, &copy_extent);

	if (result == -E_NODE_FULL) {
		/*
		 * free blocks which were just allocated
		 */
		reiser4_dealloc_blocks(&first_allocated, &allocated,
				       (state == ALLOCATED_EXTENT)
				       ? BLOCK_FLUSH_RESERVED
				       : BLOCK_UNALLOCATED,
				       BA_PERMANENT);
		/*
		 * rewind the preceder
		 */
		flush_pos->preceder.blk = first_allocated;
		check_preceder(flush_pos->preceder.blk);
		return SQUEEZE_TARGET_FULL;
	}
	if (state == ALLOCATED_EXTENT) {
		/*
		 * free nodes which were relocated
		 */
		reiser4_dealloc_blocks(&start, &allocated, 0, BA_DEFER);
	}
	/*
	 * assign new block numbers to protected nodes
	 */
	assign_real_blocknrs(flush_pos, oid, index, allocated,
			     first_allocated);
	set_key_offset(key,
		       get_key_offset(key) +
		       (allocated << current_blocksize_bits));
	return SQUEEZE_CONTINUE;
}

/**
 * forward_overwrite_unformatted - put bunch of jnodes to overwrite set
 * @flush_pos: flush position
 * @oid: objectid of file jnodes belong to
 * @index: starting index
 * @width: extent width
 *
 * Puts nodes of one extent (file objectid @oid, extent width @width) to atom's
 * overwrite set. Starting from the one with index @index. If end of slum is
 * detected (node is not found or flushprepped) - stop iterating and set flush
 * position's state to POS_INVALID.
 */
static void forward_overwrite_unformatted(flush_pos_t *flush_pos, oid_t oid,
					  unsigned long index,
					  reiser4_block_nr width)
{
	unsigned long i;
	reiser4_tree *tree;
	jnode *node;
	txn_atom *atom;
	LIST_HEAD(jnodes);

	tree = current_tree;

	atom = atom_locked_by_fq(reiser4_pos_fq(flush_pos));
	assert("vs-1478", atom);

	for (i = flush_pos->pos_in_unit; i < width; i++, index++) {
		node = jlookup(tree, oid, index);
		if (!node) {
			flush_pos->state = POS_INVALID;
			break;
		}
		if (jnode_check_flushprepped(node)) {
			flush_pos->state = POS_INVALID;
			atomic_dec(&node->x_count);
			break;
		}
		if (node->atom != atom) {
			flush_pos->state = POS_INVALID;
			atomic_dec(&node->x_count);
			break;
		}
		mark_jnode_overwrite(&jnodes, node);
		atomic_dec(&node->x_count);
	}

	list_splice_init(&jnodes, ATOM_OVRWR_LIST(atom)->prev);
	spin_unlock_atom(atom);
}

static squeeze_result squeeze_overwrite_unformatted(znode *left,
						    const coord_t *coord,
						    flush_pos_t *flush_pos,
						    reiser4_key *key,
						    reiser4_key *stop_key)
{
	int result;
	reiser4_extent *ext;
	__u64 index;
	__u64 width;
	reiser4_block_nr start;
	extent_state state;
	oid_t oid;
	reiser4_extent copy_extent;

	assert("vs-1457", flush_pos->pos_in_unit == 0);
	assert("vs-1467", coord_is_leftmost_unit(coord));
	assert("vs-1467", item_is_extent(coord));

	ext = extent_by_coord(coord);
	index = extent_unit_index(coord);
	start = extent_get_start(ext);
	width = extent_get_width(ext);
	state = state_of_extent(ext);
	unit_key_by_coord(coord, key);
	oid = get_key_objectid(key);
	/*
	 * try to copy unit as it is to left neighbor
	 * and make all first not flushprepped nodes
	 * overwrite nodes
	 */
	reiser4_set_extent(&copy_extent, start, width);

	result = put_unit_to_end(left, key, &copy_extent);
	if (result == -E_NODE_FULL)
		return SQUEEZE_TARGET_FULL;

	if (state != HOLE_EXTENT)
		forward_overwrite_unformatted(flush_pos, oid, index, width);

	set_key_offset(key,
		      get_key_offset(key) + (width << current_blocksize_bits));
	return SQUEEZE_CONTINUE;
}

/************************ HYBRID TRANSACTION MODEL ****************************/

/**
 * This is the default transaction model suggested by Josh MacDonald and
 * Hans Reiser. This was the single hardcoded transaction mode till Feb 2014
 * when Edward introduced pure Journalling and pure Write-Anywhere.
 *
 * In this mode all relocate-overwrite decisions are result of attempts to
 * defragment atom's locality.
 */

/* REVERSE PARENT-FIRST RELOCATION POLICIES */

/* This implements the is-it-close-enough-to-its-preceder? test for relocation
   in the reverse parent-first relocate context. Here all we know is the
   preceder and the block number. Since we are going in reverse, the preceder
   may still be relocated as well, so we can't ask the block allocator "is there
   a closer block available to relocate?" here. In the _forward_ parent-first
   relocate context (not here) we actually call the block allocator to try and
   find a closer location.
*/
static int reverse_try_defragment_if_close(const reiser4_block_nr * pblk,
					   const reiser4_block_nr * nblk)
{
	reiser4_block_nr dist;

	assert("jmacd-7710", *pblk != 0 && *nblk != 0);
	assert("jmacd-7711", !reiser4_blocknr_is_fake(pblk));
	assert("jmacd-7712", !reiser4_blocknr_is_fake(nblk));

	/* Distance is the absolute value. */
	dist = (*pblk > *nblk) ? (*pblk - *nblk) : (*nblk - *pblk);

	/* If the block is less than FLUSH_RELOCATE_DISTANCE blocks away from
	   its preceder block, do not relocate. */
	if (dist <= get_current_super_private()->flush.relocate_distance)
		return 0;

	return 1;
}

/**
 * This function is a predicate that tests for relocation. Always called in the
 * reverse-parent-first context, when we are asking whether the current node
 * should be relocated in order to expand the flush by dirtying the parent level
 * (and thus proceeding to flush that level). When traversing in the forward
 * parent-first direction (not here), relocation decisions are handled in two
 * places: allocate_znode() and extent_needs_allocation().
 */
static int reverse_alloc_formatted_hybrid(jnode * node,
					  const coord_t *parent_coord,
					  flush_pos_t *pos)
{
	reiser4_block_nr pblk = 0;
	reiser4_block_nr nblk = 0;

	assert("jmacd-8989", !jnode_is_root(node));
	/*
	 * This function is called only from the
	 * reverse_relocate_check_dirty_parent() and only if the parent
	 * node is clean. This implies that the parent has the real (i.e., not
	 * fake) block number, and, so does the child, because otherwise the
	 * parent would be dirty.
	 */

	/* New nodes are treated as if they are being relocated. */
	if (JF_ISSET(node, JNODE_CREATED) ||
	    (pos->leaf_relocate && jnode_get_level(node) == LEAF_LEVEL))
		return 1;

	/* Find the preceder. FIXME(B): When the child is an unformatted,
	   previously existing node, the coord may be leftmost even though the
	   child is not the parent-first preceder of the parent. If the first
	   dirty node appears somewhere in the middle of the first extent unit,
	   this preceder calculation is wrong.
	   Needs more logic in here. */
	if (coord_is_leftmost_unit(parent_coord)) {
		pblk = *znode_get_block(parent_coord->node);
	} else {
		pblk = pos->preceder.blk;
	}
	check_preceder(pblk);

	/* If (pblk == 0) then the preceder isn't allocated or isn't known:
	   relocate. */
	if (pblk == 0)
		return 1;

	nblk = *jnode_get_block(node);

	if (reiser4_blocknr_is_fake(&nblk))
		/* child is unallocated, mark parent dirty */
		return 1;

	return reverse_try_defragment_if_close(&pblk, &nblk);
}

/**
 * A subroutine of forward_alloc_formatted_hybrid(), this is called first to see
 * if there is a close position to relocate to. It may return ENOSPC if there is
 * no close position. If there is no close position it may not relocate. This
 * takes care of updating the parent node with the relocated block address.
 *
 * was allocate_znode_update()
 */
static int forward_try_defragment_locality(znode * node,
					   const coord_t *parent_coord,
					   flush_pos_t *pos)
{
	int ret;
	reiser4_block_nr blk;
	lock_handle uber_lock;
	int flush_reserved_used = 0;
	int grabbed;
	reiser4_context *ctx;
	reiser4_super_info_data *sbinfo;

	init_lh(&uber_lock);

	ctx = get_current_context();
	sbinfo = get_super_private(ctx->super);

	grabbed = ctx->grabbed_blocks;

	ret = zload(node);
	if (ret)
		return ret;

	if (ZF_ISSET(node, JNODE_CREATED)) {
		assert("zam-816", reiser4_blocknr_is_fake(znode_get_block(node)));
		pos->preceder.block_stage = BLOCK_UNALLOCATED;
	} else {
		pos->preceder.block_stage = BLOCK_GRABBED;

		/* The disk space for relocating the @node is already reserved
		 * in "flush reserved" counter if @node is leaf, otherwise we
		 * grab space using BA_RESERVED (means grab space from whole
		 * disk not from only 95%). */
		if (znode_get_level(node) == LEAF_LEVEL) {
			/*
			 * earlier (during do_jnode_make_dirty()) we decided
			 * that @node can possibly go into overwrite set and
			 * reserved block for its wandering location.
			 */
			txn_atom *atom = get_current_atom_locked();
			assert("nikita-3449",
			       ZF_ISSET(node, JNODE_FLUSH_RESERVED));
			flush_reserved2grabbed(atom, (__u64) 1);
			spin_unlock_atom(atom);
			/*
			 * we are trying to move node into relocate
			 * set. Allocation of relocated position "uses"
			 * reserved block.
			 */
			ZF_CLR(node, JNODE_FLUSH_RESERVED);
			flush_reserved_used = 1;
		} else {
			ret = reiser4_grab_space_force((__u64) 1, BA_RESERVED);
			if (ret != 0)
				goto exit;
		}
	}

	/* We may do not use 5% of reserved disk space here and flush will not
	   pack tightly. */
	ret = reiser4_alloc_block(&pos->preceder, &blk,
				  BA_FORMATTED | BA_PERMANENT);
	if (ret)
		goto exit;

	if (!ZF_ISSET(node, JNODE_CREATED) &&
	    (ret = reiser4_dealloc_block(znode_get_block(node), 0,
					 BA_DEFER | BA_FORMATTED)))
		goto exit;

	if (likely(!znode_is_root(node))) {
		item_plugin *iplug;

		iplug = item_plugin_by_coord(parent_coord);
		assert("nikita-2954", iplug->f.update != NULL);
		iplug->f.update(parent_coord, &blk);

		znode_make_dirty(parent_coord->node);

	} else {
		reiser4_tree *tree = znode_get_tree(node);
		znode *uber;

		/* We take a longterm lock on the fake node in order to change
		   the root block number.  This may cause atom fusion. */
		ret = get_uber_znode(tree, ZNODE_WRITE_LOCK, ZNODE_LOCK_HIPRI,
				     &uber_lock);
		/* The fake node cannot be deleted, and we must have priority
		   here, and may not be confused with ENOSPC. */
		assert("jmacd-74412",
		       ret != -EINVAL && ret != -E_DEADLOCK && ret != -ENOSPC);

		if (ret)
			goto exit;

		uber = uber_lock.node;

		write_lock_tree(tree);
		tree->root_block = blk;
		write_unlock_tree(tree);

		znode_make_dirty(uber);
	}
	ret = znode_rehash(node, &blk);
exit:
	if (ret) {
		/* Get flush reserved block back if something fails, because
		 * callers assume that on error block wasn't relocated and its
		 * flush reserved block wasn't used. */
		if (flush_reserved_used) {
			/*
			 * ok, we failed to move node into relocate
			 * set. Restore status quo.
			 */
			grabbed2flush_reserved((__u64) 1);
			ZF_SET(node, JNODE_FLUSH_RESERVED);
		}
	}
	zrelse(node);
	done_lh(&uber_lock);
	grabbed2free_mark(grabbed);
	return ret;
}

/*
 * Make the final relocate/wander decision during
 * forward parent-first squalloc for a formatted node
 */
static int forward_alloc_formatted_hybrid(znode * node,
					  const coord_t *parent_coord,
					  flush_pos_t *pos)
{
	int ret;
	reiser4_super_info_data *sbinfo = get_current_super_private();
	/**
 	 * FIXME(D): We have the node write-locked and should have checked for !
	 * allocated() somewhere before reaching this point, but there can be a
	 * race, so this assertion is bogus.
	 */
	assert("edward-1614", znode_is_loaded(node));
	assert("jmacd-7987", !jnode_check_flushprepped(ZJNODE(node)));
	assert("jmacd-7988", znode_is_write_locked(node));
	assert("jmacd-7989", coord_is_invalid(parent_coord)
	       || znode_is_write_locked(parent_coord->node));

	if (ZF_ISSET(node, JNODE_REPACK) || ZF_ISSET(node, JNODE_CREATED) ||
	    znode_is_root(node) ||
	    /*
	     * We have enough nodes to relocate no matter what.
	     */
	    (pos->leaf_relocate != 0 && znode_get_level(node) == LEAF_LEVEL)) {
		/*
		 * No need to decide with new nodes, they are treated the same
		 * as relocate. If the root node is dirty, relocate.
		 */
		if (pos->preceder.blk == 0) {
			/*
			 * preceder is unknown and we have decided to relocate
			 * node -- using of default value for search start is
			 * better than search from block #0.
			 */
			get_blocknr_hint_default(&pos->preceder.blk);
			check_preceder(pos->preceder.blk);
		}
		goto best_reloc;

	} else if (pos->preceder.blk == 0) {
		/* If we don't know the preceder, leave it where it is. */
		jnode_make_wander(ZJNODE(node));
	} else {
		/* Make a decision based on block distance. */
		reiser4_block_nr dist;
		reiser4_block_nr nblk = *znode_get_block(node);

		assert("jmacd-6172", !reiser4_blocknr_is_fake(&nblk));
		assert("jmacd-6173", !reiser4_blocknr_is_fake(&pos->preceder.blk));
		assert("jmacd-6174", pos->preceder.blk != 0);

		if (pos->preceder.blk == nblk - 1) {
			/* Ideal. */
			jnode_make_wander(ZJNODE(node));
		} else {

			dist =
			    (nblk <
			     pos->preceder.blk) ? (pos->preceder.blk -
						   nblk) : (nblk -
							    pos->preceder.blk);

			/* See if we can find a closer block
			   (forward direction only). */
			pos->preceder.max_dist =
			    min((reiser4_block_nr) sbinfo->flush.
				relocate_distance, dist);
			pos->preceder.level = znode_get_level(node);

			ret = forward_try_defragment_locality(node,
							      parent_coord,
							      pos);
			pos->preceder.max_dist = 0;

			if (ret && (ret != -ENOSPC))
				return ret;

			if (ret == 0) {
				/* Got a better allocation. */
				znode_make_reloc(node, pos->fq);
			} else if (dist < sbinfo->flush.relocate_distance) {
				/* The present allocation is good enough. */
				jnode_make_wander(ZJNODE(node));
			} else {
				/*
				 * Otherwise, try to relocate to the best
				 * position.
				 */
			best_reloc:
				ret = forward_try_defragment_locality(node,
								      parent_coord,
								      pos);
				if (ret != 0)
					return ret;
				/*
				 * set JNODE_RELOC bit _after_ node gets
				 * allocated
				 */
				znode_make_reloc(node, pos->fq);
			}
		}
	}
	/*
	 * This is the new preceder
	 */
	pos->preceder.blk = *znode_get_block(node);
	check_preceder(pos->preceder.blk);
	pos->alloc_cnt += 1;

	assert("jmacd-4277", !reiser4_blocknr_is_fake(&pos->preceder.blk));

	return 0;
}

static int forward_alloc_unformatted_hybrid(flush_pos_t *flush_pos)
{
	coord_t *coord;
	reiser4_extent *ext;
	oid_t oid;
	__u64 index;
	__u64 width;
	extent_state state;
	reiser4_key key;

	assert("vs-1468", flush_pos->state == POS_ON_EPOINT);
	assert("vs-1469", coord_is_existing_unit(&flush_pos->coord)
	       && item_is_extent(&flush_pos->coord));

	coord = &flush_pos->coord;

	ext = extent_by_coord(coord);
	state = state_of_extent(ext);
	if (state == HOLE_EXTENT) {
		flush_pos->state = POS_INVALID;
		return 0;
	}
	item_key_by_coord(coord, &key);
	oid = get_key_objectid(&key);
	index = extent_unit_index(coord) + flush_pos->pos_in_unit;
	width = extent_get_width(ext);

	assert("vs-1457", width > flush_pos->pos_in_unit);

	if (flush_pos->leaf_relocate || state == UNALLOCATED_EXTENT) {
		int exit;
		int result;
		result = forward_relocate_unformatted(flush_pos, ext, state,
						      oid,
						      index, width, &exit);
		if (exit)
			return result;
	} else
		forward_overwrite_unformatted(flush_pos, oid, index, width);

	flush_pos->pos_in_unit = 0;
	return 0;
}

static squeeze_result squeeze_alloc_unformatted_hybrid(znode *left,
						       const coord_t *coord,
						       flush_pos_t *flush_pos,
						       reiser4_key *stop_key)
{
	squeeze_result ret;
	reiser4_key key;
	reiser4_extent *ext;
	extent_state state;

	ext = extent_by_coord(coord);
	state = state_of_extent(ext);

	if ((flush_pos->leaf_relocate && state == ALLOCATED_EXTENT) ||
	    (state == UNALLOCATED_EXTENT))
		/*
		 * relocate
		 */
		ret = squeeze_relocate_unformatted(left, coord,
						   flush_pos, &key, stop_key);
	else
		/*
		 * (state == ALLOCATED_EXTENT && !flush_pos->leaf_relocate) ||
		 *  state == HOLE_EXTENT - overwrite
		 */
		ret = squeeze_overwrite_unformatted(left, coord,
						    flush_pos, &key, stop_key);
	if (ret == SQUEEZE_CONTINUE)
		*stop_key = key;
	return ret;
}

/*********************** JOURNAL TRANSACTION MODEL ****************************/

static int forward_alloc_formatted_journal(znode * node,
					   const coord_t *parent_coord,
					   flush_pos_t *pos)
{
	int ret;

	if (ZF_ISSET(node, JNODE_CREATED)) {
		if (pos->preceder.blk == 0) {
			/*
			 * preceder is unknown and we have decided to relocate
			 * node -- using of default value for search start is
			 * better than search from block #0.
			 */
			get_blocknr_hint_default(&pos->preceder.blk);
			check_preceder(pos->preceder.blk);
		}
		ret = forward_try_defragment_locality(node,
						      parent_coord,
						      pos);
		if (ret != 0) {
			warning("edward-1615",
				"forward defrag failed (%d)", ret);
			return ret;
		}
		/*
		 * set JNODE_RELOC bit _after_ node gets
		 * allocated
		 */
		znode_make_reloc(node, pos->fq);
	}
	else
		jnode_make_wander(ZJNODE(node));
	/*
	 * This is the new preceder
	 */
	pos->preceder.blk = *znode_get_block(node);
	check_preceder(pos->preceder.blk);
	pos->alloc_cnt += 1;

	assert("edward-1616", !reiser4_blocknr_is_fake(&pos->preceder.blk));
	return 0;
}

static int forward_alloc_unformatted_journal(flush_pos_t *flush_pos)
{

	coord_t *coord;
	reiser4_extent *ext;
	oid_t oid;
	__u64 index;
	__u64 width;
	extent_state state;
	reiser4_key key;

	assert("edward-1617", flush_pos->state == POS_ON_EPOINT);
	assert("edward-1618", coord_is_existing_unit(&flush_pos->coord)
	       && item_is_extent(&flush_pos->coord));

	coord = &flush_pos->coord;

	ext = extent_by_coord(coord);
	state = state_of_extent(ext);
	if (state == HOLE_EXTENT) {
		flush_pos->state = POS_INVALID;
		return 0;
	}
	item_key_by_coord(coord, &key);
	oid = get_key_objectid(&key);
	index = extent_unit_index(coord) + flush_pos->pos_in_unit;
	width = extent_get_width(ext);

	assert("edward-1619", width > flush_pos->pos_in_unit);

	if (state == UNALLOCATED_EXTENT) {
		int exit;
		int result;
		result = forward_relocate_unformatted(flush_pos, ext, state,
						      oid,
						      index, width, &exit);
		if (exit)
			return result;
	}
	else
		/*
		 * state == ALLOCATED_EXTENT
		 * keep old allocation
		 */
		forward_overwrite_unformatted(flush_pos, oid, index, width);

	flush_pos->pos_in_unit = 0;
	return 0;
}

static squeeze_result squeeze_alloc_unformatted_journal(znode *left,
							const coord_t *coord,
							flush_pos_t *flush_pos,
							reiser4_key *stop_key)
{
	squeeze_result ret;
	reiser4_key key;
	reiser4_extent *ext;
	extent_state state;

	ext = extent_by_coord(coord);
	state = state_of_extent(ext);

	if (state == UNALLOCATED_EXTENT)
		ret = squeeze_relocate_unformatted(left, coord,
						   flush_pos, &key, stop_key);
	else
		/*
		 * state == ALLOCATED_EXTENT || state == HOLE_EXTENT
		 */
		ret = squeeze_overwrite_unformatted(left, coord,
						    flush_pos, &key, stop_key);
	if (ret == SQUEEZE_CONTINUE)
		*stop_key = key;
	return ret;
}

/**********************  WA (Write-Anywhere) TRANSACTION MODEL  ***************/

static int forward_alloc_formatted_wa(znode * node,
				      const coord_t *parent_coord,
				      flush_pos_t *pos)
{
	int ret;

	assert("edward-1620", znode_is_loaded(node));
	assert("edward-1621", !jnode_check_flushprepped(ZJNODE(node)));
	assert("edward-1622", znode_is_write_locked(node));
	assert("edward-1623", coord_is_invalid(parent_coord)
	       || znode_is_write_locked(parent_coord->node));

	if (pos->preceder.blk == 0) {
		/*
		 * preceder is unknown and we have decided to relocate
		 * node -- using of default value for search start is
		 * better than search from block #0.
		 */
		get_blocknr_hint_default(&pos->preceder.blk);
		check_preceder(pos->preceder.blk);
	}
	ret = forward_try_defragment_locality(node, parent_coord, pos);
	if (ret && (ret != -ENOSPC)) {
		warning("edward-1624",
			"forward defrag failed (%d)", ret);
		return ret;
	}
	if (ret == 0)
		znode_make_reloc(node, pos->fq);
	else {
		ret = forward_try_defragment_locality(node, parent_coord, pos);
		if (ret) {
			warning("edward-1625",
				"forward defrag failed (%d)", ret);
			return ret;
		}
		/* set JNODE_RELOC bit _after_ node gets allocated */
		znode_make_reloc(node, pos->fq);
	}
	/*
	 * This is the new preceder
	 */
	pos->preceder.blk = *znode_get_block(node);
	check_preceder(pos->preceder.blk);
	pos->alloc_cnt += 1;

	assert("edward-1626", !reiser4_blocknr_is_fake(&pos->preceder.blk));
	return 0;
}

static int forward_alloc_unformatted_wa(flush_pos_t *flush_pos)
{
	int exit;
	int result;

	coord_t *coord;
	reiser4_extent *ext;
	oid_t oid;
	__u64 index;
	__u64 width;
	extent_state state;
	reiser4_key key;

	assert("edward-1627", flush_pos->state == POS_ON_EPOINT);
	assert("edward-1628", coord_is_existing_unit(&flush_pos->coord)
	       && item_is_extent(&flush_pos->coord));

	coord = &flush_pos->coord;

	ext = extent_by_coord(coord);
	state = state_of_extent(ext);
	if (state == HOLE_EXTENT) {
		flush_pos->state = POS_INVALID;
		return 0;
	}

	item_key_by_coord(coord, &key);
	oid = get_key_objectid(&key);
	index = extent_unit_index(coord) + flush_pos->pos_in_unit;
	width = extent_get_width(ext);

	assert("edward-1629", width > flush_pos->pos_in_unit);
	assert("edward-1630",
	       state == ALLOCATED_EXTENT || state == UNALLOCATED_EXTENT);
	/*
	 * always relocate
	 */
	result = forward_relocate_unformatted(flush_pos, ext, state, oid,
					      index, width, &exit);
	if (exit)
		return result;
	flush_pos->pos_in_unit = 0;
	return 0;
}

static squeeze_result squeeze_alloc_unformatted_wa(znode *left,
						   const coord_t *coord,
						   flush_pos_t *flush_pos,
						   reiser4_key *stop_key)
{
	squeeze_result ret;
	reiser4_key key;
	reiser4_extent *ext;
	extent_state state;

	ext = extent_by_coord(coord);
	state = state_of_extent(ext);

	if (state == HOLE_EXTENT)
		/*
		 * hole extents are handled in squeeze_overwrite
		 */
		ret = squeeze_overwrite_unformatted(left, coord,
						    flush_pos, &key, stop_key);
	else
		ret = squeeze_relocate_unformatted(left, coord,
						   flush_pos, &key, stop_key);
	if (ret == SQUEEZE_CONTINUE)
		*stop_key = key;
	return ret;
}

/******************************************************************************/

txmod_plugin txmod_plugins[LAST_TXMOD_ID] = {
	[HYBRID_TXMOD_ID] = {
		.h = {
			.type_id = REISER4_TXMOD_PLUGIN_TYPE,
			.id = HYBRID_TXMOD_ID,
			.pops = NULL,
			.label = "hybrid",
			.desc =	"Hybrid Transaction Model",
			.linkage = {NULL, NULL}
		},
		.forward_alloc_formatted = forward_alloc_formatted_hybrid,
		.reverse_alloc_formatted = reverse_alloc_formatted_hybrid,
		.forward_alloc_unformatted = forward_alloc_unformatted_hybrid,
		.squeeze_alloc_unformatted = squeeze_alloc_unformatted_hybrid
	},
	[JOURNAL_TXMOD_ID] = {
		.h = {
			.type_id = REISER4_TXMOD_PLUGIN_TYPE,
			.id = JOURNAL_TXMOD_ID,
			.pops = NULL,
			.label = "journal",
			.desc = "Journalling Transaction Model",
			.linkage = {NULL, NULL}
		},
		.forward_alloc_formatted = forward_alloc_formatted_journal,
		.reverse_alloc_formatted = NULL,
		.forward_alloc_unformatted = forward_alloc_unformatted_journal,
		.squeeze_alloc_unformatted = squeeze_alloc_unformatted_journal
	},
	[WA_TXMOD_ID] = {
		.h = {
			.type_id = REISER4_TXMOD_PLUGIN_TYPE,
			.id = WA_TXMOD_ID,
			.pops = NULL,
			.label = "wa",
			.desc =	"Write-Anywhere Transaction Model",
			.linkage = {NULL, NULL}
		},
		.forward_alloc_formatted = forward_alloc_formatted_wa,
		.reverse_alloc_formatted = NULL,
		.forward_alloc_unformatted = forward_alloc_unformatted_wa,
		.squeeze_alloc_unformatted = squeeze_alloc_unformatted_wa
	}
};

/*
 * Local variables:
 * c-indentation-style: "K&R"
 * mode-name: "LC"
 * c-basic-offset: 8
 * tab-width: 8
 * fill-column: 79
 * End:
 */
