/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

#include "forward.h"
#include "debug.h"
#include "dformat.h"
#include "key.h"
#include "coord.h"
#include "seal.h"
#include "plugin/item/item.h"
#include "plugin/node/node.h"
#include "plugin/plugin.h"
#include "jnode.h"
#include "znode.h"
#include "block_alloc.h"
#include "tree_walk.h"
#include "tree.h"
#include "reiser4.h"
#include "super.h"
#include "inode.h"

#include <linux/slab.h>

static const char *bias_name(lookup_bias bias);

/* tree searching algorithm, intranode searching algorithms are in
   plugin/node/ */

/* tree lookup cache
 *
 * The coord by key cache consists of small list of recently accessed nodes
 * maintained according to the LRU discipline. Before doing real top-to-down
 * tree traversal this cache is scanned for nodes that can contain key
 * requested.
 *
 * The efficiency of coord cache depends heavily on locality of reference for
 * tree accesses. Our user level simulations show reasonably good hit ratios
 * for coord cache under most loads so far.
 */

/* Initialise coord cache slot */
static void cbk_cache_init_slot(cbk_cache_slot *slot)
{
	assert("nikita-345", slot != NULL);

	INIT_LIST_HEAD(&slot->lru);
	slot->node = NULL;
}

/* Initialize coord cache */
int cbk_cache_init(cbk_cache * cache/* cache to init */)
{
	int i;

	assert("nikita-346", cache != NULL);

	cache->slot =
		kmalloc(sizeof(cbk_cache_slot) * cache->nr_slots,
			reiser4_ctx_gfp_mask_get());
	if (cache->slot == NULL)
		return RETERR(-ENOMEM);

	INIT_LIST_HEAD(&cache->lru);
	for (i = 0; i < cache->nr_slots; ++i) {
		cbk_cache_init_slot(cache->slot + i);
		list_add_tail(&((cache->slot + i)->lru), &cache->lru);
	}
	rwlock_init(&cache->guard);
	return 0;
}

/* free cbk cache data */
void cbk_cache_done(cbk_cache * cache/* cache to release */)
{
	assert("nikita-2493", cache != NULL);
	if (cache->slot != NULL) {
		kfree(cache->slot);
		cache->slot = NULL;
	}
}

/* macro to iterate over all cbk cache slots */
#define for_all_slots(cache, slot)					  \
	for ((slot) = list_entry((cache)->lru.next, cbk_cache_slot, lru); \
	     &(cache)->lru != &(slot)->lru;				  \
	     (slot) = list_entry(slot->lru.next, cbk_cache_slot, lru))

#if REISER4_DEBUG
/* this function assures that [cbk-cache-invariant] invariant holds */
static int cbk_cache_invariant(const cbk_cache * cache)
{
	cbk_cache_slot *slot;
	int result;
	int unused;

	if (cache->nr_slots == 0)
		return 1;

	assert("nikita-2469", cache != NULL);
	unused = 0;
	result = 1;
	read_lock(&((cbk_cache *)cache)->guard);
	for_all_slots(cache, slot) {
		/* in LRU first go all `used' slots followed by `unused' */
		if (unused && (slot->node != NULL))
			result = 0;
		if (slot->node == NULL)
			unused = 1;
		else {
			cbk_cache_slot *scan;

			/* all cached nodes are different */
			scan = slot;
			while (result) {
				scan = list_entry(scan->lru.next,
						  cbk_cache_slot, lru);
				if (&cache->lru == &scan->lru)
					break;
				if (slot->node == scan->node)
					result = 0;
			}
		}
		if (!result)
			break;
	}
	read_unlock(&((cbk_cache *)cache)->guard);
	return result;
}

#endif

/* Remove references, if any, to @node from coord cache */
void cbk_cache_invalidate(const znode * node /* node to remove from cache */ ,
			  reiser4_tree * tree/* tree to remove node from */)
{
	cbk_cache_slot *slot;
	cbk_cache *cache;
	int i;

	assert("nikita-350", node != NULL);
	assert("nikita-1479", LOCK_CNT_GTZ(rw_locked_tree));

	cache = &tree->cbk_cache;
	assert("nikita-2470", cbk_cache_invariant(cache));

	write_lock(&(cache->guard));
	for (i = 0, slot = cache->slot; i < cache->nr_slots; ++i, ++slot) {
		if (slot->node == node) {
			list_move_tail(&slot->lru, &cache->lru);
			slot->node = NULL;
			break;
		}
	}
	write_unlock(&(cache->guard));
	assert("nikita-2471", cbk_cache_invariant(cache));
}

/* add to the cbk-cache in the "tree" information about "node". This
    can actually be update of existing slot in a cache. */
static void cbk_cache_add(const znode * node/* node to add to the cache */)
{
	cbk_cache *cache;

	cbk_cache_slot *slot;
	int i;

	assert("nikita-352", node != NULL);

	cache = &znode_get_tree(node)->cbk_cache;
	assert("nikita-2472", cbk_cache_invariant(cache));

	if (cache->nr_slots == 0)
		return;

	write_lock(&(cache->guard));
	/* find slot to update/add */
	for (i = 0, slot = cache->slot; i < cache->nr_slots; ++i, ++slot) {
		/* oops, this node is already in a cache */
		if (slot->node == node)
			break;
	}
	/* if all slots are used, reuse least recently used one */
	if (i == cache->nr_slots) {
		slot = list_entry(cache->lru.prev, cbk_cache_slot, lru);
		slot->node = (znode *) node;
	}
	list_move(&slot->lru, &cache->lru);
	write_unlock(&(cache->guard));
	assert("nikita-2473", cbk_cache_invariant(cache));
}

static int setup_delimiting_keys(cbk_handle * h);
static lookup_result coord_by_handle(cbk_handle * handle);
static lookup_result traverse_tree(cbk_handle * h);
static int cbk_cache_search(cbk_handle * h);

static level_lookup_result cbk_level_lookup(cbk_handle * h);
static level_lookup_result cbk_node_lookup(cbk_handle * h);

/* helper functions */

static void update_stale_dk(reiser4_tree * tree, znode * node);

/* release parent node during traversal */
static void put_parent(cbk_handle * h);
/* check consistency of fields */
static int sanity_check(cbk_handle * h);
/* release resources in handle */
static void hput(cbk_handle * h);

static level_lookup_result search_to_left(cbk_handle * h);

/* pack numerous (numberous I should say) arguments of coord_by_key() into
 * cbk_handle */
static cbk_handle *cbk_pack(cbk_handle * handle,
			    reiser4_tree * tree,
			    const reiser4_key * key,
			    coord_t *coord,
			    lock_handle * active_lh,
			    lock_handle * parent_lh,
			    znode_lock_mode lock_mode,
			    lookup_bias bias,
			    tree_level lock_level,
			    tree_level stop_level,
			    __u32 flags, ra_info_t *info)
{
	memset(handle, 0, sizeof *handle);

	handle->tree = tree;
	handle->key = key;
	handle->lock_mode = lock_mode;
	handle->bias = bias;
	handle->lock_level = lock_level;
	handle->stop_level = stop_level;
	handle->coord = coord;
	/* set flags. See comment in tree.h:cbk_flags */
	handle->flags = flags | CBK_TRUST_DK | CBK_USE_CRABLOCK;

	handle->active_lh = active_lh;
	handle->parent_lh = parent_lh;
	handle->ra_info = info;
	return handle;
}

/* main tree lookup procedure

   Check coord cache. If key we are looking for is not found there, call cbk()
   to do real tree traversal.

   As we have extents on the twig level, @lock_level and @stop_level can
   be different from LEAF_LEVEL and each other.

   Thread cannot keep any reiser4 locks (tree, znode, dk spin-locks, or znode
   long term locks) while calling this.
*/
lookup_result coord_by_key(reiser4_tree * tree	/* tree to perform search
						 * in. Usually this tree is
						 * part of file-system
						 * super-block */ ,
			   const reiser4_key * key /* key to look for */ ,
			   coord_t *coord	/* where to store found
						 * position in a tree. Fields
						 * in "coord" are only valid if
						 * coord_by_key() returned
						 * "CBK_COORD_FOUND" */ ,
			   lock_handle * lh,	/* resulting lock handle */
			   znode_lock_mode lock_mode	/* type of lookup we
							 * want on node. Pass
							 * ZNODE_READ_LOCK here
							 * if you only want to
							 * read item found and
							 * ZNODE_WRITE_LOCK if
							 * you want to modify
							 * it */ ,
			   lookup_bias bias	/* what to return if coord
						 * with exactly the @key is
						 * not in the tree */ ,
			   tree_level lock_level/* tree level where to start
						 * taking @lock type of
						 * locks */ ,
			   tree_level stop_level/* tree level to stop. Pass
						 * LEAF_LEVEL or TWIG_LEVEL
						 * here Item being looked
						 * for has to be between
						 * @lock_level and
						 * @stop_level, inclusive */ ,
			   __u32 flags /* search flags */ ,
			   ra_info_t *
			   info
			   /* information about desired tree traversal
			    * readahead */
			   )
{
	cbk_handle handle;
	lock_handle parent_lh;
	lookup_result result;

	init_lh(lh);
	init_lh(&parent_lh);

	assert("nikita-3023", reiser4_schedulable());

	assert("nikita-353", tree != NULL);
	assert("nikita-354", key != NULL);
	assert("nikita-355", coord != NULL);
	assert("nikita-356", (bias == FIND_EXACT)
	       || (bias == FIND_MAX_NOT_MORE_THAN));
	assert("nikita-357", stop_level >= LEAF_LEVEL);
	/* no locks can be held during tree traversal */
	assert("nikita-2104", lock_stack_isclean(get_current_lock_stack()));

	cbk_pack(&handle,
		 tree,
		 key,
		 coord,
		 lh,
		 &parent_lh,
		 lock_mode, bias, lock_level, stop_level, flags, info);

	result = coord_by_handle(&handle);
	assert("nikita-3247",
	       ergo(!IS_CBKERR(result), coord->node == lh->node));
	return result;
}

/* like coord_by_key(), but starts traversal from vroot of @object rather than
 * from tree root. */
lookup_result reiser4_object_lookup(struct inode *object,
				    const reiser4_key * key,
				    coord_t *coord,
				    lock_handle * lh,
				    znode_lock_mode lock_mode,
				    lookup_bias bias,
				    tree_level lock_level,
				    tree_level stop_level, __u32 flags,
				    ra_info_t *info)
{
	cbk_handle handle;
	lock_handle parent_lh;
	lookup_result result;

	init_lh(lh);
	init_lh(&parent_lh);

	assert("nikita-3023", reiser4_schedulable());

	assert("nikita-354", key != NULL);
	assert("nikita-355", coord != NULL);
	assert("nikita-356", (bias == FIND_EXACT)
	       || (bias == FIND_MAX_NOT_MORE_THAN));
	assert("nikita-357", stop_level >= LEAF_LEVEL);
	/* no locks can be held during tree search by key */
	assert("nikita-2104", lock_stack_isclean(get_current_lock_stack()));

	cbk_pack(&handle,
		 object != NULL ? reiser4_tree_by_inode(object) : current_tree,
		 key,
		 coord,
		 lh,
		 &parent_lh,
		 lock_mode, bias, lock_level, stop_level, flags, info);
	handle.object = object;

	result = coord_by_handle(&handle);
	assert("nikita-3247",
	       ergo(!IS_CBKERR(result), coord->node == lh->node));
	return result;
}

/* lookup by cbk_handle. Common part of coord_by_key() and
   reiser4_object_lookup(). */
static lookup_result coord_by_handle(cbk_handle * handle)
{
	/*
	 * first check cbk_cache (which is look-aside cache for our tree) and
	 * of this fails, start traversal.
	 */
	/* first check whether "key" is in cache of recent lookups. */
	if (cbk_cache_search(handle) == 0)
		return handle->result;
	else
		return traverse_tree(handle);
}

/* Execute actor for each item (or unit, depending on @through_units_p),
   starting from @coord, right-ward, until either:

   - end of the tree is reached
   - unformatted node is met
   - error occurred
   - @actor returns 0 or less

   Error code, or last actor return value is returned.

   This is used by plugin/dir/hashe_dir.c:reiser4_find_entry() to move through
   sequence of entries with identical keys and alikes.
*/
int reiser4_iterate_tree(reiser4_tree * tree /* tree to scan */ ,
			 coord_t *coord /* coord to start from */ ,
			 lock_handle * lh /* lock handle to start with and to
					   * update along the way */ ,
			 tree_iterate_actor_t actor /* function to call on each
						     * item/unit */ ,
			 void *arg /* argument to pass to @actor */ ,
			 znode_lock_mode mode /* lock mode on scanned nodes */ ,
			 int through_units_p /* call @actor on each item or on
					      *	each unit */ )
{
	int result;

	assert("nikita-1143", tree != NULL);
	assert("nikita-1145", coord != NULL);
	assert("nikita-1146", lh != NULL);
	assert("nikita-1147", actor != NULL);

	result = zload(coord->node);
	coord_clear_iplug(coord);
	if (result != 0)
		return result;
	if (!coord_is_existing_unit(coord)) {
		zrelse(coord->node);
		return -ENOENT;
	}
	while ((result = actor(tree, coord, lh, arg)) > 0) {
		/* move further  */
		if ((through_units_p && coord_next_unit(coord)) ||
		    (!through_units_p && coord_next_item(coord))) {
			do {
				lock_handle couple;

				/* move to the next node  */
				init_lh(&couple);
				result =
				    reiser4_get_right_neighbor(&couple,
							       coord->node,
							       (int)mode,
							       GN_CAN_USE_UPPER_LEVELS);
				zrelse(coord->node);
				if (result == 0) {

					result = zload(couple.node);
					if (result != 0) {
						done_lh(&couple);
						return result;
					}

					coord_init_first_unit(coord,
							      couple.node);
					done_lh(lh);
					move_lh(lh, &couple);
				} else
					return result;
			} while (node_is_empty(coord->node));
		}

		assert("nikita-1149", coord_is_existing_unit(coord));
	}
	zrelse(coord->node);
	return result;
}

/* return locked uber znode for @tree */
int get_uber_znode(reiser4_tree * tree, znode_lock_mode mode,
		   znode_lock_request pri, lock_handle * lh)
{
	int result;

	result = longterm_lock_znode(lh, tree->uber, mode, pri);
	return result;
}

/* true if @key is strictly within @node

   we are looking for possibly non-unique key and it is item is at the edge of
   @node. May be it is in the neighbor.
*/
static int znode_contains_key_strict(znode * node	/* node to check key
							 * against */ ,
				     const reiser4_key *
				     key /* key to check */ ,
				     int isunique)
{
	int answer;

	assert("nikita-1760", node != NULL);
	assert("nikita-1722", key != NULL);

	if (keyge(key, &node->rd_key))
		return 0;

	answer = keycmp(&node->ld_key, key);

	if (isunique)
		return answer != GREATER_THAN;
	else
		return answer == LESS_THAN;
}

/*
 * Virtual Root (vroot) code.
 *
 *     For given file system object (e.g., regular file or directory) let's
 *     define its "virtual root" as lowest in the tree (that is, furtherest
 *     from the tree root) node such that all body items of said object are
 *     located in a tree rooted at this node.
 *
 *     Once vroot of object is found all tree lookups for items within body of
 *     this object ("object lookups") can be started from its vroot rather
 *     than from real root. This has following advantages:
 *
 *         1. amount of nodes traversed during lookup (and, hence, amount of
 *         key comparisons made) decreases, and
 *
 *         2. contention on tree root is decreased. This latter was actually
 *         motivating reason behind vroot, because spin lock of root node,
 *         which is taken when acquiring long-term lock on root node is the
 *         hottest lock in the reiser4.
 *
 * How to find vroot.
 *
 *     When vroot of object F is not yet determined, all object lookups start
 *     from the root of the tree. At each tree level during traversal we have
 *     a node N such that a key we are looking for (which is the key inside
 *     object's body) is located within N. In function handle_vroot() called
 *     from cbk_level_lookup() we check whether N is possible vroot for
 *     F. Check is trivial---if neither leftmost nor rightmost item of N
 *     belongs to F (and we already have helpful ->owns_item() method of
 *     object plugin for this), then N is possible vroot of F. This, of
 *     course, relies on the assumption that each object occupies contiguous
 *     range of keys in the tree.
 *
 *     Thus, traversing tree downward and checking each node as we go, we can
 *     find lowest such node, which, by definition, is vroot.
 *
 * How to track vroot.
 *
 *     Nohow. If actual vroot changes, next object lookup will just restart
 *     from the actual tree root, refreshing object's vroot along the way.
 *
 */

/*
 * Check whether @node is possible vroot of @object.
 */
static void handle_vroot(struct inode *object, znode * node)
{
	file_plugin *fplug;
	coord_t coord;

	fplug = inode_file_plugin(object);
	assert("nikita-3353", fplug != NULL);
	assert("nikita-3354", fplug->owns_item != NULL);

	if (unlikely(node_is_empty(node)))
		return;

	coord_init_first_unit(&coord, node);
	/*
	 * if leftmost item of @node belongs to @object, we cannot be sure
	 * that @node is vroot of @object, because, some items of @object are
	 * probably in the sub-tree rooted at the left neighbor of @node.
	 */
	if (fplug->owns_item(object, &coord))
		return;
	coord_init_last_unit(&coord, node);
	/* mutatis mutandis for the rightmost item */
	if (fplug->owns_item(object, &coord))
		return;
	/* otherwise, @node is possible vroot of @object */
	inode_set_vroot(object, node);
}

/*
 * helper function used by traverse tree to start tree traversal not from the
 * tree root, but from @h->object's vroot, if possible.
 */
static int prepare_object_lookup(cbk_handle * h)
{
	znode *vroot;
	int result;

	vroot = inode_get_vroot(h->object);
	if (vroot == NULL) {
		/*
		 * object doesn't have known vroot, start from real tree root.
		 */
		return LOOKUP_CONT;
	}

	h->level = znode_get_level(vroot);
	/* take a long-term lock on vroot */
	h->result = longterm_lock_znode(h->active_lh, vroot,
					cbk_lock_mode(h->level, h),
					ZNODE_LOCK_LOPRI);
	result = LOOKUP_REST;
	if (h->result == 0) {
		int isunique;
		int inside;

		isunique = h->flags & CBK_UNIQUE;
		/* check that key is inside vroot */
		read_lock_dk(h->tree);
		inside = (znode_contains_key_strict(vroot, h->key, isunique) &&
			  !ZF_ISSET(vroot, JNODE_HEARD_BANSHEE));
		read_unlock_dk(h->tree);
		if (inside) {
			h->result = zload(vroot);
			if (h->result == 0) {
				/* search for key in vroot. */
				result = cbk_node_lookup(h);
				zrelse(vroot);	/*h->active_lh->node); */
				if (h->active_lh->node != vroot) {
					result = LOOKUP_REST;
				} else if (result == LOOKUP_CONT) {
					move_lh(h->parent_lh, h->active_lh);
					h->flags &= ~CBK_DKSET;
				}
			}
		}
	}

	zput(vroot);

	if (IS_CBKERR(h->result) || result == LOOKUP_REST)
		hput(h);
	return result;
}

/* main function that handles common parts of tree traversal: starting
    (fake znode handling), restarts, error handling, completion */
static lookup_result traverse_tree(cbk_handle * h/* search handle */)
{
	int done;
	int iterations;
	int vroot_used;

	assert("nikita-365", h != NULL);
	assert("nikita-366", h->tree != NULL);
	assert("nikita-367", h->key != NULL);
	assert("nikita-368", h->coord != NULL);
	assert("nikita-369", (h->bias == FIND_EXACT)
	       || (h->bias == FIND_MAX_NOT_MORE_THAN));
	assert("nikita-370", h->stop_level >= LEAF_LEVEL);
	assert("nikita-2949", !(h->flags & CBK_DKSET));
	assert("zam-355", lock_stack_isclean(get_current_lock_stack()));

	done = 0;
	iterations = 0;
	vroot_used = 0;

	/* loop for restarts */
restart:

	assert("nikita-3024", reiser4_schedulable());

	h->result = CBK_COORD_FOUND;
	/* connect_znode() needs it */
	h->ld_key = *reiser4_min_key();
	h->rd_key = *reiser4_max_key();
	h->flags |= CBK_DKSET;
	h->error = NULL;

	if (!vroot_used && h->object != NULL) {
		vroot_used = 1;
		done = prepare_object_lookup(h);
		if (done == LOOKUP_REST)
			goto restart;
		else if (done == LOOKUP_DONE)
			return h->result;
	}
	if (h->parent_lh->node == NULL) {
		done =
		    get_uber_znode(h->tree, ZNODE_READ_LOCK, ZNODE_LOCK_LOPRI,
				   h->parent_lh);

		assert("nikita-1637", done != -E_DEADLOCK);

		h->block = h->tree->root_block;
		h->level = h->tree->height;
		h->coord->node = h->parent_lh->node;

		if (done != 0)
			return done;
	}

	/* loop descending a tree */
	while (!done) {

		if (unlikely((iterations > REISER4_CBK_ITERATIONS_LIMIT) &&
			     IS_POW(iterations))) {
			warning("nikita-1481", "Too many iterations: %i",
				iterations);
			reiser4_print_key("key", h->key);
			++iterations;
		} else if (unlikely(iterations > REISER4_MAX_CBK_ITERATIONS)) {
			h->error =
			    "reiser-2018: Too many iterations. Tree corrupted, or (less likely) starvation occurring.";
			h->result = RETERR(-EIO);
			break;
		}
		switch (cbk_level_lookup(h)) {
		case LOOKUP_CONT:
			move_lh(h->parent_lh, h->active_lh);
			continue;
		default:
			wrong_return_value("nikita-372", "cbk_level");
		case LOOKUP_DONE:
			done = 1;
			break;
		case LOOKUP_REST:
			hput(h);
			/* deadlock avoidance is normal case. */
			if (h->result != -E_DEADLOCK)
				++iterations;
			reiser4_preempt_point();
			goto restart;
		}
	}
	/* that's all. The rest is error handling */
	if (unlikely(h->error != NULL)) {
		warning("nikita-373", "%s: level: %i, "
			"lock_level: %i, stop_level: %i "
			"lock_mode: %s, bias: %s",
			h->error, h->level, h->lock_level, h->stop_level,
			lock_mode_name(h->lock_mode), bias_name(h->bias));
		reiser4_print_address("block", &h->block);
		reiser4_print_key("key", h->key);
		print_coord_content("coord", h->coord);
	}
	/* `unlikely' error case */
	if (unlikely(IS_CBKERR(h->result))) {
		/* failure. do cleanup */
		hput(h);
	} else {
		assert("nikita-1605", WITH_DATA_RET
		       (h->coord->node, 1,
			ergo((h->result == CBK_COORD_FOUND) &&
			     (h->bias == FIND_EXACT) &&
			     (!node_is_empty(h->coord->node)),
			     coord_is_existing_item(h->coord))));
	}
	return h->result;
}

/* find delimiting keys of child

   Determine left and right delimiting keys for child pointed to by
   @parent_coord.

*/
static void find_child_delimiting_keys(znode * parent	/* parent znode, passed
							 * locked */ ,
				       const coord_t *parent_coord
							/* coord where pointer
							 * to child is stored
							 */ ,
				       reiser4_key * ld	/* where to store left
							 * delimiting key */ ,
				       reiser4_key * rd	/* where to store right
							 * delimiting key */ )
{
	coord_t neighbor;

	assert("nikita-1484", parent != NULL);
	assert_rw_locked(&(znode_get_tree(parent)->dk_lock));

	coord_dup(&neighbor, parent_coord);

	if (neighbor.between == AT_UNIT)
		/* imitate item ->lookup() behavior. */
		neighbor.between = AFTER_UNIT;

	if (coord_set_to_left(&neighbor) == 0)
		unit_key_by_coord(&neighbor, ld);
	else {
		assert("nikita-14851", 0);
		*ld = *znode_get_ld_key(parent);
	}

	coord_dup(&neighbor, parent_coord);
	if (neighbor.between == AT_UNIT)
		neighbor.between = AFTER_UNIT;
	if (coord_set_to_right(&neighbor) == 0)
		unit_key_by_coord(&neighbor, rd);
	else
		*rd = *znode_get_rd_key(parent);
}

/*
 * setup delimiting keys for a child
 *
 * @parent parent node
 *
 * @coord location in @parent where pointer to @child is
 *
 * @child child node
 */
int
set_child_delimiting_keys(znode * parent, const coord_t *coord, znode * child)
{
	reiser4_tree *tree;

	assert("nikita-2952",
	       znode_get_level(parent) == znode_get_level(coord->node));

	/* fast check without taking dk lock. This is safe, because
	 * JNODE_DKSET is never cleared once set. */
	if (!ZF_ISSET(child, JNODE_DKSET)) {
		tree = znode_get_tree(parent);
		write_lock_dk(tree);
		if (likely(!ZF_ISSET(child, JNODE_DKSET))) {
			find_child_delimiting_keys(parent, coord,
						   &child->ld_key,
						   &child->rd_key);
			ON_DEBUG(child->ld_key_version =
				 atomic_inc_return(&delim_key_version);
				 child->rd_key_version =
				 atomic_inc_return(&delim_key_version););
			ZF_SET(child, JNODE_DKSET);
		}
		write_unlock_dk(tree);
		return 1;
	}
	return 0;
}

/* Perform tree lookup at one level. This is called from cbk_traverse()
   function that drives lookup through tree and calls cbk_node_lookup() to
   perform lookup within one node.

   See comments in a code.
*/
static level_lookup_result cbk_level_lookup(cbk_handle * h/* search handle */)
{
	int ret;
	int setdk;
	int ldkeyset = 0;
	reiser4_key ldkey;
	reiser4_key key;
	znode *active;

	assert("nikita-3025", reiser4_schedulable());

	/* acquire reference to @active node */
	active =
	    zget(h->tree, &h->block, h->parent_lh->node, h->level,
		 reiser4_ctx_gfp_mask_get());

	if (IS_ERR(active)) {
		h->result = PTR_ERR(active);
		return LOOKUP_DONE;
	}

	/* lock @active */
	h->result = longterm_lock_znode(h->active_lh,
					active,
					cbk_lock_mode(h->level, h),
					ZNODE_LOCK_LOPRI);
	/* longterm_lock_znode() acquires additional reference to znode (which
	   will be later released by longterm_unlock_znode()). Release
	   reference acquired by zget().
	 */
	zput(active);
	if (unlikely(h->result != 0))
		goto fail_or_restart;

	setdk = 0;
	/* if @active is accessed for the first time, setup delimiting keys on
	   it. Delimiting keys are taken from the parent node. See
	   setup_delimiting_keys() for details.
	 */
	if (h->flags & CBK_DKSET) {
		setdk = setup_delimiting_keys(h);
		h->flags &= ~CBK_DKSET;
	} else {
		znode *parent;

		parent = h->parent_lh->node;
		h->result = zload(parent);
		if (unlikely(h->result != 0))
			goto fail_or_restart;

		if (!ZF_ISSET(active, JNODE_DKSET))
			setdk = set_child_delimiting_keys(parent,
							  h->coord, active);
		else {
			read_lock_dk(h->tree);
			find_child_delimiting_keys(parent, h->coord, &ldkey,
						   &key);
			read_unlock_dk(h->tree);
			ldkeyset = 1;
		}
		zrelse(parent);
	}

	/* this is ugly kludge. Reminder: this is necessary, because
	   ->lookup() method returns coord with ->between field probably set
	   to something different from AT_UNIT.
	 */
	h->coord->between = AT_UNIT;

	if (znode_just_created(active) && (h->coord->node != NULL)) {
		write_lock_tree(h->tree);
		/* if we are going to load znode right now, setup
		   ->in_parent: coord where pointer to this node is stored in
		   parent.
		 */
		coord_to_parent_coord(h->coord, &active->in_parent);
		write_unlock_tree(h->tree);
	}

	/* check connectedness without holding tree lock---false negatives
	 * will be re-checked by connect_znode(), and false positives are
	 * impossible---@active cannot suddenly turn into unconnected
	 * state. */
	if (!znode_is_connected(active)) {
		h->result = connect_znode(h->coord, active);
		if (unlikely(h->result != 0)) {
			put_parent(h);
			goto fail_or_restart;
		}
	}

	jload_prefetch(ZJNODE(active));

	if (setdk)
		update_stale_dk(h->tree, active);

	/* put_parent() cannot be called earlier, because connect_znode()
	   assumes parent node is referenced; */
	put_parent(h);

	if ((!znode_contains_key_lock(active, h->key) &&
	     (h->flags & CBK_TRUST_DK))
	    || ZF_ISSET(active, JNODE_HEARD_BANSHEE)) {
		/* 1. key was moved out of this node while this thread was
		   waiting for the lock. Restart. More elaborate solution is
		   to determine where key moved (to the left, or to the right)
		   and try to follow it through sibling pointers.

		   2. or, node itself is going to be removed from the
		   tree. Release lock and restart.
		 */
		h->result = -E_REPEAT;
	}
	if (h->result == -E_REPEAT)
		return LOOKUP_REST;

	h->result = zload_ra(active, h->ra_info);
	if (h->result)
		return LOOKUP_DONE;

	/* sanity checks */
	if (sanity_check(h)) {
		zrelse(active);
		return LOOKUP_DONE;
	}

	/* check that key of leftmost item in the @active is the same as in
	 * its parent */
	if (ldkeyset && !node_is_empty(active) &&
	    !keyeq(leftmost_key_in_node(active, &key), &ldkey)) {
		warning("vs-3533", "Keys are inconsistent. Fsck?");
		reiser4_print_key("inparent", &ldkey);
		reiser4_print_key("inchild", &key);
		h->result = RETERR(-EIO);
		zrelse(active);
		return LOOKUP_DONE;
	}

	if (h->object != NULL)
		handle_vroot(h->object, active);

	ret = cbk_node_lookup(h);

	/* h->active_lh->node might change, but active is yet to be zrelsed */
	zrelse(active);

	return ret;

fail_or_restart:
	if (h->result == -E_DEADLOCK)
		return LOOKUP_REST;
	return LOOKUP_DONE;
}

#if REISER4_DEBUG
/* check left and right delimiting keys of a znode */
void check_dkeys(znode * node)
{
	znode *left;
	znode *right;

	read_lock_tree(current_tree);
	read_lock_dk(current_tree);

	assert("vs-1710", znode_is_any_locked(node));
	assert("vs-1197",
	       !keygt(znode_get_ld_key(node), znode_get_rd_key(node)));

	left = node->left;
	right = node->right;

	if (ZF_ISSET(node, JNODE_LEFT_CONNECTED) && ZF_ISSET(node, JNODE_DKSET)
	    && left != NULL && ZF_ISSET(left, JNODE_DKSET))
		/* check left neighbor. Note that left neighbor is not locked,
		   so it might get wrong delimiting keys therefore */
		assert("vs-1198",
		       (keyeq(znode_get_rd_key(left), znode_get_ld_key(node))
			|| ZF_ISSET(left, JNODE_HEARD_BANSHEE)));

	if (ZF_ISSET(node, JNODE_RIGHT_CONNECTED) && ZF_ISSET(node, JNODE_DKSET)
	    && right != NULL && ZF_ISSET(right, JNODE_DKSET))
		/* check right neighbor. Note that right neighbor is not
		   locked, so it might get wrong delimiting keys therefore  */
		assert("vs-1199",
		       (keyeq(znode_get_rd_key(node), znode_get_ld_key(right))
			|| ZF_ISSET(right, JNODE_HEARD_BANSHEE)));

	read_unlock_dk(current_tree);
	read_unlock_tree(current_tree);
}
#endif

/* true if @key is left delimiting key of @node */
static int key_is_ld(znode * node, const reiser4_key * key)
{
	int ld;

	assert("nikita-1716", node != NULL);
	assert("nikita-1758", key != NULL);

	read_lock_dk(znode_get_tree(node));
	assert("nikita-1759", znode_contains_key(node, key));
	ld = keyeq(znode_get_ld_key(node), key);
	read_unlock_dk(znode_get_tree(node));
	return ld;
}

/* Process one node during tree traversal.

   This is called by cbk_level_lookup(). */
static level_lookup_result cbk_node_lookup(cbk_handle * h/* search handle */)
{
	/* node plugin of @active */
	node_plugin *nplug;
	/* item plugin of item that was found */
	item_plugin *iplug;
	/* search bias */
	lookup_bias node_bias;
	/* node we are operating upon */
	znode *active;
	/* tree we are searching in */
	reiser4_tree *tree;
	/* result */
	int result;

	assert("nikita-379", h != NULL);

	active = h->active_lh->node;
	tree = h->tree;

	nplug = active->nplug;
	assert("nikita-380", nplug != NULL);

	ON_DEBUG(check_dkeys(active));

	/* return item from "active" node with maximal key not greater than
	   "key"  */
	node_bias = h->bias;
	result = nplug->lookup(active, h->key, node_bias, h->coord);
	if (unlikely(result != NS_FOUND && result != NS_NOT_FOUND)) {
		/* error occurred */
		h->result = result;
		return LOOKUP_DONE;
	}
	if (h->level == h->stop_level) {
		/* welcome to the stop level */
		assert("nikita-381", h->coord->node == active);
		if (result == NS_FOUND) {
			/* success of tree lookup */
			if (!(h->flags & CBK_UNIQUE)
			    && key_is_ld(active, h->key))
				return search_to_left(h);
			else
				h->result = CBK_COORD_FOUND;
		} else {
			h->result = CBK_COORD_NOTFOUND;
		}
		if (!(h->flags & CBK_IN_CACHE))
			cbk_cache_add(active);
		return LOOKUP_DONE;
	}

	if (h->level > TWIG_LEVEL && result == NS_NOT_FOUND) {
		h->error = "not found on internal node";
		h->result = result;
		return LOOKUP_DONE;
	}

	assert("vs-361", h->level > h->stop_level);

	if (handle_eottl(h, &result)) {
		assert("vs-1674", (result == LOOKUP_DONE ||
				   result == LOOKUP_REST));
		return result;
	}

	/* go down to next level */
	check_me("vs-12", zload(h->coord->node) == 0);
	assert("nikita-2116", item_is_internal(h->coord));
	iplug = item_plugin_by_coord(h->coord);
	iplug->s.internal.down_link(h->coord, h->key, &h->block);
	zrelse(h->coord->node);
	--h->level;
	return LOOKUP_CONT;	/* continue */
}

/* scan cbk_cache slots looking for a match for @h */
static int cbk_cache_scan_slots(cbk_handle * h/* cbk handle */)
{
	level_lookup_result llr;
	znode *node;
	reiser4_tree *tree;
	cbk_cache_slot *slot;
	cbk_cache *cache;
	tree_level level;
	int isunique;
	const reiser4_key *key;
	int result;

	assert("nikita-1317", h != NULL);
	assert("nikita-1315", h->tree != NULL);
	assert("nikita-1316", h->key != NULL);

	tree = h->tree;
	cache = &tree->cbk_cache;
	if (cache->nr_slots == 0)
		/* size of cbk cache was set to 0 by mount time option. */
		return RETERR(-ENOENT);

	assert("nikita-2474", cbk_cache_invariant(cache));
	node = NULL;		/* to keep gcc happy */
	level = h->level;
	key = h->key;
	isunique = h->flags & CBK_UNIQUE;
	result = RETERR(-ENOENT);

	/*
	 * this is time-critical function and dragons had, hence, been settled
	 * here.
	 *
	 * Loop below scans cbk cache slots trying to find matching node with
	 * suitable range of delimiting keys and located at the h->level.
	 *
	 * Scan is done under cbk cache spin lock that protects slot->node
	 * pointers. If suitable node is found we want to pin it in
	 * memory. But slot->node can point to the node with x_count 0
	 * (unreferenced). Such node can be recycled at any moment, or can
	 * already be in the process of being recycled (within jput()).
	 *
	 * As we found node in the cbk cache, it means that jput() hasn't yet
	 * called cbk_cache_invalidate().
	 *
	 * We acquire reference to the node without holding tree lock, and
	 * later, check node's RIP bit. This avoids races with jput().
	 */

	rcu_read_lock();
	read_lock(&((cbk_cache *)cache)->guard);

	slot = list_entry(cache->lru.next, cbk_cache_slot, lru);
	slot = list_entry(slot->lru.prev, cbk_cache_slot, lru);
	BUG_ON(&slot->lru != &cache->lru);/*????*/
	while (1) {

		slot = list_entry(slot->lru.next, cbk_cache_slot, lru);

		if (&cache->lru != &slot->lru)
			node = slot->node;
		else
			node = NULL;

		if (unlikely(node == NULL))
			break;

		/*
		 * this is (hopefully) the only place in the code where we are
		 * working with delimiting keys without holding dk lock. This
		 * is fine here, because this is only "guess" anyway---keys
		 * are rechecked under dk lock below.
		 */
		if (znode_get_level(node) == level &&
		    /* reiser4_min_key < key < reiser4_max_key */
		    znode_contains_key_strict(node, key, isunique)) {
			zref(node);
			result = 0;
			spin_lock_prefetch(&tree->tree_lock);
			break;
		}
	}
	read_unlock(&((cbk_cache *)cache)->guard);

	assert("nikita-2475", cbk_cache_invariant(cache));

	if (unlikely(result == 0 && ZF_ISSET(node, JNODE_RIP)))
		result = -ENOENT;

	rcu_read_unlock();

	if (result != 0) {
		h->result = CBK_COORD_NOTFOUND;
		return RETERR(-ENOENT);
	}

	result =
	    longterm_lock_znode(h->active_lh, node, cbk_lock_mode(level, h),
				ZNODE_LOCK_LOPRI);
	zput(node);
	if (result != 0)
		return result;
	result = zload(node);
	if (result != 0)
		return result;

	/* recheck keys */
	read_lock_dk(tree);
	result = (znode_contains_key_strict(node, key, isunique) &&
		!ZF_ISSET(node, JNODE_HEARD_BANSHEE));
	read_unlock_dk(tree);
	if (result) {
		/* do lookup inside node */
		llr = cbk_node_lookup(h);
		/* if cbk_node_lookup() wandered to another node (due to eottl
		   or non-unique keys), adjust @node */
		/*node = h->active_lh->node; */

		if (llr != LOOKUP_DONE) {
			/* restart or continue on the next level */
			result = RETERR(-ENOENT);
		} else if (IS_CBKERR(h->result))
			/* io or oom */
			result = RETERR(-ENOENT);
		else {
			/* good. Either item found or definitely not found. */
			result = 0;

			write_lock(&(cache->guard));
			if (slot->node == h->active_lh->node) {
				/* if this node is still in cbk cache---move
				   its slot to the head of the LRU list. */
				list_move(&slot->lru, &cache->lru);
			}
			write_unlock(&(cache->guard));
		}
	} else {
		/* race. While this thread was waiting for the lock, node was
		   rebalanced and item we are looking for, shifted out of it
		   (if it ever was here).

		   Continuing scanning is almost hopeless: node key range was
		   moved to, is almost certainly at the beginning of the LRU
		   list at this time, because it's hot, but restarting
		   scanning from the very beginning is complex. Just return,
		   so that cbk() will be performed. This is not that
		   important, because such races should be rare. Are they?
		 */
		result = RETERR(-ENOENT);	/* -ERAUGHT */
	}
	zrelse(node);
	assert("nikita-2476", cbk_cache_invariant(cache));
	return result;
}

/* look for item with given key in the coord cache

   This function, called by coord_by_key(), scans "coord cache" (&cbk_cache)
   which is a small LRU list of znodes accessed lately. For each znode in
   znode in this list, it checks whether key we are looking for fits into key
   range covered by this node. If so, and in addition, node lies at allowed
   level (this is to handle extents on a twig level), node is locked, and
   lookup inside it is performed.

   we need a measurement of the cost of this cache search compared to the cost
   of coord_by_key.

*/
static int cbk_cache_search(cbk_handle * h/* cbk handle */)
{
	int result = 0;
	tree_level level;

	/* add CBK_IN_CACHE to the handle flags. This means that
	 * cbk_node_lookup() assumes that cbk_cache is scanned and would add
	 * found node to the cache. */
	h->flags |= CBK_IN_CACHE;
	for (level = h->stop_level; level <= h->lock_level; ++level) {
		h->level = level;
		result = cbk_cache_scan_slots(h);
		if (result != 0) {
			done_lh(h->active_lh);
			done_lh(h->parent_lh);
		} else {
			assert("nikita-1319", !IS_CBKERR(h->result));
			break;
		}
	}
	h->flags &= ~CBK_IN_CACHE;
	return result;
}

/* type of lock we want to obtain during tree traversal. On stop level
    we want type of lock user asked for, on upper levels: read lock. */
znode_lock_mode cbk_lock_mode(tree_level level, cbk_handle * h)
{
	assert("nikita-382", h != NULL);

	return (level <= h->lock_level) ? h->lock_mode : ZNODE_READ_LOCK;
}

/* update outdated delimiting keys */
static void stale_dk(reiser4_tree * tree, znode * node)
{
	znode *right;

	read_lock_tree(tree);
	write_lock_dk(tree);
	right = node->right;

	if (ZF_ISSET(node, JNODE_RIGHT_CONNECTED) &&
	    right && ZF_ISSET(right, JNODE_DKSET) &&
	    !keyeq(znode_get_rd_key(node), znode_get_ld_key(right)))
		znode_set_rd_key(node, znode_get_ld_key(right));

	write_unlock_dk(tree);
	read_unlock_tree(tree);
}

/* check for possibly outdated delimiting keys, and update them if
 * necessary. */
static void update_stale_dk(reiser4_tree * tree, znode * node)
{
	znode *right;
	reiser4_key rd;

	read_lock_tree(tree);
	read_lock_dk(tree);
	rd = *znode_get_rd_key(node);
	right = node->right;
	if (unlikely(ZF_ISSET(node, JNODE_RIGHT_CONNECTED) &&
		     right && ZF_ISSET(right, JNODE_DKSET) &&
		     !keyeq(&rd, znode_get_ld_key(right)))) {
		assert("nikita-38211", ZF_ISSET(node, JNODE_DKSET));
		read_unlock_dk(tree);
		read_unlock_tree(tree);
		stale_dk(tree, node);
		return;
	}
	read_unlock_dk(tree);
	read_unlock_tree(tree);
}

/*
 * handle searches a the non-unique key.
 *
 * Suppose that we are looking for an item with possibly non-unique key 100.
 *
 * Root node contains two pointers: one to a node with left delimiting key 0,
 * and another to a node with left delimiting key 100. Item we interested in
 * may well happen in the sub-tree rooted at the first pointer.
 *
 * To handle this search_to_left() is called when search reaches stop
 * level. This function checks it is _possible_ that item we are looking for
 * is in the left neighbor (this can be done by comparing delimiting keys) and
 * if so, tries to lock left neighbor (this is low priority lock, so it can
 * deadlock, tree traversal is just restarted if it did) and then checks
 * whether left neighbor actually contains items with our key.
 *
 * Note that this is done on the stop level only. It is possible to try such
 * left-check on each level, but as duplicate keys are supposed to be rare
 * (very unlikely that more than one node is completely filled with items with
 * duplicate keys), it sis cheaper to scan to the left on the stop level once.
 *
 */
static level_lookup_result search_to_left(cbk_handle * h/* search handle */)
{
	level_lookup_result result;
	coord_t *coord;
	znode *node;
	znode *neighbor;

	lock_handle lh;

	assert("nikita-1761", h != NULL);
	assert("nikita-1762", h->level == h->stop_level);

	init_lh(&lh);
	coord = h->coord;
	node = h->active_lh->node;
	assert("nikita-1763", coord_is_leftmost_unit(coord));

	h->result =
	    reiser4_get_left_neighbor(&lh, node, (int)h->lock_mode,
				      GN_CAN_USE_UPPER_LEVELS);
	neighbor = NULL;
	switch (h->result) {
	case -E_DEADLOCK:
		result = LOOKUP_REST;
		break;
	case 0:{
			node_plugin *nplug;
			coord_t crd;
			lookup_bias bias;

			neighbor = lh.node;
			h->result = zload(neighbor);
			if (h->result != 0) {
				result = LOOKUP_DONE;
				break;
			}

			nplug = neighbor->nplug;

			coord_init_zero(&crd);
			bias = h->bias;
			h->bias = FIND_EXACT;
			h->result =
			    nplug->lookup(neighbor, h->key, h->bias, &crd);
			h->bias = bias;

			if (h->result == NS_NOT_FOUND) {
	case -E_NO_NEIGHBOR:
				h->result = CBK_COORD_FOUND;
				if (!(h->flags & CBK_IN_CACHE))
					cbk_cache_add(node);
	default:		/* some other error */
				result = LOOKUP_DONE;
			} else if (h->result == NS_FOUND) {
				read_lock_dk(znode_get_tree(neighbor));
				h->rd_key = *znode_get_ld_key(node);
				leftmost_key_in_node(neighbor, &h->ld_key);
				read_unlock_dk(znode_get_tree(neighbor));
				h->flags |= CBK_DKSET;

				h->block = *znode_get_block(neighbor);
				/* clear coord->node so that cbk_level_lookup()
				   wouldn't overwrite parent hint in neighbor.

				   Parent hint was set up by
				   reiser4_get_left_neighbor()
				 */
				/* FIXME: why do we have to spinlock here? */
				write_lock_tree(znode_get_tree(neighbor));
				h->coord->node = NULL;
				write_unlock_tree(znode_get_tree(neighbor));
				result = LOOKUP_CONT;
			} else {
				result = LOOKUP_DONE;
			}
			if (neighbor != NULL)
				zrelse(neighbor);
		}
	}
	done_lh(&lh);
	return result;
}

/* debugging aid: return symbolic name of search bias */
static const char *bias_name(lookup_bias bias/* bias to get name of */)
{
	if (bias == FIND_EXACT)
		return "exact";
	else if (bias == FIND_MAX_NOT_MORE_THAN)
		return "left-slant";
/* 	else if( bias == RIGHT_SLANT_BIAS ) */
/* 		return "right-bias"; */
	else {
		static char buf[30];

		sprintf(buf, "unknown: %i", bias);
		return buf;
	}
}

#if REISER4_DEBUG
/* debugging aid: print human readable information about @p */
void print_coord_content(const char *prefix /* prefix to print */ ,
			 coord_t *p/* coord to print */)
{
	reiser4_key key;

	if (p == NULL) {
		printk("%s: null\n", prefix);
		return;
	}
	if ((p->node != NULL) && znode_is_loaded(p->node)
	    && coord_is_existing_item(p))
		printk("%s: data: %p, length: %i\n", prefix,
		       item_body_by_coord(p), item_length_by_coord(p));
	if (znode_is_loaded(p->node)) {
		item_key_by_coord(p, &key);
		reiser4_print_key(prefix, &key);
	}
}

/* debugging aid: print human readable information about @block */
void reiser4_print_address(const char *prefix /* prefix to print */ ,
		   const reiser4_block_nr * block/* block number to print */)
{
	printk("%s: %s\n", prefix, sprint_address(block));
}
#endif

/* return string containing human readable representation of @block */
char *sprint_address(const reiser4_block_nr *
		     block/* block number to print */)
{
	static char address[30];

	if (block == NULL)
		sprintf(address, "null");
	else if (reiser4_blocknr_is_fake(block))
		sprintf(address, "%llx", (unsigned long long)(*block));
	else
		sprintf(address, "%llu", (unsigned long long)(*block));
	return address;
}

/* release parent node during traversal */
static void put_parent(cbk_handle * h/* search handle */)
{
	assert("nikita-383", h != NULL);
	if (h->parent_lh->node != NULL)
		longterm_unlock_znode(h->parent_lh);
}

/* helper function used by coord_by_key(): release reference to parent znode
   stored in handle before processing its child. */
static void hput(cbk_handle * h/* search handle */)
{
	assert("nikita-385", h != NULL);
	done_lh(h->parent_lh);
	done_lh(h->active_lh);
}

/* Helper function used by cbk(): update delimiting keys of child node (stored
   in h->active_lh->node) using key taken from parent on the parent level. */
static int setup_delimiting_keys(cbk_handle * h/* search handle */)
{
	znode *active;
	reiser4_tree *tree;

	assert("nikita-1088", h != NULL);

	active = h->active_lh->node;

	/* fast check without taking dk lock. This is safe, because
	 * JNODE_DKSET is never cleared once set. */
	if (!ZF_ISSET(active, JNODE_DKSET)) {
		tree = znode_get_tree(active);
		write_lock_dk(tree);
		if (!ZF_ISSET(active, JNODE_DKSET)) {
			znode_set_ld_key(active, &h->ld_key);
			znode_set_rd_key(active, &h->rd_key);
			ZF_SET(active, JNODE_DKSET);
		}
		write_unlock_dk(tree);
		return 1;
	}
	return 0;
}

/* true if @block makes sense for the @tree. Used to detect corrupted node
 * pointers */
static int
block_nr_is_correct(reiser4_block_nr * block /* block number to check */ ,
		    reiser4_tree * tree/* tree to check against */)
{
	assert("nikita-757", block != NULL);
	assert("nikita-758", tree != NULL);

	/* check to see if it exceeds the size of the device. */
	return reiser4_blocknr_is_sane_for(tree->super, block);
}

/* check consistency of fields */
static int sanity_check(cbk_handle * h/* search handle */)
{
	assert("nikita-384", h != NULL);

	if (h->level < h->stop_level) {
		h->error = "Buried under leaves";
		h->result = RETERR(-EIO);
		return LOOKUP_DONE;
	} else if (!block_nr_is_correct(&h->block, h->tree)) {
		h->error = "bad block number";
		h->result = RETERR(-EIO);
		return LOOKUP_DONE;
	} else
		return 0;
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
