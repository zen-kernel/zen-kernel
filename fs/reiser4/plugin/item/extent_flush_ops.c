/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

#include "item.h"
#include "../../tree.h"
#include "../../jnode.h"
#include "../../super.h"
#include "../../flush.h"
#include "../../carry.h"
#include "../object.h"

#include <linux/pagemap.h>

static reiser4_block_nr extent_unit_start(const coord_t * item);

/* Return either first or last extent (depending on @side) of the item
   @coord is set to. Set @pos_in_unit either to first or to last block
   of extent. */
static reiser4_extent *extent_utmost_ext(const coord_t * coord, sideof side,
					 reiser4_block_nr * pos_in_unit)
{
	reiser4_extent *ext;

	if (side == LEFT_SIDE) {
		/* get first extent of item */
		ext = extent_item(coord);
		*pos_in_unit = 0;
	} else {
		/* get last extent of item and last position within it */
		assert("vs-363", side == RIGHT_SIDE);
		ext = extent_item(coord) + coord_last_unit_pos(coord);
		*pos_in_unit = extent_get_width(ext) - 1;
	}

	return ext;
}

/* item_plugin->f.utmost_child */
/* Return the child. Coord is set to extent item. Find jnode corresponding
   either to first or to last unformatted node pointed by the item */
int utmost_child_extent(const coord_t * coord, sideof side, jnode ** childp)
{
	reiser4_extent *ext;
	reiser4_block_nr pos_in_unit;

	ext = extent_utmost_ext(coord, side, &pos_in_unit);

	switch (state_of_extent(ext)) {
	case HOLE_EXTENT:
		*childp = NULL;
		return 0;
	case ALLOCATED_EXTENT:
	case UNALLOCATED_EXTENT:
		break;
	default:
		/* this should never happen */
		assert("vs-1417", 0);
	}

	{
		reiser4_key key;
		reiser4_tree *tree;
		unsigned long index;

		if (side == LEFT_SIDE) {
			/* get key of first byte addressed by the extent */
			item_key_by_coord(coord, &key);
		} else {
			/* get key of byte which next after last byte addressed by the extent */
			append_key_extent(coord, &key);
		}

		assert("vs-544",
		       (get_key_offset(&key) >> PAGE_CACHE_SHIFT) < ~0ul);
		/* index of first or last (depending on @side) page addressed
		   by the extent */
		index =
		    (unsigned long)(get_key_offset(&key) >> PAGE_CACHE_SHIFT);
		if (side == RIGHT_SIDE)
			index--;

		tree = coord->node->zjnode.tree;
		*childp = jlookup(tree, get_key_objectid(&key), index);
	}

	return 0;
}

/* item_plugin->f.utmost_child_real_block */
/* Return the child's block, if allocated. */
int
utmost_child_real_block_extent(const coord_t * coord, sideof side,
			       reiser4_block_nr * block)
{
	reiser4_extent *ext;

	ext = extent_by_coord(coord);

	switch (state_of_extent(ext)) {
	case ALLOCATED_EXTENT:
		*block = extent_get_start(ext);
		if (side == RIGHT_SIDE)
			*block += extent_get_width(ext) - 1;
		break;
	case HOLE_EXTENT:
	case UNALLOCATED_EXTENT:
		*block = 0;
		break;
	default:
		/* this should never happen */
		assert("vs-1418", 0);
	}

	return 0;
}

/* item_plugin->f.scan */
/* Performs leftward scanning starting from an unformatted node and its parent coordinate.
   This scan continues, advancing the parent coordinate, until either it encounters a
   formatted child or it finishes scanning this node.

   If unallocated, the entire extent must be dirty and in the same atom.  (Actually, I'm
   not sure this is last property (same atom) is enforced, but it should be the case since
   one atom must write the parent and the others must read the parent, thus fusing?).  In
   any case, the code below asserts this case for unallocated extents.  Unallocated
   extents are thus optimized because we can skip to the endpoint when scanning.

   It returns control to reiser4_scan_extent, handles these terminating conditions,
   e.g., by loading the next twig.
*/
int reiser4_scan_extent(flush_scan * scan)
{
	coord_t coord;
	jnode *neighbor;
	unsigned long scan_index, unit_index, unit_width, scan_max, scan_dist;
	reiser4_block_nr unit_start;
	__u64 oid;
	reiser4_key key;
	int ret = 0, allocated, incr;
	reiser4_tree *tree;

	if (!JF_ISSET(scan->node, JNODE_DIRTY)) {
		scan->stop = 1;
		return 0;	/* Race with truncate, this node is already
				 * truncated. */
	}

	coord_dup(&coord, &scan->parent_coord);

	assert("jmacd-1404", !reiser4_scan_finished(scan));
	assert("jmacd-1405", jnode_get_level(scan->node) == LEAF_LEVEL);
	assert("jmacd-1406", jnode_is_unformatted(scan->node));

	/* The scan_index variable corresponds to the current page index of the
	   unformatted block scan position. */
	scan_index = index_jnode(scan->node);

	assert("jmacd-7889", item_is_extent(&coord));

      repeat:
	/* objectid of file */
	oid = get_key_objectid(item_key_by_coord(&coord, &key));

	allocated = !extent_is_unallocated(&coord);
	/* Get the values of this extent unit: */
	unit_index = extent_unit_index(&coord);
	unit_width = extent_unit_width(&coord);
	unit_start = extent_unit_start(&coord);

	assert("jmacd-7187", unit_width > 0);
	assert("jmacd-7188", scan_index >= unit_index);
	assert("jmacd-7189", scan_index <= unit_index + unit_width - 1);

	/* Depending on the scan direction, we set different maximum values for scan_index
	   (scan_max) and the number of nodes that would be passed if the scan goes the
	   entire way (scan_dist).  Incr is an integer reflecting the incremental
	   direction of scan_index. */
	if (reiser4_scanning_left(scan)) {
		scan_max = unit_index;
		scan_dist = scan_index - unit_index;
		incr = -1;
	} else {
		scan_max = unit_index + unit_width - 1;
		scan_dist = scan_max - unit_index;
		incr = +1;
	}

	tree = coord.node->zjnode.tree;

	/* If the extent is allocated we have to check each of its blocks.  If the extent
	   is unallocated we can skip to the scan_max. */
	if (allocated) {
		do {
			neighbor = jlookup(tree, oid, scan_index);
			if (neighbor == NULL)
				goto stop_same_parent;

			if (scan->node != neighbor
			    && !reiser4_scan_goto(scan, neighbor)) {
				/* @neighbor was jput() by reiser4_scan_goto */
				goto stop_same_parent;
			}

			ret = scan_set_current(scan, neighbor, 1, &coord);
			if (ret != 0) {
				goto exit;
			}

			/* reference to @neighbor is stored in @scan, no need
			   to jput(). */
			scan_index += incr;

		} while (incr + scan_max != scan_index);

	} else {
		/* Optimized case for unallocated extents, skip to the end. */
		neighbor = jlookup(tree, oid, scan_max /*index */ );
		if (neighbor == NULL) {
			/* Race with truncate */
			scan->stop = 1;
			ret = 0;
			goto exit;
		}

		assert("zam-1043",
		       reiser4_blocknr_is_fake(jnode_get_block(neighbor)));

		ret = scan_set_current(scan, neighbor, scan_dist, &coord);
		if (ret != 0) {
			goto exit;
		}
	}

	if (coord_sideof_unit(&coord, scan->direction) == 0
	    && item_is_extent(&coord)) {
		/* Continue as long as there are more extent units. */

		scan_index =
		    extent_unit_index(&coord) +
		    (reiser4_scanning_left(scan) ?
		     extent_unit_width(&coord) - 1 : 0);
		goto repeat;
	}

	if (0) {
	      stop_same_parent:

		/* If we are scanning left and we stop in the middle of an allocated
		   extent, we know the preceder immediately.. */
		/* middle of extent is (scan_index - unit_index) != 0. */
		if (reiser4_scanning_left(scan) &&
		    (scan_index - unit_index) != 0) {
			/* FIXME(B): Someone should step-through and verify that this preceder
			   calculation is indeed correct. */
			/* @unit_start is starting block (number) of extent
			   unit. Flush stopped at the @scan_index block from
			   the beginning of the file, which is (scan_index -
			   unit_index) block within extent.
			 */
			if (unit_start) {
				/* skip preceder update when we are at hole */
				scan->preceder_blk =
				    unit_start + scan_index - unit_index;
				check_preceder(scan->preceder_blk);
			}
		}

		/* In this case, we leave coord set to the parent of scan->node. */
		scan->stop = 1;

	} else {
		/* In this case, we are still scanning, coord is set to the next item which is
		   either off-the-end of the node or not an extent. */
		assert("jmacd-8912", scan->stop == 0);
		assert("jmacd-7812",
		       (coord_is_after_sideof_unit(&coord, scan->direction)
			|| !item_is_extent(&coord)));
	}

	ret = 0;
      exit:
	return ret;
}

/* ask block allocator for some blocks */
static void extent_allocate_blocks(reiser4_blocknr_hint *preceder,
				   reiser4_block_nr wanted_count,
				   reiser4_block_nr *first_allocated,
				   reiser4_block_nr *allocated,
				   block_stage_t block_stage)
{
	*allocated = wanted_count;
	preceder->max_dist = 0;	/* scan whole disk, if needed */

	/* that number of blocks (wanted_count) is either in UNALLOCATED or in GRABBED */
	preceder->block_stage = block_stage;

	/* FIXME: we do not handle errors here now */
	check_me("vs-420",
		 reiser4_alloc_blocks(preceder, first_allocated, allocated,
				      BA_PERMANENT) == 0);
	/* update flush_pos's preceder to last allocated block number */
	preceder->blk = *first_allocated + *allocated - 1;
}

/* when on flush time unallocated extent is to be replaced with allocated one it may happen that one unallocated extent
   will have to be replaced with set of allocated extents. In this case insert_into_item will be called which may have
   to add new nodes into tree. Space for that is taken from inviolable reserve (5%). */
static reiser4_block_nr reserve_replace(void)
{
	reiser4_block_nr grabbed, needed;

	grabbed = get_current_context()->grabbed_blocks;
	needed = estimate_one_insert_into_item(current_tree);
	check_me("vpf-340", !reiser4_grab_space_force(needed, BA_RESERVED));
	return grabbed;
}

static void free_replace_reserved(reiser4_block_nr grabbed)
{
	reiser4_context *ctx;

	ctx = get_current_context();
	grabbed2free(ctx, get_super_private(ctx->super),
		     ctx->grabbed_blocks - grabbed);
}

/* Block offset of first block addressed by unit */
__u64 extent_unit_index(const coord_t * item)
{
	reiser4_key key;

	assert("vs-648", coord_is_existing_unit(item));
	unit_key_by_coord(item, &key);
	return get_key_offset(&key) >> current_blocksize_bits;
}

/* AUDIT shouldn't return value be of reiser4_block_nr type?
   Josh's answer: who knows?  Is a "number of blocks" the same type as "block offset"? */
__u64 extent_unit_width(const coord_t * item)
{
	assert("vs-649", coord_is_existing_unit(item));
	return width_by_coord(item);
}

/* Starting block location of this unit */
static reiser4_block_nr extent_unit_start(const coord_t * item)
{
	return extent_get_start(extent_by_coord(item));
}

/**
 * split_allocated_extent -
 * @coord:
 * @pos_in_unit:
 *
 * replace allocated extent with two allocated extents
 */
static int split_allocated_extent(coord_t *coord, reiser4_block_nr pos_in_unit)
{
	int result;
	struct replace_handle *h;
	reiser4_extent *ext;
	reiser4_block_nr grabbed;

	ext = extent_by_coord(coord);
	assert("vs-1410", state_of_extent(ext) == ALLOCATED_EXTENT);
	assert("vs-1411", extent_get_width(ext) > pos_in_unit);

	h = kmalloc(sizeof(*h), reiser4_ctx_gfp_mask_get());
	if (h == NULL)
		return RETERR(-ENOMEM);
	h->coord = coord;
	h->lh = znode_lh(coord->node);
	h->pkey = &h->key;
	unit_key_by_coord(coord, h->pkey);
	set_key_offset(h->pkey,
		       (get_key_offset(h->pkey) +
			pos_in_unit * current_blocksize));
	reiser4_set_extent(&h->overwrite, extent_get_start(ext),
			   pos_in_unit);
	reiser4_set_extent(&h->new_extents[0],
			   extent_get_start(ext) + pos_in_unit,
			   extent_get_width(ext) - pos_in_unit);
	h->nr_new_extents = 1;
	h->flags = COPI_DONT_SHIFT_LEFT;
	h->paste_key = h->key;

	/* reserve space for extent unit paste, @grabbed is reserved before */
	grabbed = reserve_replace();
	result = reiser4_replace_extent(h, 0 /* leave @coord set to overwritten
						extent */);
	/* restore reserved */
	free_replace_reserved(grabbed);
	kfree(h);
	return result;
}

/* replace extent @ext by extent @replace. Try to merge @replace with previous extent of the item (if there is
   one). Return 1 if it succeeded, 0 - otherwise */
static int try_to_merge_with_left(coord_t *coord, reiser4_extent *ext,
		       reiser4_extent *replace)
{
	assert("vs-1415", extent_by_coord(coord) == ext);

	if (coord->unit_pos == 0
	    || state_of_extent(ext - 1) != ALLOCATED_EXTENT)
		/* @ext either does not exist or is not allocated extent */
		return 0;
	if (extent_get_start(ext - 1) + extent_get_width(ext - 1) !=
	    extent_get_start(replace))
		return 0;

	/* we can glue, widen previous unit */
	extent_set_width(ext - 1,
			 extent_get_width(ext - 1) + extent_get_width(replace));

	if (extent_get_width(ext) != extent_get_width(replace)) {
		/* make current extent narrower */
		if (state_of_extent(ext) == ALLOCATED_EXTENT)
			extent_set_start(ext,
					 extent_get_start(ext) +
					 extent_get_width(replace));
		extent_set_width(ext,
				 extent_get_width(ext) -
				 extent_get_width(replace));
	} else {
		/* current extent completely glued with its left neighbor, remove it */
		coord_t from, to;

		coord_dup(&from, coord);
		from.unit_pos = nr_units_extent(coord) - 1;
		coord_dup(&to, &from);

		/* currently cut from extent can cut either from the beginning or from the end. Move place which got
		   freed after unit removal to end of item */
		memmove(ext, ext + 1,
			(from.unit_pos -
			 coord->unit_pos) * sizeof(reiser4_extent));
		/* wipe part of item which is going to be cut, so that node_check will not be confused */
		cut_node_content(&from, &to, NULL, NULL, NULL);
	}
	znode_make_dirty(coord->node);
	/* move coord back */
	coord->unit_pos--;
	return 1;
}

/**
 * conv_extent - replace extent with 2 ones
 * @coord: coordinate of extent to be replaced
 * @replace: extent to overwrite the one @coord is set to
 *
 * Overwrites extent @coord is set to and paste one extent unit after
 * overwritten one if @replace is shorter than initial extent
 */
static int conv_extent(coord_t *coord, reiser4_extent *replace)
{
	int result;
	struct replace_handle *h;
	reiser4_extent *ext;
	reiser4_block_nr start, width, new_width;
	reiser4_block_nr grabbed;
	extent_state state;

	ext = extent_by_coord(coord);
	state = state_of_extent(ext);
	start = extent_get_start(ext);
	width = extent_get_width(ext);
	new_width = extent_get_width(replace);

	assert("vs-1458", (state == UNALLOCATED_EXTENT ||
			   state == ALLOCATED_EXTENT));
	assert("vs-1459", width >= new_width);

	if (try_to_merge_with_left(coord, ext, replace)) {
		/* merged @replace with left neighbor. Current unit is either
		   removed or narrowed */
		return 0;
	}

	if (width == new_width) {
		/* replace current extent with @replace */
		*ext = *replace;
		znode_make_dirty(coord->node);
		return 0;
	}

	h = kmalloc(sizeof(*h), reiser4_ctx_gfp_mask_get());
	if (h == NULL)
		return RETERR(-ENOMEM);
	h->coord = coord;
	h->lh = znode_lh(coord->node);
	h->pkey = &h->key;
	unit_key_by_coord(coord, h->pkey);
	set_key_offset(h->pkey,
		       (get_key_offset(h->pkey) + new_width * current_blocksize));
	h->overwrite = *replace;

	/* replace @ext with @replace and padding extent */
	reiser4_set_extent(&h->new_extents[0],
			   (state == ALLOCATED_EXTENT) ?
			   (start + new_width) :
			   UNALLOCATED_EXTENT_START,
			   width - new_width);
	h->nr_new_extents = 1;
	h->flags = COPI_DONT_SHIFT_LEFT;
	h->paste_key = h->key;

	/* reserve space for extent unit paste, @grabbed is reserved before */
	grabbed = reserve_replace();
	result = reiser4_replace_extent(h, 0 /* leave @coord set to overwritten
						extent */);

	/* restore reserved */
	free_replace_reserved(grabbed);
	kfree(h);
	return result;
}

/**
 * assign_real_blocknrs
 * @flush_pos:
 * @oid: objectid of file jnodes to assign block number to belongs to
 * @index: first jnode on the range
 * @count: number of jnodes to assign block numbers to
 * @first: start of allocated block range
 *
 * Assigns block numbers to each of @count jnodes. Index of first jnode is
 * @index. Jnodes get lookuped with jlookup.
 */
static void assign_real_blocknrs(flush_pos_t *flush_pos, oid_t oid,
				 unsigned long index, reiser4_block_nr count,
				 reiser4_block_nr first)
{
	unsigned long i;
	reiser4_tree *tree;
	txn_atom *atom;
	int nr;

	atom = atom_locked_by_fq(flush_pos->fq);
	assert("vs-1468", atom);
	BUG_ON(atom == NULL);

	nr = 0;
	tree = current_tree;
	for (i = 0; i < count; ++i, ++index) {
		jnode *node;

		node = jlookup(tree, oid, index);
		assert("", node != NULL);
		BUG_ON(node == NULL);

		spin_lock_jnode(node);
		assert("", !jnode_is_flushprepped(node));
		assert("vs-1475", node->atom == atom);
		assert("vs-1476", atomic_read(&node->x_count) > 0);

		JF_CLR(node, JNODE_FLUSH_RESERVED);
		jnode_set_block(node, &first);
		unformatted_make_reloc(node, flush_pos->fq);
		ON_DEBUG(count_jnode(node->atom, node, NODE_LIST(node),
				     FQ_LIST, 0));
		spin_unlock_jnode(node);
		first++;

		atomic_dec(&node->x_count);
		nr ++;
	}

	spin_unlock_atom(atom);
	return;
}

/**
 * make_node_ovrwr - assign node to overwrite set
 * @jnodes: overwrite set list head
 * @node: jnode to belong to overwrite set
 *
 * Sets OVRWR jnode state bit and puts @node to the end of list head @jnodes
 * which is an accumulator for nodes before they get to overwrite set list of
 * atom.
 */
static void make_node_ovrwr(struct list_head *jnodes, jnode *node)
{
	spin_lock_jnode(node);

	assert("zam-917", !JF_ISSET(node, JNODE_RELOC));
	assert("zam-918", !JF_ISSET(node, JNODE_OVRWR));

	JF_SET(node, JNODE_OVRWR);
	list_move_tail(&node->capture_link, jnodes);
	ON_DEBUG(count_jnode(node->atom, node, DIRTY_LIST, OVRWR_LIST, 0));

	spin_unlock_jnode(node);
}

/**
 * mark_jnodes_overwrite - put bunch of jnodes to overwrite set
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
static void mark_jnodes_overwrite(flush_pos_t *flush_pos, oid_t oid,
				  unsigned long index, reiser4_block_nr width)
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
		make_node_ovrwr(&jnodes, node);
		atomic_dec(&node->x_count);
	}

	list_splice_init(&jnodes, ATOM_OVRWR_LIST(atom)->prev);
	spin_unlock_atom(atom);
}

/**
 * allocated_extent_slum_size
 * @flush_pos:
 * @oid:
 * @index:
 * @count:
 *
 *
 */
static int allocated_extent_slum_size(flush_pos_t *flush_pos, oid_t oid,
				      unsigned long index, unsigned long count)
{
	unsigned long i;
	reiser4_tree *tree;
	txn_atom *atom;
	int nr;

	atom = atom_locked_by_fq(reiser4_pos_fq(flush_pos));
	assert("vs-1468", atom);

	nr = 0;
	tree = current_tree;
	for (i = 0; i < count; ++i, ++index) {
		jnode *node;

		node = jlookup(tree, oid, index);
		if (!node)
			break;

		if (jnode_check_flushprepped(node)) {
			atomic_dec(&node->x_count);
			break;
		}

		if (node->atom != atom) {
			/*
			 * this is possible on overwrite: extent_write may
			 * capture several unformatted nodes without capturing
			 * any formatted nodes.
			 */
			atomic_dec(&node->x_count);
			break;
		}

		assert("vs-1476", atomic_read(&node->x_count) > 1);
		atomic_dec(&node->x_count);
		nr ++;
	}

	spin_unlock_atom(atom);
	return nr;
}

/**
 * alloc_extent
 * @flush_pos:
 *
 *
 * this is called by handle_pos_on_twig to proceed extent unit flush_pos->coord
 * is set to. It is to prepare for flushing sequence of not flushprepped nodes
 * (slum). It supposes that slum starts at flush_pos->pos_in_unit position
 * within the extent. Slum gets to relocate set if flush_pos->leaf_relocate is
 * set to 1 and to overwrite set otherwise
 */
int reiser4_alloc_extent(flush_pos_t *flush_pos)
{
	coord_t *coord;
	reiser4_extent *ext;
	reiser4_extent replace_ext;
	oid_t oid;
	reiser4_block_nr protected;
	reiser4_block_nr start;
	__u64 index;
	__u64 width;
	extent_state state;
	int result;
	reiser4_block_nr first_allocated;
	__u64 allocated;
	reiser4_key key;
	block_stage_t block_stage;

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
	start = extent_get_start(ext);
	width = extent_get_width(ext);

	assert("vs-1457", width > flush_pos->pos_in_unit);

	if (flush_pos->leaf_relocate || state == UNALLOCATED_EXTENT) {
		/* relocate */
		if (flush_pos->pos_in_unit) {
			/* split extent unit into two */
			result =
			    split_allocated_extent(coord,
						   flush_pos->pos_in_unit);
			flush_pos->pos_in_unit = 0;
			return result;
		}

		/* limit number of nodes to allocate */
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

		/* allocate new block numbers for protected nodes */
		extent_allocate_blocks(reiser4_pos_hint(flush_pos),
				       protected,
				       &first_allocated, &allocated,
				       block_stage);

		if (state == ALLOCATED_EXTENT)
			/*
			 * on relocating - free nodes which are going to be
			 * relocated
			 */
			reiser4_dealloc_blocks(&start, &allocated,
					       BLOCK_ALLOCATED, BA_DEFER);

		/* assign new block numbers to protected nodes */
		assign_real_blocknrs(flush_pos, oid, index, allocated, first_allocated);

		/* prepare extent which will replace current one */
		reiser4_set_extent(&replace_ext, first_allocated, allocated);

		/* adjust extent item */
		result = conv_extent(coord, &replace_ext);
		if (result != 0 && result != -ENOMEM) {
			warning("vs-1461",
				"Failed to allocate extent. Should not happen\n");
			return result;
		}

		/*
		 * break flush: we prepared for flushing as many blocks as we
		 * were asked for
		 */
		if (flush_pos->nr_to_write == allocated)
			flush_pos->state = POS_INVALID;
	} else {
		/* overwrite */
		mark_jnodes_overwrite(flush_pos, oid, index, width);
	}
	flush_pos->pos_in_unit = 0;
	return 0;
}

/* if @key is glueable to the item @coord is set to */
static int must_insert(const coord_t *coord, const reiser4_key *key)
{
	reiser4_key last;

	if (item_id_by_coord(coord) == EXTENT_POINTER_ID
	    && keyeq(append_key_extent(coord, &last), key))
		return 0;
	return 1;
}

/* copy extent @copy to the end of @node. It may have to either insert new item after the last one, or append last item,
   or modify last unit of last item to have greater width */
static int put_unit_to_end(znode *node, const reiser4_key *key,
			   reiser4_extent *copy_ext)
{
	int result;
	coord_t coord;
	cop_insert_flag flags;
	reiser4_extent *last_ext;
	reiser4_item_data data;

	/* set coord after last unit in an item */
	coord_init_last_unit(&coord, node);
	coord.between = AFTER_UNIT;

	flags =
	    COPI_DONT_SHIFT_LEFT | COPI_DONT_SHIFT_RIGHT | COPI_DONT_ALLOCATE;
	if (must_insert(&coord, key)) {
		result =
		    insert_by_coord(&coord, init_new_extent(&data, copy_ext, 1),
				    key, NULL /*lh */ , flags);

	} else {
		/* try to glue with last unit */
		last_ext = extent_by_coord(&coord);
		if (state_of_extent(last_ext) &&
		    extent_get_start(last_ext) + extent_get_width(last_ext) ==
		    extent_get_start(copy_ext)) {
			/* widen last unit of node */
			extent_set_width(last_ext,
					 extent_get_width(last_ext) +
					 extent_get_width(copy_ext));
			znode_make_dirty(node);
			return 0;
		}

		/* FIXME: put an assertion here that we can not merge last unit in @node and new unit */
		result =
		    insert_into_item(&coord, NULL /*lh */ , key,
				     init_new_extent(&data, copy_ext, 1),
				     flags);
	}

	assert("vs-438", result == 0 || result == -E_NODE_FULL);
	return result;
}

/* @coord is set to extent unit */
squeeze_result squalloc_extent(znode *left, const coord_t *coord,
			       flush_pos_t *flush_pos,
			       reiser4_key *stop_key)
{
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
	reiser4_key key;
	int result;
	block_stage_t block_stage;

	assert("vs-1457", flush_pos->pos_in_unit == 0);
	assert("vs-1467", coord_is_leftmost_unit(coord));
	assert("vs-1467", item_is_extent(coord));

	ext = extent_by_coord(coord);
	index = extent_unit_index(coord);
	start = extent_get_start(ext);
	width = extent_get_width(ext);
	state = state_of_extent(ext);
	unit_key_by_coord(coord, &key);
	oid = get_key_objectid(&key);

	if ((flush_pos->leaf_relocate && state == ALLOCATED_EXTENT) ||
	    (state == UNALLOCATED_EXTENT)) {
		/* relocate */
		if (state == ALLOCATED_EXTENT) {
			/* all protected nodes are not flushprepped, therefore
			 * they are counted as flush_reserved */
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

		/* allocate new block numbers for protected nodes */
		extent_allocate_blocks(reiser4_pos_hint(flush_pos),
				       protected,
				       &first_allocated, &allocated,
				       block_stage);

		/* prepare extent which will be copied to left */
		reiser4_set_extent(&copy_extent, first_allocated, allocated);

		result = put_unit_to_end(left, &key, &copy_extent);
		if (result == -E_NODE_FULL) {
			int target_block_stage;

			/* free blocks which were just allocated */
			target_block_stage =
			    (state ==
			     ALLOCATED_EXTENT) ? BLOCK_FLUSH_RESERVED :
			    BLOCK_UNALLOCATED;
			reiser4_dealloc_blocks(&first_allocated, &allocated,
					       target_block_stage,
					       BA_PERMANENT);

			/* rewind the preceder. */
			flush_pos->preceder.blk = first_allocated;
			check_preceder(flush_pos->preceder.blk);

			return SQUEEZE_TARGET_FULL;
		}

		if (state == ALLOCATED_EXTENT) {
			/* free nodes which were relocated */
			reiser4_dealloc_blocks(&start, &allocated,
					       BLOCK_ALLOCATED, BA_DEFER);
		}

		/* assign new block numbers to protected nodes */
		assign_real_blocknrs(flush_pos, oid, index, allocated,
				     first_allocated);

		set_key_offset(&key,
			       get_key_offset(&key) +
			       (allocated << current_blocksize_bits));
	} else {
		/*
		 * overwrite: try to copy unit as it is to left neighbor and
		 * make all first not flushprepped nodes overwrite nodes
		 */
		reiser4_set_extent(&copy_extent, start, width);
		result = put_unit_to_end(left, &key, &copy_extent);
		if (result == -E_NODE_FULL)
			return SQUEEZE_TARGET_FULL;

		if (state != HOLE_EXTENT)
			mark_jnodes_overwrite(flush_pos, oid, index, width);
		set_key_offset(&key,
			       get_key_offset(&key) +
			       (width << current_blocksize_bits));
	}
	*stop_key = key;
	return SQUEEZE_CONTINUE;
}

int key_by_offset_extent(struct inode *inode, loff_t off, reiser4_key * key)
{
	return key_by_inode_and_offset_common(inode, off, key);
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
