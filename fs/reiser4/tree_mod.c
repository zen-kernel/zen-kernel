/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

/*
 * Functions to add/delete new nodes to/from the tree.
 *
 * Functions from this file are used by carry (see carry*) to handle:
 *
 *     . insertion of new formatted node into tree
 *
 *     . addition of new tree root, increasing tree height
 *
 *     . removing tree root, decreasing tree height
 *
 */

#include "forward.h"
#include "debug.h"
#include "dformat.h"
#include "key.h"
#include "coord.h"
#include "plugin/plugin.h"
#include "jnode.h"
#include "znode.h"
#include "tree_mod.h"
#include "block_alloc.h"
#include "tree_walk.h"
#include "tree.h"
#include "super.h"

#include <linux/err.h>

static int add_child_ptr(znode * parent, znode * child);
/* warning only issued if error is not -E_REPEAT */
#define ewarning( error, ... )			\
	if( ( error ) != -E_REPEAT )		\
		warning( __VA_ARGS__ )

/* allocate new node on the @level and immediately on the right of @brother. */
znode * reiser4_new_node(znode * brother /* existing left neighbor
					  *  of new node */,
			 tree_level level /* tree level at which new node is to
					   * be allocated */)
{
	znode *result;
	int retcode;
	reiser4_block_nr blocknr;

	assert("nikita-930", brother != NULL);
	assert("umka-264", level < REAL_MAX_ZTREE_HEIGHT);

	retcode = assign_fake_blocknr_formatted(&blocknr);
	if (retcode == 0) {
		result =
		    zget(znode_get_tree(brother), &blocknr, NULL, level,
			 reiser4_ctx_gfp_mask_get());
		if (IS_ERR(result)) {
			ewarning(PTR_ERR(result), "nikita-929",
				 "Cannot allocate znode for carry: %li",
				 PTR_ERR(result));
			return result;
		}
		/* cheap test, can be executed even when debugging is off */
		if (!znode_just_created(result)) {
			warning("nikita-2213",
				"Allocated already existing block: %llu",
				(unsigned long long)blocknr);
			zput(result);
			return ERR_PTR(RETERR(-EIO));
		}

		assert("nikita-931", result != NULL);
		result->nplug = znode_get_tree(brother)->nplug;
		assert("nikita-933", result->nplug != NULL);

		retcode = zinit_new(result, reiser4_ctx_gfp_mask_get());
		if (retcode == 0) {
			ZF_SET(result, JNODE_CREATED);
			zrelse(result);
		} else {
			zput(result);
			result = ERR_PTR(retcode);
		}
	} else {
		/* failure to allocate new node during balancing.
		   This should never happen. Ever. Returning -E_REPEAT
		   is not viable solution, because "out of disk space"
		   is not transient error that will go away by itself.
		 */
		ewarning(retcode, "nikita-928",
			 "Cannot allocate block for carry: %i", retcode);
		result = ERR_PTR(retcode);
	}
	assert("nikita-1071", result != NULL);
	return result;
}

/* allocate new root and add it to the tree

   This helper function is called by add_new_root().

*/
znode *reiser4_add_tree_root(znode * old_root /* existing tree root */ ,
		     znode * fake /* "fake" znode */ )
{
	reiser4_tree *tree = znode_get_tree(old_root);
	znode *new_root = NULL;	/* to shut gcc up */
	int result;

	assert("nikita-1069", old_root != NULL);
	assert("umka-262", fake != NULL);
	assert("umka-263", tree != NULL);

	/* "fake" znode---one always hanging just above current root. This
	   node is locked when new root is created or existing root is
	   deleted. Downward tree traversal takes lock on it before taking
	   lock on a root node. This avoids race conditions with root
	   manipulations.

	 */
	assert("nikita-1348", znode_above_root(fake));
	assert("nikita-1211", znode_is_root(old_root));

	result = 0;
	if (tree->height >= REAL_MAX_ZTREE_HEIGHT) {
		warning("nikita-1344", "Tree is too tall: %i", tree->height);
		/* ext2 returns -ENOSPC when it runs out of free inodes with a
		   following comment (fs/ext2/ialloc.c:441): Is it really
		   ENOSPC?

		   -EXFULL? -EINVAL?
		 */
		result = RETERR(-ENOSPC);
	} else {
		/* Allocate block for new root. It's not that
		   important where it will be allocated, as root is
		   almost always in memory. Moreover, allocate on
		   flush can be going here.
		 */
		assert("nikita-1448", znode_is_root(old_root));
		new_root = reiser4_new_node(fake, tree->height + 1);
		if (!IS_ERR(new_root) && (result = zload(new_root)) == 0) {
			lock_handle rlh;

			init_lh(&rlh);
			result =
			    longterm_lock_znode(&rlh, new_root,
						ZNODE_WRITE_LOCK,
						ZNODE_LOCK_LOPRI);
			if (result == 0) {
				parent_coord_t *in_parent;

				znode_make_dirty(fake);

				/* new root is a child of "fake" node */
				write_lock_tree(tree);

				++tree->height;

				/* recalculate max balance overhead */
				tree->estimate_one_insert =
				    estimate_one_insert_item(tree);

				tree->root_block = *znode_get_block(new_root);
				in_parent = &new_root->in_parent;
				init_parent_coord(in_parent, fake);
				/* manually insert new root into sibling
				 * list. With this all nodes involved into
				 * balancing are connected after balancing is
				 * done---useful invariant to check. */
				sibling_list_insert_nolock(new_root, NULL);
				write_unlock_tree(tree);

				/* insert into new root pointer to the
				   @old_root. */
				assert("nikita-1110",
				       WITH_DATA(new_root,
						 node_is_empty(new_root)));
				write_lock_dk(tree);
				znode_set_ld_key(new_root, reiser4_min_key());
				znode_set_rd_key(new_root, reiser4_max_key());
				write_unlock_dk(tree);
				if (REISER4_DEBUG) {
					ZF_CLR(old_root, JNODE_LEFT_CONNECTED);
					ZF_CLR(old_root, JNODE_RIGHT_CONNECTED);
					ZF_SET(old_root, JNODE_ORPHAN);
				}
				result = add_child_ptr(new_root, old_root);
				done_lh(&rlh);
			}
			zrelse(new_root);
		}
	}
	if (result != 0)
		new_root = ERR_PTR(result);
	return new_root;
}

/* build &reiser4_item_data for inserting child pointer

   Build &reiser4_item_data that can be later used to insert pointer to @child
   in its parent.

*/
void build_child_ptr_data(znode * child	/* node pointer to which will be
					 * inserted */ ,
			  reiser4_item_data * data /* where to store result */ )
{
	assert("nikita-1116", child != NULL);
	assert("nikita-1117", data != NULL);

	/*
	 * NOTE: use address of child's blocknr as address of data to be
	 * inserted. As result of this data gets into on-disk structure in cpu
	 * byte order. internal's create_hook converts it to little endian byte
	 * order.
	 */
	data->data = (char *)znode_get_block(child);
	/* data -> data is kernel space */
	data->user = 0;
	data->length = sizeof(reiser4_block_nr);
	/* FIXME-VS: hardcoded internal item? */

	/* AUDIT: Is it possible that "item_plugin_by_id" may find nothing? */
	data->iplug = item_plugin_by_id(NODE_POINTER_ID);
}

/* add pointer to @child into empty @parent.

   This is used when pointer to old root is inserted into new root which is
   empty.
*/
static int add_child_ptr(znode * parent, znode * child)
{
	coord_t coord;
	reiser4_item_data data;
	int result;
	reiser4_key key;

	assert("nikita-1111", parent != NULL);
	assert("nikita-1112", child != NULL);
	assert("nikita-1115",
	       znode_get_level(parent) == znode_get_level(child) + 1);

	result = zload(parent);
	if (result != 0)
		return result;
	assert("nikita-1113", node_is_empty(parent));
	coord_init_first_unit(&coord, parent);

	build_child_ptr_data(child, &data);
	data.arg = NULL;

	read_lock_dk(znode_get_tree(parent));
	key = *znode_get_ld_key(child);
	read_unlock_dk(znode_get_tree(parent));

	result = node_plugin_by_node(parent)->create_item(&coord, &key, &data,
							  NULL);
	znode_make_dirty(parent);
	zrelse(parent);
	return result;
}

/* actually remove tree root */
static int reiser4_kill_root(reiser4_tree * tree /* tree from which root is
						  * being removed */,
			     znode * old_root /* root node that is being
					       * removed */ ,
			     znode * new_root	/* new root---sole child of
						 * @old_root */,
		     const reiser4_block_nr * new_root_blk /* disk address of
							    * @new_root */)
{
	znode *uber;
	int result;
	lock_handle handle_for_uber;

	assert("umka-265", tree != NULL);
	assert("nikita-1198", new_root != NULL);
	assert("nikita-1199",
	       znode_get_level(new_root) + 1 == znode_get_level(old_root));

	assert("nikita-1201", znode_is_write_locked(old_root));

	assert("nikita-1203",
	       disk_addr_eq(new_root_blk, znode_get_block(new_root)));

	init_lh(&handle_for_uber);
	/* obtain and lock "fake" znode protecting changes in tree height. */
	result = get_uber_znode(tree, ZNODE_WRITE_LOCK, ZNODE_LOCK_HIPRI,
				&handle_for_uber);
	if (result == 0) {
		uber = handle_for_uber.node;

		znode_make_dirty(uber);

		/* don't take long term lock a @new_root. Take spinlock. */

		write_lock_tree(tree);

		tree->root_block = *new_root_blk;
		--tree->height;

		/* recalculate max balance overhead */
		tree->estimate_one_insert = estimate_one_insert_item(tree);

		assert("nikita-1202",
		       tree->height == znode_get_level(new_root));

		/* new root is child on "fake" node */
		init_parent_coord(&new_root->in_parent, uber);
		++uber->c_count;

		/* sibling_list_insert_nolock(new_root, NULL); */
		write_unlock_tree(tree);

		/* reinitialise old root. */
		result = node_plugin_by_node(old_root)->init(old_root);
		znode_make_dirty(old_root);
		if (result == 0) {
			assert("nikita-1279", node_is_empty(old_root));
			ZF_SET(old_root, JNODE_HEARD_BANSHEE);
			old_root->c_count = 0;
		}
	}
	done_lh(&handle_for_uber);

	return result;
}

/* remove tree root

   This function removes tree root, decreasing tree height by one.  Tree root
   and its only child (that is going to become new tree root) are write locked
   at the entry.

   To remove tree root we need to take lock on special "fake" znode that
   protects changes of tree height. See comments in reiser4_add_tree_root() for
   more on this.

   Also parent pointers have to be updated in
   old and new root. To simplify code, function is split into two parts: outer
   reiser4_kill_tree_root() collects all necessary arguments and calls
   reiser4_kill_root() to do the actual job.

*/
int reiser4_kill_tree_root(znode * old_root /* tree root that we are
					       removing*/)
{
	int result;
	coord_t down_link;
	znode *new_root;
	reiser4_tree *tree;

	assert("umka-266", current_tree != NULL);
	assert("nikita-1194", old_root != NULL);
	assert("nikita-1196", znode_is_root(old_root));
	assert("nikita-1200", node_num_items(old_root) == 1);
	assert("nikita-1401", znode_is_write_locked(old_root));

	coord_init_first_unit(&down_link, old_root);

	tree = znode_get_tree(old_root);
	new_root = child_znode(&down_link, old_root, 0, 1);
	if (!IS_ERR(new_root)) {
		result =
			reiser4_kill_root(tree, old_root, new_root,
					  znode_get_block(new_root));
		zput(new_root);
	} else
		result = PTR_ERR(new_root);

	return result;
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
