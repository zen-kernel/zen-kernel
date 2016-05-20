/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
   reiser4/README */

#include "forward.h"
#include "debug.h"
#include "key.h"
#include "coord.h"
#include "plugin/item/item.h"
#include "plugin/node/node.h"
#include "znode.h"
#include "block_alloc.h"
#include "tree_walk.h"
#include "tree_mod.h"
#include "carry.h"
#include "tree.h"
#include "super.h"

#include <linux/types.h>	/* for __u??  */

/*
 * Extents on the twig level (EOTTL) handling.
 *
 * EOTTL poses some problems to the tree traversal, that are better explained
 * by example.
 *
 * Suppose we have block B1 on the twig level with the following items:
 *
 * 0. internal item I0 with key (0:0:0:0) (locality, key-type, object-id,
 * offset)
 * 1. extent item E1 with key (1:4:100:0), having 10 blocks of 4k each
 * 2. internal item I2 with key (10:0:0:0)
 *
 * We are trying to insert item with key (5:0:0:0). Lookup finds node B1, and
 * then intra-node lookup is done. This lookup finished on the E1, because the
 * key we are looking for is larger than the key of E1 and is smaller than key
 * the of I2.
 *
 * Here search is stuck.
 *
 * After some thought it is clear what is wrong here: extents on the twig level
 * break some basic property of the *search* tree (on the pretext, that they
 * restore property of balanced tree).
 *
 * Said property is the following: if in the internal node of the search tree
 * we have [ ... Key1 Pointer Key2 ... ] then, all data that are or will be
 * keyed in the tree with the Key such that Key1 <= Key < Key2 are accessible
 * through the Pointer.
 *
 * This is not true, when Pointer is Extent-Pointer, simply because extent
 * cannot expand indefinitely to the right to include any item with
 *
 *   Key1 <= Key <= Key2.
 *
 * For example, our E1 extent is only responsible for the data with keys
 *
 *   (1:4:100:0) <= key <= (1:4:100:0xffffffffffffffff), and
 *
 * so, key range
 *
 *   ( (1:4:100:0xffffffffffffffff), (10:0:0:0) )
 *
 * is orphaned: there is no way to get there from the tree root.
 *
 * In other words, extent pointers are different than normal child pointers as
 * far as search tree is concerned, and this creates such problems.
 *
 * Possible solution for this problem is to insert our item into node pointed
 * to by I2. There are some problems through:
 *
 * (1) I2 can be in a different node.
 * (2) E1 can be immediately followed by another extent E2.
 *
 * (1) is solved by calling reiser4_get_right_neighbor() and accounting
 * for locks/coords as necessary.
 *
 * (2) is more complex. Solution here is to insert new empty leaf node and
 * insert internal item between E1 and E2 pointing to said leaf node. This is
 * further complicated by possibility that E2 is in a different node, etc.
 *
 * Problems:
 *
 * (1) if there was internal item I2 immediately on the right of an extent E1
 * we and we decided to insert new item S1 into node N2 pointed to by I2, then
 * key of S1 will be less than smallest key in the N2. Normally, search key
 * checks that key we are looking for is in the range of keys covered by the
 * node key is being looked in. To work around of this situation, while
 * preserving useful consistency check new flag CBK_TRUST_DK was added to the
 * cbk falgs bitmask. This flag is automatically set on entrance to the
 * coord_by_key() and is only cleared when we are about to enter situation
 * described above.
 *
 * (2) If extent E1 is immediately followed by another extent E2 and we are
 * searching for the key that is between E1 and E2 we only have to insert new
 * empty leaf node when coord_by_key was called for insertion, rather than just
 * for lookup. To distinguish these cases, new flag CBK_FOR_INSERT was added to
 * the cbk falgs bitmask. This flag is automatically set by coord_by_key calls
 * performed by insert_by_key() and friends.
 *
 * (3) Insertion of new empty leaf node (possibly) requires balancing. In any
 * case it requires modification of node content which is only possible under
 * write lock. It may well happen that we only have read lock on the node where
 * new internal pointer is to be inserted (common case: lookup of non-existent
 * stat-data that fells between two extents). If only read lock is held, tree
 * traversal is restarted with lock_level modified so that next time we hit
 * this problem, write lock will be held. Once we have write lock, balancing
 * will be performed.
 */

/**
 * is_next_item_internal - check whether next item is internal
 * @coord: coordinate of extent item in twig node
 * @key: search key
 * @lh: twig node lock handle
 *
 * Looks at the unit next to @coord. If it is an internal one - 1 is returned,
 * @coord is set to that unit. If that unit is in right neighbor, @lh is moved
 * to that node, @coord is set to its first unit. If next item is not internal
 * or does not exist then 0 is returned, @coord and @lh are left unchanged. 2
 * is returned if search restart has to be done.
 */
static int
is_next_item_internal(coord_t *coord, const reiser4_key * key,
		      lock_handle * lh)
{
	coord_t next;
	lock_handle rn;
	int result;

	coord_dup(&next, coord);
	if (coord_next_unit(&next) == 0) {
		/* next unit is in this node */
		if (item_is_internal(&next)) {
			coord_dup(coord, &next);
			return 1;
		}
		assert("vs-3", item_is_extent(&next));
		return 0;
	}

	/*
	 * next unit either does not exist or is in right neighbor. If it is in
	 * right neighbor we have to check right delimiting key because
	 * concurrent thread could get their first and insert item with a key
	 * smaller than @key
	 */
	read_lock_dk(current_tree);
	result = keycmp(key, znode_get_rd_key(coord->node));
	read_unlock_dk(current_tree);
	assert("vs-6", result != EQUAL_TO);
	if (result == GREATER_THAN)
		return 2;

	/* lock right neighbor */
	init_lh(&rn);
	result = reiser4_get_right_neighbor(&rn, coord->node,
					    znode_is_wlocked(coord->node) ?
					    ZNODE_WRITE_LOCK : ZNODE_READ_LOCK,
					    GN_CAN_USE_UPPER_LEVELS);
	if (result == -E_NO_NEIGHBOR) {
		/* we are on the rightmost edge of the tree */
		done_lh(&rn);
		return 0;
	}

	if (result) {
		assert("vs-4", result < 0);
		done_lh(&rn);
		return result;
	}

	/*
	 * check whether concurrent thread managed to insert item with a key
	 * smaller than @key
	 */
	read_lock_dk(current_tree);
	result = keycmp(key, znode_get_ld_key(rn.node));
	read_unlock_dk(current_tree);
	assert("vs-6", result != EQUAL_TO);
	if (result == GREATER_THAN) {
		done_lh(&rn);
		return 2;
	}

	result = zload(rn.node);
	if (result) {
		assert("vs-5", result < 0);
		done_lh(&rn);
		return result;
	}

	coord_init_first_unit(&next, rn.node);
	if (item_is_internal(&next)) {
		/*
		 * next unit is in right neighbor and it is an unit of internal
		 * item. Unlock coord->node. Move @lh to right neighbor. @coord
		 * is set to the first unit of right neighbor.
		 */
		coord_dup(coord, &next);
		zrelse(rn.node);
		done_lh(lh);
		move_lh(lh, &rn);
		return 1;
	}

	/*
	 * next unit is unit of extent item. Return without chaning @lh and
	 * @coord.
	 */
	assert("vs-6", item_is_extent(&next));
	zrelse(rn.node);
	done_lh(&rn);
	return 0;
}

/**
 * rd_key - calculate key of an item next to the given one
 * @coord: position in a node
 * @key: storage for result key
 *
 * @coord is set between items or after the last item in a node. Calculate key
 * of item to the right of @coord.
 */
static reiser4_key *rd_key(const coord_t *coord, reiser4_key *key)
{
	coord_t dup;

	assert("nikita-2281", coord_is_between_items(coord));
	coord_dup(&dup, coord);

	if (coord_set_to_right(&dup) == 0)
		/* next item is in this node. Return its key. */
		unit_key_by_coord(&dup, key);
	else {
		/*
		 * next item either does not exist or is in right
		 * neighbor. Return znode's right delimiting key.
		 */
		read_lock_dk(current_tree);
		*key = *znode_get_rd_key(coord->node);
		read_unlock_dk(current_tree);
	}
	return key;
}

/**
 * add_empty_leaf - insert empty leaf between two extents
 * @insert_coord: position in twig node between two extents
 * @lh: twig node lock handle
 * @key: left delimiting key of new node
 * @rdkey: right delimiting key of new node
 *
 * Inserts empty leaf node between two extent items. It is necessary when we
 * have to insert an item on leaf level between two extents (items on the twig
 * level).
 */
static int
add_empty_leaf(coord_t *insert_coord, lock_handle *lh,
	       const reiser4_key *key, const reiser4_key *rdkey)
{
	int result;
	carry_pool *pool;
	carry_level *todo;
	reiser4_item_data *item;
	carry_insert_data *cdata;
	carry_op *op;
	znode *node;
	reiser4_tree *tree;

	assert("vs-49827", znode_contains_key_lock(insert_coord->node, key));
	tree = znode_get_tree(insert_coord->node);
	node = reiser4_new_node(insert_coord->node, LEAF_LEVEL);
	if (IS_ERR(node))
		return PTR_ERR(node);

	/* setup delimiting keys for node being inserted */
	write_lock_dk(tree);
	znode_set_ld_key(node, key);
	znode_set_rd_key(node, rdkey);
	ON_DEBUG(node->creator = current);
	ON_DEBUG(node->first_key = *key);
	write_unlock_dk(tree);

	ZF_SET(node, JNODE_ORPHAN);

	/*
	 * allocate carry_pool, 3 carry_level-s, reiser4_item_data and
	 * carry_insert_data
	 */
	pool = init_carry_pool(sizeof(*pool) + 3 * sizeof(*todo) +
			       sizeof(*item) + sizeof(*cdata));
	if (IS_ERR(pool))
		return PTR_ERR(pool);
	todo = (carry_level *) (pool + 1);
	init_carry_level(todo, pool);

	item = (reiser4_item_data *) (todo + 3);
	cdata = (carry_insert_data *) (item + 1);

	op = reiser4_post_carry(todo, COP_INSERT, insert_coord->node, 0);
	if (!IS_ERR(op)) {
		cdata->coord = insert_coord;
		cdata->key = key;
		cdata->data = item;
		op->u.insert.d = cdata;
		op->u.insert.type = COPT_ITEM_DATA;
		build_child_ptr_data(node, item);
		item->arg = NULL;
		/* have @insert_coord to be set at inserted item after
		   insertion is done */
		todo->track_type = CARRY_TRACK_CHANGE;
		todo->tracked = lh;

		result = reiser4_carry(todo, NULL);
		if (result == 0) {
			/*
			 * pin node in memory. This is necessary for
			 * znode_make_dirty() below.
			 */
			result = zload(node);
			if (result == 0) {
				lock_handle local_lh;

				/*
				 * if we inserted new child into tree we have
				 * to mark it dirty so that flush will be able
				 * to process it.
				 */
				init_lh(&local_lh);
				result = longterm_lock_znode(&local_lh, node,
							     ZNODE_WRITE_LOCK,
							     ZNODE_LOCK_LOPRI);
				if (result == 0) {
					znode_make_dirty(node);

					/*
					 * when internal item pointing to @node
					 * was inserted into twig node
					 * create_hook_internal did not connect
					 * it properly because its right
					 * neighbor was not known. Do it
					 * here
					 */
					write_lock_tree(tree);
					assert("nikita-3312",
					       znode_is_right_connected(node));
					assert("nikita-2984",
					       node->right == NULL);
					ZF_CLR(node, JNODE_RIGHT_CONNECTED);
					write_unlock_tree(tree);
					result =
					    connect_znode(insert_coord, node);
					ON_DEBUG(if (result == 0) check_dkeys(node););

					done_lh(lh);
					move_lh(lh, &local_lh);
					assert("vs-1676", node_is_empty(node));
					coord_init_first_unit(insert_coord,
							      node);
				} else {
					warning("nikita-3136",
						"Cannot lock child");
				}
				done_lh(&local_lh);
				zrelse(node);
			}
		}
	} else
		result = PTR_ERR(op);
	zput(node);
	done_carry_pool(pool);
	return result;
}

/**
 * handle_eottl - handle extent-on-the-twig-level cases in tree traversal
 * @h: search handle
 * @outcome: flag saying whether search has to restart or is done
 *
 * Handles search on twig level. If this function completes search itself then
 * it returns 1. If search has to go one level down then 0 is returned. If
 * error happens then LOOKUP_DONE is returned via @outcome and error code is
 * saved in @h->result.
 */
int handle_eottl(cbk_handle *h, int *outcome)
{
	int result;
	reiser4_key key;
	coord_t *coord;

	coord = h->coord;

	if (h->level != TWIG_LEVEL ||
	    (coord_is_existing_item(coord) && item_is_internal(coord))) {
		/* Continue to traverse tree downward. */
		return 0;
	}

	/*
	 * make sure that @h->coord is set to twig node and that it is either
	 * set to extent item or after extent item
	 */
	assert("vs-356", h->level == TWIG_LEVEL);
	assert("vs-357", ({
			  coord_t lcoord;
			  coord_dup(&lcoord, coord);
			  check_me("vs-733", coord_set_to_left(&lcoord) == 0);
			  item_is_extent(&lcoord);
			  }
	       ));

	if (*outcome == NS_FOUND) {
		/* we have found desired key on twig level in extent item */
		h->result = CBK_COORD_FOUND;
		*outcome = LOOKUP_DONE;
		return 1;
	}

	if (!(h->flags & CBK_FOR_INSERT)) {
		/* tree traversal is not for insertion. Just return
		   CBK_COORD_NOTFOUND. */
		h->result = CBK_COORD_NOTFOUND;
		*outcome = LOOKUP_DONE;
		return 1;
	}

	/* take a look at the item to the right of h -> coord */
	result = is_next_item_internal(coord, h->key, h->active_lh);
	if (unlikely(result < 0)) {
		h->error = "get_right_neighbor failed";
		h->result = result;
		*outcome = LOOKUP_DONE;
		return 1;
	}
	if (result == 0) {
		/*
		 * item to the right is also an extent one. Allocate a new node
		 * and insert pointer to it after item h -> coord.
		 *
		 * This is a result of extents being located at the twig
		 * level. For explanation, see comment just above
		 * is_next_item_internal().
		 */
		znode *loaded;

		if (cbk_lock_mode(h->level, h) != ZNODE_WRITE_LOCK) {
			/*
			 * we got node read locked, restart coord_by_key to
			 * have write lock on twig level
			 */
			h->lock_level = TWIG_LEVEL;
			h->lock_mode = ZNODE_WRITE_LOCK;
			*outcome = LOOKUP_REST;
			return 1;
		}

		loaded = coord->node;
		result =
		    add_empty_leaf(coord, h->active_lh, h->key,
				   rd_key(coord, &key));
		if (result) {
			h->error = "could not add empty leaf";
			h->result = result;
			*outcome = LOOKUP_DONE;
			return 1;
		}
		/* added empty leaf is locked (h->active_lh), its parent node
		   is unlocked, h->coord is set as EMPTY */
		assert("vs-13", coord->between == EMPTY_NODE);
		assert("vs-14", znode_is_write_locked(coord->node));
		assert("vs-15",
		       WITH_DATA(coord->node, node_is_empty(coord->node)));
		assert("vs-16", jnode_is_leaf(ZJNODE(coord->node)));
		assert("vs-17", coord->node == h->active_lh->node);
		*outcome = LOOKUP_DONE;
		h->result = CBK_COORD_NOTFOUND;
		return 1;
	} else if (result == 1) {
		/*
		 * this is special case mentioned in the comment on
		 * tree.h:cbk_flags. We have found internal item immediately on
		 * the right of extent, and we are going to insert new item
		 * there. Key of item we are going to insert is smaller than
		 * leftmost key in the node pointed to by said internal item
		 * (otherwise search wouldn't come to the extent in the first
		 * place).
		 *
		 * This is a result of extents being located at the twig
		 * level. For explanation, see comment just above
		 * is_next_item_internal().
		 */
		h->flags &= ~CBK_TRUST_DK;
	} else {
		assert("vs-8", result == 2);
		*outcome = LOOKUP_REST;
		return 1;
	}
	assert("vs-362", WITH_DATA(coord->node, item_is_internal(coord)));
	return 0;
}

/*
 * Local variables:
 * c-indentation-style: "K&R"
 * mode-name: "LC"
 * c-basic-offset: 8
 * tab-width: 8
 * fill-column: 120
 * scroll-step: 1
 * End:
 */
