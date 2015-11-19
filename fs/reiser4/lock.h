/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

/* Long term locking data structures. See lock.c for details. */

#ifndef __LOCK_H__
#define __LOCK_H__

#include "forward.h"
#include "debug.h"
#include "dformat.h"
#include "key.h"
#include "coord.h"
#include "plugin/node/node.h"
#include "txnmgr.h"
#include "readahead.h"

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/pagemap.h>	/* for PAGE_CACHE_SIZE */
#include <asm/atomic.h>
#include <linux/wait.h>

/* Per-znode lock object */
struct zlock {
	spinlock_t guard;
	/* The number of readers if positive; the number of recursively taken
	   write locks if negative. Protected by zlock spin lock. */
	int nr_readers;
	/* A number of processes (lock_stacks) that have this object
	   locked with high priority */
	unsigned nr_hipri_owners;
	/* A number of attempts to lock znode in high priority direction */
	unsigned nr_hipri_requests;
	/* A linked list of lock_handle objects that contains pointers
	   for all lock_stacks which have this lock object locked */
	unsigned nr_hipri_write_requests;
	struct list_head owners;
	/* A linked list of lock_stacks that wait for this lock */
	struct list_head requestors;
};

static inline void spin_lock_zlock(zlock *lock)
{
	/* check that zlock is not locked */
	assert("", LOCK_CNT_NIL(spin_locked_zlock));
	/* check that spinlocks of lower priorities are not held */
	assert("", LOCK_CNT_NIL(spin_locked_stack));

	spin_lock(&lock->guard);

	LOCK_CNT_INC(spin_locked_zlock);
	LOCK_CNT_INC(spin_locked);
}

static inline void spin_unlock_zlock(zlock *lock)
{
	assert("nikita-1375", LOCK_CNT_GTZ(spin_locked_zlock));
	assert("nikita-1376", LOCK_CNT_GTZ(spin_locked));

	LOCK_CNT_DEC(spin_locked_zlock);
	LOCK_CNT_DEC(spin_locked);

	spin_unlock(&lock->guard);
}

#define lock_is_locked(lock)          ((lock)->nr_readers != 0)
#define lock_is_rlocked(lock)         ((lock)->nr_readers > 0)
#define lock_is_wlocked(lock)         ((lock)->nr_readers < 0)
#define lock_is_wlocked_once(lock)    ((lock)->nr_readers == -1)
#define lock_can_be_rlocked(lock)     ((lock)->nr_readers >= 0)
#define lock_mode_compatible(lock, mode)				\
	      (((mode) == ZNODE_WRITE_LOCK && !lock_is_locked(lock)) ||	\
	      ((mode) == ZNODE_READ_LOCK && lock_can_be_rlocked(lock)))

/* Since we have R/W znode locks we need additional bidirectional `link'
   objects to implement n<->m relationship between lock owners and lock
   objects. We call them `lock handles'.

   Locking: see lock.c/"SHORT-TERM LOCKING"
*/
struct lock_handle {
	/* This flag indicates that a signal to yield a lock was passed to
	   lock owner and counted in owner->nr_signalled

	   Locking: this is accessed under spin lock on ->node.
	 */
	int signaled;
	/* A link to owner of a lock */
	lock_stack *owner;
	/* A link to znode locked */
	znode *node;
	/* A list of all locks for a process */
	struct list_head locks_link;
	/* A list of all owners for a znode */
	struct list_head owners_link;
};

struct lock_request {
	/* A pointer to uninitialized link object */
	lock_handle *handle;
	/* A pointer to the object we want to lock */
	znode *node;
	/* Lock mode (ZNODE_READ_LOCK or ZNODE_WRITE_LOCK) */
	znode_lock_mode mode;
	/* how dispatch_lock_requests() returns lock request result code */
	int ret_code;
};

/* A lock stack structure for accumulating locks owned by a process */
struct lock_stack {
	/* A guard lock protecting a lock stack */
	spinlock_t sguard;
	/* number of znodes which were requested by high priority processes */
	atomic_t nr_signaled;
	/* Current priority of a process

	   This is only accessed by the current thread and thus requires no
	   locking.
	 */
	int curpri;
	/* A list of all locks owned by this process. Elements can be added to
	 * this list only by the current thread. ->node pointers in this list
	 * can be only changed by the current thread. */
	struct list_head locks;
	/* When lock_stack waits for the lock, it puts itself on double-linked
	   requestors list of that lock */
	struct list_head requestors_link;
	/* Current lock request info.

	   This is only accessed by the current thread and thus requires no
	   locking.
	 */
	struct lock_request request;
	/* the following two fields are the lock stack's
	 * synchronization object to use with the standard linux/wait.h
	 * interface. See reiser4_go_to_sleep and __reiser4_wake_up for
	 * usage details. */
	wait_queue_head_t wait;
	atomic_t wakeup;
#if REISER4_DEBUG
	int nr_locks;		/* number of lock handles in the above list */
#endif
};

/*
  User-visible znode locking functions
*/

extern int longterm_lock_znode(lock_handle * handle,
			       znode * node,
			       znode_lock_mode mode,
			       znode_lock_request request);

extern void longterm_unlock_znode(lock_handle * handle);

extern int reiser4_check_deadlock(void);

extern lock_stack *get_current_lock_stack(void);

extern void init_lock_stack(lock_stack * owner);
extern void reiser4_init_lock(zlock * lock);

static inline void init_lh(lock_handle *lh)
{
#if REISER4_DEBUG
	memset(lh, 0, sizeof *lh);
	INIT_LIST_HEAD(&lh->locks_link);
	INIT_LIST_HEAD(&lh->owners_link);
#else
	lh->node = NULL;
#endif
}

static inline  void done_lh(lock_handle *lh)
{
	assert("zam-342", lh != NULL);
	if (lh->node != NULL)
		longterm_unlock_znode(lh);
}

extern void move_lh(lock_handle * new, lock_handle * old);
extern void copy_lh(lock_handle * new, lock_handle * old);

extern int reiser4_prepare_to_sleep(lock_stack * owner);
extern void reiser4_go_to_sleep(lock_stack * owner);
extern void __reiser4_wake_up(lock_stack * owner);

extern int lock_stack_isclean(lock_stack * owner);

/* zlock object state check macros: only used in assertions. Both forms imply
   that the lock is held by the current thread. */
extern int znode_is_write_locked(const znode *);
extern void reiser4_invalidate_lock(lock_handle *);

/* lock ordering is: first take zlock spin lock, then lock stack spin lock */
#define spin_ordering_pred_stack(stack)			\
	(LOCK_CNT_NIL(spin_locked_stack) &&		\
	 LOCK_CNT_NIL(spin_locked_txnmgr) &&		\
	 LOCK_CNT_NIL(spin_locked_inode) &&		\
	 LOCK_CNT_NIL(rw_locked_cbk_cache) &&		\
	 LOCK_CNT_NIL(spin_locked_super_eflush))

static inline void spin_lock_stack(lock_stack *stack)
{
	assert("", spin_ordering_pred_stack(stack));
	spin_lock(&(stack->sguard));
	LOCK_CNT_INC(spin_locked_stack);
	LOCK_CNT_INC(spin_locked);
}

static inline void spin_unlock_stack(lock_stack *stack)
{
	assert_spin_locked(&(stack->sguard));
	assert("nikita-1375", LOCK_CNT_GTZ(spin_locked_stack));
	assert("nikita-1376", LOCK_CNT_GTZ(spin_locked));
	LOCK_CNT_DEC(spin_locked_stack);
	LOCK_CNT_DEC(spin_locked);
	spin_unlock(&(stack->sguard));
}

static inline void reiser4_wake_up(lock_stack * owner)
{
	spin_lock_stack(owner);
	__reiser4_wake_up(owner);
	spin_unlock_stack(owner);
}

const char *lock_mode_name(znode_lock_mode lock);

#if REISER4_DEBUG
extern void check_lock_data(void);
extern void check_lock_node_data(znode * node);
#else
#define check_lock_data() noop
#define check_lock_node_data() noop
#endif

/* __LOCK_H__ */
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
