/* SPDX-License-Identifier: GPL-2.0-or-later */
/******************************************************************************
 *
 *   Copyright Collabora Ltd., 2021
 *
 * DESCRIPTION
 *	Futex2 library addons for old futex library
 *
 * AUTHOR
 *	André Almeida <andrealmeid@collabora.com>
 *
 * HISTORY
 *      2021-Feb-5: Initial version by André <andrealmeid@collabora.com>
 *
 *****************************************************************************/
#include "futextest.h"
#include <stdio.h>

#define NSEC_PER_SEC	1000000000L

#ifndef FUTEX_8
# define FUTEX_8	0
#endif
#ifndef FUTEX_16
# define FUTEX_16	1
#endif
#ifndef FUTEX_32
# define FUTEX_32	2
#endif

#ifndef FUTEX_SHARED_FLAG
#define FUTEX_SHARED_FLAG 8
#endif

/*
 * - Y2038 section for 32-bit applications -
 *
 * Remove this when glibc is ready for y2038. Then, always compile with
 * `-DTIME_BITS=64` or `-D__USE_TIME_BITS64`. glibc will provide both
 * timespec64 and clock_gettime64 so we won't need to define here.
 */
#if defined(__i386__) || __TIMESIZE == 32
# define NR_gettime __NR_clock_gettime64
#else
# define NR_gettime __NR_clock_gettime
#endif

struct timespec64 {
	long long tv_sec;	/* seconds */
	long long tv_nsec;	/* nanoseconds */
};

int gettime64(clock_t clockid, struct timespec64 *tv)
{
	return syscall(NR_gettime, clockid, tv);
}
/*
 * - End of Y2038 section -
 */

/**
 * futex2_wait - If (*uaddr == val), wait at uaddr until timo
 * @uaddr: User address to wait on
 * @val:   Expected value at uaddr, return if is not equal
 * @flags: Operation flags
 * @timo:  Optional timeout for operation
 */
static inline int futex2_wait(volatile void *uaddr, unsigned long val,
			      unsigned long flags, struct timespec64 *timo)
{
	return syscall(__NR_futex_wait, uaddr, val, flags, timo);
}

/**
 * futex2_wake - Wake a number of waiters at uaddr
 * @uaddr: Address to wake
 * @nr:    Number of waiters to wake
 * @flags: Operation flags
 */
static inline int futex2_wake(volatile void *uaddr, unsigned int nr, unsigned long flags)
{
	return syscall(__NR_futex_wake, uaddr, nr, flags);
}

/**
 * futex2_waitv - Wait at multiple futexes, wake on any
 * @waiters:    Array of waiters
 * @nr_waiters: Length of waiters array
 * @flags: Operation flags
 * @timo:  Optional timeout for operation
 */
static inline int futex2_waitv(volatile struct futex_waitv *waiters, unsigned long nr_waiters,
			      unsigned long flags, struct timespec64 *timo)
{
	return syscall(__NR_futex_waitv, waiters, nr_waiters, flags, timo);
}

/**
 * futex2_requeue - Wake futexes at uaddr1 and requeue from uaddr1 to uaddr2
 * @uaddr1:     Original address to wake and requeue from
 * @uaddr2:     Address to requeue to
 * @nr_wake:    Number of futexes to wake at uaddr1 before requeuing
 * @nr_requeue: Number of futexes to requeue from uaddr1 to uaddr2
 * @cmpval:     If (uaddr1->uaddr != cmpval), return immediatally
 * @flgas:      Operation flags
 */
static inline int futex2_requeue(struct futex_requeue *uaddr1, struct futex_requeue *uaddr2,
				 unsigned int nr_wake, unsigned int nr_requeue,
				 unsigned int cmpval, unsigned long flags)
{
	return syscall(__NR_futex_requeue, uaddr1, uaddr2, nr_wake, nr_requeue, cmpval, flags);
}
