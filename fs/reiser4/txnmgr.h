/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

/* data-types and function declarations for transaction manager. See txnmgr.c
 * for details. */

#ifndef __REISER4_TXNMGR_H__
#define __REISER4_TXNMGR_H__

#include "forward.h"
#include "dformat.h"

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/types.h>
#include <linux/spinlock.h>
#include <asm/atomic.h>
#include <linux/wait.h>

/* TYPE DECLARATIONS */

/* This enumeration describes the possible types of a capture request (reiser4_try_capture).
   A capture request dynamically assigns a block to the calling thread's transaction
   handle. */
typedef enum {
	/* A READ_ATOMIC request indicates that a block will be read and that the caller's
	   atom should fuse in order to ensure that the block commits atomically with the
	   caller. */
	TXN_CAPTURE_READ_ATOMIC = (1 << 0),

	/* A READ_NONCOM request indicates that a block will be read and that the caller is
	   willing to read a non-committed block without causing atoms to fuse. */
	TXN_CAPTURE_READ_NONCOM = (1 << 1),

	/* A READ_MODIFY request indicates that a block will be read but that the caller
	   wishes for the block to be captured as it will be written.  This capture request
	   mode is not currently used, but eventually it will be useful for preventing
	   deadlock in read-modify-write cycles. */
	TXN_CAPTURE_READ_MODIFY = (1 << 2),

	/* A WRITE capture request indicates that a block will be modified and that atoms
	   should fuse to make the commit atomic. */
	TXN_CAPTURE_WRITE = (1 << 3),

	/* CAPTURE_TYPES is a mask of the four above capture types, used to separate the
	   exclusive type designation from extra bits that may be supplied -- see
	   below. */
	TXN_CAPTURE_TYPES = (TXN_CAPTURE_READ_ATOMIC |
			     TXN_CAPTURE_READ_NONCOM | TXN_CAPTURE_READ_MODIFY |
			     TXN_CAPTURE_WRITE),

	/* A subset of CAPTURE_TYPES, CAPTURE_WTYPES is a mask of request types that
	   indicate modification will occur. */
	TXN_CAPTURE_WTYPES = (TXN_CAPTURE_READ_MODIFY | TXN_CAPTURE_WRITE),

	/* An option to reiser4_try_capture, NONBLOCKING indicates that the caller would
	   prefer not to sleep waiting for an aging atom to commit. */
	TXN_CAPTURE_NONBLOCKING = (1 << 4),

	/* An option to reiser4_try_capture to prevent atom fusion, just simple
	   capturing is allowed */
	TXN_CAPTURE_DONT_FUSE = (1 << 5)

	/* This macro selects only the exclusive capture request types, stripping out any
	   options that were supplied (i.e., NONBLOCKING). */
#define CAPTURE_TYPE(x) ((x) & TXN_CAPTURE_TYPES)
} txn_capture;

/* There are two kinds of transaction handle: WRITE_FUSING and READ_FUSING, the only
   difference is in the handling of read requests.  A WRITE_FUSING transaction handle
   defaults read capture requests to TXN_CAPTURE_READ_NONCOM whereas a READ_FUSIONG
   transaction handle defaults to TXN_CAPTURE_READ_ATOMIC. */
typedef enum {
	TXN_WRITE_FUSING = (1 << 0),
	TXN_READ_FUSING = (1 << 1) | TXN_WRITE_FUSING,	/* READ implies WRITE */
} txn_mode;

/* Every atom has a stage, which is one of these exclusive values: */
typedef enum {
	/* Initially an atom is free. */
	ASTAGE_FREE = 0,

	/* An atom begins by entering the CAPTURE_FUSE stage, where it proceeds to capture
	   blocks and fuse with other atoms. */
	ASTAGE_CAPTURE_FUSE = 1,

	/* We need to have a ASTAGE_CAPTURE_SLOW in which an atom fuses with one node for every X nodes it flushes to disk where X > 1. */

	/* When an atom reaches a certain age it must do all it can to commit.  An atom in
	   the CAPTURE_WAIT stage refuses new transaction handles and prevents fusion from
	   atoms in the CAPTURE_FUSE stage. */
	ASTAGE_CAPTURE_WAIT = 2,

	/* Waiting for I/O before commit.  Copy-on-capture (see
	   http://namesys.com/v4/v4.html). */
	ASTAGE_PRE_COMMIT = 3,

	/* Post-commit overwrite I/O.  Steal-on-capture. */
	ASTAGE_POST_COMMIT = 4,

	/* Atom which waits for the removal of the last reference to (it? ) to
	 * be deleted from memory  */
	ASTAGE_DONE = 5,

	/* invalid atom. */
	ASTAGE_INVALID = 6,

} txn_stage;

/* Certain flags may be set in the txn_atom->flags field. */
typedef enum {
	/* Indicates that the atom should commit as soon as possible. */
	ATOM_FORCE_COMMIT = (1 << 0),
	/* to avoid endless loop, mark the atom (which was considered as too
	 * small) after failed attempt to fuse it. */
	ATOM_CANCEL_FUSION = (1 << 1)
} txn_flags;

/* Flags for controlling commit_txnh */
typedef enum {
	/* Wait commit atom completion in commit_txnh */
	TXNH_WAIT_COMMIT = 0x2,
	/* Don't commit atom when this handle is closed */
	TXNH_DONT_COMMIT = 0x4
} txn_handle_flags_t;

/* TYPE DEFINITIONS */

/* A note on lock ordering: the handle & jnode spinlock protects reading of their ->atom
   fields, so typically an operation on the atom through either of these objects must (1)
   lock the object, (2) read the atom pointer, (3) lock the atom.

   During atom fusion, the process holds locks on both atoms at once.  Then, it iterates
   through the list of handles and pages held by the smaller of the two atoms.  For each
   handle and page referencing the smaller atom, the fusing process must: (1) lock the
   object, and (2) update the atom pointer.

   You can see that there is a conflict of lock ordering here, so the more-complex
   procedure should have priority, i.e., the fusing process has priority so that it is
   guaranteed to make progress and to avoid restarts.

   This decision, however, means additional complexity for aquiring the atom lock in the
   first place.

   The general original procedure followed in the code was:

       TXN_OBJECT *obj = ...;
       TXN_ATOM   *atom;

       spin_lock (& obj->_lock);

       atom = obj->_atom;

       if (! spin_trylock_atom (atom))
         {
           spin_unlock (& obj->_lock);
           RESTART OPERATION, THERE WAS A RACE;
         }

       ELSE YOU HAVE BOTH ATOM AND OBJ LOCKED

   It has however been found that this wastes CPU a lot in a manner that is
   hard to profile. So, proper refcounting was added to atoms, and new
   standard locking sequence is like following:

       TXN_OBJECT *obj = ...;
       TXN_ATOM   *atom;

       spin_lock (& obj->_lock);

       atom = obj->_atom;

       if (! spin_trylock_atom (atom))
         {
           atomic_inc (& atom->refcount);
           spin_unlock (& obj->_lock);
           spin_lock (&atom->_lock);
           atomic_dec (& atom->refcount);
           // HERE atom is locked
           spin_unlock (&atom->_lock);
           RESTART OPERATION, THERE WAS A RACE;
         }

       ELSE YOU HAVE BOTH ATOM AND OBJ LOCKED

   (core of this is implemented in trylock_throttle() function)

   See the jnode_get_atom() function for a common case.

   As an additional (and important) optimization allowing to avoid restarts,
   it is possible to re-check required pre-conditions at the HERE point in
   code above and proceed without restarting if they are still satisfied.
*/

/* An atomic transaction: this is the underlying system representation
   of a transaction, not the one seen by clients.

   Invariants involving this data-type:

      [sb-fake-allocated]
*/
struct txn_atom {
	/* The spinlock protecting the atom, held during fusion and various other state
	   changes. */
	spinlock_t alock;

	/* The atom's reference counter, increasing (in case of a duplication
	   of an existing reference or when we are sure that some other
	   reference exists) may be done without taking spinlock, decrementing
	   of the ref. counter requires a spinlock to be held.

	   Each transaction handle counts in ->refcount. All jnodes count as
	   one reference acquired in atom_begin_andlock(), released in
	   commit_current_atom().
	 */
	atomic_t refcount;

	/* The atom_id identifies the atom in persistent records such as the log. */
	__u32 atom_id;

	/* Flags holding any of the txn_flags enumerated values (e.g.,
	   ATOM_FORCE_COMMIT). */
	__u32 flags;

	/* Number of open handles. */
	__u32 txnh_count;

	/* The number of znodes captured by this atom.  Equal to the sum of lengths of the
	   dirty_nodes[level] and clean_nodes lists. */
	__u32 capture_count;

#if REISER4_DEBUG
	int clean;
	int dirty;
	int ovrwr;
	int wb;
	int fq;
#endif

	__u32 flushed;

	/* Current transaction stage. */
	txn_stage stage;

	/* Start time. */
	unsigned long start_time;

	/* The atom's delete sets.
	   "simple" are blocknr_set instances and are used when discard is disabled.
	   "discard" are blocknr_list instances and are used when discard is enabled. */
	union {
		struct {
		/* The atom's delete set. It collects block numbers of the nodes
		   which were deleted during the transaction. */
			struct list_head delete_set;
		} nodiscard;

		struct {
			/* The atom's delete set. It collects all blocks that have been
			   deallocated (both immediate and deferred) during the transaction.
			   These blocks are considered for discarding at commit time.
			   For details see discard.c */
			struct list_head delete_set;
		} discard;
	};

	/* The atom's wandered_block mapping. */
	struct list_head wandered_map;

	/* The transaction's list of dirty captured nodes--per level.  Index
	   by (level). dirty_nodes[0] is for znode-above-root */
	struct list_head dirty_nodes[REAL_MAX_ZTREE_HEIGHT + 1];

	/* The transaction's list of clean captured nodes. */
	struct list_head clean_nodes;

	/* The atom's overwrite set */
	struct list_head ovrwr_nodes;

	/* nodes which are being written to disk */
	struct list_head writeback_nodes;

	/* list of inodes */
	struct list_head inodes;

	/* List of handles associated with this atom. */
	struct list_head txnh_list;

	/* Transaction list link: list of atoms in the transaction manager. */
	struct list_head atom_link;

	/* List of handles waiting FOR this atom: see 'capture_fuse_wait' comment. */
	struct list_head fwaitfor_list;

	/* List of this atom's handles that are waiting: see 'capture_fuse_wait' comment. */
	struct list_head fwaiting_list;

	/* Numbers of objects which were deleted/created in this transaction
	   thereby numbers of objects IDs which were released/deallocated. */
	int nr_objects_deleted;
	int nr_objects_created;
	/* number of blocks allocated during the transaction */
	__u64 nr_blocks_allocated;
	/* All atom's flush queue objects are on this list  */
	struct list_head flush_queues;
#if REISER4_DEBUG
	/* number of flush queues for this atom. */
	int nr_flush_queues;
	/* Number of jnodes which were removed from atom's lists and put
	   on flush_queue */
	int num_queued;
#endif
	/* number of threads who wait for this atom to complete commit */
	int nr_waiters;
	/* number of threads which do jnode_flush() over this atom */
	int nr_flushers;
	/* number of flush queues which are IN_USE and jnodes from fq->prepped
	   are submitted to disk by the reiser4_write_fq() routine. */
	int nr_running_queues;
	/* A counter of grabbed unformatted nodes, see a description of the
	 * reiser4 space reservation scheme at block_alloc.c */
	reiser4_block_nr flush_reserved;
#if REISER4_DEBUG
	void *committer;
#endif
	struct super_block *super;
};

#define ATOM_DIRTY_LIST(atom, level) (&(atom)->dirty_nodes[level])
#define ATOM_CLEAN_LIST(atom) (&(atom)->clean_nodes)
#define ATOM_OVRWR_LIST(atom) (&(atom)->ovrwr_nodes)
#define ATOM_WB_LIST(atom) (&(atom)->writeback_nodes)
#define ATOM_FQ_LIST(fq) (&(fq)->prepped)

#define NODE_LIST(node) (node)->list
#define ASSIGN_NODE_LIST(node, list) ON_DEBUG(NODE_LIST(node) = list)
ON_DEBUG(void
	 count_jnode(txn_atom *, jnode *, atom_list old_list,
		     atom_list new_list, int check_lists));

/* A transaction handle: the client obtains and commits this handle which is assigned by
   the system to a txn_atom. */
struct txn_handle {
	/* Spinlock protecting ->atom pointer */
	spinlock_t hlock;

	/* Flags for controlling commit_txnh() behavior */
	/* from txn_handle_flags_t */
	txn_handle_flags_t flags;

	/* Whether it is READ_FUSING or WRITE_FUSING. */
	txn_mode mode;

	/* If assigned, the atom it is part of. */
	txn_atom *atom;

	/* Transaction list link. Head is in txn_atom. */
	struct list_head txnh_link;
};

/* The transaction manager: one is contained in the reiser4_super_info_data */
struct txn_mgr {
	/* A spinlock protecting the atom list, id_count, flush_control */
	spinlock_t tmgr_lock;

	/* List of atoms. */
	struct list_head atoms_list;

	/* Number of atoms. */
	int atom_count;

	/* A counter used to assign atom->atom_id values. */
	__u32 id_count;

	/* a mutex object for commit serialization */
	struct mutex commit_mutex;

	/* a list of all txnmrgs served by particular daemon. */
	struct list_head linkage;

	/* description of daemon for this txnmgr */
	ktxnmgrd_context *daemon;

	/* parameters. Adjustable through mount options. */
	unsigned int atom_max_size;
	unsigned int atom_max_age;
	unsigned int atom_min_size;
	/* max number of concurrent flushers for one atom, 0 - unlimited.  */
	unsigned int atom_max_flushers;
	struct dentry *debugfs_atom_count;
	struct dentry *debugfs_id_count;
};

/* FUNCTION DECLARATIONS */

/* These are the externally (within Reiser4) visible transaction functions, therefore they
   are prefixed with "txn_".  For comments, see txnmgr.c. */

extern int init_txnmgr_static(void);
extern void done_txnmgr_static(void);

extern void reiser4_init_txnmgr(txn_mgr *);
extern void reiser4_done_txnmgr(txn_mgr *);

extern int reiser4_txn_reserve(int reserved);

extern void reiser4_txn_begin(reiser4_context * context);
extern int reiser4_txn_end(reiser4_context * context);

extern void reiser4_txn_restart(reiser4_context * context);
extern void reiser4_txn_restart_current(void);

extern int txnmgr_force_commit_all(struct super_block *, int);
extern int current_atom_should_commit(void);

extern jnode *find_first_dirty_jnode(txn_atom *, int);

extern int commit_some_atoms(txn_mgr *);
extern int force_commit_atom(txn_handle *);
extern int flush_current_atom(int, long, long *, txn_atom **, jnode *);

extern int flush_some_atom(jnode *, long *, const struct writeback_control *, int);

extern void reiser4_atom_set_stage(txn_atom * atom, txn_stage stage);

extern int same_slum_check(jnode * base, jnode * check, int alloc_check,
			   int alloc_value);
extern void atom_dec_and_unlock(txn_atom * atom);

extern int reiser4_try_capture(jnode * node, znode_lock_mode mode, txn_capture flags);
extern int try_capture_page_to_invalidate(struct page *pg);

extern void reiser4_uncapture_page(struct page *pg);
extern void reiser4_uncapture_block(jnode *);
extern void reiser4_uncapture_jnode(jnode *);

extern int reiser4_capture_inode(struct inode *);
extern int reiser4_uncapture_inode(struct inode *);

extern txn_atom *get_current_atom_locked_nocheck(void);

#if REISER4_DEBUG

/**
 * atom_is_protected - make sure that nobody but us can do anything with atom
 * @atom: atom to be checked
 *
 * This is used to assert that atom either entered commit stages or is spin
 * locked.
 */
static inline int atom_is_protected(txn_atom *atom)
{
	if (atom->stage >= ASTAGE_PRE_COMMIT)
		return 1;
	assert_spin_locked(&(atom->alock));
	return 1;
}

#endif

/* Get the current atom and spinlock it if current atom present. May not return NULL */
static inline txn_atom *get_current_atom_locked(void)
{
	txn_atom *atom;

	atom = get_current_atom_locked_nocheck();
	assert("zam-761", atom != NULL);

	return atom;
}

extern txn_atom *jnode_get_atom(jnode *);

extern void reiser4_atom_wait_event(txn_atom *);
extern void reiser4_atom_send_event(txn_atom *);

extern void insert_into_atom_ovrwr_list(txn_atom * atom, jnode * node);
extern int reiser4_capture_super_block(struct super_block *s);
int capture_bulk(jnode **, int count);

/* See the comment on the function blocknrset.c:blocknr_set_add for the
   calling convention of these three routines. */
extern int blocknr_set_init_static(void);
extern void blocknr_set_done_static(void);
extern void blocknr_set_init(struct list_head * bset);
extern void blocknr_set_destroy(struct list_head * bset);
extern void blocknr_set_merge(struct list_head * from, struct list_head * into);
extern int blocknr_set_add_extent(txn_atom * atom,
				  struct list_head * bset,
				  blocknr_set_entry ** new_bsep,
				  const reiser4_block_nr * start,
				  const reiser4_block_nr * len);
extern int blocknr_set_add_pair(txn_atom * atom, struct list_head * bset,
				blocknr_set_entry ** new_bsep,
				const reiser4_block_nr * a,
				const reiser4_block_nr * b);

typedef int (*blocknr_set_actor_f) (txn_atom *, const reiser4_block_nr *,
				    const reiser4_block_nr *, void *);

extern int blocknr_set_iterator(txn_atom * atom, struct list_head * bset,
				blocknr_set_actor_f actor, void *data,
				int delete);

/* This is the block list interface (see blocknrlist.c) */
extern int blocknr_list_init_static(void);
extern void blocknr_list_done_static(void);
extern void blocknr_list_init(struct list_head *blist);
extern void blocknr_list_destroy(struct list_head *blist);
extern void blocknr_list_merge(struct list_head *from, struct list_head *to);
extern void blocknr_list_sort_and_join(struct list_head *blist);
/**
 * The @atom should be locked.
 */
extern int blocknr_list_add_extent(txn_atom *atom,
                                   struct list_head *blist,
                                   blocknr_list_entry **new_entry,
                                   const reiser4_block_nr *start,
                                   const reiser4_block_nr *len);
extern int blocknr_list_iterator(txn_atom *atom,
                                 struct list_head *blist,
                                 blocknr_set_actor_f actor,
                                 void *data,
                                 int delete);

/* These are wrappers for accessing and modifying atom's delete lists,
   depending on whether discard is enabled or not.
   If it is enabled, (less memory efficient) blocknr_list is used for delete
   list storage. Otherwise, blocknr_set is used for this purpose. */
extern void atom_dset_init(txn_atom *atom);
extern void atom_dset_destroy(txn_atom *atom);
extern void atom_dset_merge(txn_atom *from, txn_atom *to);
extern int atom_dset_deferred_apply(txn_atom* atom,
                                    blocknr_set_actor_f actor,
                                    void *data,
                                    int delete);
extern int atom_dset_deferred_add_extent(txn_atom *atom,
                                         void **new_entry,
                                         const reiser4_block_nr *start,
                                         const reiser4_block_nr *len);

/* flush code takes care about how to fuse flush queues */
extern void flush_init_atom(txn_atom * atom);
extern void flush_fuse_queues(txn_atom * large, txn_atom * small);

static inline void spin_lock_atom(txn_atom *atom)
{
	/* check that spinlocks of lower priorities are not held */
	assert("", (LOCK_CNT_NIL(spin_locked_txnh) &&
		    LOCK_CNT_NIL(spin_locked_atom) &&
		    LOCK_CNT_NIL(spin_locked_jnode) &&
		    LOCK_CNT_NIL(spin_locked_zlock) &&
		    LOCK_CNT_NIL(rw_locked_dk) &&
		    LOCK_CNT_NIL(rw_locked_tree)));

	spin_lock(&(atom->alock));

	LOCK_CNT_INC(spin_locked_atom);
	LOCK_CNT_INC(spin_locked);
}

static inline void spin_lock_atom_nested(txn_atom *atom)
{
	assert("", (LOCK_CNT_NIL(spin_locked_txnh) &&
		    LOCK_CNT_NIL(spin_locked_jnode) &&
		    LOCK_CNT_NIL(spin_locked_zlock) &&
		    LOCK_CNT_NIL(rw_locked_dk) &&
		    LOCK_CNT_NIL(rw_locked_tree)));

	spin_lock_nested(&(atom->alock), SINGLE_DEPTH_NESTING);

	LOCK_CNT_INC(spin_locked_atom);
	LOCK_CNT_INC(spin_locked);
}

static inline int spin_trylock_atom(txn_atom *atom)
{
	if (spin_trylock(&(atom->alock))) {
		LOCK_CNT_INC(spin_locked_atom);
		LOCK_CNT_INC(spin_locked);
		return 1;
	}
	return 0;
}

static inline void spin_unlock_atom(txn_atom *atom)
{
	assert_spin_locked(&(atom->alock));
	assert("nikita-1375", LOCK_CNT_GTZ(spin_locked_atom));
	assert("nikita-1376", LOCK_CNT_GTZ(spin_locked));

	LOCK_CNT_DEC(spin_locked_atom);
	LOCK_CNT_DEC(spin_locked);

	spin_unlock(&(atom->alock));
}

static inline void spin_lock_txnh(txn_handle *txnh)
{
	/* check that spinlocks of lower priorities are not held */
	assert("", (LOCK_CNT_NIL(rw_locked_dk) &&
		    LOCK_CNT_NIL(spin_locked_zlock) &&
		    LOCK_CNT_NIL(rw_locked_tree)));

	spin_lock(&(txnh->hlock));

	LOCK_CNT_INC(spin_locked_txnh);
	LOCK_CNT_INC(spin_locked);
}

static inline int spin_trylock_txnh(txn_handle *txnh)
{
	if (spin_trylock(&(txnh->hlock))) {
		LOCK_CNT_INC(spin_locked_txnh);
		LOCK_CNT_INC(spin_locked);
		return 1;
	}
	return 0;
}

static inline void spin_unlock_txnh(txn_handle *txnh)
{
	assert_spin_locked(&(txnh->hlock));
	assert("nikita-1375", LOCK_CNT_GTZ(spin_locked_txnh));
	assert("nikita-1376", LOCK_CNT_GTZ(spin_locked));

	LOCK_CNT_DEC(spin_locked_txnh);
	LOCK_CNT_DEC(spin_locked);

	spin_unlock(&(txnh->hlock));
}

#define spin_ordering_pred_txnmgr(tmgr)		\
	( LOCK_CNT_NIL(spin_locked_atom) &&	\
	  LOCK_CNT_NIL(spin_locked_txnh) &&	\
	  LOCK_CNT_NIL(spin_locked_jnode) &&	\
	  LOCK_CNT_NIL(rw_locked_zlock) &&	\
	  LOCK_CNT_NIL(rw_locked_dk) &&		\
	  LOCK_CNT_NIL(rw_locked_tree) )

static inline void spin_lock_txnmgr(txn_mgr *mgr)
{
	/* check that spinlocks of lower priorities are not held */
	assert("", (LOCK_CNT_NIL(spin_locked_atom) &&
		    LOCK_CNT_NIL(spin_locked_txnh) &&
		    LOCK_CNT_NIL(spin_locked_jnode) &&
		    LOCK_CNT_NIL(spin_locked_zlock) &&
		    LOCK_CNT_NIL(rw_locked_dk) &&
		    LOCK_CNT_NIL(rw_locked_tree)));

	spin_lock(&(mgr->tmgr_lock));

	LOCK_CNT_INC(spin_locked_txnmgr);
	LOCK_CNT_INC(spin_locked);
}

static inline int spin_trylock_txnmgr(txn_mgr *mgr)
{
	if (spin_trylock(&(mgr->tmgr_lock))) {
		LOCK_CNT_INC(spin_locked_txnmgr);
		LOCK_CNT_INC(spin_locked);
		return 1;
	}
	return 0;
}

static inline void spin_unlock_txnmgr(txn_mgr *mgr)
{
	assert_spin_locked(&(mgr->tmgr_lock));
	assert("nikita-1375", LOCK_CNT_GTZ(spin_locked_txnmgr));
	assert("nikita-1376", LOCK_CNT_GTZ(spin_locked));

	LOCK_CNT_DEC(spin_locked_txnmgr);
	LOCK_CNT_DEC(spin_locked);

	spin_unlock(&(mgr->tmgr_lock));
}

typedef enum {
	FQ_IN_USE = 0x1
} flush_queue_state_t;

typedef struct flush_queue flush_queue_t;

/* This is an accumulator for jnodes prepared for writing to disk. A flush queue
   is filled by the jnode_flush() routine, and written to disk under memory
   pressure or at atom commit time. */
/* LOCKING: fq state and fq->atom are protected by guard spinlock, fq->nr_queued
   field and fq->prepped list can be modified if atom is spin-locked and fq
   object is "in-use" state.  For read-only traversal of the fq->prepped list
   and reading of the fq->nr_queued field it is enough to keep fq "in-use" or
   only have atom spin-locked. */
struct flush_queue {
	/* linkage element is the first in this structure to make debugging
	   easier.  See field in atom struct for description of list. */
	struct list_head alink;
	/* A spinlock to protect changes of fq state and fq->atom pointer */
	spinlock_t guard;
	/* flush_queue state: [in_use | ready] */
	flush_queue_state_t state;
	/* A list which contains queued nodes, queued nodes are removed from any
	 * atom's list and put on this ->prepped one. */
	struct list_head prepped;
	/* number of submitted i/o requests */
	atomic_t nr_submitted;
	/* number of i/o errors */
	atomic_t nr_errors;
	/* An atom this flush queue is attached to */
	txn_atom *atom;
	/* A wait queue head to wait on i/o completion */
	wait_queue_head_t wait;
#if REISER4_DEBUG
	/* A thread which took this fq in exclusive use, NULL if fq is free,
	 * used for debugging. */
	struct task_struct *owner;
#endif
};

extern int reiser4_fq_by_atom(txn_atom *, flush_queue_t **);
extern void reiser4_fq_put_nolock(flush_queue_t *);
extern void reiser4_fq_put(flush_queue_t *);
extern void reiser4_fuse_fq(txn_atom * to, txn_atom * from);
extern void queue_jnode(flush_queue_t *, jnode *);

extern int reiser4_write_fq(flush_queue_t *, long *, int);
extern int current_atom_finish_all_fq(void);
extern void init_atom_fq_parts(txn_atom *);

extern reiser4_block_nr txnmgr_count_deleted_blocks(void);

extern void znode_make_dirty(znode * node);
extern void jnode_make_dirty_locked(jnode * node);

extern int reiser4_sync_atom(txn_atom * atom);

#if REISER4_DEBUG
extern int atom_fq_parts_are_clean(txn_atom *);
#endif

extern void add_fq_to_bio(flush_queue_t *, struct bio *);
extern flush_queue_t *get_fq_for_current_atom(void);

void reiser4_invalidate_list(struct list_head * head);

# endif				/* __REISER4_TXNMGR_H__ */

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   End:
*/
