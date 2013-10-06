/* Copyright 2001, 2002, 2003, 2004 by Hans Reiser, licensing governed by
 * reiser4/README */

/* Declaration of znode (Zam's node). See znode.c for more details. */

#ifndef __ZNODE_H__
#define __ZNODE_H__

#include "forward.h"
#include "debug.h"
#include "dformat.h"
#include "key.h"
#include "coord.h"
#include "plugin/node/node.h"
#include "jnode.h"
#include "lock.h"
#include "readahead.h"

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/pagemap.h>	/* for PAGE_CACHE_SIZE */
#include <asm/atomic.h>

/* znode tracks its position within parent (internal item in a parent node,
 * that contains znode's block number). */
typedef struct parent_coord {
	znode *node;
	pos_in_node_t item_pos;
} parent_coord_t;

/* &znode - node in a reiser4 tree.

   NOTE-NIKITA fields in this struct have to be rearranged (later) to reduce
   cacheline pressure.

   Locking:

   Long term: data in a disk node attached to this znode are protected
   by long term, deadlock aware lock ->lock;

   Spin lock: the following fields are protected by the spin lock:

    ->lock

   Following fields are protected by the global tree lock:

    ->left
    ->right
    ->in_parent
    ->c_count

   Following fields are protected by the global delimiting key lock (dk_lock):

    ->ld_key (to update ->ld_key long-term lock on the node is also required)
    ->rd_key

   Following fields are protected by the long term lock:

    ->nr_items

   ->node_plugin is never changed once set. This means that after code made
   itself sure that field is valid it can be accessed without any additional
   locking.

   ->level is immutable.

   Invariants involving this data-type:

      [znode-fake]
      [znode-level]
      [znode-connected]
      [znode-c_count]
      [znode-refs]
      [jnode-refs]
      [jnode-queued]
      [znode-modify]

    For this to be made into a clustering or NUMA filesystem, we would want to eliminate all of the global locks.
    Suggestions for how to do that are desired.*/
struct znode {
	/* Embedded jnode. */
	jnode zjnode;

	/* contains three subfields, node, pos_in_node, and pos_in_unit.

	   pos_in_node and pos_in_unit are only hints that are cached to
	   speed up lookups during balancing. They are not required to be up to
	   date. Synched in find_child_ptr().

	   This value allows us to avoid expensive binary searches.

	   in_parent->node points to the parent of this node, and is NOT a
	   hint.
	 */
	parent_coord_t in_parent;

	/*
	 * sibling list pointers
	 */

	/* left-neighbor */
	znode *left;
	/* right-neighbor */
	znode *right;

	/* long term lock on node content. This lock supports deadlock
	   detection. See lock.c
	 */
	zlock lock;

	/* You cannot remove from memory a node that has children in
	   memory. This is because we rely on the fact that parent of given
	   node can always be reached without blocking for io. When reading a
	   node into memory you must increase the c_count of its parent, when
	   removing it from memory you must decrease the c_count.  This makes
	   the code simpler, and the cases where it is suboptimal are truly
	   obscure.
	 */
	int c_count;

	/* plugin of node attached to this znode. NULL if znode is not
	   loaded. */
	node_plugin *nplug;

	/* version of znode data. This is increased on each modification. This
	 * is necessary to implement seals (see seal.[ch]) efficiently. */
	__u64 version;

	/* left delimiting key. Necessary to efficiently perform
	   balancing with node-level locking. Kept in memory only. */
	reiser4_key ld_key;
	/* right delimiting key. */
	reiser4_key rd_key;

	/* znode's tree level */
	__u16 level;
	/* number of items in this node. This field is modified by node
	 * plugin. */
	__u16 nr_items;

#if REISER4_DEBUG
	void *creator;
	reiser4_key first_key;
	unsigned long times_locked;
	int left_version;	/* when node->left was updated */
	int right_version;	/* when node->right was updated */
	int ld_key_version;	/* when node->ld_key was updated */
	int rd_key_version;	/* when node->rd_key was updated */
#endif

} __attribute__ ((aligned(16)));

ON_DEBUG(extern atomic_t delim_key_version;
    )

/* In general I think these macros should not be exposed. */
#define znode_is_locked(node)          (lock_is_locked(&node->lock))
#define znode_is_rlocked(node)         (lock_is_rlocked(&node->lock))
#define znode_is_wlocked(node)         (lock_is_wlocked(&node->lock))
#define znode_is_wlocked_once(node)    (lock_is_wlocked_once(&node->lock))
#define znode_can_be_rlocked(node)     (lock_can_be_rlocked(&node->lock))
#define is_lock_compatible(node, mode) (lock_mode_compatible(&node->lock, mode))
/* Macros for accessing the znode state. */
#define	ZF_CLR(p,f)	        JF_CLR  (ZJNODE(p), (f))
#define	ZF_ISSET(p,f)	        JF_ISSET(ZJNODE(p), (f))
#define	ZF_SET(p,f)		JF_SET  (ZJNODE(p), (f))
extern znode *zget(reiser4_tree * tree, const reiser4_block_nr * const block,
		   znode * parent, tree_level level, gfp_t gfp_flag);
extern znode *zlook(reiser4_tree * tree, const reiser4_block_nr * const block);
extern int zload(znode * node);
extern int zload_ra(znode * node, ra_info_t * info);
extern int zinit_new(znode * node, gfp_t gfp_flags);
extern void zrelse(znode * node);
extern void znode_change_parent(znode * new_parent, reiser4_block_nr * block);

/* size of data in znode */
static inline unsigned
znode_size(const znode * node UNUSED_ARG /* znode to query */ )
{
	assert("nikita-1416", node != NULL);
	return PAGE_CACHE_SIZE;
}

extern void parent_coord_to_coord(const parent_coord_t * pcoord,
				  coord_t * coord);
extern void coord_to_parent_coord(const coord_t * coord,
				  parent_coord_t * pcoord);
extern void init_parent_coord(parent_coord_t * pcoord, const znode * node);

extern unsigned znode_free_space(znode * node);

extern reiser4_key *znode_get_rd_key(znode * node);
extern reiser4_key *znode_get_ld_key(znode * node);

extern reiser4_key *znode_set_rd_key(znode * node, const reiser4_key * key);
extern reiser4_key *znode_set_ld_key(znode * node, const reiser4_key * key);

/* `connected' state checks */
static inline int znode_is_right_connected(const znode * node)
{
	return ZF_ISSET(node, JNODE_RIGHT_CONNECTED);
}

static inline int znode_is_left_connected(const znode * node)
{
	return ZF_ISSET(node, JNODE_LEFT_CONNECTED);
}

static inline int znode_is_connected(const znode * node)
{
	return znode_is_right_connected(node) && znode_is_left_connected(node);
}

extern int znode_shift_order;
extern int znode_rehash(znode * node, const reiser4_block_nr * new_block_nr);
extern void znode_remove(znode *, reiser4_tree *);
extern znode *znode_parent(const znode * node);
extern znode *znode_parent_nolock(const znode * node);
extern int znode_above_root(const znode * node);
extern int init_znodes(void);
extern void done_znodes(void);
extern int znodes_tree_init(reiser4_tree * ztree);
extern void znodes_tree_done(reiser4_tree * ztree);
extern int znode_contains_key(znode * node, const reiser4_key * key);
extern int znode_contains_key_lock(znode * node, const reiser4_key * key);
extern unsigned znode_save_free_space(znode * node);
extern unsigned znode_recover_free_space(znode * node);
extern znode *zalloc(gfp_t gfp_flag);
extern void zinit(znode *, const znode * parent, reiser4_tree *);
extern int zparse(znode * node);

extern int znode_just_created(const znode * node);

extern void zfree(znode * node);

#if REISER4_DEBUG
extern void print_znode(const char *prefix, const znode * node);
#else
#define print_znode( p, n ) noop
#endif

/* Make it look like various znode functions exist instead of treating znodes as
   jnodes in znode-specific code. */
#define znode_page(x)               jnode_page ( ZJNODE(x) )
#define zdata(x)                    jdata ( ZJNODE(x) )
#define znode_get_block(x)          jnode_get_block ( ZJNODE(x) )
#define znode_created(x)            jnode_created ( ZJNODE(x) )
#define znode_set_created(x)        jnode_set_created ( ZJNODE(x) )
#define znode_convertible(x)        jnode_convertible (ZJNODE(x))
#define znode_set_convertible(x)    jnode_set_convertible (ZJNODE(x))

#define znode_is_dirty(x)           jnode_is_dirty    ( ZJNODE(x) )
#define znode_check_dirty(x)        jnode_check_dirty ( ZJNODE(x) )
#define znode_make_clean(x)         jnode_make_clean   ( ZJNODE(x) )
#define znode_set_block(x, b)       jnode_set_block ( ZJNODE(x), (b) )

#define spin_lock_znode(x)          spin_lock_jnode ( ZJNODE(x) )
#define spin_unlock_znode(x)        spin_unlock_jnode ( ZJNODE(x) )
#define spin_trylock_znode(x)       spin_trylock_jnode ( ZJNODE(x) )
#define spin_znode_is_locked(x)     spin_jnode_is_locked ( ZJNODE(x) )
#define spin_znode_is_not_locked(x) spin_jnode_is_not_locked ( ZJNODE(x) )

#if REISER4_DEBUG
extern int znode_x_count_is_protected(const znode * node);
extern int znode_invariant(znode * node);
#endif

/* acquire reference to @node */
static inline znode *zref(znode * node)
{
	/* change of x_count from 0 to 1 is protected by tree spin-lock */
	return JZNODE(jref(ZJNODE(node)));
}

/* release reference to @node */
static inline void zput(znode * node)
{
	assert("nikita-3564", znode_invariant(node));
	jput(ZJNODE(node));
}

/* get the level field for a znode */
static inline tree_level znode_get_level(const znode * node)
{
	return node->level;
}

/* get the level field for a jnode */
static inline tree_level jnode_get_level(const jnode * node)
{
	if (jnode_is_znode(node))
		return znode_get_level(JZNODE(node));
	else
		/* unformatted nodes are all at the LEAF_LEVEL and for
		   "semi-formatted" nodes like bitmaps, level doesn't matter. */
		return LEAF_LEVEL;
}

/* true if jnode is on leaf level */
static inline int jnode_is_leaf(const jnode * node)
{
	if (jnode_is_znode(node))
		return (znode_get_level(JZNODE(node)) == LEAF_LEVEL);
	if (jnode_get_type(node) == JNODE_UNFORMATTED_BLOCK)
		return 1;
	return 0;
}

/* return znode's tree */
static inline reiser4_tree *znode_get_tree(const znode * node)
{
	assert("nikita-2692", node != NULL);
	return jnode_get_tree(ZJNODE(node));
}

/* resolve race with zput */
static inline znode *znode_rip_check(reiser4_tree * tree, znode * node)
{
	jnode *j;

	j = jnode_rip_sync(tree, ZJNODE(node));
	if (likely(j != NULL))
		node = JZNODE(j);
	else
		node = NULL;
	return node;
}

#if defined(REISER4_DEBUG)
int znode_is_loaded(const znode * node /* znode to query */ );
#endif

extern __u64 znode_build_version(reiser4_tree * tree);

/* Data-handles.  A data handle object manages pairing calls to zload() and zrelse().  We
   must load the data for a node in many places.  We could do this by simply calling
   zload() everywhere, the difficulty arises when we must release the loaded data by
   calling zrelse.  In a function with many possible error/return paths, it requires extra
   work to figure out which exit paths must call zrelse and those which do not.  The data
   handle automatically calls zrelse for every zload that it is responsible for.  In that
   sense, it acts much like a lock_handle.
*/
typedef struct load_count {
	znode *node;
	int d_ref;
} load_count;

extern void init_load_count(load_count * lc);	/* Initialize a load_count set the current node to NULL. */
extern void done_load_count(load_count * dh);	/* Finalize a load_count: call zrelse() if necessary */
extern int incr_load_count_znode(load_count * dh, znode * node);	/* Set the argument znode to the current node, call zload(). */
extern int incr_load_count_jnode(load_count * dh, jnode * node);	/* If the argument jnode is formatted, do the same as
									 * incr_load_count_znode, otherwise do nothing (unformatted nodes
									 * don't require zload/zrelse treatment). */
extern void move_load_count(load_count * new, load_count * old);	/* Move the contents of a load_count.  Old handle is released. */
extern void copy_load_count(load_count * new, load_count * old);	/* Copy the contents of a load_count.  Old handle remains held. */

/* Variable initializers for load_count. */
#define INIT_LOAD_COUNT ( load_count * ){ .node = NULL, .d_ref = 0 }
#define INIT_LOAD_COUNT_NODE( n ) ( load_count ){ .node = ( n ), .d_ref = 0 }
/* A convenience macro for use in assertions or debug-only code, where loaded
   data is only required to perform the debugging check.  This macro
   encapsulates an expression inside a pair of calls to zload()/zrelse(). */
#define WITH_DATA( node, exp )				\
({							\
	long __with_dh_result;				\
	znode *__with_dh_node;				\
							\
	__with_dh_node = ( node );			\
	__with_dh_result = zload( __with_dh_node );	\
	if( __with_dh_result == 0 ) {			\
		__with_dh_result = ( long )( exp );	\
		zrelse( __with_dh_node );		\
	}						\
	__with_dh_result;				\
})

/* Same as above, but accepts a return value in case zload fails. */
#define WITH_DATA_RET( node, ret, exp )			\
({							\
	int __with_dh_result;				\
	znode *__with_dh_node;				\
							\
	__with_dh_node = ( node );			\
	__with_dh_result = zload( __with_dh_node );	\
	if( __with_dh_result == 0 ) {			\
		__with_dh_result = ( int )( exp );	\
		zrelse( __with_dh_node );		\
	} else						\
		__with_dh_result = ( ret );		\
	__with_dh_result;				\
})

#define WITH_COORD(coord, exp)			\
({						\
	coord_t *__coord;			\
						\
	__coord = (coord);			\
	coord_clear_iplug(__coord);		\
	WITH_DATA(__coord->node, exp);		\
})

#if REISER4_DEBUG
#define STORE_COUNTERS						\
	reiser4_lock_cnt_info __entry_counters =		\
		*reiser4_lock_counters()
#define CHECK_COUNTERS						        \
ON_DEBUG_CONTEXT(						        \
({							        	\
	__entry_counters.x_refs = reiser4_lock_counters() -> x_refs;	\
	__entry_counters.t_refs = reiser4_lock_counters() -> t_refs;	\
	__entry_counters.d_refs = reiser4_lock_counters() -> d_refs;	\
	assert("nikita-2159",					        \
	       !memcmp(&__entry_counters, reiser4_lock_counters(),	\
		       sizeof __entry_counters));		        \
}) )

#else
#define STORE_COUNTERS
#define CHECK_COUNTERS noop
#endif

/* __ZNODE_H__ */
#endif

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   End:
*/
