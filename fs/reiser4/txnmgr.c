/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

/* Joshua MacDonald wrote the first draft of this code. */

/* ZAM-LONGTERM-FIXME-HANS: The locking in this file is badly designed, and a
filesystem scales only as well as its worst locking design.  You need to
substantially restructure this code. Josh was not as experienced a programmer
as you.  Particularly review how the locking style differs from what you did
for znodes usingt hi-lo priority locking, and present to me an opinion on
whether the differences are well founded.  */

/* I cannot help but to disagree with the sentiment above. Locking of
 * transaction manager is _not_ badly designed, and, at the very least, is not
 * the scaling bottleneck. Scaling bottleneck is _exactly_ hi-lo priority
 * locking on znodes, especially on the root node of the tree. --nikita,
 * 2003.10.13 */

/* The txnmgr is a set of interfaces that keep track of atoms and transcrash handles.  The
   txnmgr processes capture_block requests and manages the relationship between jnodes and
   atoms through the various stages of a transcrash, and it also oversees the fusion and
   capture-on-copy processes.  The main difficulty with this task is maintaining a
   deadlock-free lock ordering between atoms and jnodes/handles.  The reason for the
   difficulty is that jnodes, handles, and atoms contain pointer circles, and the cycle
   must be broken.  The main requirement is that atom-fusion be deadlock free, so once you
   hold the atom_lock you may then wait to acquire any jnode or handle lock.  This implies
   that any time you check the atom-pointer of a jnode or handle and then try to lock that
   atom, you must use trylock() and possibly reverse the order.

   This code implements the design documented at:

     http://namesys.com/txn-doc.html

ZAM-FIXME-HANS: update v4.html to contain all of the information present in the above (but updated), and then remove the
above document and reference the new.  Be sure to provide some credit to Josh.  I already have some writings on this
topic in v4.html, but they are lacking in details present in the above.  Cure that.  Remember to write for the bright 12
year old --- define all technical terms used.

*/

/* Thoughts on the external transaction interface:

   In the current code, a TRANSCRASH handle is created implicitly by reiser4_init_context() (which
   creates state that lasts for the duration of a system call and is called at the start
   of ReiserFS methods implementing VFS operations), and closed by reiser4_exit_context(),
   occupying the scope of a single system call.  We wish to give certain applications an
   interface to begin and close (commit) transactions.  Since our implementation of
   transactions does not yet support isolation, allowing an application to open a
   transaction implies trusting it to later close the transaction.  Part of the
   transaction interface will be aimed at enabling that trust, but the interface for
   actually using transactions is fairly narrow.

   BEGIN_TRANSCRASH: Returns a transcrash identifier.  It should be possible to translate
   this identifier into a string that a shell-script could use, allowing you to start a
   transaction by issuing a command.  Once open, the transcrash should be set in the task
   structure, and there should be options (I suppose) to allow it to be carried across
   fork/exec.  A transcrash has several options:

     - READ_FUSING or WRITE_FUSING: The default policy is for txn-capture to capture only
     on writes (WRITE_FUSING) and allow "dirty reads".  If the application wishes to
     capture on reads as well, it should set READ_FUSING.

     - TIMEOUT: Since a non-isolated transcrash cannot be undone, every transcrash must
     eventually close (or else the machine must crash).  If the application dies an
     unexpected death with an open transcrash, for example, or if it hangs for a long
     duration, one solution (to avoid crashing the machine) is to simply close it anyway.
     This is a dangerous option, but it is one way to solve the problem until isolated
     transcrashes are available for untrusted applications.

     It seems to be what databases do, though it is unclear how one avoids a DoS attack
     creating a vulnerability based on resource starvation.  Guaranteeing that some
     minimum amount of computational resources are made available would seem more correct
     than guaranteeing some amount of time.  When we again have someone to code the work,
     this issue should be considered carefully.  -Hans

   RESERVE_BLOCKS: A running transcrash should indicate to the transaction manager how
   many dirty blocks it expects.  The reserve_blocks interface should be called at a point
   where it is safe for the application to fail, because the system may not be able to
   grant the allocation and the application must be able to back-out.  For this reason,
   the number of reserve-blocks can also be passed as an argument to BEGIN_TRANSCRASH, but
   the application may also wish to extend the allocation after beginning its transcrash.

   CLOSE_TRANSCRASH: The application closes the transcrash when it is finished making
   modifications that require transaction protection.  When isolated transactions are
   supported the CLOSE operation is replaced by either COMMIT or ABORT.  For example, if a
   RESERVE_BLOCKS call fails for the application, it should "abort" by calling
   CLOSE_TRANSCRASH, even though it really commits any changes that were made (which is
   why, for safety, the application should call RESERVE_BLOCKS before making any changes).

   For actually implementing these out-of-system-call-scopped transcrashes, the
   reiser4_context has a "txn_handle *trans" pointer that may be set to an open
   transcrash.  Currently there are no dynamically-allocated transcrashes, but there is a
   "struct kmem_cache *_txnh_slab" created for that purpose in this file.
*/

/* Extending the other system call interfaces for future transaction features:

   Specialized applications may benefit from passing flags to the ordinary system call
   interface such as read(), write(), or stat().  For example, the application specifies
   WRITE_FUSING by default but wishes to add that a certain read() command should be
   treated as READ_FUSING.  But which read?  Is it the directory-entry read, the stat-data
   read, or the file-data read?  These issues are straight-forward, but there are a lot of
   them and adding the necessary flags-passing code will be tedious.

   When supporting isolated transactions, there is a corresponding READ_MODIFY_WRITE (RMW)
   flag, which specifies that although it is a read operation being requested, a
   write-lock should be taken.  The reason is that read-locks are shared while write-locks
   are exclusive, so taking a read-lock when a later-write is known in advance will often
   leads to deadlock.  If a reader knows it will write later, it should issue read
   requests with the RMW flag set.
*/

/*
   The znode/atom deadlock avoidance.

   FIXME(Zam): writing of this comment is in progress.

   The atom's special stage ASTAGE_CAPTURE_WAIT introduces a kind of atom's
   long-term locking, which makes reiser4 locking scheme more complex.  It had
   deadlocks until we implement deadlock avoidance algorithms.  That deadlocks
   looked as the following: one stopped thread waits for a long-term lock on
   znode, the thread who owns that lock waits when fusion with another atom will
   be allowed.

   The source of the deadlocks is an optimization of not capturing index nodes
   for read.  Let's prove it.  Suppose we have dumb node capturing scheme which
   unconditionally captures each block before locking it.

   That scheme has no deadlocks.  Let's begin with the thread which stage is
   ASTAGE_CAPTURE_WAIT and it waits for a znode lock.  The thread can't wait for
   a capture because it's stage allows fusion with any atom except which are
   being committed currently. A process of atom commit can't deadlock because
   atom commit procedure does not acquire locks and does not fuse with other
   atoms.  Reiser4 does capturing right before going to sleep inside the
   longtertm_lock_znode() function, it means the znode which we want to lock is
   already captured and its atom is in ASTAGE_CAPTURE_WAIT stage.  If we
   continue the analysis we understand that no one process in the sequence may
   waits atom fusion.  Thereby there are no deadlocks of described kind.

   The capturing optimization makes the deadlocks possible.  A thread can wait a
   lock which owner did not captured that node.  The lock owner's current atom
   is not fused with the first atom and it does not get a ASTAGE_CAPTURE_WAIT
   state. A deadlock is possible when that atom meets another one which is in
   ASTAGE_CAPTURE_WAIT already.

   The deadlock avoidance scheme includes two algorithms:

   First algorithm is used when a thread captures a node which is locked but not
   captured by another thread.  Those nodes are marked MISSED_IN_CAPTURE at the
   moment we skip their capturing.  If such a node (marked MISSED_IN_CAPTURE) is
   being captured by a thread with current atom is in ASTAGE_CAPTURE_WAIT, the
   routine which forces all lock owners to join with current atom is executed.

   Second algorithm does not allow to skip capturing of already captured nodes.

   Both algorithms together prevent waiting a longterm lock without atom fusion
   with atoms of all lock owners, which is a key thing for getting atom/znode
   locking deadlocks.
*/

/*
 * Transactions and mmap(2).
 *
 *     1. Transactions are not supported for accesses through mmap(2), because
 *     this would effectively amount to user-level transactions whose duration
 *     is beyond control of the kernel.
 *
 *     2. That said, we still want to preserve some decency with regard to
 *     mmap(2). During normal write(2) call, following sequence of events
 *     happens:
 *
 *         1. page is created;
 *
 *         2. jnode is created, dirtied and captured into current atom.
 *
 *         3. extent is inserted and modified.
 *
 *     Steps (2) and (3) take place under long term lock on the twig node.
 *
 *     When file is accessed through mmap(2) page is always created during
 *     page fault.
 *     After this (in reiser4_readpage_dispatch()->reiser4_readpage_extent()):
 *
 *         1. if access is made to non-hole page new jnode is created, (if
 *         necessary)
 *
 *         2. if access is made to the hole page, jnode is not created (XXX
 *         not clear why).
 *
 *     Also, even if page is created by write page fault it is not marked
 *     dirty immediately by handle_mm_fault(). Probably this is to avoid races
 *     with page write-out.
 *
 *     Dirty bit installed by hardware is only transferred to the struct page
 *     later, when page is unmapped (in zap_pte_range(), or
 *     try_to_unmap_one()).
 *
 *     So, with mmap(2) we have to handle following irksome situations:
 *
 *         1. there exists modified page (clean or dirty) without jnode
 *
 *         2. there exists modified page (clean or dirty) with clean jnode
 *
 *         3. clean page which is a part of atom can be transparently modified
 *         at any moment through mapping without becoming dirty.
 *
 *     (1) and (2) can lead to the out-of-memory situation: ->writepage()
 *     doesn't know what to do with such pages and ->sync_sb()/->writepages()
 *     don't see them, because these methods operate on atoms.
 *
 *     (3) can lead to the loss of data: suppose we have dirty page with dirty
 *     captured jnode captured by some atom. As part of early flush (for
 *     example) page was written out. Dirty bit was cleared on both page and
 *     jnode. After this page is modified through mapping, but kernel doesn't
 *     notice and just discards page and jnode as part of commit. (XXX
 *     actually it doesn't, because to reclaim page ->releasepage() has to be
 *     called and before this dirty bit will be transferred to the struct
 *     page).
 *
 */

#include "debug.h"
#include "txnmgr.h"
#include "jnode.h"
#include "znode.h"
#include "block_alloc.h"
#include "tree.h"
#include "wander.h"
#include "ktxnmgrd.h"
#include "super.h"
#include "page_cache.h"
#include "reiser4.h"
#include "vfs_ops.h"
#include "inode.h"
#include "flush.h"
#include "discard.h"

#include <asm/atomic.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/writeback.h>
#include <linux/swap.h>		/* for totalram_pages */

static void atom_free(txn_atom * atom);

static int commit_txnh(txn_handle * txnh);

static void wakeup_atom_waitfor_list(txn_atom * atom);
static void wakeup_atom_waiting_list(txn_atom * atom);

static void capture_assign_txnh_nolock(txn_atom * atom, txn_handle * txnh);

static void capture_assign_block_nolock(txn_atom * atom, jnode * node);

static void fuse_not_fused_lock_owners(txn_handle * txnh, znode * node);

static int capture_init_fusion(jnode * node, txn_handle * txnh,
			       txn_capture mode);

static int capture_fuse_wait(txn_handle *, txn_atom *, txn_atom *, txn_capture);

static void capture_fuse_into(txn_atom * small, txn_atom * large);

void reiser4_invalidate_list(struct list_head *);

/* GENERIC STRUCTURES */

typedef struct _txn_wait_links txn_wait_links;

struct _txn_wait_links {
	lock_stack *_lock_stack;
	struct list_head _fwaitfor_link;
	struct list_head _fwaiting_link;
	int (*waitfor_cb) (txn_atom * atom, struct _txn_wait_links * wlinks);
	int (*waiting_cb) (txn_atom * atom, struct _txn_wait_links * wlinks);
};

/* FIXME: In theory, we should be using the slab cache init & destructor
   methods instead of, e.g., jnode_init, etc. */
static struct kmem_cache *_atom_slab = NULL;
/* this is for user-visible, cross system-call transactions. */
static struct kmem_cache *_txnh_slab = NULL;

/**
 * init_txnmgr_static - create transaction manager slab caches
 *
 * Initializes caches of txn-atoms and txn_handle. It is part of reiser4 module
 * initialization.
 */
int init_txnmgr_static(void)
{
	assert("jmacd-600", _atom_slab == NULL);
	assert("jmacd-601", _txnh_slab == NULL);

	ON_DEBUG(atomic_set(&flush_cnt, 0));

	_atom_slab = kmem_cache_create("txn_atom", sizeof(txn_atom), 0,
				       SLAB_HWCACHE_ALIGN |
				       SLAB_RECLAIM_ACCOUNT, NULL);
	if (_atom_slab == NULL)
		return RETERR(-ENOMEM);

	_txnh_slab = kmem_cache_create("txn_handle", sizeof(txn_handle), 0,
			      SLAB_HWCACHE_ALIGN, NULL);
	if (_txnh_slab == NULL) {
		kmem_cache_destroy(_atom_slab);
		_atom_slab = NULL;
		return RETERR(-ENOMEM);
	}

	return 0;
}

/**
 * done_txnmgr_static - delete txn_atom and txn_handle caches
 *
 * This is called on reiser4 module unloading or system shutdown.
 */
void done_txnmgr_static(void)
{
	destroy_reiser4_cache(&_atom_slab);
	destroy_reiser4_cache(&_txnh_slab);
}

/**
 * init_txnmgr - initialize a new transaction manager
 * @mgr: pointer to transaction manager embedded in reiser4 super block
 *
 * This is called on mount. Makes necessary initializations.
 */
void reiser4_init_txnmgr(txn_mgr *mgr)
{
	assert("umka-169", mgr != NULL);

	mgr->atom_count = 0;
	mgr->id_count = 1;
	INIT_LIST_HEAD(&mgr->atoms_list);
	spin_lock_init(&mgr->tmgr_lock);
	mutex_init(&mgr->commit_mutex);
}

/**
 * reiser4_done_txnmgr - stop transaction manager
 * @mgr: pointer to transaction manager embedded in reiser4 super block
 *
 * This is called on umount. Does sanity checks.
 */
void reiser4_done_txnmgr(txn_mgr *mgr)
{
	assert("umka-170", mgr != NULL);
	assert("umka-1701", list_empty_careful(&mgr->atoms_list));
	assert("umka-1702", mgr->atom_count == 0);
}

/* Initialize a transaction handle. */
/* Audited by: umka (2002.06.13) */
static void txnh_init(txn_handle * txnh, txn_mode mode)
{
	assert("umka-171", txnh != NULL);

	txnh->mode = mode;
	txnh->atom = NULL;
	reiser4_ctx_gfp_mask_set();
	txnh->flags = 0;
	spin_lock_init(&txnh->hlock);
	INIT_LIST_HEAD(&txnh->txnh_link);
}

#if REISER4_DEBUG
/* Check if a transaction handle is clean. */
static int txnh_isclean(txn_handle * txnh)
{
	assert("umka-172", txnh != NULL);
	return txnh->atom == NULL &&
		LOCK_CNT_NIL(spin_locked_txnh);
}
#endif

/* Initialize an atom. */
static void atom_init(txn_atom * atom)
{
	int level;

	assert("umka-173", atom != NULL);

	memset(atom, 0, sizeof(txn_atom));

	atom->stage = ASTAGE_FREE;
	atom->start_time = jiffies;

	for (level = 0; level < REAL_MAX_ZTREE_HEIGHT + 1; level += 1)
		INIT_LIST_HEAD(ATOM_DIRTY_LIST(atom, level));

	INIT_LIST_HEAD(ATOM_CLEAN_LIST(atom));
	INIT_LIST_HEAD(ATOM_OVRWR_LIST(atom));
	INIT_LIST_HEAD(ATOM_WB_LIST(atom));
	INIT_LIST_HEAD(&atom->inodes);
	spin_lock_init(&(atom->alock));
	/* list of transaction handles */
	INIT_LIST_HEAD(&atom->txnh_list);
	/* link to transaction manager's list of atoms */
	INIT_LIST_HEAD(&atom->atom_link);
	INIT_LIST_HEAD(&atom->fwaitfor_list);
	INIT_LIST_HEAD(&atom->fwaiting_list);
	blocknr_set_init(&atom->wandered_map);

	atom_dset_init(atom);

	init_atom_fq_parts(atom);
}

#if REISER4_DEBUG
/* Check if an atom is clean. */
static int atom_isclean(txn_atom * atom)
{
	int level;

	assert("umka-174", atom != NULL);

	for (level = 0; level < REAL_MAX_ZTREE_HEIGHT + 1; level += 1) {
		if (!list_empty_careful(ATOM_DIRTY_LIST(atom, level))) {
			return 0;
		}
	}

	return	atom->stage == ASTAGE_FREE &&
		atom->txnh_count == 0 &&
		atom->capture_count == 0 &&
		atomic_read(&atom->refcount) == 0 &&
		(&atom->atom_link == atom->atom_link.next &&
		 &atom->atom_link == atom->atom_link.prev) &&
		list_empty_careful(&atom->txnh_list) &&
		list_empty_careful(ATOM_CLEAN_LIST(atom)) &&
		list_empty_careful(ATOM_OVRWR_LIST(atom)) &&
		list_empty_careful(ATOM_WB_LIST(atom)) &&
		list_empty_careful(&atom->fwaitfor_list) &&
		list_empty_careful(&atom->fwaiting_list) &&
		atom_fq_parts_are_clean(atom);
}
#endif

/* Begin a transaction in this context.  Currently this uses the reiser4_context's
   trans_in_ctx, which means that transaction handles are stack-allocated.  Eventually
   this will be extended to allow transaction handles to span several contexts. */
/* Audited by: umka (2002.06.13) */
void reiser4_txn_begin(reiser4_context * context)
{
	assert("jmacd-544", context->trans == NULL);

	context->trans = &context->trans_in_ctx;

	/* FIXME_LATER_JMACD Currently there's no way to begin a TXN_READ_FUSING
	   transcrash.  Default should be TXN_WRITE_FUSING.  Also, the _trans variable is
	   stack allocated right now, but we would like to allow for dynamically allocated
	   transcrashes that span multiple system calls.
	 */
	txnh_init(context->trans, TXN_WRITE_FUSING);
}

/* Finish a transaction handle context. */
int reiser4_txn_end(reiser4_context * context)
{
	long ret = 0;
	txn_handle *txnh;

	assert("umka-283", context != NULL);
	assert("nikita-3012", reiser4_schedulable());
	assert("vs-24", context == get_current_context());
	assert("nikita-2967", lock_stack_isclean(get_current_lock_stack()));

	txnh = context->trans;
	if (txnh != NULL) {
		if (txnh->atom != NULL)
			ret = commit_txnh(txnh);
		assert("jmacd-633", txnh_isclean(txnh));
		context->trans = NULL;
	}
	return ret;
}

void reiser4_txn_restart(reiser4_context * context)
{
	reiser4_txn_end(context);
	reiser4_preempt_point();
	reiser4_txn_begin(context);
}

void reiser4_txn_restart_current(void)
{
	reiser4_txn_restart(get_current_context());
}

/* TXN_ATOM */

/* Get the atom belonging to a txnh, which is not locked.  Return txnh locked. Locks atom, if atom
   is not NULL.  This performs the necessary spin_trylock to break the lock-ordering cycle.  May
   return NULL. */
static txn_atom *txnh_get_atom(txn_handle * txnh)
{
	txn_atom *atom;

	assert("umka-180", txnh != NULL);
	assert_spin_not_locked(&(txnh->hlock));

	while (1) {
		spin_lock_txnh(txnh);
		atom = txnh->atom;

		if (atom == NULL)
			break;

		if (spin_trylock_atom(atom))
			break;

		atomic_inc(&atom->refcount);

		spin_unlock_txnh(txnh);
		spin_lock_atom(atom);
		spin_lock_txnh(txnh);

		if (txnh->atom == atom) {
			atomic_dec(&atom->refcount);
			break;
		}

		spin_unlock_txnh(txnh);
		atom_dec_and_unlock(atom);
	}

	return atom;
}

/* Get the current atom and spinlock it if current atom present. May return NULL  */
txn_atom *get_current_atom_locked_nocheck(void)
{
	reiser4_context *cx;
	txn_atom *atom;
	txn_handle *txnh;

	cx = get_current_context();
	assert("zam-437", cx != NULL);

	txnh = cx->trans;
	assert("zam-435", txnh != NULL);

	atom = txnh_get_atom(txnh);

	spin_unlock_txnh(txnh);
	return atom;
}

/* Get the atom belonging to a jnode, which is initially locked.  Return with
   both jnode and atom locked.  This performs the necessary spin_trylock to
   break the lock-ordering cycle.  Assumes the jnode is already locked, and
   returns NULL if atom is not set. */
txn_atom *jnode_get_atom(jnode * node)
{
	txn_atom *atom;

	assert("umka-181", node != NULL);

	while (1) {
		assert_spin_locked(&(node->guard));

		atom = node->atom;
		/* node is not in any atom */
		if (atom == NULL)
			break;

		/* If atom is not locked, grab the lock and return */
		if (spin_trylock_atom(atom))
			break;

		/* At least one jnode belongs to this atom it guarantees that
		 * atom->refcount > 0, we can safely increment refcount. */
		atomic_inc(&atom->refcount);
		spin_unlock_jnode(node);

		/* re-acquire spin locks in the right order */
		spin_lock_atom(atom);
		spin_lock_jnode(node);

		/* check if node still points to the same atom. */
		if (node->atom == atom) {
			atomic_dec(&atom->refcount);
			break;
		}

		/* releasing of atom lock and reference requires not holding
		 * locks on jnodes.  */
		spin_unlock_jnode(node);

		/* We do not sure that this atom has extra references except our
		 * one, so we should call proper function which may free atom if
		 * last reference is released. */
		atom_dec_and_unlock(atom);

		/* lock jnode again for getting valid node->atom pointer
		 * value. */
		spin_lock_jnode(node);
	}

	return atom;
}

/* Returns true if @node is dirty and part of the same atom as one of its neighbors.  Used
   by flush code to indicate whether the next node (in some direction) is suitable for
   flushing. */
int
same_slum_check(jnode * node, jnode * check, int alloc_check, int alloc_value)
{
	int compat;
	txn_atom *atom;

	assert("umka-182", node != NULL);
	assert("umka-183", check != NULL);

	/* Not sure what this function is supposed to do if supplied with @check that is
	   neither formatted nor unformatted (bitmap or so). */
	assert("nikita-2373", jnode_is_znode(check)
	       || jnode_is_unformatted(check));

	/* Need a lock on CHECK to get its atom and to check various state bits.
	   Don't need a lock on NODE once we get the atom lock. */
	/* It is not enough to lock two nodes and check (node->atom ==
	   check->atom) because atom could be locked and being fused at that
	   moment, jnodes of the atom of that state (being fused) can point to
	   different objects, but the atom is the same. */
	spin_lock_jnode(check);

	atom = jnode_get_atom(check);

	if (atom == NULL) {
		compat = 0;
	} else {
		compat = (node->atom == atom && JF_ISSET(check, JNODE_DIRTY));

		if (compat && jnode_is_znode(check)) {
			compat &= znode_is_connected(JZNODE(check));
		}

		if (compat && alloc_check) {
			compat &= (alloc_value == jnode_is_flushprepped(check));
		}

		spin_unlock_atom(atom);
	}

	spin_unlock_jnode(check);

	return compat;
}

/* Decrement the atom's reference count and if it falls to zero, free it. */
void atom_dec_and_unlock(txn_atom * atom)
{
	txn_mgr *mgr = &get_super_private(reiser4_get_current_sb())->tmgr;

	assert("umka-186", atom != NULL);
	assert_spin_locked(&(atom->alock));
	assert("zam-1039", atomic_read(&atom->refcount) > 0);

	if (atomic_dec_and_test(&atom->refcount)) {
		/* take txnmgr lock and atom lock in proper order. */
		if (!spin_trylock_txnmgr(mgr)) {
			/* This atom should exist after we re-acquire its
			 * spinlock, so we increment its reference counter. */
			atomic_inc(&atom->refcount);
			spin_unlock_atom(atom);
			spin_lock_txnmgr(mgr);
			spin_lock_atom(atom);

			if (!atomic_dec_and_test(&atom->refcount)) {
				spin_unlock_atom(atom);
				spin_unlock_txnmgr(mgr);
				return;
			}
		}
		assert_spin_locked(&(mgr->tmgr_lock));
		atom_free(atom);
		spin_unlock_txnmgr(mgr);
	} else
		spin_unlock_atom(atom);
}

/* Create new atom and connect it to given transaction handle.  This adds the
   atom to the transaction manager's list and sets its reference count to 1, an
   artificial reference which is kept until it commits.  We play strange games
   to avoid allocation under jnode & txnh spinlocks.*/

static int atom_begin_and_assign_to_txnh(txn_atom ** atom_alloc, txn_handle * txnh)
{
	txn_atom *atom;
	txn_mgr *mgr;

	if (REISER4_DEBUG && rofs_tree(current_tree)) {
		warning("nikita-3366", "Creating atom on rofs");
		dump_stack();
	}

	if (*atom_alloc == NULL) {
		(*atom_alloc) = kmem_cache_alloc(_atom_slab,
						 reiser4_ctx_gfp_mask_get());

		if (*atom_alloc == NULL)
			return RETERR(-ENOMEM);
	}

	/* and, also, txnmgr spin lock should be taken before jnode and txnh
	   locks. */
	mgr = &get_super_private(reiser4_get_current_sb())->tmgr;
	spin_lock_txnmgr(mgr);
	spin_lock_txnh(txnh);

	/* Check whether new atom still needed */
	if (txnh->atom != NULL) {
		/* NOTE-NIKITA probably it is rather better to free
		 * atom_alloc here than thread it up to reiser4_try_capture() */

		spin_unlock_txnh(txnh);
		spin_unlock_txnmgr(mgr);

		return -E_REPEAT;
	}

	atom = *atom_alloc;
	*atom_alloc = NULL;

	atom_init(atom);

	assert("jmacd-17", atom_isclean(atom));

        /*
	 * lock ordering is broken here. It is ok, as long as @atom is new
	 * and inaccessible for others. We can't use spin_lock_atom or
	 * spin_lock(&atom->alock) because they care about locking
	 * dependencies. spin_trylock_lock doesn't.
	 */
	check_me("", spin_trylock_atom(atom));

	/* add atom to the end of transaction manager's list of atoms */
	list_add_tail(&atom->atom_link, &mgr->atoms_list);
	atom->atom_id = mgr->id_count++;
	mgr->atom_count += 1;

	/* Release txnmgr lock */
	spin_unlock_txnmgr(mgr);

	/* One reference until it commits. */
	atomic_inc(&atom->refcount);
	atom->stage = ASTAGE_CAPTURE_FUSE;
	atom->super = reiser4_get_current_sb();
	capture_assign_txnh_nolock(atom, txnh);

	spin_unlock_atom(atom);
	spin_unlock_txnh(txnh);

	return -E_REPEAT;
}

/* Return true if an atom is currently "open". */
static int atom_isopen(const txn_atom * atom)
{
	assert("umka-185", atom != NULL);

	return atom->stage > 0 && atom->stage < ASTAGE_PRE_COMMIT;
}

/* Return the number of pointers to this atom that must be updated during fusion.  This
   approximates the amount of work to be done.  Fusion chooses the atom with fewer
   pointers to fuse into the atom with more pointers. */
static int atom_pointer_count(const txn_atom * atom)
{
	assert("umka-187", atom != NULL);

	/* This is a measure of the amount of work needed to fuse this atom
	 * into another. */
	return atom->txnh_count + atom->capture_count;
}

/* Called holding the atom lock, this removes the atom from the transaction manager list
   and frees it. */
static void atom_free(txn_atom * atom)
{
	txn_mgr *mgr = &get_super_private(reiser4_get_current_sb())->tmgr;

	assert("umka-188", atom != NULL);
	assert_spin_locked(&(atom->alock));

	/* Remove from the txn_mgr's atom list */
	assert_spin_locked(&(mgr->tmgr_lock));
	mgr->atom_count -= 1;
	list_del_init(&atom->atom_link);

	/* Clean the atom */
	assert("jmacd-16",
	       (atom->stage == ASTAGE_INVALID || atom->stage == ASTAGE_DONE));
	atom->stage = ASTAGE_FREE;

	blocknr_set_destroy(&atom->wandered_map);

	atom_dset_destroy(atom);

	assert("jmacd-16", atom_isclean(atom));

	spin_unlock_atom(atom);

	kmem_cache_free(_atom_slab, atom);
}

static int atom_is_dotard(const txn_atom * atom)
{
	return time_after(jiffies, atom->start_time +
			  get_current_super_private()->tmgr.atom_max_age);
}

static int atom_can_be_committed(txn_atom * atom)
{
	assert_spin_locked(&(atom->alock));
	assert("zam-885", atom->txnh_count > atom->nr_waiters);
	return atom->txnh_count == atom->nr_waiters + 1;
}

/* Return true if an atom should commit now.  This is determined by aging, atom
   size or atom flags. */
static int atom_should_commit(const txn_atom * atom)
{
	assert("umka-189", atom != NULL);
	return
	    (atom->flags & ATOM_FORCE_COMMIT) ||
	    ((unsigned)atom_pointer_count(atom) >
	     get_current_super_private()->tmgr.atom_max_size)
	    || atom_is_dotard(atom);
}

/* return 1 if current atom exists and requires commit. */
int current_atom_should_commit(void)
{
	txn_atom *atom;
	int result = 0;

	atom = get_current_atom_locked_nocheck();
	if (atom) {
		result = atom_should_commit(atom);
		spin_unlock_atom(atom);
	}
	return result;
}

static int atom_should_commit_asap(const txn_atom * atom)
{
	unsigned int captured;
	unsigned int pinnedpages;

	assert("nikita-3309", atom != NULL);

	captured = (unsigned)atom->capture_count;
	pinnedpages = (captured >> PAGE_CACHE_SHIFT) * sizeof(znode);

	return (pinnedpages > (totalram_pages >> 3)) || (atom->flushed > 100);
}

static jnode *find_first_dirty_in_list(struct list_head *head, int flags)
{
	jnode *first_dirty;

	list_for_each_entry(first_dirty, head, capture_link) {
		if (!(flags & JNODE_FLUSH_COMMIT)) {
			/*
			 * skip jnodes which "heard banshee" or having active
			 * I/O
			 */
			if (JF_ISSET(first_dirty, JNODE_HEARD_BANSHEE) ||
			    JF_ISSET(first_dirty, JNODE_WRITEBACK))
				continue;
		}
		return first_dirty;
	}
	return NULL;
}

/* Get first dirty node from the atom's dirty_nodes[n] lists; return NULL if atom has no dirty
   nodes on atom's lists */
jnode *find_first_dirty_jnode(txn_atom * atom, int flags)
{
	jnode *first_dirty;
	tree_level level;

	assert_spin_locked(&(atom->alock));

	/* The flush starts from LEAF_LEVEL (=1). */
	for (level = 1; level < REAL_MAX_ZTREE_HEIGHT + 1; level += 1) {
		if (list_empty_careful(ATOM_DIRTY_LIST(atom, level)))
			continue;

		first_dirty =
		    find_first_dirty_in_list(ATOM_DIRTY_LIST(atom, level),
					     flags);
		if (first_dirty)
			return first_dirty;
	}

	/* znode-above-root is on the list #0. */
	return find_first_dirty_in_list(ATOM_DIRTY_LIST(atom, 0), flags);
}

static void dispatch_wb_list(txn_atom * atom, flush_queue_t * fq)
{
	jnode *cur;

	assert("zam-905", atom_is_protected(atom));

	cur = list_entry(ATOM_WB_LIST(atom)->next, jnode, capture_link);
	while (ATOM_WB_LIST(atom) != &cur->capture_link) {
		jnode *next = list_entry(cur->capture_link.next, jnode, capture_link);

		spin_lock_jnode(cur);
		if (!JF_ISSET(cur, JNODE_WRITEBACK)) {
			if (JF_ISSET(cur, JNODE_DIRTY)) {
				queue_jnode(fq, cur);
			} else {
				/* move jnode to atom's clean list */
				list_move_tail(&cur->capture_link,
					      ATOM_CLEAN_LIST(atom));
			}
		}
		spin_unlock_jnode(cur);

		cur = next;
	}
}

/* Scan current atom->writeback_nodes list, re-submit dirty and !writeback
 * jnodes to disk. */
static int submit_wb_list(void)
{
	int ret;
	flush_queue_t *fq;

	fq = get_fq_for_current_atom();
	if (IS_ERR(fq))
		return PTR_ERR(fq);

	dispatch_wb_list(fq->atom, fq);
	spin_unlock_atom(fq->atom);

	ret = reiser4_write_fq(fq, NULL, 1);
	reiser4_fq_put(fq);

	return ret;
}

/* Wait completion of all writes, re-submit atom writeback list if needed. */
static int current_atom_complete_writes(void)
{
	int ret;

	/* Each jnode from that list was modified and dirtied when it had i/o
	 * request running already. After i/o completion we have to resubmit
	 * them to disk again.*/
	ret = submit_wb_list();
	if (ret < 0)
		return ret;

	/* Wait all i/o completion */
	ret = current_atom_finish_all_fq();
	if (ret)
		return ret;

	/* Scan wb list again; all i/o should be completed, we re-submit dirty
	 * nodes to disk */
	ret = submit_wb_list();
	if (ret < 0)
		return ret;

	/* Wait all nodes we just submitted */
	return current_atom_finish_all_fq();
}

#if REISER4_DEBUG

static void reiser4_info_atom(const char *prefix, const txn_atom * atom)
{
	if (atom == NULL) {
		printk("%s: no atom\n", prefix);
		return;
	}

	printk("%s: refcount: %i id: %i flags: %x txnh_count: %i"
	       " capture_count: %i stage: %x start: %lu, flushed: %i\n", prefix,
	       atomic_read(&atom->refcount), atom->atom_id, atom->flags,
	       atom->txnh_count, atom->capture_count, atom->stage,
	       atom->start_time, atom->flushed);
}

#else  /*  REISER4_DEBUG  */

static inline void reiser4_info_atom(const char *prefix, const txn_atom * atom) {}

#endif  /*  REISER4_DEBUG  */

#define TOOMANYFLUSHES (1 << 13)

/* Called with the atom locked and no open "active" transaction handlers except
   ours, this function calls flush_current_atom() until all dirty nodes are
   processed.  Then it initiates commit processing.

   Called by the single remaining open "active" txnh, which is closing. Other
   open txnhs belong to processes which wait atom commit in commit_txnh()
   routine. They are counted as "waiters" in atom->nr_waiters.  Therefore as
   long as we hold the atom lock none of the jnodes can be captured and/or
   locked.

   Return value is an error code if commit fails.
*/
static int commit_current_atom(long *nr_submitted, txn_atom ** atom)
{
	reiser4_super_info_data *sbinfo = get_current_super_private();
	long ret = 0;
	/* how many times jnode_flush() was called as a part of attempt to
	 * commit this atom. */
	int flushiters;

	assert("zam-888", atom != NULL && *atom != NULL);
	assert_spin_locked(&((*atom)->alock));
	assert("zam-887", get_current_context()->trans->atom == *atom);
	assert("jmacd-151", atom_isopen(*atom));

	assert("nikita-3184",
	       get_current_super_private()->delete_mutex_owner != current);

	for (flushiters = 0;; ++flushiters) {
		ret =
		    flush_current_atom(JNODE_FLUSH_WRITE_BLOCKS |
				       JNODE_FLUSH_COMMIT,
				       LONG_MAX /* nr_to_write */ ,
				       nr_submitted, atom, NULL);
		if (ret != -E_REPEAT)
			break;

		/* if atom's dirty list contains one znode which is
		   HEARD_BANSHEE and is locked we have to allow lock owner to
		   continue and uncapture that znode */
		reiser4_preempt_point();

		*atom = get_current_atom_locked();
		if (flushiters > TOOMANYFLUSHES && IS_POW(flushiters)) {
			warning("nikita-3176",
				"Flushing like mad: %i", flushiters);
			reiser4_info_atom("atom", *atom);
			DEBUGON(flushiters > (1 << 20));
		}
	}

	if (ret)
		return ret;

	assert_spin_locked(&((*atom)->alock));

	if (!atom_can_be_committed(*atom)) {
		spin_unlock_atom(*atom);
		return RETERR(-E_REPEAT);
	}

	if ((*atom)->capture_count == 0)
		goto done;

	/* Up to this point we have been flushing and after flush is called we
	   return -E_REPEAT.  Now we can commit.  We cannot return -E_REPEAT
	   at this point, commit should be successful. */
	reiser4_atom_set_stage(*atom, ASTAGE_PRE_COMMIT);
	ON_DEBUG(((*atom)->committer = current));
	spin_unlock_atom(*atom);

	ret = current_atom_complete_writes();
	if (ret)
		return ret;

	assert("zam-906", list_empty(ATOM_WB_LIST(*atom)));

	/* isolate critical code path which should be executed by only one
	 * thread using tmgr mutex */
	mutex_lock(&sbinfo->tmgr.commit_mutex);

	ret = reiser4_write_logs(nr_submitted);
	if (ret < 0)
		reiser4_panic("zam-597", "write log failed (%ld)\n", ret);

	/* The atom->ovrwr_nodes list is processed under commit mutex held
	   because of bitmap nodes which are captured by special way in
	   reiser4_pre_commit_hook_bitmap(), that way does not include
	   capture_fuse_wait() as a capturing of other nodes does -- the commit
	   mutex is used for transaction isolation instead. */
	reiser4_invalidate_list(ATOM_OVRWR_LIST(*atom));
	mutex_unlock(&sbinfo->tmgr.commit_mutex);

	reiser4_invalidate_list(ATOM_CLEAN_LIST(*atom));
	reiser4_invalidate_list(ATOM_WB_LIST(*atom));
	assert("zam-927", list_empty(&(*atom)->inodes));

	spin_lock_atom(*atom);
 done:
	reiser4_atom_set_stage(*atom, ASTAGE_DONE);
	ON_DEBUG((*atom)->committer = NULL);

	/* Atom's state changes, so wake up everybody waiting for this
	   event. */
	wakeup_atom_waiting_list(*atom);

	/* Decrement the "until commit" reference, at least one txnh (the caller) is
	   still open. */
	atomic_dec(&(*atom)->refcount);

	assert("jmacd-1070", atomic_read(&(*atom)->refcount) > 0);
	assert("jmacd-1062", (*atom)->capture_count == 0);
	BUG_ON((*atom)->capture_count != 0);
	assert_spin_locked(&((*atom)->alock));

	return ret;
}

/* TXN_TXNH */

/**
 * force_commit_atom - commit current atom and wait commit completion
 * @txnh:
 *
 * Commits current atom and wait commit completion; current atom and @txnh have
 * to be spinlocked before call, this function unlocks them on exit.
 */
int force_commit_atom(txn_handle *txnh)
{
	txn_atom *atom;

	assert("zam-837", txnh != NULL);
	assert_spin_locked(&(txnh->hlock));
	assert("nikita-2966", lock_stack_isclean(get_current_lock_stack()));

	atom = txnh->atom;

	assert("zam-834", atom != NULL);
	assert_spin_locked(&(atom->alock));

	/*
	 * Set flags for atom and txnh: forcing atom commit and waiting for
	 * commit completion
	 */
	txnh->flags |= TXNH_WAIT_COMMIT;
	atom->flags |= ATOM_FORCE_COMMIT;

	spin_unlock_txnh(txnh);
	spin_unlock_atom(atom);

	/* commit is here */
	reiser4_txn_restart_current();
	return 0;
}

/* Called to force commit of any outstanding atoms.  @commit_all_atoms controls
 * should we commit all atoms including new ones which are created after this
 * functions is called. */
int txnmgr_force_commit_all(struct super_block *super, int commit_all_atoms)
{
	int ret;
	txn_atom *atom;
	txn_mgr *mgr;
	txn_handle *txnh;
	unsigned long start_time = jiffies;
	reiser4_context *ctx = get_current_context();

	assert("nikita-2965", lock_stack_isclean(get_current_lock_stack()));
	assert("nikita-3058", reiser4_commit_check_locks());

	reiser4_txn_restart_current();

	mgr = &get_super_private(super)->tmgr;

	txnh = ctx->trans;

      again:

	spin_lock_txnmgr(mgr);

	list_for_each_entry(atom, &mgr->atoms_list, atom_link) {
		spin_lock_atom(atom);

		/* Commit any atom which can be committed.  If @commit_new_atoms
		 * is not set we commit only atoms which were created before
		 * this call is started. */
		if (commit_all_atoms
		    || time_before_eq(atom->start_time, start_time)) {
			if (atom->stage <= ASTAGE_POST_COMMIT) {
				spin_unlock_txnmgr(mgr);

				if (atom->stage < ASTAGE_PRE_COMMIT) {
					spin_lock_txnh(txnh);
					/* Add force-context txnh */
					capture_assign_txnh_nolock(atom, txnh);
					ret = force_commit_atom(txnh);
					if (ret)
						return ret;
				} else
					/* wait atom commit */
					reiser4_atom_wait_event(atom);

				goto again;
			}
		}

		spin_unlock_atom(atom);
	}

#if REISER4_DEBUG
	if (commit_all_atoms) {
		reiser4_super_info_data *sbinfo = get_super_private(super);
		spin_lock_reiser4_super(sbinfo);
		assert("zam-813",
		       sbinfo->blocks_fake_allocated_unformatted == 0);
		assert("zam-812", sbinfo->blocks_fake_allocated == 0);
		spin_unlock_reiser4_super(sbinfo);
	}
#endif

	spin_unlock_txnmgr(mgr);

	return 0;
}

/* check whether commit_some_atoms() can commit @atom. Locking is up to the
 * caller */
static int atom_is_committable(txn_atom * atom)
{
	return
	    atom->stage < ASTAGE_PRE_COMMIT &&
	    atom->txnh_count == atom->nr_waiters && atom_should_commit(atom);
}

/* called periodically from ktxnmgrd to commit old atoms. Releases ktxnmgrd spin
 * lock at exit */
int commit_some_atoms(txn_mgr * mgr)
{
	int ret = 0;
	txn_atom *atom;
	txn_handle *txnh;
	reiser4_context *ctx;
	struct list_head *pos, *tmp;

	ctx = get_current_context();
	assert("nikita-2444", ctx != NULL);

	txnh = ctx->trans;
	spin_lock_txnmgr(mgr);

	/*
	 * this is to avoid gcc complain that atom might be used
	 * uninitialized
	 */
	atom = NULL;

	/* look for atom to commit */
	list_for_each_safe(pos, tmp, &mgr->atoms_list) {
		atom = list_entry(pos, txn_atom, atom_link);
		/*
		 * first test without taking atom spin lock, whether it is
		 * eligible for committing at all
		 */
		if (atom_is_committable(atom)) {
			/* now, take spin lock and re-check */
			spin_lock_atom(atom);
			if (atom_is_committable(atom))
				break;
			spin_unlock_atom(atom);
		}
	}

	ret = (&mgr->atoms_list == pos);
	spin_unlock_txnmgr(mgr);

	if (ret) {
		/* nothing found */
		spin_unlock(&mgr->daemon->guard);
		return 0;
	}

	spin_lock_txnh(txnh);

	BUG_ON(atom == NULL);
	/* Set the atom to force committing */
	atom->flags |= ATOM_FORCE_COMMIT;

	/* Add force-context txnh */
	capture_assign_txnh_nolock(atom, txnh);

	spin_unlock_txnh(txnh);
	spin_unlock_atom(atom);

	/* we are about to release daemon spin lock, notify daemon it
	   has to rescan atoms */
	mgr->daemon->rescan = 1;
	spin_unlock(&mgr->daemon->guard);
	reiser4_txn_restart_current();
	return 0;
}

static int txn_try_to_fuse_small_atom(txn_mgr * tmgr, txn_atom * atom)
{
	int atom_stage;
	txn_atom *atom_2;
	int repeat;

	assert("zam-1051", atom->stage < ASTAGE_PRE_COMMIT);

	atom_stage = atom->stage;
	repeat = 0;

	if (!spin_trylock_txnmgr(tmgr)) {
		atomic_inc(&atom->refcount);
		spin_unlock_atom(atom);
		spin_lock_txnmgr(tmgr);
		spin_lock_atom(atom);
		repeat = 1;
		if (atom->stage != atom_stage) {
			spin_unlock_txnmgr(tmgr);
			atom_dec_and_unlock(atom);
			return -E_REPEAT;
		}
		atomic_dec(&atom->refcount);
	}

	list_for_each_entry(atom_2, &tmgr->atoms_list, atom_link) {
		if (atom == atom_2)
			continue;
		/*
		 * if trylock does not succeed we just do not fuse with that
		 * atom.
		 */
		if (spin_trylock_atom(atom_2)) {
			if (atom_2->stage < ASTAGE_PRE_COMMIT) {
				spin_unlock_txnmgr(tmgr);
				capture_fuse_into(atom_2, atom);
				/* all locks are lost we can only repeat here */
				return -E_REPEAT;
			}
			spin_unlock_atom(atom_2);
		}
	}
	atom->flags |= ATOM_CANCEL_FUSION;
	spin_unlock_txnmgr(tmgr);
	if (repeat) {
		spin_unlock_atom(atom);
		return -E_REPEAT;
	}
	return 0;
}

/* Calls jnode_flush for current atom if it exists; if not, just take another
   atom and call jnode_flush() for him.  If current transaction handle has
   already assigned atom (current atom) we have to close current transaction
   prior to switch to another atom or do something with current atom. This
   code tries to flush current atom.

   flush_some_atom() is called as part of memory clearing process. It is
   invoked from balance_dirty_pages(), pdflushd, and entd.

   If we can flush no nodes, atom is committed, because this frees memory.

   If atom is too large or too old it is committed also.
*/
int
flush_some_atom(jnode * start, long *nr_submitted, const struct writeback_control *wbc,
		int flags)
{
	reiser4_context *ctx = get_current_context();
	txn_mgr *tmgr = &get_super_private(ctx->super)->tmgr;
	txn_handle *txnh = ctx->trans;
	txn_atom *atom;
	int ret;

	BUG_ON(wbc->nr_to_write == 0);
	BUG_ON(*nr_submitted != 0);
	assert("zam-1042", txnh != NULL);
repeat:
	if (txnh->atom == NULL) {
		/* current atom is not available, take first from txnmgr */
		spin_lock_txnmgr(tmgr);

		/* traverse the list of all atoms */
		list_for_each_entry(atom, &tmgr->atoms_list, atom_link) {
			/* lock atom before checking its state */
			spin_lock_atom(atom);

			/*
			 * we need an atom which is not being committed and
			 * which has no flushers (jnode_flush() add one flusher
			 * at the beginning and subtract one at the end).
			 */
			if (atom->stage < ASTAGE_PRE_COMMIT &&
			    atom->nr_flushers == 0) {
				spin_lock_txnh(txnh);
				capture_assign_txnh_nolock(atom, txnh);
				spin_unlock_txnh(txnh);

				goto found;
			}

			spin_unlock_atom(atom);
		}

		/*
		 * Write throttling is case of no one atom can be
		 * flushed/committed.
		 */
		if (!current_is_flush_bd_task()) {
			list_for_each_entry(atom, &tmgr->atoms_list, atom_link) {
				spin_lock_atom(atom);
				/* Repeat the check from the above. */
				if (atom->stage < ASTAGE_PRE_COMMIT
				    && atom->nr_flushers == 0) {
					spin_lock_txnh(txnh);
					capture_assign_txnh_nolock(atom, txnh);
					spin_unlock_txnh(txnh);

					goto found;
				}
				if (atom->stage <= ASTAGE_POST_COMMIT) {
					spin_unlock_txnmgr(tmgr);
					/*
					 * we just wait until atom's flusher
					 * makes a progress in flushing or
					 * committing the atom
					 */
					reiser4_atom_wait_event(atom);
					goto repeat;
				}
				spin_unlock_atom(atom);
			}
		}
		spin_unlock_txnmgr(tmgr);
		return 0;
	      found:
		spin_unlock_txnmgr(tmgr);
	} else
		atom = get_current_atom_locked();

	BUG_ON(atom->super != ctx->super);
	assert("vs-35", atom->super == ctx->super);
	if (start) {
		spin_lock_jnode(start);
		ret = (atom == start->atom) ? 1 : 0;
		spin_unlock_jnode(start);
		if (ret == 0)
			start = NULL;
	}
	ret = flush_current_atom(flags, wbc->nr_to_write, nr_submitted, &atom, start);
	if (ret == 0) {
		/* flush_current_atom returns 0 only if it submitted for write
		   nothing */
		BUG_ON(*nr_submitted != 0);
		if (*nr_submitted == 0 || atom_should_commit_asap(atom)) {
			if (atom->capture_count < tmgr->atom_min_size &&
			    !(atom->flags & ATOM_CANCEL_FUSION)) {
				ret = txn_try_to_fuse_small_atom(tmgr, atom);
				if (ret == -E_REPEAT) {
					reiser4_preempt_point();
					goto repeat;
				}
			}
			/* if early flushing could not make more nodes clean,
			 * or atom is too old/large,
			 * we force current atom to commit */
			/* wait for commit completion but only if this
			 * wouldn't stall pdflushd and ent thread. */
			if (!ctx->entd)
				txnh->flags |= TXNH_WAIT_COMMIT;
			atom->flags |= ATOM_FORCE_COMMIT;
		}
		spin_unlock_atom(atom);
	} else if (ret == -E_REPEAT) {
		if (*nr_submitted == 0) {
			/* let others who hampers flushing (hold longterm locks,
			   for instance) to free the way for flush */
			reiser4_preempt_point();
			goto repeat;
		}
		ret = 0;
	}
/*
	if (*nr_submitted > wbc->nr_to_write)
		warning("", "asked for %ld, written %ld\n", wbc->nr_to_write, *nr_submitted);
*/
	reiser4_txn_restart(ctx);

	return ret;
}

/* Remove processed nodes from atom's clean list (thereby remove them from transaction). */
void reiser4_invalidate_list(struct list_head *head)
{
	while (!list_empty(head)) {
		jnode *node;

		node = list_entry(head->next, jnode, capture_link);
		spin_lock_jnode(node);
		reiser4_uncapture_block(node);
		jput(node);
	}
}

static void init_wlinks(txn_wait_links * wlinks)
{
	wlinks->_lock_stack = get_current_lock_stack();
	INIT_LIST_HEAD(&wlinks->_fwaitfor_link);
	INIT_LIST_HEAD(&wlinks->_fwaiting_link);
	wlinks->waitfor_cb = NULL;
	wlinks->waiting_cb = NULL;
}

/* Add atom to the atom's waitfor list and wait for somebody to wake us up; */
void reiser4_atom_wait_event(txn_atom * atom)
{
	txn_wait_links _wlinks;

	assert_spin_locked(&(atom->alock));
	assert("nikita-3156",
	       lock_stack_isclean(get_current_lock_stack()) ||
	       atom->nr_running_queues > 0);

	init_wlinks(&_wlinks);
	list_add_tail(&_wlinks._fwaitfor_link, &atom->fwaitfor_list);
	atomic_inc(&atom->refcount);
	spin_unlock_atom(atom);

	reiser4_prepare_to_sleep(_wlinks._lock_stack);
	reiser4_go_to_sleep(_wlinks._lock_stack);

	spin_lock_atom(atom);
	list_del(&_wlinks._fwaitfor_link);
	atom_dec_and_unlock(atom);
}

void reiser4_atom_set_stage(txn_atom * atom, txn_stage stage)
{
	assert("nikita-3535", atom != NULL);
	assert_spin_locked(&(atom->alock));
	assert("nikita-3536", stage <= ASTAGE_INVALID);
	/* Excelsior! */
	assert("nikita-3537", stage >= atom->stage);
	if (atom->stage != stage) {
		atom->stage = stage;
		reiser4_atom_send_event(atom);
	}
}

/* wake all threads which wait for an event */
void reiser4_atom_send_event(txn_atom * atom)
{
	assert_spin_locked(&(atom->alock));
	wakeup_atom_waitfor_list(atom);
}

/* Informs txn manager code that owner of this txn_handle should wait atom commit completion (for
   example, because it does fsync(2)) */
static int should_wait_commit(txn_handle * h)
{
	return h->flags & TXNH_WAIT_COMMIT;
}

typedef struct commit_data {
	txn_atom *atom;
	txn_handle *txnh;
	long nr_written;
	/* as an optimization we start committing atom by first trying to
	 * flush it few times without switching into ASTAGE_CAPTURE_WAIT. This
	 * allows to reduce stalls due to other threads waiting for atom in
	 * ASTAGE_CAPTURE_WAIT stage. ->preflush is counter of these
	 * preliminary flushes. */
	int preflush;
	/* have we waited on atom. */
	int wait;
	int failed;
	int wake_ktxnmgrd_up;
} commit_data;

/*
 * Called from commit_txnh() repeatedly, until either error happens, or atom
 * commits successfully.
 */
static int try_commit_txnh(commit_data * cd)
{
	int result;

	assert("nikita-2968", lock_stack_isclean(get_current_lock_stack()));

	/* Get the atom and txnh locked. */
	cd->atom = txnh_get_atom(cd->txnh);
	assert("jmacd-309", cd->atom != NULL);
	spin_unlock_txnh(cd->txnh);

	if (cd->wait) {
		cd->atom->nr_waiters--;
		cd->wait = 0;
	}

	if (cd->atom->stage == ASTAGE_DONE)
		return 0;

	if (cd->failed)
		return 0;

	if (atom_should_commit(cd->atom)) {
		/* if atom is _very_ large schedule it for commit as soon as
		 * possible. */
		if (atom_should_commit_asap(cd->atom)) {
			/*
			 * When atom is in PRE_COMMIT or later stage following
			 * invariant (encoded   in    atom_can_be_committed())
			 * holds:  there is exactly one non-waiter transaction
			 * handle opened  on this atom.  When  thread wants to
			 * wait  until atom  commits (for  example  sync()) it
			 * waits    on    atom  event     after     increasing
			 * atom->nr_waiters (see blow  in  this  function). It
			 * cannot be guaranteed that atom is already committed
			 * after    receiving event,  so     loop has   to  be
			 * re-started. But  if  atom switched into  PRE_COMMIT
			 * stage and became  too  large, we cannot  change its
			 * state back   to CAPTURE_WAIT (atom  stage can  only
			 * increase monotonically), hence this check.
			 */
			if (cd->atom->stage < ASTAGE_CAPTURE_WAIT)
				reiser4_atom_set_stage(cd->atom,
						       ASTAGE_CAPTURE_WAIT);
			cd->atom->flags |= ATOM_FORCE_COMMIT;
		}
		if (cd->txnh->flags & TXNH_DONT_COMMIT) {
			/*
			 * this  thread (transaction  handle  that is) doesn't
			 * want to commit  atom. Notify waiters that handle is
			 * closed. This can happen, for  example, when we  are
			 * under  VFS directory lock  and don't want to commit
			 * atom  right   now to  avoid  stalling other threads
			 * working in the same directory.
			 */

			/* Wake  the ktxnmgrd up if  the ktxnmgrd is needed to
			 * commit this  atom: no  atom  waiters  and only  one
			 * (our) open transaction handle. */
			cd->wake_ktxnmgrd_up =
			    cd->atom->txnh_count == 1 &&
			    cd->atom->nr_waiters == 0;
			reiser4_atom_send_event(cd->atom);
			result = 0;
		} else if (!atom_can_be_committed(cd->atom)) {
			if (should_wait_commit(cd->txnh)) {
				/* sync(): wait for commit */
				cd->atom->nr_waiters++;
				cd->wait = 1;
				reiser4_atom_wait_event(cd->atom);
				result = RETERR(-E_REPEAT);
			} else {
				result = 0;
			}
		} else if (cd->preflush > 0 && !is_current_ktxnmgrd()) {
			/*
			 * optimization: flush  atom without switching it into
			 * ASTAGE_CAPTURE_WAIT.
			 *
			 * But don't  do this for  ktxnmgrd, because  ktxnmgrd
			 * should never block on atom fusion.
			 */
			result = flush_current_atom(JNODE_FLUSH_WRITE_BLOCKS,
						    LONG_MAX, &cd->nr_written,
						    &cd->atom, NULL);
			if (result == 0) {
				spin_unlock_atom(cd->atom);
				cd->preflush = 0;
				result = RETERR(-E_REPEAT);
			} else	/* Atoms wasn't flushed
				 * completely. Rinse. Repeat. */
				--cd->preflush;
		} else {
			/* We change   atom state  to   ASTAGE_CAPTURE_WAIT to
			   prevent atom fusion and count  ourself as an active
			   flusher */
			reiser4_atom_set_stage(cd->atom, ASTAGE_CAPTURE_WAIT);
			cd->atom->flags |= ATOM_FORCE_COMMIT;

			result =
			    commit_current_atom(&cd->nr_written, &cd->atom);
			if (result != 0 && result != -E_REPEAT)
				cd->failed = 1;
		}
	} else
		result = 0;

#if REISER4_DEBUG
	if (result == 0)
		assert_spin_locked(&(cd->atom->alock));
#endif

	/* perfectly valid assertion, except that when atom/txnh is not locked
	 * fusion can take place, and cd->atom points nowhere. */
	/*
	   assert("jmacd-1028", ergo(result != 0, spin_atom_is_not_locked(cd->atom)));
	 */
	return result;
}

/* Called to commit a transaction handle.  This decrements the atom's number of open
   handles and if it is the last handle to commit and the atom should commit, initiates
   atom commit. if commit does not fail, return number of written blocks */
static int commit_txnh(txn_handle * txnh)
{
	commit_data cd;
	assert("umka-192", txnh != NULL);

	memset(&cd, 0, sizeof cd);
	cd.txnh = txnh;
	cd.preflush = 10;

	/* calls try_commit_txnh() until either atom commits, or error
	 * happens */
	while (try_commit_txnh(&cd) != 0)
		reiser4_preempt_point();

	spin_lock_txnh(txnh);

	cd.atom->txnh_count -= 1;
	txnh->atom = NULL;
	/* remove transaction handle from atom's list of transaction handles */
	list_del_init(&txnh->txnh_link);

	spin_unlock_txnh(txnh);
	atom_dec_and_unlock(cd.atom);
	/* if we don't want to do a commit (TXNH_DONT_COMMIT is set, probably
	 * because it takes time) by current thread, we do that work
	 * asynchronously by ktxnmgrd daemon. */
	if (cd.wake_ktxnmgrd_up)
		ktxnmgrd_kick(&get_current_super_private()->tmgr);

	return 0;
}

/* TRY_CAPTURE */

/* This routine attempts a single block-capture request.  It may return -E_REPEAT if some
   condition indicates that the request should be retried, and it may block if the
   txn_capture mode does not include the TXN_CAPTURE_NONBLOCKING request flag.

   This routine encodes the basic logic of block capturing described by:

     http://namesys.com/v4/v4.html

   Our goal here is to ensure that any two blocks that contain dependent modifications
   should commit at the same time.  This function enforces this discipline by initiating
   fusion whenever a transaction handle belonging to one atom requests to read or write a
   block belonging to another atom (TXN_CAPTURE_WRITE or TXN_CAPTURE_READ_ATOMIC).

   In addition, this routine handles the initial assignment of atoms to blocks and
   transaction handles.  These are possible outcomes of this function:

   1. The block and handle are already part of the same atom: return immediate success

   2. The block is assigned but the handle is not: call capture_assign_txnh to assign
      the handle to the block's atom.

   3. The handle is assigned but the block is not: call capture_assign_block to assign
      the block to the handle's atom.

   4. Both handle and block are assigned, but to different atoms: call capture_init_fusion
      to fuse atoms.

   5. Neither block nor handle are assigned: create a new atom and assign them both.

   6. A read request for a non-captured block: return immediate success.

   This function acquires and releases the handle's spinlock.  This function is called
   under the jnode lock and if the return value is 0, it returns with the jnode lock still
   held.  If the return is -E_REPEAT or some other error condition, the jnode lock is
   released.  The external interface (reiser4_try_capture) manages re-aquiring the jnode
   lock in the failure case.
*/
static int try_capture_block(
	txn_handle * txnh, jnode * node, txn_capture mode,
	txn_atom ** atom_alloc)
{
	txn_atom *block_atom;
	txn_atom *txnh_atom;

	/* Should not call capture for READ_NONCOM requests, handled in reiser4_try_capture. */
	assert("jmacd-567", CAPTURE_TYPE(mode) != TXN_CAPTURE_READ_NONCOM);

	/* FIXME-ZAM-HANS: FIXME_LATER_JMACD Should assert that atom->tree ==
	 * node->tree somewhere. */
	assert("umka-194", txnh != NULL);
	assert("umka-195", node != NULL);

	/* The jnode is already locked!  Being called from reiser4_try_capture(). */
	assert_spin_locked(&(node->guard));
	block_atom = node->atom;

	/* Get txnh spinlock, this allows us to compare txn_atom pointers but it doesn't
	   let us touch the atoms themselves. */
	spin_lock_txnh(txnh);
	txnh_atom = txnh->atom;
	/* Process of capturing continues into one of four branches depends on
	   which atoms from (block atom (node->atom), current atom (txnh->atom))
	   exist. */
	if (txnh_atom == NULL) {
		if (block_atom == NULL) {
			spin_unlock_txnh(txnh);
			spin_unlock_jnode(node);
			/* assign empty atom to the txnh and repeat */
			return atom_begin_and_assign_to_txnh(atom_alloc, txnh);
		} else {
			atomic_inc(&block_atom->refcount);
			/* node spin-lock isn't needed anymore */
			spin_unlock_jnode(node);
			if (!spin_trylock_atom(block_atom)) {
				spin_unlock_txnh(txnh);
				spin_lock_atom(block_atom);
				spin_lock_txnh(txnh);
			}
			/* re-check state after getting txnh and the node
			 * atom spin-locked */
			if (node->atom != block_atom || txnh->atom != NULL) {
				spin_unlock_txnh(txnh);
				atom_dec_and_unlock(block_atom);
				return RETERR(-E_REPEAT);
			}
			atomic_dec(&block_atom->refcount);
			if (block_atom->stage > ASTAGE_CAPTURE_WAIT ||
			    (block_atom->stage == ASTAGE_CAPTURE_WAIT &&
			     block_atom->txnh_count != 0))
				return capture_fuse_wait(txnh, block_atom, NULL, mode);
			capture_assign_txnh_nolock(block_atom, txnh);
			spin_unlock_txnh(txnh);
			spin_unlock_atom(block_atom);
			return RETERR(-E_REPEAT);
		}
	} else {
		/* It is time to perform deadlock prevention check over the
                  node we want to capture.  It is possible this node was locked
                  for read without capturing it. The optimization which allows
                  to do it helps us in keeping atoms independent as long as
                  possible but it may cause lock/fuse deadlock problems.

                  A number of similar deadlock situations with locked but not
                  captured nodes were found.  In each situation there are two
                  or more threads: one of them does flushing while another one
                  does routine balancing or tree lookup.  The flushing thread
                  (F) sleeps in long term locking request for node (N), another
                  thread (A) sleeps in trying to capture some node already
                  belonging the atom F, F has a state which prevents
                  immediately fusion .

                  Deadlocks of this kind cannot happen if node N was properly
                  captured by thread A. The F thread fuse atoms before locking
                  therefore current atom of thread F and current atom of thread
                  A became the same atom and thread A may proceed.  This does
                  not work if node N was not captured because the fusion of
                  atom does not happens.

                  The following scheme solves the deadlock: If
                  longterm_lock_znode locks and does not capture a znode, that
                  znode is marked as MISSED_IN_CAPTURE.  A node marked this way
                  is processed by the code below which restores the missed
                  capture and fuses current atoms of all the node lock owners
                  by calling the fuse_not_fused_lock_owners() function. */
		if (JF_ISSET(node, JNODE_MISSED_IN_CAPTURE)) {
			JF_CLR(node, JNODE_MISSED_IN_CAPTURE);
			if (jnode_is_znode(node) && znode_is_locked(JZNODE(node))) {
				spin_unlock_txnh(txnh);
				spin_unlock_jnode(node);
				fuse_not_fused_lock_owners(txnh, JZNODE(node));
				return RETERR(-E_REPEAT);
			}
		}
		if (block_atom == NULL) {
			atomic_inc(&txnh_atom->refcount);
			spin_unlock_txnh(txnh);
			if (!spin_trylock_atom(txnh_atom)) {
				spin_unlock_jnode(node);
				spin_lock_atom(txnh_atom);
				spin_lock_jnode(node);
			}
			if (txnh->atom != txnh_atom || node->atom != NULL
				|| JF_ISSET(node, JNODE_IS_DYING)) {
				spin_unlock_jnode(node);
				atom_dec_and_unlock(txnh_atom);
				return RETERR(-E_REPEAT);
			}
			atomic_dec(&txnh_atom->refcount);
			capture_assign_block_nolock(txnh_atom, node);
			spin_unlock_atom(txnh_atom);
		} else {
			if (txnh_atom != block_atom) {
				if (mode & TXN_CAPTURE_DONT_FUSE) {
					spin_unlock_txnh(txnh);
					spin_unlock_jnode(node);
					/* we are in a "no-fusion" mode and @node is
					 * already part of transaction. */
					return RETERR(-E_NO_NEIGHBOR);
				}
				return capture_init_fusion(node, txnh, mode);
			}
			spin_unlock_txnh(txnh);
		}
	}
	return 0;
}

static txn_capture
build_capture_mode(jnode * node, znode_lock_mode lock_mode, txn_capture flags)
{
	txn_capture cap_mode;

	assert_spin_locked(&(node->guard));

	/* FIXME_JMACD No way to set TXN_CAPTURE_READ_MODIFY yet. */

	if (lock_mode == ZNODE_WRITE_LOCK) {
		cap_mode = TXN_CAPTURE_WRITE;
	} else if (node->atom != NULL) {
		cap_mode = TXN_CAPTURE_WRITE;
	} else if (0 &&		/* txnh->mode == TXN_READ_FUSING && */
		   jnode_get_level(node) == LEAF_LEVEL) {
		/* NOTE-NIKITA TXN_READ_FUSING is not currently used */
		/* We only need a READ_FUSING capture at the leaf level.  This
		   is because the internal levels of the tree (twigs included)
		   are redundant from the point of the user that asked for a
		   read-fusing transcrash.  The user only wants to read-fuse
		   atoms due to reading uncommitted data that another user has
		   written.  It is the file system that reads/writes the
		   internal tree levels, the user only reads/writes leaves. */
		cap_mode = TXN_CAPTURE_READ_ATOMIC;
	} else {
		/* In this case (read lock at a non-leaf) there's no reason to
		 * capture. */
		/* cap_mode = TXN_CAPTURE_READ_NONCOM; */
		return 0;
	}

	cap_mode |= (flags & (TXN_CAPTURE_NONBLOCKING | TXN_CAPTURE_DONT_FUSE));
	assert("nikita-3186", cap_mode != 0);
	return cap_mode;
}

/* This is an external interface to try_capture_block(), it calls
   try_capture_block() repeatedly as long as -E_REPEAT is returned.

   @node:         node to capture,
   @lock_mode:    read or write lock is used in capture mode calculation,
   @flags:        see txn_capture flags enumeration,
   @can_coc     : can copy-on-capture

   @return: 0 - node was successfully captured, -E_REPEAT - capture request
            cannot be processed immediately as it was requested in flags,
	    < 0 - other errors.
*/
int reiser4_try_capture(jnode *node, znode_lock_mode lock_mode,
			txn_capture flags)
{
	txn_atom *atom_alloc = NULL;
	txn_capture cap_mode;
	txn_handle *txnh = get_current_context()->trans;
	int ret;

	assert_spin_locked(&(node->guard));

      repeat:
	if (JF_ISSET(node, JNODE_IS_DYING))
		return RETERR(-EINVAL);
	if (node->atom != NULL && txnh->atom == node->atom)
		return 0;
	cap_mode = build_capture_mode(node, lock_mode, flags);
	if (cap_mode == 0 ||
	    (!(cap_mode & TXN_CAPTURE_WTYPES) && node->atom == NULL)) {
		/* Mark this node as "MISSED".  It helps in further deadlock
		 * analysis */
		if (jnode_is_znode(node))
			JF_SET(node, JNODE_MISSED_IN_CAPTURE);
		return 0;
	}
	/* Repeat try_capture as long as -E_REPEAT is returned. */
	ret = try_capture_block(txnh, node, cap_mode, &atom_alloc);
	/* Regardless of non_blocking:

	   If ret == 0 then jnode is still locked.
	   If ret != 0 then jnode is unlocked.
	 */
#if REISER4_DEBUG
	if (ret == 0)
		assert_spin_locked(&(node->guard));
	else
		assert_spin_not_locked(&(node->guard));
#endif
	assert_spin_not_locked(&(txnh->guard));

	if (ret == -E_REPEAT) {
		/* E_REPEAT implies all locks were released, therefore we need
		   to take the jnode's lock again. */
		spin_lock_jnode(node);

		/* Although this may appear to be a busy loop, it is not.
		   There are several conditions that cause E_REPEAT to be
		   returned by the call to try_capture_block, all cases
		   indicating some kind of state change that means you should
		   retry the request and will get a different result.  In some
		   cases this could be avoided with some extra code, but
		   generally it is done because the necessary locks were
		   released as a result of the operation and repeating is the
		   simplest thing to do (less bug potential).  The cases are:
		   atom fusion returns E_REPEAT after it completes (jnode and
		   txnh were unlocked); race conditions in assign_block,
		   assign_txnh, and init_fusion return E_REPEAT (trylock
		   failure); after going to sleep in capture_fuse_wait
		   (request was blocked but may now succeed).  I'm not quite
		   sure how capture_copy works yet, but it may also return
		   E_REPEAT.  When the request is legitimately blocked, the
		   requestor goes to sleep in fuse_wait, so this is not a busy
		   loop. */
		/* NOTE-NIKITA: still don't understand:

		   try_capture_block->capture_assign_txnh->spin_trylock_atom->E_REPEAT

		   looks like busy loop?
		 */
		goto repeat;
	}

	/* free extra atom object that was possibly allocated by
	   try_capture_block().

	   Do this before acquiring jnode spin lock to
	   minimize time spent under lock. --nikita */
	if (atom_alloc != NULL) {
		kmem_cache_free(_atom_slab, atom_alloc);
	}

	if (ret != 0) {
		if (ret == -E_BLOCK) {
			assert("nikita-3360",
			       cap_mode & TXN_CAPTURE_NONBLOCKING);
			ret = -E_REPEAT;
		}

		/* Failure means jnode is not locked.  FIXME_LATER_JMACD May
		   want to fix the above code to avoid releasing the lock and
		   re-acquiring it, but there are cases were failure occurs
		   when the lock is not held, and those cases would need to be
		   modified to re-take the lock. */
		spin_lock_jnode(node);
	}

	/* Jnode is still locked. */
	assert_spin_locked(&(node->guard));
	return ret;
}

static void release_two_atoms(txn_atom *one, txn_atom *two)
{
	spin_unlock_atom(one);
	atom_dec_and_unlock(two);
	spin_lock_atom(one);
	atom_dec_and_unlock(one);
}

/* This function sets up a call to try_capture_block and repeats as long as -E_REPEAT is
   returned by that routine.  The txn_capture request mode is computed here depending on
   the transaction handle's type and the lock request.  This is called from the depths of
   the lock manager with the jnode lock held and it always returns with the jnode lock
   held.
*/

/* fuse all 'active' atoms of lock owners of given node. */
static void fuse_not_fused_lock_owners(txn_handle * txnh, znode * node)
{
	lock_handle *lh;
	int repeat;
	txn_atom *atomh, *atomf;
	reiser4_context *me = get_current_context();
	reiser4_context *ctx = NULL;

	assert_spin_not_locked(&(ZJNODE(node)->guard));
	assert_spin_not_locked(&(txnh->hlock));

 repeat:
	repeat = 0;
	atomh = txnh_get_atom(txnh);
	spin_unlock_txnh(txnh);
	assert("zam-692", atomh != NULL);

	spin_lock_zlock(&node->lock);
	/* inspect list of lock owners */
	list_for_each_entry(lh, &node->lock.owners, owners_link) {
		ctx = get_context_by_lock_stack(lh->owner);
		if (ctx == me)
			continue;
		/* below we use two assumptions to avoid addition spin-locks
		   for checking the condition :

		   1) if the lock stack has lock, the transaction should be
		   opened, i.e. ctx->trans != NULL;

		   2) reading of well-aligned ctx->trans->atom is atomic, if it
		   equals to the address of spin-locked atomh, we take that
		   the atoms are the same, nothing has to be captured. */
		if (atomh != ctx->trans->atom) {
			reiser4_wake_up(lh->owner);
			repeat = 1;
			break;
		}
	}
	if (repeat) {
		if (!spin_trylock_txnh(ctx->trans)) {
			spin_unlock_zlock(&node->lock);
			spin_unlock_atom(atomh);
			goto repeat;
		}
		atomf = ctx->trans->atom;
		if (atomf == NULL) {
			capture_assign_txnh_nolock(atomh, ctx->trans);
			/* release zlock lock _after_ assigning the atom to the
			 * transaction handle, otherwise the lock owner thread
			 * may unlock all znodes, exit kernel context and here
			 * we would access an invalid transaction handle. */
			spin_unlock_zlock(&node->lock);
			spin_unlock_atom(atomh);
			spin_unlock_txnh(ctx->trans);
			goto repeat;
		}
		assert("zam-1059", atomf != atomh);
		spin_unlock_zlock(&node->lock);
		atomic_inc(&atomh->refcount);
		atomic_inc(&atomf->refcount);
		spin_unlock_txnh(ctx->trans);
		if (atomf > atomh) {
			spin_lock_atom_nested(atomf);
		} else {
			spin_unlock_atom(atomh);
			spin_lock_atom(atomf);
			spin_lock_atom_nested(atomh);
		}
		if (atomh == atomf || !atom_isopen(atomh) || !atom_isopen(atomf)) {
			release_two_atoms(atomf, atomh);
			goto repeat;
		}
		atomic_dec(&atomh->refcount);
		atomic_dec(&atomf->refcount);
		capture_fuse_into(atomf, atomh);
		goto repeat;
	}
	spin_unlock_zlock(&node->lock);
	spin_unlock_atom(atomh);
}

/* This is the interface to capture unformatted nodes via their struct page
   reference. Currently it is only used in reiser4_invalidatepage */
int try_capture_page_to_invalidate(struct page *pg)
{
	int ret;
	jnode *node;

	assert("umka-292", pg != NULL);
	assert("nikita-2597", PageLocked(pg));

	if (IS_ERR(node = jnode_of_page(pg))) {
		return PTR_ERR(node);
	}

	spin_lock_jnode(node);
	unlock_page(pg);

	ret = reiser4_try_capture(node, ZNODE_WRITE_LOCK, 0);
	spin_unlock_jnode(node);
	jput(node);
	lock_page(pg);
	return ret;
}

/* This informs the transaction manager when a node is deleted.  Add the block to the
   atom's delete set and uncapture the block.

VS-FIXME-HANS: this E_REPEAT paradigm clutters the code and creates a need for
explanations.  find all the functions that use it, and unless there is some very
good reason to use it (I have not noticed one so far and I doubt it exists, but maybe somewhere somehow....),
move the loop to inside the function.

VS-FIXME-HANS: can this code be at all streamlined?  In particular, can you lock and unlock the jnode fewer times?
  */
void reiser4_uncapture_page(struct page *pg)
{
	jnode *node;
	txn_atom *atom;

	assert("umka-199", pg != NULL);
	assert("nikita-3155", PageLocked(pg));

	clear_page_dirty_for_io(pg);

	reiser4_wait_page_writeback(pg);

	node = jprivate(pg);
	BUG_ON(node == NULL);

	spin_lock_jnode(node);

	atom = jnode_get_atom(node);
	if (atom == NULL) {
		assert("jmacd-7111", !JF_ISSET(node, JNODE_DIRTY));
		spin_unlock_jnode(node);
		return;
	}

	/* We can remove jnode from transaction even if it is on flush queue
	 * prepped list, we only need to be sure that flush queue is not being
	 * written by reiser4_write_fq().  reiser4_write_fq() does not use atom
	 * spin lock for protection of the prepped nodes list, instead
	 * write_fq() increments atom's nr_running_queues counters for the time
	 * when prepped list is not protected by spin lock.  Here we check this
	 * counter if we want to remove jnode from flush queue and, if the
	 * counter is not zero, wait all reiser4_write_fq() for this atom to
	 * complete. This is not significant overhead. */
	while (JF_ISSET(node, JNODE_FLUSH_QUEUED) && atom->nr_running_queues) {
		spin_unlock_jnode(node);
		/*
		 * at this moment we want to wait for "atom event", viz. wait
		 * until @node can be removed from flush queue. But
		 * reiser4_atom_wait_event() cannot be called with page locked,
		 * because it deadlocks with jnode_extent_write(). Unlock page,
		 * after making sure (through page_cache_get()) that it cannot
		 * be released from memory.
		 */
		page_cache_get(pg);
		unlock_page(pg);
		reiser4_atom_wait_event(atom);
		lock_page(pg);
		/*
		 * page may has been detached by ->writepage()->releasepage().
		 */
		reiser4_wait_page_writeback(pg);
		spin_lock_jnode(node);
		page_cache_release(pg);
		atom = jnode_get_atom(node);
/* VS-FIXME-HANS: improve the commenting in this function */
		if (atom == NULL) {
			spin_unlock_jnode(node);
			return;
		}
	}
	reiser4_uncapture_block(node);
	spin_unlock_atom(atom);
	jput(node);
}

/* this is used in extent's kill hook to uncapture and unhash jnodes attached to
 * inode's tree of jnodes */
void reiser4_uncapture_jnode(jnode * node)
{
	txn_atom *atom;

	assert_spin_locked(&(node->guard));
	assert("", node->pg == 0);

	atom = jnode_get_atom(node);
	if (atom == NULL) {
		assert("jmacd-7111", !JF_ISSET(node, JNODE_DIRTY));
		spin_unlock_jnode(node);
		return;
	}

	reiser4_uncapture_block(node);
	spin_unlock_atom(atom);
	jput(node);
}

/* No-locking version of assign_txnh.  Sets the transaction handle's atom pointer,
   increases atom refcount and txnh_count, adds to txnh_list. */
static void capture_assign_txnh_nolock(txn_atom *atom, txn_handle *txnh)
{
	assert("umka-200", atom != NULL);
	assert("umka-201", txnh != NULL);

	assert_spin_locked(&(txnh->hlock));
	assert_spin_locked(&(atom->alock));
	assert("jmacd-824", txnh->atom == NULL);
	assert("nikita-3540", atom_isopen(atom));
	BUG_ON(txnh->atom != NULL);

	atomic_inc(&atom->refcount);
	txnh->atom = atom;
	reiser4_ctx_gfp_mask_set();
	list_add_tail(&txnh->txnh_link, &atom->txnh_list);
	atom->txnh_count += 1;
}

/* No-locking version of assign_block.  Sets the block's atom pointer, references the
   block, adds it to the clean or dirty capture_jnode list, increments capture_count. */
static void capture_assign_block_nolock(txn_atom *atom, jnode *node)
{
	assert("umka-202", atom != NULL);
	assert("umka-203", node != NULL);
	assert_spin_locked(&(node->guard));
	assert_spin_locked(&(atom->alock));
	assert("jmacd-323", node->atom == NULL);
	BUG_ON(!list_empty_careful(&node->capture_link));
	assert("nikita-3470", !JF_ISSET(node, JNODE_DIRTY));

	/* Pointer from jnode to atom is not counted in atom->refcount. */
	node->atom = atom;

	list_add_tail(&node->capture_link, ATOM_CLEAN_LIST(atom));
	atom->capture_count += 1;
	/* reference to jnode is acquired by atom. */
	jref(node);

	ON_DEBUG(count_jnode(atom, node, NOT_CAPTURED, CLEAN_LIST, 1));

	LOCK_CNT_INC(t_refs);
}

/* common code for dirtying both unformatted jnodes and formatted znodes. */
static void do_jnode_make_dirty(jnode * node, txn_atom * atom)
{
	assert_spin_locked(&(node->guard));
	assert_spin_locked(&(atom->alock));
	assert("jmacd-3981", !JF_ISSET(node, JNODE_DIRTY));

	JF_SET(node, JNODE_DIRTY);

	if (!JF_ISSET(node, JNODE_CLUSTER_PAGE))
		get_current_context()->nr_marked_dirty++;

	/* We grab2flush_reserve one additional block only if node was
	   not CREATED and jnode_flush did not sort it into neither
	   relocate set nor overwrite one. If node is in overwrite or
	   relocate set we assume that atom's flush reserved counter was
	   already adjusted. */
	if (!JF_ISSET(node, JNODE_CREATED) && !JF_ISSET(node, JNODE_RELOC)
	    && !JF_ISSET(node, JNODE_OVRWR) && jnode_is_leaf(node)
	    && !jnode_is_cluster_page(node)) {
		assert("vs-1093", !reiser4_blocknr_is_fake(&node->blocknr));
		assert("vs-1506", *jnode_get_block(node) != 0);
		grabbed2flush_reserved_nolock(atom, (__u64) 1);
		JF_SET(node, JNODE_FLUSH_RESERVED);
	}

	if (!JF_ISSET(node, JNODE_FLUSH_QUEUED)) {
		/* If the atom is not set yet, it will be added to the appropriate list in
		   capture_assign_block_nolock. */
		/* Sometimes a node is set dirty before being captured -- the case for new
		   jnodes.  In that case the jnode will be added to the appropriate list
		   in capture_assign_block_nolock. Another reason not to re-link jnode is
		   that jnode is on a flush queue (see flush.c for details) */

		int level = jnode_get_level(node);

		assert("nikita-3152", !JF_ISSET(node, JNODE_OVRWR));
		assert("zam-654", atom->stage < ASTAGE_PRE_COMMIT);
		assert("nikita-2607", 0 <= level);
		assert("nikita-2606", level <= REAL_MAX_ZTREE_HEIGHT);

		/* move node to atom's dirty list */
		list_move_tail(&node->capture_link, ATOM_DIRTY_LIST(atom, level));
		ON_DEBUG(count_jnode
			 (atom, node, NODE_LIST(node), DIRTY_LIST, 1));
	}
}

/* Set the dirty status for this (spin locked) jnode. */
void jnode_make_dirty_locked(jnode * node)
{
	assert("umka-204", node != NULL);
	assert_spin_locked(&(node->guard));

	if (REISER4_DEBUG && rofs_jnode(node)) {
		warning("nikita-3365", "Dirtying jnode on rofs");
		dump_stack();
	}

	/* Fast check for already dirty node */
	if (!JF_ISSET(node, JNODE_DIRTY)) {
		txn_atom *atom;

		atom = jnode_get_atom(node);
		assert("vs-1094", atom);
		/* Check jnode dirty status again because node spin lock might
		 * be released inside jnode_get_atom(). */
		if (likely(!JF_ISSET(node, JNODE_DIRTY)))
			do_jnode_make_dirty(node, atom);
		spin_unlock_atom(atom);
	}
}

/* Set the dirty status for this znode. */
void znode_make_dirty(znode * z)
{
	jnode *node;
	struct page *page;

	assert("umka-204", z != NULL);
	assert("nikita-3290", znode_above_root(z) || znode_is_loaded(z));
	assert("nikita-3560", znode_is_write_locked(z));

	node = ZJNODE(z);
	/* znode is longterm locked, we can check dirty bit without spinlock */
	if (JF_ISSET(node, JNODE_DIRTY)) {
		/* znode is dirty already. All we have to do is to change znode version */
		z->version = znode_build_version(jnode_get_tree(node));
		return;
	}

	spin_lock_jnode(node);
	jnode_make_dirty_locked(node);
	page = jnode_page(node);
	if (page != NULL) {
		/* this is useful assertion (allows one to check that no
		 * modifications are lost due to update of in-flight page),
		 * but it requires locking on page to check PG_writeback
		 * bit. */
		/* assert("nikita-3292",
		   !PageWriteback(page) || ZF_ISSET(z, JNODE_WRITEBACK)); */
		page_cache_get(page);

		/* jnode lock is not needed for the rest of
		 * znode_set_dirty(). */
		spin_unlock_jnode(node);
		/* reiser4 file write code calls set_page_dirty for
		 * unformatted nodes, for formatted nodes we do it here. */
		set_page_dirty_notag(page);
		page_cache_release(page);
		/* bump version counter in znode */
		z->version = znode_build_version(jnode_get_tree(node));
	} else {
		assert("zam-596", znode_above_root(JZNODE(node)));
		spin_unlock_jnode(node);
	}

	assert("nikita-1900", znode_is_write_locked(z));
	assert("jmacd-9777", node->atom != NULL);
}

int reiser4_sync_atom(txn_atom * atom)
{
	int result;
	txn_handle *txnh;

	txnh = get_current_context()->trans;

	result = 0;
	if (atom != NULL) {
		if (atom->stage < ASTAGE_PRE_COMMIT) {
			spin_lock_txnh(txnh);
			capture_assign_txnh_nolock(atom, txnh);
			result = force_commit_atom(txnh);
		} else if (atom->stage < ASTAGE_POST_COMMIT) {
			/* wait atom commit */
			reiser4_atom_wait_event(atom);
			/* try once more */
			result = RETERR(-E_REPEAT);
		} else
			spin_unlock_atom(atom);
	}
	return result;
}

#if REISER4_DEBUG

/* move jnode form one list to another
   call this after atom->capture_count is updated */
void
count_jnode(txn_atom * atom, jnode * node, atom_list old_list,
	    atom_list new_list, int check_lists)
{
	struct list_head *pos;

	assert("zam-1018", atom_is_protected(atom));
	assert_spin_locked(&(node->guard));
	assert("", NODE_LIST(node) == old_list);

	switch (NODE_LIST(node)) {
	case NOT_CAPTURED:
		break;
	case DIRTY_LIST:
		assert("", atom->dirty > 0);
		atom->dirty--;
		break;
	case CLEAN_LIST:
		assert("", atom->clean > 0);
		atom->clean--;
		break;
	case FQ_LIST:
		assert("", atom->fq > 0);
		atom->fq--;
		break;
	case WB_LIST:
		assert("", atom->wb > 0);
		atom->wb--;
		break;
	case OVRWR_LIST:
		assert("", atom->ovrwr > 0);
		atom->ovrwr--;
		break;
	default:
		impossible("", "");
	}

	switch (new_list) {
	case NOT_CAPTURED:
		break;
	case DIRTY_LIST:
		atom->dirty++;
		break;
	case CLEAN_LIST:
		atom->clean++;
		break;
	case FQ_LIST:
		atom->fq++;
		break;
	case WB_LIST:
		atom->wb++;
		break;
	case OVRWR_LIST:
		atom->ovrwr++;
		break;
	default:
		impossible("", "");
	}
	ASSIGN_NODE_LIST(node, new_list);
	if (0 && check_lists) {
		int count;
		tree_level level;

		count = 0;

		/* flush queue list */
		/* reiser4_check_fq(atom); */

		/* dirty list */
		count = 0;
		for (level = 0; level < REAL_MAX_ZTREE_HEIGHT + 1; level += 1) {
			list_for_each(pos, ATOM_DIRTY_LIST(atom, level))
				count++;
		}
		if (count != atom->dirty)
			warning("", "dirty counter %d, real %d\n", atom->dirty,
				count);

		/* clean list */
		count = 0;
		list_for_each(pos, ATOM_CLEAN_LIST(atom))
			count++;
		if (count != atom->clean)
			warning("", "clean counter %d, real %d\n", atom->clean,
				count);

		/* wb list */
		count = 0;
		list_for_each(pos, ATOM_WB_LIST(atom))
			count++;
		if (count != atom->wb)
			warning("", "wb counter %d, real %d\n", atom->wb,
				count);

		/* overwrite list */
		count = 0;
		list_for_each(pos, ATOM_OVRWR_LIST(atom))
			count++;

		if (count != atom->ovrwr)
			warning("", "ovrwr counter %d, real %d\n", atom->ovrwr,
				count);
	}
	assert("vs-1624", atom->num_queued == atom->fq);
	if (atom->capture_count !=
	    atom->dirty + atom->clean + atom->ovrwr + atom->wb + atom->fq) {
		printk
		    ("count %d, dirty %d clean %d ovrwr %d wb %d fq %d\n",
		     atom->capture_count, atom->dirty, atom->clean, atom->ovrwr,
		     atom->wb, atom->fq);
		assert("vs-1622",
		       atom->capture_count ==
		       atom->dirty + atom->clean + atom->ovrwr + atom->wb +
		       atom->fq);
	}
}

#endif

int reiser4_capture_super_block(struct super_block *s)
{
	int result;
	znode *uber;
	lock_handle lh;

	init_lh(&lh);
	result = get_uber_znode(reiser4_get_tree(s),
				ZNODE_WRITE_LOCK, ZNODE_LOCK_LOPRI, &lh);
	if (result)
		return result;

	uber = lh.node;
	/* Grabbing one block for superblock */
	result = reiser4_grab_space_force((__u64) 1, BA_RESERVED);
	if (result != 0)
		return result;

	znode_make_dirty(uber);

	done_lh(&lh);
	return 0;
}

/* Wakeup every handle on the atom's WAITFOR list */
static void wakeup_atom_waitfor_list(txn_atom * atom)
{
	txn_wait_links *wlinks;

	assert("umka-210", atom != NULL);

	/* atom is locked */
	list_for_each_entry(wlinks, &atom->fwaitfor_list, _fwaitfor_link) {
		if (wlinks->waitfor_cb == NULL ||
		    wlinks->waitfor_cb(atom, wlinks))
			/* Wake up. */
			reiser4_wake_up(wlinks->_lock_stack);
	}
}

/* Wakeup every handle on the atom's WAITING list */
static void wakeup_atom_waiting_list(txn_atom * atom)
{
	txn_wait_links *wlinks;

	assert("umka-211", atom != NULL);

	/* atom is locked */
	list_for_each_entry(wlinks, &atom->fwaiting_list, _fwaiting_link) {
		if (wlinks->waiting_cb == NULL ||
		    wlinks->waiting_cb(atom, wlinks))
			/* Wake up. */
			reiser4_wake_up(wlinks->_lock_stack);
	}
}

/* helper function used by capture_fuse_wait() to avoid "spurious wake-ups" */
static int wait_for_fusion(txn_atom * atom, txn_wait_links * wlinks)
{
	assert("nikita-3330", atom != NULL);
	assert_spin_locked(&(atom->alock));

	/* atom->txnh_count == 1 is for waking waiters up if we are releasing
	 * last transaction handle. */
	return atom->stage != ASTAGE_CAPTURE_WAIT || atom->txnh_count == 1;
}

/* The general purpose of this function is to wait on the first of two possible events.
   The situation is that a handle (and its atom atomh) is blocked trying to capture a
   block (i.e., node) but the node's atom (atomf) is in the CAPTURE_WAIT state.  The
   handle's atom (atomh) is not in the CAPTURE_WAIT state.  However, atomh could fuse with
   another atom or, due to age, enter the CAPTURE_WAIT state itself, at which point it
   needs to unblock the handle to avoid deadlock.  When the txnh is unblocked it will
   proceed and fuse the two atoms in the CAPTURE_WAIT state.

   In other words, if either atomh or atomf change state, the handle will be awakened,
   thus there are two lists per atom: WAITING and WAITFOR.

   This is also called by capture_assign_txnh with (atomh == NULL) to wait for atomf to
   close but it is not assigned to an atom of its own.

   Lock ordering in this method: all four locks are held: JNODE_LOCK, TXNH_LOCK,
   BOTH_ATOM_LOCKS.  Result: all four locks are released.
*/
static int capture_fuse_wait(txn_handle * txnh, txn_atom * atomf,
		    txn_atom * atomh, txn_capture mode)
{
	int ret;
	txn_wait_links wlinks;

	assert("umka-213", txnh != NULL);
	assert("umka-214", atomf != NULL);

	if ((mode & TXN_CAPTURE_NONBLOCKING) != 0) {
		spin_unlock_txnh(txnh);
		spin_unlock_atom(atomf);

		if (atomh) {
			spin_unlock_atom(atomh);
		}

		return RETERR(-E_BLOCK);
	}

	/* Initialize the waiting list links. */
	init_wlinks(&wlinks);

	/* Add txnh to atomf's waitfor list, unlock atomf. */
	list_add_tail(&wlinks._fwaitfor_link, &atomf->fwaitfor_list);
	wlinks.waitfor_cb = wait_for_fusion;
	atomic_inc(&atomf->refcount);
	spin_unlock_atom(atomf);

	if (atomh) {
		/* Add txnh to atomh's waiting list, unlock atomh. */
		list_add_tail(&wlinks._fwaiting_link, &atomh->fwaiting_list);
		atomic_inc(&atomh->refcount);
		spin_unlock_atom(atomh);
	}

	/* Go to sleep. */
	spin_unlock_txnh(txnh);

	ret = reiser4_prepare_to_sleep(wlinks._lock_stack);
	if (ret == 0) {
		reiser4_go_to_sleep(wlinks._lock_stack);
		ret = RETERR(-E_REPEAT);
	}

	/* Remove from the waitfor list. */
	spin_lock_atom(atomf);

	list_del(&wlinks._fwaitfor_link);
	atom_dec_and_unlock(atomf);

	if (atomh) {
		/* Remove from the waiting list. */
		spin_lock_atom(atomh);
		list_del(&wlinks._fwaiting_link);
		atom_dec_and_unlock(atomh);
	}
	return ret;
}

static void lock_two_atoms(txn_atom * one, txn_atom * two)
{
	assert("zam-1067", one != two);

	/* lock the atom with lesser address first */
	if (one < two) {
		spin_lock_atom(one);
		spin_lock_atom_nested(two);
	} else {
		spin_lock_atom(two);
		spin_lock_atom_nested(one);
	}
}

/* Perform the necessary work to prepare for fusing two atoms, which involves
 * acquiring two atom locks in the proper order.  If one of the node's atom is
 * blocking fusion (i.e., it is in the CAPTURE_WAIT stage) and the handle's
 * atom is not then the handle's request is put to sleep.  If the node's atom
 * is committing, then the node can be copy-on-captured.  Otherwise, pick the
 * atom with fewer pointers to be fused into the atom with more pointer and
 * call capture_fuse_into.
 */
static int capture_init_fusion(jnode *node, txn_handle *txnh, txn_capture mode)
{
	txn_atom * txnh_atom = txnh->atom;
	txn_atom * block_atom = node->atom;

	atomic_inc(&txnh_atom->refcount);
	atomic_inc(&block_atom->refcount);

	spin_unlock_txnh(txnh);
	spin_unlock_jnode(node);

	lock_two_atoms(txnh_atom, block_atom);

	if (txnh->atom != txnh_atom || node->atom != block_atom ) {
		release_two_atoms(txnh_atom, block_atom);
		return RETERR(-E_REPEAT);
	}

	atomic_dec(&txnh_atom->refcount);
	atomic_dec(&block_atom->refcount);

	assert ("zam-1066", atom_isopen(txnh_atom));

	if (txnh_atom->stage >= block_atom->stage ||
	    (block_atom->stage == ASTAGE_CAPTURE_WAIT && block_atom->txnh_count == 0)) {
		capture_fuse_into(txnh_atom, block_atom);
		return RETERR(-E_REPEAT);
	}
	spin_lock_txnh(txnh);
	return capture_fuse_wait(txnh, block_atom, txnh_atom, mode);
}

/* This function splices together two jnode lists (small and large) and sets all jnodes in
   the small list to point to the large atom.  Returns the length of the list. */
static int
capture_fuse_jnode_lists(txn_atom *large, struct list_head *large_head,
			 struct list_head *small_head)
{
	int count = 0;
	jnode *node;

	assert("umka-218", large != NULL);
	assert("umka-219", large_head != NULL);
	assert("umka-220", small_head != NULL);
	/* small atom should be locked also. */
	assert_spin_locked(&(large->alock));

	/* For every jnode on small's capture list... */
	list_for_each_entry(node, small_head, capture_link) {
		count += 1;

		/* With the jnode lock held, update atom pointer. */
		spin_lock_jnode(node);
		node->atom = large;
		spin_unlock_jnode(node);
	}

	/* Splice the lists. */
	list_splice_init(small_head, large_head->prev);

	return count;
}

/* This function splices together two txnh lists (small and large) and sets all txn handles in
   the small list to point to the large atom.  Returns the length of the list. */
static int
capture_fuse_txnh_lists(txn_atom *large, struct list_head *large_head,
			struct list_head *small_head)
{
	int count = 0;
	txn_handle *txnh;

	assert("umka-221", large != NULL);
	assert("umka-222", large_head != NULL);
	assert("umka-223", small_head != NULL);

	/* Adjust every txnh to the new atom. */
	list_for_each_entry(txnh, small_head, txnh_link) {
		count += 1;

		/* With the txnh lock held, update atom pointer. */
		spin_lock_txnh(txnh);
		txnh->atom = large;
		spin_unlock_txnh(txnh);
	}

	/* Splice the txn_handle list. */
	list_splice_init(small_head, large_head->prev);

	return count;
}

/* This function fuses two atoms.  The captured nodes and handles belonging to SMALL are
   added to LARGE and their ->atom pointers are all updated.  The associated counts are
   updated as well, and any waiting handles belonging to either are awakened.  Finally the
   smaller atom's refcount is decremented.
*/
static void capture_fuse_into(txn_atom * small, txn_atom * large)
{
	int level;
	unsigned zcount = 0;
	unsigned tcount = 0;

	assert("umka-224", small != NULL);
	assert("umka-225", small != NULL);

	assert_spin_locked(&(large->alock));
	assert_spin_locked(&(small->alock));

	assert("jmacd-201", atom_isopen(small));
	assert("jmacd-202", atom_isopen(large));

	/* Splice and update the per-level dirty jnode lists */
	for (level = 0; level < REAL_MAX_ZTREE_HEIGHT + 1; level += 1) {
		zcount +=
		    capture_fuse_jnode_lists(large,
					     ATOM_DIRTY_LIST(large, level),
					     ATOM_DIRTY_LIST(small, level));
	}

	/* Splice and update the [clean,dirty] jnode and txnh lists */
	zcount +=
	    capture_fuse_jnode_lists(large, ATOM_CLEAN_LIST(large),
				     ATOM_CLEAN_LIST(small));
	zcount +=
	    capture_fuse_jnode_lists(large, ATOM_OVRWR_LIST(large),
				     ATOM_OVRWR_LIST(small));
	zcount +=
	    capture_fuse_jnode_lists(large, ATOM_WB_LIST(large),
				     ATOM_WB_LIST(small));
	zcount +=
	    capture_fuse_jnode_lists(large, &large->inodes, &small->inodes);
	tcount +=
	    capture_fuse_txnh_lists(large, &large->txnh_list,
				    &small->txnh_list);

	/* Check our accounting. */
	assert("jmacd-1063",
	       zcount + small->num_queued == small->capture_count);
	assert("jmacd-1065", tcount == small->txnh_count);

	/* sum numbers of waiters threads */
	large->nr_waiters += small->nr_waiters;
	small->nr_waiters = 0;

	/* splice flush queues */
	reiser4_fuse_fq(large, small);

	/* update counter of jnode on every atom' list */
	ON_DEBUG(large->dirty += small->dirty;
		 small->dirty = 0;
		 large->clean += small->clean;
		 small->clean = 0;
		 large->ovrwr += small->ovrwr;
		 small->ovrwr = 0;
		 large->wb += small->wb;
		 small->wb = 0;
		 large->fq += small->fq;
		 small->fq = 0;);

	/* count flushers in result atom */
	large->nr_flushers += small->nr_flushers;
	small->nr_flushers = 0;

	/* update counts of flushed nodes */
	large->flushed += small->flushed;
	small->flushed = 0;

	/* Transfer list counts to large. */
	large->txnh_count += small->txnh_count;
	large->capture_count += small->capture_count;

	/* Add all txnh references to large. */
	atomic_add(small->txnh_count, &large->refcount);
	atomic_sub(small->txnh_count, &small->refcount);

	/* Reset small counts */
	small->txnh_count = 0;
	small->capture_count = 0;

	/* Assign the oldest start_time, merge flags. */
	large->start_time = min(large->start_time, small->start_time);
	large->flags |= small->flags;

	/* Merge blocknr sets. */
	blocknr_set_merge(&small->wandered_map, &large->wandered_map);

	/* Merge delete sets. */
	atom_dset_merge(small, large);

	/* Merge allocated/deleted file counts */
	large->nr_objects_deleted += small->nr_objects_deleted;
	large->nr_objects_created += small->nr_objects_created;

	small->nr_objects_deleted = 0;
	small->nr_objects_created = 0;

	/* Merge allocated blocks counts */
	large->nr_blocks_allocated += small->nr_blocks_allocated;

	large->nr_running_queues += small->nr_running_queues;
	small->nr_running_queues = 0;

	/* Merge blocks reserved for overwrite set. */
	large->flush_reserved += small->flush_reserved;
	small->flush_reserved = 0;

	if (large->stage < small->stage) {
		/* Large only needs to notify if it has changed state. */
		reiser4_atom_set_stage(large, small->stage);
		wakeup_atom_waiting_list(large);
	}

	reiser4_atom_set_stage(small, ASTAGE_INVALID);

	/* Notify any waiters--small needs to unload its wait lists.  Waiters
	   actually remove themselves from the list before returning from the
	   fuse_wait function. */
	wakeup_atom_waiting_list(small);

	/* Unlock atoms */
	spin_unlock_atom(large);
	atom_dec_and_unlock(small);
}

/* TXNMGR STUFF */

/* Release a block from the atom, reversing the effects of being captured,
   do not release atom's reference to jnode due to holding spin-locks.
   Currently this is only called when the atom commits.

   NOTE: this function does not release a (journal) reference to jnode
   due to locking optimizations, you should call jput() somewhere after
   calling reiser4_uncapture_block(). */
void reiser4_uncapture_block(jnode * node)
{
	txn_atom *atom;

	assert("umka-226", node != NULL);
	atom = node->atom;
	assert("umka-228", atom != NULL);

	assert("jmacd-1021", node->atom == atom);
	assert_spin_locked(&(node->guard));
	assert("jmacd-1023", atom_is_protected(atom));

	JF_CLR(node, JNODE_DIRTY);
	JF_CLR(node, JNODE_RELOC);
	JF_CLR(node, JNODE_OVRWR);
	JF_CLR(node, JNODE_CREATED);
	JF_CLR(node, JNODE_WRITEBACK);
	JF_CLR(node, JNODE_REPACK);

	list_del_init(&node->capture_link);
	if (JF_ISSET(node, JNODE_FLUSH_QUEUED)) {
		assert("zam-925", atom_isopen(atom));
		assert("vs-1623", NODE_LIST(node) == FQ_LIST);
		ON_DEBUG(atom->num_queued--);
		JF_CLR(node, JNODE_FLUSH_QUEUED);
	}
	atom->capture_count -= 1;
	ON_DEBUG(count_jnode(atom, node, NODE_LIST(node), NOT_CAPTURED, 1));
	node->atom = NULL;

	spin_unlock_jnode(node);
	LOCK_CNT_DEC(t_refs);
}

/* Unconditional insert of jnode into atom's overwrite list. Currently used in
   bitmap-based allocator code for adding modified bitmap blocks the
   transaction. @atom and @node are spin locked */
void insert_into_atom_ovrwr_list(txn_atom * atom, jnode * node)
{
	assert("zam-538", atom_is_protected(atom));
	assert_spin_locked(&(node->guard));
	assert("zam-899", JF_ISSET(node, JNODE_OVRWR));
	assert("zam-543", node->atom == NULL);
	assert("vs-1433", !jnode_is_unformatted(node) && !jnode_is_znode(node));

	list_add(&node->capture_link, ATOM_OVRWR_LIST(atom));
	jref(node);
	node->atom = atom;
	atom->capture_count++;
	ON_DEBUG(count_jnode(atom, node, NODE_LIST(node), OVRWR_LIST, 1));
}

static int count_deleted_blocks_actor(txn_atom * atom,
				      const reiser4_block_nr * a,
				      const reiser4_block_nr * b, void *data)
{
	reiser4_block_nr *counter = data;

	assert("zam-995", data != NULL);
	assert("zam-996", a != NULL);
	if (b == NULL)
		*counter += 1;
	else
		*counter += *b;
	return 0;
}

reiser4_block_nr txnmgr_count_deleted_blocks(void)
{
	reiser4_block_nr result;
	txn_mgr *tmgr = &get_super_private(reiser4_get_current_sb())->tmgr;
	txn_atom *atom;

	result = 0;

	spin_lock_txnmgr(tmgr);
	list_for_each_entry(atom, &tmgr->atoms_list, atom_link) {
		spin_lock_atom(atom);
		if (atom_isopen(atom))
			atom_dset_deferred_apply(atom, count_deleted_blocks_actor, &result, 0);
		spin_unlock_atom(atom);
	}
	spin_unlock_txnmgr(tmgr);

	return result;
}

void atom_dset_init(txn_atom *atom)
{
	if (reiser4_is_set(reiser4_get_current_sb(), REISER4_DISCARD)) {
		blocknr_list_init(&atom->discard.delete_set);
	} else {
		blocknr_set_init(&atom->nodiscard.delete_set);
	}
}

void atom_dset_destroy(txn_atom *atom)
{
	if (reiser4_is_set(reiser4_get_current_sb(), REISER4_DISCARD)) {
		blocknr_list_destroy(&atom->discard.delete_set);
	} else {
		blocknr_set_destroy(&atom->nodiscard.delete_set);
	}
}

void atom_dset_merge(txn_atom *from, txn_atom *to)
{
	if (reiser4_is_set(reiser4_get_current_sb(), REISER4_DISCARD)) {
		blocknr_list_merge(&from->discard.delete_set, &to->discard.delete_set);
	} else {
		blocknr_set_merge(&from->nodiscard.delete_set, &to->nodiscard.delete_set);
	}
}

int atom_dset_deferred_apply(txn_atom* atom,
                             blocknr_set_actor_f actor,
                             void *data,
                             int delete)
{
	int ret;

	if (reiser4_is_set(reiser4_get_current_sb(), REISER4_DISCARD)) {
		ret = blocknr_list_iterator(atom,
		                            &atom->discard.delete_set,
		                            actor,
		                            data,
		                            delete);
	} else {
		ret = blocknr_set_iterator(atom,
		                           &atom->nodiscard.delete_set,
		                           actor,
		                           data,
		                           delete);
	}

	return ret;
}

extern int atom_dset_deferred_add_extent(txn_atom *atom,
                                         void **new_entry,
                                         const reiser4_block_nr *start,
                                         const reiser4_block_nr *len)
{
	int ret;

	if (reiser4_is_set(reiser4_get_current_sb(), REISER4_DISCARD)) {
		ret = blocknr_list_add_extent(atom,
		                              &atom->discard.delete_set,
		                              (blocknr_list_entry**)new_entry,
		                              start,
		                              len);
	} else {
		ret = blocknr_set_add_extent(atom,
		                             &atom->nodiscard.delete_set,
		                             (blocknr_set_entry**)new_entry,
		                             start,
		                             len);
	}

	return ret;
}

/*
 * Local variables:
 * c-indentation-style: "K&R"
 * mode-name: "LC"
 * c-basic-offset: 8
 * tab-width: 8
 * fill-column: 79
 * End:
 */
