/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

/* Implementation of internal-item plugin methods. */

#include "../../forward.h"
#include "../../debug.h"
#include "../../dformat.h"
#include "../../key.h"
#include "../../coord.h"
#include "internal.h"
#include "item.h"
#include "../node/node.h"
#include "../plugin.h"
#include "../../jnode.h"
#include "../../znode.h"
#include "../../tree_walk.h"
#include "../../tree_mod.h"
#include "../../tree.h"
#include "../../super.h"
#include "../../block_alloc.h"

/* see internal.h for explanation */

/* plugin->u.item.b.mergeable */
int mergeable_internal(const coord_t * p1 UNUSED_ARG /* first item */ ,
		       const coord_t * p2 UNUSED_ARG /* second item */ )
{
	/* internal items are not mergeable */
	return 0;
}

/* ->lookup() method for internal items */
lookup_result lookup_internal(const reiser4_key * key /* key to look up */ ,
			      lookup_bias bias UNUSED_ARG /* lookup bias */ ,
			      coord_t * coord /* coord of item */ )
{
	reiser4_key ukey;

	switch (keycmp(unit_key_by_coord(coord, &ukey), key)) {
	default:
		impossible("", "keycmp()?!");
	case LESS_THAN:
		/* FIXME-VS: AFTER_ITEM used to be here. But with new coord
		   item plugin can not be taken using coord set this way */
		assert("vs-681", coord->unit_pos == 0);
		coord->between = AFTER_UNIT;
	case EQUAL_TO:
		return CBK_COORD_FOUND;
	case GREATER_THAN:
		return CBK_COORD_NOTFOUND;
	}
}

/* return body of internal item at @coord */
static internal_item_layout *internal_at(const coord_t * coord	/* coord of
								 * item */ )
{
	assert("nikita-607", coord != NULL);
	assert("nikita-1650",
	       item_plugin_by_coord(coord) ==
	       item_plugin_by_id(NODE_POINTER_ID));
	return (internal_item_layout *) item_body_by_coord(coord);
}

void reiser4_update_internal(const coord_t * coord,
			     const reiser4_block_nr * blocknr)
{
	internal_item_layout *item = internal_at(coord);
	assert("nikita-2959", reiser4_blocknr_is_sane(blocknr));

	put_unaligned(cpu_to_le64(*blocknr), &item->pointer);
}

/* return child block number stored in the internal item at @coord */
static reiser4_block_nr pointer_at(const coord_t * coord /* coord of item */ )
{
	assert("nikita-608", coord != NULL);
	return le64_to_cpu(get_unaligned(&internal_at(coord)->pointer));
}

/* get znode pointed to by internal @item */
static znode *znode_at(const coord_t * item /* coord of item */ ,
		       znode * parent /* parent node */ )
{
	return child_znode(item, parent, 1, 0);
}

/* store pointer from internal item into "block". Implementation of
    ->down_link() method */
void down_link_internal(const coord_t * coord /* coord of item */ ,
			const reiser4_key * key UNUSED_ARG	/* key to get
								 * pointer for */ ,
			reiser4_block_nr * block /* resulting block number */ )
{
	ON_DEBUG(reiser4_key item_key);

	assert("nikita-609", coord != NULL);
	assert("nikita-611", block != NULL);
	assert("nikita-612", (key == NULL) ||
	       /* twig horrors */
	       (znode_get_level(coord->node) == TWIG_LEVEL)
	       || keyle(item_key_by_coord(coord, &item_key), key));

	*block = pointer_at(coord);
	assert("nikita-2960", reiser4_blocknr_is_sane(block));
}

/* Get the child's block number, or 0 if the block is unallocated. */
int
utmost_child_real_block_internal(const coord_t * coord, sideof side UNUSED_ARG,
				 reiser4_block_nr * block)
{
	assert("jmacd-2059", coord != NULL);

	*block = pointer_at(coord);
	assert("nikita-2961", reiser4_blocknr_is_sane(block));

	if (reiser4_blocknr_is_fake(block)) {
		*block = 0;
	}

	return 0;
}

/* Return the child. */
int
utmost_child_internal(const coord_t * coord, sideof side UNUSED_ARG,
		      jnode ** childp)
{
	reiser4_block_nr block = pointer_at(coord);
	znode *child;

	assert("jmacd-2059", childp != NULL);
	assert("nikita-2962", reiser4_blocknr_is_sane(&block));

	child = zlook(znode_get_tree(coord->node), &block);

	if (IS_ERR(child)) {
		return PTR_ERR(child);
	}

	*childp = ZJNODE(child);

	return 0;
}

#if REISER4_DEBUG

static void check_link(znode * left, znode * right)
{
	znode *scan;

	for (scan = left; scan != right; scan = scan->right) {
		if (ZF_ISSET(scan, JNODE_RIP))
			break;
		if (znode_is_right_connected(scan) && scan->right != NULL) {
			if (ZF_ISSET(scan->right, JNODE_RIP))
				break;
			assert("nikita-3285",
			       znode_is_left_connected(scan->right));
			assert("nikita-3265",
			       ergo(scan != left,
				    ZF_ISSET(scan, JNODE_HEARD_BANSHEE)));
			assert("nikita-3284", scan->right->left == scan);
		} else
			break;
	}
}

int check__internal(const coord_t * coord, const char **error)
{
	reiser4_block_nr blk;
	znode *child;
	coord_t cpy;

	blk = pointer_at(coord);
	if (!reiser4_blocknr_is_sane(&blk)) {
		*error = "Invalid pointer";
		return -1;
	}
	coord_dup(&cpy, coord);
	child = znode_at(&cpy, cpy.node);
	if (child != NULL) {
		znode *left_child;
		znode *right_child;

		left_child = right_child = NULL;

		assert("nikita-3256", znode_invariant(child));
		if (coord_prev_item(&cpy) == 0 && item_is_internal(&cpy)) {
			left_child = znode_at(&cpy, cpy.node);
			if (left_child != NULL) {
				read_lock_tree(znode_get_tree(child));
				check_link(left_child, child);
				read_unlock_tree(znode_get_tree(child));
				zput(left_child);
			}
		}
		coord_dup(&cpy, coord);
		if (coord_next_item(&cpy) == 0 && item_is_internal(&cpy)) {
			right_child = znode_at(&cpy, cpy.node);
			if (right_child != NULL) {
				read_lock_tree(znode_get_tree(child));
				check_link(child, right_child);
				read_unlock_tree(znode_get_tree(child));
				zput(right_child);
			}
		}
		zput(child);
	}
	return 0;
}

#endif  /*  REISER4_DEBUG  */

/* return true only if this item really points to "block" */
/* Audited by: green(2002.06.14) */
int has_pointer_to_internal(const coord_t * coord /* coord of item */ ,
			    const reiser4_block_nr * block	/* block number to
								 * check */ )
{
	assert("nikita-613", coord != NULL);
	assert("nikita-614", block != NULL);

	return pointer_at(coord) == *block;
}

/* hook called by ->create_item() method of node plugin after new internal
   item was just created.

   This is point where pointer to new node is inserted into tree. Initialize
   parent pointer in child znode, insert child into sibling list and slum.

*/
int create_hook_internal(const coord_t * item /* coord of item */ ,
			 void *arg /* child's left neighbor, if any */ )
{
	znode *child;
	__u64 child_ptr;

	assert("nikita-1252", item != NULL);
	assert("nikita-1253", item->node != NULL);
	assert("nikita-1181", znode_get_level(item->node) > LEAF_LEVEL);
	assert("nikita-1450", item->unit_pos == 0);

	/*
	 * preparing to item insertion build_child_ptr_data sets pointer to
	 * data to be inserted to jnode's blocknr which is in cpu byte
	 * order. Node's create_item simply copied those data. As result we
	 * have child pointer in cpu's byte order. Convert content of internal
	 * item to little endian byte order.
	 */
	child_ptr = get_unaligned((__u64 *)item_body_by_coord(item));
	reiser4_update_internal(item, &child_ptr);

	child = znode_at(item, item->node);
	if (child != NULL && !IS_ERR(child)) {
		znode *left;
		int result = 0;
		reiser4_tree *tree;

		left = arg;
		tree = znode_get_tree(item->node);
		write_lock_tree(tree);
		write_lock_dk(tree);
		assert("nikita-1400", (child->in_parent.node == NULL)
		       || (znode_above_root(child->in_parent.node)));
		++item->node->c_count;
		coord_to_parent_coord(item, &child->in_parent);
		sibling_list_insert_nolock(child, left);

		assert("nikita-3297", ZF_ISSET(child, JNODE_ORPHAN));
		ZF_CLR(child, JNODE_ORPHAN);

		if ((left != NULL) && !keyeq(znode_get_rd_key(left),
					     znode_get_rd_key(child))) {
			znode_set_rd_key(child, znode_get_rd_key(left));
		}
		write_unlock_dk(tree);
		write_unlock_tree(tree);
		zput(child);
		return result;
	} else {
		if (child == NULL)
			child = ERR_PTR(-EIO);
		return PTR_ERR(child);
	}
}

/* hook called by ->cut_and_kill() method of node plugin just before internal
   item is removed.

   This is point where empty node is removed from the tree. Clear parent
   pointer in child, and mark node for pending deletion.

   Node will be actually deleted later and in several installations:

    . when last lock on this node will be released, node will be removed from
    the sibling list and its lock will be invalidated

    . when last reference to this node will be dropped, bitmap will be updated
    and node will be actually removed from the memory.

*/
int kill_hook_internal(const coord_t * item /* coord of item */ ,
		       pos_in_node_t from UNUSED_ARG /* start unit */ ,
		       pos_in_node_t count UNUSED_ARG /* stop unit */ ,
		       struct carry_kill_data *p UNUSED_ARG)
{
	znode *child;
	int result = 0;

	assert("nikita-1222", item != NULL);
	assert("nikita-1224", from == 0);
	assert("nikita-1225", count == 1);

	child = znode_at(item, item->node);
	if (child == NULL)
		return 0;
	if (IS_ERR(child))
		return PTR_ERR(child);
	result = zload(child);
	if (result) {
		zput(child);
		return result;
	}
	if (node_is_empty(child)) {
		reiser4_tree *tree;

		assert("nikita-1397", znode_is_write_locked(child));
		assert("nikita-1398", child->c_count == 0);
		assert("nikita-2546", ZF_ISSET(child, JNODE_HEARD_BANSHEE));

		tree = znode_get_tree(item->node);
		write_lock_tree(tree);
		init_parent_coord(&child->in_parent, NULL);
		--item->node->c_count;
		write_unlock_tree(tree);
	} else {
		warning("nikita-1223",
			"Cowardly refuse to remove link to non-empty node");
		result = RETERR(-EIO);
	}
	zrelse(child);
	zput(child);
	return result;
}

/* hook called by ->shift() node plugin method when iternal item was just
   moved from one node to another.

   Update parent pointer in child and c_counts in old and new parent

*/
int shift_hook_internal(const coord_t * item /* coord of item */ ,
			unsigned from UNUSED_ARG /* start unit */ ,
			unsigned count UNUSED_ARG /* stop unit */ ,
			znode * old_node /* old parent */ )
{
	znode *child;
	znode *new_node;
	reiser4_tree *tree;

	assert("nikita-1276", item != NULL);
	assert("nikita-1277", from == 0);
	assert("nikita-1278", count == 1);
	assert("nikita-1451", item->unit_pos == 0);

	new_node = item->node;
	assert("nikita-2132", new_node != old_node);
	tree = znode_get_tree(item->node);
	child = child_znode(item, old_node, 1, 0);
	if (child == NULL)
		return 0;
	if (!IS_ERR(child)) {
		write_lock_tree(tree);
		++new_node->c_count;
		assert("nikita-1395", znode_parent(child) == old_node);
		assert("nikita-1396", old_node->c_count > 0);
		coord_to_parent_coord(item, &child->in_parent);
		assert("nikita-1781", znode_parent(child) == new_node);
		assert("nikita-1782",
		       check_tree_pointer(item, child) == NS_FOUND);
		--old_node->c_count;
		write_unlock_tree(tree);
		zput(child);
		return 0;
	} else
		return PTR_ERR(child);
}

/* plugin->u.item.b.max_key_inside - not defined */

/* plugin->u.item.b.nr_units - item.c:single_unit */

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   End:
*/
