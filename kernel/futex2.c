// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * futex2 system call interface by André Almeida <andrealmeid@collabora.com>
 *
 * Copyright 2021 Collabora Ltd.
 *
 * Based on original futex implementation by:
 *  (C) 2002 Rusty Russell, IBM
 *  (C) 2003, 2006 Ingo Molnar, Red Hat Inc.
 *  (C) 2003, 2004 Jamie Lokier
 *  (C) 2006 Thomas Gleixner, Timesys Corp.
 *  (C) 2007 Eric Dumazet
 *  (C) 2009 Darren Hart, IBM
 */

#include <linux/freezer.h>
#include <linux/jhash.h>
#include <linux/memblock.h>
#include <linux/sched/wake_q.h>
#include <linux/spinlock.h>
#include <linux/syscalls.h>
#include <uapi/linux/futex.h>

/**
 * struct futex_key - Components to build unique key for a futex
 * @pointer: Pointer to current->mm
 * @index: Start address of the page containing futex
 * @offset: Address offset of uaddr in a page
 */
struct futex_key {
	u64 pointer;
	unsigned long index;
	unsigned long offset;
};

/**
 * struct futex_waiter - List entry for a waiter
 * @uaddr:        Virtual address of userspace futex
 * @key:          Information that uniquely identify a futex
 * @list:	  List node struct
 * @val:	  Expected value for this waiter
 * @flags:        Flags
 * @bucket:       Pointer to the bucket for this waiter
 * @index:        Index of waiter in futexv list
 */
struct futex_waiter {
	uintptr_t uaddr;
	struct futex_key key;
	struct list_head list;
	unsigned int val;
	unsigned int flags;
	struct futex_bucket *bucket;
	unsigned int index;
};

/**
 * struct futexv_head - List of futexes to be waited
 * @task:    Task to be awaken
 * @hint:    Was someone on this list awakened?
 * @objects: List of futexes
 */
struct futexv_head {
	struct task_struct *task;
	bool hint;
	struct futex_waiter objects[0];
};

/**
 * struct futex_bucket - A bucket of futex's hash table
 * @waiters: Number of waiters in the bucket
 * @lock:    Bucket lock
 * @list:    List of waiters on this bucket
 */
struct futex_bucket {
	atomic_t waiters;
	spinlock_t lock;
	struct list_head list;
};

/**
 * struct futex_single_waiter - Wrapper for a futexv_head of one element
 * @futexv: Single futexv element
 * @waiter: Single waiter element
 */
struct futex_single_waiter {
	struct futexv_head futexv;
	struct futex_waiter waiter;
} __packed;

/* Mask for futex2 flag operations */
#define FUTEX2_MASK (FUTEX_SIZE_MASK | FUTEX_SHARED_FLAG | \
		     FUTEX_CLOCK_REALTIME)

/* Mask for sys_futex_waitv flag */
#define FUTEXV_MASK (FUTEX_CLOCK_REALTIME)

/* Mask for each futex in futex_waitv list */
#define FUTEXV_WAITER_MASK (FUTEX_SIZE_MASK | FUTEX_SHARED_FLAG)

struct futex_bucket *futex_table;
unsigned int futex2_hashsize;

/*
 * Reflects a new waiter being added to the waitqueue.
 */
static inline void bucket_inc_waiters(struct futex_bucket *bucket)
{
#ifdef CONFIG_SMP
	atomic_inc(&bucket->waiters);
	/*
	 * Issue a barrier after adding so futex_wake() will see that the
	 * value had increased
	 */
	smp_mb__after_atomic();
#endif
}

/*
 * Reflects a waiter being removed from the waitqueue by wakeup
 * paths.
 */
static inline void bucket_dec_waiters(struct futex_bucket *bucket)
{
#ifdef CONFIG_SMP
	atomic_dec(&bucket->waiters);
#endif
}

/*
 * Get the number of waiters in a bucket
 */
static inline int bucket_get_waiters(struct futex_bucket *bucket)
{
#ifdef CONFIG_SMP
	/*
	 * Issue a barrier before reading so we get an updated value from
	 * futex_wait()
	 */
	smp_mb();
	return atomic_read(&bucket->waiters);
#else
	return 1;
#endif
}

/**
 * futex_get_bucket - Check if the user address is valid, prepare internal
 *                    data and calculate the hash
 * @uaddr:   futex user address
 * @key:     data that uniquely identifies a futex
 *
 * Return: address of bucket on success, error code otherwise
 */
static struct futex_bucket *futex_get_bucket(void __user *uaddr,
					     struct futex_key *key)
{
	uintptr_t address = (uintptr_t)uaddr;
	u32 hash_key;

	/* Checking if uaddr is valid and accessible */
	if (unlikely(!IS_ALIGNED(address, sizeof(u32))))
		return ERR_PTR(-EINVAL);
	if (unlikely(!access_ok(address, sizeof(u32))))
		return ERR_PTR(-EFAULT);

	key->offset = address % PAGE_SIZE;
	address -= key->offset;
	key->pointer = (u64)address;
	key->index = (unsigned long)current->mm;

	/* Generate hash key for this futex using uaddr and current->mm */
	hash_key = jhash2((u32 *)key, sizeof(*key) / sizeof(u32), 0);

	/* Since HASH_SIZE is 2^n, subtracting 1 makes a perfect bit mask */
	return &futex_table[hash_key & (futex2_hashsize - 1)];
}

/**
 * futex_get_user - Get the userspace value on this address
 * @uval:  variable to store the value
 * @uaddr: userspace address
 *
 * Check the comment at futex_enqueue() for more information.
 */
static int futex_get_user(u32 *uval, u32 __user *uaddr)
{
	int ret;

	pagefault_disable();
	ret = __get_user(*uval, uaddr);
	pagefault_enable();

	return ret;
}

/**
 * futex_setup_time - Prepare the timeout mechanism and start it.
 * @timo:    Timeout value from userspace
 * @timeout: Pointer to hrtimer handler
 * @flags: Flags from userspace, to decide which clockid to use
 *
 * Return: 0 on success, error code otherwise
 */
static int futex_setup_time(struct __kernel_timespec __user *timo,
			    struct hrtimer_sleeper *timeout,
			    unsigned int flags)
{
	ktime_t time;
	struct timespec64 ts;
	clockid_t clockid = (flags & FUTEX_CLOCK_REALTIME) ?
			    CLOCK_REALTIME : CLOCK_MONOTONIC;

	if (get_timespec64(&ts, timo))
		return -EFAULT;

	if (!timespec64_valid(&ts))
		return -EINVAL;

	time = timespec64_to_ktime(ts);

	hrtimer_init_sleeper(timeout, clockid, HRTIMER_MODE_ABS);

	hrtimer_set_expires(&timeout->timer, time);

	hrtimer_sleeper_start_expires(timeout, HRTIMER_MODE_ABS);

	return 0;
}

/**
 * futex_dequeue_multiple - Remove multiple futexes from hash table
 * @futexv: list of waiters
 * @nr:     number of futexes to be removed
 *
 * This function is used if (a) something went wrong while enqueuing, and we
 * need to undo our work (then nr <= nr_futexes) or (b) we woke up, and thus
 * need to remove every waiter, check if some was indeed woken and return.
 * Before removing a waiter, we check if it's on the list, since we have no
 * clue who have been waken.
 *
 * Return:
 *  * -1  - If no futex was woken during the removal
 *  * 0>= - At least one futex was found woken, index of the last one
 */
static int futex_dequeue_multiple(struct futexv_head *futexv, unsigned int nr)
{
	int i, ret = -1;

	for (i = 0; i < nr; i++) {
		spin_lock(&futexv->objects[i].bucket->lock);
		if (!list_empty_careful(&futexv->objects[i].list)) {
			list_del_init_careful(&futexv->objects[i].list);
			bucket_dec_waiters(futexv->objects[i].bucket);
		} else {
			ret = i;
		}
		spin_unlock(&futexv->objects[i].bucket->lock);
	}

	return ret;
}

/**
 * futex_enqueue - Check the value and enqueue a futex on a wait list
 *
 * @futexv:     List of futexes
 * @nr_futexes: Number of futexes in the list
 * @awakened:	If a futex was awakened during enqueueing, store the index here
 *
 * Get the value from the userspace address and compares with the expected one.
 *
 * Getting the value from user futex address:
 *
 * Since we are in a hurry, we use a spin lock and we can't sleep.
 * Try to get the value with page fault disabled (when enable, we might
 * sleep).
 *
 * If we fail, we aren't sure if the address is invalid or is just a
 * page fault. Then, release the lock (so we can sleep) and try to get
 * the value with page fault enabled. In order to trigger a page fault
 * handling, we just call __get_user() again. If we sleep with enqueued
 * futexes, we might miss a wake, so dequeue everything before sleeping.
 *
 * If get_user succeeds, this mean that the address is valid and we do
 * the work again. Since we just handled the page fault, the page is
 * likely pinned in memory and we should be luckier this time and be
 * able to get the value. If we fail anyway, we will try again.
 *
 * If even with page faults enabled we get and error, this means that
 * the address is not valid and we return from the syscall.
 *
 * If we got an unexpected value or need to treat a page fault and realized that
 * a futex was awakened, we can priority this and return success.
 *
 * In success, enqueue the futex in the correct bucket
 *
 * Return:
 * * 1  - We were awake in the process and nothing is enqueued
 * * 0  - Everything is enqueued and we are ready to sleep
 * * 0< - Something went wrong, nothing is enqueued, return error code
 */
static int futex_enqueue(struct futexv_head *futexv, unsigned int nr_futexes,
			 int *awakened)
{
	int i, ret;
	u32 uval, *uaddr, val;
	struct futex_bucket *bucket;

retry:
	set_current_state(TASK_INTERRUPTIBLE);

	for (i = 0; i < nr_futexes; i++) {
		uaddr = (u32 * __user)futexv->objects[i].uaddr;
		val = (u32)futexv->objects[i].val;

		bucket = futexv->objects[i].bucket;

		bucket_inc_waiters(bucket);
		spin_lock(&bucket->lock);

		ret = futex_get_user(&uval, uaddr);

		if (unlikely(ret)) {
			spin_unlock(&bucket->lock);

			bucket_dec_waiters(bucket);
			__set_current_state(TASK_RUNNING);
			*awakened = futex_dequeue_multiple(futexv, i);

			if (__get_user(uval, uaddr))
				return -EFAULT;

			if (*awakened >= 0)
				return 1;

			goto retry;
		}

		if (uval != val) {
			spin_unlock(&bucket->lock);

			bucket_dec_waiters(bucket);
			__set_current_state(TASK_RUNNING);
			*awakened = futex_dequeue_multiple(futexv, i);

			if (*awakened >= 0)
				return 1;

			return -EAGAIN;
		}

		list_add_tail(&futexv->objects[i].list, &bucket->list);
		spin_unlock(&bucket->lock);
	}

	return 0;
}

/**
 * __futex_wait - Enqueue the list of futexes and wait to be woken
 * @futexv: List of futexes to wait
 * @nr_futexes: Length of futexv
 * @timeout: Pointer to timeout handler
 *
 * Return:
 * * 0 >= - Hint of which futex woke us
 * * 0 <  - Error code
 */
static int __futex_wait(struct futexv_head *futexv, unsigned int nr_futexes,
			struct hrtimer_sleeper *timeout)
{
	int ret;

	while (1) {
		int awakened = -1;

		ret = futex_enqueue(futexv, nr_futexes, &awakened);

		if (ret) {
			if (awakened >= 0)
				return awakened;
			return ret;
		}

		/* Before sleeping, check if someone was woken */
		if (!futexv->hint && (!timeout || timeout->task))
			freezable_schedule();

		__set_current_state(TASK_RUNNING);

		/*
		 * One of those things triggered this wake:
		 *
		 * * We have been removed from the bucket. futex_wake() woke
		 *   us. We just need to dequeue and return 0 to userspace.
		 *
		 * However, if no futex was dequeued by a futex_wake():
		 *
		 * * If the there's a timeout and it has expired,
		 *   return -ETIMEDOUT.
		 *
		 * * If there is a signal pending, something wants to kill our
		 *   thread, return -ERESTARTSYS.
		 *
		 * * If there's no signal pending, it was a spurious wake
		 *   (scheduler gave us a change to do some work, even if we
		 *   don't want to). We need to remove ourselves from the
		 *   bucket and add again, to prevent losing wakeups in the
		 *   meantime.
		 */

		ret = futex_dequeue_multiple(futexv, nr_futexes);

		/* Normal wake */
		if (ret >= 0)
			return ret;

		if (timeout && !timeout->task)
			return -ETIMEDOUT;

		if (signal_pending(current))
			return -ERESTARTSYS;

		/* Spurious wake, do everything again */
	}
}

/**
 * futex_wait - Setup the timer (if there's one) and wait on a list of futexes
 * @futexv:     List of futexes
 * @nr_futexes: Length of futexv
 * @timo:	Timeout
 * @flags:	Timeout flags
 *
 * Return:
 * * 0 >= - Hint of which futex woke us
 * * 0 <  - Error code
 */
static int futex_set_timer_and_wait(struct futexv_head *futexv,
				    unsigned int nr_futexes,
				    struct __kernel_timespec __user *timo,
				    unsigned int flags)
{
	struct hrtimer_sleeper timeout;
	int ret;

	if (timo) {
		ret = futex_setup_time(timo, &timeout, flags);
		if (ret)
			return ret;
	}

	ret = __futex_wait(futexv, nr_futexes, timo ? &timeout : NULL);

	if (timo)
		hrtimer_cancel(&timeout.timer);

	return ret;
}

/**
 * sys_futex_wait - Wait on a futex address if (*uaddr) == val
 * @uaddr: User address of futex
 * @val:   Expected value of futex
 * @flags: Specify the size of futex and the clockid
 * @timo:  Optional absolute timeout.
 *
 * The user thread is put to sleep, waiting for a futex_wake() at uaddr, if the
 * value at *uaddr is the same as val (otherwise, the syscall returns
 * immediately with -EAGAIN).
 *
 * Returns 0 on success, error code otherwise.
 */
SYSCALL_DEFINE4(futex_wait, void __user *, uaddr, unsigned int, val,
		unsigned int, flags, struct __kernel_timespec __user *, timo)
{
	unsigned int size = flags & FUTEX_SIZE_MASK;
	struct futex_single_waiter wait_single = {0};
	struct futex_waiter *waiter;
	struct futexv_head *futexv;

	if (flags & ~FUTEX2_MASK)
		return -EINVAL;

	if (size != FUTEX_32)
		return -EINVAL;

	futexv = &wait_single.futexv;
	futexv->task = current;
	futexv->hint = false;

	waiter = &wait_single.waiter;
	waiter->index = 0;
	waiter->val = val;
	waiter->uaddr = (uintptr_t)uaddr;

	INIT_LIST_HEAD(&waiter->list);

	/* Get an unlocked hash bucket */
	waiter->bucket = futex_get_bucket(uaddr, &waiter->key);
	if (IS_ERR(waiter->bucket))
		return PTR_ERR(waiter->bucket);

	return futex_set_timer_and_wait(futexv, 1, timo, flags);
}

/**
 * futex_get_parent - For a given futex in a futexv list, get a pointer to the futexv
 * @waiter: Address of futex in the list
 * @index: Index of futex in the list
 *
 * Return: A pointer to its futexv struct
 */
static inline struct futexv_head *futex_get_parent(uintptr_t waiter,
						   unsigned int index)
{
	uintptr_t parent = waiter - sizeof(struct futexv_head)
			   - (uintptr_t)(index * sizeof(struct futex_waiter));

	return (struct futexv_head *)parent;
}

/**
 * futex_mark_wake - Find the task to be wake and add it in wake queue
 * @waiter: Waiter to be wake
 * @bucket: Bucket to be decremented
 * @wake_q: Wake queue to insert the task
 */
static void futex_mark_wake(struct futex_waiter *waiter,
			    struct futex_bucket *bucket,
			    struct wake_q_head *wake_q)
{
	struct task_struct *task;
	struct futexv_head *parent = futex_get_parent((uintptr_t)waiter,
						      waiter->index);

	parent->hint = true;
	task = parent->task;
	get_task_struct(task);
	list_del_init_careful(&waiter->list);
	wake_q_add_safe(wake_q, task);
	bucket_dec_waiters(bucket);
}

static inline bool futex_match(struct futex_key key1, struct futex_key key2)
{
	return (key1.index == key2.index &&
		key1.pointer == key2.pointer &&
		key1.offset == key2.offset);
}

/**
 * sys_futex_wake - Wake a number of futexes waiting on an address
 * @uaddr:   Address of futex to be woken up
 * @nr_wake: Number of futexes waiting in uaddr to be woken up
 * @flags:   Flags for size and shared
 *
 * Wake `nr_wake` threads waiting at uaddr.
 *
 * Returns the number of woken threads on success, error code otherwise.
 */
SYSCALL_DEFINE3(futex_wake, void __user *, uaddr, unsigned int, nr_wake,
		unsigned int, flags)
{
	unsigned int size = flags & FUTEX_SIZE_MASK;
	struct futex_waiter waiter, *aux, *tmp;
	struct futex_bucket *bucket;
	DEFINE_WAKE_Q(wake_q);
	int ret = 0;

	if (flags & ~FUTEX2_MASK)
		return -EINVAL;

	if (size != FUTEX_32)
		return -EINVAL;

	bucket = futex_get_bucket(uaddr, &waiter.key);
	if (IS_ERR(bucket))
		return PTR_ERR(bucket);

	if (!bucket_get_waiters(bucket) || !nr_wake)
		return 0;

	spin_lock(&bucket->lock);
	list_for_each_entry_safe(aux, tmp, &bucket->list, list) {
		if (futex_match(waiter.key, aux->key)) {
			futex_mark_wake(aux, bucket, &wake_q);
			if (++ret >= nr_wake)
				break;
		}
	}
	spin_unlock(&bucket->lock);

	wake_up_q(&wake_q);

	return ret;
}

static int __init futex2_init(void)
{
	int i;
	unsigned int futex_shift;

#if CONFIG_BASE_SMALL
	futex2_hashsize = 16;
#else
	futex2_hashsize = roundup_pow_of_two(256 * num_possible_cpus());
#endif

	futex_table = alloc_large_system_hash("futex2", sizeof(struct futex_bucket),
					      futex2_hashsize, 0,
					      futex2_hashsize < 256 ? HASH_SMALL : 0,
					      &futex_shift, NULL,
					      futex2_hashsize, futex2_hashsize);
	futex2_hashsize = 1UL << futex_shift;

	for (i = 0; i < futex2_hashsize; i++) {
		INIT_LIST_HEAD(&futex_table[i].list);
		spin_lock_init(&futex_table[i].lock);
		atomic_set(&futex_table[i].waiters, 0);
	}

	return 0;
}
core_initcall(futex2_init);
