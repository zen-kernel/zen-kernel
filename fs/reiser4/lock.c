/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

/* Traditional deadlock avoidance is achieved by acquiring all locks in a single
   order.  V4 balances the tree from the bottom up, and searches the tree from
   the top down, and that is really the way we want it, so tradition won't work
   for us.

   Instead we have two lock orderings, a high priority lock ordering, and a low
   priority lock ordering.  Each node in the tree has a lock in its znode.

   Suppose we have a set of processes which lock (R/W) tree nodes. Each process
   has a set (maybe empty) of already locked nodes ("process locked set"). Each
   process may have a pending lock request to a node locked by another process.
   Note: we lock and unlock, but do not transfer locks: it is possible
   transferring locks instead would save some bus locking....

   Deadlock occurs when we have a loop constructed from process locked sets and
   lock request vectors.

   NOTE: The reiser4 "tree" is a tree on disk, but its cached representation in
   memory is extended with "znodes" with which we connect nodes with their left
   and right neighbors using sibling pointers stored in the znodes.  When we
   perform balancing operations we often go from left to right and from right to
   left.

   +-P1-+          +-P3-+
   |+--+|   V1     |+--+|
   ||N1|| -------> ||N3||
   |+--+|          |+--+|
   +----+          +----+
     ^               |
     |V2             |V3
     |               v
   +---------P2---------+
   |+--+            +--+|
   ||N2|  --------  |N4||
   |+--+            +--+|
   +--------------------+

   We solve this by ensuring that only low priority processes lock in top to
   bottom order and from right to left, and high priority processes lock from
   bottom to top and left to right.

   ZAM-FIXME-HANS: order not just node locks in this way, order atom locks, and
   kill those damn busy loops.
   ANSWER(ZAM): atom locks (which are introduced by ASTAGE_CAPTURE_WAIT atom
   stage) cannot be ordered that way. There are no rules what nodes can belong
   to the atom and what nodes cannot.  We cannot define what is right or left
   direction, what is top or bottom.  We can take immediate parent or side
   neighbor of one node, but nobody guarantees that, say, left neighbor node is
   not a far right neighbor for other nodes from the same atom.  It breaks
   deadlock avoidance rules and hi-low priority locking cannot be applied for
   atom locks.

   How does it help to avoid deadlocks ?

   Suppose we have a deadlock with n processes. Processes from one priority
   class never deadlock because they take locks in one consistent
   order.

   So, any possible deadlock loop must have low priority as well as high
   priority processes.  There are no other lock priority levels except low and
   high. We know that any deadlock loop contains at least one node locked by a
   low priority process and requested by a high priority process. If this
   situation is caught and resolved it is sufficient to avoid deadlocks.

   V4 DEADLOCK PREVENTION ALGORITHM IMPLEMENTATION.

   The deadlock prevention algorithm is based on comparing
   priorities of node owners (processes which keep znode locked) and
   requesters (processes which want to acquire a lock on znode).  We
   implement a scheme where low-priority owners yield locks to
   high-priority requesters. We created a signal passing system that
   is used to ask low-priority processes to yield one or more locked
   znodes.

   The condition when a znode needs to change its owners is described by the
   following formula:

   #############################################
   #                                           #
   # (number of high-priority requesters) >  0 #
   #                AND                        #
   # (numbers of high-priority owners)    == 0 #
   #                                           #
   #############################################

   Note that a low-priority process delays node releasing if another
   high-priority process owns this node.  So, slightly more strictly speaking,
   to have a deadlock capable cycle you must have a loop in which a high
   priority process is waiting on a low priority process to yield a node, which
   is slightly different from saying a high priority process is waiting on a
   node owned by a low priority process.

   It is enough to avoid deadlocks if we prevent any low-priority process from
   falling asleep if its locked set contains a node which satisfies the
   deadlock condition.

   That condition is implicitly or explicitly checked in all places where new
   high-priority requests may be added or removed from node request queue or
   high-priority process takes or releases a lock on node. The main
   goal of these checks is to never lose the moment when node becomes "has
   wrong owners" and send "must-yield-this-lock" signals to its low-pri owners
   at that time.

   The information about received signals is stored in the per-process
   structure (lock stack) and analyzed before a low-priority process goes to
   sleep but after a "fast" attempt to lock a node fails. Any signal wakes
   sleeping process up and forces him to re-check lock status and received
   signal info. If "must-yield-this-lock" signals were received the locking
   primitive (longterm_lock_znode()) fails with -E_DEADLOCK error code.

   V4 LOCKING DRAWBACKS

   If we have already balanced on one level, and we are propagating our changes
   upward to a higher level, it could be very messy to surrender all locks on
   the lower level because we put so much computational work into it, and
   reverting them to their state before they were locked might be very complex.
   We also don't want to acquire all locks before performing balancing because
   that would either be almost as much work as the balancing, or it would be
   too conservative and lock too much.  We want balancing to be done only at
   high priority.  Yet, we might want to go to the left one node and use some
   of its empty space... So we make one attempt at getting the node to the left
   using try_lock, and if it fails we do without it, because we didn't really
   need it, it was only a nice to have.

   LOCK STRUCTURES DESCRIPTION

   The following data structures are used in the reiser4 locking
   implementation:

   All fields related to long-term locking are stored in znode->lock.

   The lock stack is a per thread object.  It owns all znodes locked by the
   thread. One znode may be locked by several threads in case of read lock or
   one znode may be write locked by one thread several times. The special link
   objects (lock handles) support n<->m relation between znodes and lock
   owners.

   <Thread 1>                       <Thread 2>

   +---------+                     +---------+
   |  LS1    |		           |  LS2    |
   +---------+			   +---------+
       ^                                ^
       |---------------+                +----------+
       v               v                v          v
   +---------+      +---------+    +---------+   +---------+
   |  LH1    |      |   LH2   |	   |  LH3    |   |   LH4   |
   +---------+	    +---------+	   +---------+   +---------+
       ^                   ^            ^           ^
       |                   +------------+           |
       v                   v                        v
   +---------+      +---------+                  +---------+
   |  Z1     |	    |	Z2    |                  |  Z3     |
   +---------+	    +---------+                  +---------+

   Thread 1 locked znodes Z1 and Z2, thread 2 locked znodes Z2 and Z3. The
   picture above shows that lock stack LS1 has a list of 2 lock handles LH1 and
   LH2, lock stack LS2 has a list with lock handles LH3 and LH4 on it.  Znode
   Z1 is locked by only one thread, znode has only one lock handle LH1 on its
   list, similar situation is for Z3 which is locked by the thread 2 only. Z2
   is locked (for read) twice by different threads and two lock handles are on
   its list. Each lock handle represents a single relation of a locking of a
   znode by a thread. Locking of a znode is an establishing of a locking
   relation between the lock stack and the znode by adding of a new lock handle
   to a list of lock handles, the lock stack.  The lock stack links all lock
   handles for all znodes locked by the lock stack.  The znode list groups all
   lock handles for all locks stacks which locked the znode.

   Yet another relation may exist between znode and lock owners.  If lock
   procedure cannot immediately take lock on an object it adds the lock owner
   on special `requestors' list belongs to znode.  That list represents a
   queue of pending lock requests.  Because one lock owner may request only
   only one lock object at a time, it is a 1->n relation between lock objects
   and a lock owner implemented as it is described above. Full information
   (priority, pointers to lock and link objects) about each lock request is
   stored in lock owner structure in `request' field.

   SHORT_TERM LOCKING

   This is a list of primitive operations over lock stacks / lock handles /
   znodes and locking descriptions for them.

   1. locking / unlocking which is done by two list insertion/deletion, one
      to/from znode's list of lock handles, another one is to/from lock stack's
      list of lock handles.  The first insertion is protected by
      znode->lock.guard spinlock.  The list owned by the lock stack can be
      modified only by thread who owns the lock stack and nobody else can
      modify/read it. There is nothing to be protected by a spinlock or
      something else.

   2. adding/removing a lock request to/from znode requesters list. The rule is
      that znode->lock.guard spinlock should be taken for this.

   3. we can traverse list of lock handles and use references to lock stacks who
      locked given znode if znode->lock.guard spinlock is taken.

   4. If a lock stack is associated with a znode as a lock requestor or lock
      owner its existence is guaranteed by znode->lock.guard spinlock.  Some its
      (lock stack's) fields should be protected from being accessed in parallel
      by two or more threads. Please look at  lock_stack structure definition
      for the info how those fields are protected. */

/* Znode lock and capturing intertwining. */
/* In current implementation we capture formatted nodes before locking
   them. Take a look on longterm lock znode, reiser4_try_capture() request
   precedes locking requests.  The longterm_lock_znode function unconditionally
   captures znode before even checking of locking conditions.

   Another variant is to capture znode after locking it.  It was not tested, but
   at least one deadlock condition is supposed to be there.  One thread has
   locked a znode (Node-1) and calls reiser4_try_capture() for it.
   reiser4_try_capture() sleeps because znode's atom has CAPTURE_WAIT state.
   Second thread is a flushing thread, its current atom is the atom Node-1
   belongs to. Second thread wants to lock Node-1 and sleeps because Node-1
   is locked by the first thread.  The described situation is a deadlock. */

#include "debug.h"
#include "txnmgr.h"
#include "znode.h"
#include "jnode.h"
#include "tree.h"
#include "plugin/node/node.h"
#include "super.h"

#include <linux/spinlock.h>

#if REISER4_DEBUG
static int request_is_deadlock_safe(znode * , znode_lock_mode,
				    znode_lock_request);
#endif

/* Returns a lock owner associated with current thread */
lock_stack *get_current_lock_stack(void)
{
	return &get_current_context()->stack;
}

/* Wakes up all low priority owners informing them about possible deadlock */
static void wake_up_all_lopri_owners(znode * node)
{
	lock_handle *handle;

	assert_spin_locked(&(node->lock.guard));
	list_for_each_entry(handle, &node->lock.owners, owners_link) {
		assert("nikita-1832", handle->node == node);
		/* count this signal in owner->nr_signaled */
		if (!handle->signaled) {
			handle->signaled = 1;
			atomic_inc(&handle->owner->nr_signaled);
			/* Wake up a single process */
			reiser4_wake_up(handle->owner);
		}
	}
}

/* Adds a lock to a lock owner, which means creating a link to the lock and
   putting the link into the two lists all links are on (the doubly linked list
   that forms the lock_stack, and the doubly linked list of links attached
   to a lock.
*/
static inline void
link_object(lock_handle * handle, lock_stack * owner, znode * node)
{
	assert("jmacd-810", handle->owner == NULL);
	assert_spin_locked(&(node->lock.guard));

	handle->owner = owner;
	handle->node = node;

	assert("reiser4-4",
	       ergo(list_empty_careful(&owner->locks), owner->nr_locks == 0));

	/* add lock handle to the end of lock_stack's list of locks */
	list_add_tail(&handle->locks_link, &owner->locks);
	ON_DEBUG(owner->nr_locks++);
	reiser4_ctx_gfp_mask_set();

	/* add lock handle to the head of znode's list of owners */
	list_add(&handle->owners_link, &node->lock.owners);
	handle->signaled = 0;
}

/* Breaks a relation between a lock and its owner */
static inline void unlink_object(lock_handle * handle)
{
	assert("zam-354", handle->owner != NULL);
	assert("nikita-1608", handle->node != NULL);
	assert_spin_locked(&(handle->node->lock.guard));
	assert("nikita-1829", handle->owner == get_current_lock_stack());
	assert("reiser4-5", handle->owner->nr_locks > 0);

	/* remove lock handle from lock_stack's list of locks */
	list_del(&handle->locks_link);
	ON_DEBUG(handle->owner->nr_locks--);
	reiser4_ctx_gfp_mask_set();
	assert("reiser4-6",
	       ergo(list_empty_careful(&handle->owner->locks),
		    handle->owner->nr_locks == 0));
	/* remove lock handle from znode's list of owners */
	list_del(&handle->owners_link);
	/* indicates that lock handle is free now */
	handle->node = NULL;
#if REISER4_DEBUG
	INIT_LIST_HEAD(&handle->locks_link);
	INIT_LIST_HEAD(&handle->owners_link);
	handle->owner = NULL;
#endif
}

/* Actually locks an object knowing that we are able to do this */
static void lock_object(lock_stack * owner)
{
	struct lock_request *request;
	znode *node;

	request = &owner->request;
	node = request->node;
	assert_spin_locked(&(node->lock.guard));
	if (request->mode == ZNODE_READ_LOCK) {
		node->lock.nr_readers++;
	} else {
		/* check that we don't switched from read to write lock */
		assert("nikita-1840", node->lock.nr_readers <= 0);
		/* We allow recursive locking; a node can be locked several
		   times for write by same process */
		node->lock.nr_readers--;
	}

	link_object(request->handle, owner, node);

	if (owner->curpri)
		node->lock.nr_hipri_owners++;
}

/* Check for recursive write locking */
static int recursive(lock_stack * owner)
{
	int ret;
	znode *node;
	lock_handle *lh;

	node = owner->request.node;

	/* Owners list is not empty for a locked node */
	assert("zam-314", !list_empty_careful(&node->lock.owners));
	assert("nikita-1841", owner == get_current_lock_stack());
	assert_spin_locked(&(node->lock.guard));

	lh = list_entry(node->lock.owners.next, lock_handle, owners_link);
	ret = (lh->owner == owner);

	/* Recursive read locking should be done usual way */
	assert("zam-315", !ret || owner->request.mode == ZNODE_WRITE_LOCK);
	/* mixing of read/write locks is not allowed */
	assert("zam-341", !ret || znode_is_wlocked(node));

	return ret;
}

#if REISER4_DEBUG
/* Returns true if the lock is held by the calling thread. */
int znode_is_any_locked(const znode * node)
{
	lock_handle *handle;
	lock_stack *stack;
	int ret;

	if (!znode_is_locked(node))
		return 0;

	stack = get_current_lock_stack();

	spin_lock_stack(stack);

	ret = 0;

	list_for_each_entry(handle, &stack->locks, locks_link) {
		if (handle->node == node) {
			ret = 1;
			break;
		}
	}

	spin_unlock_stack(stack);

	return ret;
}

#endif

/* Returns true if a write lock is held by the calling thread. */
int znode_is_write_locked(const znode * node)
{
	lock_stack *stack;
	lock_handle *handle;

	assert("jmacd-8765", node != NULL);

	if (!znode_is_wlocked(node))
		return 0;

	stack = get_current_lock_stack();

	/*
	 * When znode is write locked, all owner handles point to the same lock
	 * stack. Get pointer to lock stack from the first lock handle from
	 * znode's owner list
	 */
	handle = list_entry(node->lock.owners.next, lock_handle, owners_link);

	return (handle->owner == stack);
}

/* This "deadlock" condition is the essential part of reiser4 locking
   implementation. This condition is checked explicitly by calling
   check_deadlock_condition() or implicitly in all places where znode lock
   state (set of owners and request queue) is changed. Locking code is
   designed to use this condition to trigger procedure of passing object from
   low priority owner(s) to high priority one(s).

   The procedure results in passing an event (setting lock_handle->signaled
   flag) and counting this event in nr_signaled field of owner's lock stack
   object and wakeup owner's process.
*/
static inline int check_deadlock_condition(znode * node)
{
	assert_spin_locked(&(node->lock.guard));
	return node->lock.nr_hipri_requests > 0
	    && node->lock.nr_hipri_owners == 0;
}

static int check_livelock_condition(znode * node, znode_lock_mode mode)
{
	zlock * lock = &node->lock;

	return mode == ZNODE_READ_LOCK &&
		lock->nr_readers >= 0 && lock->nr_hipri_write_requests > 0;
}

/* checks lock/request compatibility */
static int can_lock_object(lock_stack * owner)
{
	znode *node = owner->request.node;

	assert_spin_locked(&(node->lock.guard));

	/* See if the node is disconnected. */
	if (unlikely(ZF_ISSET(node, JNODE_IS_DYING)))
		return RETERR(-EINVAL);

	/* Do not ever try to take a lock if we are going in low priority
	   direction and a node have a high priority request without high
	   priority owners. */
	if (unlikely(!owner->curpri && check_deadlock_condition(node)))
		return RETERR(-E_REPEAT);
	if (unlikely(owner->curpri &&
		     check_livelock_condition(node, owner->request.mode)))
		return RETERR(-E_REPEAT);
	if (unlikely(!is_lock_compatible(node, owner->request.mode)))
		return RETERR(-E_REPEAT);
	return 0;
}

/* Setting of a high priority to the process. It clears "signaled" flags
   because znode locked by high-priority process can't satisfy our "deadlock
   condition". */
static void set_high_priority(lock_stack * owner)
{
	assert("nikita-1846", owner == get_current_lock_stack());
	/* Do nothing if current priority is already high */
	if (!owner->curpri) {
		/* We don't need locking for owner->locks list, because, this
		 * function is only called with the lock stack of the current
		 * thread, and no other thread can play with owner->locks list
		 * and/or change ->node pointers of lock handles in this list.
		 *
		 * (Interrupts also are not involved.)
		 */
		lock_handle *item = list_entry(owner->locks.next, lock_handle,
					       locks_link);
		while (&owner->locks != &item->locks_link) {
			znode *node = item->node;

			spin_lock_zlock(&node->lock);

			node->lock.nr_hipri_owners++;

			/* we can safely set signaled to zero, because
			   previous statement (nr_hipri_owners ++) guarantees
			   that signaled will be never set again. */
			item->signaled = 0;
			spin_unlock_zlock(&node->lock);

			item = list_entry(item->locks_link.next, lock_handle,
					  locks_link);
		}
		owner->curpri = 1;
		atomic_set(&owner->nr_signaled, 0);
	}
}

/* Sets a low priority to the process. */
static void set_low_priority(lock_stack * owner)
{
	assert("nikita-3075", owner == get_current_lock_stack());
	/* Do nothing if current priority is already low */
	if (owner->curpri) {
		/* scan all locks (lock handles) held by @owner, which is
		   actually current thread, and check whether we are reaching
		   deadlock possibility anywhere.
		 */
		lock_handle *handle = list_entry(owner->locks.next, lock_handle,
						 locks_link);
		while (&owner->locks != &handle->locks_link) {
			znode *node = handle->node;
			spin_lock_zlock(&node->lock);
			/* this thread just was hipri owner of @node, so
			   nr_hipri_owners has to be greater than zero. */
			assert("nikita-1835", node->lock.nr_hipri_owners > 0);
			node->lock.nr_hipri_owners--;
			/* If we have deadlock condition, adjust a nr_signaled
			   field. It is enough to set "signaled" flag only for
			   current process, other low-pri owners will be
			   signaled and waken up after current process unlocks
			   this object and any high-priority requestor takes
			   control. */
			if (check_deadlock_condition(node)
			    && !handle->signaled) {
				handle->signaled = 1;
				atomic_inc(&owner->nr_signaled);
			}
			spin_unlock_zlock(&node->lock);
			handle = list_entry(handle->locks_link.next,
					    lock_handle, locks_link);
		}
		owner->curpri = 0;
	}
}

static void remove_lock_request(lock_stack * requestor)
{
	zlock * lock = &requestor->request.node->lock;

	if (requestor->curpri) {
		assert("nikita-1838", lock->nr_hipri_requests > 0);
		lock->nr_hipri_requests--;
		if (requestor->request.mode == ZNODE_WRITE_LOCK)
			lock->nr_hipri_write_requests--;
	}
	list_del(&requestor->requestors_link);
}

static void invalidate_all_lock_requests(znode * node)
{
	lock_stack *requestor, *tmp;

	assert_spin_locked(&(node->lock.guard));

	list_for_each_entry_safe(requestor, tmp, &node->lock.requestors,
				 requestors_link) {
		remove_lock_request(requestor);
		requestor->request.ret_code = -EINVAL;
		reiser4_wake_up(requestor);
		requestor->request.mode = ZNODE_NO_LOCK;
	}
}

static void dispatch_lock_requests(znode * node)
{
	lock_stack *requestor, *tmp;

	assert_spin_locked(&(node->lock.guard));

	list_for_each_entry_safe(requestor, tmp, &node->lock.requestors,
				 requestors_link) {
		if (znode_is_write_locked(node))
			break;
		if (!can_lock_object(requestor)) {
			lock_object(requestor);
			remove_lock_request(requestor);
			requestor->request.ret_code = 0;
			reiser4_wake_up(requestor);
			requestor->request.mode = ZNODE_NO_LOCK;
		}
	}
}

/* release long-term lock, acquired by longterm_lock_znode() */
void longterm_unlock_znode(lock_handle * handle)
{
	znode *node = handle->node;
	lock_stack *oldowner = handle->owner;
	int hipri;
	int readers;
	int rdelta;
	int youdie;

	/*
	 * this is time-critical and highly optimized code. Modify carefully.
	 */

	assert("jmacd-1021", handle != NULL);
	assert("jmacd-1022", handle->owner != NULL);
	assert("nikita-1392", LOCK_CNT_GTZ(long_term_locked_znode));

	assert("zam-130", oldowner == get_current_lock_stack());

	LOCK_CNT_DEC(long_term_locked_znode);

	/*
	 * to minimize amount of operations performed under lock, pre-compute
	 * all variables used within critical section. This makes code
	 * obscure.
	 */

	/* was this lock of hi or lo priority */
	hipri = oldowner->curpri ? 1 : 0;
	/* number of readers */
	readers = node->lock.nr_readers;
	/* +1 if write lock, -1 if read lock */
	rdelta = (readers > 0) ? -1 : +1;
	/* true if node is to die and write lock is released */
	youdie = ZF_ISSET(node, JNODE_HEARD_BANSHEE) && (readers < 0);

	spin_lock_zlock(&node->lock);

	assert("zam-101", znode_is_locked(node));

	/* Adjust a number of high priority owners of this lock */
	assert("nikita-1836", node->lock.nr_hipri_owners >= hipri);
	node->lock.nr_hipri_owners -= hipri;

	/* Handle znode deallocation on last write-lock release. */
	if (znode_is_wlocked_once(node)) {
		if (youdie) {
			forget_znode(handle);
			assert("nikita-2191", znode_invariant(node));
			zput(node);
			return;
		}
	}

	if (handle->signaled)
		atomic_dec(&oldowner->nr_signaled);

	/* Unlocking means owner<->object link deletion */
	unlink_object(handle);

	/* This is enough to be sure whether an object is completely
	   unlocked. */
	node->lock.nr_readers += rdelta;

	/* If the node is locked it must have an owners list.  Likewise, if
	   the node is unlocked it must have an empty owners list. */
	assert("zam-319", equi(znode_is_locked(node),
			       !list_empty_careful(&node->lock.owners)));

#if REISER4_DEBUG
	if (!znode_is_locked(node))
		++node->times_locked;
#endif

	/* If there are pending lock requests we wake up a requestor */
	if (!znode_is_wlocked(node))
		dispatch_lock_requests(node);
	if (check_deadlock_condition(node))
		wake_up_all_lopri_owners(node);
	spin_unlock_zlock(&node->lock);

	/* minus one reference from handle->node */
	assert("nikita-2190", znode_invariant(node));
	ON_DEBUG(check_lock_data());
	ON_DEBUG(check_lock_node_data(node));
	zput(node);
}

/* final portion of longterm-lock */
static int
lock_tail(lock_stack * owner, int ok, znode_lock_mode mode)
{
	znode *node = owner->request.node;

	assert_spin_locked(&(node->lock.guard));

	/* If we broke with (ok == 0) it means we can_lock, now do it. */
	if (ok == 0) {
		lock_object(owner);
		owner->request.mode = 0;
		/* count a reference from lockhandle->node

		   znode was already referenced at the entry to this function,
		   hence taking spin-lock here is not necessary (see comment
		   in the zref()).
		 */
		zref(node);

		LOCK_CNT_INC(long_term_locked_znode);
	}
	spin_unlock_zlock(&node->lock);
	ON_DEBUG(check_lock_data());
	ON_DEBUG(check_lock_node_data(node));
	return ok;
}

/*
 * version of longterm_znode_lock() optimized for the most common case: read
 * lock without any special flags. This is the kind of lock that any tree
 * traversal takes on the root node of the tree, which is very frequent.
 */
static int longterm_lock_tryfast(lock_stack * owner)
{
	int result;
	znode *node;
	zlock *lock;

	node = owner->request.node;
	lock = &node->lock;

	assert("nikita-3340", reiser4_schedulable());
	assert("nikita-3341", request_is_deadlock_safe(node,
						       ZNODE_READ_LOCK,
						       ZNODE_LOCK_LOPRI));
	spin_lock_zlock(lock);
	result = can_lock_object(owner);
	spin_unlock_zlock(lock);

	if (likely(result != -EINVAL)) {
		spin_lock_znode(node);
		result = reiser4_try_capture(ZJNODE(node), ZNODE_READ_LOCK, 0);
		spin_unlock_znode(node);
		spin_lock_zlock(lock);
		if (unlikely(result != 0)) {
			owner->request.mode = 0;
		} else {
			result = can_lock_object(owner);
			if (unlikely(result == -E_REPEAT)) {
				/* fall back to longterm_lock_znode() */
				spin_unlock_zlock(lock);
				return 1;
			}
		}
		return lock_tail(owner, result, ZNODE_READ_LOCK);
	} else
		return 1;
}

/* locks given lock object */
int longterm_lock_znode(
			       /* local link object (allocated by lock owner
				* thread, usually on its own stack) */
			       lock_handle * handle,
			       /* znode we want to lock. */
			       znode * node,
			       /* {ZNODE_READ_LOCK, ZNODE_WRITE_LOCK}; */
			       znode_lock_mode mode,
			       /* {0, -EINVAL, -E_DEADLOCK}, see return codes
				  description. */
			       znode_lock_request request) {
	int ret;
	int hipri = (request & ZNODE_LOCK_HIPRI) != 0;
	int non_blocking = 0;
	int has_atom;
	txn_capture cap_flags;
	zlock *lock;
	txn_handle *txnh;
	tree_level level;

	/* Get current process context */
	lock_stack *owner = get_current_lock_stack();

	/* Check that the lock handle is initialized and isn't already being
	 * used. */
	assert("jmacd-808", handle->owner == NULL);
	assert("nikita-3026", reiser4_schedulable());
	assert("nikita-3219", request_is_deadlock_safe(node, mode, request));
	assert("zam-1056", atomic_read(&ZJNODE(node)->x_count) > 0);
	/* long term locks are not allowed in the VM contexts (->writepage(),
	 * prune_{d,i}cache()).
	 *
	 * FIXME this doesn't work due to unused-dentry-with-unlinked-inode
	 * bug caused by d_splice_alias() only working for directories.
	 */
	assert("nikita-3547", 1 || ((current->flags & PF_MEMALLOC) == 0));
	assert("zam-1055", mode != ZNODE_NO_LOCK);

	cap_flags = 0;
	if (request & ZNODE_LOCK_NONBLOCK) {
		cap_flags |= TXN_CAPTURE_NONBLOCKING;
		non_blocking = 1;
	}

	if (request & ZNODE_LOCK_DONT_FUSE)
		cap_flags |= TXN_CAPTURE_DONT_FUSE;

	/* If we are changing our process priority we must adjust a number
	   of high priority owners for each znode that we already lock */
	if (hipri) {
		set_high_priority(owner);
	} else {
		set_low_priority(owner);
	}

	level = znode_get_level(node);

	/* Fill request structure with our values. */
	owner->request.mode = mode;
	owner->request.handle = handle;
	owner->request.node = node;

	txnh = get_current_context()->trans;
	lock = &node->lock;

	if (mode == ZNODE_READ_LOCK && request == 0) {
		ret = longterm_lock_tryfast(owner);
		if (ret <= 0)
			return ret;
	}

	has_atom = (txnh->atom != NULL);

	/* Synchronize on node's zlock guard lock. */
	spin_lock_zlock(lock);

	if (znode_is_locked(node) &&
	    mode == ZNODE_WRITE_LOCK && recursive(owner))
		return lock_tail(owner, 0, mode);

	for (;;) {
		/* Check the lock's availability: if it is unavaiable we get
		   E_REPEAT, 0 indicates "can_lock", otherwise the node is
		   invalid.  */
		ret = can_lock_object(owner);

		if (unlikely(ret == -EINVAL)) {
			/* @node is dying. Leave it alone. */
			break;
		}

		if (unlikely(ret == -E_REPEAT && non_blocking)) {
			/* either locking of @node by the current thread will
			 * lead to the deadlock, or lock modes are
			 * incompatible. */
			break;
		}

		assert("nikita-1844", (ret == 0)
		       || ((ret == -E_REPEAT) && !non_blocking));
		/* If we can get the lock... Try to capture first before
		   taking the lock. */

		/* first handle commonest case where node and txnh are already
		 * in the same atom. */
		/* safe to do without taking locks, because:
		 *
		 * 1. read of aligned word is atomic with respect to writes to
		 * this word
		 *
		 * 2. false negatives are handled in reiser4_try_capture().
		 *
		 * 3. false positives are impossible.
		 *
		 * PROOF: left as an exercise to the curious reader.
		 *
		 * Just kidding. Here is one:
		 *
		 * At the time T0 txnh->atom is stored in txnh_atom.
		 *
		 * At the time T1 node->atom is stored in node_atom.
		 *
		 * At the time T2 we observe that
		 *
		 *     txnh_atom != NULL && node_atom == txnh_atom.
		 *
		 * Imagine that at this moment we acquire node and txnh spin
		 * lock in this order. Suppose that under spin lock we have
		 *
		 *     node->atom != txnh->atom,                       (S1)
		 *
		 * at the time T3.
		 *
		 * txnh->atom != NULL still, because txnh is open by the
		 * current thread.
		 *
		 * Suppose node->atom == NULL, that is, node was un-captured
		 * between T1, and T3. But un-capturing of formatted node is
		 * always preceded by the call to reiser4_invalidate_lock(),
		 * which marks znode as JNODE_IS_DYING under zlock spin
		 * lock. Contradiction, because can_lock_object() above checks
		 * for JNODE_IS_DYING. Hence, node->atom != NULL at T3.
		 *
		 * Suppose that node->atom != node_atom, that is, atom, node
		 * belongs to was fused into another atom: node_atom was fused
		 * into node->atom. Atom of txnh was equal to node_atom at T2,
		 * which means that under spin lock, txnh->atom == node->atom,
		 * because txnh->atom can only follow fusion
		 * chain. Contradicts S1.
		 *
		 * The same for hypothesis txnh->atom != txnh_atom. Hence,
		 * node->atom == node_atom == txnh_atom == txnh->atom. Again
		 * contradicts S1. Hence S1 is false. QED.
		 *
		 */

		if (likely(has_atom && ZJNODE(node)->atom == txnh->atom)) {
			;
		} else {
			/*
			 * unlock zlock spin lock here. It is possible for
			 * longterm_unlock_znode() to sneak in here, but there
			 * is no harm: reiser4_invalidate_lock() will mark znode
			 * as JNODE_IS_DYING and this will be noted by
			 * can_lock_object() below.
			 */
			spin_unlock_zlock(lock);
			spin_lock_znode(node);
			ret = reiser4_try_capture(ZJNODE(node), mode,
						  cap_flags);
			spin_unlock_znode(node);
			spin_lock_zlock(lock);
			if (unlikely(ret != 0)) {
				/* In the failure case, the txnmgr releases
				   the znode's lock (or in some cases, it was
				   released a while ago).  There's no need to
				   reacquire it so we should return here,
				   avoid releasing the lock. */
				owner->request.mode = 0;
				break;
			}

			/* Check the lock's availability again -- this is
			   because under some circumstances the capture code
			   has to release and reacquire the znode spinlock. */
			ret = can_lock_object(owner);
		}

		/* This time, a return of (ret == 0) means we can lock, so we
		   should break out of the loop. */
		if (likely(ret != -E_REPEAT || non_blocking))
			break;

		/* Lock is unavailable, we have to wait. */
		ret = reiser4_prepare_to_sleep(owner);
		if (unlikely(ret != 0))
			break;

		assert_spin_locked(&(node->lock.guard));
		if (hipri) {
			/* If we are going in high priority direction then
			   increase high priority requests counter for the
			   node */
			lock->nr_hipri_requests++;
			if (mode == ZNODE_WRITE_LOCK)
				lock->nr_hipri_write_requests++;
			/* If there are no high priority owners for a node,
			   then immediately wake up low priority owners, so
			   they can detect possible deadlock */
			if (lock->nr_hipri_owners == 0)
				wake_up_all_lopri_owners(node);
		}
		list_add_tail(&owner->requestors_link, &lock->requestors);

		/* Ok, here we have prepared a lock request, so unlock
		   a znode ... */
		spin_unlock_zlock(lock);
		/* ... and sleep */
		reiser4_go_to_sleep(owner);
		if (owner->request.mode == ZNODE_NO_LOCK)
			goto request_is_done;
		spin_lock_zlock(lock);
		if (owner->request.mode == ZNODE_NO_LOCK) {
			spin_unlock_zlock(lock);
request_is_done:
			if (owner->request.ret_code == 0) {
				LOCK_CNT_INC(long_term_locked_znode);
				zref(node);
			}
			return owner->request.ret_code;
		}
		remove_lock_request(owner);
	}

	return lock_tail(owner, ret, mode);
}

/* lock object invalidation means changing of lock object state to `INVALID'
   and waiting for all other processes to cancel theirs lock requests. */
void reiser4_invalidate_lock(lock_handle * handle	/* path to lock
							 * owner and lock
							 * object is being
							 * invalidated. */ )
{
	znode *node = handle->node;
	lock_stack *owner = handle->owner;

	assert("zam-325", owner == get_current_lock_stack());
	assert("zam-103", znode_is_write_locked(node));
	assert("nikita-1393", !ZF_ISSET(node, JNODE_LEFT_CONNECTED));
	assert("nikita-1793", !ZF_ISSET(node, JNODE_RIGHT_CONNECTED));
	assert("nikita-1394", ZF_ISSET(node, JNODE_HEARD_BANSHEE));
	assert("nikita-3097", znode_is_wlocked_once(node));
	assert_spin_locked(&(node->lock.guard));

	if (handle->signaled)
		atomic_dec(&owner->nr_signaled);

	ZF_SET(node, JNODE_IS_DYING);
	unlink_object(handle);
	node->lock.nr_readers = 0;

	invalidate_all_lock_requests(node);
	spin_unlock_zlock(&node->lock);
}

/* Initializes lock_stack. */
void init_lock_stack(lock_stack * owner	/* pointer to
					 * allocated
					 * structure. */ )
{
	INIT_LIST_HEAD(&owner->locks);
	INIT_LIST_HEAD(&owner->requestors_link);
	spin_lock_init(&owner->sguard);
	owner->curpri = 1;
	init_waitqueue_head(&owner->wait);
}

/* Initializes lock object. */
void reiser4_init_lock(zlock * lock	/* pointer on allocated
					 * uninitialized lock object
					 * structure. */ )
{
	memset(lock, 0, sizeof(zlock));
	spin_lock_init(&lock->guard);
	INIT_LIST_HEAD(&lock->requestors);
	INIT_LIST_HEAD(&lock->owners);
}

/* Transfer a lock handle (presumably so that variables can be moved between
   stack and heap locations). */
static void
move_lh_internal(lock_handle * new, lock_handle * old, int unlink_old)
{
	znode *node = old->node;
	lock_stack *owner = old->owner;
	int signaled;

	/* locks_list, modified by link_object() is not protected by
	   anything. This is valid because only current thread ever modifies
	   locks_list of its lock_stack.
	 */
	assert("nikita-1827", owner == get_current_lock_stack());
	assert("nikita-1831", new->owner == NULL);

	spin_lock_zlock(&node->lock);

	signaled = old->signaled;
	if (unlink_old) {
		unlink_object(old);
	} else {
		if (node->lock.nr_readers > 0) {
			node->lock.nr_readers += 1;
		} else {
			node->lock.nr_readers -= 1;
		}
		if (signaled)
			atomic_inc(&owner->nr_signaled);
		if (owner->curpri)
			node->lock.nr_hipri_owners += 1;
		LOCK_CNT_INC(long_term_locked_znode);

		zref(node);
	}
	link_object(new, owner, node);
	new->signaled = signaled;

	spin_unlock_zlock(&node->lock);
}

void move_lh(lock_handle * new, lock_handle * old)
{
	move_lh_internal(new, old, /*unlink_old */ 1);
}

void copy_lh(lock_handle * new, lock_handle * old)
{
	move_lh_internal(new, old, /*unlink_old */ 0);
}

/* after getting -E_DEADLOCK we unlock znodes until this function returns false
 */
int reiser4_check_deadlock(void)
{
	lock_stack *owner = get_current_lock_stack();
	return atomic_read(&owner->nr_signaled) != 0;
}

/* Before going to sleep we re-check "release lock" requests which might come
   from threads with hi-pri lock priorities. */
int reiser4_prepare_to_sleep(lock_stack * owner)
{
	assert("nikita-1847", owner == get_current_lock_stack());

	/* We return -E_DEADLOCK if one or more "give me the lock" messages are
	 * counted in nr_signaled */
	if (unlikely(atomic_read(&owner->nr_signaled) != 0)) {
		assert("zam-959", !owner->curpri);
		return RETERR(-E_DEADLOCK);
	}
	return 0;
}

/* Wakes up a single thread */
void __reiser4_wake_up(lock_stack * owner)
{
	atomic_set(&owner->wakeup, 1);
	wake_up(&owner->wait);
}

/* Puts a thread to sleep */
void reiser4_go_to_sleep(lock_stack * owner)
{
	/* Well, we might sleep here, so holding of any spinlocks is no-no */
	assert("nikita-3027", reiser4_schedulable());

	wait_event(owner->wait, atomic_read(&owner->wakeup));
	atomic_set(&owner->wakeup, 0);
}

int lock_stack_isclean(lock_stack * owner)
{
	if (list_empty_careful(&owner->locks)) {
		assert("zam-353", atomic_read(&owner->nr_signaled) == 0);
		return 1;
	}

	return 0;
}

#if REISER4_DEBUG

/*
 * debugging functions
 */

static void list_check(struct list_head *head)
{
	struct list_head *pos;

	list_for_each(pos, head)
		assert("", (pos->prev != NULL && pos->next != NULL &&
			    pos->prev->next == pos && pos->next->prev == pos));
}

/* check consistency of locking data-structures hanging of the @stack */
static void check_lock_stack(lock_stack * stack)
{
	spin_lock_stack(stack);
	/* check that stack->locks is not corrupted */
	list_check(&stack->locks);
	spin_unlock_stack(stack);
}

/* check consistency of locking data structures */
void check_lock_data(void)
{
	check_lock_stack(&get_current_context()->stack);
}

/* check consistency of locking data structures for @node */
void check_lock_node_data(znode * node)
{
	spin_lock_zlock(&node->lock);
	list_check(&node->lock.owners);
	list_check(&node->lock.requestors);
	spin_unlock_zlock(&node->lock);
}

/* check that given lock request is dead lock safe. This check is, of course,
 * not exhaustive. */
static int
request_is_deadlock_safe(znode * node, znode_lock_mode mode,
			 znode_lock_request request)
{
	lock_stack *owner;

	owner = get_current_lock_stack();
	/*
	 * check that hipri lock request is not issued when there are locked
	 * nodes at the higher levels.
	 */
	if (request & ZNODE_LOCK_HIPRI && !(request & ZNODE_LOCK_NONBLOCK) &&
	    znode_get_level(node) != 0) {
		lock_handle *item;

		list_for_each_entry(item, &owner->locks, locks_link) {
			znode *other;

			other = item->node;

			if (znode_get_level(other) == 0)
				continue;
			if (znode_get_level(other) > znode_get_level(node))
				return 0;
		}
	}
	return 1;
}

#endif

/* return pointer to static storage with name of lock_mode. For
    debugging */
const char *lock_mode_name(znode_lock_mode lock/* lock mode to get name of */)
{
	if (lock == ZNODE_READ_LOCK)
		return "read";
	else if (lock == ZNODE_WRITE_LOCK)
		return "write";
	else {
		static char buf[30];

		sprintf(buf, "unknown: %i", lock);
		return buf;
	}
}

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 79
   End:
*/
