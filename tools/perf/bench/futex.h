/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Glibc independent futex library for testing kernel functionality.
 * Shamelessly stolen from Darren Hart <dvhltc@us.ibm.com>
 *    http://git.kernel.org/cgit/linux/kernel/git/dvhart/futextest.git/
 */

#ifndef _FUTEX_H
#define _FUTEX_H

#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <linux/futex.h>

/**
 * futex() - SYS_futex syscall wrapper
 * @uaddr:	address of first futex
 * @op:		futex op code
 * @val:	typically expected value of uaddr, but varies by op
 * @timeout:	typically an absolute struct timespec (except where noted
 *		otherwise). Overloaded by some ops
 * @uaddr2:	address of second futex for some ops\
 * @val3:	varies by op
 * @opflags:	flags to be bitwise OR'd with op, such as FUTEX_PRIVATE_FLAG
 *
 * futex() is used by all the following futex op wrappers. It can also be
 * used for misuse and abuse testing. Generally, the specific op wrappers
 * should be used instead. It is a macro instead of an static inline function as
 * some of the types over overloaded (timeout is used for nr_requeue for
 * example).
 *
 * These argument descriptions are the defaults for all
 * like-named arguments in the following wrappers except where noted below.
 */
#define futex(uaddr, op, val, timeout, uaddr2, val3, opflags) \
	syscall(SYS_futex, uaddr, op | opflags, val, timeout, uaddr2, val3)

/**
 * futex_wait() - block on uaddr with optional timeout
 * @timeout:	relative timeout
 */
static inline int
futex_wait(u_int32_t *uaddr, u_int32_t val, struct timespec *timeout, int opflags)
{
	return futex(uaddr, FUTEX_WAIT, val, timeout, NULL, 0, opflags);
}

/**
 * futex_wake() - wake one or more tasks blocked on uaddr
 * @nr_wake:	wake up to this many tasks
 */
static inline int
futex_wake(u_int32_t *uaddr, int nr_wake, int opflags)
{
	return futex(uaddr, FUTEX_WAKE, nr_wake, NULL, NULL, 0, opflags);
}

/**
 * futex_lock_pi() - block on uaddr as a PI mutex
 */
static inline int
futex_lock_pi(u_int32_t *uaddr, struct timespec *timeout, int opflags)
{
	return futex(uaddr, FUTEX_LOCK_PI, 0, timeout, NULL, 0, opflags);
}

/**
 * futex_unlock_pi() - release uaddr as a PI mutex, waking the top waiter
 */
static inline int
futex_unlock_pi(u_int32_t *uaddr, int opflags)
{
	return futex(uaddr, FUTEX_UNLOCK_PI, 0, NULL, NULL, 0, opflags);
}

/**
* futex_cmp_requeue() - requeue tasks from uaddr to uaddr2
* @nr_wake:        wake up to this many tasks
* @nr_requeue:        requeue up to this many tasks
*/
static inline int
futex_cmp_requeue(u_int32_t *uaddr, u_int32_t val, u_int32_t *uaddr2, int nr_wake,
		 int nr_requeue, int opflags)
{
	return futex(uaddr, FUTEX_CMP_REQUEUE, nr_wake, nr_requeue, uaddr2,
		 val, opflags);
}

/**
 * futex2_wait - Wait at uaddr if *uaddr == val, until timo.
 * @uaddr: User address to wait for
 * @val:   Expected value at uaddr
 * @flags: Operation options
 * @timo:  Optional timeout
 *
 * Return: 0 on success, error code otherwise
 */
static inline int futex2_wait(volatile void *uaddr, unsigned long val,
			      unsigned long flags, struct timespec *timo)
{
	return syscall(__NR_futex_wait, uaddr, val, flags, timo);
}

/**
 * futex2_wake - Wake a number of waiters waiting at uaddr
 * @uaddr: Address to wake
 * @nr:    Number of waiters to wake
 * @flags: Operation options
 *
 * Return: number of waked futexes
 */
static inline int futex2_wake(volatile void *uaddr, unsigned int nr, unsigned long flags)
{
	return syscall(__NR_futex_wake, uaddr, nr, flags);
}

/**
 * futex2_requeue - Requeue waiters from an address to another one
 * @uaddr1:     Address where waiters are currently waiting on
 * @uaddr2:     New address to wait
 * @nr_wake:    Number of waiters at uaddr1 to be wake
 * @nr_requeue: After waking nr_wake, number of waiters to be requeued
 * @cmpval:     Expected value at uaddr1
 * @flags: Operation options
 *
 * Return: waked futexes + requeued futexes at uaddr1
 */
static inline int futex2_requeue(volatile struct futex_requeue *uaddr1,
				 volatile struct futex_requeue *uaddr2,
				 unsigned int nr_wake, unsigned int nr_requeue,
				 unsigned int cmpval, unsigned long flags)
{
	return syscall(__NR_futex_requeue, uaddr1, uaddr2, nr_wake, nr_requeue, cmpval, flags);
}
#endif /* _FUTEX_H */
