/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

/*
 * KEYS IN A TREE.
 *
 * The tree consists of nodes located on the disk. Node in the tree is either
 * formatted or unformatted. Formatted node is one that has structure
 * understood by the tree balancing and traversal code. Formatted nodes are
 * further classified into leaf and internal nodes. Latter distinctions is
 * (almost) of only historical importance: general structure of leaves and
 * internal nodes is the same in Reiser4. Unformatted nodes contain raw data
 * that are part of bodies of ordinary files and attributes.
 *
 * Each node in the tree spawns some interval in the key space. Key ranges for
 * all nodes in the tree are disjoint. Actually, this only holds in some weak
 * sense, because of the non-unique keys: intersection of key ranges for
 * different nodes is either empty, or consists of exactly one key.
 *
 * Formatted node consists of a sequence of items. Each item spawns some
 * interval in key space. Key ranges for all items in a tree are disjoint,
 * modulo non-unique keys again. Items within nodes are ordered in the key
 * order of the smallest key in a item.
 *
 * Particular type of item can be further split into units. Unit is piece of
 * item that can be cut from item and moved into another item of the same
 * time. Units are used by balancing code to repack data during balancing.
 *
 * Unit can be further split into smaller entities (for example, extent unit
 * represents several pages, and it is natural for extent code to operate on
 * particular pages and even bytes within one unit), but this is of no
 * relevance to the generic balancing and lookup code.
 *
 * Although item is said to "spawn" range or interval of keys, it is not
 * necessary that item contains piece of data addressable by each and every
 * key in this range. For example, compound directory item, consisting of
 * units corresponding to directory entries and keyed by hashes of file names,
 * looks more as having "discrete spectrum": only some disjoint keys inside
 * range occupied by this item really address data.
 *
 * No than less, each item always has well-defined least (minimal) key, that
 * is recorded in item header, stored in the node this item is in. Also, item
 * plugin can optionally define method ->max_key_inside() returning maximal
 * key that can _possibly_ be located within this item. This method is used
 * (mainly) to determine when given piece of data should be merged into
 * existing item, in stead of creating new one. Because of this, even though
 * ->max_key_inside() can be larger that any key actually located in the item,
 * intervals
 *
 * [ reiser4_min_key( item ), ->max_key_inside( item ) ]
 *
 * are still disjoint for all items within the _same_ node.
 *
 * In memory node is represented by znode. It plays several roles:
 *
 *  . something locks are taken on
 *
 *  . something tracked by transaction manager (this is going to change)
 *
 *  . something used to access node data
 *
 *  . something used to maintain tree structure in memory: sibling and
 *  parental linkage.
 *
 *  . something used to organize nodes into "slums"
 *
 * More on znodes see in znode.[ch]
 *
 * DELIMITING KEYS
 *
 *   To simplify balancing, allow some flexibility in locking and speed up
 *   important coord cache optimization, we keep delimiting keys of nodes in
 *   memory. Depending on disk format (implemented by appropriate node plugin)
 *   node on disk can record both left and right delimiting key, only one of
 *   them, or none. Still, our balancing and tree traversal code keep both
 *   delimiting keys for a node that is in memory stored in the znode. When
 *   node is first brought into memory during tree traversal, its left
 *   delimiting key is taken from its parent, and its right delimiting key is
 *   either next key in its parent, or is right delimiting key of parent if
 *   node is the rightmost child of parent.
 *
 *   Physical consistency of delimiting key is protected by special dk
 *   read-write lock. That is, delimiting keys can only be inspected or
 *   modified under this lock. But dk lock is only sufficient for fast
 *   "pessimistic" check, because to simplify code and to decrease lock
 *   contention, balancing (carry) only updates delimiting keys right before
 *   unlocking all locked nodes on the given tree level. For example,
 *   coord-by-key cache scans LRU list of recently accessed znodes. For each
 *   node it first does fast check under dk spin lock. If key looked for is
 *   not between delimiting keys for this node, next node is inspected and so
 *   on. If key is inside of the key range, long term lock is taken on node
 *   and key range is rechecked.
 *
 * COORDINATES
 *
 *   To find something in the tree, you supply a key, and the key is resolved
 *   by coord_by_key() into a coord (coordinate) that is valid as long as the
 *   node the coord points to remains locked.  As mentioned above trees
 *   consist of nodes that consist of items that consist of units. A unit is
 *   the smallest and indivisible piece of tree as far as balancing and tree
 *   search are concerned. Each node, item, and unit can be addressed by
 *   giving its level in the tree and the key occupied by this entity.  A node
 *   knows what the key ranges are of the items within it, and how to find its
 *   items and invoke their item handlers, but it does not know how to access
 *   individual units within its items except through the item handlers.
 *   coord is a structure containing a pointer to the node, the ordinal number
 *   of the item within this node (a sort of item offset), and the ordinal
 *   number of the unit within this item.
 *
 * TREE LOOKUP
 *
 *   There are two types of access to the tree: lookup and modification.
 *
 *   Lookup is a search for the key in the tree. Search can look for either
 *   exactly the key given to it, or for the largest key that is not greater
 *   than the key given to it. This distinction is determined by "bias"
 *   parameter of search routine (coord_by_key()). coord_by_key() either
 *   returns error (key is not in the tree, or some kind of external error
 *   occurred), or successfully resolves key into coord.
 *
 *   This resolution is done by traversing tree top-to-bottom from root level
 *   to the desired level. On levels above twig level (level one above the
 *   leaf level) nodes consist exclusively of internal items. Internal item is
 *   nothing more than pointer to the tree node on the child level. On twig
 *   level nodes consist of internal items intermixed with extent
 *   items. Internal items form normal search tree structure used by traversal
 *   to descent through the tree.
 *
 * TREE LOOKUP OPTIMIZATIONS
 *
 * Tree lookup described above is expensive even if all nodes traversed are
 * already in the memory: for each node binary search within it has to be
 * performed and binary searches are CPU consuming and tend to destroy CPU
 * caches.
 *
 * Several optimizations are used to work around this:
 *
 *   . cbk_cache (look-aside cache for tree traversals, see search.c for
 *   details)
 *
 *   . seals (see seal.[ch])
 *
 *   . vroot (see search.c)
 *
 * General search-by-key is layered thusly:
 *
 *                   [check seal, if any]   --ok--> done
 *                           |
 *                         failed
 *                           |
 *                           V
 *                     [vroot defined] --no--> node = tree_root
 *                           |                   |
 *                          yes                  |
 *                           |                   |
 *                           V                   |
 *                       node = vroot            |
 *                                 |             |
 *                                 |             |
 *                                 |             |
 *                                 V             V
 *                            [check cbk_cache for key]  --ok--> done
 *                                        |
 *                                      failed
 *                                        |
 *                                        V
 *                       [start tree traversal from node]
 *
 */

#include "forward.h"
#include "debug.h"
#include "dformat.h"
#include "key.h"
#include "coord.h"
#include "plugin/item/static_stat.h"
#include "plugin/item/item.h"
#include "plugin/node/node.h"
#include "plugin/plugin.h"
#include "txnmgr.h"
#include "jnode.h"
#include "znode.h"
#include "block_alloc.h"
#include "tree_walk.h"
#include "carry.h"
#include "carry_ops.h"
#include "tap.h"
#include "tree.h"
#include "vfs_ops.h"
#include "page_cache.h"
#include "super.h"
#include "reiser4.h"
#include "inode.h"

#include <linux/fs.h>		/* for struct super_block  */
#include <linux/spinlock.h>

/* Disk address (block number) never ever used for any real tree node. This is
   used as block number of "uber" znode.

   Invalid block addresses are 0 by tradition.

*/
const reiser4_block_nr UBER_TREE_ADDR = 0ull;

#define CUT_TREE_MIN_ITERATIONS 64

static int find_child_by_addr(znode * parent, znode * child, coord_t *result);

/* return node plugin of coord->node */
node_plugin *node_plugin_by_coord(const coord_t *coord)
{
	assert("vs-1", coord != NULL);
	assert("vs-2", coord->node != NULL);

	return coord->node->nplug;
}

/* insert item into tree. Fields of @coord are updated so that they can be
 * used by consequent insert operation. */
insert_result insert_by_key(reiser4_tree * tree	/* tree to insert new item
						 * into */ ,
			    const reiser4_key * key /* key of new item */ ,
			    reiser4_item_data * data	/* parameters for item
							 * creation */ ,
			    coord_t *coord /* resulting insertion coord */ ,
			    lock_handle * lh	/* resulting lock
						 * handle */ ,
			    tree_level stop_level /* level where to insert */ ,
			    __u32 flags/* insertion flags */)
{
	int result;

	assert("nikita-358", tree != NULL);
	assert("nikita-360", coord != NULL);

	result = coord_by_key(tree, key, coord, lh, ZNODE_WRITE_LOCK,
			      FIND_EXACT, stop_level, stop_level,
			      flags | CBK_FOR_INSERT, NULL/*ra_info */);
	switch (result) {
	default:
		break;
	case CBK_COORD_FOUND:
		result = IBK_ALREADY_EXISTS;
		break;
	case CBK_COORD_NOTFOUND:
		assert("nikita-2017", coord->node != NULL);
		result = insert_by_coord(coord, data, key, lh, 0/*flags */);
		break;
	}
	return result;
}

/* insert item by calling carry. Helper function called if short-cut
   insertion failed  */
static insert_result insert_with_carry_by_coord(coord_t *coord,
					/* coord where to insert */
						lock_handle * lh,
					/* lock handle of insertion node */
						reiser4_item_data * data,
					/* parameters of new item */
						const reiser4_key * key,
					/* key of new item */
						carry_opcode cop,
					/* carry operation to perform */
						cop_insert_flag flags
					/* carry flags */ )
{
	int result;
	carry_pool *pool;
	carry_level *lowest_level;
	carry_insert_data *cdata;
	carry_op *op;

	assert("umka-314", coord != NULL);

	/* allocate carry_pool and 3 carry_level-s */
	pool =
	    init_carry_pool(sizeof(*pool) + 3 * sizeof(*lowest_level) +
			    sizeof(*cdata));
	if (IS_ERR(pool))
		return PTR_ERR(pool);
	lowest_level = (carry_level *) (pool + 1);
	init_carry_level(lowest_level, pool);

	op = reiser4_post_carry(lowest_level, cop, coord->node, 0);
	if (IS_ERR(op) || (op == NULL)) {
		done_carry_pool(pool);
		return RETERR(op ? PTR_ERR(op) : -EIO);
	}
	cdata = (carry_insert_data *) (lowest_level + 3);
	cdata->coord = coord;
	cdata->data = data;
	cdata->key = key;
	op->u.insert.d = cdata;
	if (flags == 0)
		flags = znode_get_tree(coord->node)->carry.insert_flags;
	op->u.insert.flags = flags;
	op->u.insert.type = COPT_ITEM_DATA;
	op->u.insert.child = NULL;
	if (lh != NULL) {
		assert("nikita-3245", lh->node == coord->node);
		lowest_level->track_type = CARRY_TRACK_CHANGE;
		lowest_level->tracked = lh;
	}

	result = reiser4_carry(lowest_level, NULL);
	done_carry_pool(pool);

	return result;
}

/* form carry queue to perform paste of @data with @key at @coord, and launch
   its execution by calling carry().

   Instruct carry to update @lh it after balancing insertion coord moves into
   different block.

*/
static int paste_with_carry(coord_t *coord,	/* coord of paste */
			    lock_handle * lh,	/* lock handle of node
						 * where item is
						 * pasted */
			    reiser4_item_data * data,	/* parameters of new
							 * item */
			    const reiser4_key * key,	/* key of new item */
			    unsigned flags/* paste flags */)
{
	int result;
	carry_pool *pool;
	carry_level *lowest_level;
	carry_insert_data *cdata;
	carry_op *op;

	assert("umka-315", coord != NULL);
	assert("umka-316", key != NULL);

	pool =
	    init_carry_pool(sizeof(*pool) + 3 * sizeof(*lowest_level) +
			    sizeof(*cdata));
	if (IS_ERR(pool))
		return PTR_ERR(pool);
	lowest_level = (carry_level *) (pool + 1);
	init_carry_level(lowest_level, pool);

	op = reiser4_post_carry(lowest_level, COP_PASTE, coord->node, 0);
	if (IS_ERR(op) || (op == NULL)) {
		done_carry_pool(pool);
		return RETERR(op ? PTR_ERR(op) : -EIO);
	}
	cdata = (carry_insert_data *) (lowest_level + 3);
	cdata->coord = coord;
	cdata->data = data;
	cdata->key = key;
	op->u.paste.d = cdata;
	if (flags == 0)
		flags = znode_get_tree(coord->node)->carry.paste_flags;
	op->u.paste.flags = flags;
	op->u.paste.type = COPT_ITEM_DATA;
	if (lh != NULL) {
		lowest_level->track_type = CARRY_TRACK_CHANGE;
		lowest_level->tracked = lh;
	}

	result = reiser4_carry(lowest_level, NULL);
	done_carry_pool(pool);

	return result;
}

/* insert item at the given coord.

   First try to skip carry by directly calling ->create_item() method of node
   plugin. If this is impossible (there is not enough free space in the node,
   or leftmost item in the node is created), call insert_with_carry_by_coord()
   that will do full carry().

*/
insert_result insert_by_coord(coord_t *coord	/* coord where to
						 * insert. coord->node has
						 * to be write locked by
						 * caller */ ,
			      reiser4_item_data * data	/* data to be
							 * inserted */ ,
			      const reiser4_key * key /* key of new item */ ,
			      lock_handle * lh	/* lock handle of write
						 * lock on node */ ,
			      __u32 flags/* insertion flags */)
{
	unsigned item_size;
	int result;
	znode *node;

	assert("vs-247", coord != NULL);
	assert("vs-248", data != NULL);
	assert("vs-249", data->length >= 0);
	assert("nikita-1191", znode_is_write_locked(coord->node));

	node = coord->node;
	coord_clear_iplug(coord);
	result = zload(node);
	if (result != 0)
		return result;

	item_size = space_needed(node, NULL, data, 1);
	if (item_size > znode_free_space(node) &&
	    (flags & COPI_DONT_SHIFT_LEFT) && (flags & COPI_DONT_SHIFT_RIGHT)
	    && (flags & COPI_DONT_ALLOCATE)) {
		/* we are forced to use free space of coord->node and new item
		   does not fit into it.

		   Currently we get here only when we allocate and copy units
		   of extent item from a node to its left neighbor during
		   "squalloc"-ing.  If @node (this is left neighbor) does not
		   have enough free space - we do not want to attempt any
		   shifting and allocations because we are in squeezing and
		   everything to the left of @node is tightly packed.
		 */
		result = -E_NODE_FULL;
	} else if ((item_size <= znode_free_space(node)) &&
		   !coord_is_before_leftmost(coord) &&
		   (node_plugin_by_node(node)->fast_insert != NULL)
		   && node_plugin_by_node(node)->fast_insert(coord)) {
		/* shortcut insertion without carry() overhead.

		   Only possible if:

		   - there is enough free space

		   - insertion is not into the leftmost position in a node
		   (otherwise it would require updating of delimiting key in a
		   parent)

		   - node plugin agrees with this

		 */
		result =
		    node_plugin_by_node(node)->create_item(coord, key, data,
							   NULL);
		znode_make_dirty(node);
	} else {
		/* otherwise do full-fledged carry(). */
		result =
		    insert_with_carry_by_coord(coord, lh, data, key, COP_INSERT,
					       flags);
	}
	zrelse(node);
	return result;
}

/* @coord is set to leaf level and @data is to be inserted to twig level */
insert_result
insert_extent_by_coord(coord_t *coord,         /* coord where to insert.
				                * coord->node has to be write
					        * locked by caller */
		       reiser4_item_data *data,/* data to be inserted */
		       const reiser4_key *key, /* key of new item */
		       lock_handle *lh         /* lock handle of write lock
						  on node */)
{
	assert("vs-405", coord != NULL);
	assert("vs-406", data != NULL);
	assert("vs-407", data->length > 0);
	assert("vs-408", znode_is_write_locked(coord->node));
	assert("vs-409", znode_get_level(coord->node) == LEAF_LEVEL);

	return insert_with_carry_by_coord(coord, lh, data, key, COP_EXTENT,
					  0 /*flags */ );
}

/* Insert into the item at the given coord.

   First try to skip carry by directly calling ->paste() method of item
   plugin. If this is impossible (there is not enough free space in the node,
   or we are pasting into leftmost position in the node), call
   paste_with_carry() that will do full carry().

*/
/* paste_into_item */
int insert_into_item(coord_t * coord /* coord of pasting */ ,
		     lock_handle * lh /* lock handle on node involved */ ,
		     const reiser4_key * key /* key of unit being pasted */ ,
		     reiser4_item_data * data /* parameters for new unit */ ,
		     unsigned flags /* insert/paste flags */ )
{
	int result;
	int size_change;
	node_plugin *nplug;
	item_plugin *iplug;

	assert("umka-317", coord != NULL);
	assert("umka-318", key != NULL);

	iplug = item_plugin_by_coord(coord);
	nplug = node_plugin_by_coord(coord);

	assert("nikita-1480", iplug == data->iplug);

	size_change = space_needed(coord->node, coord, data, 0);
	if (size_change > (int)znode_free_space(coord->node) &&
	    (flags & COPI_DONT_SHIFT_LEFT) && (flags & COPI_DONT_SHIFT_RIGHT)
	    && (flags & COPI_DONT_ALLOCATE)) {
		/* we are forced to use free space of coord->node and new data
		   does not fit into it. */
		return -E_NODE_FULL;
	}

	/* shortcut paste without carry() overhead.

	   Only possible if:

	   - there is enough free space

	   - paste is not into the leftmost unit in a node (otherwise
	   it would require updating of delimiting key in a parent)

	   - node plugin agrees with this

	   - item plugin agrees with us
	 */
	if (size_change <= (int)znode_free_space(coord->node) &&
	    (coord->item_pos != 0 ||
	     coord->unit_pos != 0 || coord->between == AFTER_UNIT) &&
	    coord->unit_pos != 0 && nplug->fast_paste != NULL &&
	    nplug->fast_paste(coord) &&
	    iplug->b.fast_paste != NULL && iplug->b.fast_paste(coord)) {
		if (size_change > 0)
			nplug->change_item_size(coord, size_change);
		/* NOTE-NIKITA: huh? where @key is used? */
		result = iplug->b.paste(coord, data, NULL);
		if (size_change < 0)
			nplug->change_item_size(coord, size_change);
		znode_make_dirty(coord->node);
	} else
		/* otherwise do full-fledged carry(). */
		result = paste_with_carry(coord, lh, data, key, flags);
	return result;
}

/* this either appends or truncates item @coord */
int reiser4_resize_item(coord_t * coord /* coord of item being resized */ ,
			reiser4_item_data * data /* parameters of resize */ ,
			reiser4_key * key /* key of new unit */ ,
			lock_handle * lh	/* lock handle of node
						 * being modified */ ,
			cop_insert_flag flags /* carry flags */ )
{
	int result;
	znode *node;

	assert("nikita-362", coord != NULL);
	assert("nikita-363", data != NULL);
	assert("vs-245", data->length != 0);

	node = coord->node;
	coord_clear_iplug(coord);
	result = zload(node);
	if (result != 0)
		return result;

	if (data->length < 0)
		result = node_plugin_by_coord(coord)->shrink_item(coord,
								  -data->length);
	else
		result = insert_into_item(coord, lh, key, data, flags);

	zrelse(node);
	return result;
}

/* insert flow @f */
int reiser4_insert_flow(coord_t * coord, lock_handle * lh, flow_t * f)
{
	int result;
	carry_pool *pool;
	carry_level *lowest_level;
	reiser4_item_data *data;
	carry_op *op;

	pool =
	    init_carry_pool(sizeof(*pool) + 3 * sizeof(*lowest_level) +
			    sizeof(*data));
	if (IS_ERR(pool))
		return PTR_ERR(pool);
	lowest_level = (carry_level *) (pool + 1);
	init_carry_level(lowest_level, pool);

	op = reiser4_post_carry(lowest_level, COP_INSERT_FLOW, coord->node,
			0 /* operate directly on coord -> node */ );
	if (IS_ERR(op) || (op == NULL)) {
		done_carry_pool(pool);
		return RETERR(op ? PTR_ERR(op) : -EIO);
	}

	/* these are permanent during insert_flow */
	data = (reiser4_item_data *) (lowest_level + 3);
	data->user = 1;
	data->iplug = item_plugin_by_id(FORMATTING_ID);
	data->arg = NULL;
	/* data.length and data.data will be set before calling paste or
	   insert */
	data->length = 0;
	data->data = NULL;

	op->u.insert_flow.flags = 0;
	op->u.insert_flow.insert_point = coord;
	op->u.insert_flow.flow = f;
	op->u.insert_flow.data = data;
	op->u.insert_flow.new_nodes = 0;

	lowest_level->track_type = CARRY_TRACK_CHANGE;
	lowest_level->tracked = lh;

	result = reiser4_carry(lowest_level, NULL);
	done_carry_pool(pool);

	return result;
}

/* Given a coord in parent node, obtain a znode for the corresponding child */
znode *child_znode(const coord_t * parent_coord	/* coord of pointer to
						 * child */ ,
		   znode * parent /* parent of child */ ,
		   int incore_p	/* if !0 only return child if already in
				 * memory */ ,
		   int setup_dkeys_p	/* if !0 update delimiting keys of
					 * child */ )
{
	znode *child;

	assert("nikita-1374", parent_coord != NULL);
	assert("nikita-1482", parent != NULL);
#if REISER4_DEBUG
	if (setup_dkeys_p)
		assert_rw_not_locked(&(znode_get_tree(parent)->dk_lock));
#endif
	assert("nikita-2947", znode_is_any_locked(parent));

	if (znode_get_level(parent) <= LEAF_LEVEL) {
		/* trying to get child of leaf node */
		warning("nikita-1217", "Child of maize?");
		return ERR_PTR(RETERR(-EIO));
	}
	if (item_is_internal(parent_coord)) {
		reiser4_block_nr addr;
		item_plugin *iplug;
		reiser4_tree *tree;

		iplug = item_plugin_by_coord(parent_coord);
		assert("vs-512", iplug->s.internal.down_link);
		iplug->s.internal.down_link(parent_coord, NULL, &addr);

		tree = znode_get_tree(parent);
		if (incore_p)
			child = zlook(tree, &addr);
		else
			child =
			    zget(tree, &addr, parent,
				 znode_get_level(parent) - 1,
				 reiser4_ctx_gfp_mask_get());
		if ((child != NULL) && !IS_ERR(child) && setup_dkeys_p)
			set_child_delimiting_keys(parent, parent_coord, child);
	} else {
		warning("nikita-1483", "Internal item expected");
		child = ERR_PTR(RETERR(-EIO));
	}
	return child;
}

/* remove znode from transaction */
static void uncapture_znode(znode * node)
{
	struct page *page;

	assert("zam-1001", ZF_ISSET(node, JNODE_HEARD_BANSHEE));

	if (!reiser4_blocknr_is_fake(znode_get_block(node))) {
		int ret;

		/* An already allocated block goes right to the atom's delete set. */
		ret =
		    reiser4_dealloc_block(znode_get_block(node), 0,
					  BA_DEFER | BA_FORMATTED);
		if (ret)
			warning("zam-942",
				"can\'t add a block (%llu) number to atom's delete set\n",
				(unsigned long long)(*znode_get_block(node)));

		spin_lock_znode(node);
		/* Here we return flush reserved block which was reserved at the
		 * moment when this allocated node was marked dirty and still
		 * not used by flush in node relocation procedure.  */
		if (ZF_ISSET(node, JNODE_FLUSH_RESERVED)) {
			txn_atom *atom;

			atom = jnode_get_atom(ZJNODE(node));
			assert("zam-939", atom != NULL);
			spin_unlock_znode(node);
			flush_reserved2grabbed(atom, (__u64) 1);
			spin_unlock_atom(atom);
		} else
			spin_unlock_znode(node);
	} else {
		/* znode has assigned block which is counted as "fake
		   allocated". Return it back to "free blocks") */
		fake_allocated2free((__u64) 1, BA_FORMATTED);
	}

	/*
	 * uncapture page from transaction. There is a possibility of a race
	 * with ->releasepage(): reiser4_releasepage() detaches page from this
	 * jnode and we have nothing to uncapture. To avoid this, get
	 * reference of node->pg under jnode spin lock. reiser4_uncapture_page()
	 * will deal with released page itself.
	 */
	spin_lock_znode(node);
	page = znode_page(node);
	if (likely(page != NULL)) {
		/*
		 * reiser4_uncapture_page() can only be called when we are sure
		 * that znode is pinned in memory, which we are, because
		 * forget_znode() is only called from longterm_unlock_znode().
		 */
		page_cache_get(page);
		spin_unlock_znode(node);
		lock_page(page);
		reiser4_uncapture_page(page);
		unlock_page(page);
		page_cache_release(page);
	} else {
		txn_atom *atom;

		/* handle "flush queued" znodes */
		while (1) {
			atom = jnode_get_atom(ZJNODE(node));
			assert("zam-943", atom != NULL);

			if (!ZF_ISSET(node, JNODE_FLUSH_QUEUED)
			    || !atom->nr_running_queues)
				break;

			spin_unlock_znode(node);
			reiser4_atom_wait_event(atom);
			spin_lock_znode(node);
		}

		reiser4_uncapture_block(ZJNODE(node));
		spin_unlock_atom(atom);
		zput(node);
	}
}

/* This is called from longterm_unlock_znode() when last lock is released from
   the node that has been removed from the tree. At this point node is removed
   from sibling list and its lock is invalidated. */
void forget_znode(lock_handle * handle)
{
	znode *node;
	reiser4_tree *tree;

	assert("umka-319", handle != NULL);

	node = handle->node;
	tree = znode_get_tree(node);

	assert("vs-164", znode_is_write_locked(node));
	assert("nikita-1280", ZF_ISSET(node, JNODE_HEARD_BANSHEE));
	assert_rw_locked(&(node->lock.guard));

	/* We assume that this node was detached from its parent before
	 * unlocking, it gives no way to reach this node from parent through a
	 * down link.  The node should have no children and, thereby, can't be
	 * reached from them by their parent pointers.  The only way to obtain a
	 * reference to the node is to use sibling pointers from its left and
	 * right neighbors.  In the next several lines we remove the node from
	 * the sibling list. */

	write_lock_tree(tree);
	sibling_list_remove(node);
	znode_remove(node, tree);
	write_unlock_tree(tree);

	/* Here we set JNODE_DYING and cancel all pending lock requests.  It
	 * forces all lock requestor threads to repeat iterations of getting
	 * lock on a child, neighbor or parent node.  But, those threads can't
	 * come to this node again, because this node is no longer a child,
	 * neighbor or parent of any other node.  This order of znode
	 * invalidation does not allow other threads to waste cpu time is a busy
	 * loop, trying to lock dying object.  The exception is in the flush
	 * code when we take node directly from atom's capture list.*/
	reiser4_invalidate_lock(handle);
	uncapture_znode(node);
}

/* Check that internal item at @pointer really contains pointer to @child. */
int check_tree_pointer(const coord_t * pointer	/* would-be pointer to
						 * @child */ ,
		       const znode * child /* child znode */ )
{
	assert("nikita-1016", pointer != NULL);
	assert("nikita-1017", child != NULL);
	assert("nikita-1018", pointer->node != NULL);

	assert("nikita-1325", znode_is_any_locked(pointer->node));

	assert("nikita-2985",
	       znode_get_level(pointer->node) == znode_get_level(child) + 1);

	coord_clear_iplug((coord_t *) pointer);

	if (coord_is_existing_unit(pointer)) {
		item_plugin *iplug;
		reiser4_block_nr addr;

		if (item_is_internal(pointer)) {
			iplug = item_plugin_by_coord(pointer);
			assert("vs-513", iplug->s.internal.down_link);
			iplug->s.internal.down_link(pointer, NULL, &addr);
			/* check that cached value is correct */
			if (disk_addr_eq(&addr, znode_get_block(child))) {
				return NS_FOUND;
			}
		}
	}
	/* warning ("jmacd-1002", "tree pointer incorrect"); */
	return NS_NOT_FOUND;
}

/* find coord of pointer to new @child in @parent.

   Find the &coord_t in the @parent where pointer to a given @child will
   be in.

*/
int find_new_child_ptr(znode * parent /* parent znode, passed locked */ ,
		       znode *
		       child UNUSED_ARG /* child znode, passed locked */ ,
		       znode * left /* left brother of new node */ ,
		       coord_t * result /* where result is stored in */ )
{
	int ret;

	assert("nikita-1486", parent != NULL);
	assert("nikita-1487", child != NULL);
	assert("nikita-1488", result != NULL);

	ret = find_child_ptr(parent, left, result);
	if (ret != NS_FOUND) {
		warning("nikita-1489", "Cannot find brother position: %i", ret);
		return RETERR(-EIO);
	} else {
		result->between = AFTER_UNIT;
		return RETERR(NS_NOT_FOUND);
	}
}

/* find coord of pointer to @child in @parent.

   Find the &coord_t in the @parent where pointer to a given @child is in.

*/
int find_child_ptr(znode * parent /* parent znode, passed locked */ ,
		   znode * child /* child znode, passed locked */ ,
		   coord_t * result /* where result is stored in */ )
{
	int lookup_res;
	node_plugin *nplug;
	/* left delimiting key of a child */
	reiser4_key ld;
	reiser4_tree *tree;

	assert("nikita-934", parent != NULL);
	assert("nikita-935", child != NULL);
	assert("nikita-936", result != NULL);
	assert("zam-356", znode_is_loaded(parent));

	coord_init_zero(result);
	result->node = parent;

	nplug = parent->nplug;
	assert("nikita-939", nplug != NULL);

	tree = znode_get_tree(parent);
	/* NOTE-NIKITA taking read-lock on tree here assumes that @result is
	 * not aliased to ->in_parent of some znode. Otherwise,
	 * parent_coord_to_coord() below would modify data protected by tree
	 * lock. */
	read_lock_tree(tree);
	/* fast path. Try to use cached value. Lock tree to keep
	   node->pos_in_parent and pos->*_blocknr consistent. */
	if (child->in_parent.item_pos + 1 != 0) {
		parent_coord_to_coord(&child->in_parent, result);
		if (check_tree_pointer(result, child) == NS_FOUND) {
			read_unlock_tree(tree);
			return NS_FOUND;
		}

		child->in_parent.item_pos = (unsigned short)~0;
	}
	read_unlock_tree(tree);

	/* is above failed, find some key from @child. We are looking for the
	   least key in a child. */
	read_lock_dk(tree);
	ld = *znode_get_ld_key(child);
	read_unlock_dk(tree);
	/*
	 * now, lookup parent with key just found. Note, that left delimiting
	 * key doesn't identify node uniquely, because (in extremely rare
	 * case) two nodes can have equal left delimiting keys, if one of them
	 * is completely filled with directory entries that all happened to be
	 * hash collision. But, we check block number in check_tree_pointer()
	 * and, so, are safe.
	 */
	lookup_res = nplug->lookup(parent, &ld, FIND_EXACT, result);
	/* update cached pos_in_node */
	if (lookup_res == NS_FOUND) {
		write_lock_tree(tree);
		coord_to_parent_coord(result, &child->in_parent);
		write_unlock_tree(tree);
		lookup_res = check_tree_pointer(result, child);
	}
	if (lookup_res == NS_NOT_FOUND)
		lookup_res = find_child_by_addr(parent, child, result);
	return lookup_res;
}

/* find coord of pointer to @child in @parent by scanning

   Find the &coord_t in the @parent where pointer to a given @child
   is in by scanning all internal items in @parent and comparing block
   numbers in them with that of @child.

*/
static int find_child_by_addr(znode * parent /* parent znode, passed locked */ ,
			      znode * child /* child znode, passed locked */ ,
			      coord_t * result /* where result is stored in */ )
{
	int ret;

	assert("nikita-1320", parent != NULL);
	assert("nikita-1321", child != NULL);
	assert("nikita-1322", result != NULL);

	ret = NS_NOT_FOUND;

	for_all_units(result, parent) {
		if (check_tree_pointer(result, child) == NS_FOUND) {
			write_lock_tree(znode_get_tree(parent));
			coord_to_parent_coord(result, &child->in_parent);
			write_unlock_tree(znode_get_tree(parent));
			ret = NS_FOUND;
			break;
		}
	}
	return ret;
}

/* true, if @addr is "unallocated block number", which is just address, with
   highest bit set. */
int is_disk_addr_unallocated(const reiser4_block_nr * addr	/* address to
								 * check */ )
{
	assert("nikita-1766", addr != NULL);
	cassert(sizeof(reiser4_block_nr) == 8);
	return (*addr & REISER4_BLOCKNR_STATUS_BIT_MASK) ==
	    REISER4_UNALLOCATED_STATUS_VALUE;
}

/* returns true if removing bytes of given range of key [from_key, to_key]
   causes removing of whole item @from */
static int
item_removed_completely(coord_t * from, const reiser4_key * from_key,
			const reiser4_key * to_key)
{
	item_plugin *iplug;
	reiser4_key key_in_item;

	assert("umka-325", from != NULL);
	assert("", item_is_extent(from));

	/* check first key just for case */
	item_key_by_coord(from, &key_in_item);
	if (keygt(from_key, &key_in_item))
		return 0;

	/* check last key */
	iplug = item_plugin_by_coord(from);
	assert("vs-611", iplug && iplug->s.file.append_key);

	iplug->s.file.append_key(from, &key_in_item);
	set_key_offset(&key_in_item, get_key_offset(&key_in_item) - 1);

	if (keylt(to_key, &key_in_item))
		/* last byte is not removed */
		return 0;
	return 1;
}

/* helper function for prepare_twig_kill(): @left and @right are formatted
 * neighbors of extent item being completely removed. Load and lock neighbors
 * and store lock handles into @cdata for later use by kill_hook_extent() */
static int
prepare_children(znode * left, znode * right, carry_kill_data * kdata)
{
	int result;
	int left_loaded;
	int right_loaded;

	result = 0;
	left_loaded = right_loaded = 0;

	if (left != NULL) {
		result = zload(left);
		if (result == 0) {
			left_loaded = 1;
			result = longterm_lock_znode(kdata->left, left,
						     ZNODE_READ_LOCK,
						     ZNODE_LOCK_LOPRI);
		}
	}
	if (result == 0 && right != NULL) {
		result = zload(right);
		if (result == 0) {
			right_loaded = 1;
			result = longterm_lock_znode(kdata->right, right,
						     ZNODE_READ_LOCK,
						     ZNODE_LOCK_HIPRI |
						     ZNODE_LOCK_NONBLOCK);
		}
	}
	if (result != 0) {
		done_lh(kdata->left);
		done_lh(kdata->right);
		if (left_loaded != 0)
			zrelse(left);
		if (right_loaded != 0)
			zrelse(right);
	}
	return result;
}

static void done_children(carry_kill_data * kdata)
{
	if (kdata->left != NULL && kdata->left->node != NULL) {
		zrelse(kdata->left->node);
		done_lh(kdata->left);
	}
	if (kdata->right != NULL && kdata->right->node != NULL) {
		zrelse(kdata->right->node);
		done_lh(kdata->right);
	}
}

/* part of cut_node. It is called when cut_node is called to remove or cut part
   of extent item. When head of that item is removed - we have to update right
   delimiting of left neighbor of extent. When item is removed completely - we
   have to set sibling link between left and right neighbor of removed
   extent. This may return -E_DEADLOCK because of trying to get left neighbor
   locked. So, caller should repeat an attempt
*/
/* Audited by: umka (2002.06.16) */
static int
prepare_twig_kill(carry_kill_data * kdata, znode * locked_left_neighbor)
{
	int result;
	reiser4_key key;
	lock_handle left_lh;
	lock_handle right_lh;
	coord_t left_coord;
	coord_t *from;
	znode *left_child;
	znode *right_child;
	reiser4_tree *tree;
	int left_zloaded_here, right_zloaded_here;

	from = kdata->params.from;
	assert("umka-326", from != NULL);
	assert("umka-327", kdata->params.to != NULL);

	/* for one extent item only yet */
	assert("vs-591", item_is_extent(from));
	assert("vs-592", from->item_pos == kdata->params.to->item_pos);

	if ((kdata->params.from_key
	     && keygt(kdata->params.from_key, item_key_by_coord(from, &key)))
	    || from->unit_pos != 0) {
		/* head of item @from is not removed, there is nothing to
		   worry about */
		return 0;
	}

	result = 0;
	left_zloaded_here = 0;
	right_zloaded_here = 0;

	left_child = right_child = NULL;

	coord_dup(&left_coord, from);
	init_lh(&left_lh);
	init_lh(&right_lh);
	if (coord_prev_unit(&left_coord)) {
		/* @from is leftmost item in its node */
		if (!locked_left_neighbor) {
			result =
			    reiser4_get_left_neighbor(&left_lh, from->node,
						      ZNODE_READ_LOCK,
						      GN_CAN_USE_UPPER_LEVELS);
			switch (result) {
			case 0:
				break;
			case -E_NO_NEIGHBOR:
				/* there is no formatted node to the left of
				   from->node */
				warning("vs-605",
					"extent item has smallest key in "
					"the tree and it is about to be removed");
				return 0;
			case -E_DEADLOCK:
				/* need to restart */
			default:
				return result;
			}

			/* we have acquired left neighbor of from->node */
			result = zload(left_lh.node);
			if (result)
				goto done;

			locked_left_neighbor = left_lh.node;
		} else {
			/* squalloc_right_twig_cut should have supplied locked
			 * left neighbor */
			assert("vs-834",
			       znode_is_write_locked(locked_left_neighbor));
			result = zload(locked_left_neighbor);
			if (result)
				return result;
		}

		left_zloaded_here = 1;
		coord_init_last_unit(&left_coord, locked_left_neighbor);
	}

	if (!item_is_internal(&left_coord)) {
		/* what else but extent can be on twig level */
		assert("vs-606", item_is_extent(&left_coord));

		/* there is no left formatted child */
		if (left_zloaded_here)
			zrelse(locked_left_neighbor);
		done_lh(&left_lh);
		return 0;
	}

	tree = znode_get_tree(left_coord.node);
	left_child = child_znode(&left_coord, left_coord.node, 1, 0);

	if (IS_ERR(left_child)) {
		result = PTR_ERR(left_child);
		goto done;
	}

	/* left child is acquired, calculate new right delimiting key for it
	   and get right child if it is necessary */
	if (item_removed_completely
	    (from, kdata->params.from_key, kdata->params.to_key)) {
		/* try to get right child of removed item */
		coord_t right_coord;

		assert("vs-607",
		       kdata->params.to->unit_pos ==
		       coord_last_unit_pos(kdata->params.to));
		coord_dup(&right_coord, kdata->params.to);
		if (coord_next_unit(&right_coord)) {
			/* @to is rightmost unit in the node */
			result =
			    reiser4_get_right_neighbor(&right_lh, from->node,
						       ZNODE_READ_LOCK,
						       GN_CAN_USE_UPPER_LEVELS);
			switch (result) {
			case 0:
				result = zload(right_lh.node);
				if (result)
					goto done;

				right_zloaded_here = 1;
				coord_init_first_unit(&right_coord,
						      right_lh.node);
				item_key_by_coord(&right_coord, &key);
				break;

			case -E_NO_NEIGHBOR:
				/* there is no formatted node to the right of
				   from->node */
				read_lock_dk(tree);
				key = *znode_get_rd_key(from->node);
				read_unlock_dk(tree);
				right_coord.node = NULL;
				result = 0;
				break;
			default:
				/* real error */
				goto done;
			}
		} else {
			/* there is an item to the right of @from - take its key */
			item_key_by_coord(&right_coord, &key);
		}

		/* try to get right child of @from */
		if (right_coord.node &&	/* there is right neighbor of @from */
		    item_is_internal(&right_coord)) {	/* it is internal item */
			right_child = child_znode(&right_coord,
						  right_coord.node, 1, 0);

			if (IS_ERR(right_child)) {
				result = PTR_ERR(right_child);
				goto done;
			}

		}
		/* whole extent is removed between znodes left_child and right_child. Prepare them for linking and
		   update of right delimiting key of left_child */
		result = prepare_children(left_child, right_child, kdata);
	} else {
		/* head of item @to is removed. left_child has to get right delimting key update. Prepare it for that */
		result = prepare_children(left_child, NULL, kdata);
	}

      done:
	if (right_child)
		zput(right_child);
	if (right_zloaded_here)
		zrelse(right_lh.node);
	done_lh(&right_lh);

	if (left_child)
		zput(left_child);
	if (left_zloaded_here)
		zrelse(locked_left_neighbor);
	done_lh(&left_lh);
	return result;
}

/* this is used to remove part of node content between coordinates @from and @to. Units to which @from and @to are set
   are to be cut completely */
/* for try_to_merge_with_left, delete_copied, reiser4_delete_node */
int cut_node_content(coord_t * from, coord_t * to, const reiser4_key * from_key,	/* first key to be removed */
		     const reiser4_key * to_key,	/* last key to be removed */
		     reiser4_key *
		     smallest_removed /* smallest key actually removed */ )
{
	int result;
	carry_pool *pool;
	carry_level *lowest_level;
	carry_cut_data *cut_data;
	carry_op *op;

	assert("vs-1715", coord_compare(from, to) != COORD_CMP_ON_RIGHT);

	pool =
	    init_carry_pool(sizeof(*pool) + 3 * sizeof(*lowest_level) +
			    sizeof(*cut_data));
	if (IS_ERR(pool))
		return PTR_ERR(pool);
	lowest_level = (carry_level *) (pool + 1);
	init_carry_level(lowest_level, pool);

	op = reiser4_post_carry(lowest_level, COP_CUT, from->node, 0);
	assert("vs-1509", op != 0);
	if (IS_ERR(op)) {
		done_carry_pool(pool);
		return PTR_ERR(op);
	}

	cut_data = (carry_cut_data *) (lowest_level + 3);
	cut_data->params.from = from;
	cut_data->params.to = to;
	cut_data->params.from_key = from_key;
	cut_data->params.to_key = to_key;
	cut_data->params.smallest_removed = smallest_removed;

	op->u.cut_or_kill.is_cut = 1;
	op->u.cut_or_kill.u.cut = cut_data;

	result = reiser4_carry(lowest_level, NULL);
	done_carry_pool(pool);

	return result;
}

/* cut part of the node

   Cut part or whole content of node.

   cut data between @from and @to of @from->node and call carry() to make
   corresponding changes in the tree. @from->node may become empty. If so -
   pointer to it will be removed. Neighboring nodes are not changed. Smallest
   removed key is stored in @smallest_removed

*/
int kill_node_content(coord_t * from,	/* coord of the first unit/item that will be eliminated */
		      coord_t * to,	/* coord of the last unit/item that will be eliminated */
		      const reiser4_key * from_key,	/* first key to be removed */
		      const reiser4_key * to_key,	/* last key to be removed */
		      reiser4_key * smallest_removed,	/* smallest key actually removed */
		      znode * locked_left_neighbor,	/* this is set when kill_node_content is called with left neighbor
							 * locked (in squalloc_right_twig_cut, namely) */
		      struct inode *inode,	/* inode of file whose item (or its part) is to be killed. This is necessary to
						   invalidate pages together with item pointing to them */
		      int truncate)
{				/* this call is made for file truncate)  */
	int result;
	carry_pool *pool;
	carry_level *lowest_level;
	carry_kill_data *kdata;
	lock_handle *left_child;
	lock_handle *right_child;
	carry_op *op;

	assert("umka-328", from != NULL);
	assert("vs-316", !node_is_empty(from->node));
	assert("nikita-1812", coord_is_existing_unit(from)
	       && coord_is_existing_unit(to));

	/* allocate carry_pool, 3 carry_level-s, carry_kill_data and structures for kill_hook_extent */
	pool = init_carry_pool(sizeof(*pool) + 3 * sizeof(*lowest_level) +
			       sizeof(carry_kill_data) +
			       2 * sizeof(lock_handle) +
			       5 * sizeof(reiser4_key) + 2 * sizeof(coord_t));
	if (IS_ERR(pool))
		return PTR_ERR(pool);

	lowest_level = (carry_level *) (pool + 1);
	init_carry_level(lowest_level, pool);

	kdata = (carry_kill_data *) (lowest_level + 3);
	left_child = (lock_handle *) (kdata + 1);
	right_child = left_child + 1;

	init_lh(left_child);
	init_lh(right_child);

	kdata->params.from = from;
	kdata->params.to = to;
	kdata->params.from_key = from_key;
	kdata->params.to_key = to_key;
	kdata->params.smallest_removed = smallest_removed;
	kdata->params.truncate = truncate;
	kdata->flags = 0;
	kdata->inode = inode;
	kdata->left = left_child;
	kdata->right = right_child;
	/* memory for 5 reiser4_key and 2 coord_t will be used in kill_hook_extent */
	kdata->buf = (char *)(right_child + 1);

	if (znode_get_level(from->node) == TWIG_LEVEL && item_is_extent(from)) {
		/* left child of extent item may have to get updated right
		   delimiting key and to get linked with right child of extent
		   @from if it will be removed completely */
		result = prepare_twig_kill(kdata, locked_left_neighbor);
		if (result) {
			done_children(kdata);
			done_carry_pool(pool);
			return result;
		}
	}

	op = reiser4_post_carry(lowest_level, COP_CUT, from->node, 0);
	if (IS_ERR(op) || (op == NULL)) {
		done_children(kdata);
		done_carry_pool(pool);
		return RETERR(op ? PTR_ERR(op) : -EIO);
	}

	op->u.cut_or_kill.is_cut = 0;
	op->u.cut_or_kill.u.kill = kdata;

	result = reiser4_carry(lowest_level, NULL);

	done_children(kdata);
	done_carry_pool(pool);
	return result;
}

void
fake_kill_hook_tail(struct inode *inode, loff_t start, loff_t end, int truncate)
{
	if (reiser4_inode_get_flag(inode, REISER4_HAS_MMAP)) {
		pgoff_t start_pg, end_pg;

		start_pg = start >> PAGE_CACHE_SHIFT;
		end_pg = (end - 1) >> PAGE_CACHE_SHIFT;

		if ((start & (PAGE_CACHE_SIZE - 1)) == 0) {
			/*
			 * kill up to the page boundary.
			 */
			assert("vs-123456", start_pg == end_pg);
			reiser4_invalidate_pages(inode->i_mapping, start_pg, 1,
						 truncate);
		} else if (start_pg != end_pg) {
			/*
			 * page boundary is within killed portion of node.
			 */
			assert("vs-654321", end_pg - start_pg == 1);
			reiser4_invalidate_pages(inode->i_mapping, end_pg,
						 end_pg - start_pg, 1);
		}
	}
	inode_sub_bytes(inode, end - start);
}

/**
 * Delete whole @node from the reiser4 tree without loading it.
 *
 * @left: locked left neighbor,
 * @node: node to be deleted,
 * @smallest_removed: leftmost key of deleted node,
 * @object: inode pointer, if we truncate a file body.
 * @truncate: true if called for file truncate.
 *
 * @return: 0 if success, error code otherwise.
 *
 * NOTE: if @object!=NULL we assume that @smallest_removed != NULL and it
 * contains the right value of the smallest removed key from the previous
 * cut_worker() iteration.  This is needed for proper accounting of
 * "i_blocks" and "i_bytes" fields of the @object.
 */
int reiser4_delete_node(znode * node, reiser4_key * smallest_removed,
			struct inode *object, int truncate)
{
	lock_handle parent_lock;
	coord_t cut_from;
	coord_t cut_to;
	reiser4_tree *tree;
	int ret;

	assert("zam-937", node != NULL);
	assert("zam-933", znode_is_write_locked(node));
	assert("zam-999", smallest_removed != NULL);

	init_lh(&parent_lock);

	ret = reiser4_get_parent(&parent_lock, node, ZNODE_WRITE_LOCK);
	if (ret)
		return ret;

	assert("zam-934", !znode_above_root(parent_lock.node));

	ret = zload(parent_lock.node);
	if (ret)
		goto failed_nozrelse;

	ret = find_child_ptr(parent_lock.node, node, &cut_from);
	if (ret)
		goto failed;

	/* decrement child counter and set parent pointer to NULL before
	   deleting the list from parent node because of checks in
	   internal_kill_item_hook (we can delete the last item from the parent
	   node, the parent node is going to be deleted and its c_count should
	   be zero). */

	tree = znode_get_tree(node);
	write_lock_tree(tree);
	init_parent_coord(&node->in_parent, NULL);
	--parent_lock.node->c_count;
	write_unlock_tree(tree);

	assert("zam-989", item_is_internal(&cut_from));

	/* @node should be deleted after unlocking. */
	ZF_SET(node, JNODE_HEARD_BANSHEE);

	/* remove a pointer from the parent node to the node being deleted. */
	coord_dup(&cut_to, &cut_from);
	/* FIXME: shouldn't this be kill_node_content */
	ret = cut_node_content(&cut_from, &cut_to, NULL, NULL, NULL);
	if (ret)
		/* FIXME(Zam): Should we re-connect the node to its parent if
		 * cut_node fails? */
		goto failed;

	{
		reiser4_tree *tree = current_tree;
		__u64 start_offset = 0, end_offset = 0;

		read_lock_tree(tree);
		write_lock_dk(tree);
		if (object) {
			/* We use @smallest_removed and the left delimiting of
			 * the current node for @object->i_blocks, i_bytes
			 * calculation.  We assume that the items after the
			 * *@smallest_removed key have been deleted from the
			 * file body. */
			start_offset = get_key_offset(znode_get_ld_key(node));
			end_offset = get_key_offset(smallest_removed);
		}

		assert("zam-1021", znode_is_connected(node));
		if (node->left)
			znode_set_rd_key(node->left, znode_get_rd_key(node));

		*smallest_removed = *znode_get_ld_key(node);

		write_unlock_dk(tree);
		read_unlock_tree(tree);

		if (object) {
			/* we used to perform actions which are to be performed on items on their removal from tree in
			   special item method - kill_hook. Here for optimization reasons we avoid reading node
			   containing item we remove and can not call item's kill hook. Instead we call function which
			   does exactly the same things as tail kill hook in assumption that node we avoid reading
			   contains only one item and that item is a tail one. */
			fake_kill_hook_tail(object, start_offset, end_offset,
					    truncate);
		}
	}
      failed:
	zrelse(parent_lock.node);
      failed_nozrelse:
	done_lh(&parent_lock);

	return ret;
}

static int can_delete(const reiser4_key *key, znode *node)
{
	int result;

	read_lock_dk(current_tree);
	result = keyle(key, znode_get_ld_key(node));
	read_unlock_dk(current_tree);
	return result;
}

/**
 * This subroutine is not optimal but implementation seems to
 * be easier).
 *
 * @tap: the point deletion process begins from,
 * @from_key: the beginning of the deleted key range,
 * @to_key: the end of the deleted key range,
 * @smallest_removed: the smallest removed key,
 * @truncate: true if called for file truncate.
 * @progress: return true if a progress in file items deletions was made,
 *            @smallest_removed value is actual in that case.
 *
 * @return: 0 if success, error code otherwise, -E_REPEAT means that long
 * reiser4_cut_tree operation was interrupted for allowing atom commit.
 */
int
cut_tree_worker_common(tap_t * tap, const reiser4_key * from_key,
		       const reiser4_key * to_key,
		       reiser4_key * smallest_removed, struct inode *object,
		       int truncate, int *progress)
{
	lock_handle next_node_lock;
	coord_t left_coord;
	int result;

	assert("zam-931", tap->coord->node != NULL);
	assert("zam-932", znode_is_write_locked(tap->coord->node));

	*progress = 0;
	init_lh(&next_node_lock);

	while (1) {
		znode *node;	/* node from which items are cut */
		node_plugin *nplug;	/* node plugin for @node */

		node = tap->coord->node;

		/* Move next_node_lock to the next node on the left. */
		result =
		    reiser4_get_left_neighbor(&next_node_lock, node,
					      ZNODE_WRITE_LOCK,
					      GN_CAN_USE_UPPER_LEVELS);
		if (result != 0 && result != -E_NO_NEIGHBOR)
			break;
		/* Check can we delete the node as a whole. */
		if (*progress && znode_get_level(node) == LEAF_LEVEL &&
		    can_delete(from_key, node)) {
			result = reiser4_delete_node(node, smallest_removed,
						     object, truncate);
		} else {
			result = reiser4_tap_load(tap);
			if (result)
				return result;

			/* Prepare the second (right) point for cut_node() */
			if (*progress)
				coord_init_last_unit(tap->coord, node);

			else if (item_plugin_by_coord(tap->coord)->b.lookup ==
				 NULL)
				/* set rightmost unit for the items without lookup method */
				tap->coord->unit_pos =
				    coord_last_unit_pos(tap->coord);

			nplug = node->nplug;

			assert("vs-686", nplug);
			assert("vs-687", nplug->lookup);

			/* left_coord is leftmost unit cut from @node */
			result = nplug->lookup(node, from_key,
					       FIND_MAX_NOT_MORE_THAN,
					       &left_coord);

			if (IS_CBKERR(result))
				break;

			/* adjust coordinates so that they are set to existing units */
			if (coord_set_to_right(&left_coord)
			    || coord_set_to_left(tap->coord)) {
				result = 0;
				break;
			}

			if (coord_compare(&left_coord, tap->coord) ==
			    COORD_CMP_ON_RIGHT) {
				/* keys from @from_key to @to_key are not in the tree */
				result = 0;
				break;
			}

			if (left_coord.item_pos != tap->coord->item_pos) {
				/* do not allow to cut more than one item. It is added to solve problem of truncating
				   partially converted files. If file is partially converted there may exist a twig node
				   containing both internal item or items pointing to leaf nodes with formatting items
				   and extent item. We do not want to kill internal items being at twig node here
				   because cut_tree_worker assumes killing them from level level */
				coord_dup(&left_coord, tap->coord);
				assert("vs-1652",
				       coord_is_existing_unit(&left_coord));
				left_coord.unit_pos = 0;
			}

			/* cut data from one node */
			/* *smallest_removed = *reiser4_min_key(); */
			result =
			    kill_node_content(&left_coord, tap->coord, from_key,
					      to_key, smallest_removed,
					      next_node_lock.node, object,
					      truncate);
			reiser4_tap_relse(tap);
		}
		if (result)
			break;

		++(*progress);

		/* Check whether all items with keys >= from_key were removed
		 * from the tree. */
		if (keyle(smallest_removed, from_key))
			/* result = 0; */
			break;

		if (next_node_lock.node == NULL)
			break;

		result = reiser4_tap_move(tap, &next_node_lock);
		done_lh(&next_node_lock);
		if (result)
			break;

		/* Break long reiser4_cut_tree operation (deletion of a large
		   file) if atom requires commit. */
		if (*progress > CUT_TREE_MIN_ITERATIONS
		    && current_atom_should_commit()) {
			result = -E_REPEAT;
			break;
		}
	}
	done_lh(&next_node_lock);
	/* assert("vs-301", !keyeq(&smallest_removed, reiser4_min_key())); */
	return result;
}

/* there is a fundamental problem with optimizing deletes: VFS does it
   one file at a time.  Another problem is that if an item can be
   anything, then deleting items must be done one at a time.  It just
   seems clean to writes this to specify a from and a to key, and cut
   everything between them though.  */

/* use this function with care if deleting more than what is part of a single file. */
/* do not use this when cutting a single item, it is suboptimal for that */

/* You are encouraged to write plugin specific versions of this.  It
   cannot be optimal for all plugins because it works item at a time,
   and some plugins could sometimes work node at a time. Regular files
   however are not optimizable to work node at a time because of
   extents needing to free the blocks they point to.

   Optimizations compared to v3 code:

   It does not balance (that task is left to memory pressure code).

   Nodes are deleted only if empty.

   Uses extents.

   Performs read-ahead of formatted nodes whose contents are part of
   the deletion.
*/

/**
 * Delete everything from the reiser4 tree between two keys: @from_key and
 * @to_key.
 *
 * @from_key: the beginning of the deleted key range,
 * @to_key: the end of the deleted key range,
 * @smallest_removed: the smallest removed key,
 * @object: owner of cutting items.
 * @truncate: true if called for file truncate.
 * @progress: return true if a progress in file items deletions was made,
 *            @smallest_removed value is actual in that case.
 *
 * @return: 0 if success, error code otherwise, -E_REPEAT means that long cut_tree
 * operation was interrupted for allowing atom commit .
 */

int reiser4_cut_tree_object(reiser4_tree * tree, const reiser4_key * from_key,
			    const reiser4_key * to_key,
			    reiser4_key * smallest_removed_p,
			    struct inode *object, int truncate, int *progress)
{
	lock_handle lock;
	int result;
	tap_t tap;
	coord_t right_coord;
	reiser4_key smallest_removed;
	int (*cut_tree_worker) (tap_t *, const reiser4_key *,
				const reiser4_key *, reiser4_key *,
				struct inode *, int, int *);
	STORE_COUNTERS;

	assert("umka-329", tree != NULL);
	assert("umka-330", from_key != NULL);
	assert("umka-331", to_key != NULL);
	assert("zam-936", keyle(from_key, to_key));

	if (smallest_removed_p == NULL)
		smallest_removed_p = &smallest_removed;

	init_lh(&lock);

	do {
		/* Find rightmost item to cut away from the tree. */
		result = reiser4_object_lookup(object, to_key, &right_coord,
					       &lock, ZNODE_WRITE_LOCK,
					       FIND_MAX_NOT_MORE_THAN,
					       TWIG_LEVEL, LEAF_LEVEL,
					       CBK_UNIQUE, NULL /*ra_info */);
		if (result != CBK_COORD_FOUND)
			break;
		if (object == NULL
		    || inode_file_plugin(object)->cut_tree_worker == NULL)
			cut_tree_worker = cut_tree_worker_common;
		else
			cut_tree_worker =
			    inode_file_plugin(object)->cut_tree_worker;
		reiser4_tap_init(&tap, &right_coord, &lock, ZNODE_WRITE_LOCK);
		result =
		    cut_tree_worker(&tap, from_key, to_key, smallest_removed_p,
				    object, truncate, progress);
		reiser4_tap_done(&tap);

		reiser4_preempt_point();

	} while (0);

	done_lh(&lock);

	if (result) {
		switch (result) {
		case -E_NO_NEIGHBOR:
			result = 0;
			break;
		case -E_DEADLOCK:
			result = -E_REPEAT;
		case -E_REPEAT:
		case -ENOMEM:
		case -ENOENT:
			break;
		default:
			warning("nikita-2861", "failure: %i", result);
		}
	}

	CHECK_COUNTERS;
	return result;
}

/* repeat reiser4_cut_tree_object until everything is deleted.
 * unlike cut_file_items, it does not end current transaction if -E_REPEAT
 * is returned by cut_tree_object. */
int reiser4_cut_tree(reiser4_tree * tree, const reiser4_key * from,
		     const reiser4_key * to, struct inode *inode, int truncate)
{
	int result;
	int progress;

	do {
		result = reiser4_cut_tree_object(tree, from, to, NULL,
						 inode, truncate, &progress);
	} while (result == -E_REPEAT);

	return result;
}

/* finishing reiser4 initialization */
int reiser4_init_tree(reiser4_tree * tree	/* pointer to structure being
					 * initialized */ ,
	      const reiser4_block_nr * root_block	/* address of a root block
							 * on a disk */ ,
	      tree_level height /* height of a tree */ ,
	      node_plugin * nplug /* default node plugin */ )
{
	int result;

	assert("nikita-306", tree != NULL);
	assert("nikita-307", root_block != NULL);
	assert("nikita-308", height > 0);
	assert("nikita-309", nplug != NULL);
	assert("zam-587", tree->super != NULL);

	tree->root_block = *root_block;
	tree->height = height;
	tree->estimate_one_insert = calc_estimate_one_insert(height);
	tree->nplug = nplug;

	tree->znode_epoch = 1ull;

	cbk_cache_init(&tree->cbk_cache);

	result = znodes_tree_init(tree);
	if (result == 0)
		result = jnodes_tree_init(tree);
	if (result == 0) {
		tree->uber = zget(tree, &UBER_TREE_ADDR, NULL, 0,
				  reiser4_ctx_gfp_mask_get());
		if (IS_ERR(tree->uber)) {
			result = PTR_ERR(tree->uber);
			tree->uber = NULL;
		}
	}
	return result;
}

/* release resources associated with @tree */
void reiser4_done_tree(reiser4_tree * tree /* tree to release */ )
{
	if (tree == NULL)
		return;

	if (tree->uber != NULL) {
		zput(tree->uber);
		tree->uber = NULL;
	}
	znodes_tree_done(tree);
	jnodes_tree_done(tree);
	cbk_cache_done(&tree->cbk_cache);
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
