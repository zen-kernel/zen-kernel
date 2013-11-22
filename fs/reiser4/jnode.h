/* Copyright 2001, 2002, 2003, 2004 by Hans Reiser, licensing governed by
 * reiser4/README */

/* Declaration of jnode. See jnode.c for details. */

#ifndef __JNODE_H__
#define __JNODE_H__

#include "forward.h"
#include "type_safe_hash.h"
#include "txnmgr.h"
#include "key.h"
#include "debug.h"
#include "dformat.h"
#include "page_cache.h"
#include "context.h"

#include "plugin/plugin.h"

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/spinlock.h>
#include <asm/atomic.h>
#include <linux/bitops.h>
#include <linux/list.h>
#include <linux/rcupdate.h>

/* declare hash table of jnodes (jnodes proper, that is, unformatted
   nodes)  */
TYPE_SAFE_HASH_DECLARE(j, jnode);

/* declare hash table of znodes */
TYPE_SAFE_HASH_DECLARE(z, znode);

struct jnode_key {
	__u64 objectid;
	unsigned long index;
	struct address_space *mapping;
};

/*
   Jnode is the "base class" of other nodes in reiser4. It is also happens to
   be exactly the node we use for unformatted tree nodes.

   Jnode provides following basic functionality:

   . reference counting and indexing.

   . integration with page cache. Jnode has ->pg reference to which page can
   be attached.

   . interface to transaction manager. It is jnode that is kept in transaction
   manager lists, attached to atoms, etc. (NOTE-NIKITA one may argue that this
   means, there should be special type of jnode for inode.)

   Locking:

   Spin lock: the following fields are protected by the per-jnode spin lock:

    ->state
    ->atom
    ->capture_link

   Following fields are protected by the global tree lock:

    ->link
    ->key.z (content of ->key.z is only changed in znode_rehash())
    ->key.j

   Atomic counters

    ->x_count
    ->d_count

    ->pg, and ->data are protected by spin lock for unused jnode and are
    immutable for used jnode (one for which fs/reiser4/vfs_ops.c:releasable()
    is false).

    ->tree is immutable after creation

   Unclear

    ->blocknr: should be under jnode spin-lock, but current interface is based
    on passing of block address.

   If you ever need to spin lock two nodes at once, do this in "natural"
   memory order: lock znode with lower address first. (See lock_two_nodes().)

   Invariants involving this data-type:

      [jnode-dirty]
      [jnode-refs]
      [jnode-oid]
      [jnode-queued]
      [jnode-atom-valid]
      [jnode-page-binding]
*/

struct jnode {
#if REISER4_DEBUG
#define JMAGIC 0x52654973	/* "ReIs" */
	int magic;
#endif
	/* FIRST CACHE LINE (16 bytes): data used by jload */

	/* jnode's state: bitwise flags from the reiser4_jnode_state enum. */
	/*   0 */ unsigned long state;

	/* lock, protecting jnode's fields. */
	/*   4 */ spinlock_t load;

	/* counter of references to jnode itself. Increased on jref().
	   Decreased on jput().
	 */
	/*   8 */ atomic_t x_count;

	/* counter of references to jnode's data. Pin data page(s) in
	   memory while this is greater than 0. Increased on jload().
	   Decreased on jrelse().
	 */
	/*   12 */ atomic_t d_count;

	/* SECOND CACHE LINE: data used by hash table lookups */

	/*   16 */ union {
		/* znodes are hashed by block number */
		reiser4_block_nr z;
		/* unformatted nodes are hashed by mapping plus offset */
		struct jnode_key j;
	} key;

	/* THIRD CACHE LINE */

	/*   32 */ union {
		/* pointers to maintain hash-table */
		z_hash_link z;
		j_hash_link j;
	} link;

	/* pointer to jnode page.  */
	/*   36 */ struct page *pg;
	/* pointer to node itself. This is page_address(node->pg) when page is
	   attached to the jnode
	 */
	/*   40 */ void *data;

	/*   44 */ reiser4_tree *tree;

	/* FOURTH CACHE LINE: atom related fields */

	/*   48 */ spinlock_t guard;

	/* atom the block is in, if any */
	/*   52 */ txn_atom *atom;

	/* capture list */
	/*   56 */ struct list_head capture_link;

	/* FIFTH CACHE LINE */

	/*   64 */ struct rcu_head rcu;
	/* crosses cache line */

	/* SIXTH CACHE LINE */

	/* the real blocknr (where io is going to/from) */
	/*   80 */ reiser4_block_nr blocknr;
	/* Parent item type, unformatted and CRC need it for
	 * offset => key conversion.  */
	/* NOTE: this parent_item_id looks like jnode type. */
	/*   88 */ reiser4_plugin_id parent_item_id;
	/*   92 */
#if REISER4_DEBUG
	/* list of all jnodes for debugging purposes. */
	struct list_head jnodes;
	/* how many times this jnode was written in one transaction */
	int written;
	/* this indicates which atom's list the jnode is on */
	atom_list list;
#endif
} __attribute__ ((aligned(16)));

/*
 * jnode types. Enumeration of existing jnode types.
 */
typedef enum {
	JNODE_UNFORMATTED_BLOCK,	/* unformatted block */
	JNODE_FORMATTED_BLOCK,	/* formatted block, znode */
	JNODE_BITMAP,		/* bitmap */
	JNODE_IO_HEAD,		/* jnode representing a block in the
				 * wandering log */
	JNODE_INODE,		/* jnode embedded into inode */
	LAST_JNODE_TYPE
} jnode_type;

/* jnode states */
typedef enum {
	/* jnode's page is loaded and data checked */
	JNODE_PARSED = 0,
	/* node was deleted, not all locks on it were released. This
	   node is empty and is going to be removed from the tree
	   shortly. */
	JNODE_HEARD_BANSHEE = 1,
	/* left sibling pointer is valid */
	JNODE_LEFT_CONNECTED = 2,
	/* right sibling pointer is valid */
	JNODE_RIGHT_CONNECTED = 3,

	/* znode was just created and doesn't yet have a pointer from
	   its parent */
	JNODE_ORPHAN = 4,

	/* this node was created by its transaction and has not been assigned
	   a block address. */
	JNODE_CREATED = 5,

	/* this node is currently relocated */
	JNODE_RELOC = 6,
	/* this node is currently wandered */
	JNODE_OVRWR = 7,

	/* this znode has been modified */
	JNODE_DIRTY = 8,

	/* znode lock is being invalidated */
	JNODE_IS_DYING = 9,

	/* THIS PLACE IS INTENTIONALLY LEFT BLANK */

	/* jnode is queued for flushing. */
	JNODE_FLUSH_QUEUED = 12,

	/* In the following bits jnode type is encoded. */
	JNODE_TYPE_1 = 13,
	JNODE_TYPE_2 = 14,
	JNODE_TYPE_3 = 15,

	/* jnode is being destroyed */
	JNODE_RIP = 16,

	/* znode was not captured during locking (it might so be because
	   ->level != LEAF_LEVEL and lock_mode == READ_LOCK) */
	JNODE_MISSED_IN_CAPTURE = 17,

	/* write is in progress */
	JNODE_WRITEBACK = 18,

	/* FIXME: now it is used by crypto-compress plugin only */
	JNODE_NEW = 19,

	/* delimiting keys are already set for this znode. */
	JNODE_DKSET = 20,

	/* when this bit is set page and jnode can not be disconnected */
	JNODE_WRITE_PREPARED = 21,

	JNODE_CLUSTER_PAGE = 22,
	/* Jnode is marked for repacking, that means the reiser4 flush and the
	 * block allocator should process this node special way  */
	JNODE_REPACK = 23,
	/* node should be converted by flush in squalloc phase */
	JNODE_CONVERTIBLE = 24,
	/*
	 * When jnode is dirtied for the first time in given transaction,
	 * do_jnode_make_dirty() checks whether this jnode can possible became
	 * member of overwrite set. If so, this bit is set, and one block is
	 * reserved in the ->flush_reserved space of atom.
	 *
	 * This block is "used" (and JNODE_FLUSH_RESERVED bit is cleared) when
	 *
	 *     (1) flush decides that we want this block to go into relocate
	 *     set after all.
	 *
	 *     (2) wandering log is allocated (by log writer)
	 *
	 *     (3) extent is allocated
	 *
	 */
	JNODE_FLUSH_RESERVED = 29
} reiser4_jnode_state;

/* Macros for accessing the jnode state. */

static inline void JF_CLR(jnode * j, int f)
{
	assert("unknown-1", j->magic == JMAGIC);
	clear_bit(f, &j->state);
}
static inline int JF_ISSET(const jnode * j, int f)
{
	assert("unknown-2", j->magic == JMAGIC);
	return test_bit(f, &((jnode *) j)->state);
}
static inline void JF_SET(jnode * j, int f)
{
	assert("unknown-3", j->magic == JMAGIC);
	set_bit(f, &j->state);
}

static inline int JF_TEST_AND_SET(jnode * j, int f)
{
	assert("unknown-4", j->magic == JMAGIC);
	return test_and_set_bit(f, &j->state);
}

static inline void spin_lock_jnode(jnode *node)
{
	/* check that spinlocks of lower priorities are not held */
	assert("", (LOCK_CNT_NIL(rw_locked_tree) &&
		    LOCK_CNT_NIL(spin_locked_txnh) &&
		    LOCK_CNT_NIL(spin_locked_zlock) &&
		    LOCK_CNT_NIL(rw_locked_dk) &&
		    LOCK_CNT_LT(spin_locked_jnode, 2)));

	spin_lock(&(node->guard));

	LOCK_CNT_INC(spin_locked_jnode);
	LOCK_CNT_INC(spin_locked);
}

static inline void spin_unlock_jnode(jnode *node)
{
	assert_spin_locked(&(node->guard));
	assert("nikita-1375", LOCK_CNT_GTZ(spin_locked_jnode));
	assert("nikita-1376", LOCK_CNT_GTZ(spin_locked));

	LOCK_CNT_DEC(spin_locked_jnode);
	LOCK_CNT_DEC(spin_locked);

	spin_unlock(&(node->guard));
}

static inline int jnode_is_in_deleteset(const jnode * node)
{
	return JF_ISSET(node, JNODE_RELOC);
}

extern int init_jnodes(void);
extern void done_jnodes(void);

/* Jnode routines */
extern jnode *jalloc(void);
extern void jfree(jnode * node) NONNULL;
extern jnode *jclone(jnode *);
extern jnode *jlookup(reiser4_tree * tree,
		      oid_t objectid, unsigned long ind) NONNULL;
extern jnode *jfind(struct address_space *, unsigned long index) NONNULL;
extern jnode *jnode_by_page(struct page *pg) NONNULL;
extern jnode *jnode_of_page(struct page *pg) NONNULL;
void jnode_attach_page(jnode * node, struct page *pg);

void unhash_unformatted_jnode(jnode *);
extern jnode *page_next_jnode(jnode * node) NONNULL;
extern void jnode_init(jnode * node, reiser4_tree * tree, jnode_type) NONNULL;
extern void jnode_make_dirty(jnode * node) NONNULL;
extern void jnode_make_clean(jnode * node) NONNULL;
extern void jnode_make_wander_nolock(jnode * node) NONNULL;
extern void jnode_make_wander(jnode *) NONNULL;
extern void znode_make_reloc(znode * , flush_queue_t *) NONNULL;
extern void unformatted_make_reloc(jnode *, flush_queue_t *) NONNULL;
extern struct address_space *jnode_get_mapping(const jnode * node) NONNULL;

/**
 * jnode_get_block
 * @node: jnode to query
 *
 */
static inline const reiser4_block_nr *jnode_get_block(const jnode *node)
{
	assert("nikita-528", node != NULL);

	return &node->blocknr;
}

/**
 * jnode_set_block
 * @node: jnode to update
 * @blocknr: new block nr
 */
static inline void jnode_set_block(jnode *node, const reiser4_block_nr *blocknr)
{
	assert("nikita-2020", node != NULL);
	assert("umka-055", blocknr != NULL);
	node->blocknr = *blocknr;
}


/* block number for IO. Usually this is the same as jnode_get_block(), unless
 * jnode was emergency flushed---then block number chosen by eflush is
 * used. */
static inline const reiser4_block_nr *jnode_get_io_block(jnode * node)
{
	assert("nikita-2768", node != NULL);
	assert_spin_locked(&(node->guard));

	return jnode_get_block(node);
}

/* Jnode flush interface. */
extern reiser4_blocknr_hint *reiser4_pos_hint(flush_pos_t *pos);
extern flush_queue_t *reiser4_pos_fq(flush_pos_t *pos);

/* FIXME-VS: these are used in plugin/item/extent.c */

/* does extent_get_block have to be called */
#define jnode_mapped(node)     JF_ISSET (node, JNODE_MAPPED)
#define jnode_set_mapped(node) JF_SET (node, JNODE_MAPPED)

/* the node should be converted during flush squalloc phase */
#define jnode_convertible(node)        JF_ISSET (node, JNODE_CONVERTIBLE)
#define jnode_set_convertible(node)    JF_SET (node, JNODE_CONVERTIBLE)

/* Macros to convert from jnode to znode, znode to jnode.  These are macros
   because C doesn't allow overloading of const prototypes. */
#define ZJNODE(x) (&(x)->zjnode)
#define JZNODE(x)						\
({								\
	typeof(x) __tmp_x;					\
								\
	__tmp_x = (x);						\
	assert("jmacd-1300", jnode_is_znode(__tmp_x));		\
	(znode*) __tmp_x;					\
})

extern int jnodes_tree_init(reiser4_tree * tree);
extern int jnodes_tree_done(reiser4_tree * tree);

#if REISER4_DEBUG

extern int znode_is_any_locked(const znode * node);
extern void jnode_list_remove(jnode * node);

#else

#define jnode_list_remove(node) noop

#endif

int znode_is_root(const znode * node) NONNULL;

/* bump reference counter on @node */
static inline void add_x_ref(jnode * node/* node to increase x_count of */)
{
	assert("nikita-1911", node != NULL);

	atomic_inc(&node->x_count);
	LOCK_CNT_INC(x_refs);
}

static inline void dec_x_ref(jnode * node)
{
	assert("nikita-3215", node != NULL);
	assert("nikita-3216", atomic_read(&node->x_count) > 0);

	atomic_dec(&node->x_count);
	assert("nikita-3217", LOCK_CNT_GTZ(x_refs));
	LOCK_CNT_DEC(x_refs);
}

/* jref() - increase counter of references to jnode/znode (x_count) */
static inline jnode *jref(jnode * node)
{
	assert("jmacd-508", (node != NULL) && !IS_ERR(node));
	add_x_ref(node);
	return node;
}

/* get the page of jnode */
static inline struct page *jnode_page(const jnode * node)
{
	return node->pg;
}

/* return pointer to jnode data */
static inline char *jdata(const jnode * node)
{
	assert("nikita-1415", node != NULL);
	assert("nikita-3198", jnode_page(node) != NULL);
	return node->data;
}

static inline int jnode_is_loaded(const jnode * node)
{
	assert("zam-506", node != NULL);
	return atomic_read(&node->d_count) > 0;
}

extern void page_clear_jnode(struct page *page, jnode * node) NONNULL;

static inline void jnode_set_reloc(jnode * node)
{
	assert("nikita-2431", node != NULL);
	assert("nikita-2432", !JF_ISSET(node, JNODE_OVRWR));
	JF_SET(node, JNODE_RELOC);
}

/* jload/jwrite/junload give a bread/bwrite/brelse functionality for jnodes */

extern int jload_gfp(jnode *, gfp_t, int do_kmap) NONNULL;

static inline int jload(jnode *node)
{
	return jload_gfp(node, reiser4_ctx_gfp_mask_get(), 1);
}

extern int jinit_new(jnode *, gfp_t) NONNULL;
extern int jstartio(jnode *) NONNULL;

extern void jdrop(jnode *) NONNULL;
extern int jwait_io(jnode *, int rw) NONNULL;

void jload_prefetch(jnode *);

extern jnode *reiser4_alloc_io_head(const reiser4_block_nr * block) NONNULL;
extern void reiser4_drop_io_head(jnode * node) NONNULL;

static inline reiser4_tree *jnode_get_tree(const jnode * node)
{
	assert("nikita-2691", node != NULL);
	return node->tree;
}

extern void pin_jnode_data(jnode *);
extern void unpin_jnode_data(jnode *);

static inline jnode_type jnode_get_type(const jnode * node)
{
	static const unsigned long state_mask =
	    (1 << JNODE_TYPE_1) | (1 << JNODE_TYPE_2) | (1 << JNODE_TYPE_3);

	static jnode_type mask_to_type[] = {
		/*  JNODE_TYPE_3 : JNODE_TYPE_2 : JNODE_TYPE_1 */

		/* 000 */
		[0] = JNODE_FORMATTED_BLOCK,
		/* 001 */
		[1] = JNODE_UNFORMATTED_BLOCK,
		/* 010 */
		[2] = JNODE_BITMAP,
		/* 011 */
		[3] = LAST_JNODE_TYPE,	/*invalid */
		/* 100 */
		[4] = JNODE_INODE,
		/* 101 */
		[5] = LAST_JNODE_TYPE,
		/* 110 */
		[6] = JNODE_IO_HEAD,
		/* 111 */
		[7] = LAST_JNODE_TYPE,	/* invalid */
	};

	return mask_to_type[(node->state & state_mask) >> JNODE_TYPE_1];
}

/* returns true if node is a znode */
static inline int jnode_is_znode(const jnode * node)
{
	return jnode_get_type(node) == JNODE_FORMATTED_BLOCK;
}

static inline int jnode_is_flushprepped(jnode * node)
{
	assert("jmacd-78212", node != NULL);
	assert_spin_locked(&(node->guard));
	return !JF_ISSET(node, JNODE_DIRTY) || JF_ISSET(node, JNODE_RELOC) ||
		JF_ISSET(node, JNODE_OVRWR);
}

/* Return true if @node has already been processed by the squeeze and allocate
   process.  This implies the block address has been finalized for the
   duration of this atom (or it is clean and will remain in place).  If this
   returns true you may use the block number as a hint. */
static inline int jnode_check_flushprepped(jnode * node)
{
	int result;

	/* It must be clean or relocated or wandered.  New allocations are set
	 * to relocate. */
	spin_lock_jnode(node);
	result = jnode_is_flushprepped(node);
	spin_unlock_jnode(node);
	return result;
}

/* returns true if node is unformatted */
static inline int jnode_is_unformatted(const jnode * node)
{
	assert("jmacd-0123", node != NULL);
	return jnode_get_type(node) == JNODE_UNFORMATTED_BLOCK;
}

/* returns true if node represents a cluster cache page */
static inline int jnode_is_cluster_page(const jnode * node)
{
	assert("edward-50", node != NULL);
	return (JF_ISSET(node, JNODE_CLUSTER_PAGE));
}

/* returns true is node is builtin inode's jnode */
static inline int jnode_is_inode(const jnode * node)
{
	assert("vs-1240", node != NULL);
	return jnode_get_type(node) == JNODE_INODE;
}

static inline jnode_plugin *jnode_ops_of(const jnode_type type)
{
	assert("nikita-2367", type < LAST_JNODE_TYPE);
	return jnode_plugin_by_id((reiser4_plugin_id) type);
}

static inline jnode_plugin *jnode_ops(const jnode * node)
{
	assert("nikita-2366", node != NULL);

	return jnode_ops_of(jnode_get_type(node));
}

/* Get the index of a block. */
static inline unsigned long jnode_get_index(jnode * node)
{
	return jnode_ops(node)->index(node);
}

/* return true if "node" is the root */
static inline int jnode_is_root(const jnode * node)
{
	return jnode_is_znode(node) && znode_is_root(JZNODE(node));
}

extern struct address_space *mapping_jnode(const jnode * node);
extern unsigned long index_jnode(const jnode * node);

static inline void jput(jnode * node);
extern void jput_final(jnode * node);

/* bump data counter on @node */
static inline void add_d_ref(jnode * node/* node to increase d_count of */)
{
	assert("nikita-1962", node != NULL);

	atomic_inc(&node->d_count);
	if (jnode_is_unformatted(node) || jnode_is_znode(node))
		LOCK_CNT_INC(d_refs);
}

/* jput() - decrement x_count reference counter on znode.

   Count may drop to 0, jnode stays in cache until memory pressure causes the
   eviction of its page. The c_count variable also ensures that children are
   pressured out of memory before the parent. The jnode remains hashed as
   long as the VM allows its page to stay in memory.
*/
static inline void jput(jnode * node)
{
	assert("jmacd-509", node != NULL);
	assert("jmacd-510", atomic_read(&node->x_count) > 0);
	assert("zam-926", reiser4_schedulable());
	LOCK_CNT_DEC(x_refs);

	rcu_read_lock();
	/*
	 * we don't need any kind of lock here--jput_final() uses RCU.
	 */
	if (unlikely(atomic_dec_and_test(&node->x_count)))
		jput_final(node);
	else
		rcu_read_unlock();
	assert("nikita-3473", reiser4_schedulable());
}

extern void jrelse(jnode * node);
extern void jrelse_tail(jnode * node);

extern jnode *jnode_rip_sync(reiser4_tree * t, jnode * node);

/* resolve race with jput */
static inline jnode *jnode_rip_check(reiser4_tree * tree, jnode * node)
{
	if (unlikely(JF_ISSET(node, JNODE_RIP)))
		node = jnode_rip_sync(tree, node);
	return node;
}

extern reiser4_key *jnode_build_key(const jnode *node, reiser4_key * key);

#if REISER4_DEBUG
extern int jnode_invariant_f(const jnode *node, char const **msg);
#endif

extern jnode_plugin jnode_plugins[LAST_JNODE_TYPE];

/* __JNODE_H__ */
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
