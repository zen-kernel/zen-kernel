/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */
/* Znode manipulation functions. */
/* Znode is the in-memory header for a tree node. It is stored
   separately from the node itself so that it does not get written to
   disk.  In this respect znode is like buffer head or page head. We
   also use znodes for additional reiser4 specific purposes:

    . they are organized into tree structure which is a part of whole
      reiser4 tree.
    . they are used to implement node grained locking
    . they are used to keep additional state associated with a
      node
    . they contain links to lists used by the transaction manager

   Znode is attached to some variable "block number" which is instance of
   fs/reiser4/tree.h:reiser4_block_nr type. Znode can exist without
   appropriate node being actually loaded in memory. Existence of znode itself
   is regulated by reference count (->x_count) in it. Each time thread
   acquires reference to znode through call to zget(), ->x_count is
   incremented and decremented on call to zput().  Data (content of node) are
   brought in memory through call to zload(), which also increments ->d_count
   reference counter.  zload can block waiting on IO.  Call to zrelse()
   decreases this counter. Also, ->c_count keeps track of number of child
   znodes and prevents parent znode from being recycled until all of its
   children are. ->c_count is decremented whenever child goes out of existence
   (being actually recycled in zdestroy()) which can be some time after last
   reference to this child dies if we support some form of LRU cache for
   znodes.

*/
/* EVERY ZNODE'S STORY

   1. His infancy.

   Once upon a time, the znode was born deep inside of zget() by call to
   zalloc(). At the return from zget() znode had:

    . reference counter (x_count) of 1
    . assigned block number, marked as used in bitmap
    . pointer to parent znode. Root znode parent pointer points
      to its father: "fake" znode. This, in turn, has NULL parent pointer.
    . hash table linkage
    . no data loaded from disk
    . no node plugin
    . no sibling linkage

   2. His childhood

   Each node is either brought into memory as a result of tree traversal, or
   created afresh, creation of the root being a special case of the latter. In
   either case it's inserted into sibling list. This will typically require
   some ancillary tree traversing, but ultimately both sibling pointers will
   exist and JNODE_LEFT_CONNECTED and JNODE_RIGHT_CONNECTED will be true in
   zjnode.state.

   3. His youth.

   If znode is bound to already existing node in a tree, its content is read
   from the disk by call to zload(). At that moment, JNODE_LOADED bit is set
   in zjnode.state and zdata() function starts to return non null for this
   znode. zload() further calls zparse() that determines which node layout
   this node is rendered in, and sets ->nplug on success.

   If znode is for new node just created, memory for it is allocated and
   zinit_new() function is called to initialise data, according to selected
   node layout.

   4. His maturity.

   After this point, znode lingers in memory for some time. Threads can
   acquire references to znode either by blocknr through call to zget(), or by
   following a pointer to unallocated znode from internal item. Each time
   reference to znode is obtained, x_count is increased. Thread can read/write
   lock znode. Znode data can be loaded through calls to zload(), d_count will
   be increased appropriately. If all references to znode are released
   (x_count drops to 0), znode is not recycled immediately. Rather, it is
   still cached in the hash table in the hope that it will be accessed
   shortly.

   There are two ways in which znode existence can be terminated:

    . sudden death: node bound to this znode is removed from the tree
    . overpopulation: znode is purged out of memory due to memory pressure

   5. His death.

   Death is complex process.

   When we irrevocably commit ourselves to decision to remove node from the
   tree, JNODE_HEARD_BANSHEE bit is set in zjnode.state of corresponding
   znode. This is done either in ->kill_hook() of internal item or in
   reiser4_kill_root() function when tree root is removed.

   At this moment znode still has:

    . locks held on it, necessary write ones
    . references to it
    . disk block assigned to it
    . data loaded from the disk
    . pending requests for lock

   But once JNODE_HEARD_BANSHEE bit set, last call to unlock_znode() does node
   deletion. Node deletion includes two phases. First all ways to get
   references to that znode (sibling and parent links and hash lookup using
   block number stored in parent node) should be deleted -- it is done through
   sibling_list_remove(), also we assume that nobody uses down link from
   parent node due to its nonexistence or proper parent node locking and
   nobody uses parent pointers from children due to absence of them. Second we
   invalidate all pending lock requests which still are on znode's lock
   request queue, this is done by reiser4_invalidate_lock(). Another
   JNODE_IS_DYING znode status bit is used to invalidate pending lock requests.
   Once it set all requesters are forced to return -EINVAL from
   longterm_lock_znode(). Future locking attempts are not possible because all
   ways to get references to that znode are removed already. Last, node is
   uncaptured from transaction.

   When last reference to the dying znode is just about to be released,
   block number for this lock is released and znode is removed from the
   hash table.

   Now znode can be recycled.

   [it's possible to free bitmap block and remove znode from the hash
   table when last lock is released. This will result in having
   referenced but completely orphaned znode]

   6. Limbo

   As have been mentioned above znodes with reference counter 0 are
   still cached in a hash table. Once memory pressure increases they are
   purged out of there [this requires something like LRU list for
   efficient implementation. LRU list would also greatly simplify
   implementation of coord cache that would in this case morph to just
   scanning some initial segment of LRU list]. Data loaded into
   unreferenced znode are flushed back to the durable storage if
   necessary and memory is freed. Znodes themselves can be recycled at
   this point too.

*/

#include "debug.h"
#include "dformat.h"
#include "key.h"
#include "coord.h"
#include "plugin/plugin_header.h"
#include "plugin/node/node.h"
#include "plugin/plugin.h"
#include "txnmgr.h"
#include "jnode.h"
#include "znode.h"
#include "block_alloc.h"
#include "tree.h"
#include "tree_walk.h"
#include "super.h"
#include "reiser4.h"

#include <linux/pagemap.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/err.h>

static z_hash_table *get_htable(reiser4_tree *,
				const reiser4_block_nr * const blocknr);
static z_hash_table *znode_get_htable(const znode *);
static void zdrop(znode *);

/* hash table support */

/* compare two block numbers for equality. Used by hash-table macros */
static inline int
blknreq(const reiser4_block_nr * b1, const reiser4_block_nr * b2)
{
	assert("nikita-534", b1 != NULL);
	assert("nikita-535", b2 != NULL);

	return *b1 == *b2;
}

/* Hash znode by block number. Used by hash-table macros */
/* Audited by: umka (2002.06.11) */
static inline __u32
blknrhashfn(z_hash_table * table, const reiser4_block_nr * b)
{
	assert("nikita-536", b != NULL);

	return *b & (REISER4_ZNODE_HASH_TABLE_SIZE - 1);
}

/* The hash table definition */
#define KMALLOC(size) kmalloc((size), reiser4_ctx_gfp_mask_get())
#define KFREE(ptr, size) kfree(ptr)
TYPE_SAFE_HASH_DEFINE(z, znode, reiser4_block_nr, zjnode.key.z, zjnode.link.z,
		      blknrhashfn, blknreq);
#undef KFREE
#undef KMALLOC

/* slab for znodes */
static struct kmem_cache *znode_cache;

int znode_shift_order;

/**
 * init_znodes - create znode cache
 *
 * Initializes slab cache of znodes. It is part of reiser4 module initialization.
 */
int init_znodes(void)
{
	znode_cache = kmem_cache_create("znode", sizeof(znode), 0,
					SLAB_HWCACHE_ALIGN |
					SLAB_RECLAIM_ACCOUNT, NULL);
	if (znode_cache == NULL)
		return RETERR(-ENOMEM);

	for (znode_shift_order = 0; (1 << znode_shift_order) < sizeof(znode);
	     ++znode_shift_order);
	--znode_shift_order;
	return 0;
}

/**
 * done_znodes - delete znode cache
 *
 * This is called on reiser4 module unloading or system shutdown.
 */
void done_znodes(void)
{
	destroy_reiser4_cache(&znode_cache);
}

/* call this to initialise tree of znodes */
int znodes_tree_init(reiser4_tree * tree /* tree to initialise znodes for */ )
{
	int result;
	assert("umka-050", tree != NULL);

	rwlock_init(&tree->dk_lock);

	result = z_hash_init(&tree->zhash_table, REISER4_ZNODE_HASH_TABLE_SIZE);
	if (result != 0)
		return result;
	result = z_hash_init(&tree->zfake_table, REISER4_ZNODE_HASH_TABLE_SIZE);
	return result;
}

/* free this znode */
void zfree(znode * node /* znode to free */ )
{
	assert("nikita-465", node != NULL);
	assert("nikita-2120", znode_page(node) == NULL);
	assert("nikita-2301", list_empty_careful(&node->lock.owners));
	assert("nikita-2302", list_empty_careful(&node->lock.requestors));
	assert("nikita-2663", (list_empty_careful(&ZJNODE(node)->capture_link) &&
			       NODE_LIST(ZJNODE(node)) == NOT_CAPTURED));
	assert("nikita-3220", list_empty(&ZJNODE(node)->jnodes));
	assert("nikita-3293", !znode_is_right_connected(node));
	assert("nikita-3294", !znode_is_left_connected(node));
	assert("nikita-3295", node->left == NULL);
	assert("nikita-3296", node->right == NULL);

	/* not yet phash_jnode_destroy(ZJNODE(node)); */

	kmem_cache_free(znode_cache, node);
}

/* call this to free tree of znodes */
void znodes_tree_done(reiser4_tree * tree /* tree to finish with znodes of */ )
{
	znode *node;
	znode *next;
	z_hash_table *ztable;

	/* scan znode hash-tables and kill all znodes, then free hash tables
	 * themselves. */

	assert("nikita-795", tree != NULL);

	ztable = &tree->zhash_table;

	if (ztable->_table != NULL) {
		for_all_in_htable(ztable, z, node, next) {
			node->c_count = 0;
			node->in_parent.node = NULL;
			assert("nikita-2179", atomic_read(&ZJNODE(node)->x_count) == 0);
			zdrop(node);
		}

		z_hash_done(&tree->zhash_table);
	}

	ztable = &tree->zfake_table;

	if (ztable->_table != NULL) {
		for_all_in_htable(ztable, z, node, next) {
			node->c_count = 0;
			node->in_parent.node = NULL;
			assert("nikita-2179", atomic_read(&ZJNODE(node)->x_count) == 0);
			zdrop(node);
		}

		z_hash_done(&tree->zfake_table);
	}
}

/* ZNODE STRUCTURES */

/* allocate fresh znode */
znode *zalloc(gfp_t gfp_flag /* allocation flag */ )
{
	znode *node;

	node = kmem_cache_alloc(znode_cache, gfp_flag);
	return node;
}

/* Initialize fields of znode
   @node:    znode to initialize;
   @parent:  parent znode;
   @tree:    tree we are in. */
void zinit(znode * node, const znode * parent, reiser4_tree * tree)
{
	assert("nikita-466", node != NULL);
	assert("umka-268", current_tree != NULL);

	memset(node, 0, sizeof *node);

	assert("umka-051", tree != NULL);

	jnode_init(&node->zjnode, tree, JNODE_FORMATTED_BLOCK);
	reiser4_init_lock(&node->lock);
	init_parent_coord(&node->in_parent, parent);
}

/*
 * remove znode from indices. This is called jput() when last reference on
 * znode is released.
 */
void znode_remove(znode * node /* znode to remove */ , reiser4_tree * tree)
{
	assert("nikita-2108", node != NULL);
	assert("nikita-470", node->c_count == 0);
	assert_rw_write_locked(&(tree->tree_lock));

	/* remove reference to this znode from cbk cache */
	cbk_cache_invalidate(node, tree);

	/* update c_count of parent */
	if (znode_parent(node) != NULL) {
		assert("nikita-472", znode_parent(node)->c_count > 0);
		/* father, onto your hands I forward my spirit... */
		znode_parent(node)->c_count--;
		node->in_parent.node = NULL;
	} else {
		/* orphaned znode?! Root? */
	}

	/* remove znode from hash-table */
	z_hash_remove_rcu(znode_get_htable(node), node);
}

/* zdrop() -- Remove znode from the tree.

   This is called when znode is removed from the memory. */
static void zdrop(znode * node /* znode to finish with */ )
{
	jdrop(ZJNODE(node));
}

/*
 * put znode into right place in the hash table. This is called by relocate
 * code.
 */
int znode_rehash(znode * node /* node to rehash */ ,
		 const reiser4_block_nr * new_block_nr /* new block number */ )
{
	z_hash_table *oldtable;
	z_hash_table *newtable;
	reiser4_tree *tree;

	assert("nikita-2018", node != NULL);

	tree = znode_get_tree(node);
	oldtable = znode_get_htable(node);
	newtable = get_htable(tree, new_block_nr);

	write_lock_tree(tree);
	/* remove znode from hash-table */
	z_hash_remove_rcu(oldtable, node);

	/* assertion no longer valid due to RCU */
	/* assert("nikita-2019", z_hash_find(newtable, new_block_nr) == NULL); */

	/* update blocknr */
	znode_set_block(node, new_block_nr);
	node->zjnode.key.z = *new_block_nr;

	/* insert it into hash */
	z_hash_insert_rcu(newtable, node);
	write_unlock_tree(tree);
	return 0;
}

/* ZNODE LOOKUP, GET, PUT */

/* zlook() - get znode with given block_nr in a hash table or return NULL

   If result is non-NULL then the znode's x_count is incremented.  Internal version
   accepts pre-computed hash index.  The hash table is accessed under caller's
   tree->hash_lock.
*/
znode *zlook(reiser4_tree * tree, const reiser4_block_nr * const blocknr)
{
	znode *result;
	__u32 hash;
	z_hash_table *htable;

	assert("jmacd-506", tree != NULL);
	assert("jmacd-507", blocknr != NULL);

	htable = get_htable(tree, blocknr);
	hash = blknrhashfn(htable, blocknr);

	rcu_read_lock();
	result = z_hash_find_index(htable, hash, blocknr);

	if (result != NULL) {
		add_x_ref(ZJNODE(result));
		result = znode_rip_check(tree, result);
	}
	rcu_read_unlock();

	return result;
}

/* return hash table where znode with block @blocknr is (or should be)
 * stored */
static z_hash_table *get_htable(reiser4_tree * tree,
				const reiser4_block_nr * const blocknr)
{
	z_hash_table *table;
	if (is_disk_addr_unallocated(blocknr))
		table = &tree->zfake_table;
	else
		table = &tree->zhash_table;
	return table;
}

/* return hash table where znode @node is (or should be) stored */
static z_hash_table *znode_get_htable(const znode * node)
{
	return get_htable(znode_get_tree(node), znode_get_block(node));
}

/* zget() - get znode from hash table, allocating it if necessary.

   First a call to zlook, locating a x-referenced znode if one
   exists.  If znode is not found, allocate new one and return.  Result
   is returned with x_count reference increased.

   LOCKS TAKEN:   TREE_LOCK, ZNODE_LOCK
   LOCK ORDERING: NONE
*/
znode *zget(reiser4_tree * tree,
	    const reiser4_block_nr * const blocknr,
	    znode * parent, tree_level level, gfp_t gfp_flag)
{
	znode *result;
	__u32 hashi;

	z_hash_table *zth;

	assert("jmacd-512", tree != NULL);
	assert("jmacd-513", blocknr != NULL);
	assert("jmacd-514", level < REISER4_MAX_ZTREE_HEIGHT);

	zth = get_htable(tree, blocknr);
	hashi = blknrhashfn(zth, blocknr);

	/* NOTE-NIKITA address-as-unallocated-blocknr still is not
	   implemented. */

	z_hash_prefetch_bucket(zth, hashi);

	rcu_read_lock();
	/* Find a matching BLOCKNR in the hash table.  If the znode is found,
	   we obtain an reference (x_count) but the znode remains unlocked.
	   Have to worry about race conditions later. */
	result = z_hash_find_index(zth, hashi, blocknr);
	/* According to the current design, the hash table lock protects new
	   znode references. */
	if (result != NULL) {
		add_x_ref(ZJNODE(result));
		/* NOTE-NIKITA it should be so, but special case during
		   creation of new root makes such assertion highly
		   complicated.  */
		assert("nikita-2131", 1 || znode_parent(result) == parent ||
		       (ZF_ISSET(result, JNODE_ORPHAN)
			&& (znode_parent(result) == NULL)));
		result = znode_rip_check(tree, result);
	}

	rcu_read_unlock();

	if (!result) {
		znode *shadow;

		result = zalloc(gfp_flag);
		if (!result) {
			return ERR_PTR(RETERR(-ENOMEM));
		}

		zinit(result, parent, tree);
		ZJNODE(result)->blocknr = *blocknr;
		ZJNODE(result)->key.z = *blocknr;
		result->level = level;

		write_lock_tree(tree);

		shadow = z_hash_find_index(zth, hashi, blocknr);
		if (unlikely(shadow != NULL && !ZF_ISSET(shadow, JNODE_RIP))) {
			jnode_list_remove(ZJNODE(result));
			zfree(result);
			result = shadow;
		} else {
			result->version = znode_build_version(tree);
			z_hash_insert_index_rcu(zth, hashi, result);

			if (parent != NULL)
				++parent->c_count;
		}

		add_x_ref(ZJNODE(result));

		write_unlock_tree(tree);
	}
#if REISER4_DEBUG
	if (!reiser4_blocknr_is_fake(blocknr) && *blocknr != 0)
		reiser4_check_block(blocknr, 1);
#endif
	/* Check for invalid tree level, return -EIO */
	if (unlikely(znode_get_level(result) != level)) {
		warning("jmacd-504",
			"Wrong level for cached block %llu: %i expecting %i",
			(unsigned long long)(*blocknr), znode_get_level(result),
			level);
		zput(result);
		return ERR_PTR(RETERR(-EIO));
	}

	assert("nikita-1227", znode_invariant(result));

	return result;
}

/* ZNODE PLUGINS/DATA */

/* "guess" plugin for node loaded from the disk. Plugin id of node plugin is
   stored at the fixed offset from the beginning of the node. */
static node_plugin *znode_guess_plugin(const znode * node	/* znode to guess
								 * plugin of */ )
{
	reiser4_tree *tree;

	assert("nikita-1053", node != NULL);
	assert("nikita-1055", zdata(node) != NULL);

	tree = znode_get_tree(node);
	assert("umka-053", tree != NULL);

	if (reiser4_is_set(tree->super, REISER4_ONE_NODE_PLUGIN)) {
		return tree->nplug;
	} else {
		return node_plugin_by_disk_id
		    (tree, &((common_node_header *) zdata(node))->plugin_id);
#ifdef GUESS_EXISTS
		reiser4_plugin *plugin;

		/* NOTE-NIKITA add locking here when dynamic plugins will be
		 * implemented */
		for_all_plugins(REISER4_NODE_PLUGIN_TYPE, plugin) {
			if ((plugin->u.node.guess != NULL)
			    && plugin->u.node.guess(node))
				return plugin;
		}
		warning("nikita-1057", "Cannot guess node plugin");
		print_znode("node", node);
		return NULL;
#endif
	}
}

/* parse node header and install ->node_plugin */
int zparse(znode * node /* znode to parse */ )
{
	int result;

	assert("nikita-1233", node != NULL);
	assert("nikita-2370", zdata(node) != NULL);

	if (node->nplug == NULL) {
		node_plugin *nplug;

		nplug = znode_guess_plugin(node);
		if (likely(nplug != NULL)) {
			result = nplug->parse(node);
			if (likely(result == 0))
				node->nplug = nplug;
		} else {
			result = RETERR(-EIO);
		}
	} else
		result = 0;
	return result;
}

/* zload with readahead */
int zload_ra(znode * node /* znode to load */ , ra_info_t * info)
{
	int result;

	assert("nikita-484", node != NULL);
	assert("nikita-1377", znode_invariant(node));
	assert("jmacd-7771", !znode_above_root(node));
	assert("nikita-2125", atomic_read(&ZJNODE(node)->x_count) > 0);
	assert("nikita-3016", reiser4_schedulable());

	if (info)
		formatted_readahead(node, info);

	result = jload(ZJNODE(node));
	assert("nikita-1378", znode_invariant(node));
	return result;
}

/* load content of node into memory */
int zload(znode * node)
{
	return zload_ra(node, NULL);
}

/* call node plugin to initialise newly allocated node. */
int zinit_new(znode * node /* znode to initialise */ , gfp_t gfp_flags)
{
	return jinit_new(ZJNODE(node), gfp_flags);
}

/* drop reference to node data. When last reference is dropped, data are
   unloaded. */
void zrelse(znode * node /* znode to release references to */ )
{
	assert("nikita-1381", znode_invariant(node));

	jrelse(ZJNODE(node));
}

/* returns free space in node */
unsigned znode_free_space(znode * node /* znode to query */ )
{
	assert("nikita-852", node != NULL);
	return node_plugin_by_node(node)->free_space(node);
}

/* left delimiting key of znode */
reiser4_key *znode_get_rd_key(znode * node /* znode to query */ )
{
	assert("nikita-958", node != NULL);
	assert_rw_locked(&(znode_get_tree(node)->dk_lock));
	assert("nikita-3067", LOCK_CNT_GTZ(rw_locked_dk));
	assert("nikita-30671", node->rd_key_version != 0);
	return &node->rd_key;
}

/* right delimiting key of znode */
reiser4_key *znode_get_ld_key(znode * node /* znode to query */ )
{
	assert("nikita-974", node != NULL);
	assert_rw_locked(&(znode_get_tree(node)->dk_lock));
	assert("nikita-3068", LOCK_CNT_GTZ(rw_locked_dk));
	assert("nikita-30681", node->ld_key_version != 0);
	return &node->ld_key;
}

ON_DEBUG(atomic_t delim_key_version = ATOMIC_INIT(0);
    )

/* update right-delimiting key of @node */
reiser4_key *znode_set_rd_key(znode * node, const reiser4_key * key)
{
	assert("nikita-2937", node != NULL);
	assert("nikita-2939", key != NULL);
	assert_rw_write_locked(&(znode_get_tree(node)->dk_lock));
	assert("nikita-3069", LOCK_CNT_GTZ(write_locked_dk));
	assert("nikita-2944",
	       znode_is_any_locked(node) ||
	       znode_get_level(node) != LEAF_LEVEL ||
	       keyge(key, &node->rd_key) ||
	       keyeq(&node->rd_key, reiser4_min_key()) ||
	       ZF_ISSET(node, JNODE_HEARD_BANSHEE));

	node->rd_key = *key;
	ON_DEBUG(node->rd_key_version = atomic_inc_return(&delim_key_version));
	return &node->rd_key;
}

/* update left-delimiting key of @node */
reiser4_key *znode_set_ld_key(znode * node, const reiser4_key * key)
{
	assert("nikita-2940", node != NULL);
	assert("nikita-2941", key != NULL);
	assert_rw_write_locked(&(znode_get_tree(node)->dk_lock));
	assert("nikita-3070", LOCK_CNT_GTZ(write_locked_dk));
	assert("nikita-2943",
	       znode_is_any_locked(node) || keyeq(&node->ld_key,
						  reiser4_min_key()));

	node->ld_key = *key;
	ON_DEBUG(node->ld_key_version = atomic_inc_return(&delim_key_version));
	return &node->ld_key;
}

/* true if @key is inside key range for @node */
int znode_contains_key(znode * node /* znode to look in */ ,
		       const reiser4_key * key /* key to look for */ )
{
	assert("nikita-1237", node != NULL);
	assert("nikita-1238", key != NULL);

	/* left_delimiting_key <= key <= right_delimiting_key */
	return keyle(znode_get_ld_key(node), key)
	    && keyle(key, znode_get_rd_key(node));
}

/* same as znode_contains_key(), but lock dk lock */
int znode_contains_key_lock(znode * node /* znode to look in */ ,
			    const reiser4_key * key /* key to look for */ )
{
	int result;

	assert("umka-056", node != NULL);
	assert("umka-057", key != NULL);

	read_lock_dk(znode_get_tree(node));
	result = znode_contains_key(node, key);
	read_unlock_dk(znode_get_tree(node));
	return result;
}

/* get parent pointer, assuming tree is not locked */
znode *znode_parent_nolock(const znode * node /* child znode */ )
{
	assert("nikita-1444", node != NULL);
	return node->in_parent.node;
}

/* get parent pointer of znode */
znode *znode_parent(const znode * node /* child znode */ )
{
	assert("nikita-1226", node != NULL);
	assert("nikita-1406", LOCK_CNT_GTZ(rw_locked_tree));
	return znode_parent_nolock(node);
}

/* detect uber znode used to protect in-superblock tree root pointer */
int znode_above_root(const znode * node /* znode to query */ )
{
	assert("umka-059", node != NULL);

	return disk_addr_eq(&ZJNODE(node)->blocknr, &UBER_TREE_ADDR);
}

/* check that @node is root---that its block number is recorder in the tree as
   that of root node */
#if REISER4_DEBUG
static int znode_is_true_root(const znode * node /* znode to query */ )
{
	assert("umka-060", node != NULL);
	assert("umka-061", current_tree != NULL);

	return disk_addr_eq(znode_get_block(node),
			    &znode_get_tree(node)->root_block);
}
#endif

/* check that @node is root */
int znode_is_root(const znode * node /* znode to query */ )
{
	assert("nikita-1206", node != NULL);

	return znode_get_level(node) == znode_get_tree(node)->height;
}

/* Returns true is @node was just created by zget() and wasn't ever loaded
   into memory. */
/* NIKITA-HANS: yes */
int znode_just_created(const znode * node)
{
	assert("nikita-2188", node != NULL);
	return (znode_page(node) == NULL);
}

/* obtain updated ->znode_epoch. See seal.c for description. */
__u64 znode_build_version(reiser4_tree * tree)
{
	__u64 result;

	spin_lock(&tree->epoch_lock);
	result = ++tree->znode_epoch;
	spin_unlock(&tree->epoch_lock);
	return result;
}

void init_load_count(load_count * dh)
{
	assert("nikita-2105", dh != NULL);
	memset(dh, 0, sizeof *dh);
}

void done_load_count(load_count * dh)
{
	assert("nikita-2106", dh != NULL);
	if (dh->node != NULL) {
		for (; dh->d_ref > 0; --dh->d_ref)
			zrelse(dh->node);
		dh->node = NULL;
	}
}

static int incr_load_count(load_count * dh)
{
	int result;

	assert("nikita-2110", dh != NULL);
	assert("nikita-2111", dh->node != NULL);

	result = zload(dh->node);
	if (result == 0)
		++dh->d_ref;
	return result;
}

int incr_load_count_znode(load_count * dh, znode * node)
{
	assert("nikita-2107", dh != NULL);
	assert("nikita-2158", node != NULL);
	assert("nikita-2109",
	       ergo(dh->node != NULL, (dh->node == node) || (dh->d_ref == 0)));

	dh->node = node;
	return incr_load_count(dh);
}

int incr_load_count_jnode(load_count * dh, jnode * node)
{
	if (jnode_is_znode(node)) {
		return incr_load_count_znode(dh, JZNODE(node));
	}
	return 0;
}

void copy_load_count(load_count * new, load_count * old)
{
	int ret = 0;
	done_load_count(new);
	new->node = old->node;
	new->d_ref = 0;

	while ((new->d_ref < old->d_ref) && (ret = incr_load_count(new)) == 0) {
	}

	assert("jmacd-87589", ret == 0);
}

void move_load_count(load_count * new, load_count * old)
{
	done_load_count(new);
	new->node = old->node;
	new->d_ref = old->d_ref;
	old->node = NULL;
	old->d_ref = 0;
}

/* convert parent pointer into coord */
void parent_coord_to_coord(const parent_coord_t * pcoord, coord_t * coord)
{
	assert("nikita-3204", pcoord != NULL);
	assert("nikita-3205", coord != NULL);

	coord_init_first_unit_nocheck(coord, pcoord->node);
	coord_set_item_pos(coord, pcoord->item_pos);
	coord->between = AT_UNIT;
}

/* pack coord into parent_coord_t */
void coord_to_parent_coord(const coord_t * coord, parent_coord_t * pcoord)
{
	assert("nikita-3206", pcoord != NULL);
	assert("nikita-3207", coord != NULL);

	pcoord->node = coord->node;
	pcoord->item_pos = coord->item_pos;
}

/* Initialize a parent hint pointer. (parent hint pointer is a field in znode,
   look for comments there) */
void init_parent_coord(parent_coord_t * pcoord, const znode * node)
{
	pcoord->node = (znode *) node;
	pcoord->item_pos = (unsigned short)~0;
}

#if REISER4_DEBUG

/* debugging aid: znode invariant */
static int znode_invariant_f(const znode * node /* znode to check */ ,
			     char const **msg	/* where to store error
						 * message, if any */ )
{
#define _ergo(ant, con) 						\
	((*msg) = "{" #ant "} ergo {" #con "}", ergo((ant), (con)))

#define _equi(e1, e2) 						\
	((*msg) = "{" #e1 "} <=> {" #e2 "}", equi((e1), (e2)))

#define _check(exp) ((*msg) = #exp, (exp))

	return jnode_invariant_f(ZJNODE(node), msg) &&
	    /* [znode-fake] invariant */
	    /* fake znode doesn't have a parent, and */
	    _ergo(znode_get_level(node) == 0, znode_parent(node) == NULL) &&
	    /* there is another way to express this very check, and */
	    _ergo(znode_above_root(node), znode_parent(node) == NULL) &&
	    /* it has special block number, and */
	    _ergo(znode_get_level(node) == 0,
		  disk_addr_eq(znode_get_block(node), &UBER_TREE_ADDR)) &&
	    /* it is the only znode with such block number, and */
	    _ergo(!znode_above_root(node) && znode_is_loaded(node),
		  !disk_addr_eq(znode_get_block(node), &UBER_TREE_ADDR)) &&
	    /* it is parent of the tree root node */
	    _ergo(znode_is_true_root(node),
		  znode_above_root(znode_parent(node))) &&
	    /* [znode-level] invariant */
	    /* level of parent znode is one larger than that of child,
	       except for the fake znode, and */
	    _ergo(znode_parent(node) && !znode_above_root(znode_parent(node)),
		  znode_get_level(znode_parent(node)) ==
		  znode_get_level(node) + 1) &&
	    /* left neighbor is at the same level, and */
	    _ergo(znode_is_left_connected(node) && node->left != NULL,
		  znode_get_level(node) == znode_get_level(node->left)) &&
	    /* right neighbor is at the same level */
	    _ergo(znode_is_right_connected(node) && node->right != NULL,
		  znode_get_level(node) == znode_get_level(node->right)) &&
	    /* [znode-connected] invariant */
	    _ergo(node->left != NULL, znode_is_left_connected(node)) &&
	    _ergo(node->right != NULL, znode_is_right_connected(node)) &&
	    _ergo(!znode_is_root(node) && node->left != NULL,
		  znode_is_right_connected(node->left) &&
		  node->left->right == node) &&
	    _ergo(!znode_is_root(node) && node->right != NULL,
		  znode_is_left_connected(node->right) &&
		  node->right->left == node) &&
	    /* [znode-c_count] invariant */
	    /* for any znode, c_count of its parent is greater than 0 */
	    _ergo(znode_parent(node) != NULL &&
		  !znode_above_root(znode_parent(node)),
		  znode_parent(node)->c_count > 0) &&
	    /* leaves don't have children */
	    _ergo(znode_get_level(node) == LEAF_LEVEL,
		  node->c_count == 0) &&
	    _check(node->zjnode.jnodes.prev != NULL) &&
	    _check(node->zjnode.jnodes.next != NULL) &&
	    /* orphan doesn't have a parent */
	    _ergo(ZF_ISSET(node, JNODE_ORPHAN), znode_parent(node) == 0) &&
	    /* [znode-modify] invariant */
	    /* if znode is not write-locked, its checksum remains
	     * invariant */
	    /* unfortunately, zlock is unordered w.r.t. jnode_lock, so we
	     * cannot check this. */
	    /* [znode-refs] invariant */
	    /* only referenced znode can be long-term locked */
	    _ergo(znode_is_locked(node),
		  atomic_read(&ZJNODE(node)->x_count) != 0);
}

/* debugging aid: check znode invariant and panic if it doesn't hold */
int znode_invariant(znode * node /* znode to check */ )
{
	char const *failed_msg;
	int result;

	assert("umka-063", node != NULL);
	assert("umka-064", current_tree != NULL);

	spin_lock_znode(node);
	read_lock_tree(znode_get_tree(node));
	result = znode_invariant_f(node, &failed_msg);
	if (!result) {
		/* print_znode("corrupted node", node); */
		warning("jmacd-555", "Condition %s failed", failed_msg);
	}
	read_unlock_tree(znode_get_tree(node));
	spin_unlock_znode(node);
	return result;
}

/* return non-0 iff data are loaded into znode */
int znode_is_loaded(const znode * node /* znode to query */ )
{
	assert("nikita-497", node != NULL);
	return jnode_is_loaded(ZJNODE(node));
}

unsigned long znode_times_locked(const znode * z)
{
	return z->times_locked;
}

#endif				/* REISER4_DEBUG */

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   End:
*/
