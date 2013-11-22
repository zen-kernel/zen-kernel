/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
   reiser4/README */

/* implementation of carry operations */

#include "forward.h"
#include "debug.h"
#include "key.h"
#include "coord.h"
#include "plugin/item/item.h"
#include "plugin/node/node.h"
#include "jnode.h"
#include "znode.h"
#include "block_alloc.h"
#include "tree_walk.h"
#include "pool.h"
#include "tree_mod.h"
#include "carry.h"
#include "carry_ops.h"
#include "tree.h"
#include "super.h"
#include "reiser4.h"

#include <linux/types.h>
#include <linux/err.h>

static int carry_shift_data(sideof side, coord_t *insert_coord, znode * node,
			    carry_level * doing, carry_level * todo,
			    unsigned int including_insert_coord_p);

extern int lock_carry_node(carry_level * level, carry_node * node);
extern int lock_carry_node_tail(carry_node * node);

/* find left neighbor of a carry node

   Look for left neighbor of @node and add it to the @doing queue. See
   comments in the body.

*/
static carry_node *find_left_neighbor(carry_op * op	/* node to find left
							 * neighbor of */ ,
				      carry_level * doing/* level to scan */)
{
	int result;
	carry_node *node;
	carry_node *left;
	int flags;
	reiser4_tree *tree;

	node = op->node;

	tree = current_tree;
	read_lock_tree(tree);
	/* first, check whether left neighbor is already in a @doing queue */
	if (reiser4_carry_real(node)->left != NULL) {
		/* NOTE: there is locking subtlety here. Look into
		 * find_right_neighbor() for more info */
		if (find_carry_node(doing,
				    reiser4_carry_real(node)->left) != NULL) {
			read_unlock_tree(tree);
			left = node;
			do {
				left = list_entry(left->header.level_linkage.prev,
						  carry_node, header.level_linkage);
				assert("nikita-3408", !carry_node_end(doing,
								      left));
			} while (reiser4_carry_real(left) ==
				 reiser4_carry_real(node));
			return left;
		}
	}
	read_unlock_tree(tree);

	left = reiser4_add_carry_skip(doing, POOLO_BEFORE, node);
	if (IS_ERR(left))
		return left;

	left->node = node->node;
	left->free = 1;

	flags = GN_TRY_LOCK;
	if (!(op->u.insert.flags & COPI_LOAD_LEFT))
		flags |= GN_NO_ALLOC;

	/* then, feeling lucky, peek left neighbor in the cache. */
	result = reiser4_get_left_neighbor(&left->lock_handle,
					   reiser4_carry_real(node),
					   ZNODE_WRITE_LOCK, flags);
	if (result == 0) {
		/* ok, node found and locked. */
		result = lock_carry_node_tail(left);
		if (result != 0)
			left = ERR_PTR(result);
	} else if (result == -E_NO_NEIGHBOR || result == -ENOENT) {
		/* node is leftmost node in a tree, or neighbor wasn't in
		   cache, or there is an extent on the left. */
		reiser4_pool_free(&doing->pool->node_pool, &left->header);
		left = NULL;
	} else if (doing->restartable) {
		/* if left neighbor is locked, and level is restartable, add
		   new node to @doing and restart. */
		assert("nikita-913", node->parent != 0);
		assert("nikita-914", node->node != NULL);
		left->left = 1;
		left->free = 0;
		left = ERR_PTR(-E_REPEAT);
	} else {
		/* left neighbor is locked, level cannot be restarted. Just
		   ignore left neighbor. */
		reiser4_pool_free(&doing->pool->node_pool, &left->header);
		left = NULL;
	}
	return left;
}

/* find right neighbor of a carry node

   Look for right neighbor of @node and add it to the @doing queue. See
   comments in the body.

*/
static carry_node *find_right_neighbor(carry_op * op	/* node to find right
							 * neighbor of */ ,
				       carry_level * doing/* level to scan */)
{
	int result;
	carry_node *node;
	carry_node *right;
	lock_handle lh;
	int flags;
	reiser4_tree *tree;

	init_lh(&lh);

	node = op->node;

	tree = current_tree;
	read_lock_tree(tree);
	/* first, check whether right neighbor is already in a @doing queue */
	if (reiser4_carry_real(node)->right != NULL) {
		/*
		 * Tree lock is taken here anyway, because, even if _outcome_
		 * of (find_carry_node() != NULL) doesn't depends on
		 * concurrent updates to ->right, find_carry_node() cannot
		 * work with second argument NULL. Hence, following comment is
		 * of historic importance only.
		 *
		 * Subtle:
		 *
		 * Q: why don't we need tree lock here, looking for the right
		 * neighbor?
		 *
		 * A: even if value of node->real_node->right were changed
		 * during find_carry_node() execution, outcome of execution
		 * wouldn't change, because (in short) other thread cannot add
		 * elements to the @doing, and if node->real_node->right
		 * already was in @doing, value of node->real_node->right
		 * couldn't change, because node cannot be inserted between
		 * locked neighbors.
		 */
		if (find_carry_node(doing,
				    reiser4_carry_real(node)->right) != NULL) {
			read_unlock_tree(tree);
			/*
			 * What we are doing here (this is also applicable to
			 * the find_left_neighbor()).
			 *
			 * tree_walk.c code requires that insertion of a
			 * pointer to a child, modification of parent pointer
			 * in the child, and insertion of the child into
			 * sibling list are atomic (see
			 * plugin/item/internal.c:create_hook_internal()).
			 *
			 * carry allocates new node long before pointer to it
			 * is inserted into parent and, actually, long before
			 * parent is even known. Such allocated-but-orphaned
			 * nodes are only trackable through carry level lists.
			 *
			 * Situation that is handled here is following: @node
			 * has valid ->right pointer, but there is
			 * allocated-but-orphaned node in the carry queue that
			 * is logically between @node and @node->right. Here
			 * we are searching for it. Critical point is that
			 * this is only possible if @node->right is also in
			 * the carry queue (this is checked above), because
			 * this is the only way new orphaned node could be
			 * inserted between them (before inserting new node,
			 * make_space() first tries to shift to the right, so,
			 * right neighbor will be locked and queued).
			 *
			 */
			right = node;
			do {
				right = list_entry(right->header.level_linkage.next,
						   carry_node, header.level_linkage);
				assert("nikita-3408", !carry_node_end(doing,
								      right));
			} while (reiser4_carry_real(right) ==
				 reiser4_carry_real(node));
			return right;
		}
	}
	read_unlock_tree(tree);

	flags = GN_CAN_USE_UPPER_LEVELS;
	if (!(op->u.insert.flags & COPI_LOAD_RIGHT))
		flags = GN_NO_ALLOC;

	/* then, try to lock right neighbor */
	init_lh(&lh);
	result = reiser4_get_right_neighbor(&lh,
					    reiser4_carry_real(node),
					    ZNODE_WRITE_LOCK, flags);
	if (result == 0) {
		/* ok, node found and locked. */
		right = reiser4_add_carry_skip(doing, POOLO_AFTER, node);
		if (!IS_ERR(right)) {
			right->node = lh.node;
			move_lh(&right->lock_handle, &lh);
			right->free = 1;
			result = lock_carry_node_tail(right);
			if (result != 0)
				right = ERR_PTR(result);
		}
	} else if ((result == -E_NO_NEIGHBOR) || (result == -ENOENT)) {
		/* node is rightmost node in a tree, or neighbor wasn't in
		   cache, or there is an extent on the right. */
		right = NULL;
	} else
		right = ERR_PTR(result);
	done_lh(&lh);
	return right;
}

/* how much free space in a @node is needed for @op

   How much space in @node is required for completion of @op, where @op is
   insert or paste operation.
*/
static unsigned int space_needed_for_op(znode * node	/* znode data are
							 * inserted or
							 * pasted in */ ,
					carry_op * op	/* carry
							   operation */ )
{
	assert("nikita-919", op != NULL);

	switch (op->op) {
	default:
		impossible("nikita-1701", "Wrong opcode");
	case COP_INSERT:
		return space_needed(node, NULL, op->u.insert.d->data, 1);
	case COP_PASTE:
		return space_needed(node, op->u.insert.d->coord,
				    op->u.insert.d->data, 0);
	}
}

/* how much space in @node is required to insert or paste @data at
   @coord. */
unsigned int space_needed(const znode * node	/* node data are inserted or
						 * pasted in */ ,
			  const coord_t *coord	/* coord where data are
						 * inserted or pasted
						 * at */ ,
			  const reiser4_item_data * data /* data to insert or
							  * paste */ ,
			  int insertion/* non-0 is inserting, 0---paste */)
{
	int result;
	item_plugin *iplug;

	assert("nikita-917", node != NULL);
	assert("nikita-918", node_plugin_by_node(node) != NULL);
	assert("vs-230", !insertion || (coord == NULL));

	result = 0;
	iplug = data->iplug;
	if (iplug->b.estimate != NULL) {
		/* ask item plugin how much space is needed to insert this
		   item */
		result += iplug->b.estimate(insertion ? NULL : coord, data);
	} else {
		/* reasonable default */
		result += data->length;
	}
	if (insertion) {
		node_plugin *nplug;

		nplug = node->nplug;
		/* and add node overhead */
		if (nplug->item_overhead != NULL)
			result += nplug->item_overhead(node, NULL);
	}
	return result;
}

/* find &coord in parent where pointer to new child is to be stored. */
static int find_new_child_coord(carry_op * op	/* COP_INSERT carry operation to
						 * insert pointer to new
						 * child */ )
{
	int result;
	znode *node;
	znode *child;

	assert("nikita-941", op != NULL);
	assert("nikita-942", op->op == COP_INSERT);

	node = reiser4_carry_real(op->node);
	assert("nikita-943", node != NULL);
	assert("nikita-944", node_plugin_by_node(node) != NULL);

	child = reiser4_carry_real(op->u.insert.child);
	result =
	    find_new_child_ptr(node, child, op->u.insert.brother,
			       op->u.insert.d->coord);

	build_child_ptr_data(child, op->u.insert.d->data);
	return result;
}

/* additional amount of free space in @node required to complete @op */
static int free_space_shortage(znode * node /* node to check */ ,
			       carry_op * op/* operation being performed */)
{
	assert("nikita-1061", node != NULL);
	assert("nikita-1062", op != NULL);

	switch (op->op) {
	default:
		impossible("nikita-1702", "Wrong opcode");
	case COP_INSERT:
	case COP_PASTE:
		return space_needed_for_op(node, op) - znode_free_space(node);
	case COP_EXTENT:
		/* when inserting extent shift data around until insertion
		   point is utmost in the node. */
		if (coord_wrt(op->u.insert.d->coord) == COORD_INSIDE)
			return +1;
		else
			return -1;
	}
}

/* helper function: update node pointer in operation after insertion
   point was probably shifted into @target. */
static znode *sync_op(carry_op * op, carry_node * target)
{
	znode *insertion_node;

	/* reget node from coord: shift might move insertion coord to
	   the neighbor */
	insertion_node = op->u.insert.d->coord->node;
	/* if insertion point was actually moved into new node,
	   update carry node pointer in operation. */
	if (insertion_node != reiser4_carry_real(op->node)) {
		op->node = target;
		assert("nikita-2540",
		       reiser4_carry_real(target) == insertion_node);
	}
	assert("nikita-2541",
	       reiser4_carry_real(op->node) == op->u.insert.d->coord->node);
	return insertion_node;
}

/*
 * complete make_space() call: update tracked lock handle if necessary. See
 * comments for fs/reiser4/carry.h:carry_track_type
 */
static int
make_space_tail(carry_op * op, carry_level * doing, znode * orig_node)
{
	int result;
	carry_track_type tracking;
	znode *node;

	tracking = doing->track_type;
	node = op->u.insert.d->coord->node;

	if (tracking == CARRY_TRACK_NODE ||
	    (tracking == CARRY_TRACK_CHANGE && node != orig_node)) {
		/* inserting or pasting into node different from
		   original. Update lock handle supplied by caller. */
		assert("nikita-1417", doing->tracked != NULL);
		done_lh(doing->tracked);
		init_lh(doing->tracked);
		result = longterm_lock_znode(doing->tracked, node,
					     ZNODE_WRITE_LOCK,
					     ZNODE_LOCK_HIPRI);
	} else
		result = 0;
	return result;
}

/* This is insertion policy function. It shifts data to the left and right
   neighbors of insertion coord and allocates new nodes until there is enough
   free space to complete @op.

   See comments in the body.

   Assumes that the node format favors insertions at the right end of the node
   as node40 does.

   See carry_flow() on detail about flow insertion
*/
static int make_space(carry_op * op /* carry operation, insert or paste */ ,
		      carry_level * doing /* current carry queue */ ,
		      carry_level * todo/* carry queue on the parent level */)
{
	znode *node;
	int result;
	int not_enough_space;
	int blk_alloc;
	znode *orig_node;
	__u32 flags;

	coord_t *coord;

	assert("nikita-890", op != NULL);
	assert("nikita-891", todo != NULL);
	assert("nikita-892",
	       op->op == COP_INSERT ||
	       op->op == COP_PASTE || op->op == COP_EXTENT);
	assert("nikita-1607",
	       reiser4_carry_real(op->node) == op->u.insert.d->coord->node);

	flags = op->u.insert.flags;

	/* NOTE check that new node can only be allocated after checking left
	 * and right neighbors. This is necessary for proper work of
	 * find_{left,right}_neighbor(). */
	assert("nikita-3410", ergo(flags & COPI_DONT_ALLOCATE,
				   flags & COPI_DONT_SHIFT_LEFT));
	assert("nikita-3411", ergo(flags & COPI_DONT_ALLOCATE,
				   flags & COPI_DONT_SHIFT_RIGHT));

	coord = op->u.insert.d->coord;
	orig_node = node = coord->node;

	assert("nikita-908", node != NULL);
	assert("nikita-909", node_plugin_by_node(node) != NULL);

	result = 0;
	/* If there is not enough space in a node, try to shift something to
	   the left neighbor. This is a bit tricky, as locking to the left is
	   low priority. This is handled by restart logic in carry().
	 */
	not_enough_space = free_space_shortage(node, op);
	if (not_enough_space <= 0)
		/* it is possible that carry was called when there actually
		   was enough space in the node. For example, when inserting
		   leftmost item so that delimiting keys have to be updated.
		 */
		return make_space_tail(op, doing, orig_node);
	if (!(flags & COPI_DONT_SHIFT_LEFT)) {
		carry_node *left;
		/* make note in statistics of an attempt to move
		   something into the left neighbor */
		left = find_left_neighbor(op, doing);
		if (unlikely(IS_ERR(left))) {
			if (PTR_ERR(left) == -E_REPEAT)
				return -E_REPEAT;
			else {
				/* some error other than restart request
				   occurred. This shouldn't happen. Issue a
				   warning and continue as if left neighbor
				   weren't existing.
				 */
				warning("nikita-924",
					"Error accessing left neighbor: %li",
					PTR_ERR(left));
			}
		} else if (left != NULL) {

			/* shift everything possible on the left of and
			   including insertion coord into the left neighbor */
			result = carry_shift_data(LEFT_SIDE, coord,
						  reiser4_carry_real(left),
						  doing, todo,
						  flags & COPI_GO_LEFT);

			/* reget node from coord: shift_left() might move
			   insertion coord to the left neighbor */
			node = sync_op(op, left);

			not_enough_space = free_space_shortage(node, op);
			/* There is not enough free space in @node, but
			   may be, there is enough free space in
			   @left. Various balancing decisions are valid here.
			   The same for the shifiting to the right.
			 */
		}
	}
	/* If there still is not enough space, shift to the right */
	if (not_enough_space > 0 && !(flags & COPI_DONT_SHIFT_RIGHT)) {
		carry_node *right;

		right = find_right_neighbor(op, doing);
		if (IS_ERR(right)) {
			warning("nikita-1065",
				"Error accessing right neighbor: %li",
				PTR_ERR(right));
		} else if (right != NULL) {
			/* node containing insertion point, and its right
			   neighbor node are write locked by now.

			   shift everything possible on the right of but
			   excluding insertion coord into the right neighbor
			 */
			result = carry_shift_data(RIGHT_SIDE, coord,
						  reiser4_carry_real(right),
						  doing, todo,
						  flags & COPI_GO_RIGHT);
			/* reget node from coord: shift_right() might move
			   insertion coord to the right neighbor */
			node = sync_op(op, right);
			not_enough_space = free_space_shortage(node, op);
		}
	}
	/* If there is still not enough space, allocate new node(s).

	   We try to allocate new blocks if COPI_DONT_ALLOCATE is not set in
	   the carry operation flags (currently this is needed during flush
	   only).
	 */
	for (blk_alloc = 0;
	     not_enough_space > 0 && result == 0 && blk_alloc < 2 &&
	     !(flags & COPI_DONT_ALLOCATE); ++blk_alloc) {
		carry_node *fresh;	/* new node we are allocating */
		coord_t coord_shadow;	/* remembered insertion point before
					 * shifting data into new node */
		carry_node *node_shadow;	/* remembered insertion node
						 * before shifting */
		unsigned int gointo;	/* whether insertion point should move
					 * into newly allocated node */

		/* allocate new node on the right of @node. Znode and disk
		   fake block number for new node are allocated.

		   add_new_znode() posts carry operation COP_INSERT with
		   COPT_CHILD option to the parent level to add
		   pointer to newly created node to its parent.

		   Subtle point: if several new nodes are required to complete
		   insertion operation at this level, they will be inserted
		   into their parents in the order of creation, which means
		   that @node will be valid "cookie" at the time of insertion.

		 */
		fresh = add_new_znode(node, op->node, doing, todo);
		if (IS_ERR(fresh))
			return PTR_ERR(fresh);

		/* Try to shift into new node. */
		result = lock_carry_node(doing, fresh);
		zput(reiser4_carry_real(fresh));
		if (result != 0) {
			warning("nikita-947",
				"Cannot lock new node: %i", result);
			return result;
		}

		/* both nodes are write locked by now.

		   shift everything possible on the right of and
		   including insertion coord into the right neighbor.
		 */
		coord_dup(&coord_shadow, op->u.insert.d->coord);
		node_shadow = op->node;
		/* move insertion point into newly created node if:

		   . insertion point is rightmost in the source node, or
		   . this is not the first node we are allocating in a row.
		 */
		gointo =
		    (blk_alloc > 0) ||
		    coord_is_after_rightmost(op->u.insert.d->coord);

		if (gointo &&
		    op->op == COP_PASTE &&
		    coord_is_existing_item(op->u.insert.d->coord) &&
		    is_solid_item((item_plugin_by_coord(op->u.insert.d->coord)))) {
			/* paste into solid (atomic) item, which can contain
			   only one unit, so we need to shift it right, where
			   insertion point supposed to be */

			assert("edward-1444", op->u.insert.d->data->iplug ==
			       item_plugin_by_id(STATIC_STAT_DATA_ID));
			assert("edward-1445",
			       op->u.insert.d->data->length >
			       node_plugin_by_node(coord->node)->free_space
			       (coord->node));

			op->u.insert.d->coord->between = BEFORE_UNIT;
		}

		result = carry_shift_data(RIGHT_SIDE, coord,
					  reiser4_carry_real(fresh),
					  doing, todo, gointo);
		/* if insertion point was actually moved into new node,
		   update carry node pointer in operation. */
		node = sync_op(op, fresh);
		not_enough_space = free_space_shortage(node, op);
		if ((not_enough_space > 0) && (node != coord_shadow.node)) {
			/* there is not enough free in new node. Shift
			   insertion point back to the @shadow_node so that
			   next new node would be inserted between
			   @shadow_node and @fresh.
			 */
			coord_normalize(&coord_shadow);
			coord_dup(coord, &coord_shadow);
			node = coord->node;
			op->node = node_shadow;
			if (1 || (flags & COPI_STEP_BACK)) {
				/* still not enough space?! Maybe there is
				   enough space in the source node (i.e., node
				   data are moved from) now.
				 */
				not_enough_space =
				    free_space_shortage(node, op);
			}
		}
	}
	if (not_enough_space > 0) {
		if (!(flags & COPI_DONT_ALLOCATE))
			warning("nikita-948", "Cannot insert new item");
		result = -E_NODE_FULL;
	}
	assert("nikita-1622", ergo(result == 0,
				  reiser4_carry_real(op->node) == coord->node));
	assert("nikita-2616", coord == op->u.insert.d->coord);
	if (result == 0)
		result = make_space_tail(op, doing, orig_node);
	return result;
}

/* insert_paste_common() - common part of insert and paste operations

   This function performs common part of COP_INSERT and COP_PASTE.

   There are two ways in which insertion/paste can be requested:

    . by directly supplying reiser4_item_data. In this case, op ->
    u.insert.type is set to COPT_ITEM_DATA.

    . by supplying child pointer to which is to inserted into parent. In this
    case op -> u.insert.type == COPT_CHILD.

    . by supplying key of new item/unit. This is currently only used during
    extent insertion

   This is required, because when new node is allocated we don't know at what
   position pointer to it is to be stored in the parent. Actually, we don't
   even know what its parent will be, because parent can be re-balanced
   concurrently and new node re-parented, and because parent can be full and
   pointer to the new node will go into some other node.

   insert_paste_common() resolves pointer to child node into position in the
   parent by calling find_new_child_coord(), that fills
   reiser4_item_data. After this, insertion/paste proceeds uniformly.

   Another complication is with finding free space during pasting. It may
   happen that while shifting items to the neighbors and newly allocated
   nodes, insertion coord can no longer be in the item we wanted to paste
   into. At this point, paste becomes (morphs) into insert. Moreover free
   space analysis has to be repeated, because amount of space required for
   insertion is different from that of paste (item header overhead, etc).

   This function "unifies" different insertion modes (by resolving child
   pointer or key into insertion coord), and then calls make_space() to free
   enough space in the node by shifting data to the left and right and by
   allocating new nodes if necessary. Carry operation knows amount of space
   required for its completion. After enough free space is obtained, caller of
   this function (carry_{insert,paste,etc.}) performs actual insertion/paste
   by calling item plugin method.

*/
static int insert_paste_common(carry_op * op	/* carry operation being
						 * performed */ ,
			       carry_level * doing /* current carry level */ ,
			       carry_level * todo /* next carry level */ ,
			       carry_insert_data * cdata	/* pointer to
								 * cdata */ ,
			       coord_t *coord /* insertion/paste coord */ ,
			       reiser4_item_data * data	/* data to be
							 * inserted/pasted */ )
{
	assert("nikita-981", op != NULL);
	assert("nikita-980", todo != NULL);
	assert("nikita-979", (op->op == COP_INSERT) || (op->op == COP_PASTE)
	       || (op->op == COP_EXTENT));

	if (op->u.insert.type == COPT_PASTE_RESTARTED) {
		/* nothing to do. Fall through to make_space(). */
		;
	} else if (op->u.insert.type == COPT_KEY) {
		node_search_result intra_node;
		znode *node;
		/* Problem with doing batching at the lowest level, is that
		   operations here are given by coords where modification is
		   to be performed, and one modification can invalidate coords
		   of all following operations.

		   So, we are implementing yet another type for operation that
		   will use (the only) "locator" stable across shifting of
		   data between nodes, etc.: key (COPT_KEY).

		   This clause resolves key to the coord in the node.

		   But node can change also. Probably some pieces have to be
		   added to the lock_carry_node(), to lock node by its key.

		 */
		/* NOTE-NIKITA Lookup bias is fixed to FIND_EXACT. Complain
		   if you need something else. */
		op->u.insert.d->coord = coord;
		node = reiser4_carry_real(op->node);
		intra_node = node_plugin_by_node(node)->lookup
		    (node, op->u.insert.d->key, FIND_EXACT,
		     op->u.insert.d->coord);
		if ((intra_node != NS_FOUND) && (intra_node != NS_NOT_FOUND)) {
			warning("nikita-1715", "Intra node lookup failure: %i",
				intra_node);
			return intra_node;
		}
	} else if (op->u.insert.type == COPT_CHILD) {
		/* if we are asked to insert pointer to the child into
		   internal node, first convert pointer to the child into
		   coord within parent node.
		 */
		znode *child;
		int result;

		op->u.insert.d = cdata;
		op->u.insert.d->coord = coord;
		op->u.insert.d->data = data;
		op->u.insert.d->coord->node = reiser4_carry_real(op->node);
		result = find_new_child_coord(op);
		child = reiser4_carry_real(op->u.insert.child);
		if (result != NS_NOT_FOUND) {
			warning("nikita-993",
				"Cannot find a place for child pointer: %i",
				result);
			return result;
		}
		/* This only happens when we did multiple insertions at
		   the previous level, trying to insert single item and
		   it so happened, that insertion of pointers to all new
		   nodes before this one already caused parent node to
		   split (may be several times).

		   I am going to come up with better solution.

		   You are not expected to understand this.
		   -- v6root/usr/sys/ken/slp.c

		   Basically, what happens here is the following: carry came
		   to the parent level and is about to insert internal item
		   pointing to the child node that it just inserted in the
		   level below. Position where internal item is to be inserted
		   was found by find_new_child_coord() above, but node of the
		   current carry operation (that is, parent node of child
		   inserted on the previous level), was determined earlier in
		   the lock_carry_level/lock_carry_node. It could so happen
		   that other carry operations already performed on the parent
		   level already split parent node, so that insertion point
		   moved into another node. Handle this by creating new carry
		   node for insertion point if necessary.
		 */
		if (reiser4_carry_real(op->node) !=
		    op->u.insert.d->coord->node) {
			pool_ordering direction;
			znode *z1;
			znode *z2;
			reiser4_key k1;
			reiser4_key k2;

			/*
			 * determine in what direction insertion point
			 * moved. Do this by comparing delimiting keys.
			 */
			z1 = op->u.insert.d->coord->node;
			z2 = reiser4_carry_real(op->node);
			if (keyle(leftmost_key_in_node(z1, &k1),
				  leftmost_key_in_node(z2, &k2)))
				/* insertion point moved to the left */
				direction = POOLO_BEFORE;
			else
				/* insertion point moved to the right */
				direction = POOLO_AFTER;

			op->node = reiser4_add_carry_skip(doing,
							  direction, op->node);
			if (IS_ERR(op->node))
				return PTR_ERR(op->node);
			op->node->node = op->u.insert.d->coord->node;
			op->node->free = 1;
			result = lock_carry_node(doing, op->node);
			if (result != 0)
				return result;
		}

		/*
		 * set up key of an item being inserted: we are inserting
		 * internal item and its key is (by the very definition of
		 * search tree) is leftmost key in the child node.
		 */
		write_lock_dk(znode_get_tree(child));
		op->u.insert.d->key = leftmost_key_in_node(child,
							   znode_get_ld_key(child));
		write_unlock_dk(znode_get_tree(child));
		op->u.insert.d->data->arg = op->u.insert.brother;
	} else {
		assert("vs-243", op->u.insert.d->coord != NULL);
		op->u.insert.d->coord->node = reiser4_carry_real(op->node);
	}

	/* find free space. */
	return make_space(op, doing, todo);
}

/* handle carry COP_INSERT operation.

   Insert new item into node. New item can be given in one of two ways:

   - by passing &tree_coord and &reiser4_item_data as part of @op. This is
   only applicable at the leaf/twig level.

   - by passing a child node pointer to which is to be inserted by this
   operation.

*/
static int carry_insert(carry_op * op /* operation to perform */ ,
			carry_level * doing	/* queue of operations @op
						 * is part of */ ,
			carry_level * todo	/* queue where new operations
						 * are accumulated */ )
{
	znode *node;
	carry_insert_data cdata;
	coord_t coord;
	reiser4_item_data data;
	carry_plugin_info info;
	int result;

	assert("nikita-1036", op != NULL);
	assert("nikita-1037", todo != NULL);
	assert("nikita-1038", op->op == COP_INSERT);

	coord_init_zero(&coord);

	/* perform common functionality of insert and paste. */
	result = insert_paste_common(op, doing, todo, &cdata, &coord, &data);
	if (result != 0)
		return result;

	node = op->u.insert.d->coord->node;
	assert("nikita-1039", node != NULL);
	assert("nikita-1040", node_plugin_by_node(node) != NULL);

	assert("nikita-949",
	       space_needed_for_op(node, op) <= znode_free_space(node));

	/* ask node layout to create new item. */
	info.doing = doing;
	info.todo = todo;
	result = node_plugin_by_node(node)->create_item
	    (op->u.insert.d->coord, op->u.insert.d->key, op->u.insert.d->data,
	     &info);
	doing->restartable = 0;
	znode_make_dirty(node);

	return result;
}

/*
 * Flow insertion code. COP_INSERT_FLOW is special tree operation that is
 * supplied with a "flow" (that is, a stream of data) and inserts it into tree
 * by slicing into multiple items.
 */

#define flow_insert_point(op) ((op)->u.insert_flow.insert_point)
#define flow_insert_flow(op) ((op)->u.insert_flow.flow)
#define flow_insert_data(op) ((op)->u.insert_flow.data)

static size_t item_data_overhead(carry_op * op)
{
	if (flow_insert_data(op)->iplug->b.estimate == NULL)
		return 0;
	return (flow_insert_data(op)->iplug->b.
		estimate(NULL /* estimate insertion */ , flow_insert_data(op)) -
		flow_insert_data(op)->length);
}

/* FIXME-VS: this is called several times during one make_flow_for_insertion
   and it will always return the same result. Some optimization could be made
   by calculating this value once at the beginning and passing it around. That
   would reduce some flexibility in future changes
*/
static int can_paste(coord_t *, const reiser4_key *, const reiser4_item_data *);
static size_t flow_insertion_overhead(carry_op * op)
{
	znode *node;
	size_t insertion_overhead;

	node = flow_insert_point(op)->node;
	insertion_overhead = 0;
	if (node->nplug->item_overhead &&
	    !can_paste(flow_insert_point(op), &flow_insert_flow(op)->key,
		       flow_insert_data(op)))
		insertion_overhead =
		    node->nplug->item_overhead(node, NULL) +
			item_data_overhead(op);
	return insertion_overhead;
}

/* how many bytes of flow does fit to the node */
static int what_can_fit_into_node(carry_op * op)
{
	size_t free, overhead;

	overhead = flow_insertion_overhead(op);
	free = znode_free_space(flow_insert_point(op)->node);
	if (free <= overhead)
		return 0;
	free -= overhead;
	/* FIXME: flow->length is loff_t only to not get overflowed in case of
	   expandign truncate */
	if (free < op->u.insert_flow.flow->length)
		return free;
	return (int)op->u.insert_flow.flow->length;
}

/* in make_space_for_flow_insertion we need to check either whether whole flow
   fits into a node or whether minimal fraction of flow fits into a node */
static int enough_space_for_whole_flow(carry_op * op)
{
	return (unsigned)what_can_fit_into_node(op) ==
	    op->u.insert_flow.flow->length;
}

#define MIN_FLOW_FRACTION 1
static int enough_space_for_min_flow_fraction(carry_op * op)
{
	//assert("vs-902", coord_is_after_rightmost(flow_insert_point(op)));

	return what_can_fit_into_node(op) >= MIN_FLOW_FRACTION;
}

/* this returns 0 if left neighbor was obtained successfully and everything
   upto insertion point including it were shifted and left neighbor still has
   some free space to put minimal fraction of flow into it */
static int
make_space_by_shift_left(carry_op * op, carry_level * doing, carry_level * todo)
{
	carry_node *left;
	znode *orig;

	left = find_left_neighbor(op, doing);
	if (unlikely(IS_ERR(left))) {
		warning("vs-899",
			"make_space_by_shift_left: "
			"error accessing left neighbor: %li", PTR_ERR(left));
		return 1;
	}
	if (left == NULL)
		/* left neighbor either does not exist or is unformatted
		   node */
		return 1;

	orig = flow_insert_point(op)->node;
	/* try to shift content of node @orig from its head upto insert point
	   including insertion point into the left neighbor */
	carry_shift_data(LEFT_SIDE, flow_insert_point(op),
			 reiser4_carry_real(left), doing, todo,
			 1/* including insert point */);
	if (reiser4_carry_real(left) != flow_insert_point(op)->node) {
		/* insertion point did not move */
		return 1;
	}

	/* insertion point is set after last item in the node */
	assert("vs-900", coord_is_after_rightmost(flow_insert_point(op)));

	if (!enough_space_for_min_flow_fraction(op)) {
		/* insertion point node does not have enough free space to put
		   even minimal portion of flow into it, therefore, move
		   insertion point back to orig node (before first item) */
		coord_init_before_first_item(flow_insert_point(op), orig);
		return 1;
	}

	/* part of flow is to be written to the end of node */
	op->node = left;
	return 0;
}

/* this returns 0 if right neighbor was obtained successfully and everything to
   the right of insertion point was shifted to it and node got enough free
   space to put minimal fraction of flow into it */
static int
make_space_by_shift_right(carry_op * op, carry_level * doing,
			  carry_level * todo)
{
	carry_node *right;

	right = find_right_neighbor(op, doing);
	if (unlikely(IS_ERR(right))) {
		warning("nikita-1065", "shift_right_excluding_insert_point: "
			"error accessing right neighbor: %li", PTR_ERR(right));
		return 1;
	}
	if (right) {
		/* shift everything possible on the right of but excluding
		   insertion coord into the right neighbor */
		carry_shift_data(RIGHT_SIDE, flow_insert_point(op),
				 reiser4_carry_real(right), doing, todo,
				 0/* not including insert point */);
	} else {
		/* right neighbor either does not exist or is unformatted
		   node */
		;
	}
	if (coord_is_after_rightmost(flow_insert_point(op))) {
		if (enough_space_for_min_flow_fraction(op)) {
			/* part of flow is to be written to the end of node */
			return 0;
		}
	}

	/* new node is to be added if insert point node did not get enough
	   space for whole flow */
	return 1;
}

/* this returns 0 when insert coord is set at the node end and fraction of flow
   fits into that node */
static int
make_space_by_new_nodes(carry_op * op, carry_level * doing, carry_level * todo)
{
	int result;
	znode *node;
	carry_node *new;

	node = flow_insert_point(op)->node;

	if (op->u.insert_flow.new_nodes == CARRY_FLOW_NEW_NODES_LIMIT)
		return RETERR(-E_NODE_FULL);
	/* add new node after insert point node */
	new = add_new_znode(node, op->node, doing, todo);
	if (unlikely(IS_ERR(new)))
		return PTR_ERR(new);
	result = lock_carry_node(doing, new);
	zput(reiser4_carry_real(new));
	if (unlikely(result))
		return result;
	op->u.insert_flow.new_nodes++;
	if (!coord_is_after_rightmost(flow_insert_point(op))) {
		carry_shift_data(RIGHT_SIDE, flow_insert_point(op),
				 reiser4_carry_real(new), doing, todo,
				 0/* not including insert point */);
		assert("vs-901",
		       coord_is_after_rightmost(flow_insert_point(op)));

		if (enough_space_for_min_flow_fraction(op))
			return 0;
		if (op->u.insert_flow.new_nodes == CARRY_FLOW_NEW_NODES_LIMIT)
			return RETERR(-E_NODE_FULL);

		/* add one more new node */
		new = add_new_znode(node, op->node, doing, todo);
		if (unlikely(IS_ERR(new)))
			return PTR_ERR(new);
		result = lock_carry_node(doing, new);
		zput(reiser4_carry_real(new));
		if (unlikely(result))
			return result;
		op->u.insert_flow.new_nodes++;
	}

	/* move insertion point to new node */
	coord_init_before_first_item(flow_insert_point(op),
				     reiser4_carry_real(new));
	op->node = new;
	return 0;
}

static int
make_space_for_flow_insertion(carry_op * op, carry_level * doing,
			      carry_level * todo)
{
	__u32 flags = op->u.insert_flow.flags;

	if (enough_space_for_whole_flow(op)) {
		/* whole flow fits into insert point node */
		return 0;
	}
	if ((flags & COPI_SWEEP) &&
	    enough_space_for_min_flow_fraction(op))
		/* use the rest of space in the current node */
		return 0;

	if (!(flags & COPI_DONT_SHIFT_LEFT)
	    && (make_space_by_shift_left(op, doing, todo) == 0)) {
		/* insert point is shifted to left neighbor of original insert
		   point node and is set after last unit in that node. It has
		   enough space to fit at least minimal fraction of flow. */
		return 0;
	}

	if (enough_space_for_whole_flow(op)) {
		/* whole flow fits into insert point node */
		return 0;
	}

	if (!(flags & COPI_DONT_SHIFT_RIGHT)
	    && (make_space_by_shift_right(op, doing, todo) == 0)) {
		/* insert point is still set to the same node, but there is
		   nothing to the right of insert point. */
		return 0;
	}

	if (enough_space_for_whole_flow(op)) {
		/* whole flow fits into insert point node */
		return 0;
	}

	return make_space_by_new_nodes(op, doing, todo);
}

/* implements COP_INSERT_FLOW operation */
static int
carry_insert_flow(carry_op * op, carry_level * doing, carry_level * todo)
{
	int result;
	flow_t *f;
	coord_t *insert_point;
	node_plugin *nplug;
	carry_plugin_info info;
	znode *orig_node;
	lock_handle *orig_lh;

	f = op->u.insert_flow.flow;
	result = 0;

	/* carry system needs this to work */
	info.doing = doing;
	info.todo = todo;

	orig_node = flow_insert_point(op)->node;
	orig_lh = doing->tracked;

	while (f->length) {
		result = make_space_for_flow_insertion(op, doing, todo);
		if (result)
			break;

		insert_point = flow_insert_point(op);
		nplug = node_plugin_by_node(insert_point->node);

		/* compose item data for insertion/pasting */
		flow_insert_data(op)->data = f->data;
		flow_insert_data(op)->length = what_can_fit_into_node(op);

		if (can_paste(insert_point, &f->key, flow_insert_data(op))) {
			/* insert point is set to item of file we are writing to
			   and we have to append to it */
			assert("vs-903", insert_point->between == AFTER_UNIT);
			nplug->change_item_size(insert_point,
						flow_insert_data(op)->length);
			flow_insert_data(op)->iplug->b.paste(insert_point,
							     flow_insert_data
							     (op), &info);
		} else {
			/* new item must be inserted */
			pos_in_node_t new_pos;
			flow_insert_data(op)->length += item_data_overhead(op);

			/* FIXME-VS: this is because node40_create_item changes
			   insert_point for obscure reasons */
			switch (insert_point->between) {
			case AFTER_ITEM:
				new_pos = insert_point->item_pos + 1;
				break;
			case EMPTY_NODE:
				new_pos = 0;
				break;
			case BEFORE_ITEM:
				assert("vs-905", insert_point->item_pos == 0);
				new_pos = 0;
				break;
			default:
				impossible("vs-906",
					   "carry_insert_flow: invalid coord");
				new_pos = 0;
				break;
			}

			nplug->create_item(insert_point, &f->key,
					   flow_insert_data(op), &info);
			coord_set_item_pos(insert_point, new_pos);
		}
		coord_init_after_item_end(insert_point);
		doing->restartable = 0;
		znode_make_dirty(insert_point->node);

		move_flow_forward(f, (unsigned)flow_insert_data(op)->length);
	}

	if (orig_node != flow_insert_point(op)->node) {
		/* move lock to new insert point */
		done_lh(orig_lh);
		init_lh(orig_lh);
		result =
		    longterm_lock_znode(orig_lh, flow_insert_point(op)->node,
					ZNODE_WRITE_LOCK, ZNODE_LOCK_HIPRI);
	}

	return result;
}

/* implements COP_DELETE operation

   Remove pointer to @op -> u.delete.child from it's parent.

   This function also handles killing of a tree root is last pointer from it
   was removed. This is complicated by our handling of "twig" level: root on
   twig level is never killed.

*/
static int carry_delete(carry_op * op /* operation to be performed */ ,
			carry_level * doing UNUSED_ARG	/* current carry
							 * level */ ,
			carry_level * todo/* next carry level */)
{
	int result;
	coord_t coord;
	coord_t coord2;
	znode *parent;
	znode *child;
	carry_plugin_info info;
	reiser4_tree *tree;

	/*
	 * This operation is called to delete internal item pointing to the
	 * child node that was removed by carry from the tree on the previous
	 * tree level.
	 */

	assert("nikita-893", op != NULL);
	assert("nikita-894", todo != NULL);
	assert("nikita-895", op->op == COP_DELETE);

	coord_init_zero(&coord);
	coord_init_zero(&coord2);

	parent = reiser4_carry_real(op->node);
	child = op->u.delete.child ?
		reiser4_carry_real(op->u.delete.child) : op->node->node;
	tree = znode_get_tree(child);
	read_lock_tree(tree);

	/*
	 * @parent was determined when carry entered parent level
	 * (lock_carry_level/lock_carry_node). Since then, actual parent of
	 * @child node could change due to other carry operations performed on
	 * the parent level. Check for this.
	 */

	if (znode_parent(child) != parent) {
		/* NOTE-NIKITA add stat counter for this. */
		parent = znode_parent(child);
		assert("nikita-2581", find_carry_node(doing, parent));
	}
	read_unlock_tree(tree);

	assert("nikita-1213", znode_get_level(parent) > LEAF_LEVEL);

	/* Twig level horrors: tree should be of height at least 2. So, last
	   pointer from the root at twig level is preserved even if child is
	   empty. This is ugly, but so it was architectured.
	 */

	if (znode_is_root(parent) &&
	    znode_get_level(parent) <= REISER4_MIN_TREE_HEIGHT &&
	    node_num_items(parent) == 1) {
		/* Delimiting key manipulations. */
		write_lock_dk(tree);
		znode_set_ld_key(child, znode_set_ld_key(parent, reiser4_min_key()));
		znode_set_rd_key(child, znode_set_rd_key(parent, reiser4_max_key()));
		ZF_SET(child, JNODE_DKSET);
		write_unlock_dk(tree);

		/* @child escaped imminent death! */
		ZF_CLR(child, JNODE_HEARD_BANSHEE);
		return 0;
	}

	/* convert child pointer to the coord_t */
	result = find_child_ptr(parent, child, &coord);
	if (result != NS_FOUND) {
		warning("nikita-994", "Cannot find child pointer: %i", result);
		print_coord_content("coord", &coord);
		return result;
	}

	coord_dup(&coord2, &coord);
	info.doing = doing;
	info.todo = todo;
	{
		/*
		 * Actually kill internal item: prepare structure with
		 * arguments for ->cut_and_kill() method...
		 */

		struct carry_kill_data kdata;
		kdata.params.from = &coord;
		kdata.params.to = &coord2;
		kdata.params.from_key = NULL;
		kdata.params.to_key = NULL;
		kdata.params.smallest_removed = NULL;
		kdata.params.truncate = 1;
		kdata.flags = op->u.delete.flags;
		kdata.inode = NULL;
		kdata.left = NULL;
		kdata.right = NULL;
		kdata.buf = NULL;
		/* ... and call it. */
		result = node_plugin_by_node(parent)->cut_and_kill(&kdata,
								   &info);
	}
	doing->restartable = 0;

	/* check whether root should be killed violently */
	if (znode_is_root(parent) &&
	    /* don't kill roots at and lower than twig level */
	    znode_get_level(parent) > REISER4_MIN_TREE_HEIGHT &&
	    node_num_items(parent) == 1)
		result = reiser4_kill_tree_root(coord.node);

	return result < 0 ? result : 0;
}

/* implements COP_CUT opration

   Cuts part or whole content of node.

*/
static int carry_cut(carry_op * op /* operation to be performed */ ,
		     carry_level * doing /* current carry level */ ,
		     carry_level * todo/* next carry level */)
{
	int result;
	carry_plugin_info info;
	node_plugin *nplug;

	assert("nikita-896", op != NULL);
	assert("nikita-897", todo != NULL);
	assert("nikita-898", op->op == COP_CUT);

	info.doing = doing;
	info.todo = todo;

	nplug = node_plugin_by_node(reiser4_carry_real(op->node));
	if (op->u.cut_or_kill.is_cut)
		result = nplug->cut(op->u.cut_or_kill.u.cut, &info);
	else
		result = nplug->cut_and_kill(op->u.cut_or_kill.u.kill, &info);

	doing->restartable = 0;
	return result < 0 ? result : 0;
}

/* helper function for carry_paste(): returns true if @op can be continued as
   paste  */
static int
can_paste(coord_t *icoord, const reiser4_key * key,
	  const reiser4_item_data * data)
{
	coord_t circa;
	item_plugin *new_iplug;
	item_plugin *old_iplug;
	int result = 0;		/* to keep gcc shut */

	assert("", icoord->between != AT_UNIT);

	/* obviously, one cannot paste when node is empty---there is nothing
	   to paste into. */
	if (node_is_empty(icoord->node))
		return 0;
	/* if insertion point is at the middle of the item, then paste */
	if (!coord_is_between_items(icoord))
		return 1;
	coord_dup(&circa, icoord);
	circa.between = AT_UNIT;

	old_iplug = item_plugin_by_coord(&circa);
	new_iplug = data->iplug;

	/* check whether we can paste to the item @icoord is "at" when we
	   ignore ->between field */
	if (old_iplug == new_iplug && item_can_contain_key(&circa, key, data))
		result = 1;
	else if (icoord->between == BEFORE_UNIT
		   || icoord->between == BEFORE_ITEM) {
		/* otherwise, try to glue to the item at the left, if any */
		coord_dup(&circa, icoord);
		if (coord_set_to_left(&circa)) {
			result = 0;
			coord_init_before_item(icoord);
		} else {
			old_iplug = item_plugin_by_coord(&circa);
			result = (old_iplug == new_iplug)
			    && item_can_contain_key(icoord, key, data);
			if (result) {
				coord_dup(icoord, &circa);
				icoord->between = AFTER_UNIT;
			}
		}
	} else if (icoord->between == AFTER_UNIT
		   || icoord->between == AFTER_ITEM) {
		coord_dup(&circa, icoord);
		/* otherwise, try to glue to the item at the right, if any */
		if (coord_set_to_right(&circa)) {
			result = 0;
			coord_init_after_item(icoord);
		} else {
			int (*cck) (const coord_t *, const reiser4_key *,
				    const reiser4_item_data *);

			old_iplug = item_plugin_by_coord(&circa);

			cck = old_iplug->b.can_contain_key;
			if (cck == NULL)
				/* item doesn't define ->can_contain_key
				   method? So it is not expandable. */
				result = 0;
			else {
				result = (old_iplug == new_iplug)
				    && cck(&circa /*icoord */ , key, data);
				if (result) {
					coord_dup(icoord, &circa);
					icoord->between = BEFORE_UNIT;
				}
			}
		}
	} else
		impossible("nikita-2513", "Nothing works");
	if (result) {
		if (icoord->between == BEFORE_ITEM) {
			assert("vs-912", icoord->unit_pos == 0);
			icoord->between = BEFORE_UNIT;
		} else if (icoord->between == AFTER_ITEM) {
			coord_init_after_item_end(icoord);
		}
	}
	return result;
}

/* implements COP_PASTE operation

   Paste data into existing item. This is complicated by the fact that after
   we shifted something to the left or right neighbors trying to free some
   space, item we were supposed to paste into can be in different node than
   insertion coord. If so, we are no longer doing paste, but insert. See
   comments in insert_paste_common().

*/
static int carry_paste(carry_op * op /* operation to be performed */ ,
		       carry_level * doing UNUSED_ARG	/* current carry
							 * level */ ,
		       carry_level * todo/* next carry level */)
{
	znode *node;
	carry_insert_data cdata;
	coord_t dcoord;
	reiser4_item_data data;
	int result;
	int real_size;
	item_plugin *iplug;
	carry_plugin_info info;
	coord_t *coord;

	assert("nikita-982", op != NULL);
	assert("nikita-983", todo != NULL);
	assert("nikita-984", op->op == COP_PASTE);

	coord_init_zero(&dcoord);

	result = insert_paste_common(op, doing, todo, &cdata, &dcoord, &data);
	if (result != 0)
		return result;

	coord = op->u.insert.d->coord;

	/* handle case when op -> u.insert.coord doesn't point to the item
	   of required type. restart as insert. */
	if (!can_paste(coord, op->u.insert.d->key, op->u.insert.d->data)) {
		op->op = COP_INSERT;
		op->u.insert.type = COPT_PASTE_RESTARTED;
		result = op_dispatch_table[COP_INSERT].handler(op, doing, todo);

		return result;
	}

	node = coord->node;
	iplug = item_plugin_by_coord(coord);
	assert("nikita-992", iplug != NULL);

	assert("nikita-985", node != NULL);
	assert("nikita-986", node_plugin_by_node(node) != NULL);

	assert("nikita-987",
	       space_needed_for_op(node, op) <= znode_free_space(node));

	assert("nikita-1286", coord_is_existing_item(coord));

	/*
	 * if item is expanded as a result of this operation, we should first
	 * change item size, than call ->b.paste item method. If item is
	 * shrunk, it should be done other way around: first call ->b.paste
	 * method, then reduce item size.
	 */

	real_size = space_needed_for_op(node, op);
	if (real_size > 0)
		node->nplug->change_item_size(coord, real_size);

	doing->restartable = 0;
	info.doing = doing;
	info.todo = todo;

	result = iplug->b.paste(coord, op->u.insert.d->data, &info);

	if (real_size < 0)
		node->nplug->change_item_size(coord, real_size);

	/* if we pasted at the beginning of the item, update item's key. */
	if (coord->unit_pos == 0 && coord->between != AFTER_UNIT)
		node->nplug->update_item_key(coord, op->u.insert.d->key, &info);

	znode_make_dirty(node);
	return result;
}

/* handle carry COP_EXTENT operation. */
static int carry_extent(carry_op * op /* operation to perform */ ,
			carry_level * doing	/* queue of operations @op
						 * is part of */ ,
			carry_level * todo	/* queue where new operations
						 * are accumulated */ )
{
	znode *node;
	carry_insert_data cdata;
	coord_t coord;
	reiser4_item_data data;
	carry_op *delete_dummy;
	carry_op *insert_extent;
	int result;
	carry_plugin_info info;

	assert("nikita-1751", op != NULL);
	assert("nikita-1752", todo != NULL);
	assert("nikita-1753", op->op == COP_EXTENT);

	/* extent insertion overview:

	   extents live on the TWIG LEVEL, which is level one above the leaf
	   one. This complicates extent insertion logic somewhat: it may
	   happen (and going to happen all the time) that in logical key
	   ordering extent has to be placed between items I1 and I2, located
	   at the leaf level, but I1 and I2 are in the same formatted leaf
	   node N1. To insert extent one has to

	   (1) reach node N1 and shift data between N1, its neighbors and
	   possibly newly allocated nodes until I1 and I2 fall into different
	   nodes. Since I1 and I2 are still neighboring items in logical key
	   order, they will be necessary utmost items in their respective
	   nodes.

	   (2) After this new extent item is inserted into node on the twig
	   level.

	   Fortunately this process can reuse almost all code from standard
	   insertion procedure (viz. make_space() and insert_paste_common()),
	   due to the following observation: make_space() only shifts data up
	   to and excluding or including insertion point. It never
	   "over-moves" through insertion point. Thus, one can use
	   make_space() to perform step (1). All required for this is just to
	   instruct free_space_shortage() to keep make_space() shifting data
	   until insertion point is at the node border.

	 */

	/* perform common functionality of insert and paste. */
	result = insert_paste_common(op, doing, todo, &cdata, &coord, &data);
	if (result != 0)
		return result;

	node = op->u.extent.d->coord->node;
	assert("nikita-1754", node != NULL);
	assert("nikita-1755", node_plugin_by_node(node) != NULL);
	assert("nikita-1700", coord_wrt(op->u.extent.d->coord) != COORD_INSIDE);

	/* NOTE-NIKITA add some checks here. Not assertions, -EIO. Check that
	   extent fits between items. */

	info.doing = doing;
	info.todo = todo;

	/* there is another complication due to placement of extents on the
	   twig level: extents are "rigid" in the sense that key-range
	   occupied by extent cannot grow indefinitely to the right as it is
	   for the formatted leaf nodes. Because of this when search finds two
	   adjacent extents on the twig level, it has to "drill" to the leaf
	   level, creating new node. Here we are removing this node.
	 */
	if (node_is_empty(node)) {
		delete_dummy = node_post_carry(&info, COP_DELETE, node, 1);
		if (IS_ERR(delete_dummy))
			return PTR_ERR(delete_dummy);
		delete_dummy->u.delete.child = NULL;
		delete_dummy->u.delete.flags = DELETE_RETAIN_EMPTY;
		ZF_SET(node, JNODE_HEARD_BANSHEE);
	}

	/* proceed with inserting extent item into parent. We are definitely
	   inserting rather than pasting if we get that far. */
	insert_extent = node_post_carry(&info, COP_INSERT, node, 1);
	if (IS_ERR(insert_extent))
		/* @delete_dummy will be automatically destroyed on the level
		   exiting  */
		return PTR_ERR(insert_extent);
	/* NOTE-NIKITA insertion by key is simplest option here. Another
	   possibility is to insert on the left or right of already existing
	   item.
	 */
	insert_extent->u.insert.type = COPT_KEY;
	insert_extent->u.insert.d = op->u.extent.d;
	assert("nikita-1719", op->u.extent.d->key != NULL);
	insert_extent->u.insert.d->data->arg = op->u.extent.d->coord;
	insert_extent->u.insert.flags =
	    znode_get_tree(node)->carry.new_extent_flags;

	/*
	 * if carry was asked to track lock handle we should actually track
	 * lock handle on the twig node rather than on the leaf where
	 * operation was started from. Transfer tracked lock handle.
	 */
	if (doing->track_type) {
		assert("nikita-3242", doing->tracked != NULL);
		assert("nikita-3244", todo->tracked == NULL);
		todo->tracked = doing->tracked;
		todo->track_type = CARRY_TRACK_NODE;
		doing->tracked = NULL;
		doing->track_type = 0;
	}

	return 0;
}

/* update key in @parent between pointers to @left and @right.

   Find coords of @left and @right and update delimiting key between them.
   This is helper function called by carry_update(). Finds position of
   internal item involved. Updates item key. Updates delimiting keys of child
   nodes involved.
*/
static int update_delimiting_key(znode * parent	/* node key is updated
						 * in */ ,
				 znode * left /* child of @parent */ ,
				 znode * right /* child of @parent */ ,
				 carry_level * doing	/* current carry
							 * level */ ,
				 carry_level * todo	/* parent carry
							 * level */ ,
				 const char **error_msg	/* place to
							 * store error
							 * message */ )
{
	coord_t left_pos;
	coord_t right_pos;
	int result;
	reiser4_key ldkey;
	carry_plugin_info info;

	assert("nikita-1177", right != NULL);
	/* find position of right left child in a parent */
	result = find_child_ptr(parent, right, &right_pos);
	if (result != NS_FOUND) {
		*error_msg = "Cannot find position of right child";
		return result;
	}

	if ((left != NULL) && !coord_is_leftmost_unit(&right_pos)) {
		/* find position of the left child in a parent */
		result = find_child_ptr(parent, left, &left_pos);
		if (result != NS_FOUND) {
			*error_msg = "Cannot find position of left child";
			return result;
		}
		assert("nikita-1355", left_pos.node != NULL);
	} else
		left_pos.node = NULL;

	/* check that they are separated by exactly one key and are basically
	   sane */
	if (REISER4_DEBUG) {
		if ((left_pos.node != NULL)
		    && !coord_is_existing_unit(&left_pos)) {
			*error_msg = "Left child is bastard";
			return RETERR(-EIO);
		}
		if (!coord_is_existing_unit(&right_pos)) {
			*error_msg = "Right child is bastard";
			return RETERR(-EIO);
		}
		if (left_pos.node != NULL &&
		    !coord_are_neighbors(&left_pos, &right_pos)) {
			*error_msg = "Children are not direct siblings";
			return RETERR(-EIO);
		}
	}
	*error_msg = NULL;

	info.doing = doing;
	info.todo = todo;

	/*
	 * If child node is not empty, new key of internal item is a key of
	 * leftmost item in the child node. If the child is empty, take its
	 * right delimiting key as a new key of the internal item. Precise key
	 * in the latter case is not important per se, because the child (and
	 * the internal item) are going to be killed shortly anyway, but we
	 * have to preserve correct order of keys in the parent node.
	 */

	if (!ZF_ISSET(right, JNODE_HEARD_BANSHEE))
		leftmost_key_in_node(right, &ldkey);
	else {
		read_lock_dk(znode_get_tree(parent));
		ldkey = *znode_get_rd_key(right);
		read_unlock_dk(znode_get_tree(parent));
	}
	node_plugin_by_node(parent)->update_item_key(&right_pos, &ldkey, &info);
	doing->restartable = 0;
	znode_make_dirty(parent);
	return 0;
}

/* implements COP_UPDATE opration

   Update delimiting keys.

*/
static int carry_update(carry_op * op /* operation to be performed */ ,
			carry_level * doing /* current carry level */ ,
			carry_level * todo/* next carry level */)
{
	int result;
	carry_node *missing UNUSED_ARG;
	znode *left;
	znode *right;
	carry_node *lchild;
	carry_node *rchild;
	const char *error_msg;
	reiser4_tree *tree;

	/*
	 * This operation is called to update key of internal item. This is
	 * necessary when carry shifted of cut data on the child
	 * level. Arguments of this operation are:
	 *
	 *     @right --- child node. Operation should update key of internal
	 *     item pointing to @right.
	 *
	 *     @left --- left neighbor of @right. This parameter is optional.
	 */

	assert("nikita-902", op != NULL);
	assert("nikita-903", todo != NULL);
	assert("nikita-904", op->op == COP_UPDATE);

	lchild = op->u.update.left;
	rchild = op->node;

	if (lchild != NULL) {
		assert("nikita-1001", lchild->parent);
		assert("nikita-1003", !lchild->left);
		left = reiser4_carry_real(lchild);
	} else
		left = NULL;

	tree = znode_get_tree(rchild->node);
	read_lock_tree(tree);
	right = znode_parent(rchild->node);
	read_unlock_tree(tree);

	if (right != NULL) {
		result = update_delimiting_key(right,
					       lchild ? lchild->node : NULL,
					       rchild->node,
					       doing, todo, &error_msg);
	} else {
		error_msg = "Cannot find node to update key in";
		result = RETERR(-EIO);
	}
	/* operation will be reposted to the next level by the
	   ->update_item_key() method of node plugin, if necessary. */

	if (result != 0) {
		warning("nikita-999", "Error updating delimiting key: %s (%i)",
			error_msg ? : "", result);
	}
	return result;
}

/* move items from @node during carry */
static int carry_shift_data(sideof side /* in what direction to move data */ ,
			    coord_t *insert_coord	/* coord where new item
							 * is to be inserted */,
			    znode * node /* node which data are moved from */ ,
			    carry_level * doing /* active carry queue */ ,
			    carry_level * todo	/* carry queue where new
						 * operations are to be put
						 * in */ ,
			    unsigned int including_insert_coord_p
				/* true if @insertion_coord can be moved */ )
{
	int result;
	znode *source;
	carry_plugin_info info;
	node_plugin *nplug;

	source = insert_coord->node;

	info.doing = doing;
	info.todo = todo;

	nplug = node_plugin_by_node(node);
	result = nplug->shift(insert_coord, node,
			      (side == LEFT_SIDE) ? SHIFT_LEFT : SHIFT_RIGHT, 0,
			      (int)including_insert_coord_p, &info);
	/* the only error ->shift() method of node plugin can return is
	   -ENOMEM due to carry node/operation allocation. */
	assert("nikita-915", result >= 0 || result == -ENOMEM);
	if (result > 0) {
		/*
		 * if some number of bytes was actually shifted, mark nodes
		 * dirty, and carry level as non-restartable.
		 */
		doing->restartable = 0;
		znode_make_dirty(source);
		znode_make_dirty(node);
	}

	assert("nikita-2077", coord_check(insert_coord));
	return 0;
}

typedef carry_node *(*carry_iterator) (carry_node * node);
static carry_node *find_dir_carry(carry_node * node, carry_level * level,
				  carry_iterator iterator);

static carry_node *pool_level_list_prev(carry_node *node)
{
	return list_entry(node->header.level_linkage.prev, carry_node, header.level_linkage);
}

/* look for the left neighbor of given carry node in a carry queue.

   This is used by find_left_neighbor(), but I am not sure that this
   really gives any advantage. More statistics required.

*/
carry_node *find_left_carry(carry_node * node	/* node to find left neighbor
						 * of */ ,
			    carry_level * level/* level to scan */)
{
	return find_dir_carry(node, level,
			      (carry_iterator) pool_level_list_prev);
}

static carry_node *pool_level_list_next(carry_node *node)
{
	return list_entry(node->header.level_linkage.next, carry_node, header.level_linkage);
}

/* look for the right neighbor of given carry node in a
   carry queue.

   This is used by find_right_neighbor(), but I am not sure that this
   really gives any advantage. More statistics required.

*/
carry_node *find_right_carry(carry_node * node	/* node to find right neighbor
						 * of */ ,
			     carry_level * level/* level to scan */)
{
	return find_dir_carry(node, level,
			      (carry_iterator) pool_level_list_next);
}

/* look for the left or right neighbor of given carry node in a carry
   queue.

   Helper function used by find_{left|right}_carry().
*/
static carry_node *find_dir_carry(carry_node * node	/* node to start
							 * scanning from */ ,
				  carry_level * level /* level to scan */ ,
				  carry_iterator iterator	/* operation to
								 * move to the
								 * next node */)
{
	carry_node *neighbor;

	assert("nikita-1059", node != NULL);
	assert("nikita-1060", level != NULL);

	/* scan list of carry nodes on this list dir-ward, skipping all
	   carry nodes referencing the same znode. */
	neighbor = node;
	while (1) {
		neighbor = iterator(neighbor);
		if (carry_node_end(level, neighbor))
			/* list head is reached */
			return NULL;
		if (reiser4_carry_real(neighbor) != reiser4_carry_real(node))
			return neighbor;
	}
}

/*
 * Memory reservation estimation.
 *
 * Carry process proceeds through tree levels upwards. Carry assumes that it
 * takes tree in consistent state (e.g., that search tree invariants hold),
 * and leaves tree consistent after it finishes. This means that when some
 * error occurs carry cannot simply return if there are pending carry
 * operations. Generic solution for this problem is carry-undo either as
 * transaction manager feature (requiring checkpoints and isolation), or
 * through some carry specific mechanism.
 *
 * Our current approach is to panic if carry hits an error while tree is
 * inconsistent. Unfortunately -ENOMEM can easily be triggered. To work around
 * this "memory reservation" mechanism was added.
 *
 * Memory reservation is implemented by perthread-pages.diff patch from
 * core-patches. Its API is defined in <linux/gfp.h>
 *
 *     int  perthread_pages_reserve(int nrpages, gfp_t gfp);
 *     void perthread_pages_release(int nrpages);
 *     int  perthread_pages_count(void);
 *
 * carry estimates its worst case memory requirements at the entry, reserved
 * enough memory, and released unused pages before returning.
 *
 * Code below estimates worst case memory requirements for a given carry
 * queue. This is dome by summing worst case memory requirements for each
 * operation in the queue.
 *
 */

/*
 * Memory memory requirements of many operations depends on the tree
 * height. For example, item insertion requires new node to be inserted at
 * each tree level in the worst case. What tree height should be used for
 * estimation? Current tree height is wrong, because tree height can change
 * between the time when estimation was done and the time when operation is
 * actually performed. Maximal possible tree height (REISER4_MAX_ZTREE_HEIGHT)
 * is also not desirable, because it would lead to the huge over-estimation
 * all the time. Plausible solution is "capped tree height": if current tree
 * height is less than some TREE_HEIGHT_CAP constant, capped tree height is
 * TREE_HEIGHT_CAP, otherwise it's current tree height. Idea behind this is
 * that if tree height is TREE_HEIGHT_CAP or larger, it's extremely unlikely
 * to be increased even more during short interval of time.
 */
#define TREE_HEIGHT_CAP (5)

/* return capped tree height for the @tree. See comment above. */
static int cap_tree_height(reiser4_tree * tree)
{
	return max_t(int, tree->height, TREE_HEIGHT_CAP);
}

/* return capped tree height for the current tree. */
static int capped_height(void)
{
	return cap_tree_height(current_tree);
}

/* return number of pages required to store given number of bytes */
static int bytes_to_pages(int bytes)
{
	return (bytes + PAGE_CACHE_SIZE - 1) >> PAGE_CACHE_SHIFT;
}

/* how many pages are required to allocate znodes during item insertion. */
static int carry_estimate_znodes(void)
{
	/*
	 * Note, that there we have some problem here: there is no way to
	 * reserve pages specifically for the given slab. This means that
	 * these pages can be hijacked for some other end.
	 */

	/* in the worst case we need 3 new znode on each tree level */
	return bytes_to_pages(capped_height() * sizeof(znode) * 3);
}

/*
 * how many pages are required to load bitmaps. One bitmap per level.
 */
static int carry_estimate_bitmaps(void)
{
	if (reiser4_is_set(reiser4_get_current_sb(), REISER4_DONT_LOAD_BITMAP)) {
		int bytes;

		bytes = capped_height() * (0 +	/* bnode should be added, but
						 * it is private to bitmap.c,
						 * skip for now. */
					   2 * sizeof(jnode));
						/* working and commit jnodes */
		return bytes_to_pages(bytes) + 2;	/* and their contents */
	} else
		/* bitmaps were pre-loaded during mount */
		return 0;
}

/* worst case item insertion memory requirements */
static int carry_estimate_insert(carry_op * op, carry_level * level)
{
	return carry_estimate_bitmaps() + carry_estimate_znodes() + 1 +
								/* new atom */
	    capped_height() +	/* new block on each level */
	    1 +		/* and possibly extra new block at the leaf level */
	    3;			/* loading of leaves into memory */
}

/* worst case item deletion memory requirements */
static int carry_estimate_delete(carry_op * op, carry_level * level)
{
	return carry_estimate_bitmaps() + carry_estimate_znodes() + 1 +
								/* new atom */
	    3;			/* loading of leaves into memory */
}

/* worst case tree cut memory requirements */
static int carry_estimate_cut(carry_op * op, carry_level * level)
{
	return carry_estimate_bitmaps() + carry_estimate_znodes() + 1 +
								/* new atom */
	    3;			/* loading of leaves into memory */
}

/* worst case memory requirements of pasting into item */
static int carry_estimate_paste(carry_op * op, carry_level * level)
{
	return carry_estimate_bitmaps() + carry_estimate_znodes() + 1 +
								/* new atom */
	    capped_height() +	/* new block on each level */
	    1 +		/* and possibly extra new block at the leaf level */
	    3;			/* loading of leaves into memory */
}

/* worst case memory requirements of extent insertion */
static int carry_estimate_extent(carry_op * op, carry_level * level)
{
	return carry_estimate_insert(op, level) +	/* insert extent */
	    carry_estimate_delete(op, level);	/* kill leaf */
}

/* worst case memory requirements of key update */
static int carry_estimate_update(carry_op * op, carry_level * level)
{
	return 0;
}

/* worst case memory requirements of flow insertion */
static int carry_estimate_insert_flow(carry_op * op, carry_level * level)
{
	int newnodes;

	newnodes = min(bytes_to_pages(op->u.insert_flow.flow->length),
		       CARRY_FLOW_NEW_NODES_LIMIT);
	/*
	 * roughly estimate insert_flow as a sequence of insertions.
	 */
	return newnodes * carry_estimate_insert(op, level);
}

/* This is dispatch table for carry operations. It can be trivially
   abstracted into useful plugin: tunable balancing policy is a good
   thing. */
carry_op_handler op_dispatch_table[COP_LAST_OP] = {
	[COP_INSERT] = {
			.handler = carry_insert,
			.estimate = carry_estimate_insert}
	,
	[COP_DELETE] = {
			.handler = carry_delete,
			.estimate = carry_estimate_delete}
	,
	[COP_CUT] = {
		     .handler = carry_cut,
		     .estimate = carry_estimate_cut}
	,
	[COP_PASTE] = {
		       .handler = carry_paste,
		       .estimate = carry_estimate_paste}
	,
	[COP_EXTENT] = {
			.handler = carry_extent,
			.estimate = carry_estimate_extent}
	,
	[COP_UPDATE] = {
			.handler = carry_update,
			.estimate = carry_estimate_update}
	,
	[COP_INSERT_FLOW] = {
			     .handler = carry_insert_flow,
			     .estimate = carry_estimate_insert_flow}
};

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
