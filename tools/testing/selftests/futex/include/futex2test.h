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
