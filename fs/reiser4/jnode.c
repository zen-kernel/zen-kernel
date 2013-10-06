/* Copyright 2001, 2002, 2003, 2004 by Hans Reiser, licensing governed by
 * reiser4/README */
/* Jnode manipulation functions. */
/* Jnode is entity used to track blocks with data and meta-data in reiser4.

   In particular, jnodes are used to track transactional information
   associated with each block. Each znode contains jnode as ->zjnode field.

   Jnode stands for either Josh or Journal node.
*/

/*
 * Taxonomy.
 *
 *     Jnode represents block containing data or meta-data. There are jnodes
 *     for:
 *
 *         unformatted blocks (jnodes proper). There are plans, however to
 *         have a handle per extent unit rather than per each unformatted
 *         block, because there are so many of them.
 *
 *         For bitmaps. Each bitmap is actually represented by two jnodes--one
 *         for working and another for "commit" data, together forming bnode.
 *
 *         For io-heads. These are used by log writer.
 *
 *         For formatted nodes (znode). See comment at the top of znode.c for
 *         details specific to the formatted nodes (znodes).
 *
 * Node data.
 *
 *     Jnode provides access to the data of node it represents. Data are
 *     stored in a page. Page is kept in a page cache. This means, that jnodes
 *     are highly interconnected with page cache and VM internals.
 *
 *     jnode has a pointer to page (->pg) containing its data. Pointer to data
 *     themselves is cached in ->data field to avoid frequent calls to
 *     page_address().
 *
 *     jnode and page are attached to each other by jnode_attach_page(). This
 *     function places pointer to jnode in set_page_private(), sets PG_private
 *     flag and increments page counter.
 *
 *     Opposite operation is performed by page_clear_jnode().
 *
 *     jnode->pg is protected by jnode spin lock, and page->private is
 *     protected by page lock. See comment at the top of page_cache.c for
 *     more.
 *
 *     page can be detached from jnode for two reasons:
 *
 *         . jnode is removed from a tree (file is truncated, of formatted
 *         node is removed by balancing).
 *
 *         . during memory pressure, VM calls ->releasepage() method
 *         (reiser4_releasepage()) to evict page from memory.
 *
 *    (there, of course, is also umount, but this is special case we are not
 *    concerned with here).
 *
 *    To protect jnode page from eviction, one calls jload() function that
 *    "pins" page in memory (loading it if necessary), increments
 *    jnode->d_count, and kmap()s page. Page is unpinned through call to
 *    jrelse().
 *
 * Jnode life cycle.
 *
 *    jnode is created, placed in hash table, and, optionally, in per-inode
 *    radix tree. Page can be attached to jnode, pinned, released, etc.
 *
 *    When jnode is captured into atom its reference counter is
 *    increased. While being part of an atom, jnode can be "early
 *    flushed". This means that as part of flush procedure, jnode is placed
 *    into "relocate set", and its page is submitted to the disk. After io
 *    completes, page can be detached, then loaded again, re-dirtied, etc.
 *
 *    Thread acquired reference to jnode by calling jref() and releases it by
 *    jput(). When last reference is removed, jnode is still retained in
 *    memory (cached) if it has page attached, _unless_ it is scheduled for
 *    destruction (has JNODE_HEARD_BANSHEE bit set).
 *
 *    Tree read-write lock was used as "existential" lock for jnodes. That is,
 *    jnode->x_count could be changed from 0 to 1 only under tree write lock,
 *    that is, tree lock protected unreferenced jnodes stored in the hash
 *    table, from recycling.
 *
 *    This resulted in high contention on tree lock, because jref()/jput() is
 *    frequent operation. To ameliorate this problem, RCU is used: when jput()
 *    is just about to release last reference on jnode it sets JNODE_RIP bit
 *    on it, and then proceed with jnode destruction (removing jnode from hash
 *    table, cbk_cache, detaching page, etc.). All places that change jnode
 *    reference counter from 0 to 1 (jlookup(), zlook(), zget(), and
 *    cbk_cache_scan_slots()) check for JNODE_RIP bit (this is done by
 *    jnode_rip_check() function), and pretend that nothing was found in hash
 *    table if bit is set.
 *
 *    jput defers actual return of jnode into slab cache to some later time
 *    (by call_rcu()), this guarantees that other threads can safely continue
 *    working with JNODE_RIP-ped jnode.
 *
 */

#include "reiser4.h"
#include "debug.h"
#include "dformat.h"
#include "jnode.h"
#include "plugin/plugin_header.h"
#include "plugin/plugin.h"
#include "txnmgr.h"
/*#include "jnode.h"*/
#include "znode.h"
#include "tree.h"
#include "tree_walk.h"
#include "super.h"
#include "inode.h"
#include "page_cache.h"

#include <asm/uaccess.h>	/* UML needs this for PAGE_OFFSET */
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/swap.h>
#include <linux/fs.h>		/* for struct address_space  */
#include <linux/writeback.h>	/* for inode_wb_list_lock */

static struct kmem_cache *_jnode_slab = NULL;

static void jnode_set_type(jnode * node, jnode_type type);
static int jdelete(jnode * node);
static int jnode_try_drop(jnode * node);

#if REISER4_DEBUG
static int jnode_invariant(jnode * node, int tlocked, int jlocked);
#endif

/* true if valid page is attached to jnode */
static inline int jnode_is_parsed(jnode * node)
{
	return JF_ISSET(node, JNODE_PARSED);
}

/* hash table support */

/* compare two jnode keys for equality. Used by hash-table macros */
static inline int jnode_key_eq(const struct jnode_key *k1,
			       const struct jnode_key *k2)
{
	assert("nikita-2350", k1 != NULL);
	assert("nikita-2351", k2 != NULL);

	return (k1->index == k2->index && k1->objectid == k2->objectid);
}

/* Hash jnode by its key (inode plus offset). Used by hash-table macros */
static inline __u32 jnode_key_hashfn(j_hash_table * table,
				     const struct jnode_key *key)
{
	assert("nikita-2352", key != NULL);
	assert("nikita-3346", IS_POW(table->_buckets));

	/* yes, this is remarkable simply (where not stupid) hash function. */
	return (key->objectid + key->index) & (table->_buckets - 1);
}

/* The hash table definition */
#define KMALLOC(size) reiser4_vmalloc(size)
#define KFREE(ptr, size) vfree(ptr)
TYPE_SAFE_HASH_DEFINE(j, jnode, struct jnode_key, key.j, link.j,
		      jnode_key_hashfn, jnode_key_eq);
#undef KFREE
#undef KMALLOC

/* call this to initialise jnode hash table */
int jnodes_tree_init(reiser4_tree * tree/* tree to initialise jnodes for */)
{
	assert("nikita-2359", tree != NULL);
	return j_hash_init(&tree->jhash_table, 16384);
}

/* call this to destroy jnode hash table. This is called during umount. */
int jnodes_tree_done(reiser4_tree * tree/* tree to destroy jnodes for */)
{
	j_hash_table *jtable;
	jnode *node;
	jnode *next;

	assert("nikita-2360", tree != NULL);

	/*
	 * Scan hash table and free all jnodes.
	 */
	jtable = &tree->jhash_table;
	if (jtable->_table) {
		for_all_in_htable(jtable, j, node, next) {
			assert("nikita-2361", !atomic_read(&node->x_count));
			jdrop(node);
		}

		j_hash_done(&tree->jhash_table);
	}
	return 0;
}

/**
 * init_jnodes - create jnode cache
 *
 * Initializes slab cache jnodes. It is part of reiser4 module initialization.
 */
int init_jnodes(void)
{
	assert("umka-168", _jnode_slab == NULL);

	_jnode_slab = kmem_cache_create("jnode", sizeof(jnode), 0,
					SLAB_HWCACHE_ALIGN |
					SLAB_RECLAIM_ACCOUNT, NULL);
	if (_jnode_slab == NULL)
		return RETERR(-ENOMEM);

	return 0;
}

/**
 * done_znodes - delete znode cache
 *
 * This is called on reiser4 module unloading or system shutdown.
 */
void done_jnodes(void)
{
	destroy_reiser4_cache(&_jnode_slab);
}

/* Initialize a jnode. */
void jnode_init(jnode * node, reiser4_tree * tree, jnode_type type)
{
	assert("umka-175", node != NULL);

	memset(node, 0, sizeof(jnode));
	ON_DEBUG(node->magic = JMAGIC);
	jnode_set_type(node, type);
	atomic_set(&node->d_count, 0);
	atomic_set(&node->x_count, 0);
	spin_lock_init(&node->guard);
	spin_lock_init(&node->load);
	node->atom = NULL;
	node->tree = tree;
	INIT_LIST_HEAD(&node->capture_link);

	ASSIGN_NODE_LIST(node, NOT_CAPTURED);

#if REISER4_DEBUG
	{
		reiser4_super_info_data *sbinfo;

		sbinfo = get_super_private(tree->super);
		spin_lock_irq(&sbinfo->all_guard);
		list_add(&node->jnodes, &sbinfo->all_jnodes);
		spin_unlock_irq(&sbinfo->all_guard);
	}
#endif
}

#if REISER4_DEBUG
/*
 * Remove jnode from ->all_jnodes list.
 */
static void jnode_done(jnode * node, reiser4_tree * tree)
{
	reiser4_super_info_data *sbinfo;

	sbinfo = get_super_private(tree->super);

	spin_lock_irq(&sbinfo->all_guard);
	assert("nikita-2422", !list_empty(&node->jnodes));
	list_del_init(&node->jnodes);
	spin_unlock_irq(&sbinfo->all_guard);
}
#endif

/* return already existing jnode of page */
jnode *jnode_by_page(struct page *pg)
{
	assert("nikita-2066", pg != NULL);
	assert("nikita-2400", PageLocked(pg));
	assert("nikita-2068", PagePrivate(pg));
	assert("nikita-2067", jprivate(pg) != NULL);
	return jprivate(pg);
}

/* exported functions to allocate/free jnode objects outside this file */
jnode *jalloc(void)
{
	jnode *jal = kmem_cache_alloc(_jnode_slab, reiser4_ctx_gfp_mask_get());
	return jal;
}

/* return jnode back to the slab allocator */
inline void jfree(jnode * node)
{
	assert("zam-449", node != NULL);

	assert("nikita-2663", (list_empty_careful(&node->capture_link) &&
			       NODE_LIST(node) == NOT_CAPTURED));
	assert("nikita-3222", list_empty(&node->jnodes));
	assert("nikita-3221", jnode_page(node) == NULL);

	/* not yet phash_jnode_destroy(node); */

	kmem_cache_free(_jnode_slab, node);
}

/*
 * This function is supplied as RCU callback. It actually frees jnode when
 * last reference to it is gone.
 */
static void jnode_free_actor(struct rcu_head *head)
{
	jnode *node;
	jnode_type jtype;

	node = container_of(head, jnode, rcu);
	jtype = jnode_get_type(node);

	ON_DEBUG(jnode_done(node, jnode_get_tree(node)));

	switch (jtype) {
	case JNODE_IO_HEAD:
	case JNODE_BITMAP:
	case JNODE_UNFORMATTED_BLOCK:
		jfree(node);
		break;
	case JNODE_FORMATTED_BLOCK:
		zfree(JZNODE(node));
		break;
	case JNODE_INODE:
	default:
		wrong_return_value("nikita-3197", "Wrong jnode type");
	}
}

/*
 * Free a jnode. Post a callback to be executed later through RCU when all
 * references to @node are released.
 */
static inline void jnode_free(jnode * node, jnode_type jtype)
{
	if (jtype != JNODE_INODE) {
		/*assert("nikita-3219", list_empty(&node->rcu.list)); */
		call_rcu(&node->rcu, jnode_free_actor);
	} else
		jnode_list_remove(node);
}

/* allocate new unformatted jnode */
static jnode *jnew_unformatted(void)
{
	jnode *jal;

	jal = jalloc();
	if (jal == NULL)
		return NULL;

	jnode_init(jal, current_tree, JNODE_UNFORMATTED_BLOCK);
	jal->key.j.mapping = NULL;
	jal->key.j.index = (unsigned long)-1;
	jal->key.j.objectid = 0;
	return jal;
}

/* look for jnode with given mapping and offset within hash table */
jnode *jlookup(reiser4_tree * tree, oid_t objectid, unsigned long index)
{
	struct jnode_key jkey;
	jnode *node;

	assert("nikita-2353", tree != NULL);

	jkey.objectid = objectid;
	jkey.index = index;

	/*
	 * hash table is _not_ protected by any lock during lookups. All we
	 * have to do is to disable preemption to keep RCU happy.
	 */

	rcu_read_lock();
	node = j_hash_find(&tree->jhash_table, &jkey);
	if (node != NULL) {
		/* protect @node from recycling */
		jref(node);
		assert("nikita-2955", jnode_invariant(node, 0, 0));
		node = jnode_rip_check(tree, node);
	}
	rcu_read_unlock();
	return node;
}

/* per inode radix tree of jnodes is protected by tree's read write spin lock */
static jnode *jfind_nolock(struct address_space *mapping, unsigned long index)
{
	assert("vs-1694", mapping->host != NULL);

	return radix_tree_lookup(jnode_tree_by_inode(mapping->host), index);
}

jnode *jfind(struct address_space *mapping, unsigned long index)
{
	reiser4_tree *tree;
	jnode *node;

	assert("vs-1694", mapping->host != NULL);
	tree = reiser4_tree_by_inode(mapping->host);

	read_lock_tree(tree);
	node = jfind_nolock(mapping, index);
	if (node != NULL)
		jref(node);
	read_unlock_tree(tree);
	return node;
}

static void inode_attach_jnode(jnode * node)
{
	struct inode *inode;
	reiser4_inode *info;
	struct radix_tree_root *rtree;

	assert_rw_write_locked(&(jnode_get_tree(node)->tree_lock));
	assert("zam-1043", node->key.j.mapping != NULL);
	inode = node->key.j.mapping->host;
	info = reiser4_inode_data(inode);
	rtree = jnode_tree_by_reiser4_inode(info);
	if (rtree->rnode == NULL) {
		/* prevent inode from being pruned when it has jnodes attached
		   to it */
		spin_lock_irq(&inode->i_data.tree_lock);
		inode->i_data.nrpages++;
		spin_unlock_irq(&inode->i_data.tree_lock);
	}
	assert("zam-1049", equi(rtree->rnode != NULL, info->nr_jnodes != 0));
	check_me("zam-1045",
		 !radix_tree_insert(rtree, node->key.j.index, node));
	ON_DEBUG(info->nr_jnodes++);
}

static void inode_detach_jnode(jnode * node)
{
	struct inode *inode;
	reiser4_inode *info;
	struct radix_tree_root *rtree;

	assert_rw_write_locked(&(jnode_get_tree(node)->tree_lock));
	assert("zam-1044", node->key.j.mapping != NULL);
	inode = node->key.j.mapping->host;
	info = reiser4_inode_data(inode);
	rtree = jnode_tree_by_reiser4_inode(info);

	assert("zam-1051", info->nr_jnodes != 0);
	assert("zam-1052", rtree->rnode != NULL);
	ON_DEBUG(info->nr_jnodes--);

	/* delete jnode from inode's radix tree of jnodes */
	check_me("zam-1046", radix_tree_delete(rtree, node->key.j.index));
	if (rtree->rnode == NULL) {
		/* inode can be pruned now */
		spin_lock_irq(&inode->i_data.tree_lock);
		inode->i_data.nrpages--;
		spin_unlock_irq(&inode->i_data.tree_lock);
	}
}

/* put jnode into hash table (where they can be found by flush who does not know
   mapping) and to inode's tree of jnodes (where they can be found (hopefully
   faster) in places where mapping is known). Currently it is used by
   fs/reiser4/plugin/item/extent_file_ops.c:index_extent_jnode when new jnode is
   created */
static void
hash_unformatted_jnode(jnode * node, struct address_space *mapping,
		       unsigned long index)
{
	j_hash_table *jtable;

	assert("vs-1446", jnode_is_unformatted(node));
	assert("vs-1442", node->key.j.mapping == 0);
	assert("vs-1443", node->key.j.objectid == 0);
	assert("vs-1444", node->key.j.index == (unsigned long)-1);
	assert_rw_write_locked(&(jnode_get_tree(node)->tree_lock));

	node->key.j.mapping = mapping;
	node->key.j.objectid = get_inode_oid(mapping->host);
	node->key.j.index = index;

	jtable = &jnode_get_tree(node)->jhash_table;

	/* race with some other thread inserting jnode into the hash table is
	 * impossible, because we keep the page lock. */
	/*
	 * following assertion no longer holds because of RCU: it is possible
	 * jnode is in the hash table, but with JNODE_RIP bit set.
	 */
	/* assert("nikita-3211", j_hash_find(jtable, &node->key.j) == NULL); */
	j_hash_insert_rcu(jtable, node);
	inode_attach_jnode(node);
}

static void unhash_unformatted_node_nolock(jnode * node)
{
	assert("vs-1683", node->key.j.mapping != NULL);
	assert("vs-1684",
	       node->key.j.objectid ==
	       get_inode_oid(node->key.j.mapping->host));

	/* remove jnode from hash-table */
	j_hash_remove_rcu(&node->tree->jhash_table, node);
	inode_detach_jnode(node);
	node->key.j.mapping = NULL;
	node->key.j.index = (unsigned long)-1;
	node->key.j.objectid = 0;

}

/* remove jnode from hash table and from inode's tree of jnodes. This is used in
   reiser4_invalidatepage and in kill_hook_extent -> truncate_inode_jnodes ->
   reiser4_uncapture_jnode */
void unhash_unformatted_jnode(jnode * node)
{
	assert("vs-1445", jnode_is_unformatted(node));

	write_lock_tree(node->tree);
	unhash_unformatted_node_nolock(node);
	write_unlock_tree(node->tree);
}

/*
 * search hash table for a jnode with given oid and index. If not found,
 * allocate new jnode, insert it, and also insert into radix tree for the
 * given inode/mapping.
 */
static jnode *find_get_jnode(reiser4_tree * tree,
			     struct address_space *mapping,
			     oid_t oid, unsigned long index)
{
	jnode *result;
	jnode *shadow;
	int preload;

	result = jnew_unformatted();

	if (unlikely(result == NULL))
		return ERR_PTR(RETERR(-ENOMEM));

	preload = radix_tree_preload(reiser4_ctx_gfp_mask_get());
	if (preload != 0)
		return ERR_PTR(preload);

	write_lock_tree(tree);
	shadow = jfind_nolock(mapping, index);
	if (likely(shadow == NULL)) {
		/* add new jnode to hash table and inode's radix tree of
		 * jnodes */
		jref(result);
		hash_unformatted_jnode(result, mapping, index);
	} else {
		/* jnode is found in inode's radix tree of jnodes */
		jref(shadow);
		jnode_free(result, JNODE_UNFORMATTED_BLOCK);
		assert("vs-1498", shadow->key.j.mapping == mapping);
		result = shadow;
	}
	write_unlock_tree(tree);

	assert("nikita-2955",
	       ergo(result != NULL, jnode_invariant(result, 0, 0)));
	radix_tree_preload_end();
	return result;
}

/* jget() (a la zget() but for unformatted nodes). Returns (and possibly
   creates) jnode corresponding to page @pg. jnode is attached to page and
   inserted into jnode hash-table. */
static jnode *do_jget(reiser4_tree * tree, struct page *pg)
{
	/*
	 * There are two ways to create jnode: starting with pre-existing page
	 * and without page.
	 *
	 * When page already exists, jnode is created
	 * (jnode_of_page()->do_jget()) under page lock. This is done in
	 * ->writepage(), or when capturing anonymous page dirtied through
	 * mmap.
	 *
	 * Jnode without page is created by index_extent_jnode().
	 *
	 */

	jnode *result;
	oid_t oid = get_inode_oid(pg->mapping->host);

	assert("umka-176", pg != NULL);
	assert("nikita-2394", PageLocked(pg));

	result = jprivate(pg);
	if (likely(result != NULL))
		return jref(result);

	tree = reiser4_tree_by_page(pg);

	/* check hash-table first */
	result = jfind(pg->mapping, pg->index);
	if (unlikely(result != NULL)) {
		spin_lock_jnode(result);
		jnode_attach_page(result, pg);
		spin_unlock_jnode(result);
		result->key.j.mapping = pg->mapping;
		return result;
	}

	/* since page is locked, jnode should be allocated with GFP_NOFS flag */
	reiser4_ctx_gfp_mask_force(GFP_NOFS);
	result = find_get_jnode(tree, pg->mapping, oid, pg->index);
	if (unlikely(IS_ERR(result)))
		return result;
	/* attach jnode to page */
	spin_lock_jnode(result);
	jnode_attach_page(result, pg);
	spin_unlock_jnode(result);
	return result;
}

/*
 * return jnode for @pg, creating it if necessary.
 */
jnode *jnode_of_page(struct page *pg)
{
	jnode *result;

	assert("umka-176", pg != NULL);
	assert("nikita-2394", PageLocked(pg));

	result = do_jget(reiser4_tree_by_page(pg), pg);

	if (REISER4_DEBUG && !IS_ERR(result)) {
		assert("nikita-3210", result == jprivate(pg));
		assert("nikita-2046", jnode_page(jprivate(pg)) == pg);
		if (jnode_is_unformatted(jprivate(pg))) {
			assert("nikita-2364",
			       jprivate(pg)->key.j.index == pg->index);
			assert("nikita-2367",
			       jprivate(pg)->key.j.mapping == pg->mapping);
			assert("nikita-2365",
			       jprivate(pg)->key.j.objectid ==
			       get_inode_oid(pg->mapping->host));
			assert("vs-1200",
			       jprivate(pg)->key.j.objectid ==
			       pg->mapping->host->i_ino);
			assert("nikita-2356",
			       jnode_is_unformatted(jnode_by_page(pg)));
		}
		assert("nikita-2956", jnode_invariant(jprivate(pg), 0, 0));
	}
	return result;
}

/* attach page to jnode: set ->pg pointer in jnode, and ->private one in the
 * page.*/
void jnode_attach_page(jnode * node, struct page *pg)
{
	assert("nikita-2060", node != NULL);
	assert("nikita-2061", pg != NULL);

	assert("nikita-2050", jprivate(pg) == 0ul);
	assert("nikita-2393", !PagePrivate(pg));
	assert("vs-1741", node->pg == NULL);

	assert("nikita-2396", PageLocked(pg));
	assert_spin_locked(&(node->guard));

	page_cache_get(pg);
	set_page_private(pg, (unsigned long)node);
	node->pg = pg;
	SetPagePrivate(pg);
}

/* Dual to jnode_attach_page: break a binding between page and jnode */
void page_clear_jnode(struct page *page, jnode * node)
{
	assert("nikita-2424", page != NULL);
	assert("nikita-2425", PageLocked(page));
	assert("nikita-2426", node != NULL);
	assert_spin_locked(&(node->guard));
	assert("nikita-2428", PagePrivate(page));

	assert("nikita-3551", !PageWriteback(page));

	JF_CLR(node, JNODE_PARSED);
	set_page_private(page, 0ul);
	ClearPagePrivate(page);
	node->pg = NULL;
	page_cache_release(page);
}

#if 0
/* it is only used in one place to handle error */
void
page_detach_jnode(struct page *page, struct address_space *mapping,
		  unsigned long index)
{
	assert("nikita-2395", page != NULL);

	lock_page(page);
	if ((page->mapping == mapping) && (page->index == index)
	    && PagePrivate(page)) {
		jnode *node;

		node = jprivate(page);
		spin_lock_jnode(node);
		page_clear_jnode(page, node);
		spin_unlock_jnode(node);
	}
	unlock_page(page);
}
#endif  /*  0  */

/* return @node page locked.

   Locking ordering requires that one first takes page lock and afterwards
   spin lock on node attached to this page. Sometimes it is necessary to go in
   the opposite direction. This is done through standard trylock-and-release
   loop.
*/
static struct page *jnode_lock_page(jnode * node)
{
	struct page *page;

	assert("nikita-2052", node != NULL);
	assert("nikita-2401", LOCK_CNT_NIL(spin_locked_jnode));

	while (1) {

		spin_lock_jnode(node);
		page = jnode_page(node);
		if (page == NULL)
			break;

		/* no need to page_cache_get( page ) here, because page cannot
		   be evicted from memory without detaching it from jnode and
		   this requires spin lock on jnode that we already hold.
		 */
		if (trylock_page(page)) {
			/* We won a lock on jnode page, proceed. */
			break;
		}

		/* Page is locked by someone else. */
		page_cache_get(page);
		spin_unlock_jnode(node);
		wait_on_page_locked(page);
		/* it is possible that page was detached from jnode and
		   returned to the free pool, or re-assigned while we were
		   waiting on locked bit. This will be rechecked on the next
		   loop iteration.
		 */
		page_cache_release(page);

		/* try again */
	}
	return page;
}

/*
 * is JNODE_PARSED bit is not set, call ->parse() method of jnode, to verify
 * validness of jnode content.
 */
static inline int jparse(jnode * node)
{
	int result;

	assert("nikita-2466", node != NULL);

	spin_lock_jnode(node);
	if (likely(!jnode_is_parsed(node))) {
		result = jnode_ops(node)->parse(node);
		if (likely(result == 0))
			JF_SET(node, JNODE_PARSED);
	} else
		result = 0;
	spin_unlock_jnode(node);
	return result;
}

/* Lock a page attached to jnode, create and attach page to jnode if it had no
 * one. */
static struct page *jnode_get_page_locked(jnode * node, gfp_t gfp_flags)
{
	struct page *page;

	spin_lock_jnode(node);
	page = jnode_page(node);

	if (page == NULL) {
		spin_unlock_jnode(node);
		page = find_or_create_page(jnode_get_mapping(node),
					   jnode_get_index(node), gfp_flags);
		if (page == NULL)
			return ERR_PTR(RETERR(-ENOMEM));
	} else {
		if (trylock_page(page)) {
			spin_unlock_jnode(node);
			return page;
		}
		page_cache_get(page);
		spin_unlock_jnode(node);
		lock_page(page);
		assert("nikita-3134", page->mapping == jnode_get_mapping(node));
	}

	spin_lock_jnode(node);
	if (!jnode_page(node))
		jnode_attach_page(node, page);
	spin_unlock_jnode(node);

	page_cache_release(page);
	assert("zam-894", jnode_page(node) == page);
	return page;
}

/* Start read operation for jnode's page if page is not up-to-date. */
static int jnode_start_read(jnode * node, struct page *page)
{
	assert("zam-893", PageLocked(page));

	if (PageUptodate(page)) {
		unlock_page(page);
		return 0;
	}
	return reiser4_page_io(page, node, READ, reiser4_ctx_gfp_mask_get());
}

#if REISER4_DEBUG
static void check_jload(jnode * node, struct page *page)
{
	if (jnode_is_znode(node)) {
		node40_header *nh;
		znode *z;

		z = JZNODE(node);
		if (znode_is_any_locked(z)) {
			nh = (node40_header *) kmap(page);
			/* this only works for node40-only file systems. For
			 * debugging. */
			assert("nikita-3253",
			       z->nr_items == le16_to_cpu(get_unaligned(&nh->nr_items)));
			kunmap(page);
		}
		assert("nikita-3565", znode_invariant(z));
	}
}
#else
#define check_jload(node, page) noop
#endif

/* prefetch jnode to speed up next call to jload. Call this when you are going
 * to call jload() shortly. This will bring appropriate portion of jnode into
 * CPU cache. */
void jload_prefetch(jnode * node)
{
	prefetchw(&node->x_count);
}

/* load jnode's data into memory */
int jload_gfp(jnode * node /* node to load */ ,
	      gfp_t gfp_flags /* allocation flags */ ,
	      int do_kmap/* true if page should be kmapped */)
{
	struct page *page;
	int result = 0;
	int parsed;

	assert("nikita-3010", reiser4_schedulable());

	prefetchw(&node->pg);

	/* taking d-reference implies taking x-reference. */
	jref(node);

	/*
	 * acquiring d-reference to @jnode and check for JNODE_PARSED bit
	 * should be atomic, otherwise there is a race against
	 * reiser4_releasepage().
	 */
	spin_lock(&(node->load));
	add_d_ref(node);
	parsed = jnode_is_parsed(node);
	spin_unlock(&(node->load));

	if (unlikely(!parsed)) {
		page = jnode_get_page_locked(node, gfp_flags);
		if (unlikely(IS_ERR(page))) {
			result = PTR_ERR(page);
			goto failed;
		}

		result = jnode_start_read(node, page);
		if (unlikely(result != 0))
			goto failed;

		wait_on_page_locked(page);
		if (unlikely(!PageUptodate(page))) {
			result = RETERR(-EIO);
			goto failed;
		}

		if (do_kmap)
			node->data = kmap(page);

		result = jparse(node);
		if (unlikely(result != 0)) {
			if (do_kmap)
				kunmap(page);
			goto failed;
		}
		check_jload(node, page);
	} else {
		page = jnode_page(node);
		check_jload(node, page);
		if (do_kmap)
			node->data = kmap(page);
	}

	if (!is_writeout_mode())
		/* We do not mark pages active if jload is called as a part of
		 * jnode_flush() or reiser4_write_logs().  Both jnode_flush()
		 * and write_logs() add no value to cached data, there is no
		 * sense to mark pages as active when they go to disk, it just
		 * confuses vm scanning routines because clean page could be
		 * moved out from inactive list as a result of this
		 * mark_page_accessed() call. */
		mark_page_accessed(page);

	return 0;

failed:
	jrelse_tail(node);
	return result;

}

/* start asynchronous reading for given jnode's page. */
int jstartio(jnode * node)
{
	struct page *page;

	page = jnode_get_page_locked(node, reiser4_ctx_gfp_mask_get());
	if (IS_ERR(page))
		return PTR_ERR(page);

	return jnode_start_read(node, page);
}

/* Initialize a node by calling appropriate plugin instead of reading
 * node from disk as in jload(). */
int jinit_new(jnode * node, gfp_t gfp_flags)
{
	struct page *page;
	int result;

	jref(node);
	add_d_ref(node);

	page = jnode_get_page_locked(node, gfp_flags);
	if (IS_ERR(page)) {
		result = PTR_ERR(page);
		goto failed;
	}

	SetPageUptodate(page);
	unlock_page(page);

	node->data = kmap(page);

	if (!jnode_is_parsed(node)) {
		jnode_plugin *jplug = jnode_ops(node);
		spin_lock_jnode(node);
		result = jplug->init(node);
		spin_unlock_jnode(node);
		if (result) {
			kunmap(page);
			goto failed;
		}
		JF_SET(node, JNODE_PARSED);
	}

	return 0;

failed:
	jrelse(node);
	return result;
}

/* release a reference to jnode acquired by jload(), decrement ->d_count */
void jrelse_tail(jnode * node/* jnode to release references to */)
{
	assert("nikita-489", atomic_read(&node->d_count) > 0);
	atomic_dec(&node->d_count);
	/* release reference acquired in jload_gfp() or jinit_new() */
	jput(node);
	if (jnode_is_unformatted(node) || jnode_is_znode(node))
		LOCK_CNT_DEC(d_refs);
}

/* drop reference to node data. When last reference is dropped, data are
   unloaded. */
void jrelse(jnode * node/* jnode to release references to */)
{
	struct page *page;

	assert("nikita-487", node != NULL);
	assert_spin_not_locked(&(node->guard));

	page = jnode_page(node);
	if (likely(page != NULL)) {
		/*
		 * it is safe not to lock jnode here, because at this point
		 * @node->d_count is greater than zero (if jrelse() is used
		 * correctly, that is). JNODE_PARSED may be not set yet, if,
		 * for example, we got here as a result of error handling path
		 * in jload(). Anyway, page cannot be detached by
		 * reiser4_releasepage(). truncate will invalidate page
		 * regardless, but this should not be a problem.
		 */
		kunmap(page);
	}
	jrelse_tail(node);
}

/* called from jput() to wait for io completion */
static void jnode_finish_io(jnode * node)
{
	struct page *page;

	assert("nikita-2922", node != NULL);

	spin_lock_jnode(node);
	page = jnode_page(node);
	if (page != NULL) {
		page_cache_get(page);
		spin_unlock_jnode(node);
		wait_on_page_writeback(page);
		page_cache_release(page);
	} else
		spin_unlock_jnode(node);
}

/*
 * This is called by jput() when last reference to jnode is released. This is
 * separate function, because we want fast path of jput() to be inline and,
 * therefore, small.
 */
void jput_final(jnode * node)
{
	int r_i_p;

	/* A fast check for keeping node in cache. We always keep node in cache
	 * if its page is present and node was not marked for deletion */
	if (jnode_page(node) != NULL && !JF_ISSET(node, JNODE_HEARD_BANSHEE)) {
		rcu_read_unlock();
		return;
	}
	r_i_p = !JF_TEST_AND_SET(node, JNODE_RIP);
	/*
	 * if r_i_p is true, we were first to set JNODE_RIP on this node. In
	 * this case it is safe to access node after unlock.
	 */
	rcu_read_unlock();
	if (r_i_p) {
		jnode_finish_io(node);
		if (JF_ISSET(node, JNODE_HEARD_BANSHEE))
			/* node is removed from the tree. */
			jdelete(node);
		else
			jnode_try_drop(node);
	}
	/* if !r_i_p some other thread is already killing it */
}

int jwait_io(jnode * node, int rw)
{
	struct page *page;
	int result;

	assert("zam-447", node != NULL);
	assert("zam-448", jnode_page(node) != NULL);

	page = jnode_page(node);

	result = 0;
	if (rw == READ) {
		wait_on_page_locked(page);
	} else {
		assert("nikita-2227", rw == WRITE);
		wait_on_page_writeback(page);
	}
	if (PageError(page))
		result = RETERR(-EIO);

	return result;
}

/*
 * jnode types and plugins.
 *
 * jnode by itself is a "base type". There are several different jnode
 * flavors, called "jnode types" (see jnode_type for a list). Sometimes code
 * has to do different things based on jnode type. In the standard reiser4 way
 * this is done by having jnode plugin (see fs/reiser4/plugin.h:jnode_plugin).
 *
 * Functions below deal with jnode types and define methods of jnode plugin.
 *
 */

/* set jnode type. This is done during jnode initialization. */
static void jnode_set_type(jnode * node, jnode_type type)
{
	static unsigned long type_to_mask[] = {
		[JNODE_UNFORMATTED_BLOCK] = 1,
		[JNODE_FORMATTED_BLOCK] = 0,
		[JNODE_BITMAP] = 2,
		[JNODE_IO_HEAD] = 6,
		[JNODE_INODE] = 4
	};

	assert("zam-647", type < LAST_JNODE_TYPE);
	assert("nikita-2815", !jnode_is_loaded(node));
	assert("nikita-3386", node->state == 0);

	node->state |= (type_to_mask[type] << JNODE_TYPE_1);
}

/* ->init() method of jnode plugin for jnodes that don't require plugin
 * specific initialization. */
static int init_noinit(jnode * node UNUSED_ARG)
{
	return 0;
}

/* ->parse() method of jnode plugin for jnodes that don't require plugin
 * specific pasring. */
static int parse_noparse(jnode * node UNUSED_ARG)
{
	return 0;
}

/* ->mapping() method for unformatted jnode */
struct address_space *mapping_jnode(const jnode * node)
{
	struct address_space *map;

	assert("nikita-2713", node != NULL);

	/* mapping is stored in jnode */

	map = node->key.j.mapping;
	assert("nikita-2714", map != NULL);
	assert("nikita-2897", is_reiser4_inode(map->host));
	assert("nikita-2715", get_inode_oid(map->host) == node->key.j.objectid);
	return map;
}

/* ->index() method for unformatted jnodes */
unsigned long index_jnode(const jnode * node)
{
	/* index is stored in jnode */
	return node->key.j.index;
}

/* ->remove() method for unformatted jnodes */
static inline void remove_jnode(jnode * node, reiser4_tree * tree)
{
	/* remove jnode from hash table and radix tree */
	if (node->key.j.mapping)
		unhash_unformatted_node_nolock(node);
}

/* ->mapping() method for znodes */
static struct address_space *mapping_znode(const jnode * node)
{
	/* all znodes belong to fake inode */
	return reiser4_get_super_fake(jnode_get_tree(node)->super)->i_mapping;
}

/* ->index() method for znodes */
static unsigned long index_znode(const jnode * node)
{
	unsigned long addr;
	assert("nikita-3317", (1 << znode_shift_order) < sizeof(znode));

	/* index of znode is just its address (shifted) */
	addr = (unsigned long)node;
	return (addr - PAGE_OFFSET) >> znode_shift_order;
}

/* ->mapping() method for bitmap jnode */
static struct address_space *mapping_bitmap(const jnode * node)
{
	/* all bitmap blocks belong to special bitmap inode */
	return get_super_private(jnode_get_tree(node)->super)->bitmap->
	    i_mapping;
}

/* ->index() method for jnodes that are indexed by address */
static unsigned long index_is_address(const jnode * node)
{
	unsigned long ind;

	ind = (unsigned long)node;
	return ind - PAGE_OFFSET;
}

/* resolve race with jput */
jnode *jnode_rip_sync(reiser4_tree *tree, jnode *node)
{
	/*
	 * This is used as part of RCU-based jnode handling.
	 *
	 * jlookup(), zlook(), zget(), and cbk_cache_scan_slots() have to work
	 * with unreferenced jnodes (ones with ->x_count == 0). Hash table is
	 * not protected during this, so concurrent thread may execute
	 * zget-set-HEARD_BANSHEE-zput, or somehow else cause jnode to be
	 * freed in jput_final(). To avoid such races, jput_final() sets
	 * JNODE_RIP on jnode (under tree lock). All places that work with
	 * unreferenced jnodes call this function. It checks for JNODE_RIP bit
	 * (first without taking tree lock), and if this bit is set, released
	 * reference acquired by the current thread and returns NULL.
	 *
	 * As a result, if jnode is being concurrently freed, NULL is returned
	 * and caller should pretend that jnode wasn't found in the first
	 * place.
	 *
	 * Otherwise it's safe to release "rcu-read-lock" and continue with
	 * jnode.
	 */
	if (unlikely(JF_ISSET(node, JNODE_RIP))) {
		read_lock_tree(tree);
		if (JF_ISSET(node, JNODE_RIP)) {
			dec_x_ref(node);
			node = NULL;
		}
		read_unlock_tree(tree);
	}
	return node;
}

reiser4_key *jnode_build_key(const jnode * node, reiser4_key * key)
{
	struct inode *inode;
	item_plugin *iplug;
	loff_t off;

	assert("nikita-3092", node != NULL);
	assert("nikita-3093", key != NULL);
	assert("nikita-3094", jnode_is_unformatted(node));

	off = ((loff_t) index_jnode(node)) << PAGE_CACHE_SHIFT;
	inode = mapping_jnode(node)->host;

	if (node->parent_item_id != 0)
		iplug = item_plugin_by_id(node->parent_item_id);
	else
		iplug = NULL;

	if (iplug != NULL && iplug->f.key_by_offset)
		iplug->f.key_by_offset(inode, off, key);
	else {
		file_plugin *fplug;

		fplug = inode_file_plugin(inode);
		assert("zam-1007", fplug != NULL);
		assert("zam-1008", fplug->key_by_inode != NULL);

		fplug->key_by_inode(inode, off, key);
	}

	return key;
}

/* ->parse() method for formatted nodes */
static int parse_znode(jnode * node)
{
	return zparse(JZNODE(node));
}

/* ->delete() method for formatted nodes */
static void delete_znode(jnode * node, reiser4_tree * tree)
{
	znode *z;

	assert_rw_write_locked(&(tree->tree_lock));
	assert("vs-898", JF_ISSET(node, JNODE_HEARD_BANSHEE));

	z = JZNODE(node);
	assert("vs-899", z->c_count == 0);

	/* delete znode from sibling list. */
	sibling_list_remove(z);

	znode_remove(z, tree);
}

/* ->remove() method for formatted nodes */
static int remove_znode(jnode * node, reiser4_tree * tree)
{
	znode *z;

	assert_rw_write_locked(&(tree->tree_lock));
	z = JZNODE(node);

	if (z->c_count == 0) {
		/* detach znode from sibling list. */
		sibling_list_drop(z);
		/* this is called with tree spin-lock held, so call
		   znode_remove() directly (rather than znode_lock_remove()). */
		znode_remove(z, tree);
		return 0;
	}
	return RETERR(-EBUSY);
}

/* ->init() method for formatted nodes */
static int init_znode(jnode * node)
{
	znode *z;

	z = JZNODE(node);
	/* call node plugin to do actual initialization */
	return z->nplug->init(z);
}

/* ->clone() method for formatted nodes */
static jnode *clone_formatted(jnode * node)
{
	znode *clone;

	assert("vs-1430", jnode_is_znode(node));
	clone = zalloc(reiser4_ctx_gfp_mask_get());
	if (clone == NULL)
		return ERR_PTR(RETERR(-ENOMEM));
	zinit(clone, NULL, current_tree);
	jnode_set_block(ZJNODE(clone), jnode_get_block(node));
	/* ZJNODE(clone)->key.z is not initialized */
	clone->level = JZNODE(node)->level;

	return ZJNODE(clone);
}

/* jplug->clone for unformatted nodes */
static jnode *clone_unformatted(jnode * node)
{
	jnode *clone;

	assert("vs-1431", jnode_is_unformatted(node));
	clone = jalloc();
	if (clone == NULL)
		return ERR_PTR(RETERR(-ENOMEM));

	jnode_init(clone, current_tree, JNODE_UNFORMATTED_BLOCK);
	jnode_set_block(clone, jnode_get_block(node));

	return clone;

}

/*
 * Setup jnode plugin methods for various jnode types.
 */
jnode_plugin jnode_plugins[LAST_JNODE_TYPE] = {
	[JNODE_UNFORMATTED_BLOCK] = {
		.h = {
			.type_id = REISER4_JNODE_PLUGIN_TYPE,
			.id = JNODE_UNFORMATTED_BLOCK,
			.pops = NULL,
			.label = "unformatted",
			.desc = "unformatted node",
			.linkage = {NULL, NULL}
		},
		.init = init_noinit,
		.parse = parse_noparse,
		.mapping = mapping_jnode,
		.index = index_jnode,
		.clone = clone_unformatted
	},
	[JNODE_FORMATTED_BLOCK] = {
		.h = {
			.type_id = REISER4_JNODE_PLUGIN_TYPE,
			.id = JNODE_FORMATTED_BLOCK,
			.pops = NULL,
			.label = "formatted",
			.desc = "formatted tree node",
			.linkage = {NULL, NULL}
		},
		.init = init_znode,
		.parse = parse_znode,
		.mapping = mapping_znode,
		.index = index_znode,
		.clone = clone_formatted
	},
	[JNODE_BITMAP] = {
		.h = {
			.type_id = REISER4_JNODE_PLUGIN_TYPE,
			.id = JNODE_BITMAP,
			.pops = NULL,
			.label = "bitmap",
			.desc = "bitmap node",
			.linkage = {NULL, NULL}
		},
		.init = init_noinit,
		.parse = parse_noparse,
		.mapping = mapping_bitmap,
		.index = index_is_address,
		.clone = NULL
	},
	[JNODE_IO_HEAD] = {
		.h = {
			.type_id = REISER4_JNODE_PLUGIN_TYPE,
			.id = JNODE_IO_HEAD,
			.pops = NULL,
			.label = "io head",
			.desc = "io head",
			.linkage = {NULL, NULL}
		},
		.init = init_noinit,
		.parse = parse_noparse,
		.mapping = mapping_bitmap,
		.index = index_is_address,
		.clone = NULL
	},
	[JNODE_INODE] = {
		.h = {
			.type_id = REISER4_JNODE_PLUGIN_TYPE,
			.id = JNODE_INODE,
			.pops = NULL,
			.label = "inode",
			.desc = "inode's builtin jnode",
			.linkage = {NULL, NULL}
		},
		.init = NULL,
		.parse = NULL,
		.mapping = NULL,
		.index = NULL,
		.clone = NULL
	}
};

/*
 * jnode destruction.
 *
 * Thread may use a jnode after it acquired a reference to it. References are
 * counted in ->x_count field. Reference protects jnode from being
 * recycled. This is different from protecting jnode data (that are stored in
 * jnode page) from being evicted from memory. Data are protected by jload()
 * and released by jrelse().
 *
 * If thread already possesses a reference to the jnode it can acquire another
 * one through jref(). Initial reference is obtained (usually) by locating
 * jnode in some indexing structure that depends on jnode type: formatted
 * nodes are kept in global hash table, where they are indexed by block
 * number, and also in the cbk cache. Unformatted jnodes are also kept in hash
 * table, which is indexed by oid and offset within file, and in per-inode
 * radix tree.
 *
 * Reference to jnode is released by jput(). If last reference is released,
 * jput_final() is called. This function determines whether jnode has to be
 * deleted (this happens when corresponding node is removed from the file
 * system, jnode is marked with JNODE_HEARD_BANSHEE bit in this case), or it
 * should be just "removed" (deleted from memory).
 *
 * Jnode destruction is signally delicate dance because of locking and RCU.
 */

/*
 * Returns true if jnode cannot be removed right now. This check is called
 * under tree lock. If it returns true, jnode is irrevocably committed to be
 * deleted/removed.
 */
static inline int jnode_is_busy(const jnode * node, jnode_type jtype)
{
	/* if other thread managed to acquire a reference to this jnode, don't
	 * free it. */
	if (atomic_read(&node->x_count) > 0)
		return 1;
	/* also, don't free znode that has children in memory */
	if (jtype == JNODE_FORMATTED_BLOCK && JZNODE(node)->c_count > 0)
		return 1;
	return 0;
}

/*
 * this is called as part of removing jnode. Based on jnode type, call
 * corresponding function that removes jnode from indices and returns it back
 * to the appropriate slab (through RCU).
 */
static inline void
jnode_remove(jnode * node, jnode_type jtype, reiser4_tree * tree)
{
	switch (jtype) {
	case JNODE_UNFORMATTED_BLOCK:
		remove_jnode(node, tree);
		break;
	case JNODE_IO_HEAD:
	case JNODE_BITMAP:
		break;
	case JNODE_INODE:
		break;
	case JNODE_FORMATTED_BLOCK:
		remove_znode(node, tree);
		break;
	default:
		wrong_return_value("nikita-3196", "Wrong jnode type");
	}
}

/*
 * this is called as part of deleting jnode. Based on jnode type, call
 * corresponding function that removes jnode from indices and returns it back
 * to the appropriate slab (through RCU).
 *
 * This differs from jnode_remove() only for formatted nodes---for them
 * sibling list handling is different for removal and deletion.
 */
static inline void
jnode_delete(jnode * node, jnode_type jtype, reiser4_tree * tree UNUSED_ARG)
{
	switch (jtype) {
	case JNODE_UNFORMATTED_BLOCK:
		remove_jnode(node, tree);
		break;
	case JNODE_IO_HEAD:
	case JNODE_BITMAP:
		break;
	case JNODE_FORMATTED_BLOCK:
		delete_znode(node, tree);
		break;
	case JNODE_INODE:
	default:
		wrong_return_value("nikita-3195", "Wrong jnode type");
	}
}

#if REISER4_DEBUG
/*
 * remove jnode from the debugging list of all jnodes hanging off super-block.
 */
void jnode_list_remove(jnode * node)
{
	reiser4_super_info_data *sbinfo;

	sbinfo = get_super_private(jnode_get_tree(node)->super);

	spin_lock_irq(&sbinfo->all_guard);
	assert("nikita-2422", !list_empty(&node->jnodes));
	list_del_init(&node->jnodes);
	spin_unlock_irq(&sbinfo->all_guard);
}
#endif

/*
 * this is called by jput_final() to remove jnode when last reference to it is
 * released.
 */
static int jnode_try_drop(jnode * node)
{
	int result;
	reiser4_tree *tree;
	jnode_type jtype;

	assert("nikita-2491", node != NULL);
	assert("nikita-2583", JF_ISSET(node, JNODE_RIP));

	tree = jnode_get_tree(node);
	jtype = jnode_get_type(node);

	spin_lock_jnode(node);
	write_lock_tree(tree);
	/*
	 * if jnode has a page---leave it alone. Memory pressure will
	 * eventually kill page and jnode.
	 */
	if (jnode_page(node) != NULL) {
		write_unlock_tree(tree);
		spin_unlock_jnode(node);
		JF_CLR(node, JNODE_RIP);
		return RETERR(-EBUSY);
	}

	/* re-check ->x_count under tree lock. */
	result = jnode_is_busy(node, jtype);
	if (result == 0) {
		assert("nikita-2582", !JF_ISSET(node, JNODE_HEARD_BANSHEE));
		assert("jmacd-511/b", atomic_read(&node->d_count) == 0);

		spin_unlock_jnode(node);
		/* no page and no references---despatch him. */
		jnode_remove(node, jtype, tree);
		write_unlock_tree(tree);
		jnode_free(node, jtype);
	} else {
		/* busy check failed: reference was acquired by concurrent
		 * thread. */
		write_unlock_tree(tree);
		spin_unlock_jnode(node);
		JF_CLR(node, JNODE_RIP);
	}
	return result;
}

/* jdelete() -- Delete jnode from the tree and file system */
static int jdelete(jnode * node/* jnode to finish with */)
{
	struct page *page;
	int result;
	reiser4_tree *tree;
	jnode_type jtype;

	assert("nikita-467", node != NULL);
	assert("nikita-2531", JF_ISSET(node, JNODE_RIP));

	jtype = jnode_get_type(node);

	page = jnode_lock_page(node);
	assert_spin_locked(&(node->guard));

	tree = jnode_get_tree(node);

	write_lock_tree(tree);
	/* re-check ->x_count under tree lock. */
	result = jnode_is_busy(node, jtype);
	if (likely(!result)) {
		assert("nikita-2123", JF_ISSET(node, JNODE_HEARD_BANSHEE));
		assert("jmacd-511", atomic_read(&node->d_count) == 0);

		/* detach page */
		if (page != NULL) {
			/*
			 * FIXME this is racy against jnode_extent_write().
			 */
			page_clear_jnode(page, node);
		}
		spin_unlock_jnode(node);
		/* goodbye */
		jnode_delete(node, jtype, tree);
		write_unlock_tree(tree);
		jnode_free(node, jtype);
		/* @node is no longer valid pointer */
		if (page != NULL)
			reiser4_drop_page(page);
	} else {
		/* busy check failed: reference was acquired by concurrent
		 * thread. */
		JF_CLR(node, JNODE_RIP);
		write_unlock_tree(tree);
		spin_unlock_jnode(node);
		if (page != NULL)
			unlock_page(page);
	}
	return result;
}

/* drop jnode on the floor.

   Return value:

    -EBUSY:  failed to drop jnode, because there are still references to it

    0:       successfully dropped jnode

*/
static int jdrop_in_tree(jnode * node, reiser4_tree * tree)
{
	struct page *page;
	jnode_type jtype;
	int result;

	assert("zam-602", node != NULL);
	assert_rw_not_read_locked(&(tree->tree_lock));
	assert_rw_not_write_locked(&(tree->tree_lock));
	assert("nikita-2403", !JF_ISSET(node, JNODE_HEARD_BANSHEE));

	jtype = jnode_get_type(node);

	page = jnode_lock_page(node);
	assert_spin_locked(&(node->guard));

	write_lock_tree(tree);

	/* re-check ->x_count under tree lock. */
	result = jnode_is_busy(node, jtype);
	if (!result) {
		assert("nikita-2488", page == jnode_page(node));
		assert("nikita-2533", atomic_read(&node->d_count) == 0);
		if (page != NULL) {
			assert("nikita-2126", !PageDirty(page));
			assert("nikita-2127", PageUptodate(page));
			assert("nikita-2181", PageLocked(page));
			page_clear_jnode(page, node);
		}
		spin_unlock_jnode(node);
		jnode_remove(node, jtype, tree);
		write_unlock_tree(tree);
		jnode_free(node, jtype);
		if (page != NULL)
			reiser4_drop_page(page);
	} else {
		/* busy check failed: reference was acquired by concurrent
		 * thread. */
		JF_CLR(node, JNODE_RIP);
		write_unlock_tree(tree);
		spin_unlock_jnode(node);
		if (page != NULL)
			unlock_page(page);
	}
	return result;
}

/* This function frees jnode "if possible". In particular, [dcx]_count has to
   be 0 (where applicable).  */
void jdrop(jnode * node)
{
	jdrop_in_tree(node, jnode_get_tree(node));
}

/* IO head jnode implementation; The io heads are simple j-nodes with limited
   functionality (these j-nodes are not in any hash table) just for reading
   from and writing to disk. */

jnode *reiser4_alloc_io_head(const reiser4_block_nr * block)
{
	jnode *jal = jalloc();

	if (jal != NULL) {
		jnode_init(jal, current_tree, JNODE_IO_HEAD);
		jnode_set_block(jal, block);
	}

	jref(jal);

	return jal;
}

void reiser4_drop_io_head(jnode * node)
{
	assert("zam-648", jnode_get_type(node) == JNODE_IO_HEAD);

	jput(node);
	jdrop(node);
}

/* protect keep jnode data from reiser4_releasepage()  */
void pin_jnode_data(jnode * node)
{
	assert("zam-671", jnode_page(node) != NULL);
	page_cache_get(jnode_page(node));
}

/* make jnode data free-able again */
void unpin_jnode_data(jnode * node)
{
	assert("zam-672", jnode_page(node) != NULL);
	page_cache_release(jnode_page(node));
}

struct address_space *jnode_get_mapping(const jnode * node)
{
	assert("nikita-3162", node != NULL);
	return jnode_ops(node)->mapping(node);
}

#if REISER4_DEBUG
/* debugging aid: jnode invariant */
int jnode_invariant_f(const jnode * node, char const **msg)
{
#define _ergo(ant, con) 						\
	((*msg) = "{" #ant "} ergo {" #con "}", ergo((ant), (con)))
#define _check(exp) ((*msg) = #exp, (exp))

	return _check(node != NULL) &&
	    /* [jnode-queued] */
	    /* only relocated node can be queued, except that when znode
	     * is being deleted, its JNODE_RELOC bit is cleared */
	    _ergo(JF_ISSET(node, JNODE_FLUSH_QUEUED),
		  JF_ISSET(node, JNODE_RELOC) ||
		  JF_ISSET(node, JNODE_HEARD_BANSHEE)) &&
	    _check(node->jnodes.prev != NULL) &&
	    _check(node->jnodes.next != NULL) &&
	    /* [jnode-dirty] invariant */
	    /* dirty inode is part of atom */
	    _ergo(JF_ISSET(node, JNODE_DIRTY), node->atom != NULL) &&
	    /* [jnode-oid] invariant */
	    /* for unformatted node ->objectid and ->mapping fields are
	     * consistent */
	    _ergo(jnode_is_unformatted(node) && node->key.j.mapping != NULL,
		  node->key.j.objectid ==
		  get_inode_oid(node->key.j.mapping->host)) &&
	    /* [jnode-atom-valid] invariant */
	    /* node atom has valid state */
	    _ergo(node->atom != NULL, node->atom->stage != ASTAGE_INVALID) &&
	    /* [jnode-page-binding] invariant */
	    /* if node points to page, it points back to node */
	    _ergo(node->pg != NULL, jprivate(node->pg) == node) &&
	    /* [jnode-refs] invariant */
	    /* only referenced jnode can be loaded */
	    _check(atomic_read(&node->x_count) >= atomic_read(&node->d_count));

}

static const char *jnode_type_name(jnode_type type)
{
	switch (type) {
	case JNODE_UNFORMATTED_BLOCK:
		return "unformatted";
	case JNODE_FORMATTED_BLOCK:
		return "formatted";
	case JNODE_BITMAP:
		return "bitmap";
	case JNODE_IO_HEAD:
		return "io head";
	case JNODE_INODE:
		return "inode";
	case LAST_JNODE_TYPE:
		return "last";
	default:{
			static char unknown[30];

			sprintf(unknown, "unknown %i", type);
			return unknown;
		}
	}
}

#define jnode_state_name(node, flag)			\
	(JF_ISSET((node), (flag)) ? ((#flag "|")+6) : "")

/* debugging aid: output human readable information about @node */
static void info_jnode(const char *prefix /* prefix to print */ ,
		       const jnode * node/* node to print */)
{
	assert("umka-068", prefix != NULL);

	if (node == NULL) {
		printk("%s: null\n", prefix);
		return;
	}

	printk
	    ("%s: %p: state: %lx: [%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s], level: %i,"
	     " block: %s, d_count: %d, x_count: %d, "
	     "pg: %p, atom: %p, lock: %i:%i, type: %s, ", prefix, node,
	     node->state,
	     jnode_state_name(node, JNODE_PARSED),
	     jnode_state_name(node, JNODE_HEARD_BANSHEE),
	     jnode_state_name(node, JNODE_LEFT_CONNECTED),
	     jnode_state_name(node, JNODE_RIGHT_CONNECTED),
	     jnode_state_name(node, JNODE_ORPHAN),
	     jnode_state_name(node, JNODE_CREATED),
	     jnode_state_name(node, JNODE_RELOC),
	     jnode_state_name(node, JNODE_OVRWR),
	     jnode_state_name(node, JNODE_DIRTY),
	     jnode_state_name(node, JNODE_IS_DYING),
	     jnode_state_name(node, JNODE_RIP),
	     jnode_state_name(node, JNODE_MISSED_IN_CAPTURE),
	     jnode_state_name(node, JNODE_WRITEBACK),
	     jnode_state_name(node, JNODE_NEW),
	     jnode_state_name(node, JNODE_DKSET),
	     jnode_state_name(node, JNODE_REPACK),
	     jnode_state_name(node, JNODE_CLUSTER_PAGE),
	     jnode_get_level(node), sprint_address(jnode_get_block(node)),
	     atomic_read(&node->d_count), atomic_read(&node->x_count),
	     jnode_page(node), node->atom, 0, 0,
	     jnode_type_name(jnode_get_type(node)));
	if (jnode_is_unformatted(node)) {
		printk("inode: %llu, index: %lu, ",
		       node->key.j.objectid, node->key.j.index);
	}
}

/* debugging aid: check znode invariant and panic if it doesn't hold */
static int jnode_invariant(jnode * node, int tlocked, int jlocked)
{
	char const *failed_msg;
	int result;
	reiser4_tree *tree;

	tree = jnode_get_tree(node);

	assert("umka-063312", node != NULL);
	assert("umka-064321", tree != NULL);

	if (!jlocked && !tlocked)
		spin_lock_jnode((jnode *) node);
	if (!tlocked)
		read_lock_tree(jnode_get_tree(node));
	result = jnode_invariant_f(node, &failed_msg);
	if (!result) {
		info_jnode("corrupted node", node);
		warning("jmacd-555", "Condition %s failed", failed_msg);
	}
	if (!tlocked)
		read_unlock_tree(jnode_get_tree(node));
	if (!jlocked && !tlocked)
		spin_unlock_jnode((jnode *) node);
	return result;
}

#endif				/* REISER4_DEBUG */

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 80
   End:
*/
