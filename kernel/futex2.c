// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * futex2 system call interface by André Almeida <andrealmeid@collabora.com>
 *
 * Copyright 2021 Collabora Ltd.
 */

#include <asm/futex.h>

#include <linux/freezer.h>
#include <linux/syscalls.h>

/* Mask for each futex in futex_waitv list */
#define FUTEXV_WAITER_MASK (FUTEX_SIZE_MASK | FUTEX_SHARED_FLAG)

/* Mask for sys_futex_waitv flag */
#define FUTEXV_MASK (FUTEX_CLOCK_REALTIME)

#ifdef CONFIG_COMPAT
/**
 * compat_futex_parse_waitv - Parse a waitv array from userspace
 * @futexv:	Kernel side list of waiters to be filled
 * @uwaitv:     Userspace list to be parsed
 * @nr_futexes: Length of futexv
 *
 * Return: Error code on failure, pointer to a prepared futexv otherwise
 */
static int compat_futex_parse_waitv(struct futex_vector *futexv,
				    struct compat_futex_waitv __user *uwaitv,
				    unsigned int nr_futexes)
{
	struct compat_futex_waitv aux;
	unsigned int i;

	for (i = 0; i < nr_futexes; i++) {
		if (copy_from_user(&aux, &uwaitv[i], sizeof(aux)))
			return -EFAULT;

		if ((aux.flags & ~FUTEXV_WAITER_MASK) ||
		    (aux.flags & FUTEX_SIZE_MASK) != FUTEX_32)
			return -EINVAL;

		futexv[i].w.flags = aux.flags;
		futexv[i].w.val = aux.val;
		futexv[i].w.uaddr = compat_ptr(aux.uaddr);
		futexv[i].q = futex_q_init;
	}

	return 0;
}

COMPAT_SYSCALL_DEFINE4(futex_waitv, struct compat_futex_waitv __user *, waiters,
		       unsigned int, nr_futexes, unsigned int, flags,
		       struct __kernel_timespec __user *, timo)
{
	struct hrtimer_sleeper to;
	struct futex_vector *futexv;
	struct timespec64 ts;
	ktime_t time;
	int ret;

	if (flags & ~FUTEXV_MASK)
		return -EINVAL;

	if (!nr_futexes || nr_futexes > FUTEX_WAITV_MAX || !waiters)
		return -EINVAL;

	if (timo) {
		int flag_clkid = 0;

		if (get_timespec64(&ts, timo))
			return -EFAULT;

		if (!timespec64_valid(&ts))
			return -EINVAL;

		if (flags & FUTEX_CLOCK_REALTIME)
			flag_clkid = FLAGS_CLOCKRT;

		time = timespec64_to_ktime(ts);
		futex_setup_timer(&time, &to, flag_clkid, 0);
	}

	futexv = kcalloc(nr_futexes, sizeof(*futexv), GFP_KERNEL);
	if (!futexv)
		return -ENOMEM;

	ret = compat_futex_parse_waitv(futexv, waiters, nr_futexes);
	if (!ret)
		ret = futex_wait_multiple(futexv, nr_futexes, timo ? &to : NULL);

	if (timo) {
		hrtimer_cancel(&to.timer);
		destroy_hrtimer_on_stack(&to.timer);
	}

	kfree(futexv);
	return ret;
}
#endif

static int futex_parse_waitv(struct futex_vector *futexv,
			     struct futex_waitv __user *uwaitv,
			     unsigned int nr_futexes)
{
	struct futex_waitv aux;
	unsigned int i;

	for (i = 0; i < nr_futexes; i++) {
		if (copy_from_user(&aux, &uwaitv[i], sizeof(aux)))
			return -EFAULT;

		if ((aux.flags & ~FUTEXV_WAITER_MASK) ||
		    (aux.flags & FUTEX_SIZE_MASK) != FUTEX_32)
			return -EINVAL;

		futexv[i].w.flags = aux.flags;
		futexv[i].w.val = aux.val;
		futexv[i].w.uaddr = aux.uaddr;
		futexv[i].q = futex_q_init;
	}

	return 0;
}

/**
 * sys_futex_waitv - Wait on a list of futexes
 * @waiters:    List of futexes to wait on
 * @nr_futexes: Length of futexv
 * @flags:      Flag for timeout (monotonic/realtime)
 * @timo:	Optional absolute timeout.
 *
 * Given an array of `struct futex_waitv`, wait on each uaddr. The thread wakes
 * if a futex_wake() is performed at any uaddr. The syscall returns immediately
 * if any waiter has *uaddr != val. *timo is an optional timeout value for the
 * operation. Each waiter has individual flags. The `flags` argument for the
 * syscall should be used solely for specifying the timeout as realtime, if
 * needed. Flags for shared futexes, sizes, etc. should be used on the
 * individual flags of each waiter.
 *
 * Returns the array index of one of the awaken futexes. There's no given
 * information of how many were awakened, or any particular attribute of it (if
 * it's the first awakened, if it is of the smaller index...).
 */
SYSCALL_DEFINE4(futex_waitv, struct futex_waitv __user *, waiters,
		unsigned int, nr_futexes, unsigned int, flags,
		struct __kernel_timespec __user *, timo)
{
	struct hrtimer_sleeper to;
	struct futex_vector *futexv;
	struct timespec64 ts;
	ktime_t time;
	int ret;

	if (flags & ~FUTEXV_MASK)
		return -EINVAL;

	if (!nr_futexes || nr_futexes > FUTEX_WAITV_MAX || !waiters)
		return -EINVAL;

	if (timo) {
		int flag_clkid = 0;

		if (get_timespec64(&ts, timo))
			return -EFAULT;

		if (!timespec64_valid(&ts))
			return -EINVAL;

		if (flags & FUTEX_CLOCK_REALTIME)
			flag_clkid = FLAGS_CLOCKRT;

		time = timespec64_to_ktime(ts);
		futex_setup_timer(&time, &to, flag_clkid, 0);
	}

	futexv = kcalloc(nr_futexes, sizeof(*futexv), GFP_KERNEL);
	if (!futexv)
		return -ENOMEM;

	ret = futex_parse_waitv(futexv, waiters, nr_futexes);
	if (!ret)
		ret = futex_wait_multiple(futexv, nr_futexes, timo ? &to : NULL);

	if (timo) {
		hrtimer_cancel(&to.timer);
		destroy_hrtimer_on_stack(&to.timer);
	}

	kfree(futexv);
	return ret;
}
