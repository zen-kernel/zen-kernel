// SPDX-License-Identifier: GPL-2.0
/*
 * Performance events core code:
 *
 *  Copyright (C) 2008 Thomas Gleixner <tglx@linutronix.de>
 *  Copyright (C) 2008-2011 Red Hat, Inc., Ingo Molnar
 *  Copyright (C) 2008-2011 Red Hat, Inc., Peter Zijlstra
 *  Copyright  ©  2009 Paul Mackerras, IBM Corp. <paulus@au1.ibm.com>
 */

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/cpu.h>
#include <linux/smp.h>
#include <linux/idr.h>
#include <linux/file.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/hash.h>
#include <linux/tick.h>
#include <linux/sysfs.h>
#include <linux/dcache.h>
#include <linux/percpu.h>
#include <linux/ptrace.h>
#include <linux/reboot.h>
#include <linux/vmstat.h>
#include <linux/device.h>
#include <linux/export.h>
#include <linux/vmalloc.h>
#include <linux/hardirq.h>
#include <linux/hugetlb.h>
#include <linux/rculist.h>
#include <linux/uaccess.h>
#include <linux/syscalls.h>
#include <linux/anon_inodes.h>
#include <linux/kernel_stat.h>
#include <linux/cgroup.h>
#include <linux/perf_event.h>
#include <linux/trace_events.h>
#include <linux/hw_breakpoint.h>
#include <linux/mm_types.h>
#include <linux/module.h>
#include <linux/mman.h>
#include <linux/compat.h>
#include <linux/bpf.h>
#include <linux/filter.h>
#include <linux/namei.h>
#include <linux/parser.h>
#include <linux/sched/clock.h>
#include <linux/sched/mm.h>
#include <linux/proc_ns.h>
#include <linux/mount.h>
#include <linux/min_heap.h>
#include <linux/highmem.h>
#include <linux/pgtable.h>
#include <linux/buildid.h>
#include <linux/task_work.h>
#include <linux/percpu-rwsem.h>

#include "internal.h"

#include <asm/irq_regs.h>

typedef int (*remote_function_f)(void *);

struct remote_function_call {
	struct task_struct	*p;
	remote_function_f	func;
	void			*info;
	int			ret;
};

static void remote_function(void *data)
{
	struct remote_function_call *tfc = data;
	struct task_struct *p = tfc->p;

	if (p) {
		/* -EAGAIN */
		if (task_cpu(p) != smp_processor_id())
			return;

		/*
		 * Now that we're on right CPU with IRQs disabled, we can test
		 * if we hit the right task without races.
		 */

		tfc->ret = -ESRCH; /* No such (running) process */
		if (p != current)
			return;
	}

	tfc->ret = tfc->func(tfc->info);
}

/**
 * task_function_call - call a function on the cpu on which a task runs
 * @p:		the task to evaluate
 * @func:	the function to be called
 * @info:	the function call argument
 *
 * Calls the function @func when the task is currently running. This might
 * be on the current CPU, which just calls the function directly.  This will
 * retry due to any failures in smp_call_function_single(), such as if the
 * task_cpu() goes offline concurrently.
 *
 * returns @func return value or -ESRCH or -ENXIO when the process isn't running
 */
static int
task_function_call(struct task_struct *p, remote_function_f func, void *info)
{
	struct remote_function_call data = {
		.p	= p,
		.func	= func,
		.info	= info,
		.ret	= -EAGAIN,
	};
	int ret;

	for (;;) {
		ret = smp_call_function_single(task_cpu(p), remote_function,
					       &data, 1);
		if (!ret)
			ret = data.ret;

		if (ret != -EAGAIN)
			break;

		cond_resched();
	}

	return ret;
}

/**
 * cpu_function_call - call a function on the cpu
 * @cpu:	target cpu to queue this function
 * @func:	the function to be called
 * @info:	the function call argument
 *
 * Calls the function @func on the remote cpu.
 *
 * returns: @func return value or -ENXIO when the cpu is offline
 */
static int cpu_function_call(int cpu, remote_function_f func, void *info)
{
	struct remote_function_call data = {
		.p	= NULL,
		.func	= func,
		.info	= info,
		.ret	= -ENXIO, /* No such CPU */
	};

	smp_call_function_single(cpu, remote_function, &data, 1);

	return data.ret;
}

enum event_type_t {
	EVENT_FLEXIBLE	= 0x01,
	EVENT_PINNED	= 0x02,
	EVENT_TIME	= 0x04,
	EVENT_FROZEN	= 0x08,
	/* see ctx_resched() for details */
	EVENT_CPU	= 0x10,
	EVENT_CGROUP	= 0x20,

	/* compound helpers */
	EVENT_ALL         = EVENT_FLEXIBLE | EVENT_PINNED,
	EVENT_TIME_FROZEN = EVENT_TIME | EVENT_FROZEN,
};

static inline void __perf_ctx_lock(struct perf_event_context *ctx)
{
	raw_spin_lock(&ctx->lock);
	WARN_ON_ONCE(ctx->is_active & EVENT_FROZEN);
}

static void perf_ctx_lock(struct perf_cpu_context *cpuctx,
			  struct perf_event_context *ctx)
{
	__perf_ctx_lock(&cpuctx->ctx);
	if (ctx)
		__perf_ctx_lock(ctx);
}

static inline void __perf_ctx_unlock(struct perf_event_context *ctx)
{
	/*
	 * If ctx_sched_in() didn't again set any ALL flags, clean up
	 * after ctx_sched_out() by clearing is_active.
	 */
	if (ctx->is_active & EVENT_FROZEN) {
		if (!(ctx->is_active & EVENT_ALL))
			ctx->is_active = 0;
		else
			ctx->is_active &= ~EVENT_FROZEN;
	}
	raw_spin_unlock(&ctx->lock);
}

static void perf_ctx_unlock(struct perf_cpu_context *cpuctx,
			    struct perf_event_context *ctx)
{
	if (ctx)
		__perf_ctx_unlock(ctx);
	__perf_ctx_unlock(&cpuctx->ctx);
}

typedef struct {
	struct perf_cpu_context *cpuctx;
	struct perf_event_context *ctx;
} class_perf_ctx_lock_t;

static inline void class_perf_ctx_lock_destructor(class_perf_ctx_lock_t *_T)
{ perf_ctx_unlock(_T->cpuctx, _T->ctx); }

static inline class_perf_ctx_lock_t
class_perf_ctx_lock_constructor(struct perf_cpu_context *cpuctx,
				struct perf_event_context *ctx)
{ perf_ctx_lock(cpuctx, ctx); return (class_perf_ctx_lock_t){ cpuctx, ctx }; }

#define TASK_TOMBSTONE ((void *)-1L)

static bool is_kernel_event(struct perf_event *event)
{
	return READ_ONCE(event->owner) == TASK_TOMBSTONE;
}

static DEFINE_PER_CPU(struct perf_cpu_context, perf_cpu_context);

struct perf_event_context *perf_cpu_task_ctx(void)
{
	lockdep_assert_irqs_disabled();
	return this_cpu_ptr(&perf_cpu_context)->task_ctx;
}

/*
 * On task ctx scheduling...
 *
 * When !ctx->nr_events a task context will not be scheduled. This means
 * we can disable the scheduler hooks (for performance) without leaving
 * pending task ctx state.
 *
 * This however results in two special cases:
 *
 *  - removing the last event from a task ctx; this is relatively straight
 *    forward and is done in __perf_remove_from_context.
 *
 *  - adding the first event to a task ctx; this is tricky because we cannot
 *    rely on ctx->is_active and therefore cannot use event_function_call().
 *    See perf_install_in_context().
 *
 * If ctx->nr_events, then ctx->is_active and cpuctx->task_ctx are set.
 */

typedef void (*event_f)(struct perf_event *, struct perf_cpu_context *,
			struct perf_event_context *, void *);

struct event_function_struct {
	struct perf_event *event;
	event_f func;
	void *data;
};

static int event_function(void *info)
{
	struct event_function_struct *efs = info;
	struct perf_event *event = efs->event;
	struct perf_event_context *ctx = event->ctx;
	struct perf_cpu_context *cpuctx = this_cpu_ptr(&perf_cpu_context);
	struct perf_event_context *task_ctx = cpuctx->task_ctx;
	int ret = 0;

	lockdep_assert_irqs_disabled();

	perf_ctx_lock(cpuctx, task_ctx);
	/*
	 * Since we do the IPI call without holding ctx->lock things can have
	 * changed, double check we hit the task we set out to hit.
	 */
	if (ctx->task) {
		if (ctx->task != current) {
			ret = -ESRCH;
			goto unlock;
		}

		/*
		 * We only use event_function_call() on established contexts,
		 * and event_function() is only ever called when active (or
		 * rather, we'll have bailed in task_function_call() or the
		 * above ctx->task != current test), therefore we must have
		 * ctx->is_active here.
		 */
		WARN_ON_ONCE(!ctx->is_active);
		/*
		 * And since we have ctx->is_active, cpuctx->task_ctx must
		 * match.
		 */
		WARN_ON_ONCE(task_ctx != ctx);
	} else {
		WARN_ON_ONCE(&cpuctx->ctx != ctx);
	}

	efs->func(event, cpuctx, ctx, efs->data);
unlock:
	perf_ctx_unlock(cpuctx, task_ctx);

	return ret;
}

static void event_function_call(struct perf_event *event, event_f func, void *data)
{
	struct perf_event_context *ctx = event->ctx;
	struct task_struct *task = READ_ONCE(ctx->task); /* verified in event_function */
	struct perf_cpu_context *cpuctx;
	struct event_function_struct efs = {
		.event = event,
		.func = func,
		.data = data,
	};

	if (!event->parent) {
		/*
		 * If this is a !child event, we must hold ctx::mutex to
		 * stabilize the event->ctx relation. See
		 * perf_event_ctx_lock().
		 */
		lockdep_assert_held(&ctx->mutex);
	}

	if (!task) {
		cpu_function_call(event->cpu, event_function, &efs);
		return;
	}

	if (task == TASK_TOMBSTONE)
		return;

again:
	if (!task_function_call(task, event_function, &efs))
		return;

	local_irq_disable();
	cpuctx = this_cpu_ptr(&perf_cpu_context);
	perf_ctx_lock(cpuctx, ctx);
	/*
	 * Reload the task pointer, it might have been changed by
	 * a concurrent perf_event_context_sched_out().
	 */
	task = ctx->task;
	if (task == TASK_TOMBSTONE)
		goto unlock;
	if (ctx->is_active) {
		perf_ctx_unlock(cpuctx, ctx);
		local_irq_enable();
		goto again;
	}
	func(event, NULL, ctx, data);
unlock:
	perf_ctx_unlock(cpuctx, ctx);
	local_irq_enable();
}

/*
 * Similar to event_function_call() + event_function(), but hard assumes IRQs
 * are already disabled and we're on the right CPU.
 */
static void event_function_local(struct perf_event *event, event_f func, void *data)
{
	struct perf_event_context *ctx = event->ctx;
	struct perf_cpu_context *cpuctx = this_cpu_ptr(&perf_cpu_context);
	struct task_struct *task = READ_ONCE(ctx->task);
	struct perf_event_context *task_ctx = NULL;

	lockdep_assert_irqs_disabled();

	if (task) {
		if (task == TASK_TOMBSTONE)
			return;

		task_ctx = ctx;
	}

	perf_ctx_lock(cpuctx, task_ctx);

	task = ctx->task;
	if (task == TASK_TOMBSTONE)
		goto unlock;

	if (task) {
		/*
		 * We must be either inactive or active and the right task,
		 * otherwise we're screwed, since we cannot IPI to somewhere
		 * else.
		 */
		if (ctx->is_active) {
			if (WARN_ON_ONCE(task != current))
				goto unlock;

			if (WARN_ON_ONCE(cpuctx->task_ctx != ctx))
				goto unlock;
		}
	} else {
		WARN_ON_ONCE(&cpuctx->ctx != ctx);
	}

	func(event, cpuctx, ctx, data);
unlock:
	perf_ctx_unlock(cpuctx, task_ctx);
}

#define PERF_FLAG_ALL (PERF_FLAG_FD_NO_GROUP |\
		       PERF_FLAG_FD_OUTPUT  |\
		       PERF_FLAG_PID_CGROUP |\
		       PERF_FLAG_FD_CLOEXEC)

/*
 * branch priv levels that need permission checks
 */
#define PERF_SAMPLE_BRANCH_PERM_PLM \
	(PERF_SAMPLE_BRANCH_KERNEL |\
	 PERF_SAMPLE_BRANCH_HV)

/*
 * perf_sched_events : >0 events exist
 */

static void perf_sched_delayed(struct work_struct *work);
DEFINE_STATIC_KEY_FALSE(perf_sched_events);
static DECLARE_DELAYED_WORK(perf_sched_work, perf_sched_delayed);
static DEFINE_MUTEX(perf_sched_mutex);
static atomic_t perf_sched_count;

static DEFINE_PER_CPU(struct pmu_event_list, pmu_sb_events);

static atomic_t nr_mmap_events __read_mostly;
static atomic_t nr_comm_events __read_mostly;
static atomic_t nr_namespaces_events __read_mostly;
static atomic_t nr_task_events __read_mostly;
static atomic_t nr_freq_events __read_mostly;
static atomic_t nr_switch_events __read_mostly;
static atomic_t nr_ksymbol_events __read_mostly;
static atomic_t nr_bpf_events __read_mostly;
static atomic_t nr_cgroup_events __read_mostly;
static atomic_t nr_text_poke_events __read_mostly;
static atomic_t nr_build_id_events __read_mostly;

static LIST_HEAD(pmus);
static DEFINE_MUTEX(pmus_lock);
static struct srcu_struct pmus_srcu;
static cpumask_var_t perf_online_mask;
static cpumask_var_t perf_online_core_mask;
static cpumask_var_t perf_online_die_mask;
static cpumask_var_t perf_online_cluster_mask;
static cpumask_var_t perf_online_pkg_mask;
static cpumask_var_t perf_online_sys_mask;
static struct kmem_cache *perf_event_cache;

/*
 * perf event paranoia level:
 *  -1 - not paranoid at all
 *   0 - disallow raw tracepoint access for unpriv
 *   1 - disallow cpu events for unpriv
 *   2 - disallow kernel profiling for unpriv
 */
int sysctl_perf_event_paranoid __read_mostly = 2;

/* Minimum for 512 kiB + 1 user control page. 'free' kiB per user. */
static int sysctl_perf_event_mlock __read_mostly = 512 + (PAGE_SIZE / 1024);

/*
 * max perf event sample rate
 */
#define DEFAULT_MAX_SAMPLE_RATE		100000
#define DEFAULT_SAMPLE_PERIOD_NS	(NSEC_PER_SEC / DEFAULT_MAX_SAMPLE_RATE)
#define DEFAULT_CPU_TIME_MAX_PERCENT	25

int sysctl_perf_event_sample_rate __read_mostly	= DEFAULT_MAX_SAMPLE_RATE;
static int sysctl_perf_cpu_time_max_percent __read_mostly = DEFAULT_CPU_TIME_MAX_PERCENT;

static int max_samples_per_tick __read_mostly	= DIV_ROUND_UP(DEFAULT_MAX_SAMPLE_RATE, HZ);
static int perf_sample_period_ns __read_mostly	= DEFAULT_SAMPLE_PERIOD_NS;

static int perf_sample_allowed_ns __read_mostly =
	DEFAULT_SAMPLE_PERIOD_NS * DEFAULT_CPU_TIME_MAX_PERCENT / 100;

static void update_perf_cpu_limits(void)
{
	u64 tmp = perf_sample_period_ns;

	tmp *= sysctl_perf_cpu_time_max_percent;
	tmp = div_u64(tmp, 100);
	if (!tmp)
		tmp = 1;

	WRITE_ONCE(perf_sample_allowed_ns, tmp);
}

static bool perf_rotate_context(struct perf_cpu_pmu_context *cpc);

static int perf_event_max_sample_rate_handler(const struct ctl_table *table, int write,
				       void *buffer, size_t *lenp, loff_t *ppos)
{
	int ret;
	int perf_cpu = sysctl_perf_cpu_time_max_percent;
	/*
	 * If throttling is disabled don't allow the write:
	 */
	if (write && (perf_cpu == 100 || perf_cpu == 0))
		return -EINVAL;

	ret = proc_dointvec_minmax(table, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	max_samples_per_tick = DIV_ROUND_UP(sysctl_perf_event_sample_rate, HZ);
	perf_sample_period_ns = NSEC_PER_SEC / sysctl_perf_event_sample_rate;
	update_perf_cpu_limits();

	return 0;
}

static int perf_cpu_time_max_percent_handler(const struct ctl_table *table, int write,
		void *buffer, size_t *lenp, loff_t *ppos)
{
	int ret = proc_dointvec_minmax(table, write, buffer, lenp, ppos);

	if (ret || !write)
		return ret;

	if (sysctl_perf_cpu_time_max_percent == 100 ||
	    sysctl_perf_cpu_time_max_percent == 0) {
		printk(KERN_WARNING
		       "perf: Dynamic interrupt throttling disabled, can hang your system!\n");
		WRITE_ONCE(perf_sample_allowed_ns, 0);
	} else {
		update_perf_cpu_limits();
	}

	return 0;
}

static const struct ctl_table events_core_sysctl_table[] = {
	/*
	 * User-space relies on this file as a feature check for
	 * perf_events being enabled. It's an ABI, do not remove!
	 */
	{
		.procname	= "perf_event_paranoid",
		.data		= &sysctl_perf_event_paranoid,
		.maxlen		= sizeof(sysctl_perf_event_paranoid),
		.mode		= 0644,
		.proc_handler	= proc_dointvec,
	},
	{
		.procname	= "perf_event_mlock_kb",
		.data		= &sysctl_perf_event_mlock,
		.maxlen		= sizeof(sysctl_perf_event_mlock),
		.mode		= 0644,
		.proc_handler	= proc_dointvec,
	},
	{
		.procname	= "perf_event_max_sample_rate",
		.data		= &sysctl_perf_event_sample_rate,
		.maxlen		= sizeof(sysctl_perf_event_sample_rate),
		.mode		= 0644,
		.proc_handler	= perf_event_max_sample_rate_handler,
		.extra1		= SYSCTL_ONE,
	},
	{
		.procname	= "perf_cpu_time_max_percent",
		.data		= &sysctl_perf_cpu_time_max_percent,
		.maxlen		= sizeof(sysctl_perf_cpu_time_max_percent),
		.mode		= 0644,
		.proc_handler	= perf_cpu_time_max_percent_handler,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE_HUNDRED,
	},
};

static int __init init_events_core_sysctls(void)
{
	register_sysctl_init("kernel", events_core_sysctl_table);
	return 0;
}
core_initcall(init_events_core_sysctls);


/*
 * perf samples are done in some very critical code paths (NMIs).
 * If they take too much CPU time, the system can lock up and not
 * get any real work done.  This will drop the sample rate when
 * we detect that events are taking too long.
 */
#define NR_ACCUMULATED_SAMPLES 128
static DEFINE_PER_CPU(u64, running_sample_length);

static u64 __report_avg;
static u64 __report_allowed;

static void perf_duration_warn(struct irq_work *w)
{
	printk_ratelimited(KERN_INFO
		"perf: interrupt took too long (%lld > %lld), lowering "
		"kernel.perf_event_max_sample_rate to %d\n",
		__report_avg, __report_allowed,
		sysctl_perf_event_sample_rate);
}

static DEFINE_IRQ_WORK(perf_duration_work, perf_duration_warn);

void perf_sample_event_took(u64 sample_len_ns)
{
	u64 max_len = READ_ONCE(perf_sample_allowed_ns);
	u64 running_len;
	u64 avg_len;
	u32 max;

	if (max_len == 0)
		return;

	/* Decay the counter by 1 average sample. */
	running_len = __this_cpu_read(running_sample_length);
	running_len -= running_len/NR_ACCUMULATED_SAMPLES;
	running_len += sample_len_ns;
	__this_cpu_write(running_sample_length, running_len);

	/*
	 * Note: this will be biased artificially low until we have
	 * seen NR_ACCUMULATED_SAMPLES. Doing it this way keeps us
	 * from having to maintain a count.
	 */
	avg_len = running_len/NR_ACCUMULATED_SAMPLES;
	if (avg_len <= max_len)
		return;

	__report_avg = avg_len;
	__report_allowed = max_len;

	/*
	 * Compute a throttle threshold 25% below the current duration.
	 */
	avg_len += avg_len / 4;
	max = (TICK_NSEC / 100) * sysctl_perf_cpu_time_max_percent;
	if (avg_len < max)
		max /= (u32)avg_len;
	else
		max = 1;

	WRITE_ONCE(perf_sample_allowed_ns, avg_len);
	WRITE_ONCE(max_samples_per_tick, max);

	sysctl_perf_event_sample_rate = max * HZ;
	perf_sample_period_ns = NSEC_PER_SEC / sysctl_perf_event_sample_rate;

	if (!irq_work_queue(&perf_duration_work)) {
		early_printk("perf: interrupt took too long (%lld > %lld), lowering "
			     "kernel.perf_event_max_sample_rate to %d\n",
			     __report_avg, __report_allowed,
			     sysctl_perf_event_sample_rate);
	}
}

static atomic64_t perf_event_id;

static void update_context_time(struct perf_event_context *ctx);
static u64 perf_event_time(struct perf_event *event);

void __weak perf_event_print_debug(void)	{ }

static inline u64 perf_clock(void)
{
	return local_clock();
}

static inline u64 perf_event_clock(struct perf_event *event)
{
	return event->clock();
}

/*
 * State based event timekeeping...
 *
 * The basic idea is to use event->state to determine which (if any) time
 * fields to increment with the current delta. This means we only need to
 * update timestamps when we change state or when they are explicitly requested
 * (read).
 *
 * Event groups make things a little more complicated, but not terribly so. The
 * rules for a group are that if the group leader is OFF the entire group is
 * OFF, irrespective of what the group member states are. This results in
 * __perf_effective_state().
 *
 * A further ramification is that when a group leader flips between OFF and
 * !OFF, we need to update all group member times.
 *
 *
 * NOTE: perf_event_time() is based on the (cgroup) context time, and thus we
 * need to make sure the relevant context time is updated before we try and
 * update our timestamps.
 */

static __always_inline enum perf_event_state
__perf_effective_state(struct perf_event *event)
{
	struct perf_event *leader = event->group_leader;

	if (leader->state <= PERF_EVENT_STATE_OFF)
		return leader->state;

	return event->state;
}

static __always_inline void
__perf_update_times(struct perf_event *event, u64 now, u64 *enabled, u64 *running)
{
	enum perf_event_state state = __perf_effective_state(event);
	u64 delta = now - event->tstamp;

	*enabled = event->total_time_enabled;
	if (state >= PERF_EVENT_STATE_INACTIVE)
		*enabled += delta;

	*running = event->total_time_running;
	if (state >= PERF_EVENT_STATE_ACTIVE)
		*running += delta;
}

static void perf_event_update_time(struct perf_event *event)
{
	u64 now = perf_event_time(event);

	__perf_update_times(event, now, &event->total_time_enabled,
					&event->total_time_running);
	event->tstamp = now;
}

static void perf_event_update_sibling_time(struct perf_event *leader)
{
	struct perf_event *sibling;

	for_each_sibling_event(sibling, leader)
		perf_event_update_time(sibling);
}

static void
perf_event_set_state(struct perf_event *event, enum perf_event_state state)
{
	if (event->state == state)
		return;

	perf_event_update_time(event);
	/*
	 * If a group leader gets enabled/disabled all its siblings
	 * are affected too.
	 */
	if ((event->state < 0) ^ (state < 0))
		perf_event_update_sibling_time(event);

	WRITE_ONCE(event->state, state);
}

/*
 * UP store-release, load-acquire
 */

#define __store_release(ptr, val)					\
do {									\
	barrier();							\
	WRITE_ONCE(*(ptr), (val));					\
} while (0)

#define __load_acquire(ptr)						\
({									\
	__unqual_scalar_typeof(*(ptr)) ___p = READ_ONCE(*(ptr));	\
	barrier();							\
	___p;								\
})

#define for_each_epc(_epc, _ctx, _pmu, _cgroup)				\
	list_for_each_entry(_epc, &((_ctx)->pmu_ctx_list), pmu_ctx_entry) \
		if (_cgroup && !_epc->nr_cgroups)			\
			continue;					\
		else if (_pmu && _epc->pmu != _pmu)			\
			continue;					\
		else

static void perf_ctx_disable(struct perf_event_context *ctx, bool cgroup)
{
	struct perf_event_pmu_context *pmu_ctx;

	for_each_epc(pmu_ctx, ctx, NULL, cgroup)
		perf_pmu_disable(pmu_ctx->pmu);
}

static void perf_ctx_enable(struct perf_event_context *ctx, bool cgroup)
{
	struct perf_event_pmu_context *pmu_ctx;

	for_each_epc(pmu_ctx, ctx, NULL, cgroup)
		perf_pmu_enable(pmu_ctx->pmu);
}

static void ctx_sched_out(struct perf_event_context *ctx, struct pmu *pmu, enum event_type_t event_type);
static void ctx_sched_in(struct perf_event_context *ctx, struct pmu *pmu, enum event_type_t event_type);

#ifdef CONFIG_CGROUP_PERF

static inline bool
perf_cgroup_match(struct perf_event *event)
{
	struct perf_cpu_context *cpuctx = this_cpu_ptr(&perf_cpu_context);

	/* @event doesn't care about cgroup */
	if (!event->cgrp)
		return true;

	/* wants specific cgroup scope but @cpuctx isn't associated with any */
	if (!cpuctx->cgrp)
		return false;

	/*
	 * Cgroup scoping is recursive.  An event enabled for a cgroup is
	 * also enabled for all its descendant cgroups.  If @cpuctx's
	 * cgroup is a descendant of @event's (the test covers identity
	 * case), it's a match.
	 */
	return cgroup_is_descendant(cpuctx->cgrp->css.cgroup,
				    event->cgrp->css.cgroup);
}

static inline void perf_detach_cgroup(struct perf_event *event)
{
	css_put(&event->cgrp->css);
	event->cgrp = NULL;
}

static inline int is_cgroup_event(struct perf_event *event)
{
	return event->cgrp != NULL;
}

static inline u64 perf_cgroup_event_time(struct perf_event *event)
{
	struct perf_cgroup_info *t;

	t = per_cpu_ptr(event->cgrp->info, event->cpu);
	return t->time;
}

static inline u64 perf_cgroup_event_time_now(struct perf_event *event, u64 now)
{
	struct perf_cgroup_info *t;

	t = per_cpu_ptr(event->cgrp->info, event->cpu);
	if (!__load_acquire(&t->active))
		return t->time;
	now += READ_ONCE(t->timeoffset);
	return now;
}

static inline void __update_cgrp_time(struct perf_cgroup_info *info, u64 now, bool adv)
{
	if (adv)
		info->time += now - info->timestamp;
	info->timestamp = now;
	/*
	 * see update_context_time()
	 */
	WRITE_ONCE(info->timeoffset, info->time - info->timestamp);
}

static inline void update_cgrp_time_from_cpuctx(struct perf_cpu_context *cpuctx, bool final)
{
	struct perf_cgroup *cgrp = cpuctx->cgrp;
	struct cgroup_subsys_state *css;
	struct perf_cgroup_info *info;

	if (cgrp) {
		u64 now = perf_clock();

		for (css = &cgrp->css; css; css = css->parent) {
			cgrp = container_of(css, struct perf_cgroup, css);
			info = this_cpu_ptr(cgrp->info);

			__update_cgrp_time(info, now, true);
			if (final)
				__store_release(&info->active, 0);
		}
	}
}

static inline void update_cgrp_time_from_event(struct perf_event *event)
{
	struct perf_cgroup_info *info;

	/*
	 * ensure we access cgroup data only when needed and
	 * when we know the cgroup is pinned (css_get)
	 */
	if (!is_cgroup_event(event))
		return;

	info = this_cpu_ptr(event->cgrp->info);
	/*
	 * Do not update time when cgroup is not active
	 */
	if (info->active)
		__update_cgrp_time(info, perf_clock(), true);
}

static inline void
perf_cgroup_set_timestamp(struct perf_cpu_context *cpuctx)
{
	struct perf_event_context *ctx = &cpuctx->ctx;
	struct perf_cgroup *cgrp = cpuctx->cgrp;
	struct perf_cgroup_info *info;
	struct cgroup_subsys_state *css;

	/*
	 * ctx->lock held by caller
	 * ensure we do not access cgroup data
	 * unless we have the cgroup pinned (css_get)
	 */
	if (!cgrp)
		return;

	WARN_ON_ONCE(!ctx->nr_cgroups);

	for (css = &cgrp->css; css; css = css->parent) {
		cgrp = container_of(css, struct perf_cgroup, css);
		info = this_cpu_ptr(cgrp->info);
		__update_cgrp_time(info, ctx->timestamp, false);
		__store_release(&info->active, 1);
	}
}

/*
 * reschedule events based on the cgroup constraint of task.
 */
static void perf_cgroup_switch(struct task_struct *task)
{
	struct perf_cpu_context *cpuctx = this_cpu_ptr(&perf_cpu_context);
	struct perf_cgroup *cgrp;

	/*
	 * cpuctx->cgrp is set when the first cgroup event enabled,
	 * and is cleared when the last cgroup event disabled.
	 */
	if (READ_ONCE(cpuctx->cgrp) == NULL)
		return;

	cgrp = perf_cgroup_from_task(task, NULL);
	if (READ_ONCE(cpuctx->cgrp) == cgrp)
		return;

	guard(perf_ctx_lock)(cpuctx, cpuctx->task_ctx);
	/*
	 * Re-check, could've raced vs perf_remove_from_context().
	 */
	if (READ_ONCE(cpuctx->cgrp) == NULL)
		return;

	WARN_ON_ONCE(cpuctx->ctx.nr_cgroups == 0);

	perf_ctx_disable(&cpuctx->ctx, true);

	ctx_sched_out(&cpuctx->ctx, NULL, EVENT_ALL|EVENT_CGROUP);
	/*
	 * must not be done before ctxswout due
	 * to update_cgrp_time_from_cpuctx() in
	 * ctx_sched_out()
	 */
	cpuctx->cgrp = cgrp;
	/*
	 * set cgrp before ctxsw in to allow
	 * perf_cgroup_set_timestamp() in ctx_sched_in()
	 * to not have to pass task around
	 */
	ctx_sched_in(&cpuctx->ctx, NULL, EVENT_ALL|EVENT_CGROUP);

	perf_ctx_enable(&cpuctx->ctx, true);
}

static int perf_cgroup_ensure_storage(struct perf_event *event,
				struct cgroup_subsys_state *css)
{
	struct perf_cpu_context *cpuctx;
	struct perf_event **storage;
	int cpu, heap_size, ret = 0;

	/*
	 * Allow storage to have sufficient space for an iterator for each
	 * possibly nested cgroup plus an iterator for events with no cgroup.
	 */
	for (heap_size = 1; css; css = css->parent)
		heap_size++;

	for_each_possible_cpu(cpu) {
		cpuctx = per_cpu_ptr(&perf_cpu_context, cpu);
		if (heap_size <= cpuctx->heap_size)
			continue;

		storage = kmalloc_node(heap_size * sizeof(struct perf_event *),
				       GFP_KERNEL, cpu_to_node(cpu));
		if (!storage) {
			ret = -ENOMEM;
			break;
		}

		raw_spin_lock_irq(&cpuctx->ctx.lock);
		if (cpuctx->heap_size < heap_size) {
			swap(cpuctx->heap, storage);
			if (storage == cpuctx->heap_default)
				storage = NULL;
			cpuctx->heap_size = heap_size;
		}
		raw_spin_unlock_irq(&cpuctx->ctx.lock);

		kfree(storage);
	}

	return ret;
}

static inline int perf_cgroup_connect(int fd, struct perf_event *event,
				      struct perf_event_attr *attr,
				      struct perf_event *group_leader)
{
	struct perf_cgroup *cgrp;
	struct cgroup_subsys_state *css;
	CLASS(fd, f)(fd);
	int ret = 0;

	if (fd_empty(f))
		return -EBADF;

	css = css_tryget_online_from_dir(fd_file(f)->f_path.dentry,
					 &perf_event_cgrp_subsys);
	if (IS_ERR(css))
		return PTR_ERR(css);

	ret = perf_cgroup_ensure_storage(event, css);
	if (ret)
		return ret;

	cgrp = container_of(css, struct perf_cgroup, css);
	event->cgrp = cgrp;

	/*
	 * all events in a group must monitor
	 * the same cgroup because a task belongs
	 * to only one perf cgroup at a time
	 */
	if (group_leader && group_leader->cgrp != cgrp) {
		perf_detach_cgroup(event);
		ret = -EINVAL;
	}
	return ret;
}

static inline void
perf_cgroup_event_enable(struct perf_event *event, struct perf_event_context *ctx)
{
	struct perf_cpu_context *cpuctx;

	if (!is_cgroup_event(event))
		return;

	event->pmu_ctx->nr_cgroups++;

	/*
	 * Because cgroup events are always per-cpu events,
	 * @ctx == &cpuctx->ctx.
	 */
	cpuctx = container_of(ctx, struct perf_cpu_context, ctx);

	if (ctx->nr_cgroups++)
		return;

	cpuctx->cgrp = perf_cgroup_from_task(current, ctx);
}

static inline void
perf_cgroup_event_disable(struct perf_event *event, struct perf_event_context *ctx)
{
	struct perf_cpu_context *cpuctx;

	if (!is_cgroup_event(event))
		return;

	event->pmu_ctx->nr_cgroups--;

	/*
	 * Because cgroup events are always per-cpu events,
	 * @ctx == &cpuctx->ctx.
	 */
	cpuctx = container_of(ctx, struct perf_cpu_context, ctx);

	if (--ctx->nr_cgroups)
		return;

	cpuctx->cgrp = NULL;
}

#else /* !CONFIG_CGROUP_PERF */

static inline bool
perf_cgroup_match(struct perf_event *event)
{
	return true;
}

static inline void perf_detach_cgroup(struct perf_event *event)
{}

static inline int is_cgroup_event(struct perf_event *event)
{
	return 0;
}

static inline void update_cgrp_time_from_event(struct perf_event *event)
{
}

static inline void update_cgrp_time_from_cpuctx(struct perf_cpu_context *cpuctx,
						bool final)
{
}

static inline int perf_cgroup_connect(pid_t pid, struct perf_event *event,
				      struct perf_event_attr *attr,
				      struct perf_event *group_leader)
{
	return -EINVAL;
}

static inline void
perf_cgroup_set_timestamp(struct perf_cpu_context *cpuctx)
{
}

static inline u64 perf_cgroup_event_time(struct perf_event *event)
{
	return 0;
}

static inline u64 perf_cgroup_event_time_now(struct perf_event *event, u64 now)
{
	return 0;
}

static inline void
perf_cgroup_event_enable(struct perf_event *event, struct perf_event_context *ctx)
{
}

static inline void
perf_cgroup_event_disable(struct perf_event *event, struct perf_event_context *ctx)
{
}

static void perf_cgroup_switch(struct task_struct *task)
{
}
#endif

/*
 * set default to be dependent on timer tick just
 * like original code
 */
#define PERF_CPU_HRTIMER (1000 / HZ)
/*
 * function must be called with interrupts disabled
 */
static enum hrtimer_restart perf_mux_hrtimer_handler(struct hrtimer *hr)
{
	struct perf_cpu_pmu_context *cpc;
	bool rotations;

	lockdep_assert_irqs_disabled();

	cpc = container_of(hr, struct perf_cpu_pmu_context, hrtimer);
	rotations = perf_rotate_context(cpc);

	raw_spin_lock(&cpc->hrtimer_lock);
	if (rotations)
		hrtimer_forward_now(hr, cpc->hrtimer_interval);
	else
		cpc->hrtimer_active = 0;
	raw_spin_unlock(&cpc->hrtimer_lock);

	return rotations ? HRTIMER_RESTART : HRTIMER_NORESTART;
}

static void __perf_mux_hrtimer_init(struct perf_cpu_pmu_context *cpc, int cpu)
{
	struct hrtimer *timer = &cpc->hrtimer;
	struct pmu *pmu = cpc->epc.pmu;
	u64 interval;

	/*
	 * check default is sane, if not set then force to
	 * default interval (1/tick)
	 */
	interval = pmu->hrtimer_interval_ms;
	if (interval < 1)
		interval = pmu->hrtimer_interval_ms = PERF_CPU_HRTIMER;

	cpc->hrtimer_interval = ns_to_ktime(NSEC_PER_MSEC * interval);

	raw_spin_lock_init(&cpc->hrtimer_lock);
	hrtimer_setup(timer, perf_mux_hrtimer_handler, CLOCK_MONOTONIC,
		      HRTIMER_MODE_ABS_PINNED_HARD);
}

static int perf_mux_hrtimer_restart(struct perf_cpu_pmu_context *cpc)
{
	struct hrtimer *timer = &cpc->hrtimer;
	unsigned long flags;

	raw_spin_lock_irqsave(&cpc->hrtimer_lock, flags);
	if (!cpc->hrtimer_active) {
		cpc->hrtimer_active = 1;
		hrtimer_forward_now(timer, cpc->hrtimer_interval);
		hrtimer_start_expires(timer, HRTIMER_MODE_ABS_PINNED_HARD);
	}
	raw_spin_unlock_irqrestore(&cpc->hrtimer_lock, flags);

	return 0;
}

static int perf_mux_hrtimer_restart_ipi(void *arg)
{
	return perf_mux_hrtimer_restart(arg);
}

static __always_inline struct perf_cpu_pmu_context *this_cpc(struct pmu *pmu)
{
	return *this_cpu_ptr(pmu->cpu_pmu_context);
}

void perf_pmu_disable(struct pmu *pmu)
{
	int *count = &this_cpc(pmu)->pmu_disable_count;
	if (!(*count)++)
		pmu->pmu_disable(pmu);
}

void perf_pmu_enable(struct pmu *pmu)
{
	int *count = &this_cpc(pmu)->pmu_disable_count;
	if (!--(*count))
		pmu->pmu_enable(pmu);
}

static void perf_assert_pmu_disabled(struct pmu *pmu)
{
	int *count = &this_cpc(pmu)->pmu_disable_count;
	WARN_ON_ONCE(*count == 0);
}

static inline void perf_pmu_read(struct perf_event *event)
{
	if (event->state == PERF_EVENT_STATE_ACTIVE)
		event->pmu->read(event);
}

static void get_ctx(struct perf_event_context *ctx)
{
	refcount_inc(&ctx->refcount);
}

static void free_ctx(struct rcu_head *head)
{
	struct perf_event_context *ctx;

	ctx = container_of(head, struct perf_event_context, rcu_head);
	kfree(ctx);
}

static void put_ctx(struct perf_event_context *ctx)
{
	if (refcount_dec_and_test(&ctx->refcount)) {
		if (ctx->parent_ctx)
			put_ctx(ctx->parent_ctx);
		if (ctx->task && ctx->task != TASK_TOMBSTONE)
			put_task_struct(ctx->task);
		call_rcu(&ctx->rcu_head, free_ctx);
	}
}

/*
 * Because of perf_event::ctx migration in sys_perf_event_open::move_group and
 * perf_pmu_migrate_context() we need some magic.
 *
 * Those places that change perf_event::ctx will hold both
 * perf_event_ctx::mutex of the 'old' and 'new' ctx value.
 *
 * Lock ordering is by mutex address. There are two other sites where
 * perf_event_context::mutex nests and those are:
 *
 *  - perf_event_exit_task_context()	[ child , 0 ]
 *      perf_event_exit_event()
 *        put_event()			[ parent, 1 ]
 *
 *  - perf_event_init_context()		[ parent, 0 ]
 *      inherit_task_group()
 *        inherit_group()
 *          inherit_event()
 *            perf_event_alloc()
 *              perf_init_event()
 *                perf_try_init_event()	[ child , 1 ]
 *
 * While it appears there is an obvious deadlock here -- the parent and child
 * nesting levels are inverted between the two. This is in fact safe because
 * life-time rules separate them. That is an exiting task cannot fork, and a
 * spawning task cannot (yet) exit.
 *
 * But remember that these are parent<->child context relations, and
 * migration does not affect children, therefore these two orderings should not
 * interact.
 *
 * The change in perf_event::ctx does not affect children (as claimed above)
 * because the sys_perf_event_open() case will install a new event and break
 * the ctx parent<->child relation, and perf_pmu_migrate_context() is only
 * concerned with cpuctx and that doesn't have children.
 *
 * The places that change perf_event::ctx will issue:
 *
 *   perf_remove_from_context();
 *   synchronize_rcu();
 *   perf_install_in_context();
 *
 * to affect the change. The remove_from_context() + synchronize_rcu() should
 * quiesce the event, after which we can install it in the new location. This
 * means that only external vectors (perf_fops, prctl) can perturb the event
 * while in transit. Therefore all such accessors should also acquire
 * perf_event_context::mutex to serialize against this.
 *
 * However; because event->ctx can change while we're waiting to acquire
 * ctx->mutex we must be careful and use the below perf_event_ctx_lock()
 * function.
 *
 * Lock order:
 *    exec_update_lock
 *	task_struct::perf_event_mutex
 *	  perf_event_context::mutex
 *	    perf_event::child_mutex;
 *	      perf_event_context::lock
 *	    mmap_lock
 *	      perf_event::mmap_mutex
 *	        perf_buffer::aux_mutex
 *	      perf_addr_filters_head::lock
 *
 *    cpu_hotplug_lock
 *      pmus_lock
 *	  cpuctx->mutex / perf_event_context::mutex
 */
static struct perf_event_context *
perf_event_ctx_lock_nested(struct perf_event *event, int nesting)
{
	struct perf_event_context *ctx;

again:
	rcu_read_lock();
	ctx = READ_ONCE(event->ctx);
	if (!refcount_inc_not_zero(&ctx->refcount)) {
		rcu_read_unlock();
		goto again;
	}
	rcu_read_unlock();

	mutex_lock_nested(&ctx->mutex, nesting);
	if (event->ctx != ctx) {
		mutex_unlock(&ctx->mutex);
		put_ctx(ctx);
		goto again;
	}

	return ctx;
}

static inline struct perf_event_context *
perf_event_ctx_lock(struct perf_event *event)
{
	return perf_event_ctx_lock_nested(event, 0);
}

static void perf_event_ctx_unlock(struct perf_event *event,
				  struct perf_event_context *ctx)
{
	mutex_unlock(&ctx->mutex);
	put_ctx(ctx);
}

/*
 * This must be done under the ctx->lock, such as to serialize against
 * context_equiv(), therefore we cannot call put_ctx() since that might end up
 * calling scheduler related locks and ctx->lock nests inside those.
 */
static __must_check struct perf_event_context *
unclone_ctx(struct perf_event_context *ctx)
{
	struct perf_event_context *parent_ctx = ctx->parent_ctx;

	lockdep_assert_held(&ctx->lock);

	if (parent_ctx)
		ctx->parent_ctx = NULL;
	ctx->generation++;

	return parent_ctx;
}

static u32 perf_event_pid_type(struct perf_event *event, struct task_struct *p,
				enum pid_type type)
{
	u32 nr;
	/*
	 * only top level events have the pid namespace they were created in
	 */
	if (event->parent)
		event = event->parent;

	nr = __task_pid_nr_ns(p, type, event->ns);
	/* avoid -1 if it is idle thread or runs in another ns */
	if (!nr && !pid_alive(p))
		nr = -1;
	return nr;
}

static u32 perf_event_pid(struct perf_event *event, struct task_struct *p)
{
	return perf_event_pid_type(event, p, PIDTYPE_TGID);
}

static u32 perf_event_tid(struct perf_event *event, struct task_struct *p)
{
	return perf_event_pid_type(event, p, PIDTYPE_PID);
}

/*
 * If we inherit events we want to return the parent event id
 * to userspace.
 */
static u64 primary_event_id(struct perf_event *event)
{
	u64 id = event->id;

	if (event->parent)
		id = event->parent->id;

	return id;
}

/*
 * Get the perf_event_context for a task and lock it.
 *
 * This has to cope with the fact that until it is locked,
 * the context could get moved to another task.
 */
static struct perf_event_context *
perf_lock_task_context(struct task_struct *task, unsigned long *flags)
{
	struct perf_event_context *ctx;

retry:
	/*
	 * One of the few rules of preemptible RCU is that one cannot do
	 * rcu_read_unlock() while holding a scheduler (or nested) lock when
	 * part of the read side critical section was irqs-enabled -- see
	 * rcu_read_unlock_special().
	 *
	 * Since ctx->lock nests under rq->lock we must ensure the entire read
	 * side critical section has interrupts disabled.
	 */
	local_irq_save(*flags);
	rcu_read_lock();
	ctx = rcu_dereference(task->perf_event_ctxp);
	if (ctx) {
		/*
		 * If this context is a clone of another, it might
		 * get swapped for another underneath us by
		 * perf_event_task_sched_out, though the
		 * rcu_read_lock() protects us from any context
		 * getting freed.  Lock the context and check if it
		 * got swapped before we could get the lock, and retry
		 * if so.  If we locked the right context, then it
		 * can't get swapped on us any more.
		 */
		raw_spin_lock(&ctx->lock);
		if (ctx != rcu_dereference(task->perf_event_ctxp)) {
			raw_spin_unlock(&ctx->lock);
			rcu_read_unlock();
			local_irq_restore(*flags);
			goto retry;
		}

		if (ctx->task == TASK_TOMBSTONE ||
		    !refcount_inc_not_zero(&ctx->refcount)) {
			raw_spin_unlock(&ctx->lock);
			ctx = NULL;
		} else {
			WARN_ON_ONCE(ctx->task != task);
		}
	}
	rcu_read_unlock();
	if (!ctx)
		local_irq_restore(*flags);
	return ctx;
}

/*
 * Get the context for a task and increment its pin_count so it
 * can't get swapped to another task.  This also increments its
 * reference count so that the context can't get freed.
 */
static struct perf_event_context *
perf_pin_task_context(struct task_struct *task)
{
	struct perf_event_context *ctx;
	unsigned long flags;

	ctx = perf_lock_task_context(task, &flags);
	if (ctx) {
		++ctx->pin_count;
		raw_spin_unlock_irqrestore(&ctx->lock, flags);
	}
	return ctx;
}

static void perf_unpin_context(struct perf_event_context *ctx)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&ctx->lock, flags);
	--ctx->pin_count;
	raw_spin_unlock_irqrestore(&ctx->lock, flags);
}

/*
 * Update the record of the current time in a context.
 */
static void __update_context_time(struct perf_event_context *ctx, bool adv)
{
	u64 now = perf_clock();

	lockdep_assert_held(&ctx->lock);

	if (adv)
		ctx->time += now - ctx->timestamp;
	ctx->timestamp = now;

	/*
	 * The above: time' = time + (now - timestamp), can be re-arranged
	 * into: time` = now + (time - timestamp), which gives a single value
	 * offset to compute future time without locks on.
	 *
	 * See perf_event_time_now(), which can be used from NMI context where
	 * it's (obviously) not possible to acquire ctx->lock in order to read
	 * both the above values in a consistent manner.
	 */
	WRITE_ONCE(ctx->timeoffset, ctx->time - ctx->timestamp);
}

static void update_context_time(struct perf_event_context *ctx)
{
	__update_context_time(ctx, true);
}

static u64 perf_event_time(struct perf_event *event)
{
	struct perf_event_context *ctx = event->ctx;

	if (unlikely(!ctx))
		return 0;

	if (is_cgroup_event(event))
		return perf_cgroup_event_time(event);

	return ctx->time;
}

static u64 perf_event_time_now(struct perf_event *event, u64 now)
{
	struct perf_event_context *ctx = event->ctx;

	if (unlikely(!ctx))
		return 0;

	if (is_cgroup_event(event))
		return perf_cgroup_event_time_now(event, now);

	if (!(__load_acquire(&ctx->is_active) & EVENT_TIME))
		return ctx->time;

	now += READ_ONCE(ctx->timeoffset);
	return now;
}

static enum event_type_t get_event_type(struct perf_event *event)
{
	struct perf_event_context *ctx = event->ctx;
	enum event_type_t event_type;

	lockdep_assert_held(&ctx->lock);

	/*
	 * It's 'group type', really, because if our group leader is
	 * pinned, so are we.
	 */
	if (event->group_leader != event)
		event = event->group_leader;

	event_type = event->attr.pinned ? EVENT_PINNED : EVENT_FLEXIBLE;
	if (!ctx->task)
		event_type |= EVENT_CPU;

	return event_type;
}

/*
 * Helper function to initialize event group nodes.
 */
static void init_event_group(struct perf_event *event)
{
	RB_CLEAR_NODE(&event->group_node);
	event->group_index = 0;
}

/*
 * Extract pinned or flexible groups from the context
 * based on event attrs bits.
 */
static struct perf_event_groups *
get_event_groups(struct perf_event *event, struct perf_event_context *ctx)
{
	if (event->attr.pinned)
		return &ctx->pinned_groups;
	else
		return &ctx->flexible_groups;
}

/*
 * Helper function to initializes perf_event_group trees.
 */
static void perf_event_groups_init(struct perf_event_groups *groups)
{
	groups->tree = RB_ROOT;
	groups->index = 0;
}

static inline struct cgroup *event_cgroup(const struct perf_event *event)
{
	struct cgroup *cgroup = NULL;

#ifdef CONFIG_CGROUP_PERF
	if (event->cgrp)
		cgroup = event->cgrp->css.cgroup;
#endif

	return cgroup;
}

/*
 * Compare function for event groups;
 *
 * Implements complex key that first sorts by CPU and then by virtual index
 * which provides ordering when rotating groups for the same CPU.
 */
static __always_inline int
perf_event_groups_cmp(const int left_cpu, const struct pmu *left_pmu,
		      const struct cgroup *left_cgroup, const u64 left_group_index,
		      const struct perf_event *right)
{
	if (left_cpu < right->cpu)
		return -1;
	if (left_cpu > right->cpu)
		return 1;

	if (left_pmu) {
		if (left_pmu < right->pmu_ctx->pmu)
			return -1;
		if (left_pmu > right->pmu_ctx->pmu)
			return 1;
	}

#ifdef CONFIG_CGROUP_PERF
	{
		const struct cgroup *right_cgroup = event_cgroup(right);

		if (left_cgroup != right_cgroup) {
			if (!left_cgroup) {
				/*
				 * Left has no cgroup but right does, no
				 * cgroups come first.
				 */
				return -1;
			}
			if (!right_cgroup) {
				/*
				 * Right has no cgroup but left does, no
				 * cgroups come first.
				 */
				return 1;
			}
			/* Two dissimilar cgroups, order by id. */
			if (cgroup_id(left_cgroup) < cgroup_id(right_cgroup))
				return -1;

			return 1;
		}
	}
#endif

	if (left_group_index < right->group_index)
		return -1;
	if (left_group_index > right->group_index)
		return 1;

	return 0;
}

#define __node_2_pe(node) \
	rb_entry((node), struct perf_event, group_node)

static inline bool __group_less(struct rb_node *a, const struct rb_node *b)
{
	struct perf_event *e = __node_2_pe(a);
	return perf_event_groups_cmp(e->cpu, e->pmu_ctx->pmu, event_cgroup(e),
				     e->group_index, __node_2_pe(b)) < 0;
}

struct __group_key {
	int cpu;
	struct pmu *pmu;
	struct cgroup *cgroup;
};

static inline int __group_cmp(const void *key, const struct rb_node *node)
{
	const struct __group_key *a = key;
	const struct perf_event *b = __node_2_pe(node);

	/* partial/subtree match: @cpu, @pmu, @cgroup; ignore: @group_index */
	return perf_event_groups_cmp(a->cpu, a->pmu, a->cgroup, b->group_index, b);
}

static inline int
__group_cmp_ignore_cgroup(const void *key, const struct rb_node *node)
{
	const struct __group_key *a = key;
	const struct perf_event *b = __node_2_pe(node);

	/* partial/subtree match: @cpu, @pmu, ignore: @cgroup, @group_index */
	return perf_event_groups_cmp(a->cpu, a->pmu, event_cgroup(b),
				     b->group_index, b);
}

/*
 * Insert @event into @groups' tree; using
 *   {@event->cpu, @event->pmu_ctx->pmu, event_cgroup(@event), ++@groups->index}
 * as key. This places it last inside the {cpu,pmu,cgroup} subtree.
 */
static void
perf_event_groups_insert(struct perf_event_groups *groups,
			 struct perf_event *event)
{
	event->group_index = ++groups->index;

	rb_add(&event->group_node, &groups->tree, __group_less);
}

/*
 * Helper function to insert event into the pinned or flexible groups.
 */
static void
add_event_to_groups(struct perf_event *event, struct perf_event_context *ctx)
{
	struct perf_event_groups *groups;

	groups = get_event_groups(event, ctx);
	perf_event_groups_insert(groups, event);
}

/*
 * Delete a group from a tree.
 */
static void
perf_event_groups_delete(struct perf_event_groups *groups,
			 struct perf_event *event)
{
	WARN_ON_ONCE(RB_EMPTY_NODE(&event->group_node) ||
		     RB_EMPTY_ROOT(&groups->tree));

	rb_erase(&event->group_node, &groups->tree);
	init_event_group(event);
}

/*
 * Helper function to delete event from its groups.
 */
static void
del_event_from_groups(struct perf_event *event, struct perf_event_context *ctx)
{
	struct perf_event_groups *groups;

	groups = get_event_groups(event, ctx);
	perf_event_groups_delete(groups, event);
}

/*
 * Get the leftmost event in the {cpu,pmu,cgroup} subtree.
 */
static struct perf_event *
perf_event_groups_first(struct perf_event_groups *groups, int cpu,
			struct pmu *pmu, struct cgroup *cgrp)
{
	struct __group_key key = {
		.cpu = cpu,
		.pmu = pmu,
		.cgroup = cgrp,
	};
	struct rb_node *node;

	node = rb_find_first(&key, &groups->tree, __group_cmp);
	if (node)
		return __node_2_pe(node);

	return NULL;
}

static struct perf_event *
perf_event_groups_next(struct perf_event *event, struct pmu *pmu)
{
	struct __group_key key = {
		.cpu = event->cpu,
		.pmu = pmu,
		.cgroup = event_cgroup(event),
	};
	struct rb_node *next;

	next = rb_next_match(&key, &event->group_node, __group_cmp);
	if (next)
		return __node_2_pe(next);

	return NULL;
}

#define perf_event_groups_for_cpu_pmu(event, groups, cpu, pmu)		\
	for (event = perf_event_groups_first(groups, cpu, pmu, NULL);	\
	     event; event = perf_event_groups_next(event, pmu))

/*
 * Iterate through the whole groups tree.
 */
#define perf_event_groups_for_each(event, groups)			\
	for (event = rb_entry_safe(rb_first(&((groups)->tree)),		\
				typeof(*event), group_node); event;	\
		event = rb_entry_safe(rb_next(&event->group_node),	\
				typeof(*event), group_node))

/*
 * Does the event attribute request inherit with PERF_SAMPLE_READ
 */
static inline bool has_inherit_and_sample_read(struct perf_event_attr *attr)
{
	return attr->inherit && (attr->sample_type & PERF_SAMPLE_READ);
}

/*
 * Add an event from the lists for its context.
 * Must be called with ctx->mutex and ctx->lock held.
 */
static void
list_add_event(struct perf_event *event, struct perf_event_context *ctx)
{
	lockdep_assert_held(&ctx->lock);

	WARN_ON_ONCE(event->attach_state & PERF_ATTACH_CONTEXT);
	event->attach_state |= PERF_ATTACH_CONTEXT;

	event->tstamp = perf_event_time(event);

	/*
	 * If we're a stand alone event or group leader, we go to the context
	 * list, group events are kept attached to the group so that
	 * perf_group_detach can, at all times, locate all siblings.
	 */
	if (event->group_leader == event) {
		event->group_caps = event->event_caps;
		add_event_to_groups(event, ctx);
	}

	list_add_rcu(&event->event_entry, &ctx->event_list);
	ctx->nr_events++;
	if (event->hw.flags & PERF_EVENT_FLAG_USER_READ_CNT)
		ctx->nr_user++;
	if (event->attr.inherit_stat)
		ctx->nr_stat++;
	if (has_inherit_and_sample_read(&event->attr))
		local_inc(&ctx->nr_no_switch_fast);

	if (event->state > PERF_EVENT_STATE_OFF)
		perf_cgroup_event_enable(event, ctx);

	ctx->generation++;
	event->pmu_ctx->nr_events++;
}

/*
 * Initialize event state based on the perf_event_attr::disabled.
 */
static inline void perf_event__state_init(struct perf_event *event)
{
	event->state = event->attr.disabled ? PERF_EVENT_STATE_OFF :
					      PERF_EVENT_STATE_INACTIVE;
}

static int __perf_event_read_size(u64 read_format, int nr_siblings)
{
	int entry = sizeof(u64); /* value */
	int size = 0;
	int nr = 1;

	if (read_format & PERF_FORMAT_TOTAL_TIME_ENABLED)
		size += sizeof(u64);

	if (read_format & PERF_FORMAT_TOTAL_TIME_RUNNING)
		size += sizeof(u64);

	if (read_format & PERF_FORMAT_ID)
		entry += sizeof(u64);

	if (read_format & PERF_FORMAT_LOST)
		entry += sizeof(u64);

	if (read_format & PERF_FORMAT_GROUP) {
		nr += nr_siblings;
		size += sizeof(u64);
	}

	/*
	 * Since perf_event_validate_size() limits this to 16k and inhibits
	 * adding more siblings, this will never overflow.
	 */
	return size + nr * entry;
}

static void __perf_event_header_size(struct perf_event *event, u64 sample_type)
{
	struct perf_sample_data *data;
	u16 size = 0;

	if (sample_type & PERF_SAMPLE_IP)
		size += sizeof(data->ip);

	if (sample_type & PERF_SAMPLE_ADDR)
		size += sizeof(data->addr);

	if (sample_type & PERF_SAMPLE_PERIOD)
		size += sizeof(data->period);

	if (sample_type & PERF_SAMPLE_WEIGHT_TYPE)
		size += sizeof(data->weight.full);

	if (sample_type & PERF_SAMPLE_READ)
		size += event->read_size;

	if (sample_type & PERF_SAMPLE_DATA_SRC)
		size += sizeof(data->data_src.val);

	if (sample_type & PERF_SAMPLE_TRANSACTION)
		size += sizeof(data->txn);

	if (sample_type & PERF_SAMPLE_PHYS_ADDR)
		size += sizeof(data->phys_addr);

	if (sample_type & PERF_SAMPLE_CGROUP)
		size += sizeof(data->cgroup);

	if (sample_type & PERF_SAMPLE_DATA_PAGE_SIZE)
		size += sizeof(data->data_page_size);

	if (sample_type & PERF_SAMPLE_CODE_PAGE_SIZE)
		size += sizeof(data->code_page_size);

	event->header_size = size;
}

/*
 * Called at perf_event creation and when events are attached/detached from a
 * group.
 */
static void perf_event__header_size(struct perf_event *event)
{
	event->read_size =
		__perf_event_read_size(event->attr.read_format,
				       event->group_leader->nr_siblings);
	__perf_event_header_size(event, event->attr.sample_type);
}

static void perf_event__id_header_size(struct perf_event *event)
{
	struct perf_sample_data *data;
	u64 sample_type = event->attr.sample_type;
	u16 size = 0;

	if (sample_type & PERF_SAMPLE_TID)
		size += sizeof(data->tid_entry);

	if (sample_type & PERF_SAMPLE_TIME)
		size += sizeof(data->time);

	if (sample_type & PERF_SAMPLE_IDENTIFIER)
		size += sizeof(data->id);

	if (sample_type & PERF_SAMPLE_ID)
		size += sizeof(data->id);

	if (sample_type & PERF_SAMPLE_STREAM_ID)
		size += sizeof(data->stream_id);

	if (sample_type & PERF_SAMPLE_CPU)
		size += sizeof(data->cpu_entry);

	event->id_header_size = size;
}

/*
 * Check that adding an event to the group does not result in anybody
 * overflowing the 64k event limit imposed by the output buffer.
 *
 * Specifically, check that the read_size for the event does not exceed 16k,
 * read_size being the one term that grows with groups size. Since read_size
 * depends on per-event read_format, also (re)check the existing events.
 *
 * This leaves 48k for the constant size fields and things like callchains,
 * branch stacks and register sets.
 */
static bool perf_event_validate_size(struct perf_event *event)
{
	struct perf_event *sibling, *group_leader = event->group_leader;

	if (__perf_event_read_size(event->attr.read_format,
				   group_leader->nr_siblings + 1) > 16*1024)
		return false;

	if (__perf_event_read_size(group_leader->attr.read_format,
				   group_leader->nr_siblings + 1) > 16*1024)
		return false;

	/*
	 * When creating a new group leader, group_leader->ctx is initialized
	 * after the size has been validated, but we cannot safely use
	 * for_each_sibling_event() until group_leader->ctx is set. A new group
	 * leader cannot have any siblings yet, so we can safely skip checking
	 * the non-existent siblings.
	 */
	if (event == group_leader)
		return true;

	for_each_sibling_event(sibling, group_leader) {
		if (__perf_event_read_size(sibling->attr.read_format,
					   group_leader->nr_siblings + 1) > 16*1024)
			return false;
	}

	return true;
}

static void perf_group_attach(struct perf_event *event)
{
	struct perf_event *group_leader = event->group_leader, *pos;

	lockdep_assert_held(&event->ctx->lock);

	/*
	 * We can have double attach due to group movement (move_group) in
	 * perf_event_open().
	 */
	if (event->attach_state & PERF_ATTACH_GROUP)
		return;

	event->attach_state |= PERF_ATTACH_GROUP;

	if (group_leader == event)
		return;

	WARN_ON_ONCE(group_leader->ctx != event->ctx);

	group_leader->group_caps &= event->event_caps;

	list_add_tail(&event->sibling_list, &group_leader->sibling_list);
	group_leader->nr_siblings++;
	group_leader->group_generation++;

	perf_event__header_size(group_leader);

	for_each_sibling_event(pos, group_leader)
		perf_event__header_size(pos);
}

/*
 * Remove an event from the lists for its context.
 * Must be called with ctx->mutex and ctx->lock held.
 */
static void
list_del_event(struct perf_event *event, struct perf_event_context *ctx)
{
	WARN_ON_ONCE(event->ctx != ctx);
	lockdep_assert_held(&ctx->lock);

	/*
	 * We can have double detach due to exit/hot-unplug + close.
	 */
	if (!(event->attach_state & PERF_ATTACH_CONTEXT))
		return;

	event->attach_state &= ~PERF_ATTACH_CONTEXT;

	ctx->nr_events--;
	if (event->hw.flags & PERF_EVENT_FLAG_USER_READ_CNT)
		ctx->nr_user--;
	if (event->attr.inherit_stat)
		ctx->nr_stat--;
	if (has_inherit_and_sample_read(&event->attr))
		local_dec(&ctx->nr_no_switch_fast);

	list_del_rcu(&event->event_entry);

	if (event->group_leader == event)
		del_event_from_groups(event, ctx);

	/*
	 * If event was in error state, then keep it
	 * that way, otherwise bogus counts will be
	 * returned on read(). The only way to get out
	 * of error state is by explicit re-enabling
	 * of the event
	 */
	if (event->state > PERF_EVENT_STATE_OFF) {
		perf_cgroup_event_disable(event, ctx);
		perf_event_set_state(event, PERF_EVENT_STATE_OFF);
	}

	ctx->generation++;
	event->pmu_ctx->nr_events--;
}

static int
perf_aux_output_match(struct perf_event *event, struct perf_event *aux_event)
{
	if (!has_aux(aux_event))
		return 0;

	if (!event->pmu->aux_output_match)
		return 0;

	return event->pmu->aux_output_match(aux_event);
}

static void put_event(struct perf_event *event);
static void __event_disable(struct perf_event *event,
			    struct perf_event_context *ctx,
			    enum perf_event_state state);

static void perf_put_aux_event(struct perf_event *event)
{
	struct perf_event_context *ctx = event->ctx;
	struct perf_event *iter;

	/*
	 * If event uses aux_event tear down the link
	 */
	if (event->aux_event) {
		iter = event->aux_event;
		event->aux_event = NULL;
		put_event(iter);
		return;
	}

	/*
	 * If the event is an aux_event, tear down all links to
	 * it from other events.
	 */
	for_each_sibling_event(iter, event->group_leader) {
		if (iter->aux_event != event)
			continue;

		iter->aux_event = NULL;
		put_event(event);

		/*
		 * If it's ACTIVE, schedule it out and put it into ERROR
		 * state so that we don't try to schedule it again. Note
		 * that perf_event_enable() will clear the ERROR status.
		 */
		__event_disable(iter, ctx, PERF_EVENT_STATE_ERROR);
	}
}

static bool perf_need_aux_event(struct perf_event *event)
{
	return event->attr.aux_output || has_aux_action(event);
}

static int perf_get_aux_event(struct perf_event *event,
			      struct perf_event *group_leader)
{
	/*
	 * Our group leader must be an aux event if we want to be
	 * an aux_output. This way, the aux event will precede its
	 * aux_output events in the group, and therefore will always
	 * schedule first.
	 */
	if (!group_leader)
		return 0;

	/*
	 * aux_output and aux_sample_size are mutually exclusive.
	 */
	if (event->attr.aux_output && event->attr.aux_sample_size)
		return 0;

	if (event->attr.aux_output &&
	    !perf_aux_output_match(event, group_leader))
		return 0;

	if ((event->attr.aux_pause || event->attr.aux_resume) &&
	    !(group_leader->pmu->capabilities & PERF_PMU_CAP_AUX_PAUSE))
		return 0;

	if (event->attr.aux_sample_size && !group_leader->pmu->snapshot_aux)
		return 0;

	if (!atomic_long_inc_not_zero(&group_leader->refcount))
		return 0;

	/*
	 * Link aux_outputs to their aux event; this is undone in
	 * perf_group_detach() by perf_put_aux_event(). When the
	 * group in torn down, the aux_output events loose their
	 * link to the aux_event and can't schedule any more.
	 */
	event->aux_event = group_leader;

	return 1;
}

static inline struct list_head *get_event_list(struct perf_event *event)
{
	return event->attr.pinned ? &event->pmu_ctx->pinned_active :
				    &event->pmu_ctx->flexible_active;
}

static void perf_group_detach(struct perf_event *event)
{
	struct perf_event *leader = event->group_leader;
	struct perf_event *sibling, *tmp;
	struct perf_event_context *ctx = event->ctx;

	lockdep_assert_held(&ctx->lock);

	/*
	 * We can have double detach due to exit/hot-unplug + close.
	 */
	if (!(event->attach_state & PERF_ATTACH_GROUP))
		return;

	event->attach_state &= ~PERF_ATTACH_GROUP;

	perf_put_aux_event(event);

	/*
	 * If this is a sibling, remove it from its group.
	 */
	if (leader != event) {
		list_del_init(&event->sibling_list);
		event->group_leader->nr_siblings--;
		event->group_leader->group_generation++;
		goto out;
	}

	/*
	 * If this was a group event with sibling events then
	 * upgrade the siblings to singleton events by adding them
	 * to whatever list we are on.
	 */
	list_for_each_entry_safe(sibling, tmp, &event->sibling_list, sibling_list) {

		/*
		 * Events that have PERF_EV_CAP_SIBLING require being part of
		 * a group and cannot exist on their own, schedule them out
		 * and move them into the ERROR state. Also see
		 * _perf_event_enable(), it will not be able to recover this
		 * ERROR state.
		 */
		if (sibling->event_caps & PERF_EV_CAP_SIBLING)
			__event_disable(sibling, ctx, PERF_EVENT_STATE_ERROR);

		sibling->group_leader = sibling;
		list_del_init(&sibling->sibling_list);

		/* Inherit group flags from the previous leader */
		sibling->group_caps = event->group_caps;

		if (sibling->attach_state & PERF_ATTACH_CONTEXT) {
			add_event_to_groups(sibling, event->ctx);

			if (sibling->state == PERF_EVENT_STATE_ACTIVE)
				list_add_tail(&sibling->active_list, get_event_list(sibling));
		}

		WARN_ON_ONCE(sibling->ctx != event->ctx);
	}

out:
	for_each_sibling_event(tmp, leader)
		perf_event__header_size(tmp);

	perf_event__header_size(leader);
}

static void sync_child_event(struct perf_event *child_event);

static void perf_child_detach(struct perf_event *event)
{
	struct perf_event *parent_event = event->parent;

	if (!(event->attach_state & PERF_ATTACH_CHILD))
		return;

	event->attach_state &= ~PERF_ATTACH_CHILD;

	if (WARN_ON_ONCE(!parent_event))
		return;

	lockdep_assert_held(&parent_event->child_mutex);

	sync_child_event(event);
	list_del_init(&event->child_list);
}

static bool is_orphaned_event(struct perf_event *event)
{
	return event->state == PERF_EVENT_STATE_DEAD;
}

static inline int
event_filter_match(struct perf_event *event)
{
	return (event->cpu == -1 || event->cpu == smp_processor_id()) &&
	       perf_cgroup_match(event);
}

static void
event_sched_out(struct perf_event *event, struct perf_event_context *ctx)
{
	struct perf_event_pmu_context *epc = event->pmu_ctx;
	struct perf_cpu_pmu_context *cpc = this_cpc(epc->pmu);
	enum perf_event_state state = PERF_EVENT_STATE_INACTIVE;

	// XXX cpc serialization, probably per-cpu IRQ disabled

	WARN_ON_ONCE(event->ctx != ctx);
	lockdep_assert_held(&ctx->lock);

	if (event->state != PERF_EVENT_STATE_ACTIVE)
		return;

	/*
	 * Asymmetry; we only schedule events _IN_ through ctx_sched_in(), but
	 * we can schedule events _OUT_ individually through things like
	 * __perf_remove_from_context().
	 */
	list_del_init(&event->active_list);

	perf_pmu_disable(event->pmu);

	event->pmu->del(event, 0);
	event->oncpu = -1;

	if (event->pending_disable) {
		event->pending_disable = 0;
		perf_cgroup_event_disable(event, ctx);
		state = PERF_EVENT_STATE_OFF;
	}

	perf_event_set_state(event, state);

	if (!is_software_event(event))
		cpc->active_oncpu--;
	if (event->attr.freq && event->attr.sample_freq) {
		ctx->nr_freq--;
		epc->nr_freq--;
	}
	if (event->attr.exclusive || !cpc->active_oncpu)
		cpc->exclusive = 0;

	perf_pmu_enable(event->pmu);
}

static void
group_sched_out(struct perf_event *group_event, struct perf_event_context *ctx)
{
	struct perf_event *event;

	if (group_event->state != PERF_EVENT_STATE_ACTIVE)
		return;

	perf_assert_pmu_disabled(group_event->pmu_ctx->pmu);

	event_sched_out(group_event, ctx);

	/*
	 * Schedule out siblings (if any):
	 */
	for_each_sibling_event(event, group_event)
		event_sched_out(event, ctx);
}

static inline void
__ctx_time_update(struct perf_cpu_context *cpuctx, struct perf_event_context *ctx, bool final)
{
	if (ctx->is_active & EVENT_TIME) {
		if (ctx->is_active & EVENT_FROZEN)
			return;
		update_context_time(ctx);
		update_cgrp_time_from_cpuctx(cpuctx, final);
	}
}

static inline void
ctx_time_update(struct perf_cpu_context *cpuctx, struct perf_event_context *ctx)
{
	__ctx_time_update(cpuctx, ctx, false);
}

/*
 * To be used inside perf_ctx_lock() / perf_ctx_unlock(). Lasts until perf_ctx_unlock().
 */
static inline void
ctx_time_freeze(struct perf_cpu_context *cpuctx, struct perf_event_context *ctx)
{
	ctx_time_update(cpuctx, ctx);
	if (ctx->is_active & EVENT_TIME)
		ctx->is_active |= EVENT_FROZEN;
}

static inline void
ctx_time_update_event(struct perf_event_context *ctx, struct perf_event *event)
{
	if (ctx->is_active & EVENT_TIME) {
		if (ctx->is_active & EVENT_FROZEN)
			return;
		update_context_time(ctx);
		update_cgrp_time_from_event(event);
	}
}

#define DETACH_GROUP	0x01UL
#define DETACH_CHILD	0x02UL
#define DETACH_DEAD	0x04UL
#define DETACH_EXIT	0x08UL

/*
 * Cross CPU call to remove a performance event
 *
 * We disable the event on the hardware level first. After that we
 * remove it from the context list.
 */
static void
__perf_remove_from_context(struct perf_event *event,
			   struct perf_cpu_context *cpuctx,
			   struct perf_event_context *ctx,
			   void *info)
{
	struct perf_event_pmu_context *pmu_ctx = event->pmu_ctx;
	enum perf_event_state state = PERF_EVENT_STATE_OFF;
	unsigned long flags = (unsigned long)info;

	ctx_time_update(cpuctx, ctx);

	/*
	 * Ensure event_sched_out() switches to OFF, at the very least
	 * this avoids raising perf_pending_task() at this time.
	 */
	if (flags & DETACH_EXIT)
		state = PERF_EVENT_STATE_EXIT;
	if (flags & DETACH_DEAD) {
		event->pending_disable = 1;
		state = PERF_EVENT_STATE_DEAD;
	}
	event_sched_out(event, ctx);
	perf_event_set_state(event, min(event->state, state));
	if (flags & DETACH_GROUP)
		perf_group_detach(event);
	if (flags & DETACH_CHILD)
		perf_child_detach(event);
	list_del_event(event, ctx);

	if (!pmu_ctx->nr_events) {
		pmu_ctx->rotate_necessary = 0;

		if (ctx->task && ctx->is_active) {
			struct perf_cpu_pmu_context *cpc = this_cpc(pmu_ctx->pmu);

			WARN_ON_ONCE(cpc->task_epc && cpc->task_epc != pmu_ctx);
			cpc->task_epc = NULL;
		}
	}

	if (!ctx->nr_events && ctx->is_active) {
		if (ctx == &cpuctx->ctx)
			update_cgrp_time_from_cpuctx(cpuctx, true);

		ctx->is_active = 0;
		if (ctx->task) {
			WARN_ON_ONCE(cpuctx->task_ctx != ctx);
			cpuctx->task_ctx = NULL;
		}
	}
}

/*
 * Remove the event from a task's (or a CPU's) list of events.
 *
 * If event->ctx is a cloned context, callers must make sure that
 * every task struct that event->ctx->task could possibly point to
 * remains valid.  This is OK when called from perf_release since
 * that only calls us on the top-level context, which can't be a clone.
 * When called from perf_event_exit_task, it's OK because the
 * context has been detached from its task.
 */
static void perf_remove_from_context(struct perf_event *event, unsigned long flags)
{
	struct perf_event_context *ctx = event->ctx;

	lockdep_assert_held(&ctx->mutex);

	/*
	 * Because of perf_event_exit_task(), perf_remove_from_context() ought
	 * to work in the face of TASK_TOMBSTONE, unlike every other
	 * event_function_call() user.
	 */
	raw_spin_lock_irq(&ctx->lock);
	if (!ctx->is_active) {
		__perf_remove_from_context(event, this_cpu_ptr(&perf_cpu_context),
					   ctx, (void *)flags);
		raw_spin_unlock_irq(&ctx->lock);
		return;
	}
	raw_spin_unlock_irq(&ctx->lock);

	event_function_call(event, __perf_remove_from_context, (void *)flags);
}

static void __event_disable(struct perf_event *event,
			    struct perf_event_context *ctx,
			    enum perf_event_state state)
{
	event_sched_out(event, ctx);
	perf_cgroup_event_disable(event, ctx);
	perf_event_set_state(event, state);
}

/*
 * Cross CPU call to disable a performance event
 */
static void __perf_event_disable(struct perf_event *event,
				 struct perf_cpu_context *cpuctx,
				 struct perf_event_context *ctx,
				 void *info)
{
	if (event->state < PERF_EVENT_STATE_INACTIVE)
		return;

	perf_pmu_disable(event->pmu_ctx->pmu);
	ctx_time_update_event(ctx, event);

	/*
	 * When disabling a group leader, the whole group becomes ineligible
	 * to run, so schedule out the full group.
	 */
	if (event == event->group_leader)
		group_sched_out(event, ctx);

	/*
	 * But only mark the leader OFF; the siblings will remain
	 * INACTIVE.
	 */
	__event_disable(event, ctx, PERF_EVENT_STATE_OFF);

	perf_pmu_enable(event->pmu_ctx->pmu);
}

/*
 * Disable an event.
 *
 * If event->ctx is a cloned context, callers must make sure that
 * every task struct that event->ctx->task could possibly point to
 * remains valid.  This condition is satisfied when called through
 * perf_event_for_each_child or perf_event_for_each because they
 * hold the top-level event's child_mutex, so any descendant that
 * goes to exit will block in perf_event_exit_event().
 *
 * When called from perf_pending_disable it's OK because event->ctx
 * is the current context on this CPU and preemption is disabled,
 * hence we can't get into perf_event_task_sched_out for this context.
 */
static void _perf_event_disable(struct perf_event *event)
{
	struct perf_event_context *ctx = event->ctx;

	raw_spin_lock_irq(&ctx->lock);
	if (event->state <= PERF_EVENT_STATE_OFF) {
		raw_spin_unlock_irq(&ctx->lock);
		return;
	}
	raw_spin_unlock_irq(&ctx->lock);

	event_function_call(event, __perf_event_disable, NULL);
}

void perf_event_disable_local(struct perf_event *event)
{
	event_function_local(event, __perf_event_disable, NULL);
}

/*
 * Strictly speaking kernel users cannot create groups and therefore this
 * interface does not need the perf_event_ctx_lock() magic.
 */
void perf_event_disable(struct perf_event *event)
{
	struct perf_event_context *ctx;

	ctx = perf_event_ctx_lock(event);
	_perf_event_disable(event);
	perf_event_ctx_unlock(event, ctx);
}
EXPORT_SYMBOL_GPL(perf_event_disable);

void perf_event_disable_inatomic(struct perf_event *event)
{
	event->pending_disable = 1;
	irq_work_queue(&event->pending_disable_irq);
}

#define MAX_INTERRUPTS (~0ULL)

static void perf_log_throttle(struct perf_event *event, int enable);
static void perf_log_itrace_start(struct perf_event *event);

static int
event_sched_in(struct perf_event *event, struct perf_event_context *ctx)
{
	struct perf_event_pmu_context *epc = event->pmu_ctx;
	struct perf_cpu_pmu_context *cpc = this_cpc(epc->pmu);
	int ret = 0;

	WARN_ON_ONCE(event->ctx != ctx);

	lockdep_assert_held(&ctx->lock);

	if (event->state <= PERF_EVENT_STATE_OFF)
		return 0;

	WRITE_ONCE(event->oncpu, smp_processor_id());
	/*
	 * Order event::oncpu write to happen before the ACTIVE state is
	 * visible. This allows perf_event_{stop,read}() to observe the correct
	 * ->oncpu if it sees ACTIVE.
	 */
	smp_wmb();
	perf_event_set_state(event, PERF_EVENT_STATE_ACTIVE);

	/*
	 * Unthrottle events, since we scheduled we might have missed several
	 * ticks already, also for a heavily scheduling task there is little
	 * guarantee it'll get a tick in a timely manner.
	 */
	if (unlikely(event->hw.interrupts == MAX_INTERRUPTS)) {
		perf_log_throttle(event, 1);
		event->hw.interrupts = 0;
	}

	perf_pmu_disable(event->pmu);

	perf_log_itrace_start(event);

	if (event->pmu->add(event, PERF_EF_START)) {
		perf_event_set_state(event, PERF_EVENT_STATE_INACTIVE);
		event->oncpu = -1;
		ret = -EAGAIN;
		goto out;
	}

	if (!is_software_event(event))
		cpc->active_oncpu++;
	if (event->attr.freq && event->attr.sample_freq) {
		ctx->nr_freq++;
		epc->nr_freq++;
	}
	if (event->attr.exclusive)
		cpc->exclusive = 1;

out:
	perf_pmu_enable(event->pmu);

	return ret;
}

static int
group_sched_in(struct perf_event *group_event, struct perf_event_context *ctx)
{
	struct perf_event *event, *partial_group = NULL;
	struct pmu *pmu = group_event->pmu_ctx->pmu;

	if (group_event->state == PERF_EVENT_STATE_OFF)
		return 0;

	pmu->start_txn(pmu, PERF_PMU_TXN_ADD);

	if (event_sched_in(group_event, ctx))
		goto error;

	/*
	 * Schedule in siblings as one group (if any):
	 */
	for_each_sibling_event(event, group_event) {
		if (event_sched_in(event, ctx)) {
			partial_group = event;
			goto group_error;
		}
	}

	if (!pmu->commit_txn(pmu))
		return 0;

group_error:
	/*
	 * Groups can be scheduled in as one unit only, so undo any
	 * partial group before returning:
	 * The events up to the failed event are scheduled out normally.
	 */
	for_each_sibling_event(event, group_event) {
		if (event == partial_group)
			break;

		event_sched_out(event, ctx);
	}
	event_sched_out(group_event, ctx);

error:
	pmu->cancel_txn(pmu);
	return -EAGAIN;
}

/*
 * Work out whether we can put this event group on the CPU now.
 */
static int group_can_go_on(struct perf_event *event, int can_add_hw)
{
	struct perf_event_pmu_context *epc = event->pmu_ctx;
	struct perf_cpu_pmu_context *cpc = this_cpc(epc->pmu);

	/*
	 * Groups consisting entirely of software events can always go on.
	 */
	if (event->group_caps & PERF_EV_CAP_SOFTWARE)
		return 1;
	/*
	 * If an exclusive group is already on, no other hardware
	 * events can go on.
	 */
	if (cpc->exclusive)
		return 0;
	/*
	 * If this group is exclusive and there are already
	 * events on the CPU, it can't go on.
	 */
	if (event->attr.exclusive && !list_empty(get_event_list(event)))
		return 0;
	/*
	 * Otherwise, try to add it if all previous groups were able
	 * to go on.
	 */
	return can_add_hw;
}

static void add_event_to_ctx(struct perf_event *event,
			       struct perf_event_context *ctx)
{
	list_add_event(event, ctx);
	perf_group_attach(event);
}

static void task_ctx_sched_out(struct perf_event_context *ctx,
			       struct pmu *pmu,
			       enum event_type_t event_type)
{
	struct perf_cpu_context *cpuctx = this_cpu_ptr(&perf_cpu_context);

	if (!cpuctx->task_ctx)
		return;

	if (WARN_ON_ONCE(ctx != cpuctx->task_ctx))
		return;

	ctx_sched_out(ctx, pmu, event_type);
}

static void perf_event_sched_in(struct perf_cpu_context *cpuctx,
				struct perf_event_context *ctx,
				struct pmu *pmu)
{
	ctx_sched_in(&cpuctx->ctx, pmu, EVENT_PINNED);
	if (ctx)
		 ctx_sched_in(ctx, pmu, EVENT_PINNED);
	ctx_sched_in(&cpuctx->ctx, pmu, EVENT_FLEXIBLE);
	if (ctx)
		 ctx_sched_in(ctx, pmu, EVENT_FLEXIBLE);
}

/*
 * We want to maintain the following priority of scheduling:
 *  - CPU pinned (EVENT_CPU | EVENT_PINNED)
 *  - task pinned (EVENT_PINNED)
 *  - CPU flexible (EVENT_CPU | EVENT_FLEXIBLE)
 *  - task flexible (EVENT_FLEXIBLE).
 *
 * In order to avoid unscheduling and scheduling back in everything every
 * time an event is added, only do it for the groups of equal priority and
 * below.
 *
 * This can be called after a batch operation on task events, in which case
 * event_type is a bit mask of the types of events involved. For CPU events,
 * event_type is only either EVENT_PINNED or EVENT_FLEXIBLE.
 */
static void ctx_resched(struct perf_cpu_context *cpuctx,
			struct perf_event_context *task_ctx,
			struct pmu *pmu, enum event_type_t event_type)
{
	bool cpu_event = !!(event_type & EVENT_CPU);
	struct perf_event_pmu_context *epc;

	/*
	 * If pinned groups are involved, flexible groups also need to be
	 * scheduled out.
	 */
	if (event_type & EVENT_PINNED)
		event_type |= EVENT_FLEXIBLE;

	event_type &= EVENT_ALL;

	for_each_epc(epc, &cpuctx->ctx, pmu, false)
		perf_pmu_disable(epc->pmu);

	if (task_ctx) {
		for_each_epc(epc, task_ctx, pmu, false)
			perf_pmu_disable(epc->pmu);

		task_ctx_sched_out(task_ctx, pmu, event_type);
	}

	/*
	 * Decide which cpu ctx groups to schedule out based on the types
	 * of events that caused rescheduling:
	 *  - EVENT_CPU: schedule out corresponding groups;
	 *  - EVENT_PINNED task events: schedule out EVENT_FLEXIBLE groups;
	 *  - otherwise, do nothing more.
	 */
	if (cpu_event)
		ctx_sched_out(&cpuctx->ctx, pmu, event_type);
	else if (event_type & EVENT_PINNED)
		ctx_sched_out(&cpuctx->ctx, pmu, EVENT_FLEXIBLE);

	perf_event_sched_in(cpuctx, task_ctx, pmu);

	for_each_epc(epc, &cpuctx->ctx, pmu, false)
		perf_pmu_enable(epc->pmu);

	if (task_ctx) {
		for_each_epc(epc, task_ctx, pmu, false)
			perf_pmu_enable(epc->pmu);
	}
}

void perf_pmu_resched(struct pmu *pmu)
{
	struct perf_cpu_context *cpuctx = this_cpu_ptr(&perf_cpu_context);
	struct perf_event_context *task_ctx = cpuctx->task_ctx;

	perf_ctx_lock(cpuctx, task_ctx);
	ctx_resched(cpuctx, task_ctx, pmu, EVENT_ALL|EVENT_CPU);
	perf_ctx_unlock(cpuctx, task_ctx);
}

/*
 * Cross CPU call to install and enable a performance event
 *
 * Very similar to remote_function() + event_function() but cannot assume that
 * things like ctx->is_active and cpuctx->task_ctx are set.
 */
static int  __perf_install_in_context(void *info)
{
	struct perf_event *event = info;
	struct perf_event_context *ctx = event->ctx;
	struct perf_cpu_context *cpuctx = this_cpu_ptr(&perf_cpu_context);
	struct perf_event_context *task_ctx = cpuctx->task_ctx;
	bool reprogram = true;
	int ret = 0;

	raw_spin_lock(&cpuctx->ctx.lock);
	if (ctx->task) {
		raw_spin_lock(&ctx->lock);
		task_ctx = ctx;

		reprogram = (ctx->task == current);

		/*
		 * If the task is running, it must be running on this CPU,
		 * otherwise we cannot reprogram things.
		 *
		 * If its not running, we don't care, ctx->lock will
		 * serialize against it becoming runnable.
		 */
		if (task_curr(ctx->task) && !reprogram) {
			ret = -ESRCH;
			goto unlock;
		}

		WARN_ON_ONCE(reprogram && cpuctx->task_ctx && cpuctx->task_ctx != ctx);
	} else if (task_ctx) {
		raw_spin_lock(&task_ctx->lock);
	}

#ifdef CONFIG_CGROUP_PERF
	if (event->state > PERF_EVENT_STATE_OFF && is_cgroup_event(event)) {
		/*
		 * If the current cgroup doesn't match the event's
		 * cgroup, we should not try to schedule it.
		 */
		struct perf_cgroup *cgrp = perf_cgroup_from_task(current, ctx);
		reprogram = cgroup_is_descendant(cgrp->css.cgroup,
					event->cgrp->css.cgroup);
	}
#endif

	if (reprogram) {
		ctx_time_freeze(cpuctx, ctx);
		add_event_to_ctx(event, ctx);
		ctx_resched(cpuctx, task_ctx, event->pmu_ctx->pmu,
			    get_event_type(event));
	} else {
		add_event_to_ctx(event, ctx);
	}

unlock:
	perf_ctx_unlock(cpuctx, task_ctx);

	return ret;
}

static bool exclusive_event_installable(struct perf_event *event,
					struct perf_event_context *ctx);

/*
 * Attach a performance event to a context.
 *
 * Very similar to event_function_call, see comment there.
 */
static void
perf_install_in_context(struct perf_event_context *ctx,
			struct perf_event *event,
			int cpu)
{
	struct task_struct *task = READ_ONCE(ctx->task);

	lockdep_assert_held(&ctx->mutex);

	WARN_ON_ONCE(!exclusive_event_installable(event, ctx));

	if (event->cpu != -1)
		WARN_ON_ONCE(event->cpu != cpu);

	/*
	 * Ensures that if we can observe event->ctx, both the event and ctx
	 * will be 'complete'. See perf_iterate_sb_cpu().
	 */
	smp_store_release(&event->ctx, ctx);

	/*
	 * perf_event_attr::disabled events will not run and can be initialized
	 * without IPI. Except when this is the first event for the context, in
	 * that case we need the magic of the IPI to set ctx->is_active.
	 *
	 * The IOC_ENABLE that is sure to follow the creation of a disabled
	 * event will issue the IPI and reprogram the hardware.
	 */
	if (__perf_effective_state(event) == PERF_EVENT_STATE_OFF &&
	    ctx->nr_events && !is_cgroup_event(event)) {
		raw_spin_lock_irq(&ctx->lock);
		if (ctx->task == TASK_TOMBSTONE) {
			raw_spin_unlock_irq(&ctx->lock);
			return;
		}
		add_event_to_ctx(event, ctx);
		raw_spin_unlock_irq(&ctx->lock);
		return;
	}

	if (!task) {
		cpu_function_call(cpu, __perf_install_in_context, event);
		return;
	}

	/*
	 * Should not happen, we validate the ctx is still alive before calling.
	 */
	if (WARN_ON_ONCE(task == TASK_TOMBSTONE))
		return;

	/*
	 * Installing events is tricky because we cannot rely on ctx->is_active
	 * to be set in case this is the nr_events 0 -> 1 transition.
	 *
	 * Instead we use task_curr(), which tells us if the task is running.
	 * However, since we use task_curr() outside of rq::lock, we can race
	 * against the actual state. This means the result can be wrong.
	 *
	 * If we get a false positive, we retry, this is harmless.
	 *
	 * If we get a false negative, things are complicated. If we are after
	 * perf_event_context_sched_in() ctx::lock will serialize us, and the
	 * value must be correct. If we're before, it doesn't matter since
	 * perf_event_context_sched_in() will program the counter.
	 *
	 * However, this hinges on the remote context switch having observed
	 * our task->perf_event_ctxp[] store, such that it will in fact take
	 * ctx::lock in perf_event_context_sched_in().
	 *
	 * We do this by task_function_call(), if the IPI fails to hit the task
	 * we know any future context switch of task must see the
	 * perf_event_ctpx[] store.
	 */

	/*
	 * This smp_mb() orders the task->perf_event_ctxp[] store with the
	 * task_cpu() load, such that if the IPI then does not find the task
	 * running, a future context switch of that task must observe the
	 * store.
	 */
	smp_mb();
again:
	if (!task_function_call(task, __perf_install_in_context, event))
		return;

	raw_spin_lock_irq(&ctx->lock);
	task = ctx->task;
	if (WARN_ON_ONCE(task == TASK_TOMBSTONE)) {
		/*
		 * Cannot happen because we already checked above (which also
		 * cannot happen), and we hold ctx->mutex, which serializes us
		 * against perf_event_exit_task_context().
		 */
		raw_spin_unlock_irq(&ctx->lock);
		return;
	}
	/*
	 * If the task is not running, ctx->lock will avoid it becoming so,
	 * thus we can safely install the event.
	 */
	if (task_curr(task)) {
		raw_spin_unlock_irq(&ctx->lock);
		goto again;
	}
	add_event_to_ctx(event, ctx);
	raw_spin_unlock_irq(&ctx->lock);
}

/*
 * Cross CPU call to enable a performance event
 */
static void __perf_event_enable(struct perf_event *event,
				struct perf_cpu_context *cpuctx,
				struct perf_event_context *ctx,
				void *info)
{
	struct perf_event *leader = event->group_leader;
	struct perf_event_context *task_ctx;

	if (event->state >= PERF_EVENT_STATE_INACTIVE ||
	    event->state <= PERF_EVENT_STATE_ERROR)
		return;

	ctx_time_freeze(cpuctx, ctx);

	perf_event_set_state(event, PERF_EVENT_STATE_INACTIVE);
	perf_cgroup_event_enable(event, ctx);

	if (!ctx->is_active)
		return;

	if (!event_filter_match(event))
		return;

	/*
	 * If the event is in a group and isn't the group leader,
	 * then don't put it on unless the group is on.
	 */
	if (leader != event && leader->state != PERF_EVENT_STATE_ACTIVE)
		return;

	task_ctx = cpuctx->task_ctx;
	if (ctx->task)
		WARN_ON_ONCE(task_ctx != ctx);

	ctx_resched(cpuctx, task_ctx, event->pmu_ctx->pmu, get_event_type(event));
}

/*
 * Enable an event.
 *
 * If event->ctx is a cloned context, callers must make sure that
 * every task struct that event->ctx->task could possibly point to
 * remains valid.  This condition is satisfied when called through
 * perf_event_for_each_child or perf_event_for_each as described
 * for perf_event_disable.
 */
static void _perf_event_enable(struct perf_event *event)
{
	struct perf_event_context *ctx = event->ctx;

	raw_spin_lock_irq(&ctx->lock);
	if (event->state >= PERF_EVENT_STATE_INACTIVE ||
	    event->state <  PERF_EVENT_STATE_ERROR) {
out:
		raw_spin_unlock_irq(&ctx->lock);
		return;
	}

	/*
	 * If the event is in error state, clear that first.
	 *
	 * That way, if we see the event in error state below, we know that it
	 * has gone back into error state, as distinct from the task having
	 * been scheduled away before the cross-call arrived.
	 */
	if (event->state == PERF_EVENT_STATE_ERROR) {
		/*
		 * Detached SIBLING events cannot leave ERROR state.
		 */
		if (event->event_caps & PERF_EV_CAP_SIBLING &&
		    event->group_leader == event)
			goto out;

		event->state = PERF_EVENT_STATE_OFF;
	}
	raw_spin_unlock_irq(&ctx->lock);

	event_function_call(event, __perf_event_enable, NULL);
}

/*
 * See perf_event_disable();
 */
void perf_event_enable(struct perf_event *event)
{
	struct perf_event_context *ctx;

	ctx = perf_event_ctx_lock(event);
	_perf_event_enable(event);
	perf_event_ctx_unlock(event, ctx);
}
EXPORT_SYMBOL_GPL(perf_event_enable);

struct stop_event_data {
	struct perf_event	*event;
	unsigned int		restart;
};

static int __perf_event_stop(void *info)
{
	struct stop_event_data *sd = info;
	struct perf_event *event = sd->event;

	/* if it's already INACTIVE, do nothing */
	if (READ_ONCE(event->state) != PERF_EVENT_STATE_ACTIVE)
		return 0;

	/* matches smp_wmb() in event_sched_in() */
	smp_rmb();

	/*
	 * There is a window with interrupts enabled before we get here,
	 * so we need to check again lest we try to stop another CPU's event.
	 */
	if (READ_ONCE(event->oncpu) != smp_processor_id())
		return -EAGAIN;

	event->pmu->stop(event, PERF_EF_UPDATE);

	/*
	 * May race with the actual stop (through perf_pmu_output_stop()),
	 * but it is only used for events with AUX ring buffer, and such
	 * events will refuse to restart because of rb::aux_mmap_count==0,
	 * see comments in perf_aux_output_begin().
	 *
	 * Since this is happening on an event-local CPU, no trace is lost
	 * while restarting.
	 */
	if (sd->restart)
		event->pmu->start(event, 0);

	return 0;
}

static int perf_event_stop(struct perf_event *event, int restart)
{
	struct stop_event_data sd = {
		.event		= event,
		.restart	= restart,
	};
	int ret = 0;

	do {
		if (READ_ONCE(event->state) != PERF_EVENT_STATE_ACTIVE)
			return 0;

		/* matches smp_wmb() in event_sched_in() */
		smp_rmb();

		/*
		 * We only want to restart ACTIVE events, so if the event goes
		 * inactive here (event->oncpu==-1), there's nothing more to do;
		 * fall through with ret==-ENXIO.
		 */
		ret = cpu_function_call(READ_ONCE(event->oncpu),
					__perf_event_stop, &sd);
	} while (ret == -EAGAIN);

	return ret;
}

/*
 * In order to contain the amount of racy and tricky in the address filter
 * configuration management, it is a two part process:
 *
 * (p1) when userspace mappings change as a result of (1) or (2) or (3) below,
 *      we update the addresses of corresponding vmas in
 *	event::addr_filter_ranges array and bump the event::addr_filters_gen;
 * (p2) when an event is scheduled in (pmu::add), it calls
 *      perf_event_addr_filters_sync() which calls pmu::addr_filters_sync()
 *      if the generation has changed since the previous call.
 *
 * If (p1) happens while the event is active, we restart it to force (p2).
 *
 * (1) perf_addr_filters_apply(): adjusting filters' offsets based on
 *     pre-existing mappings, called once when new filters arrive via SET_FILTER
 *     ioctl;
 * (2) perf_addr_filters_adjust(): adjusting filters' offsets based on newly
 *     registered mapping, called for every new mmap(), with mm::mmap_lock down
 *     for reading;
 * (3) perf_event_addr_filters_exec(): clearing filters' offsets in the process
 *     of exec.
 */
void perf_event_addr_filters_sync(struct perf_event *event)
{
	struct perf_addr_filters_head *ifh = perf_event_addr_filters(event);

	if (!has_addr_filter(event))
		return;

	raw_spin_lock(&ifh->lock);
	if (event->addr_filters_gen != event->hw.addr_filters_gen) {
		event->pmu->addr_filters_sync(event);
		event->hw.addr_filters_gen = event->addr_filters_gen;
	}
	raw_spin_unlock(&ifh->lock);
}
EXPORT_SYMBOL_GPL(perf_event_addr_filters_sync);

static int _perf_event_refresh(struct perf_event *event, int refresh)
{
	/*
	 * not supported on inherited events
	 */
	if (event->attr.inherit || !is_sampling_event(event))
		return -EINVAL;

	atomic_add(refresh, &event->event_limit);
	_perf_event_enable(event);

	return 0;
}

/*
 * See perf_event_disable()
 */
int perf_event_refresh(struct perf_event *event, int refresh)
{
	struct perf_event_context *ctx;
	int ret;

	ctx = perf_event_ctx_lock(event);
	ret = _perf_event_refresh(event, refresh);
	perf_event_ctx_unlock(event, ctx);

	return ret;
}
EXPORT_SYMBOL_GPL(perf_event_refresh);

static int perf_event_modify_breakpoint(struct perf_event *bp,
					 struct perf_event_attr *attr)
{
	int err;

	_perf_event_disable(bp);

	err = modify_user_hw_breakpoint_check(bp, attr, true);

	if (!bp->attr.disabled)
		_perf_event_enable(bp);

	return err;
}

/*
 * Copy event-type-independent attributes that may be modified.
 */
static void perf_event_modify_copy_attr(struct perf_event_attr *to,
					const struct perf_event_attr *from)
{
	to->sig_data = from->sig_data;
}

static int perf_event_modify_attr(struct perf_event *event,
				  struct perf_event_attr *attr)
{
	int (*func)(struct perf_event *, struct perf_event_attr *);
	struct perf_event *child;
	int err;

	if (event->attr.type != attr->type)
		return -EINVAL;

	switch (event->attr.type) {
	case PERF_TYPE_BREAKPOINT:
		func = perf_event_modify_breakpoint;
		break;
	default:
		/* Place holder for future additions. */
		return -EOPNOTSUPP;
	}

	WARN_ON_ONCE(event->ctx->parent_ctx);

	mutex_lock(&event->child_mutex);
	/*
	 * Event-type-independent attributes must be copied before event-type
	 * modification, which will validate that final attributes match the
	 * source attributes after all relevant attributes have been copied.
	 */
	perf_event_modify_copy_attr(&event->attr, attr);
	err = func(event, attr);
	if (err)
		goto out;
	list_for_each_entry(child, &event->child_list, child_list) {
		perf_event_modify_copy_attr(&child->attr, attr);
		err = func(child, attr);
		if (err)
			goto out;
	}
out:
	mutex_unlock(&event->child_mutex);
	return err;
}

static void __pmu_ctx_sched_out(struct perf_event_pmu_context *pmu_ctx,
				enum event_type_t event_type)
{
	struct perf_event_context *ctx = pmu_ctx->ctx;
	struct perf_event *event, *tmp;
	struct pmu *pmu = pmu_ctx->pmu;

	if (ctx->task && !(ctx->is_active & EVENT_ALL)) {
		struct perf_cpu_pmu_context *cpc = this_cpc(pmu);

		WARN_ON_ONCE(cpc->task_epc && cpc->task_epc != pmu_ctx);
		cpc->task_epc = NULL;
	}

	if (!(event_type & EVENT_ALL))
		return;

	perf_pmu_disable(pmu);
	if (event_type & EVENT_PINNED) {
		list_for_each_entry_safe(event, tmp,
					 &pmu_ctx->pinned_active,
					 active_list)
			group_sched_out(event, ctx);
	}

	if (event_type & EVENT_FLEXIBLE) {
		list_for_each_entry_safe(event, tmp,
					 &pmu_ctx->flexible_active,
					 active_list)
			group_sched_out(event, ctx);
		/*
		 * Since we cleared EVENT_FLEXIBLE, also clear
		 * rotate_necessary, is will be reset by
		 * ctx_flexible_sched_in() when needed.
		 */
		pmu_ctx->rotate_necessary = 0;
	}
	perf_pmu_enable(pmu);
}

/*
 * Be very careful with the @pmu argument since this will change ctx state.
 * The @pmu argument works for ctx_resched(), because that is symmetric in
 * ctx_sched_out() / ctx_sched_in() usage and the ctx state ends up invariant.
 *
 * However, if you were to be asymmetrical, you could end up with messed up
 * state, eg. ctx->is_active cleared even though most EPCs would still actually
 * be active.
 */
static void
ctx_sched_out(struct perf_event_context *ctx, struct pmu *pmu, enum event_type_t event_type)
{
	struct perf_cpu_context *cpuctx = this_cpu_ptr(&perf_cpu_context);
	struct perf_event_pmu_context *pmu_ctx;
	int is_active = ctx->is_active;
	bool cgroup = event_type & EVENT_CGROUP;

	event_type &= ~EVENT_CGROUP;

	lockdep_assert_held(&ctx->lock);

	if (likely(!ctx->nr_events)) {
		/*
		 * See __perf_remove_from_context().
		 */
		WARN_ON_ONCE(ctx->is_active);
		if (ctx->task)
			WARN_ON_ONCE(cpuctx->task_ctx);
		return;
	}

	/*
	 * Always update time if it was set; not only when it changes.
	 * Otherwise we can 'forget' to update time for any but the last
	 * context we sched out. For example:
	 *
	 *   ctx_sched_out(.event_type = EVENT_FLEXIBLE)
	 *   ctx_sched_out(.event_type = EVENT_PINNED)
	 *
	 * would only update time for the pinned events.
	 */
	__ctx_time_update(cpuctx, ctx, ctx == &cpuctx->ctx);

	/*
	 * CPU-release for the below ->is_active store,
	 * see __load_acquire() in perf_event_time_now()
	 */
	barrier();
	ctx->is_active &= ~event_type;

	if (!(ctx->is_active & EVENT_ALL)) {
		/*
		 * For FROZEN, preserve TIME|FROZEN such that perf_event_time_now()
		 * does not observe a hole. perf_ctx_unlock() will clean up.
		 */
		if (ctx->is_active & EVENT_FROZEN)
			ctx->is_active &= EVENT_TIME_FROZEN;
		else
			ctx->is_active = 0;
	}

	if (ctx->task) {
		WARN_ON_ONCE(cpuctx->task_ctx != ctx);
		if (!(ctx->is_active & EVENT_ALL))
			cpuctx->task_ctx = NULL;
	}

	is_active ^= ctx->is_active; /* changed bits */

	for_each_epc(pmu_ctx, ctx, pmu, cgroup)
		__pmu_ctx_sched_out(pmu_ctx, is_active);
}

/*
 * Test whether two contexts are equivalent, i.e. whether they have both been
 * cloned from the same version of the same context.
 *
 * Equivalence is measured using a generation number in the context that is
 * incremented on each modification to it; see unclone_ctx(), list_add_event()
 * and list_del_event().
 */
static int context_equiv(struct perf_event_context *ctx1,
			 struct perf_event_context *ctx2)
{
	lockdep_assert_held(&ctx1->lock);
	lockdep_assert_held(&ctx2->lock);

	/* Pinning disables the swap optimization */
	if (ctx1->pin_count || ctx2->pin_count)
		return 0;

	/* If ctx1 is the parent of ctx2 */
	if (ctx1 == ctx2->parent_ctx && ctx1->generation == ctx2->parent_gen)
		return 1;

	/* If ctx2 is the parent of ctx1 */
	if (ctx1->parent_ctx == ctx2 && ctx1->parent_gen == ctx2->generation)
		return 1;

	/*
	 * If ctx1 and ctx2 have the same parent; we flatten the parent
	 * hierarchy, see perf_event_init_context().
	 */
	if (ctx1->parent_ctx && ctx1->parent_ctx == ctx2->parent_ctx &&
			ctx1->parent_gen == ctx2->parent_gen)
		return 1;

	/* Unmatched */
	return 0;
}

static void __perf_event_sync_stat(struct perf_event *event,
				     struct perf_event *next_event)
{
	u64 value;

	if (!event->attr.inherit_stat)
		return;

	/*
	 * Update the event value, we cannot use perf_event_read()
	 * because we're in the middle of a context switch and have IRQs
	 * disabled, which upsets smp_call_function_single(), however
	 * we know the event must be on the current CPU, therefore we
	 * don't need to use it.
	 */
	perf_pmu_read(event);

	perf_event_update_time(event);

	/*
	 * In order to keep per-task stats reliable we need to flip the event
	 * values when we flip the contexts.
	 */
	value = local64_read(&next_event->count);
	value = local64_xchg(&event->count, value);
	local64_set(&next_event->count, value);

	swap(event->total_time_enabled, next_event->total_time_enabled);
	swap(event->total_time_running, next_event->total_time_running);

	/*
	 * Since we swizzled the values, update the user visible data too.
	 */
	perf_event_update_userpage(event);
	perf_event_update_userpage(next_event);
}

static void perf_event_sync_stat(struct perf_event_context *ctx,
				   struct perf_event_context *next_ctx)
{
	struct perf_event *event, *next_event;

	if (!ctx->nr_stat)
		return;

	update_context_time(ctx);

	event = list_first_entry(&ctx->event_list,
				   struct perf_event, event_entry);

	next_event = list_first_entry(&next_ctx->event_list,
					struct perf_event, event_entry);

	while (&event->event_entry != &ctx->event_list &&
	       &next_event->event_entry != &next_ctx->event_list) {

		__perf_event_sync_stat(event, next_event);

		event = list_next_entry(event, event_entry);
		next_event = list_next_entry(next_event, event_entry);
	}
}

static void perf_ctx_sched_task_cb(struct perf_event_context *ctx,
				   struct task_struct *task, bool sched_in)
{
	struct perf_event_pmu_context *pmu_ctx;
	struct perf_cpu_pmu_context *cpc;

	list_for_each_entry(pmu_ctx, &ctx->pmu_ctx_list, pmu_ctx_entry) {
		cpc = this_cpc(pmu_ctx->pmu);

		if (cpc->sched_cb_usage && pmu_ctx->pmu->sched_task)
			pmu_ctx->pmu->sched_task(pmu_ctx, task, sched_in);
	}
}

static void
perf_event_context_sched_out(struct task_struct *task, struct task_struct *next)
{
	struct perf_event_context *ctx = task->perf_event_ctxp;
	struct perf_event_context *next_ctx;
	struct perf_event_context *parent, *next_parent;
	int do_switch = 1;

	if (likely(!ctx))
		return;

	rcu_read_lock();
	next_ctx = rcu_dereference(next->perf_event_ctxp);
	if (!next_ctx)
		goto unlock;

	parent = rcu_dereference(ctx->parent_ctx);
	next_parent = rcu_dereference(next_ctx->parent_ctx);

	/* If neither context have a parent context; they cannot be clones. */
	if (!parent && !next_parent)
		goto unlock;

	if (next_parent == ctx || next_ctx == parent || next_parent == parent) {
		/*
		 * Looks like the two contexts are clones, so we might be
		 * able to optimize the context switch.  We lock both
		 * contexts and check that they are clones under the
		 * lock (including re-checking that neither has been
		 * uncloned in the meantime).  It doesn't matter which
		 * order we take the locks because no other cpu could
		 * be trying to lock both of these tasks.
		 */
		raw_spin_lock(&ctx->lock);
		raw_spin_lock_nested(&next_ctx->lock, SINGLE_DEPTH_NESTING);
		if (context_equiv(ctx, next_ctx)) {

			perf_ctx_disable(ctx, false);

			/* PMIs are disabled; ctx->nr_no_switch_fast is stable. */
			if (local_read(&ctx->nr_no_switch_fast) ||
			    local_read(&next_ctx->nr_no_switch_fast)) {
				/*
				 * Must not swap out ctx when there's pending
				 * events that rely on the ctx->task relation.
				 *
				 * Likewise, when a context contains inherit +
				 * SAMPLE_READ events they should be switched
				 * out using the slow path so that they are
				 * treated as if they were distinct contexts.
				 */
				raw_spin_unlock(&next_ctx->lock);
				rcu_read_unlock();
				goto inside_switch;
			}

			WRITE_ONCE(ctx->task, next);
			WRITE_ONCE(next_ctx->task, task);

			perf_ctx_sched_task_cb(ctx, task, false);

			perf_ctx_enable(ctx, false);

			/*
			 * RCU_INIT_POINTER here is safe because we've not
			 * modified the ctx and the above modification of
			 * ctx->task is immaterial since this value is
			 * always verified under ctx->lock which we're now
			 * holding.
			 */
			RCU_INIT_POINTER(task->perf_event_ctxp, next_ctx);
			RCU_INIT_POINTER(next->perf_event_ctxp, ctx);

			do_switch = 0;

			perf_event_sync_stat(ctx, next_ctx);
		}
		raw_spin_unlock(&next_ctx->lock);
		raw_spin_unlock(&ctx->lock);
	}
unlock:
	rcu_read_unlock();

	if (do_switch) {
		raw_spin_lock(&ctx->lock);
		perf_ctx_disable(ctx, false);

inside_switch:
		perf_ctx_sched_task_cb(ctx, task, false);
		task_ctx_sched_out(ctx, NULL, EVENT_ALL);

		perf_ctx_enable(ctx, false);
		raw_spin_unlock(&ctx->lock);
	}
}

static DEFINE_PER_CPU(struct list_head, sched_cb_list);
static DEFINE_PER_CPU(int, perf_sched_cb_usages);

void perf_sched_cb_dec(struct pmu *pmu)
{
	struct perf_cpu_pmu_context *cpc = this_cpc(pmu);

	this_cpu_dec(perf_sched_cb_usages);
	barrier();

	if (!--cpc->sched_cb_usage)
		list_del(&cpc->sched_cb_entry);
}


void perf_sched_cb_inc(struct pmu *pmu)
{
	struct perf_cpu_pmu_context *cpc = this_cpc(pmu);

	if (!cpc->sched_cb_usage++)
		list_add(&cpc->sched_cb_entry, this_cpu_ptr(&sched_cb_list));

	barrier();
	this_cpu_inc(perf_sched_cb_usages);
}

/*
 * This function provides the context switch callback to the lower code
 * layer. It is invoked ONLY when the context switch callback is enabled.
 *
 * This callback is relevant even to per-cpu events; for example multi event
 * PEBS requires this to provide PID/TID information. This requires we flush
 * all queued PEBS records before we context switch to a new task.
 */
static void __perf_pmu_sched_task(struct perf_cpu_pmu_context *cpc,
				  struct task_struct *task, bool sched_in)
{
	struct perf_cpu_context *cpuctx = this_cpu_ptr(&perf_cpu_context);
	struct pmu *pmu;

	pmu = cpc->epc.pmu;

	/* software PMUs will not have sched_task */
	if (WARN_ON_ONCE(!pmu->sched_task))
		return;

	perf_ctx_lock(cpuctx, cpuctx->task_ctx);
	perf_pmu_disable(pmu);

	pmu->sched_task(cpc->task_epc, task, sched_in);

	perf_pmu_enable(pmu);
	perf_ctx_unlock(cpuctx, cpuctx->task_ctx);
}

static void perf_pmu_sched_task(struct task_struct *prev,
				struct task_struct *next,
				bool sched_in)
{
	struct perf_cpu_context *cpuctx = this_cpu_ptr(&perf_cpu_context);
	struct perf_cpu_pmu_context *cpc;

	/* cpuctx->task_ctx will be handled in perf_event_context_sched_in/out */
	if (prev == next || cpuctx->task_ctx)
		return;

	list_for_each_entry(cpc, this_cpu_ptr(&sched_cb_list), sched_cb_entry)
		__perf_pmu_sched_task(cpc, sched_in ? next : prev, sched_in);
}

static void perf_event_switch(struct task_struct *task,
			      struct task_struct *next_prev, bool sched_in);

/*
 * Called from scheduler to remove the events of the current task,
 * with interrupts disabled.
 *
 * We stop each event and update the event value in event->count.
 *
 * This does not protect us against NMI, but disable()
 * sets the disabled bit in the control field of event _before_
 * accessing the event control register. If a NMI hits, then it will
 * not restart the event.
 */
void __perf_event_task_sched_out(struct task_struct *task,
				 struct task_struct *next)
{
	if (__this_cpu_read(perf_sched_cb_usages))
		perf_pmu_sched_task(task, next, false);

	if (atomic_read(&nr_switch_events))
		perf_event_switch(task, next, false);

	perf_event_context_sched_out(task, next);

	/*
	 * if cgroup events exist on this CPU, then we need
	 * to check if we have to switch out PMU state.
	 * cgroup event are system-wide mode only
	 */
	perf_cgroup_switch(next);
}

static bool perf_less_group_idx(const void *l, const void *r, void __always_unused *args)
{
	const struct perf_event *le = *(const struct perf_event **)l;
	const struct perf_event *re = *(const struct perf_event **)r;

	return le->group_index < re->group_index;
}

DEFINE_MIN_HEAP(struct perf_event *, perf_event_min_heap);

static const struct min_heap_callbacks perf_min_heap = {
	.less = perf_less_group_idx,
	.swp = NULL,
};

static void __heap_add(struct perf_event_min_heap *heap, struct perf_event *event)
{
	struct perf_event **itrs = heap->data;

	if (event) {
		itrs[heap->nr] = event;
		heap->nr++;
	}
}

static void __link_epc(struct perf_event_pmu_context *pmu_ctx)
{
	struct perf_cpu_pmu_context *cpc;

	if (!pmu_ctx->ctx->task)
		return;

	cpc = this_cpc(pmu_ctx->pmu);
	WARN_ON_ONCE(cpc->task_epc && cpc->task_epc != pmu_ctx);
	cpc->task_epc = pmu_ctx;
}

static noinline int visit_groups_merge(struct perf_event_context *ctx,
				struct perf_event_groups *groups, int cpu,
				struct pmu *pmu,
				int (*func)(struct perf_event *, void *),
				void *data)
{
#ifdef CONFIG_CGROUP_PERF
	struct cgroup_subsys_state *css = NULL;
#endif
	struct perf_cpu_context *cpuctx = NULL;
	/* Space for per CPU and/or any CPU event iterators. */
	struct perf_event *itrs[2];
	struct perf_event_min_heap event_heap;
	struct perf_event **evt;
	int ret;

	if (pmu->filter && pmu->filter(pmu, cpu))
		return 0;

	if (!ctx->task) {
		cpuctx = this_cpu_ptr(&perf_cpu_context);
		event_heap = (struct perf_event_min_heap){
			.data = cpuctx->heap,
			.nr = 0,
			.size = cpuctx->heap_size,
		};

		lockdep_assert_held(&cpuctx->ctx.lock);

#ifdef CONFIG_CGROUP_PERF
		if (cpuctx->cgrp)
			css = &cpuctx->cgrp->css;
#endif
	} else {
		event_heap = (struct perf_event_min_heap){
			.data = itrs,
			.nr = 0,
			.size = ARRAY_SIZE(itrs),
		};
		/* Events not within a CPU context may be on any CPU. */
		__heap_add(&event_heap, perf_event_groups_first(groups, -1, pmu, NULL));
	}
	evt = event_heap.data;

	__heap_add(&event_heap, perf_event_groups_first(groups, cpu, pmu, NULL));

#ifdef CONFIG_CGROUP_PERF
	for (; css; css = css->parent)
		__heap_add(&event_heap, perf_event_groups_first(groups, cpu, pmu, css->cgroup));
#endif

	if (event_heap.nr) {
		__link_epc((*evt)->pmu_ctx);
		perf_assert_pmu_disabled((*evt)->pmu_ctx->pmu);
	}

	min_heapify_all_inline(&event_heap, &perf_min_heap, NULL);

	while (event_heap.nr) {
		ret = func(*evt, data);
		if (ret)
			return ret;

		*evt = perf_event_groups_next(*evt, pmu);
		if (*evt)
			min_heap_sift_down_inline(&event_heap, 0, &perf_min_heap, NULL);
		else
			min_heap_pop_inline(&event_heap, &perf_min_heap, NULL);
	}

	return 0;
}

/*
 * Because the userpage is strictly per-event (there is no concept of context,
 * so there cannot be a context indirection), every userpage must be updated
 * when context time starts :-(
 *
 * IOW, we must not miss EVENT_TIME edges.
 */
static inline bool event_update_userpage(struct perf_event *event)
{
	if (likely(!atomic_read(&event->mmap_count)))
		return false;

	perf_event_update_time(event);
	perf_event_update_userpage(event);

	return true;
}

static inline void group_update_userpage(struct perf_event *group_event)
{
	struct perf_event *event;

	if (!event_update_userpage(group_event))
		return;

	for_each_sibling_event(event, group_event)
		event_update_userpage(event);
}

static int merge_sched_in(struct perf_event *event, void *data)
{
	struct perf_event_context *ctx = event->ctx;
	int *can_add_hw = data;

	if (event->state <= PERF_EVENT_STATE_OFF)
		return 0;

	if (!event_filter_match(event))
		return 0;

	if (group_can_go_on(event, *can_add_hw)) {
		if (!group_sched_in(event, ctx))
			list_add_tail(&event->active_list, get_event_list(event));
	}

	if (event->state == PERF_EVENT_STATE_INACTIVE) {
		*can_add_hw = 0;
		if (event->attr.pinned) {
			perf_cgroup_event_disable(event, ctx);
			perf_event_set_state(event, PERF_EVENT_STATE_ERROR);

			if (*perf_event_fasync(event))
				event->pending_kill = POLL_ERR;

			perf_event_wakeup(event);
		} else {
			struct perf_cpu_pmu_context *cpc = this_cpc(event->pmu_ctx->pmu);

			event->pmu_ctx->rotate_necessary = 1;
			perf_mux_hrtimer_restart(cpc);
			group_update_userpage(event);
		}
	}

	return 0;
}

static void pmu_groups_sched_in(struct perf_event_context *ctx,
				struct perf_event_groups *groups,
				struct pmu *pmu)
{
	int can_add_hw = 1;
	visit_groups_merge(ctx, groups, smp_processor_id(), pmu,
			   merge_sched_in, &can_add_hw);
}

static void __pmu_ctx_sched_in(struct perf_event_pmu_context *pmu_ctx,
			       enum event_type_t event_type)
{
	struct perf_event_context *ctx = pmu_ctx->ctx;

	if (event_type & EVENT_PINNED)
		pmu_groups_sched_in(ctx, &ctx->pinned_groups, pmu_ctx->pmu);
	if (event_type & EVENT_FLEXIBLE)
		pmu_groups_sched_in(ctx, &ctx->flexible_groups, pmu_ctx->pmu);
}

static void
ctx_sched_in(struct perf_event_context *ctx, struct pmu *pmu, enum event_type_t event_type)
{
	struct perf_cpu_context *cpuctx = this_cpu_ptr(&perf_cpu_context);
	struct perf_event_pmu_context *pmu_ctx;
	int is_active = ctx->is_active;
	bool cgroup = event_type & EVENT_CGROUP;

	event_type &= ~EVENT_CGROUP;

	lockdep_assert_held(&ctx->lock);

	if (likely(!ctx->nr_events))
		return;

	if (!(is_active & EVENT_TIME)) {
		/* start ctx time */
		__update_context_time(ctx, false);
		perf_cgroup_set_timestamp(cpuctx);
		/*
		 * CPU-release for the below ->is_active store,
		 * see __load_acquire() in perf_event_time_now()
		 */
		barrier();
	}

	ctx->is_active |= (event_type | EVENT_TIME);
	if (ctx->task) {
		if (!(is_active & EVENT_ALL))
			cpuctx->task_ctx = ctx;
		else
			WARN_ON_ONCE(cpuctx->task_ctx != ctx);
	}

	is_active ^= ctx->is_active; /* changed bits */

	/*
	 * First go through the list and put on any pinned groups
	 * in order to give them the best chance of going on.
	 */
	if (is_active & EVENT_PINNED) {
		for_each_epc(pmu_ctx, ctx, pmu, cgroup)
			__pmu_ctx_sched_in(pmu_ctx, EVENT_PINNED);
	}

	/* Then walk through the lower prio flexible groups */
	if (is_active & EVENT_FLEXIBLE) {
		for_each_epc(pmu_ctx, ctx, pmu, cgroup)
			__pmu_ctx_sched_in(pmu_ctx, EVENT_FLEXIBLE);
	}
}

static void perf_event_context_sched_in(struct task_struct *task)
{
	struct perf_cpu_context *cpuctx = this_cpu_ptr(&perf_cpu_context);
	struct perf_event_context *ctx;

	rcu_read_lock();
	ctx = rcu_dereference(task->perf_event_ctxp);
	if (!ctx)
		goto rcu_unlock;

	if (cpuctx->task_ctx == ctx) {
		perf_ctx_lock(cpuctx, ctx);
		perf_ctx_disable(ctx, false);

		perf_ctx_sched_task_cb(ctx, task, true);

		perf_ctx_enable(ctx, false);
		perf_ctx_unlock(cpuctx, ctx);
		goto rcu_unlock;
	}

	perf_ctx_lock(cpuctx, ctx);
	/*
	 * We must check ctx->nr_events while holding ctx->lock, such
	 * that we serialize against perf_install_in_context().
	 */
	if (!ctx->nr_events)
		goto unlock;

	perf_ctx_disable(ctx, false);
	/*
	 * We want to keep the following priority order:
	 * cpu pinned (that don't need to move), task pinned,
	 * cpu flexible, task flexible.
	 *
	 * However, if task's ctx is not carrying any pinned
	 * events, no need to flip the cpuctx's events around.
	 */
	if (!RB_EMPTY_ROOT(&ctx->pinned_groups.tree)) {
		perf_ctx_disable(&cpuctx->ctx, false);
		ctx_sched_out(&cpuctx->ctx, NULL, EVENT_FLEXIBLE);
	}

	perf_event_sched_in(cpuctx, ctx, NULL);

	perf_ctx_sched_task_cb(cpuctx->task_ctx, task, true);

	if (!RB_EMPTY_ROOT(&ctx->pinned_groups.tree))
		perf_ctx_enable(&cpuctx->ctx, false);

	perf_ctx_enable(ctx, false);

unlock:
	perf_ctx_unlock(cpuctx, ctx);
rcu_unlock:
	rcu_read_unlock();
}

/*
 * Called from scheduler to add the events of the current task
 * with interrupts disabled.
 *
 * We restore the event value and then enable it.
 *
 * This does not protect us against NMI, but enable()
 * sets the enabled bit in the control field of event _before_
 * accessing the event control register. If a NMI hits, then it will
 * keep the event running.
 */
void __perf_event_task_sched_in(struct task_struct *prev,
				struct task_struct *task)
{
	perf_event_context_sched_in(task);

	if (atomic_read(&nr_switch_events))
		perf_event_switch(task, prev, true);

	if (__this_cpu_read(perf_sched_cb_usages))
		perf_pmu_sched_task(prev, task, true);
}

static u64 perf_calculate_period(struct perf_event *event, u64 nsec, u64 count)
{
	u64 frequency = event->attr.sample_freq;
	u64 sec = NSEC_PER_SEC;
	u64 divisor, dividend;

	int count_fls, nsec_fls, frequency_fls, sec_fls;

	count_fls = fls64(count);
	nsec_fls = fls64(nsec);
	frequency_fls = fls64(frequency);
	sec_fls = 30;

	/*
	 * We got @count in @nsec, with a target of sample_freq HZ
	 * the target period becomes:
	 *
	 *             @count * 10^9
	 * period = -------------------
	 *          @nsec * sample_freq
	 *
	 */

	/*
	 * Reduce accuracy by one bit such that @a and @b converge
	 * to a similar magnitude.
	 */
#define REDUCE_FLS(a, b)		\
do {					\
	if (a##_fls > b##_fls) {	\
		a >>= 1;		\
		a##_fls--;		\
	} else {			\
		b >>= 1;		\
		b##_fls--;		\
	}				\
} while (0)

	/*
	 * Reduce accuracy until either term fits in a u64, then proceed with
	 * the other, so that finally we can do a u64/u64 division.
	 */
	while (count_fls + sec_fls > 64 && nsec_fls + frequency_fls > 64) {
		REDUCE_FLS(nsec, frequency);
		REDUCE_FLS(sec, count);
	}

	if (count_fls + sec_fls > 64) {
		divisor = nsec * frequency;

		while (count_fls + sec_fls > 64) {
			REDUCE_FLS(count, sec);
			divisor >>= 1;
		}

		dividend = count * sec;
	} else {
		dividend = count * sec;

		while (nsec_fls + frequency_fls > 64) {
			REDUCE_FLS(nsec, frequency);
			dividend >>= 1;
		}

		divisor = nsec * frequency;
	}

	if (!divisor)
		return dividend;

	return div64_u64(dividend, divisor);
}

static DEFINE_PER_CPU(int, perf_throttled_count);
static DEFINE_PER_CPU(u64, perf_throttled_seq);

static void perf_adjust_period(struct perf_event *event, u64 nsec, u64 count, bool disable)
{
	struct hw_perf_event *hwc = &event->hw;
	s64 period, sample_period;
	s64 delta;

	period = perf_calculate_period(event, nsec, count);

	delta = (s64)(period - hwc->sample_period);
	if (delta >= 0)
		delta += 7;
	else
		delta -= 7;
	delta /= 8; /* low pass filter */

	sample_period = hwc->sample_period + delta;

	if (!sample_period)
		sample_period = 1;

	hwc->sample_period = sample_period;

	if (local64_read(&hwc->period_left) > 8*sample_period) {
		if (disable)
			event->pmu->stop(event, PERF_EF_UPDATE);

		local64_set(&hwc->period_left, 0);

		if (disable)
			event->pmu->start(event, PERF_EF_RELOAD);
	}
}

static void perf_adjust_freq_unthr_events(struct list_head *event_list)
{
	struct perf_event *event;
	struct hw_perf_event *hwc;
	u64 now, period = TICK_NSEC;
	s64 delta;

	list_for_each_entry(event, event_list, active_list) {
		if (event->state != PERF_EVENT_STATE_ACTIVE)
			continue;

		// XXX use visit thingy to avoid the -1,cpu match
		if (!event_filter_match(event))
			continue;

		hwc = &event->hw;

		if (hwc->interrupts == MAX_INTERRUPTS) {
			hwc->interrupts = 0;
			perf_log_throttle(event, 1);
			if (!event->attr.freq || !event->attr.sample_freq)
				event->pmu->start(event, 0);
		}

		if (!event->attr.freq || !event->attr.sample_freq)
			continue;

		/*
		 * stop the event and update event->count
		 */
		event->pmu->stop(event, PERF_EF_UPDATE);

		now = local64_read(&event->count);
		delta = now - hwc->freq_count_stamp;
		hwc->freq_count_stamp = now;

		/*
		 * restart the event
		 * reload only if value has changed
		 * we have stopped the event so tell that
		 * to perf_adjust_period() to avoid stopping it
		 * twice.
		 */
		if (delta > 0)
			perf_adjust_period(event, period, delta, false);

		event->pmu->start(event, delta > 0 ? PERF_EF_RELOAD : 0);
	}
}

/*
 * combine freq adjustment with unthrottling to avoid two passes over the
 * events. At the same time, make sure, having freq events does not change
 * the rate of unthrottling as that would introduce bias.
 */
static void
perf_adjust_freq_unthr_context(struct perf_event_context *ctx, bool unthrottle)
{
	struct perf_event_pmu_context *pmu_ctx;

	/*
	 * only need to iterate over all events iff:
	 * - context have events in frequency mode (needs freq adjust)
	 * - there are events to unthrottle on this cpu
	 */
	if (!(ctx->nr_freq || unthrottle))
		return;

	raw_spin_lock(&ctx->lock);

	list_for_each_entry(pmu_ctx, &ctx->pmu_ctx_list, pmu_ctx_entry) {
		if (!(pmu_ctx->nr_freq || unthrottle))
			continue;
		if (!perf_pmu_ctx_is_active(pmu_ctx))
			continue;
		if (pmu_ctx->pmu->capabilities & PERF_PMU_CAP_NO_INTERRUPT)
			continue;

		perf_pmu_disable(pmu_ctx->pmu);
		perf_adjust_freq_unthr_events(&pmu_ctx->pinned_active);
		perf_adjust_freq_unthr_events(&pmu_ctx->flexible_active);
		perf_pmu_enable(pmu_ctx->pmu);
	}

	raw_spin_unlock(&ctx->lock);
}

/*
 * Move @event to the tail of the @ctx's elegible events.
 */
static void rotate_ctx(struct perf_event_context *ctx, struct perf_event *event)
{
	/*
	 * Rotate the first entry last of non-pinned groups. Rotation might be
	 * disabled by the inheritance code.
	 */
	if (ctx->rotate_disable)
		return;

	perf_event_groups_delete(&ctx->flexible_groups, event);
	perf_event_groups_insert(&ctx->flexible_groups, event);
}

/* pick an event from the flexible_groups to rotate */
static inline struct perf_event *
ctx_event_to_rotate(struct perf_event_pmu_context *pmu_ctx)
{
	struct perf_event *event;
	struct rb_node *node;
	struct rb_root *tree;
	struct __group_key key = {
		.pmu = pmu_ctx->pmu,
	};

	/* pick the first active flexible event */
	event = list_first_entry_or_null(&pmu_ctx->flexible_active,
					 struct perf_event, active_list);
	if (event)
		goto out;

	/* if no active flexible event, pick the first event */
	tree = &pmu_ctx->ctx->flexible_groups.tree;

	if (!pmu_ctx->ctx->task) {
		key.cpu = smp_processor_id();

		node = rb_find_first(&key, tree, __group_cmp_ignore_cgroup);
		if (node)
			event = __node_2_pe(node);
		goto out;
	}

	key.cpu = -1;
	node = rb_find_first(&key, tree, __group_cmp_ignore_cgroup);
	if (node) {
		event = __node_2_pe(node);
		goto out;
	}

	key.cpu = smp_processor_id();
	node = rb_find_first(&key, tree, __group_cmp_ignore_cgroup);
	if (node)
		event = __node_2_pe(node);

out:
	/*
	 * Unconditionally clear rotate_necessary; if ctx_flexible_sched_in()
	 * finds there are unschedulable events, it will set it again.
	 */
	pmu_ctx->rotate_necessary = 0;

	return event;
}

static bool perf_rotate_context(struct perf_cpu_pmu_context *cpc)
{
	struct perf_cpu_context *cpuctx = this_cpu_ptr(&perf_cpu_context);
	struct perf_event_pmu_context *cpu_epc, *task_epc = NULL;
	struct perf_event *cpu_event = NULL, *task_event = NULL;
	int cpu_rotate, task_rotate;
	struct pmu *pmu;

	/*
	 * Since we run this from IRQ context, nobody can install new
	 * events, thus the event count values are stable.
	 */

	cpu_epc = &cpc->epc;
	pmu = cpu_epc->pmu;
	task_epc = cpc->task_epc;

	cpu_rotate = cpu_epc->rotate_necessary;
	task_rotate = task_epc ? task_epc->rotate_necessary : 0;

	if (!(cpu_rotate || task_rotate))
		return false;

	perf_ctx_lock(cpuctx, cpuctx->task_ctx);
	perf_pmu_disable(pmu);

	if (task_rotate)
		task_event = ctx_event_to_rotate(task_epc);
	if (cpu_rotate)
		cpu_event = ctx_event_to_rotate(cpu_epc);

	/*
	 * As per the order given at ctx_resched() first 'pop' task flexible
	 * and then, if needed CPU flexible.
	 */
	if (task_event || (task_epc && cpu_event)) {
		update_context_time(task_epc->ctx);
		__pmu_ctx_sched_out(task_epc, EVENT_FLEXIBLE);
	}

	if (cpu_event) {
		update_context_time(&cpuctx->ctx);
		__pmu_ctx_sched_out(cpu_epc, EVENT_FLEXIBLE);
		rotate_ctx(&cpuctx->ctx, cpu_event);
		__pmu_ctx_sched_in(cpu_epc, EVENT_FLEXIBLE);
	}

	if (task_event)
		rotate_ctx(task_epc->ctx, task_event);

	if (task_event || (task_epc && cpu_event))
		__pmu_ctx_sched_in(task_epc, EVENT_FLEXIBLE);

	perf_pmu_enable(pmu);
	perf_ctx_unlock(cpuctx, cpuctx->task_ctx);

	return true;
}

void perf_event_task_tick(void)
{
	struct perf_cpu_context *cpuctx = this_cpu_ptr(&perf_cpu_context);
	struct perf_event_context *ctx;
	int throttled;

	lockdep_assert_irqs_disabled();

	__this_cpu_inc(perf_throttled_seq);
	throttled = __this_cpu_xchg(perf_throttled_count, 0);
	tick_dep_clear_cpu(smp_processor_id(), TICK_DEP_BIT_PERF_EVENTS);

	perf_adjust_freq_unthr_context(&cpuctx->ctx, !!throttled);

	rcu_read_lock();
	ctx = rcu_dereference(current->perf_event_ctxp);
	if (ctx)
		perf_adjust_freq_unthr_context(ctx, !!throttled);
	rcu_read_unlock();
}

static int event_enable_on_exec(struct perf_event *event,
				struct perf_event_context *ctx)
{
	if (!event->attr.enable_on_exec)
		return 0;

	event->attr.enable_on_exec = 0;
	if (event->state >= PERF_EVENT_STATE_INACTIVE)
		return 0;

	perf_event_set_state(event, PERF_EVENT_STATE_INACTIVE);

	return 1;
}

/*
 * Enable all of a task's events that have been marked enable-on-exec.
 * This expects task == current.
 */
static void perf_event_enable_on_exec(struct perf_event_context *ctx)
{
	struct perf_event_context *clone_ctx = NULL;
	enum event_type_t event_type = 0;
	struct perf_cpu_context *cpuctx;
	struct perf_event *event;
	unsigned long flags;
	int enabled = 0;

	local_irq_save(flags);
	if (WARN_ON_ONCE(current->perf_event_ctxp != ctx))
		goto out;

	if (!ctx->nr_events)
		goto out;

	cpuctx = this_cpu_ptr(&perf_cpu_context);
	perf_ctx_lock(cpuctx, ctx);
	ctx_time_freeze(cpuctx, ctx);

	list_for_each_entry(event, &ctx->event_list, event_entry) {
		enabled |= event_enable_on_exec(event, ctx);
		event_type |= get_event_type(event);
	}

	/*
	 * Unclone and reschedule this context if we enabled any event.
	 */
	if (enabled) {
		clone_ctx = unclone_ctx(ctx);
		ctx_resched(cpuctx, ctx, NULL, event_type);
	}
	perf_ctx_unlock(cpuctx, ctx);

out:
	local_irq_restore(flags);

	if (clone_ctx)
		put_ctx(clone_ctx);
}

static void perf_remove_from_owner(struct perf_event *event);
static void perf_event_exit_event(struct perf_event *event,
				  struct perf_event_context *ctx);

/*
 * Removes all events from the current task that have been marked
 * remove-on-exec, and feeds their values back to parent events.
 */
static void perf_event_remove_on_exec(struct perf_event_context *ctx)
{
	struct perf_event_context *clone_ctx = NULL;
	struct perf_event *event, *next;
	unsigned long flags;
	bool modified = false;

	mutex_lock(&ctx->mutex);

	if (WARN_ON_ONCE(ctx->task != current))
		goto unlock;

	list_for_each_entry_safe(event, next, &ctx->event_list, event_entry) {
		if (!event->attr.remove_on_exec)
			continue;

		if (!is_kernel_event(event))
			perf_remove_from_owner(event);

		modified = true;

		perf_event_exit_event(event, ctx);
	}

	raw_spin_lock_irqsave(&ctx->lock, flags);
	if (modified)
		clone_ctx = unclone_ctx(ctx);
	raw_spin_unlock_irqrestore(&ctx->lock, flags);

unlock:
	mutex_unlock(&ctx->mutex);

	if (clone_ctx)
		put_ctx(clone_ctx);
}

struct perf_read_data {
	struct perf_event *event;
	bool group;
	int ret;
};

static inline const struct cpumask *perf_scope_cpu_topology_cpumask(unsigned int scope, int cpu);

static int __perf_event_read_cpu(struct perf_event *event, int event_cpu)
{
	int local_cpu = smp_processor_id();
	u16 local_pkg, event_pkg;

	if ((unsigned)event_cpu >= nr_cpu_ids)
		return event_cpu;

	if (event->group_caps & PERF_EV_CAP_READ_SCOPE) {
		const struct cpumask *cpumask = perf_scope_cpu_topology_cpumask(event->pmu->scope, event_cpu);

		if (cpumask && cpumask_test_cpu(local_cpu, cpumask))
			return local_cpu;
	}

	if (event->group_caps & PERF_EV_CAP_READ_ACTIVE_PKG) {
		event_pkg = topology_physical_package_id(event_cpu);
		local_pkg = topology_physical_package_id(local_cpu);

		if (event_pkg == local_pkg)
			return local_cpu;
	}

	return event_cpu;
}

/*
 * Cross CPU call to read the hardware event
 */
static void __perf_event_read(void *info)
{
	struct perf_read_data *data = info;
	struct perf_event *sub, *event = data->event;
	struct perf_event_context *ctx = event->ctx;
	struct perf_cpu_context *cpuctx = this_cpu_ptr(&perf_cpu_context);
	struct pmu *pmu = event->pmu;

	/*
	 * If this is a task context, we need to check whether it is
	 * the current task context of this cpu.  If not it has been
	 * scheduled out before the smp call arrived.  In that case
	 * event->count would have been updated to a recent sample
	 * when the event was scheduled out.
	 */
	if (ctx->task && cpuctx->task_ctx != ctx)
		return;

	raw_spin_lock(&ctx->lock);
	ctx_time_update_event(ctx, event);

	perf_event_update_time(event);
	if (data->group)
		perf_event_update_sibling_time(event);

	if (event->state != PERF_EVENT_STATE_ACTIVE)
		goto unlock;

	if (!data->group) {
		pmu->read(event);
		data->ret = 0;
		goto unlock;
	}

	pmu->start_txn(pmu, PERF_PMU_TXN_READ);

	pmu->read(event);

	for_each_sibling_event(sub, event)
		perf_pmu_read(sub);

	data->ret = pmu->commit_txn(pmu);

unlock:
	raw_spin_unlock(&ctx->lock);
}

static inline u64 perf_event_count(struct perf_event *event, bool self)
{
	if (self)
		return local64_read(&event->count);

	return local64_read(&event->count) + atomic64_read(&event->child_count);
}

static void calc_timer_values(struct perf_event *event,
				u64 *now,
				u64 *enabled,
				u64 *running)
{
	u64 ctx_time;

	*now = perf_clock();
	ctx_time = perf_event_time_now(event, *now);
	__perf_update_times(event, ctx_time, enabled, running);
}

/*
 * NMI-safe method to read a local event, that is an event that
 * is:
 *   - either for the current task, or for this CPU
 *   - does not have inherit set, for inherited task events
 *     will not be local and we cannot read them atomically
 *   - must not have a pmu::count method
 */
int perf_event_read_local(struct perf_event *event, u64 *value,
			  u64 *enabled, u64 *running)
{
	unsigned long flags;
	int event_oncpu;
	int event_cpu;
	int ret = 0;

	/*
	 * Disabling interrupts avoids all counter scheduling (context
	 * switches, timer based rotation and IPIs).
	 */
	local_irq_save(flags);

	/*
	 * It must not be an event with inherit set, we cannot read
	 * all child counters from atomic context.
	 */
	if (event->attr.inherit) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	/* If this is a per-task event, it must be for current */
	if ((event->attach_state & PERF_ATTACH_TASK) &&
	    event->hw.target != current) {
		ret = -EINVAL;
		goto out;
	}

	/*
	 * Get the event CPU numbers, and adjust them to local if the event is
	 * a per-package event that can be read locally
	 */
	event_oncpu = __perf_event_read_cpu(event, event->oncpu);
	event_cpu = __perf_event_read_cpu(event, event->cpu);

	/* If this is a per-CPU event, it must be for this CPU */
	if (!(event->attach_state & PERF_ATTACH_TASK) &&
	    event_cpu != smp_processor_id()) {
		ret = -EINVAL;
		goto out;
	}

	/* If this is a pinned event it must be running on this CPU */
	if (event->attr.pinned && event_oncpu != smp_processor_id()) {
		ret = -EBUSY;
		goto out;
	}

	/*
	 * If the event is currently on this CPU, its either a per-task event,
	 * or local to this CPU. Furthermore it means its ACTIVE (otherwise
	 * oncpu == -1).
	 */
	if (event_oncpu == smp_processor_id())
		event->pmu->read(event);

	*value = local64_read(&event->count);
	if (enabled || running) {
		u64 __enabled, __running, __now;

		calc_timer_values(event, &__now, &__enabled, &__running);
		if (enabled)
			*enabled = __enabled;
		if (running)
			*running = __running;
	}
out:
	local_irq_restore(flags);

	return ret;
}

static int perf_event_read(struct perf_event *event, bool group)
{
	enum perf_event_state state = READ_ONCE(event->state);
	int event_cpu, ret = 0;

	/*
	 * If event is enabled and currently active on a CPU, update the
	 * value in the event structure:
	 */
again:
	if (state == PERF_EVENT_STATE_ACTIVE) {
		struct perf_read_data data;

		/*
		 * Orders the ->state and ->oncpu loads such that if we see
		 * ACTIVE we must also see the right ->oncpu.
		 *
		 * Matches the smp_wmb() from event_sched_in().
		 */
		smp_rmb();

		event_cpu = READ_ONCE(event->oncpu);
		if ((unsigned)event_cpu >= nr_cpu_ids)
			return 0;

		data = (struct perf_read_data){
			.event = event,
			.group = group,
			.ret = 0,
		};

		preempt_disable();
		event_cpu = __perf_event_read_cpu(event, event_cpu);

		/*
		 * Purposely ignore the smp_call_function_single() return
		 * value.
		 *
		 * If event_cpu isn't a valid CPU it means the event got
		 * scheduled out and that will have updated the event count.
		 *
		 * Therefore, either way, we'll have an up-to-date event count
		 * after this.
		 */
		(void)smp_call_function_single(event_cpu, __perf_event_read, &data, 1);
		preempt_enable();
		ret = data.ret;

	} else if (state == PERF_EVENT_STATE_INACTIVE) {
		struct perf_event_context *ctx = event->ctx;
		unsigned long flags;

		raw_spin_lock_irqsave(&ctx->lock, flags);
		state = event->state;
		if (state != PERF_EVENT_STATE_INACTIVE) {
			raw_spin_unlock_irqrestore(&ctx->lock, flags);
			goto again;
		}

		/*
		 * May read while context is not active (e.g., thread is
		 * blocked), in that case we cannot update context time
		 */
		ctx_time_update_event(ctx, event);

		perf_event_update_time(event);
		if (group)
			perf_event_update_sibling_time(event);
		raw_spin_unlock_irqrestore(&ctx->lock, flags);
	}

	return ret;
}

/*
 * Initialize the perf_event context in a task_struct:
 */
static void __perf_event_init_context(struct perf_event_context *ctx)
{
	raw_spin_lock_init(&ctx->lock);
	mutex_init(&ctx->mutex);
	INIT_LIST_HEAD(&ctx->pmu_ctx_list);
	perf_event_groups_init(&ctx->pinned_groups);
	perf_event_groups_init(&ctx->flexible_groups);
	INIT_LIST_HEAD(&ctx->event_list);
	refcount_set(&ctx->refcount, 1);
}

static void
__perf_init_event_pmu_context(struct perf_event_pmu_context *epc, struct pmu *pmu)
{
	epc->pmu = pmu;
	INIT_LIST_HEAD(&epc->pmu_ctx_entry);
	INIT_LIST_HEAD(&epc->pinned_active);
	INIT_LIST_HEAD(&epc->flexible_active);
	atomic_set(&epc->refcount, 1);
}

static struct perf_event_context *
alloc_perf_context(struct task_struct *task)
{
	struct perf_event_context *ctx;

	ctx = kzalloc(sizeof(struct perf_event_context), GFP_KERNEL);
	if (!ctx)
		return NULL;

	__perf_event_init_context(ctx);
	if (task)
		ctx->task = get_task_struct(task);

	return ctx;
}

static struct task_struct *
find_lively_task_by_vpid(pid_t vpid)
{
	struct task_struct *task;

	rcu_read_lock();
	if (!vpid)
		task = current;
	else
		task = find_task_by_vpid(vpid);
	if (task)
		get_task_struct(task);
	rcu_read_unlock();

	if (!task)
		return ERR_PTR(-ESRCH);

	return task;
}

/*
 * Returns a matching context with refcount and pincount.
 */
static struct perf_event_context *
find_get_context(struct task_struct *task, struct perf_event *event)
{
	struct perf_event_context *ctx, *clone_ctx = NULL;
	struct perf_cpu_context *cpuctx;
	unsigned long flags;
	int err;

	if (!task) {
		/* Must be root to operate on a CPU event: */
		err = perf_allow_cpu();
		if (err)
			return ERR_PTR(err);

		cpuctx = per_cpu_ptr(&perf_cpu_context, event->cpu);
		ctx = &cpuctx->ctx;
		get_ctx(ctx);
		raw_spin_lock_irqsave(&ctx->lock, flags);
		++ctx->pin_count;
		raw_spin_unlock_irqrestore(&ctx->lock, flags);

		return ctx;
	}

	err = -EINVAL;
retry:
	ctx = perf_lock_task_context(task, &flags);
	if (ctx) {
		clone_ctx = unclone_ctx(ctx);
		++ctx->pin_count;

		raw_spin_unlock_irqrestore(&ctx->lock, flags);

		if (clone_ctx)
			put_ctx(clone_ctx);
	} else {
		ctx = alloc_perf_context(task);
		err = -ENOMEM;
		if (!ctx)
			goto errout;

		err = 0;
		mutex_lock(&task->perf_event_mutex);
		/*
		 * If it has already passed perf_event_exit_task().
		 * we must see PF_EXITING, it takes this mutex too.
		 */
		if (task->flags & PF_EXITING)
			err = -ESRCH;
		else if (task->perf_event_ctxp)
			err = -EAGAIN;
		else {
			get_ctx(ctx);
			++ctx->pin_count;
			rcu_assign_pointer(task->perf_event_ctxp, ctx);
		}
		mutex_unlock(&task->perf_event_mutex);

		if (unlikely(err)) {
			put_ctx(ctx);

			if (err == -EAGAIN)
				goto retry;
			goto errout;
		}
	}

	return ctx;

errout:
	return ERR_PTR(err);
}

static struct perf_event_pmu_context *
find_get_pmu_context(struct pmu *pmu, struct perf_event_context *ctx,
		     struct perf_event *event)
{
	struct perf_event_pmu_context *new = NULL, *pos = NULL, *epc;

	if (!ctx->task) {
		/*
		 * perf_pmu_migrate_context() / __perf_pmu_install_event()
		 * relies on the fact that find_get_pmu_context() cannot fail
		 * for CPU contexts.
		 */
		struct perf_cpu_pmu_context *cpc;

		cpc = *per_cpu_ptr(pmu->cpu_pmu_context, event->cpu);
		epc = &cpc->epc;
		raw_spin_lock_irq(&ctx->lock);
		if (!epc->ctx) {
			/*
			 * One extra reference for the pmu; see perf_pmu_free().
			 */
			atomic_set(&epc->refcount, 2);
			epc->embedded = 1;
			list_add(&epc->pmu_ctx_entry, &ctx->pmu_ctx_list);
			epc->ctx = ctx;
		} else {
			WARN_ON_ONCE(epc->ctx != ctx);
			atomic_inc(&epc->refcount);
		}
		raw_spin_unlock_irq(&ctx->lock);
		return epc;
	}

	new = kzalloc(sizeof(*epc), GFP_KERNEL);
	if (!new)
		return ERR_PTR(-ENOMEM);

	__perf_init_event_pmu_context(new, pmu);

	/*
	 * XXX
	 *
	 * lockdep_assert_held(&ctx->mutex);
	 *
	 * can't because perf_event_init_task() doesn't actually hold the
	 * child_ctx->mutex.
	 */

	raw_spin_lock_irq(&ctx->lock);
	list_for_each_entry(epc, &ctx->pmu_ctx_list, pmu_ctx_entry) {
		if (epc->pmu == pmu) {
			WARN_ON_ONCE(epc->ctx != ctx);
			atomic_inc(&epc->refcount);
			goto found_epc;
		}
		/* Make sure the pmu_ctx_list is sorted by PMU type: */
		if (!pos && epc->pmu->type > pmu->type)
			pos = epc;
	}

	epc = new;
	new = NULL;

	if (!pos)
		list_add_tail(&epc->pmu_ctx_entry, &ctx->pmu_ctx_list);
	else
		list_add(&epc->pmu_ctx_entry, pos->pmu_ctx_entry.prev);

	epc->ctx = ctx;

found_epc:
	raw_spin_unlock_irq(&ctx->lock);
	kfree(new);

	return epc;
}

static void get_pmu_ctx(struct perf_event_pmu_context *epc)
{
	WARN_ON_ONCE(!atomic_inc_not_zero(&epc->refcount));
}

static void free_cpc_rcu(struct rcu_head *head)
{
	struct perf_cpu_pmu_context *cpc =
		container_of(head, typeof(*cpc), epc.rcu_head);

	kfree(cpc);
}

static void free_epc_rcu(struct rcu_head *head)
{
	struct perf_event_pmu_context *epc = container_of(head, typeof(*epc), rcu_head);

	kfree(epc);
}

static void put_pmu_ctx(struct perf_event_pmu_context *epc)
{
	struct perf_event_context *ctx = epc->ctx;
	unsigned long flags;

	/*
	 * XXX
	 *
	 * lockdep_assert_held(&ctx->mutex);
	 *
	 * can't because of the call-site in _free_event()/put_event()
	 * which isn't always called under ctx->mutex.
	 */
	if (!atomic_dec_and_raw_lock_irqsave(&epc->refcount, &ctx->lock, flags))
		return;

	WARN_ON_ONCE(list_empty(&epc->pmu_ctx_entry));

	list_del_init(&epc->pmu_ctx_entry);
	epc->ctx = NULL;

	WARN_ON_ONCE(!list_empty(&epc->pinned_active));
	WARN_ON_ONCE(!list_empty(&epc->flexible_active));

	raw_spin_unlock_irqrestore(&ctx->lock, flags);

	if (epc->embedded) {
		call_rcu(&epc->rcu_head, free_cpc_rcu);
		return;
	}

	call_rcu(&epc->rcu_head, free_epc_rcu);
}

static void perf_event_free_filter(struct perf_event *event);

static void free_event_rcu(struct rcu_head *head)
{
	struct perf_event *event = container_of(head, typeof(*event), rcu_head);

	if (event->ns)
		put_pid_ns(event->ns);
	perf_event_free_filter(event);
	kmem_cache_free(perf_event_cache, event);
}

static void ring_buffer_attach(struct perf_event *event,
			       struct perf_buffer *rb);

static void detach_sb_event(struct perf_event *event)
{
	struct pmu_event_list *pel = per_cpu_ptr(&pmu_sb_events, event->cpu);

	raw_spin_lock(&pel->lock);
	list_del_rcu(&event->sb_list);
	raw_spin_unlock(&pel->lock);
}

static bool is_sb_event(struct perf_event *event)
{
	struct perf_event_attr *attr = &event->attr;

	if (event->parent)
		return false;

	if (event->attach_state & PERF_ATTACH_TASK)
		return false;

	if (attr->mmap || attr->mmap_data || attr->mmap2 ||
	    attr->comm || attr->comm_exec ||
	    attr->task || attr->ksymbol ||
	    attr->context_switch || attr->text_poke ||
	    attr->bpf_event)
		return true;
	return false;
}

static void unaccount_pmu_sb_event(struct perf_event *event)
{
	if (is_sb_event(event))
		detach_sb_event(event);
}

#ifdef CONFIG_NO_HZ_FULL
static DEFINE_SPINLOCK(nr_freq_lock);
#endif

static void unaccount_freq_event_nohz(void)
{
#ifdef CONFIG_NO_HZ_FULL
	spin_lock(&nr_freq_lock);
	if (atomic_dec_and_test(&nr_freq_events))
		tick_nohz_dep_clear(TICK_DEP_BIT_PERF_EVENTS);
	spin_unlock(&nr_freq_lock);
#endif
}

static void unaccount_freq_event(void)
{
	if (tick_nohz_full_enabled())
		unaccount_freq_event_nohz();
	else
		atomic_dec(&nr_freq_events);
}


static struct perf_ctx_data *
alloc_perf_ctx_data(struct kmem_cache *ctx_cache, bool global)
{
	struct perf_ctx_data *cd;

	cd = kzalloc(sizeof(*cd), GFP_KERNEL);
	if (!cd)
		return NULL;

	cd->data = kmem_cache_zalloc(ctx_cache, GFP_KERNEL);
	if (!cd->data) {
		kfree(cd);
		return NULL;
	}

	cd->global = global;
	cd->ctx_cache = ctx_cache;
	refcount_set(&cd->refcount, 1);

	return cd;
}

static void free_perf_ctx_data(struct perf_ctx_data *cd)
{
	kmem_cache_free(cd->ctx_cache, cd->data);
	kfree(cd);
}

static void __free_perf_ctx_data_rcu(struct rcu_head *rcu_head)
{
	struct perf_ctx_data *cd;

	cd = container_of(rcu_head, struct perf_ctx_data, rcu_head);
	free_perf_ctx_data(cd);
}

static inline void perf_free_ctx_data_rcu(struct perf_ctx_data *cd)
{
	call_rcu(&cd->rcu_head, __free_perf_ctx_data_rcu);
}

static int
attach_task_ctx_data(struct task_struct *task, struct kmem_cache *ctx_cache,
		     bool global)
{
	struct perf_ctx_data *cd, *old = NULL;

	cd = alloc_perf_ctx_data(ctx_cache, global);
	if (!cd)
		return -ENOMEM;

	for (;;) {
		if (try_cmpxchg((struct perf_ctx_data **)&task->perf_ctx_data, &old, cd)) {
			if (old)
				perf_free_ctx_data_rcu(old);
			return 0;
		}

		if (!old) {
			/*
			 * After seeing a dead @old, we raced with
			 * removal and lost, try again to install @cd.
			 */
			continue;
		}

		if (refcount_inc_not_zero(&old->refcount)) {
			free_perf_ctx_data(cd); /* unused */
			return 0;
		}

		/*
		 * @old is a dead object, refcount==0 is stable, try and
		 * replace it with @cd.
		 */
	}
	return 0;
}

static void __detach_global_ctx_data(void);
DEFINE_STATIC_PERCPU_RWSEM(global_ctx_data_rwsem);
static refcount_t global_ctx_data_ref;

static int
attach_global_ctx_data(struct kmem_cache *ctx_cache)
{
	struct task_struct *g, *p;
	struct perf_ctx_data *cd;
	int ret;

	if (refcount_inc_not_zero(&global_ctx_data_ref))
		return 0;

	guard(percpu_write)(&global_ctx_data_rwsem);
	if (refcount_inc_not_zero(&global_ctx_data_ref))
		return 0;
again:
	/* Allocate everything */
	scoped_guard (rcu) {
		for_each_process_thread(g, p) {
			cd = rcu_dereference(p->perf_ctx_data);
			if (cd && !cd->global) {
				cd->global = 1;
				if (!refcount_inc_not_zero(&cd->refcount))
					cd = NULL;
			}
			if (!cd) {
				get_task_struct(p);
				goto alloc;
			}
		}
	}

	refcount_set(&global_ctx_data_ref, 1);

	return 0;
alloc:
	ret = attach_task_ctx_data(p, ctx_cache, true);
	put_task_struct(p);
	if (ret) {
		__detach_global_ctx_data();
		return ret;
	}
	goto again;
}

static int
attach_perf_ctx_data(struct perf_event *event)
{
	struct task_struct *task = event->hw.target;
	struct kmem_cache *ctx_cache = event->pmu->task_ctx_cache;
	int ret;

	if (!ctx_cache)
		return -ENOMEM;

	if (task)
		return attach_task_ctx_data(task, ctx_cache, false);

	ret = attach_global_ctx_data(ctx_cache);
	if (ret)
		return ret;

	event->attach_state |= PERF_ATTACH_GLOBAL_DATA;
	return 0;
}

static void
detach_task_ctx_data(struct task_struct *p)
{
	struct perf_ctx_data *cd;

	scoped_guard (rcu) {
		cd = rcu_dereference(p->perf_ctx_data);
		if (!cd || !refcount_dec_and_test(&cd->refcount))
			return;
	}

	/*
	 * The old ctx_data may be lost because of the race.
	 * Nothing is required to do for the case.
	 * See attach_task_ctx_data().
	 */
	if (try_cmpxchg((struct perf_ctx_data **)&p->perf_ctx_data, &cd, NULL))
		perf_free_ctx_data_rcu(cd);
}

static void __detach_global_ctx_data(void)
{
	struct task_struct *g, *p;
	struct perf_ctx_data *cd;

again:
	scoped_guard (rcu) {
		for_each_process_thread(g, p) {
			cd = rcu_dereference(p->perf_ctx_data);
			if (!cd || !cd->global)
				continue;
			cd->global = 0;
			get_task_struct(p);
			goto detach;
		}
	}
	return;
detach:
	detach_task_ctx_data(p);
	put_task_struct(p);
	goto again;
}

static void detach_global_ctx_data(void)
{
	if (refcount_dec_not_one(&global_ctx_data_ref))
		return;

	guard(percpu_write)(&global_ctx_data_rwsem);
	if (!refcount_dec_and_test(&global_ctx_data_ref))
		return;

	/* remove everything */
	__detach_global_ctx_data();
}

static void detach_perf_ctx_data(struct perf_event *event)
{
	struct task_struct *task = event->hw.target;

	event->attach_state &= ~PERF_ATTACH_TASK_DATA;

	if (task)
		return detach_task_ctx_data(task);

	if (event->attach_state & PERF_ATTACH_GLOBAL_DATA) {
		detach_global_ctx_data();
		event->attach_state &= ~PERF_ATTACH_GLOBAL_DATA;
	}
}

static void unaccount_event(struct perf_event *event)
{
	bool dec = false;

	if (event->parent)
		return;

	if (event->attach_state & (PERF_ATTACH_TASK | PERF_ATTACH_SCHED_CB))
		dec = true;
	if (event->attr.mmap || event->attr.mmap_data)
		atomic_dec(&nr_mmap_events);
	if (event->attr.build_id)
		atomic_dec(&nr_build_id_events);
	if (event->attr.comm)
		atomic_dec(&nr_comm_events);
	if (event->attr.namespaces)
		atomic_dec(&nr_namespaces_events);
	if (event->attr.cgroup)
		atomic_dec(&nr_cgroup_events);
	if (event->attr.task)
		atomic_dec(&nr_task_events);
	if (event->attr.freq)
		unaccount_freq_event();
	if (event->attr.context_switch) {
		dec = true;
		atomic_dec(&nr_switch_events);
	}
	if (is_cgroup_event(event))
		dec = true;
	if (has_branch_stack(event))
		dec = true;
	if (event->attr.ksymbol)
		atomic_dec(&nr_ksymbol_events);
	if (event->attr.bpf_event)
		atomic_dec(&nr_bpf_events);
	if (event->attr.text_poke)
		atomic_dec(&nr_text_poke_events);

	if (dec) {
		if (!atomic_add_unless(&perf_sched_count, -1, 1))
			schedule_delayed_work(&perf_sched_work, HZ);
	}

	unaccount_pmu_sb_event(event);
}

static void perf_sched_delayed(struct work_struct *work)
{
	mutex_lock(&perf_sched_mutex);
	if (atomic_dec_and_test(&perf_sched_count))
		static_branch_disable(&perf_sched_events);
	mutex_unlock(&perf_sched_mutex);
}

/*
 * The following implement mutual exclusion of events on "exclusive" pmus
 * (PERF_PMU_CAP_EXCLUSIVE). Such pmus can only have one event scheduled
 * at a time, so we disallow creating events that might conflict, namely:
 *
 *  1) cpu-wide events in the presence of per-task events,
 *  2) per-task events in the presence of cpu-wide events,
 *  3) two matching events on the same perf_event_context.
 *
 * The former two cases are handled in the allocation path (perf_event_alloc(),
 * _free_event()), the latter -- before the first perf_install_in_context().
 */
static int exclusive_event_init(struct perf_event *event)
{
	struct pmu *pmu = event->pmu;

	if (!is_exclusive_pmu(pmu))
		return 0;

	/*
	 * Prevent co-existence of per-task and cpu-wide events on the
	 * same exclusive pmu.
	 *
	 * Negative pmu::exclusive_cnt means there are cpu-wide
	 * events on this "exclusive" pmu, positive means there are
	 * per-task events.
	 *
	 * Since this is called in perf_event_alloc() path, event::ctx
	 * doesn't exist yet; it is, however, safe to use PERF_ATTACH_TASK
	 * to mean "per-task event", because unlike other attach states it
	 * never gets cleared.
	 */
	if (event->attach_state & PERF_ATTACH_TASK) {
		if (!atomic_inc_unless_negative(&pmu->exclusive_cnt))
			return -EBUSY;
	} else {
		if (!atomic_dec_unless_positive(&pmu->exclusive_cnt))
			return -EBUSY;
	}

	event->attach_state |= PERF_ATTACH_EXCLUSIVE;

	return 0;
}

static void exclusive_event_destroy(struct perf_event *event)
{
	struct pmu *pmu = event->pmu;

	/* see comment in exclusive_event_init() */
	if (event->attach_state & PERF_ATTACH_TASK)
		atomic_dec(&pmu->exclusive_cnt);
	else
		atomic_inc(&pmu->exclusive_cnt);

	event->attach_state &= ~PERF_ATTACH_EXCLUSIVE;
}

static bool exclusive_event_match(struct perf_event *e1, struct perf_event *e2)
{
	if ((e1->pmu == e2->pmu) &&
	    (e1->cpu == e2->cpu ||
	     e1->cpu == -1 ||
	     e2->cpu == -1))
		return true;
	return false;
}

static bool exclusive_event_installable(struct perf_event *event,
					struct perf_event_context *ctx)
{
	struct perf_event *iter_event;
	struct pmu *pmu = event->pmu;

	lockdep_assert_held(&ctx->mutex);

	if (!is_exclusive_pmu(pmu))
		return true;

	list_for_each_entry(iter_event, &ctx->event_list, event_entry) {
		if (exclusive_event_match(iter_event, event))
			return false;
	}

	return true;
}

static void perf_free_addr_filters(struct perf_event *event);

/* vs perf_event_alloc() error */
static void __free_event(struct perf_event *event)
{
	if (event->attach_state & PERF_ATTACH_CALLCHAIN)
		put_callchain_buffers();

	kfree(event->addr_filter_ranges);

	if (event->attach_state & PERF_ATTACH_EXCLUSIVE)
		exclusive_event_destroy(event);

	if (is_cgroup_event(event))
		perf_detach_cgroup(event);

	if (event->attach_state & PERF_ATTACH_TASK_DATA)
		detach_perf_ctx_data(event);

	if (event->destroy)
		event->destroy(event);

	/*
	 * Must be after ->destroy(), due to uprobe_perf_close() using
	 * hw.target.
	 */
	if (event->hw.target)
		put_task_struct(event->hw.target);

	if (event->pmu_ctx) {
		/*
		 * put_pmu_ctx() needs an event->ctx reference, because of
		 * epc->ctx.
		 */
		WARN_ON_ONCE(!event->ctx);
		WARN_ON_ONCE(event->pmu_ctx->ctx != event->ctx);
		put_pmu_ctx(event->pmu_ctx);
	}

	/*
	 * perf_event_free_task() relies on put_ctx() being 'last', in
	 * particular all task references must be cleaned up.
	 */
	if (event->ctx)
		put_ctx(event->ctx);

	if (event->pmu)
		module_put(event->pmu->module);

	call_rcu(&event->rcu_head, free_event_rcu);
}

DEFINE_FREE(__free_event, struct perf_event *, if (_T) __free_event(_T))

/* vs perf_event_alloc() success */
static void _free_event(struct perf_event *event)
{
	irq_work_sync(&event->pending_irq);
	irq_work_sync(&event->pending_disable_irq);

	unaccount_event(event);

	security_perf_event_free(event);

	if (event->rb) {
		/*
		 * Can happen when we close an event with re-directed output.
		 *
		 * Since we have a 0 refcount, perf_mmap_close() will skip
		 * over us; possibly making our ring_buffer_put() the last.
		 */
		mutex_lock(&event->mmap_mutex);
		ring_buffer_attach(event, NULL);
		mutex_unlock(&event->mmap_mutex);
	}

	perf_event_free_bpf_prog(event);
	perf_free_addr_filters(event);

	__free_event(event);
}

/*
 * Used to free events which have a known refcount of 1, such as in error paths
 * where the event isn't exposed yet and inherited events.
 */
static void free_event(struct perf_event *event)
{
	if (WARN(atomic_long_cmpxchg(&event->refcount, 1, 0) != 1,
				"unexpected event refcount: %ld; ptr=%p\n",
				atomic_long_read(&event->refcount), event)) {
		/* leak to avoid use-after-free */
		return;
	}

	_free_event(event);
}

/*
 * Remove user event from the owner task.
 */
static void perf_remove_from_owner(struct perf_event *event)
{
	struct task_struct *owner;

	rcu_read_lock();
	/*
	 * Matches the smp_store_release() in perf_event_exit_task(). If we
	 * observe !owner it means the list deletion is complete and we can
	 * indeed free this event, otherwise we need to serialize on
	 * owner->perf_event_mutex.
	 */
	owner = READ_ONCE(event->owner);
	if (owner) {
		/*
		 * Since delayed_put_task_struct() also drops the last
		 * task reference we can safely take a new reference
		 * while holding the rcu_read_lock().
		 */
		get_task_struct(owner);
	}
	rcu_read_unlock();

	if (owner) {
		/*
		 * If we're here through perf_event_exit_task() we're already
		 * holding ctx->mutex which would be an inversion wrt. the
		 * normal lock order.
		 *
		 * However we can safely take this lock because its the child
		 * ctx->mutex.
		 */
		mutex_lock_nested(&owner->perf_event_mutex, SINGLE_DEPTH_NESTING);

		/*
		 * We have to re-check the event->owner field, if it is cleared
		 * we raced with perf_event_exit_task(), acquiring the mutex
		 * ensured they're done, and we can proceed with freeing the
		 * event.
		 */
		if (event->owner) {
			list_del_init(&event->owner_entry);
			smp_store_release(&event->owner, NULL);
		}
		mutex_unlock(&owner->perf_event_mutex);
		put_task_struct(owner);
	}
}

static void put_event(struct perf_event *event)
{
	struct perf_event *parent;

	if (!atomic_long_dec_and_test(&event->refcount))
		return;

	parent = event->parent;
	_free_event(event);

	/* Matches the refcount bump in inherit_event() */
	if (parent)
		put_event(parent);
}

/*
 * Kill an event dead; while event:refcount will preserve the event
 * object, it will not preserve its functionality. Once the last 'user'
 * gives up the object, we'll destroy the thing.
 */
int perf_event_release_kernel(struct perf_event *event)
{
	struct perf_event_context *ctx = event->ctx;
	struct perf_event *child, *tmp;
	LIST_HEAD(free_list);

	/*
	 * If we got here through err_alloc: free_event(event); we will not
	 * have attached to a context yet.
	 */
	if (!ctx) {
		WARN_ON_ONCE(event->attach_state &
				(PERF_ATTACH_CONTEXT|PERF_ATTACH_GROUP));
		goto no_ctx;
	}

	if (!is_kernel_event(event))
		perf_remove_from_owner(event);

	ctx = perf_event_ctx_lock(event);
	WARN_ON_ONCE(ctx->parent_ctx);

	/*
	 * Mark this event as STATE_DEAD, there is no external reference to it
	 * anymore.
	 *
	 * Anybody acquiring event->child_mutex after the below loop _must_
	 * also see this, most importantly inherit_event() which will avoid
	 * placing more children on the list.
	 *
	 * Thus this guarantees that we will in fact observe and kill _ALL_
	 * child events.
	 */
	perf_remove_from_context(event, DETACH_GROUP|DETACH_DEAD);

	perf_event_ctx_unlock(event, ctx);

again:
	mutex_lock(&event->child_mutex);
	list_for_each_entry(child, &event->child_list, child_list) {
		void *var = NULL;

		/*
		 * Cannot change, child events are not migrated, see the
		 * comment with perf_event_ctx_lock_nested().
		 */
		ctx = READ_ONCE(child->ctx);
		/*
		 * Since child_mutex nests inside ctx::mutex, we must jump
		 * through hoops. We start by grabbing a reference on the ctx.
		 *
		 * Since the event cannot get freed while we hold the
		 * child_mutex, the context must also exist and have a !0
		 * reference count.
		 */
		get_ctx(ctx);

		/*
		 * Now that we have a ctx ref, we can drop child_mutex, and
		 * acquire ctx::mutex without fear of it going away. Then we
		 * can re-acquire child_mutex.
		 */
		mutex_unlock(&event->child_mutex);
		mutex_lock(&ctx->mutex);
		mutex_lock(&event->child_mutex);

		/*
		 * Now that we hold ctx::mutex and child_mutex, revalidate our
		 * state, if child is still the first entry, it didn't get freed
		 * and we can continue doing so.
		 */
		tmp = list_first_entry_or_null(&event->child_list,
					       struct perf_event, child_list);
		if (tmp == child) {
			perf_remove_from_context(child, DETACH_GROUP);
			list_move(&child->child_list, &free_list);
		} else {
			var = &ctx->refcount;
		}

		mutex_unlock(&event->child_mutex);
		mutex_unlock(&ctx->mutex);
		put_ctx(ctx);

		if (var) {
			/*
			 * If perf_event_free_task() has deleted all events from the
			 * ctx while the child_mutex got released above, make sure to
			 * notify about the preceding put_ctx().
			 */
			smp_mb(); /* pairs with wait_var_event() */
			wake_up_var(var);
		}
		goto again;
	}
	mutex_unlock(&event->child_mutex);

	list_for_each_entry_safe(child, tmp, &free_list, child_list) {
		void *var = &child->ctx->refcount;

		list_del(&child->child_list);
		/* Last reference unless ->pending_task work is pending */
		put_event(child);

		/*
		 * Wake any perf_event_free_task() waiting for this event to be
		 * freed.
		 */
		smp_mb(); /* pairs with wait_var_event() */
		wake_up_var(var);
	}

no_ctx:
	/*
	 * Last reference unless ->pending_task work is pending on this event
	 * or any of its children.
	 */
	put_event(event);
	return 0;
}
EXPORT_SYMBOL_GPL(perf_event_release_kernel);

/*
 * Called when the last reference to the file is gone.
 */
static int perf_release(struct inode *inode, struct file *file)
{
	perf_event_release_kernel(file->private_data);
	return 0;
}

static u64 __perf_event_read_value(struct perf_event *event, u64 *enabled, u64 *running)
{
	struct perf_event *child;
	u64 total = 0;

	*enabled = 0;
	*running = 0;

	mutex_lock(&event->child_mutex);

	(void)perf_event_read(event, false);
	total += perf_event_count(event, false);

	*enabled += event->total_time_enabled +
			atomic64_read(&event->child_total_time_enabled);
	*running += event->total_time_running +
			atomic64_read(&event->child_total_time_running);

	list_for_each_entry(child, &event->child_list, child_list) {
		(void)perf_event_read(child, false);
		total += perf_event_count(child, false);
		*enabled += child->total_time_enabled;
		*running += child->total_time_running;
	}
	mutex_unlock(&event->child_mutex);

	return total;
}

u64 perf_event_read_value(struct perf_event *event, u64 *enabled, u64 *running)
{
	struct perf_event_context *ctx;
	u64 count;

	ctx = perf_event_ctx_lock(event);
	count = __perf_event_read_value(event, enabled, running);
	perf_event_ctx_unlock(event, ctx);

	return count;
}
EXPORT_SYMBOL_GPL(perf_event_read_value);

static int __perf_read_group_add(struct perf_event *leader,
					u64 read_format, u64 *values)
{
	struct perf_event_context *ctx = leader->ctx;
	struct perf_event *sub, *parent;
	unsigned long flags;
	int n = 1; /* skip @nr */
	int ret;

	ret = perf_event_read(leader, true);
	if (ret)
		return ret;

	raw_spin_lock_irqsave(&ctx->lock, flags);
	/*
	 * Verify the grouping between the parent and child (inherited)
	 * events is still in tact.
	 *
	 * Specifically:
	 *  - leader->ctx->lock pins leader->sibling_list
	 *  - parent->child_mutex pins parent->child_list
	 *  - parent->ctx->mutex pins parent->sibling_list
	 *
	 * Because parent->ctx != leader->ctx (and child_list nests inside
	 * ctx->mutex), group destruction is not atomic between children, also
	 * see perf_event_release_kernel(). Additionally, parent can grow the
	 * group.
	 *
	 * Therefore it is possible to have parent and child groups in a
	 * different configuration and summing over such a beast makes no sense
	 * what so ever.
	 *
	 * Reject this.
	 */
	parent = leader->parent;
	if (parent &&
	    (parent->group_generation != leader->group_generation ||
	     parent->nr_siblings != leader->nr_siblings)) {
		ret = -ECHILD;
		goto unlock;
	}

	/*
	 * Since we co-schedule groups, {enabled,running} times of siblings
	 * will be identical to those of the leader, so we only publish one
	 * set.
	 */
	if (read_format & PERF_FORMAT_TOTAL_TIME_ENABLED) {
		values[n++] += leader->total_time_enabled +
			atomic64_read(&leader->child_total_time_enabled);
	}

	if (read_format & PERF_FORMAT_TOTAL_TIME_RUNNING) {
		values[n++] += leader->total_time_running +
			atomic64_read(&leader->child_total_time_running);
	}

	/*
	 * Write {count,id} tuples for every sibling.
	 */
	values[n++] += perf_event_count(leader, false);
	if (read_format & PERF_FORMAT_ID)
		values[n++] = primary_event_id(leader);
	if (read_format & PERF_FORMAT_LOST)
		values[n++] = atomic64_read(&leader->lost_samples);

	for_each_sibling_event(sub, leader) {
		values[n++] += perf_event_count(sub, false);
		if (read_format & PERF_FORMAT_ID)
			values[n++] = primary_event_id(sub);
		if (read_format & PERF_FORMAT_LOST)
			values[n++] = atomic64_read(&sub->lost_samples);
	}

unlock:
	raw_spin_unlock_irqrestore(&ctx->lock, flags);
	return ret;
}

static int perf_read_group(struct perf_event *event,
				   u64 read_format, char __user *buf)
{
	struct perf_event *leader = event->group_leader, *child;
	struct perf_event_context *ctx = leader->ctx;
	int ret;
	u64 *values;

	lockdep_assert_held(&ctx->mutex);

	values = kzalloc(event->read_size, GFP_KERNEL);
	if (!values)
		return -ENOMEM;

	values[0] = 1 + leader->nr_siblings;

	mutex_lock(&leader->child_mutex);

	ret = __perf_read_group_add(leader, read_format, values);
	if (ret)
		goto unlock;

	list_for_each_entry(child, &leader->child_list, child_list) {
		ret = __perf_read_group_add(child, read_format, values);
		if (ret)
			goto unlock;
	}

	mutex_unlock(&leader->child_mutex);

	ret = event->read_size;
	if (copy_to_user(buf, values, event->read_size))
		ret = -EFAULT;
	goto out;

unlock:
	mutex_unlock(&leader->child_mutex);
out:
	kfree(values);
	return ret;
}

static int perf_read_one(struct perf_event *event,
				 u64 read_format, char __user *buf)
{
	u64 enabled, running;
	u64 values[5];
	int n = 0;

	values[n++] = __perf_event_read_value(event, &enabled, &running);
	if (read_format & PERF_FORMAT_TOTAL_TIME_ENABLED)
		values[n++] = enabled;
	if (read_format & PERF_FORMAT_TOTAL_TIME_RUNNING)
		values[n++] = running;
	if (read_format & PERF_FORMAT_ID)
		values[n++] = primary_event_id(event);
	if (read_format & PERF_FORMAT_LOST)
		values[n++] = atomic64_read(&event->lost_samples);

	if (copy_to_user(buf, values, n * sizeof(u64)))
		return -EFAULT;

	return n * sizeof(u64);
}

static bool is_event_hup(struct perf_event *event)
{
	bool no_children;

	if (event->state > PERF_EVENT_STATE_EXIT)
		return false;

	mutex_lock(&event->child_mutex);
	no_children = list_empty(&event->child_list);
	mutex_unlock(&event->child_mutex);
	return no_children;
}

/*
 * Read the performance event - simple non blocking version for now
 */
static ssize_t
__perf_read(struct perf_event *event, char __user *buf, size_t count)
{
	u64 read_format = event->attr.read_format;
	int ret;

	/*
	 * Return end-of-file for a read on an event that is in
	 * error state (i.e. because it was pinned but it couldn't be
	 * scheduled on to the CPU at some point).
	 */
	if (event->state == PERF_EVENT_STATE_ERROR)
		return 0;

	if (count < event->read_size)
		return -ENOSPC;

	WARN_ON_ONCE(event->ctx->parent_ctx);
	if (read_format & PERF_FORMAT_GROUP)
		ret = perf_read_group(event, read_format, buf);
	else
		ret = perf_read_one(event, read_format, buf);

	return ret;
}

static ssize_t
perf_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct perf_event *event = file->private_data;
	struct perf_event_context *ctx;
	int ret;

	ret = security_perf_event_read(event);
	if (ret)
		return ret;

	ctx = perf_event_ctx_lock(event);
	ret = __perf_read(event, buf, count);
	perf_event_ctx_unlock(event, ctx);

	return ret;
}

static __poll_t perf_poll(struct file *file, poll_table *wait)
{
	struct perf_event *event = file->private_data;
	struct perf_buffer *rb;
	__poll_t events = EPOLLHUP;

	poll_wait(file, &event->waitq, wait);

	if (is_event_hup(event))
		return events;

	if (unlikely(READ_ONCE(event->state) == PERF_EVENT_STATE_ERROR &&
		     event->attr.pinned))
		return EPOLLERR;

	/*
	 * Pin the event->rb by taking event->mmap_mutex; otherwise
	 * perf_event_set_output() can swizzle our rb and make us miss wakeups.
	 */
	mutex_lock(&event->mmap_mutex);
	rb = event->rb;
	if (rb)
		events = atomic_xchg(&rb->poll, 0);
	mutex_unlock(&event->mmap_mutex);
	return events;
}

static void _perf_event_reset(struct perf_event *event)
{
	(void)perf_event_read(event, false);
	local64_set(&event->count, 0);
	perf_event_update_userpage(event);
}

/* Assume it's not an event with inherit set. */
u64 perf_event_pause(struct perf_event *event, bool reset)
{
	struct perf_event_context *ctx;
	u64 count;

	ctx = perf_event_ctx_lock(event);
	WARN_ON_ONCE(event->attr.inherit);
	_perf_event_disable(event);
	count = local64_read(&event->count);
	if (reset)
		local64_set(&event->count, 0);
	perf_event_ctx_unlock(event, ctx);

	return count;
}
EXPORT_SYMBOL_GPL(perf_event_pause);

/*
 * Holding the top-level event's child_mutex means that any
 * descendant process that has inherited this event will block
 * in perf_event_exit_event() if it goes to exit, thus satisfying the
 * task existence requirements of perf_event_enable/disable.
 */
static void perf_event_for_each_child(struct perf_event *event,
					void (*func)(struct perf_event *))
{
	struct perf_event *child;

	WARN_ON_ONCE(event->ctx->parent_ctx);

	mutex_lock(&event->child_mutex);
	func(event);
	list_for_each_entry(child, &event->child_list, child_list)
		func(child);
	mutex_unlock(&event->child_mutex);
}

static void perf_event_for_each(struct perf_event *event,
				  void (*func)(struct perf_event *))
{
	struct perf_event_context *ctx = event->ctx;
	struct perf_event *sibling;

	lockdep_assert_held(&ctx->mutex);

	event = event->group_leader;

	perf_event_for_each_child(event, func);
	for_each_sibling_event(sibling, event)
		perf_event_for_each_child(sibling, func);
}

static void __perf_event_period(struct perf_event *event,
				struct perf_cpu_context *cpuctx,
				struct perf_event_context *ctx,
				void *info)
{
	u64 value = *((u64 *)info);
	bool active;

	if (event->attr.freq) {
		event->attr.sample_freq = value;
	} else {
		event->attr.sample_period = value;
		event->hw.sample_period = value;
	}

	active = (event->state == PERF_EVENT_STATE_ACTIVE);
	if (active) {
		perf_pmu_disable(event->pmu);
		/*
		 * We could be throttled; unthrottle now to avoid the tick
		 * trying to unthrottle while we already re-started the event.
		 */
		if (event->hw.interrupts == MAX_INTERRUPTS) {
			event->hw.interrupts = 0;
			perf_log_throttle(event, 1);
		}
		event->pmu->stop(event, PERF_EF_UPDATE);
	}

	local64_set(&event->hw.period_left, 0);

	if (active) {
		event->pmu->start(event, PERF_EF_RELOAD);
		perf_pmu_enable(event->pmu);
	}
}

static int perf_event_check_period(struct perf_event *event, u64 value)
{
	return event->pmu->check_period(event, value);
}

static int _perf_event_period(struct perf_event *event, u64 value)
{
	if (!is_sampling_event(event))
		return -EINVAL;

	if (!value)
		return -EINVAL;

	if (event->attr.freq) {
		if (value > sysctl_perf_event_sample_rate)
			return -EINVAL;
	} else {
		if (perf_event_check_period(event, value))
			return -EINVAL;
		if (value & (1ULL << 63))
			return -EINVAL;
	}

	event_function_call(event, __perf_event_period, &value);

	return 0;
}

int perf_event_period(struct perf_event *event, u64 value)
{
	struct perf_event_context *ctx;
	int ret;

	ctx = perf_event_ctx_lock(event);
	ret = _perf_event_period(event, value);
	perf_event_ctx_unlock(event, ctx);

	return ret;
}
EXPORT_SYMBOL_GPL(perf_event_period);

static const struct file_operations perf_fops;

static inline bool is_perf_file(struct fd f)
{
	return !fd_empty(f) && fd_file(f)->f_op == &perf_fops;
}

static int perf_event_set_output(struct perf_event *event,
				 struct perf_event *output_event);
static int perf_event_set_filter(struct perf_event *event, void __user *arg);
static int perf_copy_attr(struct perf_event_attr __user *uattr,
			  struct perf_event_attr *attr);
static int __perf_event_set_bpf_prog(struct perf_event *event,
				     struct bpf_prog *prog,
				     u64 bpf_cookie);

static long _perf_ioctl(struct perf_event *event, unsigned int cmd, unsigned long arg)
{
	void (*func)(struct perf_event *);
	u32 flags = arg;

	switch (cmd) {
	case PERF_EVENT_IOC_ENABLE:
		func = _perf_event_enable;
		break;
	case PERF_EVENT_IOC_DISABLE:
		func = _perf_event_disable;
		break;
	case PERF_EVENT_IOC_RESET:
		func = _perf_event_reset;
		break;

	case PERF_EVENT_IOC_REFRESH:
		return _perf_event_refresh(event, arg);

	case PERF_EVENT_IOC_PERIOD:
	{
		u64 value;

		if (copy_from_user(&value, (u64 __user *)arg, sizeof(value)))
			return -EFAULT;

		return _perf_event_period(event, value);
	}
	case PERF_EVENT_IOC_ID:
	{
		u64 id = primary_event_id(event);

		if (copy_to_user((void __user *)arg, &id, sizeof(id)))
			return -EFAULT;
		return 0;
	}

	case PERF_EVENT_IOC_SET_OUTPUT:
	{
		CLASS(fd, output)(arg);	     // arg == -1 => empty
		struct perf_event *output_event = NULL;
		if (arg != -1) {
			if (!is_perf_file(output))
				return -EBADF;
			output_event = fd_file(output)->private_data;
		}
		return perf_event_set_output(event, output_event);
	}

	case PERF_EVENT_IOC_SET_FILTER:
		return perf_event_set_filter(event, (void __user *)arg);

	case PERF_EVENT_IOC_SET_BPF:
	{
		struct bpf_prog *prog;
		int err;

		prog = bpf_prog_get(arg);
		if (IS_ERR(prog))
			return PTR_ERR(prog);

		err = __perf_event_set_bpf_prog(event, prog, 0);
		if (err) {
			bpf_prog_put(prog);
			return err;
		}

		return 0;
	}

	case PERF_EVENT_IOC_PAUSE_OUTPUT: {
		struct perf_buffer *rb;

		rcu_read_lock();
		rb = rcu_dereference(event->rb);
		if (!rb || !rb->nr_pages) {
			rcu_read_unlock();
			return -EINVAL;
		}
		rb_toggle_paused(rb, !!arg);
		rcu_read_unlock();
		return 0;
	}

	case PERF_EVENT_IOC_QUERY_BPF:
		return perf_event_query_prog_array(event, (void __user *)arg);

	case PERF_EVENT_IOC_MODIFY_ATTRIBUTES: {
		struct perf_event_attr new_attr;
		int err = perf_copy_attr((struct perf_event_attr __user *)arg,
					 &new_attr);

		if (err)
			return err;

		return perf_event_modify_attr(event,  &new_attr);
	}
	default:
		return -ENOTTY;
	}

	if (flags & PERF_IOC_FLAG_GROUP)
		perf_event_for_each(event, func);
	else
		perf_event_for_each_child(event, func);

	return 0;
}

static long perf_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct perf_event *event = file->private_data;
	struct perf_event_context *ctx;
	long ret;

	/* Treat ioctl like writes as it is likely a mutating operation. */
	ret = security_perf_event_write(event);
	if (ret)
		return ret;

	ctx = perf_event_ctx_lock(event);
	ret = _perf_ioctl(event, cmd, arg);
	perf_event_ctx_unlock(event, ctx);

	return ret;
}

#ifdef CONFIG_COMPAT
static long perf_compat_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	switch (_IOC_NR(cmd)) {
	case _IOC_NR(PERF_EVENT_IOC_SET_FILTER):
	case _IOC_NR(PERF_EVENT_IOC_ID):
	case _IOC_NR(PERF_EVENT_IOC_QUERY_BPF):
	case _IOC_NR(PERF_EVENT_IOC_MODIFY_ATTRIBUTES):
		/* Fix up pointer size (usually 4 -> 8 in 32-on-64-bit case */
		if (_IOC_SIZE(cmd) == sizeof(compat_uptr_t)) {
			cmd &= ~IOCSIZE_MASK;
			cmd |= sizeof(void *) << IOCSIZE_SHIFT;
		}
		break;
	}
	return perf_ioctl(file, cmd, arg);
}
#else
# define perf_compat_ioctl NULL
#endif

int perf_event_task_enable(void)
{
	struct perf_event_context *ctx;
	struct perf_event *event;

	mutex_lock(&current->perf_event_mutex);
	list_for_each_entry(event, &current->perf_event_list, owner_entry) {
		ctx = perf_event_ctx_lock(event);
		perf_event_for_each_child(event, _perf_event_enable);
		perf_event_ctx_unlock(event, ctx);
	}
	mutex_unlock(&current->perf_event_mutex);

	return 0;
}

int perf_event_task_disable(void)
{
	struct perf_event_context *ctx;
	struct perf_event *event;

	mutex_lock(&current->perf_event_mutex);
	list_for_each_entry(event, &current->perf_event_list, owner_entry) {
		ctx = perf_event_ctx_lock(event);
		perf_event_for_each_child(event, _perf_event_disable);
		perf_event_ctx_unlock(event, ctx);
	}
	mutex_unlock(&current->perf_event_mutex);

	return 0;
}

static int perf_event_index(struct perf_event *event)
{
	if (event->hw.state & PERF_HES_STOPPED)
		return 0;

	if (event->state != PERF_EVENT_STATE_ACTIVE)
		return 0;

	return event->pmu->event_idx(event);
}

static void perf_event_init_userpage(struct perf_event *event)
{
	struct perf_event_mmap_page *userpg;
	struct perf_buffer *rb;

	rcu_read_lock();
	rb = rcu_dereference(event->rb);
	if (!rb)
		goto unlock;

	userpg = rb->user_page;

	/* Allow new userspace to detect that bit 0 is deprecated */
	userpg->cap_bit0_is_deprecated = 1;
	userpg->size = offsetof(struct perf_event_mmap_page, __reserved);
	userpg->data_offset = PAGE_SIZE;
	userpg->data_size = perf_data_size(rb);

unlock:
	rcu_read_unlock();
}

void __weak arch_perf_update_userpage(
	struct perf_event *event, struct perf_event_mmap_page *userpg, u64 now)
{
}

/*
 * Callers need to ensure there can be no nesting of this function, otherwise
 * the seqlock logic goes bad. We can not serialize this because the arch
 * code calls this from NMI context.
 */
void perf_event_update_userpage(struct perf_event *event)
{
	struct perf_event_mmap_page *userpg;
	struct perf_buffer *rb;
	u64 enabled, running, now;

	rcu_read_lock();
	rb = rcu_dereference(event->rb);
	if (!rb)
		goto unlock;

	/*
	 * compute total_time_enabled, total_time_running
	 * based on snapshot values taken when the event
	 * was last scheduled in.
	 *
	 * we cannot simply called update_context_time()
	 * because of locking issue as we can be called in
	 * NMI context
	 */
	calc_timer_values(event, &now, &enabled, &running);

	userpg = rb->user_page;
	/*
	 * Disable preemption to guarantee consistent time stamps are stored to
	 * the user page.
	 */
	preempt_disable();
	++userpg->lock;
	barrier();
	userpg->index = perf_event_index(event);
	userpg->offset = perf_event_count(event, false);
	if (userpg->index)
		userpg->offset -= local64_read(&event->hw.prev_count);

	userpg->time_enabled = enabled +
			atomic64_read(&event->child_total_time_enabled);

	userpg->time_running = running +
			atomic64_read(&event->child_total_time_running);

	arch_perf_update_userpage(event, userpg, now);

	barrier();
	++userpg->lock;
	preempt_enable();
unlock:
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(perf_event_update_userpage);

static void ring_buffer_attach(struct perf_event *event,
			       struct perf_buffer *rb)
{
	struct perf_buffer *old_rb = NULL;
	unsigned long flags;

	WARN_ON_ONCE(event->parent);

	if (event->rb) {
		/*
		 * Should be impossible, we set this when removing
		 * event->rb_entry and wait/clear when adding event->rb_entry.
		 */
		WARN_ON_ONCE(event->rcu_pending);

		old_rb = event->rb;
		spin_lock_irqsave(&old_rb->event_lock, flags);
		list_del_rcu(&event->rb_entry);
		spin_unlock_irqrestore(&old_rb->event_lock, flags);

		event->rcu_batches = get_state_synchronize_rcu();
		event->rcu_pending = 1;
	}

	if (rb) {
		if (event->rcu_pending) {
			cond_synchronize_rcu(event->rcu_batches);
			event->rcu_pending = 0;
		}

		spin_lock_irqsave(&rb->event_lock, flags);
		list_add_rcu(&event->rb_entry, &rb->event_list);
		spin_unlock_irqrestore(&rb->event_lock, flags);
	}

	/*
	 * Avoid racing with perf_mmap_close(AUX): stop the event
	 * before swizzling the event::rb pointer; if it's getting
	 * unmapped, its aux_mmap_count will be 0 and it won't
	 * restart. See the comment in __perf_pmu_output_stop().
	 *
	 * Data will inevitably be lost when set_output is done in
	 * mid-air, but then again, whoever does it like this is
	 * not in for the data anyway.
	 */
	if (has_aux(event))
		perf_event_stop(event, 0);

	rcu_assign_pointer(event->rb, rb);

	if (old_rb) {
		ring_buffer_put(old_rb);
		/*
		 * Since we detached before setting the new rb, so that we
		 * could attach the new rb, we could have missed a wakeup.
		 * Provide it now.
		 */
		wake_up_all(&event->waitq);
	}
}

static void ring_buffer_wakeup(struct perf_event *event)
{
	struct perf_buffer *rb;

	if (event->parent)
		event = event->parent;

	rcu_read_lock();
	rb = rcu_dereference(event->rb);
	if (rb) {
		list_for_each_entry_rcu(event, &rb->event_list, rb_entry)
			wake_up_all(&event->waitq);
	}
	rcu_read_unlock();
}

struct perf_buffer *ring_buffer_get(struct perf_event *event)
{
	struct perf_buffer *rb;

	if (event->parent)
		event = event->parent;

	rcu_read_lock();
	rb = rcu_dereference(event->rb);
	if (rb) {
		if (!refcount_inc_not_zero(&rb->refcount))
			rb = NULL;
	}
	rcu_read_unlock();

	return rb;
}

void ring_buffer_put(struct perf_buffer *rb)
{
	if (!refcount_dec_and_test(&rb->refcount))
		return;

	WARN_ON_ONCE(!list_empty(&rb->event_list));

	call_rcu(&rb->rcu_head, rb_free_rcu);
}

static void perf_mmap_open(struct vm_area_struct *vma)
{
	struct perf_event *event = vma->vm_file->private_data;

	atomic_inc(&event->mmap_count);
	atomic_inc(&event->rb->mmap_count);

	if (vma->vm_pgoff)
		atomic_inc(&event->rb->aux_mmap_count);

	if (event->pmu->event_mapped)
		event->pmu->event_mapped(event, vma->vm_mm);
}

static void perf_pmu_output_stop(struct perf_event *event);

/*
 * A buffer can be mmap()ed multiple times; either directly through the same
 * event, or through other events by use of perf_event_set_output().
 *
 * In order to undo the VM accounting done by perf_mmap() we need to destroy
 * the buffer here, where we still have a VM context. This means we need
 * to detach all events redirecting to us.
 */
static void perf_mmap_close(struct vm_area_struct *vma)
{
	struct perf_event *event = vma->vm_file->private_data;
	struct perf_buffer *rb = ring_buffer_get(event);
	struct user_struct *mmap_user = rb->mmap_user;
	int mmap_locked = rb->mmap_locked;
	unsigned long size = perf_data_size(rb);
	bool detach_rest = false;

	if (event->pmu->event_unmapped)
		event->pmu->event_unmapped(event, vma->vm_mm);

	/*
	 * The AUX buffer is strictly a sub-buffer, serialize using aux_mutex
	 * to avoid complications.
	 */
	if (rb_has_aux(rb) && vma->vm_pgoff == rb->aux_pgoff &&
	    atomic_dec_and_mutex_lock(&rb->aux_mmap_count, &rb->aux_mutex)) {
		/*
		 * Stop all AUX events that are writing to this buffer,
		 * so that we can free its AUX pages and corresponding PMU
		 * data. Note that after rb::aux_mmap_count dropped to zero,
		 * they won't start any more (see perf_aux_output_begin()).
		 */
		perf_pmu_output_stop(event);

		/* now it's safe to free the pages */
		atomic_long_sub(rb->aux_nr_pages - rb->aux_mmap_locked, &mmap_user->locked_vm);
		atomic64_sub(rb->aux_mmap_locked, &vma->vm_mm->pinned_vm);

		/* this has to be the last one */
		rb_free_aux(rb);
		WARN_ON_ONCE(refcount_read(&rb->aux_refcount));

		mutex_unlock(&rb->aux_mutex);
	}

	if (atomic_dec_and_test(&rb->mmap_count))
		detach_rest = true;

	if (!atomic_dec_and_mutex_lock(&event->mmap_count, &event->mmap_mutex))
		goto out_put;

	ring_buffer_attach(event, NULL);
	mutex_unlock(&event->mmap_mutex);

	/* If there's still other mmap()s of this buffer, we're done. */
	if (!detach_rest)
		goto out_put;

	/*
	 * No other mmap()s, detach from all other events that might redirect
	 * into the now unreachable buffer. Somewhat complicated by the
	 * fact that rb::event_lock otherwise nests inside mmap_mutex.
	 */
again:
	rcu_read_lock();
	list_for_each_entry_rcu(event, &rb->event_list, rb_entry) {
		if (!atomic_long_inc_not_zero(&event->refcount)) {
			/*
			 * This event is en-route to free_event() which will
			 * detach it and remove it from the list.
			 */
			continue;
		}
		rcu_read_unlock();

		mutex_lock(&event->mmap_mutex);
		/*
		 * Check we didn't race with perf_event_set_output() which can
		 * swizzle the rb from under us while we were waiting to
		 * acquire mmap_mutex.
		 *
		 * If we find a different rb; ignore this event, a next
		 * iteration will no longer find it on the list. We have to
		 * still restart the iteration to make sure we're not now
		 * iterating the wrong list.
		 */
		if (event->rb == rb)
			ring_buffer_attach(event, NULL);

		mutex_unlock(&event->mmap_mutex);
		put_event(event);

		/*
		 * Restart the iteration; either we're on the wrong list or
		 * destroyed its integrity by doing a deletion.
		 */
		goto again;
	}
	rcu_read_unlock();

	/*
	 * It could be there's still a few 0-ref events on the list; they'll
	 * get cleaned up by free_event() -- they'll also still have their
	 * ref on the rb and will free it whenever they are done with it.
	 *
	 * Aside from that, this buffer is 'fully' detached and unmapped,
	 * undo the VM accounting.
	 */

	atomic_long_sub((size >> PAGE_SHIFT) + 1 - mmap_locked,
			&mmap_user->locked_vm);
	atomic64_sub(mmap_locked, &vma->vm_mm->pinned_vm);
	free_uid(mmap_user);

out_put:
	ring_buffer_put(rb); /* could be last */
}

static vm_fault_t perf_mmap_pfn_mkwrite(struct vm_fault *vmf)
{
	/* The first page is the user control page, others are read-only. */
	return vmf->pgoff == 0 ? 0 : VM_FAULT_SIGBUS;
}

static const struct vm_operations_struct perf_mmap_vmops = {
	.open		= perf_mmap_open,
	.close		= perf_mmap_close, /* non mergeable */
	.pfn_mkwrite	= perf_mmap_pfn_mkwrite,
};

static int map_range(struct perf_buffer *rb, struct vm_area_struct *vma)
{
	unsigned long nr_pages = vma_pages(vma);
	int err = 0;
	unsigned long pagenum;

	/*
	 * We map this as a VM_PFNMAP VMA.
	 *
	 * This is not ideal as this is designed broadly for mappings of PFNs
	 * referencing memory-mapped I/O ranges or non-system RAM i.e. for which
	 * !pfn_valid(pfn).
	 *
	 * We are mapping kernel-allocated memory (memory we manage ourselves)
	 * which would more ideally be mapped using vm_insert_page() or a
	 * similar mechanism, that is as a VM_MIXEDMAP mapping.
	 *
	 * However this won't work here, because:
	 *
	 * 1. It uses vma->vm_page_prot, but this field has not been completely
	 *    setup at the point of the f_op->mmp() hook, so we are unable to
	 *    indicate that this should be mapped CoW in order that the
	 *    mkwrite() hook can be invoked to make the first page R/W and the
	 *    rest R/O as desired.
	 *
	 * 2. Anything other than a VM_PFNMAP of valid PFNs will result in
	 *    vm_normal_page() returning a struct page * pointer, which means
	 *    vm_ops->page_mkwrite() will be invoked rather than
	 *    vm_ops->pfn_mkwrite(), and this means we have to set page->mapping
	 *    to work around retry logic in the fault handler, however this
	 *    field is no longer allowed to be used within struct page.
	 *
	 * 3. Having a struct page * made available in the fault logic also
	 *    means that the page gets put on the rmap and becomes
	 *    inappropriately accessible and subject to map and ref counting.
	 *
	 * Ideally we would have a mechanism that could explicitly express our
	 * desires, but this is not currently the case, so we instead use
	 * VM_PFNMAP.
	 *
	 * We manage the lifetime of these mappings with internal refcounts (see
	 * perf_mmap_open() and perf_mmap_close()) so we ensure the lifetime of
	 * this mapping is maintained correctly.
	 */
	for (pagenum = 0; pagenum < nr_pages; pagenum++) {
		unsigned long va = vma->vm_start + PAGE_SIZE * pagenum;
		struct page *page = perf_mmap_to_page(rb, vma->vm_pgoff + pagenum);

		if (page == NULL) {
			err = -EINVAL;
			break;
		}

		/* Map readonly, perf_mmap_pfn_mkwrite() called on write fault. */
		err = remap_pfn_range(vma, va, page_to_pfn(page), PAGE_SIZE,
				      vm_get_page_prot(vma->vm_flags & ~VM_SHARED));
		if (err)
			break;
	}

#ifdef CONFIG_MMU
	/* Clear any partial mappings on error. */
	if (err)
		zap_page_range_single(vma, vma->vm_start, nr_pages * PAGE_SIZE, NULL);
#endif

	return err;
}

static int perf_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct perf_event *event = file->private_data;
	unsigned long user_locked, user_lock_limit;
	struct user_struct *user = current_user();
	struct mutex *aux_mutex = NULL;
	struct perf_buffer *rb = NULL;
	unsigned long locked, lock_limit;
	unsigned long vma_size;
	unsigned long nr_pages;
	long user_extra = 0, extra = 0;
	int ret, flags = 0;

	/*
	 * Don't allow mmap() of inherited per-task counters. This would
	 * create a performance issue due to all children writing to the
	 * same rb.
	 */
	if (event->cpu == -1 && event->attr.inherit)
		return -EINVAL;

	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;

	ret = security_perf_event_read(event);
	if (ret)
		return ret;

	vma_size = vma->vm_end - vma->vm_start;
	nr_pages = vma_size / PAGE_SIZE;

	if (nr_pages > INT_MAX)
		return -ENOMEM;

	if (vma_size != PAGE_SIZE * nr_pages)
		return -EINVAL;

	user_extra = nr_pages;

	mutex_lock(&event->mmap_mutex);
	ret = -EINVAL;

	if (vma->vm_pgoff == 0) {
		nr_pages -= 1;

		/*
		 * If we have rb pages ensure they're a power-of-two number, so we
		 * can do bitmasks instead of modulo.
		 */
		if (nr_pages != 0 && !is_power_of_2(nr_pages))
			goto unlock;

		WARN_ON_ONCE(event->ctx->parent_ctx);

		if (event->rb) {
			if (data_page_nr(event->rb) != nr_pages)
				goto unlock;

			if (atomic_inc_not_zero(&event->rb->mmap_count)) {
				/*
				 * Success -- managed to mmap() the same buffer
				 * multiple times.
				 */
				ret = 0;
				/* We need the rb to map pages. */
				rb = event->rb;
				goto unlock;
			}

			/*
			 * Raced against perf_mmap_close()'s
			 * atomic_dec_and_mutex_lock() remove the
			 * event and continue as if !event->rb
			 */
			ring_buffer_attach(event, NULL);
		}

	} else {
		/*
		 * AUX area mapping: if rb->aux_nr_pages != 0, it's already
		 * mapped, all subsequent mappings should have the same size
		 * and offset. Must be above the normal perf buffer.
		 */
		u64 aux_offset, aux_size;

		rb = event->rb;
		if (!rb)
			goto aux_unlock;

		aux_mutex = &rb->aux_mutex;
		mutex_lock(aux_mutex);

		aux_offset = READ_ONCE(rb->user_page->aux_offset);
		aux_size = READ_ONCE(rb->user_page->aux_size);

		if (aux_offset < perf_data_size(rb) + PAGE_SIZE)
			goto aux_unlock;

		if (aux_offset != vma->vm_pgoff << PAGE_SHIFT)
			goto aux_unlock;

		/* already mapped with a different offset */
		if (rb_has_aux(rb) && rb->aux_pgoff != vma->vm_pgoff)
			goto aux_unlock;

		if (aux_size != vma_size || aux_size != nr_pages * PAGE_SIZE)
			goto aux_unlock;

		/* already mapped with a different size */
		if (rb_has_aux(rb) && rb->aux_nr_pages != nr_pages)
			goto aux_unlock;

		if (!is_power_of_2(nr_pages))
			goto aux_unlock;

		if (!atomic_inc_not_zero(&rb->mmap_count))
			goto aux_unlock;

		if (rb_has_aux(rb)) {
			atomic_inc(&rb->aux_mmap_count);
			ret = 0;
			goto unlock;
		}

		atomic_set(&rb->aux_mmap_count, 1);
	}

	user_lock_limit = sysctl_perf_event_mlock >> (PAGE_SHIFT - 10);

	/*
	 * Increase the limit linearly with more CPUs:
	 */
	user_lock_limit *= num_online_cpus();

	user_locked = atomic_long_read(&user->locked_vm);

	/*
	 * sysctl_perf_event_mlock may have changed, so that
	 *     user->locked_vm > user_lock_limit
	 */
	if (user_locked > user_lock_limit)
		user_locked = user_lock_limit;
	user_locked += user_extra;

	if (user_locked > user_lock_limit) {
		/*
		 * charge locked_vm until it hits user_lock_limit;
		 * charge the rest from pinned_vm
		 */
		extra = user_locked - user_lock_limit;
		user_extra -= extra;
	}

	lock_limit = rlimit(RLIMIT_MEMLOCK);
	lock_limit >>= PAGE_SHIFT;
	locked = atomic64_read(&vma->vm_mm->pinned_vm) + extra;

	if ((locked > lock_limit) && perf_is_paranoid() &&
		!capable(CAP_IPC_LOCK)) {
		ret = -EPERM;
		goto unlock;
	}

	WARN_ON(!rb && event->rb);

	if (vma->vm_flags & VM_WRITE)
		flags |= RING_BUFFER_WRITABLE;

	if (!rb) {
		rb = rb_alloc(nr_pages,
			      event->attr.watermark ? event->attr.wakeup_watermark : 0,
			      event->cpu, flags);

		if (!rb) {
			ret = -ENOMEM;
			goto unlock;
		}

		atomic_set(&rb->mmap_count, 1);
		rb->mmap_user = get_current_user();
		rb->mmap_locked = extra;

		ring_buffer_attach(event, rb);

		perf_event_update_time(event);
		perf_event_init_userpage(event);
		perf_event_update_userpage(event);
	} else {
		ret = rb_alloc_aux(rb, event, vma->vm_pgoff, nr_pages,
				   event->attr.aux_watermark, flags);
		if (!ret)
			rb->aux_mmap_locked = extra;
	}

	ret = 0;

unlock:
	if (!ret) {
		atomic_long_add(user_extra, &user->locked_vm);
		atomic64_add(extra, &vma->vm_mm->pinned_vm);

		atomic_inc(&event->mmap_count);
	} else if (rb) {
		atomic_dec(&rb->mmap_count);
	}
aux_unlock:
	if (aux_mutex)
		mutex_unlock(aux_mutex);
	mutex_unlock(&event->mmap_mutex);

	/*
	 * Since pinned accounting is per vm we cannot allow fork() to copy our
	 * vma.
	 */
	vm_flags_set(vma, VM_DONTCOPY | VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_ops = &perf_mmap_vmops;

	if (!ret)
		ret = map_range(rb, vma);

	if (!ret && event->pmu->event_mapped)
		event->pmu->event_mapped(event, vma->vm_mm);

	return ret;
}

static int perf_fasync(int fd, struct file *filp, int on)
{
	struct inode *inode = file_inode(filp);
	struct perf_event *event = filp->private_data;
	int retval;

	inode_lock(inode);
	retval = fasync_helper(fd, filp, on, &event->fasync);
	inode_unlock(inode);

	if (retval < 0)
		return retval;

	return 0;
}

static const struct file_operations perf_fops = {
	.release		= perf_release,
	.read			= perf_read,
	.poll			= perf_poll,
	.unlocked_ioctl		= perf_ioctl,
	.compat_ioctl		= perf_compat_ioctl,
	.mmap			= perf_mmap,
	.fasync			= perf_fasync,
};

/*
 * Perf event wakeup
 *
 * If there's data, ensure we set the poll() state and publish everything
 * to user-space before waking everybody up.
 */

void perf_event_wakeup(struct perf_event *event)
{
	ring_buffer_wakeup(event);

	if (event->pending_kill) {
		kill_fasync(perf_event_fasync(event), SIGIO, event->pending_kill);
		event->pending_kill = 0;
	}
}

static void perf_sigtrap(struct perf_event *event)
{
	/*
	 * We'd expect this to only occur if the irq_work is delayed and either
	 * ctx->task or current has changed in the meantime. This can be the
	 * case on architectures that do not implement arch_irq_work_raise().
	 */
	if (WARN_ON_ONCE(event->ctx->task != current))
		return;

	/*
	 * Both perf_pending_task() and perf_pending_irq() can race with the
	 * task exiting.
	 */
	if (current->flags & PF_EXITING)
		return;

	send_sig_perf((void __user *)event->pending_addr,
		      event->orig_type, event->attr.sig_data);
}

/*
 * Deliver the pending work in-event-context or follow the context.
 */
static void __perf_pending_disable(struct perf_event *event)
{
	int cpu = READ_ONCE(event->oncpu);

	/*
	 * If the event isn't running; we done. event_sched_out() will have
	 * taken care of things.
	 */
	if (cpu < 0)
		return;

	/*
	 * Yay, we hit home and are in the context of the event.
	 */
	if (cpu == smp_processor_id()) {
		if (event->pending_disable) {
			event->pending_disable = 0;
			perf_event_disable_local(event);
		}
		return;
	}

	/*
	 *  CPU-A			CPU-B
	 *
	 *  perf_event_disable_inatomic()
	 *    @pending_disable = CPU-A;
	 *    irq_work_queue();
	 *
	 *  sched-out
	 *    @pending_disable = -1;
	 *
	 *				sched-in
	 *				perf_event_disable_inatomic()
	 *				  @pending_disable = CPU-B;
	 *				  irq_work_queue(); // FAILS
	 *
	 *  irq_work_run()
	 *    perf_pending_disable()
	 *
	 * But the event runs on CPU-B and wants disabling there.
	 */
	irq_work_queue_on(&event->pending_disable_irq, cpu);
}

static void perf_pending_disable(struct irq_work *entry)
{
	struct perf_event *event = container_of(entry, struct perf_event, pending_disable_irq);
	int rctx;

	/*
	 * If we 'fail' here, that's OK, it means recursion is already disabled
	 * and we won't recurse 'further'.
	 */
	rctx = perf_swevent_get_recursion_context();
	__perf_pending_disable(event);
	if (rctx >= 0)
		perf_swevent_put_recursion_context(rctx);
}

static void perf_pending_irq(struct irq_work *entry)
{
	struct perf_event *event = container_of(entry, struct perf_event, pending_irq);
	int rctx;

	/*
	 * If we 'fail' here, that's OK, it means recursion is already disabled
	 * and we won't recurse 'further'.
	 */
	rctx = perf_swevent_get_recursion_context();

	/*
	 * The wakeup isn't bound to the context of the event -- it can happen
	 * irrespective of where the event is.
	 */
	if (event->pending_wakeup) {
		event->pending_wakeup = 0;
		perf_event_wakeup(event);
	}

	if (rctx >= 0)
		perf_swevent_put_recursion_context(rctx);
}

static void perf_pending_task(struct callback_head *head)
{
	struct perf_event *event = container_of(head, struct perf_event, pending_task);
	int rctx;

	/*
	 * If we 'fail' here, that's OK, it means recursion is already disabled
	 * and we won't recurse 'further'.
	 */
	rctx = perf_swevent_get_recursion_context();

	if (event->pending_work) {
		event->pending_work = 0;
		perf_sigtrap(event);
		local_dec(&event->ctx->nr_no_switch_fast);
	}
	put_event(event);

	if (rctx >= 0)
		perf_swevent_put_recursion_context(rctx);
}

#ifdef CONFIG_GUEST_PERF_EVENTS
struct perf_guest_info_callbacks __rcu *perf_guest_cbs;

DEFINE_STATIC_CALL_RET0(__perf_guest_state, *perf_guest_cbs->state);
DEFINE_STATIC_CALL_RET0(__perf_guest_get_ip, *perf_guest_cbs->get_ip);
DEFINE_STATIC_CALL_RET0(__perf_guest_handle_intel_pt_intr, *perf_guest_cbs->handle_intel_pt_intr);

void perf_register_guest_info_callbacks(struct perf_guest_info_callbacks *cbs)
{
	if (WARN_ON_ONCE(rcu_access_pointer(perf_guest_cbs)))
		return;

	rcu_assign_pointer(perf_guest_cbs, cbs);
	static_call_update(__perf_guest_state, cbs->state);
	static_call_update(__perf_guest_get_ip, cbs->get_ip);

	/* Implementing ->handle_intel_pt_intr is optional. */
	if (cbs->handle_intel_pt_intr)
		static_call_update(__perf_guest_handle_intel_pt_intr,
				   cbs->handle_intel_pt_intr);
}
EXPORT_SYMBOL_GPL(perf_register_guest_info_callbacks);

void perf_unregister_guest_info_callbacks(struct perf_guest_info_callbacks *cbs)
{
	if (WARN_ON_ONCE(rcu_access_pointer(perf_guest_cbs) != cbs))
		return;

	rcu_assign_pointer(perf_guest_cbs, NULL);
	static_call_update(__perf_guest_state, (void *)&__static_call_return0);
	static_call_update(__perf_guest_get_ip, (void *)&__static_call_return0);
	static_call_update(__perf_guest_handle_intel_pt_intr,
			   (void *)&__static_call_return0);
	synchronize_rcu();
}
EXPORT_SYMBOL_GPL(perf_unregister_guest_info_callbacks);
#endif

static bool should_sample_guest(struct perf_event *event)
{
	return !event->attr.exclude_guest && perf_guest_state();
}

unsigned long perf_misc_flags(struct perf_event *event,
			      struct pt_regs *regs)
{
	if (should_sample_guest(event))
		return perf_arch_guest_misc_flags(regs);

	return perf_arch_misc_flags(regs);
}

unsigned long perf_instruction_pointer(struct perf_event *event,
				       struct pt_regs *regs)
{
	if (should_sample_guest(event))
		return perf_guest_get_ip();

	return perf_arch_instruction_pointer(regs);
}

static void
perf_output_sample_regs(struct perf_output_handle *handle,
			struct pt_regs *regs, u64 mask)
{
	int bit;
	DECLARE_BITMAP(_mask, 64);

	bitmap_from_u64(_mask, mask);
	for_each_set_bit(bit, _mask, sizeof(mask) * BITS_PER_BYTE) {
		u64 val;

		val = perf_reg_value(regs, bit);
		perf_output_put(handle, val);
	}
}

static void perf_sample_regs_user(struct perf_regs *regs_user,
				  struct pt_regs *regs)
{
	if (user_mode(regs)) {
		regs_user->abi = perf_reg_abi(current);
		regs_user->regs = regs;
	} else if (!(current->flags & PF_KTHREAD)) {
		perf_get_regs_user(regs_user, regs);
	} else {
		regs_user->abi = PERF_SAMPLE_REGS_ABI_NONE;
		regs_user->regs = NULL;
	}
}

static void perf_sample_regs_intr(struct perf_regs *regs_intr,
				  struct pt_regs *regs)
{
	regs_intr->regs = regs;
	regs_intr->abi  = perf_reg_abi(current);
}


/*
 * Get remaining task size from user stack pointer.
 *
 * It'd be better to take stack vma map and limit this more
 * precisely, but there's no way to get it safely under interrupt,
 * so using TASK_SIZE as limit.
 */
static u64 perf_ustack_task_size(struct pt_regs *regs)
{
	unsigned long addr = perf_user_stack_pointer(regs);

	if (!addr || addr >= TASK_SIZE)
		return 0;

	return TASK_SIZE - addr;
}

static u16
perf_sample_ustack_size(u16 stack_size, u16 header_size,
			struct pt_regs *regs)
{
	u64 task_size;

	/* No regs, no stack pointer, no dump. */
	if (!regs)
		return 0;

	/* No mm, no stack, no dump. */
	if (!current->mm)
		return 0;

	/*
	 * Check if we fit in with the requested stack size into the:
	 * - TASK_SIZE
	 *   If we don't, we limit the size to the TASK_SIZE.
	 *
	 * - remaining sample size
	 *   If we don't, we customize the stack size to
	 *   fit in to the remaining sample size.
	 */

	task_size  = min((u64) USHRT_MAX, perf_ustack_task_size(regs));
	stack_size = min(stack_size, (u16) task_size);

	/* Current header size plus static size and dynamic size. */
	header_size += 2 * sizeof(u64);

	/* Do we fit in with the current stack dump size? */
	if ((u16) (header_size + stack_size) < header_size) {
		/*
		 * If we overflow the maximum size for the sample,
		 * we customize the stack dump size to fit in.
		 */
		stack_size = USHRT_MAX - header_size - sizeof(u64);
		stack_size = round_up(stack_size, sizeof(u64));
	}

	return stack_size;
}

static void
perf_output_sample_ustack(struct perf_output_handle *handle, u64 dump_size,
			  struct pt_regs *regs)
{
	/* Case of a kernel thread, nothing to dump */
	if (!regs) {
		u64 size = 0;
		perf_output_put(handle, size);
	} else {
		unsigned long sp;
		unsigned int rem;
		u64 dyn_size;

		/*
		 * We dump:
		 * static size
		 *   - the size requested by user or the best one we can fit
		 *     in to the sample max size
		 * data
		 *   - user stack dump data
		 * dynamic size
		 *   - the actual dumped size
		 */

		/* Static size. */
		perf_output_put(handle, dump_size);

		/* Data. */
		sp = perf_user_stack_pointer(regs);
		rem = __output_copy_user(handle, (void *) sp, dump_size);
		dyn_size = dump_size - rem;

		perf_output_skip(handle, rem);

		/* Dynamic size. */
		perf_output_put(handle, dyn_size);
	}
}

static unsigned long perf_prepare_sample_aux(struct perf_event *event,
					  struct perf_sample_data *data,
					  size_t size)
{
	struct perf_event *sampler = event->aux_event;
	struct perf_buffer *rb;

	data->aux_size = 0;

	if (!sampler)
		goto out;

	if (WARN_ON_ONCE(READ_ONCE(sampler->state) != PERF_EVENT_STATE_ACTIVE))
		goto out;

	if (WARN_ON_ONCE(READ_ONCE(sampler->oncpu) != smp_processor_id()))
		goto out;

	rb = ring_buffer_get(sampler);
	if (!rb)
		goto out;

	/*
	 * If this is an NMI hit inside sampling code, don't take
	 * the sample. See also perf_aux_sample_output().
	 */
	if (READ_ONCE(rb->aux_in_sampling)) {
		data->aux_size = 0;
	} else {
		size = min_t(size_t, size, perf_aux_size(rb));
		data->aux_size = ALIGN(size, sizeof(u64));
	}
	ring_buffer_put(rb);

out:
	return data->aux_size;
}

static long perf_pmu_snapshot_aux(struct perf_buffer *rb,
                                 struct perf_event *event,
                                 struct perf_output_handle *handle,
                                 unsigned long size)
{
	unsigned long flags;
	long ret;

	/*
	 * Normal ->start()/->stop() callbacks run in IRQ mode in scheduler
	 * paths. If we start calling them in NMI context, they may race with
	 * the IRQ ones, that is, for example, re-starting an event that's just
	 * been stopped, which is why we're using a separate callback that
	 * doesn't change the event state.
	 *
	 * IRQs need to be disabled to prevent IPIs from racing with us.
	 */
	local_irq_save(flags);
	/*
	 * Guard against NMI hits inside the critical section;
	 * see also perf_prepare_sample_aux().
	 */
	WRITE_ONCE(rb->aux_in_sampling, 1);
	barrier();

	ret = event->pmu->snapshot_aux(event, handle, size);

	barrier();
	WRITE_ONCE(rb->aux_in_sampling, 0);
	local_irq_restore(flags);

	return ret;
}

static void perf_aux_sample_output(struct perf_event *event,
				   struct perf_output_handle *handle,
				   struct perf_sample_data *data)
{
	struct perf_event *sampler = event->aux_event;
	struct perf_buffer *rb;
	unsigned long pad;
	long size;

	if (WARN_ON_ONCE(!sampler || !data->aux_size))
		return;

	rb = ring_buffer_get(sampler);
	if (!rb)
		return;

	size = perf_pmu_snapshot_aux(rb, sampler, handle, data->aux_size);

	/*
	 * An error here means that perf_output_copy() failed (returned a
	 * non-zero surplus that it didn't copy), which in its current
	 * enlightened implementation is not possible. If that changes, we'd
	 * like to know.
	 */
	if (WARN_ON_ONCE(size < 0))
		goto out_put;

	/*
	 * The pad comes from ALIGN()ing data->aux_size up to u64 in
	 * perf_prepare_sample_aux(), so should not be more than that.
	 */
	pad = data->aux_size - size;
	if (WARN_ON_ONCE(pad >= sizeof(u64)))
		pad = 8;

	if (pad) {
		u64 zero = 0;
		perf_output_copy(handle, &zero, pad);
	}

out_put:
	ring_buffer_put(rb);
}

/*
 * A set of common sample data types saved even for non-sample records
 * when event->attr.sample_id_all is set.
 */
#define PERF_SAMPLE_ID_ALL  (PERF_SAMPLE_TID | PERF_SAMPLE_TIME |	\
			     PERF_SAMPLE_ID | PERF_SAMPLE_STREAM_ID |	\
			     PERF_SAMPLE_CPU | PERF_SAMPLE_IDENTIFIER)

static void __perf_event_header__init_id(struct perf_sample_data *data,
					 struct perf_event *event,
					 u64 sample_type)
{
	data->type = event->attr.sample_type;
	data->sample_flags |= data->type & PERF_SAMPLE_ID_ALL;

	if (sample_type & PERF_SAMPLE_TID) {
		/* namespace issues */
		data->tid_entry.pid = perf_event_pid(event, current);
		data->tid_entry.tid = perf_event_tid(event, current);
	}

	if (sample_type & PERF_SAMPLE_TIME)
		data->time = perf_event_clock(event);

	if (sample_type & (PERF_SAMPLE_ID | PERF_SAMPLE_IDENTIFIER))
		data->id = primary_event_id(event);

	if (sample_type & PERF_SAMPLE_STREAM_ID)
		data->stream_id = event->id;

	if (sample_type & PERF_SAMPLE_CPU) {
		data->cpu_entry.cpu	 = raw_smp_processor_id();
		data->cpu_entry.reserved = 0;
	}
}

void perf_event_header__init_id(struct perf_event_header *header,
				struct perf_sample_data *data,
				struct perf_event *event)
{
	if (event->attr.sample_id_all) {
		header->size += event->id_header_size;
		__perf_event_header__init_id(data, event, event->attr.sample_type);
	}
}

static void __perf_event__output_id_sample(struct perf_output_handle *handle,
					   struct perf_sample_data *data)
{
	u64 sample_type = data->type;

	if (sample_type & PERF_SAMPLE_TID)
		perf_output_put(handle, data->tid_entry);

	if (sample_type & PERF_SAMPLE_TIME)
		perf_output_put(handle, data->time);

	if (sample_type & PERF_SAMPLE_ID)
		perf_output_put(handle, data->id);

	if (sample_type & PERF_SAMPLE_STREAM_ID)
		perf_output_put(handle, data->stream_id);

	if (sample_type & PERF_SAMPLE_CPU)
		perf_output_put(handle, data->cpu_entry);

	if (sample_type & PERF_SAMPLE_IDENTIFIER)
		perf_output_put(handle, data->id);
}

void perf_event__output_id_sample(struct perf_event *event,
				  struct perf_output_handle *handle,
				  struct perf_sample_data *sample)
{
	if (event->attr.sample_id_all)
		__perf_event__output_id_sample(handle, sample);
}

static void perf_output_read_one(struct perf_output_handle *handle,
				 struct perf_event *event,
				 u64 enabled, u64 running)
{
	u64 read_format = event->attr.read_format;
	u64 values[5];
	int n = 0;

	values[n++] = perf_event_count(event, has_inherit_and_sample_read(&event->attr));
	if (read_format & PERF_FORMAT_TOTAL_TIME_ENABLED) {
		values[n++] = enabled +
			atomic64_read(&event->child_total_time_enabled);
	}
	if (read_format & PERF_FORMAT_TOTAL_TIME_RUNNING) {
		values[n++] = running +
			atomic64_read(&event->child_total_time_running);
	}
	if (read_format & PERF_FORMAT_ID)
		values[n++] = primary_event_id(event);
	if (read_format & PERF_FORMAT_LOST)
		values[n++] = atomic64_read(&event->lost_samples);

	__output_copy(handle, values, n * sizeof(u64));
}

static void perf_output_read_group(struct perf_output_handle *handle,
				   struct perf_event *event,
				   u64 enabled, u64 running)
{
	struct perf_event *leader = event->group_leader, *sub;
	u64 read_format = event->attr.read_format;
	unsigned long flags;
	u64 values[6];
	int n = 0;
	bool self = has_inherit_and_sample_read(&event->attr);

	/*
	 * Disabling interrupts avoids all counter scheduling
	 * (context switches, timer based rotation and IPIs).
	 */
	local_irq_save(flags);

	values[n++] = 1 + leader->nr_siblings;

	if (read_format & PERF_FORMAT_TOTAL_TIME_ENABLED)
		values[n++] = enabled;

	if (read_format & PERF_FORMAT_TOTAL_TIME_RUNNING)
		values[n++] = running;

	if ((leader != event) && !handle->skip_read)
		perf_pmu_read(leader);

	values[n++] = perf_event_count(leader, self);
	if (read_format & PERF_FORMAT_ID)
		values[n++] = primary_event_id(leader);
	if (read_format & PERF_FORMAT_LOST)
		values[n++] = atomic64_read(&leader->lost_samples);

	__output_copy(handle, values, n * sizeof(u64));

	for_each_sibling_event(sub, leader) {
		n = 0;

		if ((sub != event) && !handle->skip_read)
			perf_pmu_read(sub);

		values[n++] = perf_event_count(sub, self);
		if (read_format & PERF_FORMAT_ID)
			values[n++] = primary_event_id(sub);
		if (read_format & PERF_FORMAT_LOST)
			values[n++] = atomic64_read(&sub->lost_samples);

		__output_copy(handle, values, n * sizeof(u64));
	}

	local_irq_restore(flags);
}

#define PERF_FORMAT_TOTAL_TIMES (PERF_FORMAT_TOTAL_TIME_ENABLED|\
				 PERF_FORMAT_TOTAL_TIME_RUNNING)

/*
 * XXX PERF_SAMPLE_READ vs inherited events seems difficult.
 *
 * The problem is that its both hard and excessively expensive to iterate the
 * child list, not to mention that its impossible to IPI the children running
 * on another CPU, from interrupt/NMI context.
 *
 * Instead the combination of PERF_SAMPLE_READ and inherit will track per-thread
 * counts rather than attempting to accumulate some value across all children on
 * all cores.
 */
static void perf_output_read(struct perf_output_handle *handle,
			     struct perf_event *event)
{
	u64 enabled = 0, running = 0, now;
	u64 read_format = event->attr.read_format;

	/*
	 * compute total_time_enabled, total_time_running
	 * based on snapshot values taken when the event
	 * was last scheduled in.
	 *
	 * we cannot simply called update_context_time()
	 * because of locking issue as we are called in
	 * NMI context
	 */
	if (read_format & PERF_FORMAT_TOTAL_TIMES)
		calc_timer_values(event, &now, &enabled, &running);

	if (event->attr.read_format & PERF_FORMAT_GROUP)
		perf_output_read_group(handle, event, enabled, running);
	else
		perf_output_read_one(handle, event, enabled, running);
}

void perf_output_sample(struct perf_output_handle *handle,
			struct perf_event_header *header,
			struct perf_sample_data *data,
			struct perf_event *event)
{
	u64 sample_type = data->type;

	if (data->sample_flags & PERF_SAMPLE_READ)
		handle->skip_read = 1;

	perf_output_put(handle, *header);

	if (sample_type & PERF_SAMPLE_IDENTIFIER)
		perf_output_put(handle, data->id);

	if (sample_type & PERF_SAMPLE_IP)
		perf_output_put(handle, data->ip);

	if (sample_type & PERF_SAMPLE_TID)
		perf_output_put(handle, data->tid_entry);

	if (sample_type & PERF_SAMPLE_TIME)
		perf_output_put(handle, data->time);

	if (sample_type & PERF_SAMPLE_ADDR)
		perf_output_put(handle, data->addr);

	if (sample_type & PERF_SAMPLE_ID)
		perf_output_put(handle, data->id);

	if (sample_type & PERF_SAMPLE_STREAM_ID)
		perf_output_put(handle, data->stream_id);

	if (sample_type & PERF_SAMPLE_CPU)
		perf_output_put(handle, data->cpu_entry);

	if (sample_type & PERF_SAMPLE_PERIOD)
		perf_output_put(handle, data->period);

	if (sample_type & PERF_SAMPLE_READ)
		perf_output_read(handle, event);

	if (sample_type & PERF_SAMPLE_CALLCHAIN) {
		int size = 1;

		size += data->callchain->nr;
		size *= sizeof(u64);
		__output_copy(handle, data->callchain, size);
	}

	if (sample_type & PERF_SAMPLE_RAW) {
		struct perf_raw_record *raw = data->raw;

		if (raw) {
			struct perf_raw_frag *frag = &raw->frag;

			perf_output_put(handle, raw->size);
			do {
				if (frag->copy) {
					__output_custom(handle, frag->copy,
							frag->data, frag->size);
				} else {
					__output_copy(handle, frag->data,
						      frag->size);
				}
				if (perf_raw_frag_last(frag))
					break;
				frag = frag->next;
			} while (1);
			if (frag->pad)
				__output_skip(handle, NULL, frag->pad);
		} else {
			struct {
				u32	size;
				u32	data;
			} raw = {
				.size = sizeof(u32),
				.data = 0,
			};
			perf_output_put(handle, raw);
		}
	}

	if (sample_type & PERF_SAMPLE_BRANCH_STACK) {
		if (data->br_stack) {
			size_t size;

			size = data->br_stack->nr
			     * sizeof(struct perf_branch_entry);

			perf_output_put(handle, data->br_stack->nr);
			if (branch_sample_hw_index(event))
				perf_output_put(handle, data->br_stack->hw_idx);
			perf_output_copy(handle, data->br_stack->entries, size);
			/*
			 * Add the extension space which is appended
			 * right after the struct perf_branch_stack.
			 */
			if (data->br_stack_cntr) {
				size = data->br_stack->nr * sizeof(u64);
				perf_output_copy(handle, data->br_stack_cntr, size);
			}
		} else {
			/*
			 * we always store at least the value of nr
			 */
			u64 nr = 0;
			perf_output_put(handle, nr);
		}
	}

	if (sample_type & PERF_SAMPLE_REGS_USER) {
		u64 abi = data->regs_user.abi;

		/*
		 * If there are no regs to dump, notice it through
		 * first u64 being zero (PERF_SAMPLE_REGS_ABI_NONE).
		 */
		perf_output_put(handle, abi);

		if (abi) {
			u64 mask = event->attr.sample_regs_user;
			perf_output_sample_regs(handle,
						data->regs_user.regs,
						mask);
		}
	}

	if (sample_type & PERF_SAMPLE_STACK_USER) {
		perf_output_sample_ustack(handle,
					  data->stack_user_size,
					  data->regs_user.regs);
	}

	if (sample_type & PERF_SAMPLE_WEIGHT_TYPE)
		perf_output_put(handle, data->weight.full);

	if (sample_type & PERF_SAMPLE_DATA_SRC)
		perf_output_put(handle, data->data_src.val);

	if (sample_type & PERF_SAMPLE_TRANSACTION)
		perf_output_put(handle, data->txn);

	if (sample_type & PERF_SAMPLE_REGS_INTR) {
		u64 abi = data->regs_intr.abi;
		/*
		 * If there are no regs to dump, notice it through
		 * first u64 being zero (PERF_SAMPLE_REGS_ABI_NONE).
		 */
		perf_output_put(handle, abi);

		if (abi) {
			u64 mask = event->attr.sample_regs_intr;

			perf_output_sample_regs(handle,
						data->regs_intr.regs,
						mask);
		}
	}

	if (sample_type & PERF_SAMPLE_PHYS_ADDR)
		perf_output_put(handle, data->phys_addr);

	if (sample_type & PERF_SAMPLE_CGROUP)
		perf_output_put(handle, data->cgroup);

	if (sample_type & PERF_SAMPLE_DATA_PAGE_SIZE)
		perf_output_put(handle, data->data_page_size);

	if (sample_type & PERF_SAMPLE_CODE_PAGE_SIZE)
		perf_output_put(handle, data->code_page_size);

	if (sample_type & PERF_SAMPLE_AUX) {
		perf_output_put(handle, data->aux_size);

		if (data->aux_size)
			perf_aux_sample_output(event, handle, data);
	}

	if (!event->attr.watermark) {
		int wakeup_events = event->attr.wakeup_events;

		if (wakeup_events) {
			struct perf_buffer *rb = handle->rb;
			int events = local_inc_return(&rb->events);

			if (events >= wakeup_events) {
				local_sub(wakeup_events, &rb->events);
				local_inc(&rb->wakeup);
			}
		}
	}
}

static u64 perf_virt_to_phys(u64 virt)
{
	u64 phys_addr = 0;

	if (!virt)
		return 0;

	if (virt >= TASK_SIZE) {
		/* If it's vmalloc()d memory, leave phys_addr as 0 */
		if (virt_addr_valid((void *)(uintptr_t)virt) &&
		    !(virt >= VMALLOC_START && virt < VMALLOC_END))
			phys_addr = (u64)virt_to_phys((void *)(uintptr_t)virt);
	} else {
		/*
		 * Walking the pages tables for user address.
		 * Interrupts are disabled, so it prevents any tear down
		 * of the page tables.
		 * Try IRQ-safe get_user_page_fast_only first.
		 * If failed, leave phys_addr as 0.
		 */
		if (current->mm != NULL) {
			struct page *p;

			pagefault_disable();
			if (get_user_page_fast_only(virt, 0, &p)) {
				phys_addr = page_to_phys(p) + virt % PAGE_SIZE;
				put_page(p);
			}
			pagefault_enable();
		}
	}

	return phys_addr;
}

/*
 * Return the pagetable size of a given virtual address.
 */
static u64 perf_get_pgtable_size(struct mm_struct *mm, unsigned long addr)
{
	u64 size = 0;

#ifdef CONFIG_HAVE_GUP_FAST
	pgd_t *pgdp, pgd;
	p4d_t *p4dp, p4d;
	pud_t *pudp, pud;
	pmd_t *pmdp, pmd;
	pte_t *ptep, pte;

	pgdp = pgd_offset(mm, addr);
	pgd = READ_ONCE(*pgdp);
	if (pgd_none(pgd))
		return 0;

	if (pgd_leaf(pgd))
		return pgd_leaf_size(pgd);

	p4dp = p4d_offset_lockless(pgdp, pgd, addr);
	p4d = READ_ONCE(*p4dp);
	if (!p4d_present(p4d))
		return 0;

	if (p4d_leaf(p4d))
		return p4d_leaf_size(p4d);

	pudp = pud_offset_lockless(p4dp, p4d, addr);
	pud = READ_ONCE(*pudp);
	if (!pud_present(pud))
		return 0;

	if (pud_leaf(pud))
		return pud_leaf_size(pud);

	pmdp = pmd_offset_lockless(pudp, pud, addr);
again:
	pmd = pmdp_get_lockless(pmdp);
	if (!pmd_present(pmd))
		return 0;

	if (pmd_leaf(pmd))
		return pmd_leaf_size(pmd);

	ptep = pte_offset_map(&pmd, addr);
	if (!ptep)
		goto again;

	pte = ptep_get_lockless(ptep);
	if (pte_present(pte))
		size = __pte_leaf_size(pmd, pte);
	pte_unmap(ptep);
#endif /* CONFIG_HAVE_GUP_FAST */

	return size;
}

static u64 perf_get_page_size(unsigned long addr)
{
	struct mm_struct *mm;
	unsigned long flags;
	u64 size;

	if (!addr)
		return 0;

	/*
	 * Software page-table walkers must disable IRQs,
	 * which prevents any tear down of the page tables.
	 */
	local_irq_save(flags);

	mm = current->mm;
	if (!mm) {
		/*
		 * For kernel threads and the like, use init_mm so that
		 * we can find kernel memory.
		 */
		mm = &init_mm;
	}

	size = perf_get_pgtable_size(mm, addr);

	local_irq_restore(flags);

	return size;
}

static struct perf_callchain_entry __empty_callchain = { .nr = 0, };

struct perf_callchain_entry *
perf_callchain(struct perf_event *event, struct pt_regs *regs)
{
	bool kernel = !event->attr.exclude_callchain_kernel;
	bool user   = !event->attr.exclude_callchain_user;
	/* Disallow cross-task user callchains. */
	bool crosstask = event->ctx->task && event->ctx->task != current;
	const u32 max_stack = event->attr.sample_max_stack;
	struct perf_callchain_entry *callchain;

	if (!current->mm)
		user = false;

	if (!kernel && !user)
		return &__empty_callchain;

	callchain = get_perf_callchain(regs, 0, kernel, user,
				       max_stack, crosstask, true);
	return callchain ?: &__empty_callchain;
}

static __always_inline u64 __cond_set(u64 flags, u64 s, u64 d)
{
	return d * !!(flags & s);
}

void perf_prepare_sample(struct perf_sample_data *data,
			 struct perf_event *event,
			 struct pt_regs *regs)
{
	u64 sample_type = event->attr.sample_type;
	u64 filtered_sample_type;

	/*
	 * Add the sample flags that are dependent to others.  And clear the
	 * sample flags that have already been done by the PMU driver.
	 */
	filtered_sample_type = sample_type;
	filtered_sample_type |= __cond_set(sample_type, PERF_SAMPLE_CODE_PAGE_SIZE,
					   PERF_SAMPLE_IP);
	filtered_sample_type |= __cond_set(sample_type, PERF_SAMPLE_DATA_PAGE_SIZE |
					   PERF_SAMPLE_PHYS_ADDR, PERF_SAMPLE_ADDR);
	filtered_sample_type |= __cond_set(sample_type, PERF_SAMPLE_STACK_USER,
					   PERF_SAMPLE_REGS_USER);
	filtered_sample_type &= ~data->sample_flags;

	if (filtered_sample_type == 0) {
		/* Make sure it has the correct data->type for output */
		data->type = event->attr.sample_type;
		return;
	}

	__perf_event_header__init_id(data, event, filtered_sample_type);

	if (filtered_sample_type & PERF_SAMPLE_IP) {
		data->ip = perf_instruction_pointer(event, regs);
		data->sample_flags |= PERF_SAMPLE_IP;
	}

	if (filtered_sample_type & PERF_SAMPLE_CALLCHAIN)
		perf_sample_save_callchain(data, event, regs);

	if (filtered_sample_type & PERF_SAMPLE_RAW) {
		data->raw = NULL;
		data->dyn_size += sizeof(u64);
		data->sample_flags |= PERF_SAMPLE_RAW;
	}

	if (filtered_sample_type & PERF_SAMPLE_BRANCH_STACK) {
		data->br_stack = NULL;
		data->dyn_size += sizeof(u64);
		data->sample_flags |= PERF_SAMPLE_BRANCH_STACK;
	}

	if (filtered_sample_type & PERF_SAMPLE_REGS_USER)
		perf_sample_regs_user(&data->regs_user, regs);

	/*
	 * It cannot use the filtered_sample_type here as REGS_USER can be set
	 * by STACK_USER (using __cond_set() above) and we don't want to update
	 * the dyn_size if it's not requested by users.
	 */
	if ((sample_type & ~data->sample_flags) & PERF_SAMPLE_REGS_USER) {
		/* regs dump ABI info */
		int size = sizeof(u64);

		if (data->regs_user.regs) {
			u64 mask = event->attr.sample_regs_user;
			size += hweight64(mask) * sizeof(u64);
		}

		data->dyn_size += size;
		data->sample_flags |= PERF_SAMPLE_REGS_USER;
	}

	if (filtered_sample_type & PERF_SAMPLE_STACK_USER) {
		/*
		 * Either we need PERF_SAMPLE_STACK_USER bit to be always
		 * processed as the last one or have additional check added
		 * in case new sample type is added, because we could eat
		 * up the rest of the sample size.
		 */
		u16 stack_size = event->attr.sample_stack_user;
		u16 header_size = perf_sample_data_size(data, event);
		u16 size = sizeof(u64);

		stack_size = perf_sample_ustack_size(stack_size, header_size,
						     data->regs_user.regs);

		/*
		 * If there is something to dump, add space for the dump
		 * itself and for the field that tells the dynamic size,
		 * which is how many have been actually dumped.
		 */
		if (stack_size)
			size += sizeof(u64) + stack_size;

		data->stack_user_size = stack_size;
		data->dyn_size += size;
		data->sample_flags |= PERF_SAMPLE_STACK_USER;
	}

	if (filtered_sample_type & PERF_SAMPLE_WEIGHT_TYPE) {
		data->weight.full = 0;
		data->sample_flags |= PERF_SAMPLE_WEIGHT_TYPE;
	}

	if (filtered_sample_type & PERF_SAMPLE_DATA_SRC) {
		data->data_src.val = PERF_MEM_NA;
		data->sample_flags |= PERF_SAMPLE_DATA_SRC;
	}

	if (filtered_sample_type & PERF_SAMPLE_TRANSACTION) {
		data->txn = 0;
		data->sample_flags |= PERF_SAMPLE_TRANSACTION;
	}

	if (filtered_sample_type & PERF_SAMPLE_ADDR) {
		data->addr = 0;
		data->sample_flags |= PERF_SAMPLE_ADDR;
	}

	if (filtered_sample_type & PERF_SAMPLE_REGS_INTR) {
		/* regs dump ABI info */
		int size = sizeof(u64);

		perf_sample_regs_intr(&data->regs_intr, regs);

		if (data->regs_intr.regs) {
			u64 mask = event->attr.sample_regs_intr;

			size += hweight64(mask) * sizeof(u64);
		}

		data->dyn_size += size;
		data->sample_flags |= PERF_SAMPLE_REGS_INTR;
	}

	if (filtered_sample_type & PERF_SAMPLE_PHYS_ADDR) {
		data->phys_addr = perf_virt_to_phys(data->addr);
		data->sample_flags |= PERF_SAMPLE_PHYS_ADDR;
	}

#ifdef CONFIG_CGROUP_PERF
	if (filtered_sample_type & PERF_SAMPLE_CGROUP) {
		struct cgroup *cgrp;

		/* protected by RCU */
		cgrp = task_css_check(current, perf_event_cgrp_id, 1)->cgroup;
		data->cgroup = cgroup_id(cgrp);
		data->sample_flags |= PERF_SAMPLE_CGROUP;
	}
#endif

	/*
	 * PERF_DATA_PAGE_SIZE requires PERF_SAMPLE_ADDR. If the user doesn't
	 * require PERF_SAMPLE_ADDR, kernel implicitly retrieve the data->addr,
	 * but the value will not dump to the userspace.
	 */
	if (filtered_sample_type & PERF_SAMPLE_DATA_PAGE_SIZE) {
		data->data_page_size = perf_get_page_size(data->addr);
		data->sample_flags |= PERF_SAMPLE_DATA_PAGE_SIZE;
	}

	if (filtered_sample_type & PERF_SAMPLE_CODE_PAGE_SIZE) {
		data->code_page_size = perf_get_page_size(data->ip);
		data->sample_flags |= PERF_SAMPLE_CODE_PAGE_SIZE;
	}

	if (filtered_sample_type & PERF_SAMPLE_AUX) {
		u64 size;
		u16 header_size = perf_sample_data_size(data, event);

		header_size += sizeof(u64); /* size */

		/*
		 * Given the 16bit nature of header::size, an AUX sample can
		 * easily overflow it, what with all the preceding sample bits.
		 * Make sure this doesn't happen by using up to U16_MAX bytes
		 * per sample in total (rounded down to 8 byte boundary).
		 */
		size = min_t(size_t, U16_MAX - header_size,
			     event->attr.aux_sample_size);
		size = rounddown(size, 8);
		size = perf_prepare_sample_aux(event, data, size);

		WARN_ON_ONCE(size + header_size > U16_MAX);
		data->dyn_size += size + sizeof(u64); /* size above */
		data->sample_flags |= PERF_SAMPLE_AUX;
	}
}

void perf_prepare_header(struct perf_event_header *header,
			 struct perf_sample_data *data,
			 struct perf_event *event,
			 struct pt_regs *regs)
{
	header->type = PERF_RECORD_SAMPLE;
	header->size = perf_sample_data_size(data, event);
	header->misc = perf_misc_flags(event, regs);

	/*
	 * If you're adding more sample types here, you likely need to do
	 * something about the overflowing header::size, like repurpose the
	 * lowest 3 bits of size, which should be always zero at the moment.
	 * This raises a more important question, do we really need 512k sized
	 * samples and why, so good argumentation is in order for whatever you
	 * do here next.
	 */
	WARN_ON_ONCE(header->size & 7);
}

static void __perf_event_aux_pause(struct perf_event *event, bool pause)
{
	if (pause) {
		if (!event->hw.aux_paused) {
			event->hw.aux_paused = 1;
			event->pmu->stop(event, PERF_EF_PAUSE);
		}
	} else {
		if (event->hw.aux_paused) {
			event->hw.aux_paused = 0;
			event->pmu->start(event, PERF_EF_RESUME);
		}
	}
}

static void perf_event_aux_pause(struct perf_event *event, bool pause)
{
	struct perf_buffer *rb;

	if (WARN_ON_ONCE(!event))
		return;

	rb = ring_buffer_get(event);
	if (!rb)
		return;

	scoped_guard (irqsave) {
		/*
		 * Guard against self-recursion here. Another event could trip
		 * this same from NMI context.
		 */
		if (READ_ONCE(rb->aux_in_pause_resume))
			break;

		WRITE_ONCE(rb->aux_in_pause_resume, 1);
		barrier();
		__perf_event_aux_pause(event, pause);
		barrier();
		WRITE_ONCE(rb->aux_in_pause_resume, 0);
	}
	ring_buffer_put(rb);
}

static __always_inline int
__perf_event_output(struct perf_event *event,
		    struct perf_sample_data *data,
		    struct pt_regs *regs,
		    int (*output_begin)(struct perf_output_handle *,
					struct perf_sample_data *,
					struct perf_event *,
					unsigned int))
{
	struct perf_output_handle handle;
	struct perf_event_header header;
	int err;

	/* protect the callchain buffers */
	rcu_read_lock();

	perf_prepare_sample(data, event, regs);
	perf_prepare_header(&header, data, event, regs);

	err = output_begin(&handle, data, event, header.size);
	if (err)
		goto exit;

	perf_output_sample(&handle, &header, data, event);

	perf_output_end(&handle);

exit:
	rcu_read_unlock();
	return err;
}

void
perf_event_output_forward(struct perf_event *event,
			 struct perf_sample_data *data,
			 struct pt_regs *regs)
{
	__perf_event_output(event, data, regs, perf_output_begin_forward);
}

void
perf_event_output_backward(struct perf_event *event,
			   struct perf_sample_data *data,
			   struct pt_regs *regs)
{
	__perf_event_output(event, data, regs, perf_output_begin_backward);
}

int
perf_event_output(struct perf_event *event,
		  struct perf_sample_data *data,
		  struct pt_regs *regs)
{
	return __perf_event_output(event, data, regs, perf_output_begin);
}

/*
 * read event_id
 */

struct perf_read_event {
	struct perf_event_header	header;

	u32				pid;
	u32				tid;
};

static void
perf_event_read_event(struct perf_event *event,
			struct task_struct *task)
{
	struct perf_output_handle handle;
	struct perf_sample_data sample;
	struct perf_read_event read_event = {
		.header = {
			.type = PERF_RECORD_READ,
			.misc = 0,
			.size = sizeof(read_event) + event->read_size,
		},
		.pid = perf_event_pid(event, task),
		.tid = perf_event_tid(event, task),
	};
	int ret;

	perf_event_header__init_id(&read_event.header, &sample, event);
	ret = perf_output_begin(&handle, &sample, event, read_event.header.size);
	if (ret)
		return;

	perf_output_put(&handle, read_event);
	perf_output_read(&handle, event);
	perf_event__output_id_sample(event, &handle, &sample);

	perf_output_end(&handle);
}

typedef void (perf_iterate_f)(struct perf_event *event, void *data);

static void
perf_iterate_ctx(struct perf_event_context *ctx,
		   perf_iterate_f output,
		   void *data, bool all)
{
	struct perf_event *event;

	list_for_each_entry_rcu(event, &ctx->event_list, event_entry) {
		if (!all) {
			if (event->state < PERF_EVENT_STATE_INACTIVE)
				continue;
			if (!event_filter_match(event))
				continue;
		}

		output(event, data);
	}
}

static void perf_iterate_sb_cpu(perf_iterate_f output, void *data)
{
	struct pmu_event_list *pel = this_cpu_ptr(&pmu_sb_events);
	struct perf_event *event;

	list_for_each_entry_rcu(event, &pel->list, sb_list) {
		/*
		 * Skip events that are not fully formed yet; ensure that
		 * if we observe event->ctx, both event and ctx will be
		 * complete enough. See perf_install_in_context().
		 */
		if (!smp_load_acquire(&event->ctx))
			continue;

		if (event->state < PERF_EVENT_STATE_INACTIVE)
			continue;
		if (!event_filter_match(event))
			continue;
		output(event, data);
	}
}

/*
 * Iterate all events that need to receive side-band events.
 *
 * For new callers; ensure that account_pmu_sb_event() includes
 * your event, otherwise it might not get delivered.
 */
static void
perf_iterate_sb(perf_iterate_f output, void *data,
	       struct perf_event_context *task_ctx)
{
	struct perf_event_context *ctx;

	rcu_read_lock();
	preempt_disable();

	/*
	 * If we have task_ctx != NULL we only notify the task context itself.
	 * The task_ctx is set only for EXIT events before releasing task
	 * context.
	 */
	if (task_ctx) {
		perf_iterate_ctx(task_ctx, output, data, false);
		goto done;
	}

	perf_iterate_sb_cpu(output, data);

	ctx = rcu_dereference(current->perf_event_ctxp);
	if (ctx)
		perf_iterate_ctx(ctx, output, data, false);
done:
	preempt_enable();
	rcu_read_unlock();
}

/*
 * Clear all file-based filters at exec, they'll have to be
 * re-instated when/if these objects are mmapped again.
 */
static void perf_event_addr_filters_exec(struct perf_event *event, void *data)
{
	struct perf_addr_filters_head *ifh = perf_event_addr_filters(event);
	struct perf_addr_filter *filter;
	unsigned int restart = 0, count = 0;
	unsigned long flags;

	if (!has_addr_filter(event))
		return;

	raw_spin_lock_irqsave(&ifh->lock, flags);
	list_for_each_entry(filter, &ifh->list, entry) {
		if (filter->path.dentry) {
			event->addr_filter_ranges[count].start = 0;
			event->addr_filter_ranges[count].size = 0;
			restart++;
		}

		count++;
	}

	if (restart)
		event->addr_filters_gen++;
	raw_spin_unlock_irqrestore(&ifh->lock, flags);

	if (restart)
		perf_event_stop(event, 1);
}

void perf_event_exec(void)
{
	struct perf_event_context *ctx;

	ctx = perf_pin_task_context(current);
	if (!ctx)
		return;

	perf_event_enable_on_exec(ctx);
	perf_event_remove_on_exec(ctx);
	scoped_guard(rcu)
		perf_iterate_ctx(ctx, perf_event_addr_filters_exec, NULL, true);

	perf_unpin_context(ctx);
	put_ctx(ctx);
}

struct remote_output {
	struct perf_buffer	*rb;
	int			err;
};

static void __perf_event_output_stop(struct perf_event *event, void *data)
{
	struct perf_event *parent = event->parent;
	struct remote_output *ro = data;
	struct perf_buffer *rb = ro->rb;
	struct stop_event_data sd = {
		.event	= event,
	};

	if (!has_aux(event))
		return;

	if (!parent)
		parent = event;

	/*
	 * In case of inheritance, it will be the parent that links to the
	 * ring-buffer, but it will be the child that's actually using it.
	 *
	 * We are using event::rb to determine if the event should be stopped,
	 * however this may race with ring_buffer_attach() (through set_output),
	 * which will make us skip the event that actually needs to be stopped.
	 * So ring_buffer_attach() has to stop an aux event before re-assigning
	 * its rb pointer.
	 */
	if (rcu_dereference(parent->rb) == rb)
		ro->err = __perf_event_stop(&sd);
}

static int __perf_pmu_output_stop(void *info)
{
	struct perf_event *event = info;
	struct perf_cpu_context *cpuctx = this_cpu_ptr(&perf_cpu_context);
	struct remote_output ro = {
		.rb	= event->rb,
	};

	rcu_read_lock();
	perf_iterate_ctx(&cpuctx->ctx, __perf_event_output_stop, &ro, false);
	if (cpuctx->task_ctx)
		perf_iterate_ctx(cpuctx->task_ctx, __perf_event_output_stop,
				   &ro, false);
	rcu_read_unlock();

	return ro.err;
}

static void perf_pmu_output_stop(struct perf_event *event)
{
	struct perf_event *iter;
	int err, cpu;

restart:
	rcu_read_lock();
	list_for_each_entry_rcu(iter, &event->rb->event_list, rb_entry) {
		/*
		 * For per-CPU events, we need to make sure that neither they
		 * nor their children are running; for cpu==-1 events it's
		 * sufficient to stop the event itself if it's active, since
		 * it can't have children.
		 */
		cpu = iter->cpu;
		if (cpu == -1)
			cpu = READ_ONCE(iter->oncpu);

		if (cpu == -1)
			continue;

		err = cpu_function_call(cpu, __perf_pmu_output_stop, event);
		if (err == -EAGAIN) {
			rcu_read_unlock();
			goto restart;
		}
	}
	rcu_read_unlock();
}

/*
 * task tracking -- fork/exit
 *
 * enabled by: attr.comm | attr.mmap | attr.mmap2 | attr.mmap_data | attr.task
 */

struct perf_task_event {
	struct task_struct		*task;
	struct perf_event_context	*task_ctx;

	struct {
		struct perf_event_header	header;

		u32				pid;
		u32				ppid;
		u32				tid;
		u32				ptid;
		u64				time;
	} event_id;
};

static int perf_event_task_match(struct perf_event *event)
{
	return event->attr.comm  || event->attr.mmap ||
	       event->attr.mmap2 || event->attr.mmap_data ||
	       event->attr.task;
}

static void perf_event_task_output(struct perf_event *event,
				   void *data)
{
	struct perf_task_event *task_event = data;
	struct perf_output_handle handle;
	struct perf_sample_data	sample;
	struct task_struct *task = task_event->task;
	int ret, size = task_event->event_id.header.size;

	if (!perf_event_task_match(event))
		return;

	perf_event_header__init_id(&task_event->event_id.header, &sample, event);

	ret = perf_output_begin(&handle, &sample, event,
				task_event->event_id.header.size);
	if (ret)
		goto out;

	task_event->event_id.pid = perf_event_pid(event, task);
	task_event->event_id.tid = perf_event_tid(event, task);

	if (task_event->event_id.header.type == PERF_RECORD_EXIT) {
		task_event->event_id.ppid = perf_event_pid(event,
							task->real_parent);
		task_event->event_id.ptid = perf_event_pid(event,
							task->real_parent);
	} else {  /* PERF_RECORD_FORK */
		task_event->event_id.ppid = perf_event_pid(event, current);
		task_event->event_id.ptid = perf_event_tid(event, current);
	}

	task_event->event_id.time = perf_event_clock(event);

	perf_output_put(&handle, task_event->event_id);

	perf_event__output_id_sample(event, &handle, &sample);

	perf_output_end(&handle);
out:
	task_event->event_id.header.size = size;
}

static void perf_event_task(struct task_struct *task,
			      struct perf_event_context *task_ctx,
			      int new)
{
	struct perf_task_event task_event;

	if (!atomic_read(&nr_comm_events) &&
	    !atomic_read(&nr_mmap_events) &&
	    !atomic_read(&nr_task_events))
		return;

	task_event = (struct perf_task_event){
		.task	  = task,
		.task_ctx = task_ctx,
		.event_id    = {
			.header = {
				.type = new ? PERF_RECORD_FORK : PERF_RECORD_EXIT,
				.misc = 0,
				.size = sizeof(task_event.event_id),
			},
			/* .pid  */
			/* .ppid */
			/* .tid  */
			/* .ptid */
			/* .time */
		},
	};

	perf_iterate_sb(perf_event_task_output,
		       &task_event,
		       task_ctx);
}

/*
 * Allocate data for a new task when profiling system-wide
 * events which require PMU specific data
 */
static void
perf_event_alloc_task_data(struct task_struct *child,
			   struct task_struct *parent)
{
	struct kmem_cache *ctx_cache = NULL;
	struct perf_ctx_data *cd;

	if (!refcount_read(&global_ctx_data_ref))
		return;

	scoped_guard (rcu) {
		cd = rcu_dereference(parent->perf_ctx_data);
		if (cd)
			ctx_cache = cd->ctx_cache;
	}

	if (!ctx_cache)
		return;

	guard(percpu_read)(&global_ctx_data_rwsem);
	scoped_guard (rcu) {
		cd = rcu_dereference(child->perf_ctx_data);
		if (!cd) {
			/*
			 * A system-wide event may be unaccount,
			 * when attaching the perf_ctx_data.
			 */
			if (!refcount_read(&global_ctx_data_ref))
				return;
			goto attach;
		}

		if (!cd->global) {
			cd->global = 1;
			refcount_inc(&cd->refcount);
		}
	}

	return;
attach:
	attach_task_ctx_data(child, ctx_cache, true);
}

void perf_event_fork(struct task_struct *task)
{
	perf_event_task(task, NULL, 1);
	perf_event_namespaces(task);
	perf_event_alloc_task_data(task, current);
}

/*
 * comm tracking
 */

struct perf_comm_event {
	struct task_struct	*task;
	char			*comm;
	int			comm_size;

	struct {
		struct perf_event_header	header;

		u32				pid;
		u32				tid;
	} event_id;
};

static int perf_event_comm_match(struct perf_event *event)
{
	return event->attr.comm;
}

static void perf_event_comm_output(struct perf_event *event,
				   void *data)
{
	struct perf_comm_event *comm_event = data;
	struct perf_output_handle handle;
	struct perf_sample_data sample;
	int size = comm_event->event_id.header.size;
	int ret;

	if (!perf_event_comm_match(event))
		return;

	perf_event_header__init_id(&comm_event->event_id.header, &sample, event);
	ret = perf_output_begin(&handle, &sample, event,
				comm_event->event_id.header.size);

	if (ret)
		goto out;

	comm_event->event_id.pid = perf_event_pid(event, comm_event->task);
	comm_event->event_id.tid = perf_event_tid(event, comm_event->task);

	perf_output_put(&handle, comm_event->event_id);
	__output_copy(&handle, comm_event->comm,
				   comm_event->comm_size);

	perf_event__output_id_sample(event, &handle, &sample);

	perf_output_end(&handle);
out:
	comm_event->event_id.header.size = size;
}

static void perf_event_comm_event(struct perf_comm_event *comm_event)
{
	char comm[TASK_COMM_LEN];
	unsigned int size;

	memset(comm, 0, sizeof(comm));
	strscpy(comm, comm_event->task->comm);
	size = ALIGN(strlen(comm)+1, sizeof(u64));

	comm_event->comm = comm;
	comm_event->comm_size = size;

	comm_event->event_id.header.size = sizeof(comm_event->event_id) + size;

	perf_iterate_sb(perf_event_comm_output,
		       comm_event,
		       NULL);
}

void perf_event_comm(struct task_struct *task, bool exec)
{
	struct perf_comm_event comm_event;

	if (!atomic_read(&nr_comm_events))
		return;

	comm_event = (struct perf_comm_event){
		.task	= task,
		/* .comm      */
		/* .comm_size */
		.event_id  = {
			.header = {
				.type = PERF_RECORD_COMM,
				.misc = exec ? PERF_RECORD_MISC_COMM_EXEC : 0,
				/* .size */
			},
			/* .pid */
			/* .tid */
		},
	};

	perf_event_comm_event(&comm_event);
}

/*
 * namespaces tracking
 */

struct perf_namespaces_event {
	struct task_struct		*task;

	struct {
		struct perf_event_header	header;

		u32				pid;
		u32				tid;
		u64				nr_namespaces;
		struct perf_ns_link_info	link_info[NR_NAMESPACES];
	} event_id;
};

static int perf_event_namespaces_match(struct perf_event *event)
{
	return event->attr.namespaces;
}

static void perf_event_namespaces_output(struct perf_event *event,
					 void *data)
{
	struct perf_namespaces_event *namespaces_event = data;
	struct perf_output_handle handle;
	struct perf_sample_data sample;
	u16 header_size = namespaces_event->event_id.header.size;
	int ret;

	if (!perf_event_namespaces_match(event))
		return;

	perf_event_header__init_id(&namespaces_event->event_id.header,
				   &sample, event);
	ret = perf_output_begin(&handle, &sample, event,
				namespaces_event->event_id.header.size);
	if (ret)
		goto out;

	namespaces_event->event_id.pid = perf_event_pid(event,
							namespaces_event->task);
	namespaces_event->event_id.tid = perf_event_tid(event,
							namespaces_event->task);

	perf_output_put(&handle, namespaces_event->event_id);

	perf_event__output_id_sample(event, &handle, &sample);

	perf_output_end(&handle);
out:
	namespaces_event->event_id.header.size = header_size;
}

static void perf_fill_ns_link_info(struct perf_ns_link_info *ns_link_info,
				   struct task_struct *task,
				   const struct proc_ns_operations *ns_ops)
{
	struct path ns_path;
	struct inode *ns_inode;
	int error;

	error = ns_get_path(&ns_path, task, ns_ops);
	if (!error) {
		ns_inode = ns_path.dentry->d_inode;
		ns_link_info->dev = new_encode_dev(ns_inode->i_sb->s_dev);
		ns_link_info->ino = ns_inode->i_ino;
		path_put(&ns_path);
	}
}

void perf_event_namespaces(struct task_struct *task)
{
	struct perf_namespaces_event namespaces_event;
	struct perf_ns_link_info *ns_link_info;

	if (!atomic_read(&nr_namespaces_events))
		return;

	namespaces_event = (struct perf_namespaces_event){
		.task	= task,
		.event_id  = {
			.header = {
				.type = PERF_RECORD_NAMESPACES,
				.misc = 0,
				.size = sizeof(namespaces_event.event_id),
			},
			/* .pid */
			/* .tid */
			.nr_namespaces = NR_NAMESPACES,
			/* .link_info[NR_NAMESPACES] */
		},
	};

	ns_link_info = namespaces_event.event_id.link_info;

	perf_fill_ns_link_info(&ns_link_info[MNT_NS_INDEX],
			       task, &mntns_operations);

#ifdef CONFIG_USER_NS
	perf_fill_ns_link_info(&ns_link_info[USER_NS_INDEX],
			       task, &userns_operations);
#endif
#ifdef CONFIG_NET_NS
	perf_fill_ns_link_info(&ns_link_info[NET_NS_INDEX],
			       task, &netns_operations);
#endif
#ifdef CONFIG_UTS_NS
	perf_fill_ns_link_info(&ns_link_info[UTS_NS_INDEX],
			       task, &utsns_operations);
#endif
#ifdef CONFIG_IPC_NS
	perf_fill_ns_link_info(&ns_link_info[IPC_NS_INDEX],
			       task, &ipcns_operations);
#endif
#ifdef CONFIG_PID_NS
	perf_fill_ns_link_info(&ns_link_info[PID_NS_INDEX],
			       task, &pidns_operations);
#endif
#ifdef CONFIG_CGROUPS
	perf_fill_ns_link_info(&ns_link_info[CGROUP_NS_INDEX],
			       task, &cgroupns_operations);
#endif

	perf_iterate_sb(perf_event_namespaces_output,
			&namespaces_event,
			NULL);
}

/*
 * cgroup tracking
 */
#ifdef CONFIG_CGROUP_PERF

struct perf_cgroup_event {
	char				*path;
	int				path_size;
	struct {
		struct perf_event_header	header;
		u64				id;
		char				path[];
	} event_id;
};

static int perf_event_cgroup_match(struct perf_event *event)
{
	return event->attr.cgroup;
}

static void perf_event_cgroup_output(struct perf_event *event, void *data)
{
	struct perf_cgroup_event *cgroup_event = data;
	struct perf_output_handle handle;
	struct perf_sample_data sample;
	u16 header_size = cgroup_event->event_id.header.size;
	int ret;

	if (!perf_event_cgroup_match(event))
		return;

	perf_event_header__init_id(&cgroup_event->event_id.header,
				   &sample, event);
	ret = perf_output_begin(&handle, &sample, event,
				cgroup_event->event_id.header.size);
	if (ret)
		goto out;

	perf_output_put(&handle, cgroup_event->event_id);
	__output_copy(&handle, cgroup_event->path, cgroup_event->path_size);

	perf_event__output_id_sample(event, &handle, &sample);

	perf_output_end(&handle);
out:
	cgroup_event->event_id.header.size = header_size;
}

static void perf_event_cgroup(struct cgroup *cgrp)
{
	struct perf_cgroup_event cgroup_event;
	char path_enomem[16] = "//enomem";
	char *pathname;
	size_t size;

	if (!atomic_read(&nr_cgroup_events))
		return;

	cgroup_event = (struct perf_cgroup_event){
		.event_id  = {
			.header = {
				.type = PERF_RECORD_CGROUP,
				.misc = 0,
				.size = sizeof(cgroup_event.event_id),
			},
			.id = cgroup_id(cgrp),
		},
	};

	pathname = kmalloc(PATH_MAX, GFP_KERNEL);
	if (pathname == NULL) {
		cgroup_event.path = path_enomem;
	} else {
		/* just to be sure to have enough space for alignment */
		cgroup_path(cgrp, pathname, PATH_MAX - sizeof(u64));
		cgroup_event.path = pathname;
	}

	/*
	 * Since our buffer works in 8 byte units we need to align our string
	 * size to a multiple of 8. However, we must guarantee the tail end is
	 * zero'd out to avoid leaking random bits to userspace.
	 */
	size = strlen(cgroup_event.path) + 1;
	while (!IS_ALIGNED(size, sizeof(u64)))
		cgroup_event.path[size++] = '\0';

	cgroup_event.event_id.header.size += size;
	cgroup_event.path_size = size;

	perf_iterate_sb(perf_event_cgroup_output,
			&cgroup_event,
			NULL);

	kfree(pathname);
}

#endif

/*
 * mmap tracking
 */

struct perf_mmap_event {
	struct vm_area_struct	*vma;

	const char		*file_name;
	int			file_size;
	int			maj, min;
	u64			ino;
	u64			ino_generation;
	u32			prot, flags;
	u8			build_id[BUILD_ID_SIZE_MAX];
	u32			build_id_size;

	struct {
		struct perf_event_header	header;

		u32				pid;
		u32				tid;
		u64				start;
		u64				len;
		u64				pgoff;
	} event_id;
};

static int perf_event_mmap_match(struct perf_event *event,
				 void *data)
{
	struct perf_mmap_event *mmap_event = data;
	struct vm_area_struct *vma = mmap_event->vma;
	int executable = vma->vm_flags & VM_EXEC;

	return (!executable && event->attr.mmap_data) ||
	       (executable && (event->attr.mmap || event->attr.mmap2));
}

static void perf_event_mmap_output(struct perf_event *event,
				   void *data)
{
	struct perf_mmap_event *mmap_event = data;
	struct perf_output_handle handle;
	struct perf_sample_data sample;
	int size = mmap_event->event_id.header.size;
	u32 type = mmap_event->event_id.header.type;
	bool use_build_id;
	int ret;

	if (!perf_event_mmap_match(event, data))
		return;

	if (event->attr.mmap2) {
		mmap_event->event_id.header.type = PERF_RECORD_MMAP2;
		mmap_event->event_id.header.size += sizeof(mmap_event->maj);
		mmap_event->event_id.header.size += sizeof(mmap_event->min);
		mmap_event->event_id.header.size += sizeof(mmap_event->ino);
		mmap_event->event_id.header.size += sizeof(mmap_event->ino_generation);
		mmap_event->event_id.header.size += sizeof(mmap_event->prot);
		mmap_event->event_id.header.size += sizeof(mmap_event->flags);
	}

	perf_event_header__init_id(&mmap_event->event_id.header, &sample, event);
	ret = perf_output_begin(&handle, &sample, event,
				mmap_event->event_id.header.size);
	if (ret)
		goto out;

	mmap_event->event_id.pid = perf_event_pid(event, current);
	mmap_event->event_id.tid = perf_event_tid(event, current);

	use_build_id = event->attr.build_id && mmap_event->build_id_size;

	if (event->attr.mmap2 && use_build_id)
		mmap_event->event_id.header.misc |= PERF_RECORD_MISC_MMAP_BUILD_ID;

	perf_output_put(&handle, mmap_event->event_id);

	if (event->attr.mmap2) {
		if (use_build_id) {
			u8 size[4] = { (u8) mmap_event->build_id_size, 0, 0, 0 };

			__output_copy(&handle, size, 4);
			__output_copy(&handle, mmap_event->build_id, BUILD_ID_SIZE_MAX);
		} else {
			perf_output_put(&handle, mmap_event->maj);
			perf_output_put(&handle, mmap_event->min);
			perf_output_put(&handle, mmap_event->ino);
			perf_output_put(&handle, mmap_event->ino_generation);
		}
		perf_output_put(&handle, mmap_event->prot);
		perf_output_put(&handle, mmap_event->flags);
	}

	__output_copy(&handle, mmap_event->file_name,
				   mmap_event->file_size);

	perf_event__output_id_sample(event, &handle, &sample);

	perf_output_end(&handle);
out:
	mmap_event->event_id.header.size = size;
	mmap_event->event_id.header.type = type;
}

static void perf_event_mmap_event(struct perf_mmap_event *mmap_event)
{
	struct vm_area_struct *vma = mmap_event->vma;
	struct file *file = vma->vm_file;
	int maj = 0, min = 0;
	u64 ino = 0, gen = 0;
	u32 prot = 0, flags = 0;
	unsigned int size;
	char tmp[16];
	char *buf = NULL;
	char *name = NULL;

	if (vma->vm_flags & VM_READ)
		prot |= PROT_READ;
	if (vma->vm_flags & VM_WRITE)
		prot |= PROT_WRITE;
	if (vma->vm_flags & VM_EXEC)
		prot |= PROT_EXEC;

	if (vma->vm_flags & VM_MAYSHARE)
		flags = MAP_SHARED;
	else
		flags = MAP_PRIVATE;

	if (vma->vm_flags & VM_LOCKED)
		flags |= MAP_LOCKED;
	if (is_vm_hugetlb_page(vma))
		flags |= MAP_HUGETLB;

	if (file) {
		struct inode *inode;
		dev_t dev;

		buf = kmalloc(PATH_MAX, GFP_KERNEL);
		if (!buf) {
			name = "//enomem";
			goto cpy_name;
		}
		/*
		 * d_path() works from the end of the rb backwards, so we
		 * need to add enough zero bytes after the string to handle
		 * the 64bit alignment we do later.
		 */
		name = file_path(file, buf, PATH_MAX - sizeof(u64));
		if (IS_ERR(name)) {
			name = "//toolong";
			goto cpy_name;
		}
		inode = file_inode(vma->vm_file);
		dev = inode->i_sb->s_dev;
		ino = inode->i_ino;
		gen = inode->i_generation;
		maj = MAJOR(dev);
		min = MINOR(dev);

		goto got_name;
	} else {
		if (vma->vm_ops && vma->vm_ops->name)
			name = (char *) vma->vm_ops->name(vma);
		if (!name)
			name = (char *)arch_vma_name(vma);
		if (!name) {
			if (vma_is_initial_heap(vma))
				name = "[heap]";
			else if (vma_is_initial_stack(vma))
				name = "[stack]";
			else
				name = "//anon";
		}
	}

cpy_name:
	strscpy(tmp, name);
	name = tmp;
got_name:
	/*
	 * Since our buffer works in 8 byte units we need to align our string
	 * size to a multiple of 8. However, we must guarantee the tail end is
	 * zero'd out to avoid leaking random bits to userspace.
	 */
	size = strlen(name)+1;
	while (!IS_ALIGNED(size, sizeof(u64)))
		name[size++] = '\0';

	mmap_event->file_name = name;
	mmap_event->file_size = size;
	mmap_event->maj = maj;
	mmap_event->min = min;
	mmap_event->ino = ino;
	mmap_event->ino_generation = gen;
	mmap_event->prot = prot;
	mmap_event->flags = flags;

	if (!(vma->vm_flags & VM_EXEC))
		mmap_event->event_id.header.misc |= PERF_RECORD_MISC_MMAP_DATA;

	mmap_event->event_id.header.size = sizeof(mmap_event->event_id) + size;

	if (atomic_read(&nr_build_id_events))
		build_id_parse_nofault(vma, mmap_event->build_id, &mmap_event->build_id_size);

	perf_iterate_sb(perf_event_mmap_output,
		       mmap_event,
		       NULL);

	kfree(buf);
}

/*
 * Check whether inode and address range match filter criteria.
 */
static bool perf_addr_filter_match(struct perf_addr_filter *filter,
				     struct file *file, unsigned long offset,
				     unsigned long size)
{
	/* d_inode(NULL) won't be equal to any mapped user-space file */
	if (!filter->path.dentry)
		return false;

	if (d_inode(filter->path.dentry) != file_inode(file))
		return false;

	if (filter->offset > offset + size)
		return false;

	if (filter->offset + filter->size < offset)
		return false;

	return true;
}

static bool perf_addr_filter_vma_adjust(struct perf_addr_filter *filter,
					struct vm_area_struct *vma,
					struct perf_addr_filter_range *fr)
{
	unsigned long vma_size = vma->vm_end - vma->vm_start;
	unsigned long off = vma->vm_pgoff << PAGE_SHIFT;
	struct file *file = vma->vm_file;

	if (!perf_addr_filter_match(filter, file, off, vma_size))
		return false;

	if (filter->offset < off) {
		fr->start = vma->vm_start;
		fr->size = min(vma_size, filter->size - (off - filter->offset));
	} else {
		fr->start = vma->vm_start + filter->offset - off;
		fr->size = min(vma->vm_end - fr->start, filter->size);
	}

	return true;
}

static void __perf_addr_filters_adjust(struct perf_event *event, void *data)
{
	struct perf_addr_filters_head *ifh = perf_event_addr_filters(event);
	struct vm_area_struct *vma = data;
	struct perf_addr_filter *filter;
	unsigned int restart = 0, count = 0;
	unsigned long flags;

	if (!has_addr_filter(event))
		return;

	if (!vma->vm_file)
		return;

	raw_spin_lock_irqsave(&ifh->lock, flags);
	list_for_each_entry(filter, &ifh->list, entry) {
		if (perf_addr_filter_vma_adjust(filter, vma,
						&event->addr_filter_ranges[count]))
			restart++;

		count++;
	}

	if (restart)
		event->addr_filters_gen++;
	raw_spin_unlock_irqrestore(&ifh->lock, flags);

	if (restart)
		perf_event_stop(event, 1);
}

/*
 * Adjust all task's events' filters to the new vma
 */
static void perf_addr_filters_adjust(struct vm_area_struct *vma)
{
	struct perf_event_context *ctx;

	/*
	 * Data tracing isn't supported yet and as such there is no need
	 * to keep track of anything that isn't related to executable code:
	 */
	if (!(vma->vm_flags & VM_EXEC))
		return;

	rcu_read_lock();
	ctx = rcu_dereference(current->perf_event_ctxp);
	if (ctx)
		perf_iterate_ctx(ctx, __perf_addr_filters_adjust, vma, true);
	rcu_read_unlock();
}

void perf_event_mmap(struct vm_area_struct *vma)
{
	struct perf_mmap_event mmap_event;

	if (!atomic_read(&nr_mmap_events))
		return;

	mmap_event = (struct perf_mmap_event){
		.vma	= vma,
		/* .file_name */
		/* .file_size */
		.event_id  = {
			.header = {
				.type = PERF_RECORD_MMAP,
				.misc = PERF_RECORD_MISC_USER,
				/* .size */
			},
			/* .pid */
			/* .tid */
			.start  = vma->vm_start,
			.len    = vma->vm_end - vma->vm_start,
			.pgoff  = (u64)vma->vm_pgoff << PAGE_SHIFT,
		},
		/* .maj (attr_mmap2 only) */
		/* .min (attr_mmap2 only) */
		/* .ino (attr_mmap2 only) */
		/* .ino_generation (attr_mmap2 only) */
		/* .prot (attr_mmap2 only) */
		/* .flags (attr_mmap2 only) */
	};

	perf_addr_filters_adjust(vma);
	perf_event_mmap_event(&mmap_event);
}

void perf_event_aux_event(struct perf_event *event, unsigned long head,
			  unsigned long size, u64 flags)
{
	struct perf_output_handle handle;
	struct perf_sample_data sample;
	struct perf_aux_event {
		struct perf_event_header	header;
		u64				offset;
		u64				size;
		u64				flags;
	} rec = {
		.header = {
			.type = PERF_RECORD_AUX,
			.misc = 0,
			.size = sizeof(rec),
		},
		.offset		= head,
		.size		= size,
		.flags		= flags,
	};
	int ret;

	perf_event_header__init_id(&rec.header, &sample, event);
	ret = perf_output_begin(&handle, &sample, event, rec.header.size);

	if (ret)
		return;

	perf_output_put(&handle, rec);
	perf_event__output_id_sample(event, &handle, &sample);

	perf_output_end(&handle);
}

/*
 * Lost/dropped samples logging
 */
void perf_log_lost_samples(struct perf_event *event, u64 lost)
{
	struct perf_output_handle handle;
	struct perf_sample_data sample;
	int ret;

	struct {
		struct perf_event_header	header;
		u64				lost;
	} lost_samples_event = {
		.header = {
			.type = PERF_RECORD_LOST_SAMPLES,
			.misc = 0,
			.size = sizeof(lost_samples_event),
		},
		.lost		= lost,
	};

	perf_event_header__init_id(&lost_samples_event.header, &sample, event);

	ret = perf_output_begin(&handle, &sample, event,
				lost_samples_event.header.size);
	if (ret)
		return;

	perf_output_put(&handle, lost_samples_event);
	perf_event__output_id_sample(event, &handle, &sample);
	perf_output_end(&handle);
}

/*
 * context_switch tracking
 */

struct perf_switch_event {
	struct task_struct	*task;
	struct task_struct	*next_prev;

	struct {
		struct perf_event_header	header;
		u32				next_prev_pid;
		u32				next_prev_tid;
	} event_id;
};

static int perf_event_switch_match(struct perf_event *event)
{
	return event->attr.context_switch;
}

static void perf_event_switch_output(struct perf_event *event, void *data)
{
	struct perf_switch_event *se = data;
	struct perf_output_handle handle;
	struct perf_sample_data sample;
	int ret;

	if (!perf_event_switch_match(event))
		return;

	/* Only CPU-wide events are allowed to see next/prev pid/tid */
	if (event->ctx->task) {
		se->event_id.header.type = PERF_RECORD_SWITCH;
		se->event_id.header.size = sizeof(se->event_id.header);
	} else {
		se->event_id.header.type = PERF_RECORD_SWITCH_CPU_WIDE;
		se->event_id.header.size = sizeof(se->event_id);
		se->event_id.next_prev_pid =
					perf_event_pid(event, se->next_prev);
		se->event_id.next_prev_tid =
					perf_event_tid(event, se->next_prev);
	}

	perf_event_header__init_id(&se->event_id.header, &sample, event);

	ret = perf_output_begin(&handle, &sample, event, se->event_id.header.size);
	if (ret)
		return;

	if (event->ctx->task)
		perf_output_put(&handle, se->event_id.header);
	else
		perf_output_put(&handle, se->event_id);

	perf_event__output_id_sample(event, &handle, &sample);

	perf_output_end(&handle);
}

static void perf_event_switch(struct task_struct *task,
			      struct task_struct *next_prev, bool sched_in)
{
	struct perf_switch_event switch_event;

	/* N.B. caller checks nr_switch_events != 0 */

	switch_event = (struct perf_switch_event){
		.task		= task,
		.next_prev	= next_prev,
		.event_id	= {
			.header = {
				/* .type */
				.misc = sched_in ? 0 : PERF_RECORD_MISC_SWITCH_OUT,
				/* .size */
			},
			/* .next_prev_pid */
			/* .next_prev_tid */
		},
	};

	if (!sched_in && task_is_runnable(task)) {
		switch_event.event_id.header.misc |=
				PERF_RECORD_MISC_SWITCH_OUT_PREEMPT;
	}

	perf_iterate_sb(perf_event_switch_output, &switch_event, NULL);
}

/*
 * IRQ throttle logging
 */

static void perf_log_throttle(struct perf_event *event, int enable)
{
	struct perf_output_handle handle;
	struct perf_sample_data sample;
	int ret;

	struct {
		struct perf_event_header	header;
		u64				time;
		u64				id;
		u64				stream_id;
	} throttle_event = {
		.header = {
			.type = PERF_RECORD_THROTTLE,
			.misc = 0,
			.size = sizeof(throttle_event),
		},
		.time		= perf_event_clock(event),
		.id		= primary_event_id(event),
		.stream_id	= event->id,
	};

	if (enable)
		throttle_event.header.type = PERF_RECORD_UNTHROTTLE;

	perf_event_header__init_id(&throttle_event.header, &sample, event);

	ret = perf_output_begin(&handle, &sample, event,
				throttle_event.header.size);
	if (ret)
		return;

	perf_output_put(&handle, throttle_event);
	perf_event__output_id_sample(event, &handle, &sample);
	perf_output_end(&handle);
}

/*
 * ksymbol register/unregister tracking
 */

struct perf_ksymbol_event {
	const char	*name;
	int		name_len;
	struct {
		struct perf_event_header        header;
		u64				addr;
		u32				len;
		u16				ksym_type;
		u16				flags;
	} event_id;
};

static int perf_event_ksymbol_match(struct perf_event *event)
{
	return event->attr.ksymbol;
}

static void perf_event_ksymbol_output(struct perf_event *event, void *data)
{
	struct perf_ksymbol_event *ksymbol_event = data;
	struct perf_output_handle handle;
	struct perf_sample_data sample;
	int ret;

	if (!perf_event_ksymbol_match(event))
		return;

	perf_event_header__init_id(&ksymbol_event->event_id.header,
				   &sample, event);
	ret = perf_output_begin(&handle, &sample, event,
				ksymbol_event->event_id.header.size);
	if (ret)
		return;

	perf_output_put(&handle, ksymbol_event->event_id);
	__output_copy(&handle, ksymbol_event->name, ksymbol_event->name_len);
	perf_event__output_id_sample(event, &handle, &sample);

	perf_output_end(&handle);
}

void perf_event_ksymbol(u16 ksym_type, u64 addr, u32 len, bool unregister,
			const char *sym)
{
	struct perf_ksymbol_event ksymbol_event;
	char name[KSYM_NAME_LEN];
	u16 flags = 0;
	int name_len;

	if (!atomic_read(&nr_ksymbol_events))
		return;

	if (ksym_type >= PERF_RECORD_KSYMBOL_TYPE_MAX ||
	    ksym_type == PERF_RECORD_KSYMBOL_TYPE_UNKNOWN)
		goto err;

	strscpy(name, sym);
	name_len = strlen(name) + 1;
	while (!IS_ALIGNED(name_len, sizeof(u64)))
		name[name_len++] = '\0';
	BUILD_BUG_ON(KSYM_NAME_LEN % sizeof(u64));

	if (unregister)
		flags |= PERF_RECORD_KSYMBOL_FLAGS_UNREGISTER;

	ksymbol_event = (struct perf_ksymbol_event){
		.name = name,
		.name_len = name_len,
		.event_id = {
			.header = {
				.type = PERF_RECORD_KSYMBOL,
				.size = sizeof(ksymbol_event.event_id) +
					name_len,
			},
			.addr = addr,
			.len = len,
			.ksym_type = ksym_type,
			.flags = flags,
		},
	};

	perf_iterate_sb(perf_event_ksymbol_output, &ksymbol_event, NULL);
	return;
err:
	WARN_ONCE(1, "%s: Invalid KSYMBOL type 0x%x\n", __func__, ksym_type);
}

/*
 * bpf program load/unload tracking
 */

struct perf_bpf_event {
	struct bpf_prog	*prog;
	struct {
		struct perf_event_header        header;
		u16				type;
		u16				flags;
		u32				id;
		u8				tag[BPF_TAG_SIZE];
	} event_id;
};

static int perf_event_bpf_match(struct perf_event *event)
{
	return event->attr.bpf_event;
}

static void perf_event_bpf_output(struct perf_event *event, void *data)
{
	struct perf_bpf_event *bpf_event = data;
	struct perf_output_handle handle;
	struct perf_sample_data sample;
	int ret;

	if (!perf_event_bpf_match(event))
		return;

	perf_event_header__init_id(&bpf_event->event_id.header,
				   &sample, event);
	ret = perf_output_begin(&handle, &sample, event,
				bpf_event->event_id.header.size);
	if (ret)
		return;

	perf_output_put(&handle, bpf_event->event_id);
	perf_event__output_id_sample(event, &handle, &sample);

	perf_output_end(&handle);
}

static void perf_event_bpf_emit_ksymbols(struct bpf_prog *prog,
					 enum perf_bpf_event_type type)
{
	bool unregister = type == PERF_BPF_EVENT_PROG_UNLOAD;
	int i;

	perf_event_ksymbol(PERF_RECORD_KSYMBOL_TYPE_BPF,
			   (u64)(unsigned long)prog->bpf_func,
			   prog->jited_len, unregister,
			   prog->aux->ksym.name);

	for (i = 1; i < prog->aux->func_cnt; i++) {
		struct bpf_prog *subprog = prog->aux->func[i];

		perf_event_ksymbol(
			PERF_RECORD_KSYMBOL_TYPE_BPF,
			(u64)(unsigned long)subprog->bpf_func,
			subprog->jited_len, unregister,
			subprog->aux->ksym.name);
	}
}

void perf_event_bpf_event(struct bpf_prog *prog,
			  enum perf_bpf_event_type type,
			  u16 flags)
{
	struct perf_bpf_event bpf_event;

	switch (type) {
	case PERF_BPF_EVENT_PROG_LOAD:
	case PERF_BPF_EVENT_PROG_UNLOAD:
		if (atomic_read(&nr_ksymbol_events))
			perf_event_bpf_emit_ksymbols(prog, type);
		break;
	default:
		return;
	}

	if (!atomic_read(&nr_bpf_events))
		return;

	bpf_event = (struct perf_bpf_event){
		.prog = prog,
		.event_id = {
			.header = {
				.type = PERF_RECORD_BPF_EVENT,
				.size = sizeof(bpf_event.event_id),
			},
			.type = type,
			.flags = flags,
			.id = prog->aux->id,
		},
	};

	BUILD_BUG_ON(BPF_TAG_SIZE % sizeof(u64));

	memcpy(bpf_event.event_id.tag, prog->tag, BPF_TAG_SIZE);
	perf_iterate_sb(perf_event_bpf_output, &bpf_event, NULL);
}

struct perf_text_poke_event {
	const void		*old_bytes;
	const void		*new_bytes;
	size_t			pad;
	u16			old_len;
	u16			new_len;

	struct {
		struct perf_event_header	header;

		u64				addr;
	} event_id;
};

static int perf_event_text_poke_match(struct perf_event *event)
{
	return event->attr.text_poke;
}

static void perf_event_text_poke_output(struct perf_event *event, void *data)
{
	struct perf_text_poke_event *text_poke_event = data;
	struct perf_output_handle handle;
	struct perf_sample_data sample;
	u64 padding = 0;
	int ret;

	if (!perf_event_text_poke_match(event))
		return;

	perf_event_header__init_id(&text_poke_event->event_id.header, &sample, event);

	ret = perf_output_begin(&handle, &sample, event,
				text_poke_event->event_id.header.size);
	if (ret)
		return;

	perf_output_put(&handle, text_poke_event->event_id);
	perf_output_put(&handle, text_poke_event->old_len);
	perf_output_put(&handle, text_poke_event->new_len);

	__output_copy(&handle, text_poke_event->old_bytes, text_poke_event->old_len);
	__output_copy(&handle, text_poke_event->new_bytes, text_poke_event->new_len);

	if (text_poke_event->pad)
		__output_copy(&handle, &padding, text_poke_event->pad);

	perf_event__output_id_sample(event, &handle, &sample);

	perf_output_end(&handle);
}

void perf_event_text_poke(const void *addr, const void *old_bytes,
			  size_t old_len, const void *new_bytes, size_t new_len)
{
	struct perf_text_poke_event text_poke_event;
	size_t tot, pad;

	if (!atomic_read(&nr_text_poke_events))
		return;

	tot  = sizeof(text_poke_event.old_len) + old_len;
	tot += sizeof(text_poke_event.new_len) + new_len;
	pad  = ALIGN(tot, sizeof(u64)) - tot;

	text_poke_event = (struct perf_text_poke_event){
		.old_bytes    = old_bytes,
		.new_bytes    = new_bytes,
		.pad          = pad,
		.old_len      = old_len,
		.new_len      = new_len,
		.event_id  = {
			.header = {
				.type = PERF_RECORD_TEXT_POKE,
				.misc = PERF_RECORD_MISC_KERNEL,
				.size = sizeof(text_poke_event.event_id) + tot + pad,
			},
			.addr = (unsigned long)addr,
		},
	};

	perf_iterate_sb(perf_event_text_poke_output, &text_poke_event, NULL);
}

void perf_event_itrace_started(struct perf_event *event)
{
	event->attach_state |= PERF_ATTACH_ITRACE;
}

static void perf_log_itrace_start(struct perf_event *event)
{
	struct perf_output_handle handle;
	struct perf_sample_data sample;
	struct perf_aux_event {
		struct perf_event_header        header;
		u32				pid;
		u32				tid;
	} rec;
	int ret;

	if (event->parent)
		event = event->parent;

	if (!(event->pmu->capabilities & PERF_PMU_CAP_ITRACE) ||
	    event->attach_state & PERF_ATTACH_ITRACE)
		return;

	rec.header.type	= PERF_RECORD_ITRACE_START;
	rec.header.misc	= 0;
	rec.header.size	= sizeof(rec);
	rec.pid	= perf_event_pid(event, current);
	rec.tid	= perf_event_tid(event, current);

	perf_event_header__init_id(&rec.header, &sample, event);
	ret = perf_output_begin(&handle, &sample, event, rec.header.size);

	if (ret)
		return;

	perf_output_put(&handle, rec);
	perf_event__output_id_sample(event, &handle, &sample);

	perf_output_end(&handle);
}

void perf_report_aux_output_id(struct perf_event *event, u64 hw_id)
{
	struct perf_output_handle handle;
	struct perf_sample_data sample;
	struct perf_aux_event {
		struct perf_event_header        header;
		u64				hw_id;
	} rec;
	int ret;

	if (event->parent)
		event = event->parent;

	rec.header.type	= PERF_RECORD_AUX_OUTPUT_HW_ID;
	rec.header.misc	= 0;
	rec.header.size	= sizeof(rec);
	rec.hw_id	= hw_id;

	perf_event_header__init_id(&rec.header, &sample, event);
	ret = perf_output_begin(&handle, &sample, event, rec.header.size);

	if (ret)
		return;

	perf_output_put(&handle, rec);
	perf_event__output_id_sample(event, &handle, &sample);

	perf_output_end(&handle);
}
EXPORT_SYMBOL_GPL(perf_report_aux_output_id);

static int
__perf_event_account_interrupt(struct perf_event *event, int throttle)
{
	struct hw_perf_event *hwc = &event->hw;
	int ret = 0;
	u64 seq;

	seq = __this_cpu_read(perf_throttled_seq);
	if (seq != hwc->interrupts_seq) {
		hwc->interrupts_seq = seq;
		hwc->interrupts = 1;
	} else {
		hwc->interrupts++;
	}

	if (unlikely(throttle && hwc->interrupts >= max_samples_per_tick)) {
		__this_cpu_inc(perf_throttled_count);
		tick_dep_set_cpu(smp_processor_id(), TICK_DEP_BIT_PERF_EVENTS);
		hwc->interrupts = MAX_INTERRUPTS;
		perf_log_throttle(event, 0);
		ret = 1;
	}

	if (event->attr.freq) {
		u64 now = perf_clock();
		s64 delta = now - hwc->freq_time_stamp;

		hwc->freq_time_stamp = now;

		if (delta > 0 && delta < 2*TICK_NSEC)
			perf_adjust_period(event, delta, hwc->last_period, true);
	}

	return ret;
}

int perf_event_account_interrupt(struct perf_event *event)
{
	return __perf_event_account_interrupt(event, 1);
}

static inline bool sample_is_allowed(struct perf_event *event, struct pt_regs *regs)
{
	/*
	 * Due to interrupt latency (AKA "skid"), we may enter the
	 * kernel before taking an overflow, even if the PMU is only
	 * counting user events.
	 */
	if (event->attr.exclude_kernel && !user_mode(regs))
		return false;

	return true;
}

#ifdef CONFIG_BPF_SYSCALL
static int bpf_overflow_handler(struct perf_event *event,
				struct perf_sample_data *data,
				struct pt_regs *regs)
{
	struct bpf_perf_event_data_kern ctx = {
		.data = data,
		.event = event,
	};
	struct bpf_prog *prog;
	int ret = 0;

	ctx.regs = perf_arch_bpf_user_pt_regs(regs);
	if (unlikely(__this_cpu_inc_return(bpf_prog_active) != 1))
		goto out;
	rcu_read_lock();
	prog = READ_ONCE(event->prog);
	if (prog) {
		perf_prepare_sample(data, event, regs);
		ret = bpf_prog_run(prog, &ctx);
	}
	rcu_read_unlock();
out:
	__this_cpu_dec(bpf_prog_active);

	return ret;
}

static inline int perf_event_set_bpf_handler(struct perf_event *event,
					     struct bpf_prog *prog,
					     u64 bpf_cookie)
{
	if (event->overflow_handler_context)
		/* hw breakpoint or kernel counter */
		return -EINVAL;

	if (event->prog)
		return -EEXIST;

	if (prog->type != BPF_PROG_TYPE_PERF_EVENT)
		return -EINVAL;

	if (event->attr.precise_ip &&
	    prog->call_get_stack &&
	    (!(event->attr.sample_type & PERF_SAMPLE_CALLCHAIN) ||
	     event->attr.exclude_callchain_kernel ||
	     event->attr.exclude_callchain_user)) {
		/*
		 * On perf_event with precise_ip, calling bpf_get_stack()
		 * may trigger unwinder warnings and occasional crashes.
		 * bpf_get_[stack|stackid] works around this issue by using
		 * callchain attached to perf_sample_data. If the
		 * perf_event does not full (kernel and user) callchain
		 * attached to perf_sample_data, do not allow attaching BPF
		 * program that calls bpf_get_[stack|stackid].
		 */
		return -EPROTO;
	}

	event->prog = prog;
	event->bpf_cookie = bpf_cookie;
	return 0;
}

static inline void perf_event_free_bpf_handler(struct perf_event *event)
{
	struct bpf_prog *prog = event->prog;

	if (!prog)
		return;

	event->prog = NULL;
	bpf_prog_put(prog);
}
#else
static inline int bpf_overflow_handler(struct perf_event *event,
				       struct perf_sample_data *data,
				       struct pt_regs *regs)
{
	return 1;
}

static inline int perf_event_set_bpf_handler(struct perf_event *event,
					     struct bpf_prog *prog,
					     u64 bpf_cookie)
{
	return -EOPNOTSUPP;
}

static inline void perf_event_free_bpf_handler(struct perf_event *event)
{
}
#endif

/*
 * Generic event overflow handling, sampling.
 */

static int __perf_event_overflow(struct perf_event *event,
				 int throttle, struct perf_sample_data *data,
				 struct pt_regs *regs)
{
	int events = atomic_read(&event->event_limit);
	int ret = 0;

	/*
	 * Non-sampling counters might still use the PMI to fold short
	 * hardware counters, ignore those.
	 */
	if (unlikely(!is_sampling_event(event)))
		return 0;

	ret = __perf_event_account_interrupt(event, throttle);

	if (event->attr.aux_pause)
		perf_event_aux_pause(event->aux_event, true);

	if (event->prog && event->prog->type == BPF_PROG_TYPE_PERF_EVENT &&
	    !bpf_overflow_handler(event, data, regs))
		goto out;

	/*
	 * XXX event_limit might not quite work as expected on inherited
	 * events
	 */

	event->pending_kill = POLL_IN;
	if (events && atomic_dec_and_test(&event->event_limit)) {
		ret = 1;
		event->pending_kill = POLL_HUP;
		perf_event_disable_inatomic(event);
	}

	if (event->attr.sigtrap) {
		/*
		 * The desired behaviour of sigtrap vs invalid samples is a bit
		 * tricky; on the one hand, one should not loose the SIGTRAP if
		 * it is the first event, on the other hand, we should also not
		 * trigger the WARN or override the data address.
		 */
		bool valid_sample = sample_is_allowed(event, regs);
		unsigned int pending_id = 1;
		enum task_work_notify_mode notify_mode;

		if (regs)
			pending_id = hash32_ptr((void *)instruction_pointer(regs)) ?: 1;

		notify_mode = in_nmi() ? TWA_NMI_CURRENT : TWA_RESUME;

		if (!event->pending_work &&
		    !task_work_add(current, &event->pending_task, notify_mode)) {
			event->pending_work = pending_id;
			local_inc(&event->ctx->nr_no_switch_fast);
			WARN_ON_ONCE(!atomic_long_inc_not_zero(&event->refcount));

			event->pending_addr = 0;
			if (valid_sample && (data->sample_flags & PERF_SAMPLE_ADDR))
				event->pending_addr = data->addr;

		} else if (event->attr.exclude_kernel && valid_sample) {
			/*
			 * Should not be able to return to user space without
			 * consuming pending_work; with exceptions:
			 *
			 *  1. Where !exclude_kernel, events can overflow again
			 *     in the kernel without returning to user space.
			 *
			 *  2. Events that can overflow again before the IRQ-
			 *     work without user space progress (e.g. hrtimer).
			 *     To approximate progress (with false negatives),
			 *     check 32-bit hash of the current IP.
			 */
			WARN_ON_ONCE(event->pending_work != pending_id);
		}
	}

	READ_ONCE(event->overflow_handler)(event, data, regs);

	if (*perf_event_fasync(event) && event->pending_kill) {
		event->pending_wakeup = 1;
		irq_work_queue(&event->pending_irq);
	}
out:
	if (event->attr.aux_resume)
		perf_event_aux_pause(event->aux_event, false);

	return ret;
}

int perf_event_overflow(struct perf_event *event,
			struct perf_sample_data *data,
			struct pt_regs *regs)
{
	return __perf_event_overflow(event, 1, data, regs);
}

/*
 * Generic software event infrastructure
 */

struct swevent_htable {
	struct swevent_hlist		*swevent_hlist;
	struct mutex			hlist_mutex;
	int				hlist_refcount;
};
static DEFINE_PER_CPU(struct swevent_htable, swevent_htable);

/*
 * We directly increment event->count and keep a second value in
 * event->hw.period_left to count intervals. This period event
 * is kept in the range [-sample_period, 0] so that we can use the
 * sign as trigger.
 */

u64 perf_swevent_set_period(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	u64 period = hwc->last_period;
	u64 nr, offset;
	s64 old, val;

	hwc->last_period = hwc->sample_period;

	old = local64_read(&hwc->period_left);
	do {
		val = old;
		if (val < 0)
			return 0;

		nr = div64_u64(period + val, period);
		offset = nr * period;
		val -= offset;
	} while (!local64_try_cmpxchg(&hwc->period_left, &old, val));

	return nr;
}

static void perf_swevent_overflow(struct perf_event *event, u64 overflow,
				    struct perf_sample_data *data,
				    struct pt_regs *regs)
{
	struct hw_perf_event *hwc = &event->hw;
	int throttle = 0;

	if (!overflow)
		overflow = perf_swevent_set_period(event);

	if (hwc->interrupts == MAX_INTERRUPTS)
		return;

	for (; overflow; overflow--) {
		if (__perf_event_overflow(event, throttle,
					    data, regs)) {
			/*
			 * We inhibit the overflow from happening when
			 * hwc->interrupts == MAX_INTERRUPTS.
			 */
			break;
		}
		throttle = 1;
	}
}

static void perf_swevent_event(struct perf_event *event, u64 nr,
			       struct perf_sample_data *data,
			       struct pt_regs *regs)
{
	struct hw_perf_event *hwc = &event->hw;

	local64_add(nr, &event->count);

	if (!regs)
		return;

	if (!is_sampling_event(event))
		return;

	if ((event->attr.sample_type & PERF_SAMPLE_PERIOD) && !event->attr.freq) {
		data->period = nr;
		return perf_swevent_overflow(event, 1, data, regs);
	} else
		data->period = event->hw.last_period;

	if (nr == 1 && hwc->sample_period == 1 && !event->attr.freq)
		return perf_swevent_overflow(event, 1, data, regs);

	if (local64_add_negative(nr, &hwc->period_left))
		return;

	perf_swevent_overflow(event, 0, data, regs);
}

int perf_exclude_event(struct perf_event *event, struct pt_regs *regs)
{
	if (event->hw.state & PERF_HES_STOPPED)
		return 1;

	if (regs) {
		if (event->attr.exclude_user && user_mode(regs))
			return 1;

		if (event->attr.exclude_kernel && !user_mode(regs))
			return 1;
	}

	return 0;
}

static int perf_swevent_match(struct perf_event *event,
				enum perf_type_id type,
				u32 event_id,
				struct perf_sample_data *data,
				struct pt_regs *regs)
{
	if (event->attr.type != type)
		return 0;

	if (event->attr.config != event_id)
		return 0;

	if (perf_exclude_event(event, regs))
		return 0;

	return 1;
}

static inline u64 swevent_hash(u64 type, u32 event_id)
{
	u64 val = event_id | (type << 32);

	return hash_64(val, SWEVENT_HLIST_BITS);
}

static inline struct hlist_head *
__find_swevent_head(struct swevent_hlist *hlist, u64 type, u32 event_id)
{
	u64 hash = swevent_hash(type, event_id);

	return &hlist->heads[hash];
}

/* For the read side: events when they trigger */
static inline struct hlist_head *
find_swevent_head_rcu(struct swevent_htable *swhash, u64 type, u32 event_id)
{
	struct swevent_hlist *hlist;

	hlist = rcu_dereference(swhash->swevent_hlist);
	if (!hlist)
		return NULL;

	return __find_swevent_head(hlist, type, event_id);
}

/* For the event head insertion and removal in the hlist */
static inline struct hlist_head *
find_swevent_head(struct swevent_htable *swhash, struct perf_event *event)
{
	struct swevent_hlist *hlist;
	u32 event_id = event->attr.config;
	u64 type = event->attr.type;

	/*
	 * Event scheduling is always serialized against hlist allocation
	 * and release. Which makes the protected version suitable here.
	 * The context lock guarantees that.
	 */
	hlist = rcu_dereference_protected(swhash->swevent_hlist,
					  lockdep_is_held(&event->ctx->lock));
	if (!hlist)
		return NULL;

	return __find_swevent_head(hlist, type, event_id);
}

static void do_perf_sw_event(enum perf_type_id type, u32 event_id,
				    u64 nr,
				    struct perf_sample_data *data,
				    struct pt_regs *regs)
{
	struct swevent_htable *swhash = this_cpu_ptr(&swevent_htable);
	struct perf_event *event;
	struct hlist_head *head;

	rcu_read_lock();
	head = find_swevent_head_rcu(swhash, type, event_id);
	if (!head)
		goto end;

	hlist_for_each_entry_rcu(event, head, hlist_entry) {
		if (perf_swevent_match(event, type, event_id, data, regs))
			perf_swevent_event(event, nr, data, regs);
	}
end:
	rcu_read_unlock();
}

DEFINE_PER_CPU(struct pt_regs, __perf_regs[4]);

int perf_swevent_get_recursion_context(void)
{
	return get_recursion_context(current->perf_recursion);
}
EXPORT_SYMBOL_GPL(perf_swevent_get_recursion_context);

void perf_swevent_put_recursion_context(int rctx)
{
	put_recursion_context(current->perf_recursion, rctx);
}

void ___perf_sw_event(u32 event_id, u64 nr, struct pt_regs *regs, u64 addr)
{
	struct perf_sample_data data;

	if (WARN_ON_ONCE(!regs))
		return;

	perf_sample_data_init(&data, addr, 0);
	do_perf_sw_event(PERF_TYPE_SOFTWARE, event_id, nr, &data, regs);
}

void __perf_sw_event(u32 event_id, u64 nr, struct pt_regs *regs, u64 addr)
{
	int rctx;

	preempt_disable_notrace();
	rctx = perf_swevent_get_recursion_context();
	if (unlikely(rctx < 0))
		goto fail;

	___perf_sw_event(event_id, nr, regs, addr);

	perf_swevent_put_recursion_context(rctx);
fail:
	preempt_enable_notrace();
}

static void perf_swevent_read(struct perf_event *event)
{
}

static int perf_swevent_add(struct perf_event *event, int flags)
{
	struct swevent_htable *swhash = this_cpu_ptr(&swevent_htable);
	struct hw_perf_event *hwc = &event->hw;
	struct hlist_head *head;

	if (is_sampling_event(event)) {
		hwc->last_period = hwc->sample_period;
		perf_swevent_set_period(event);
	}

	hwc->state = !(flags & PERF_EF_START);

	head = find_swevent_head(swhash, event);
	if (WARN_ON_ONCE(!head))
		return -EINVAL;

	hlist_add_head_rcu(&event->hlist_entry, head);
	perf_event_update_userpage(event);

	return 0;
}

static void perf_swevent_del(struct perf_event *event, int flags)
{
	hlist_del_rcu(&event->hlist_entry);
}

static void perf_swevent_start(struct perf_event *event, int flags)
{
	event->hw.state = 0;
}

static void perf_swevent_stop(struct perf_event *event, int flags)
{
	event->hw.state = PERF_HES_STOPPED;
}

/* Deref the hlist from the update side */
static inline struct swevent_hlist *
swevent_hlist_deref(struct swevent_htable *swhash)
{
	return rcu_dereference_protected(swhash->swevent_hlist,
					 lockdep_is_held(&swhash->hlist_mutex));
}

static void swevent_hlist_release(struct swevent_htable *swhash)
{
	struct swevent_hlist *hlist = swevent_hlist_deref(swhash);

	if (!hlist)
		return;

	RCU_INIT_POINTER(swhash->swevent_hlist, NULL);
	kfree_rcu(hlist, rcu_head);
}

static void swevent_hlist_put_cpu(int cpu)
{
	struct swevent_htable *swhash = &per_cpu(swevent_htable, cpu);

	mutex_lock(&swhash->hlist_mutex);

	if (!--swhash->hlist_refcount)
		swevent_hlist_release(swhash);

	mutex_unlock(&swhash->hlist_mutex);
}

static void swevent_hlist_put(void)
{
	int cpu;

	for_each_possible_cpu(cpu)
		swevent_hlist_put_cpu(cpu);
}

static int swevent_hlist_get_cpu(int cpu)
{
	struct swevent_htable *swhash = &per_cpu(swevent_htable, cpu);
	int err = 0;

	mutex_lock(&swhash->hlist_mutex);
	if (!swevent_hlist_deref(swhash) &&
	    cpumask_test_cpu(cpu, perf_online_mask)) {
		struct swevent_hlist *hlist;

		hlist = kzalloc(sizeof(*hlist), GFP_KERNEL);
		if (!hlist) {
			err = -ENOMEM;
			goto exit;
		}
		rcu_assign_pointer(swhash->swevent_hlist, hlist);
	}
	swhash->hlist_refcount++;
exit:
	mutex_unlock(&swhash->hlist_mutex);

	return err;
}

static int swevent_hlist_get(void)
{
	int err, cpu, failed_cpu;

	mutex_lock(&pmus_lock);
	for_each_possible_cpu(cpu) {
		err = swevent_hlist_get_cpu(cpu);
		if (err) {
			failed_cpu = cpu;
			goto fail;
		}
	}
	mutex_unlock(&pmus_lock);
	return 0;
fail:
	for_each_possible_cpu(cpu) {
		if (cpu == failed_cpu)
			break;
		swevent_hlist_put_cpu(cpu);
	}
	mutex_unlock(&pmus_lock);
	return err;
}

struct static_key perf_swevent_enabled[PERF_COUNT_SW_MAX];

static void sw_perf_event_destroy(struct perf_event *event)
{
	u64 event_id = event->attr.config;

	WARN_ON(event->parent);

	static_key_slow_dec(&perf_swevent_enabled[event_id]);
	swevent_hlist_put();
}

static struct pmu perf_cpu_clock; /* fwd declaration */
static struct pmu perf_task_clock;

static int perf_swevent_init(struct perf_event *event)
{
	u64 event_id = event->attr.config;

	if (event->attr.type != PERF_TYPE_SOFTWARE)
		return -ENOENT;

	/*
	 * no branch sampling for software events
	 */
	if (has_branch_stack(event))
		return -EOPNOTSUPP;

	switch (event_id) {
	case PERF_COUNT_SW_CPU_CLOCK:
		event->attr.type = perf_cpu_clock.type;
		return -ENOENT;
	case PERF_COUNT_SW_TASK_CLOCK:
		event->attr.type = perf_task_clock.type;
		return -ENOENT;

	default:
		break;
	}

	if (event_id >= PERF_COUNT_SW_MAX)
		return -ENOENT;

	if (!event->parent) {
		int err;

		err = swevent_hlist_get();
		if (err)
			return err;

		static_key_slow_inc(&perf_swevent_enabled[event_id]);
		event->destroy = sw_perf_event_destroy;
	}

	return 0;
}

static struct pmu perf_swevent = {
	.task_ctx_nr	= perf_sw_context,

	.capabilities	= PERF_PMU_CAP_NO_NMI,

	.event_init	= perf_swevent_init,
	.add		= perf_swevent_add,
	.del		= perf_swevent_del,
	.start		= perf_swevent_start,
	.stop		= perf_swevent_stop,
	.read		= perf_swevent_read,
};

#ifdef CONFIG_EVENT_TRACING

static void tp_perf_event_destroy(struct perf_event *event)
{
	perf_trace_destroy(event);
}

static int perf_tp_event_init(struct perf_event *event)
{
	int err;

	if (event->attr.type != PERF_TYPE_TRACEPOINT)
		return -ENOENT;

	/*
	 * no branch sampling for tracepoint events
	 */
	if (has_branch_stack(event))
		return -EOPNOTSUPP;

	err = perf_trace_init(event);
	if (err)
		return err;

	event->destroy = tp_perf_event_destroy;

	return 0;
}

static struct pmu perf_tracepoint = {
	.task_ctx_nr	= perf_sw_context,

	.event_init	= perf_tp_event_init,
	.add		= perf_trace_add,
	.del		= perf_trace_del,
	.start		= perf_swevent_start,
	.stop		= perf_swevent_stop,
	.read		= perf_swevent_read,
};

static int perf_tp_filter_match(struct perf_event *event,
				struct perf_raw_record *raw)
{
	void *record = raw->frag.data;

	/* only top level events have filters set */
	if (event->parent)
		event = event->parent;

	if (likely(!event->filter) || filter_match_preds(event->filter, record))
		return 1;
	return 0;
}

static int perf_tp_event_match(struct perf_event *event,
				struct perf_raw_record *raw,
				struct pt_regs *regs)
{
	if (event->hw.state & PERF_HES_STOPPED)
		return 0;
	/*
	 * If exclude_kernel, only trace user-space tracepoints (uprobes)
	 */
	if (event->attr.exclude_kernel && !user_mode(regs))
		return 0;

	if (!perf_tp_filter_match(event, raw))
		return 0;

	return 1;
}

void perf_trace_run_bpf_submit(void *raw_data, int size, int rctx,
			       struct trace_event_call *call, u64 count,
			       struct pt_regs *regs, struct hlist_head *head,
			       struct task_struct *task)
{
	if (bpf_prog_array_valid(call)) {
		*(struct pt_regs **)raw_data = regs;
		if (!trace_call_bpf(call, raw_data) || hlist_empty(head)) {
			perf_swevent_put_recursion_context(rctx);
			return;
		}
	}
	perf_tp_event(call->event.type, count, raw_data, size, regs, head,
		      rctx, task);
}
EXPORT_SYMBOL_GPL(perf_trace_run_bpf_submit);

static void __perf_tp_event_target_task(u64 count, void *record,
					struct pt_regs *regs,
					struct perf_sample_data *data,
					struct perf_raw_record *raw,
					struct perf_event *event)
{
	struct trace_entry *entry = record;

	if (event->attr.config != entry->type)
		return;
	/* Cannot deliver synchronous signal to other task. */
	if (event->attr.sigtrap)
		return;
	if (perf_tp_event_match(event, raw, regs)) {
		perf_sample_data_init(data, 0, 0);
		perf_sample_save_raw_data(data, event, raw);
		perf_swevent_event(event, count, data, regs);
	}
}

static void perf_tp_event_target_task(u64 count, void *record,
				      struct pt_regs *regs,
				      struct perf_sample_data *data,
				      struct perf_raw_record *raw,
				      struct perf_event_context *ctx)
{
	unsigned int cpu = smp_processor_id();
	struct pmu *pmu = &perf_tracepoint;
	struct perf_event *event, *sibling;

	perf_event_groups_for_cpu_pmu(event, &ctx->pinned_groups, cpu, pmu) {
		__perf_tp_event_target_task(count, record, regs, data, raw, event);
		for_each_sibling_event(sibling, event)
			__perf_tp_event_target_task(count, record, regs, data, raw, sibling);
	}

	perf_event_groups_for_cpu_pmu(event, &ctx->flexible_groups, cpu, pmu) {
		__perf_tp_event_target_task(count, record, regs, data, raw, event);
		for_each_sibling_event(sibling, event)
			__perf_tp_event_target_task(count, record, regs, data, raw, sibling);
	}
}

void perf_tp_event(u16 event_type, u64 count, void *record, int entry_size,
		   struct pt_regs *regs, struct hlist_head *head, int rctx,
		   struct task_struct *task)
{
	struct perf_sample_data data;
	struct perf_event *event;

	struct perf_raw_record raw = {
		.frag = {
			.size = entry_size,
			.data = record,
		},
	};

	perf_trace_buf_update(record, event_type);

	hlist_for_each_entry_rcu(event, head, hlist_entry) {
		if (perf_tp_event_match(event, &raw, regs)) {
			/*
			 * Here use the same on-stack perf_sample_data,
			 * some members in data are event-specific and
			 * need to be re-computed for different sweveents.
			 * Re-initialize data->sample_flags safely to avoid
			 * the problem that next event skips preparing data
			 * because data->sample_flags is set.
			 */
			perf_sample_data_init(&data, 0, 0);
			perf_sample_save_raw_data(&data, event, &raw);
			perf_swevent_event(event, count, &data, regs);
		}
	}

	/*
	 * If we got specified a target task, also iterate its context and
	 * deliver this event there too.
	 */
	if (task && task != current) {
		struct perf_event_context *ctx;

		rcu_read_lock();
		ctx = rcu_dereference(task->perf_event_ctxp);
		if (!ctx)
			goto unlock;

		raw_spin_lock(&ctx->lock);
		perf_tp_event_target_task(count, record, regs, &data, &raw, ctx);
		raw_spin_unlock(&ctx->lock);
unlock:
		rcu_read_unlock();
	}

	perf_swevent_put_recursion_context(rctx);
}
EXPORT_SYMBOL_GPL(perf_tp_event);

#if defined(CONFIG_KPROBE_EVENTS) || defined(CONFIG_UPROBE_EVENTS)
/*
 * Flags in config, used by dynamic PMU kprobe and uprobe
 * The flags should match following PMU_FORMAT_ATTR().
 *
 * PERF_PROBE_CONFIG_IS_RETPROBE if set, create kretprobe/uretprobe
 *                               if not set, create kprobe/uprobe
 *
 * The following values specify a reference counter (or semaphore in the
 * terminology of tools like dtrace, systemtap, etc.) Userspace Statically
 * Defined Tracepoints (USDT). Currently, we use 40 bit for the offset.
 *
 * PERF_UPROBE_REF_CTR_OFFSET_BITS	# of bits in config as th offset
 * PERF_UPROBE_REF_CTR_OFFSET_SHIFT	# of bits to shift left
 */
enum perf_probe_config {
	PERF_PROBE_CONFIG_IS_RETPROBE = 1U << 0,  /* [k,u]retprobe */
	PERF_UPROBE_REF_CTR_OFFSET_BITS = 32,
	PERF_UPROBE_REF_CTR_OFFSET_SHIFT = 64 - PERF_UPROBE_REF_CTR_OFFSET_BITS,
};

PMU_FORMAT_ATTR(retprobe, "config:0");
#endif

#ifdef CONFIG_KPROBE_EVENTS
static struct attribute *kprobe_attrs[] = {
	&format_attr_retprobe.attr,
	NULL,
};

static struct attribute_group kprobe_format_group = {
	.name = "format",
	.attrs = kprobe_attrs,
};

static const struct attribute_group *kprobe_attr_groups[] = {
	&kprobe_format_group,
	NULL,
};

static int perf_kprobe_event_init(struct perf_event *event);
static struct pmu perf_kprobe = {
	.task_ctx_nr	= perf_sw_context,
	.event_init	= perf_kprobe_event_init,
	.add		= perf_trace_add,
	.del		= perf_trace_del,
	.start		= perf_swevent_start,
	.stop		= perf_swevent_stop,
	.read		= perf_swevent_read,
	.attr_groups	= kprobe_attr_groups,
};

static int perf_kprobe_event_init(struct perf_event *event)
{
	int err;
	bool is_retprobe;

	if (event->attr.type != perf_kprobe.type)
		return -ENOENT;

	if (!perfmon_capable())
		return -EACCES;

	/*
	 * no branch sampling for probe events
	 */
	if (has_branch_stack(event))
		return -EOPNOTSUPP;

	is_retprobe = event->attr.config & PERF_PROBE_CONFIG_IS_RETPROBE;
	err = perf_kprobe_init(event, is_retprobe);
	if (err)
		return err;

	event->destroy = perf_kprobe_destroy;

	return 0;
}
#endif /* CONFIG_KPROBE_EVENTS */

#ifdef CONFIG_UPROBE_EVENTS
PMU_FORMAT_ATTR(ref_ctr_offset, "config:32-63");

static struct attribute *uprobe_attrs[] = {
	&format_attr_retprobe.attr,
	&format_attr_ref_ctr_offset.attr,
	NULL,
};

static struct attribute_group uprobe_format_group = {
	.name = "format",
	.attrs = uprobe_attrs,
};

static const struct attribute_group *uprobe_attr_groups[] = {
	&uprobe_format_group,
	NULL,
};

static int perf_uprobe_event_init(struct perf_event *event);
static struct pmu perf_uprobe = {
	.task_ctx_nr	= perf_sw_context,
	.event_init	= perf_uprobe_event_init,
	.add		= perf_trace_add,
	.del		= perf_trace_del,
	.start		= perf_swevent_start,
	.stop		= perf_swevent_stop,
	.read		= perf_swevent_read,
	.attr_groups	= uprobe_attr_groups,
};

static int perf_uprobe_event_init(struct perf_event *event)
{
	int err;
	unsigned long ref_ctr_offset;
	bool is_retprobe;

	if (event->attr.type != perf_uprobe.type)
		return -ENOENT;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	/*
	 * no branch sampling for probe events
	 */
	if (has_branch_stack(event))
		return -EOPNOTSUPP;

	is_retprobe = event->attr.config & PERF_PROBE_CONFIG_IS_RETPROBE;
	ref_ctr_offset = event->attr.config >> PERF_UPROBE_REF_CTR_OFFSET_SHIFT;
	err = perf_uprobe_init(event, ref_ctr_offset, is_retprobe);
	if (err)
		return err;

	event->destroy = perf_uprobe_destroy;

	return 0;
}
#endif /* CONFIG_UPROBE_EVENTS */

static inline void perf_tp_register(void)
{
	perf_pmu_register(&perf_tracepoint, "tracepoint", PERF_TYPE_TRACEPOINT);
#ifdef CONFIG_KPROBE_EVENTS
	perf_pmu_register(&perf_kprobe, "kprobe", -1);
#endif
#ifdef CONFIG_UPROBE_EVENTS
	perf_pmu_register(&perf_uprobe, "uprobe", -1);
#endif
}

static void perf_event_free_filter(struct perf_event *event)
{
	ftrace_profile_free_filter(event);
}

/*
 * returns true if the event is a tracepoint, or a kprobe/upprobe created
 * with perf_event_open()
 */
static inline bool perf_event_is_tracing(struct perf_event *event)
{
	if (event->pmu == &perf_tracepoint)
		return true;
#ifdef CONFIG_KPROBE_EVENTS
	if (event->pmu == &perf_kprobe)
		return true;
#endif
#ifdef CONFIG_UPROBE_EVENTS
	if (event->pmu == &perf_uprobe)
		return true;
#endif
	return false;
}

static int __perf_event_set_bpf_prog(struct perf_event *event,
				     struct bpf_prog *prog,
				     u64 bpf_cookie)
{
	bool is_kprobe, is_uprobe, is_tracepoint, is_syscall_tp;

	if (!perf_event_is_tracing(event))
		return perf_event_set_bpf_handler(event, prog, bpf_cookie);

	is_kprobe = event->tp_event->flags & TRACE_EVENT_FL_KPROBE;
	is_uprobe = event->tp_event->flags & TRACE_EVENT_FL_UPROBE;
	is_tracepoint = event->tp_event->flags & TRACE_EVENT_FL_TRACEPOINT;
	is_syscall_tp = is_syscall_trace_event(event->tp_event);
	if (!is_kprobe && !is_uprobe && !is_tracepoint && !is_syscall_tp)
		/* bpf programs can only be attached to u/kprobe or tracepoint */
		return -EINVAL;

	if (((is_kprobe || is_uprobe) && prog->type != BPF_PROG_TYPE_KPROBE) ||
	    (is_tracepoint && prog->type != BPF_PROG_TYPE_TRACEPOINT) ||
	    (is_syscall_tp && prog->type != BPF_PROG_TYPE_TRACEPOINT))
		return -EINVAL;

	if (prog->type == BPF_PROG_TYPE_KPROBE && prog->sleepable && !is_uprobe)
		/* only uprobe programs are allowed to be sleepable */
		return -EINVAL;

	/* Kprobe override only works for kprobes, not uprobes. */
	if (prog->kprobe_override && !is_kprobe)
		return -EINVAL;

	if (is_tracepoint || is_syscall_tp) {
		int off = trace_event_get_offsets(event->tp_event);

		if (prog->aux->max_ctx_offset > off)
			return -EACCES;
	}

	return perf_event_attach_bpf_prog(event, prog, bpf_cookie);
}

int perf_event_set_bpf_prog(struct perf_event *event,
			    struct bpf_prog *prog,
			    u64 bpf_cookie)
{
	struct perf_event_context *ctx;
	int ret;

	ctx = perf_event_ctx_lock(event);
	ret = __perf_event_set_bpf_prog(event, prog, bpf_cookie);
	perf_event_ctx_unlock(event, ctx);

	return ret;
}

void perf_event_free_bpf_prog(struct perf_event *event)
{
	if (!event->prog)
		return;

	if (!perf_event_is_tracing(event)) {
		perf_event_free_bpf_handler(event);
		return;
	}
	perf_event_detach_bpf_prog(event);
}

#else

static inline void perf_tp_register(void)
{
}

static void perf_event_free_filter(struct perf_event *event)
{
}

static int __perf_event_set_bpf_prog(struct perf_event *event,
				     struct bpf_prog *prog,
				     u64 bpf_cookie)
{
	return -ENOENT;
}

int perf_event_set_bpf_prog(struct perf_event *event,
			    struct bpf_prog *prog,
			    u64 bpf_cookie)
{
	return -ENOENT;
}

void perf_event_free_bpf_prog(struct perf_event *event)
{
}
#endif /* CONFIG_EVENT_TRACING */

#ifdef CONFIG_HAVE_HW_BREAKPOINT
void perf_bp_event(struct perf_event *bp, void *data)
{
	struct perf_sample_data sample;
	struct pt_regs *regs = data;

	perf_sample_data_init(&sample, bp->attr.bp_addr, 0);

	if (!bp->hw.state && !perf_exclude_event(bp, regs))
		perf_swevent_event(bp, 1, &sample, regs);
}
#endif

/*
 * Allocate a new address filter
 */
static struct perf_addr_filter *
perf_addr_filter_new(struct perf_event *event, struct list_head *filters)
{
	int node = cpu_to_node(event->cpu == -1 ? 0 : event->cpu);
	struct perf_addr_filter *filter;

	filter = kzalloc_node(sizeof(*filter), GFP_KERNEL, node);
	if (!filter)
		return NULL;

	INIT_LIST_HEAD(&filter->entry);
	list_add_tail(&filter->entry, filters);

	return filter;
}

static void free_filters_list(struct list_head *filters)
{
	struct perf_addr_filter *filter, *iter;

	list_for_each_entry_safe(filter, iter, filters, entry) {
		path_put(&filter->path);
		list_del(&filter->entry);
		kfree(filter);
	}
}

/*
 * Free existing address filters and optionally install new ones
 */
static void perf_addr_filters_splice(struct perf_event *event,
				     struct list_head *head)
{
	unsigned long flags;
	LIST_HEAD(list);

	if (!has_addr_filter(event))
		return;

	/* don't bother with children, they don't have their own filters */
	if (event->parent)
		return;

	raw_spin_lock_irqsave(&event->addr_filters.lock, flags);

	list_splice_init(&event->addr_filters.list, &list);
	if (head)
		list_splice(head, &event->addr_filters.list);

	raw_spin_unlock_irqrestore(&event->addr_filters.lock, flags);

	free_filters_list(&list);
}

static void perf_free_addr_filters(struct perf_event *event)
{
	/*
	 * Used during free paths, there is no concurrency.
	 */
	if (list_empty(&event->addr_filters.list))
		return;

	perf_addr_filters_splice(event, NULL);
}

/*
 * Scan through mm's vmas and see if one of them matches the
 * @filter; if so, adjust filter's address range.
 * Called with mm::mmap_lock down for reading.
 */
static void perf_addr_filter_apply(struct perf_addr_filter *filter,
				   struct mm_struct *mm,
				   struct perf_addr_filter_range *fr)
{
	struct vm_area_struct *vma;
	VMA_ITERATOR(vmi, mm, 0);

	for_each_vma(vmi, vma) {
		if (!vma->vm_file)
			continue;

		if (perf_addr_filter_vma_adjust(filter, vma, fr))
			return;
	}
}

/*
 * Update event's address range filters based on the
 * task's existing mappings, if any.
 */
static void perf_event_addr_filters_apply(struct perf_event *event)
{
	struct perf_addr_filters_head *ifh = perf_event_addr_filters(event);
	struct task_struct *task = READ_ONCE(event->ctx->task);
	struct perf_addr_filter *filter;
	struct mm_struct *mm = NULL;
	unsigned int count = 0;
	unsigned long flags;

	/*
	 * We may observe TASK_TOMBSTONE, which means that the event tear-down
	 * will stop on the parent's child_mutex that our caller is also holding
	 */
	if (task == TASK_TOMBSTONE)
		return;

	if (ifh->nr_file_filters) {
		mm = get_task_mm(task);
		if (!mm)
			goto restart;

		mmap_read_lock(mm);
	}

	raw_spin_lock_irqsave(&ifh->lock, flags);
	list_for_each_entry(filter, &ifh->list, entry) {
		if (filter->path.dentry) {
			/*
			 * Adjust base offset if the filter is associated to a
			 * binary that needs to be mapped:
			 */
			event->addr_filter_ranges[count].start = 0;
			event->addr_filter_ranges[count].size = 0;

			perf_addr_filter_apply(filter, mm, &event->addr_filter_ranges[count]);
		} else {
			event->addr_filter_ranges[count].start = filter->offset;
			event->addr_filter_ranges[count].size  = filter->size;
		}

		count++;
	}

	event->addr_filters_gen++;
	raw_spin_unlock_irqrestore(&ifh->lock, flags);

	if (ifh->nr_file_filters) {
		mmap_read_unlock(mm);

		mmput(mm);
	}

restart:
	perf_event_stop(event, 1);
}

/*
 * Address range filtering: limiting the data to certain
 * instruction address ranges. Filters are ioctl()ed to us from
 * userspace as ascii strings.
 *
 * Filter string format:
 *
 * ACTION RANGE_SPEC
 * where ACTION is one of the
 *  * "filter": limit the trace to this region
 *  * "start": start tracing from this address
 *  * "stop": stop tracing at this address/region;
 * RANGE_SPEC is
 *  * for kernel addresses: <start address>[/<size>]
 *  * for object files:     <start address>[/<size>]@</path/to/object/file>
 *
 * if <size> is not specified or is zero, the range is treated as a single
 * address; not valid for ACTION=="filter".
 */
enum {
	IF_ACT_NONE = -1,
	IF_ACT_FILTER,
	IF_ACT_START,
	IF_ACT_STOP,
	IF_SRC_FILE,
	IF_SRC_KERNEL,
	IF_SRC_FILEADDR,
	IF_SRC_KERNELADDR,
};

enum {
	IF_STATE_ACTION = 0,
	IF_STATE_SOURCE,
	IF_STATE_END,
};

static const match_table_t if_tokens = {
	{ IF_ACT_FILTER,	"filter" },
	{ IF_ACT_START,		"start" },
	{ IF_ACT_STOP,		"stop" },
	{ IF_SRC_FILE,		"%u/%u@%s" },
	{ IF_SRC_KERNEL,	"%u/%u" },
	{ IF_SRC_FILEADDR,	"%u@%s" },
	{ IF_SRC_KERNELADDR,	"%u" },
	{ IF_ACT_NONE,		NULL },
};

/*
 * Address filter string parser
 */
static int
perf_event_parse_addr_filter(struct perf_event *event, char *fstr,
			     struct list_head *filters)
{
	struct perf_addr_filter *filter = NULL;
	char *start, *orig, *filename = NULL;
	substring_t args[MAX_OPT_ARGS];
	int state = IF_STATE_ACTION, token;
	unsigned int kernel = 0;
	int ret = -EINVAL;

	orig = fstr = kstrdup(fstr, GFP_KERNEL);
	if (!fstr)
		return -ENOMEM;

	while ((start = strsep(&fstr, " ,\n")) != NULL) {
		static const enum perf_addr_filter_action_t actions[] = {
			[IF_ACT_FILTER]	= PERF_ADDR_FILTER_ACTION_FILTER,
			[IF_ACT_START]	= PERF_ADDR_FILTER_ACTION_START,
			[IF_ACT_STOP]	= PERF_ADDR_FILTER_ACTION_STOP,
		};
		ret = -EINVAL;

		if (!*start)
			continue;

		/* filter definition begins */
		if (state == IF_STATE_ACTION) {
			filter = perf_addr_filter_new(event, filters);
			if (!filter)
				goto fail;
		}

		token = match_token(start, if_tokens, args);
		switch (token) {
		case IF_ACT_FILTER:
		case IF_ACT_START:
		case IF_ACT_STOP:
			if (state != IF_STATE_ACTION)
				goto fail;

			filter->action = actions[token];
			state = IF_STATE_SOURCE;
			break;

		case IF_SRC_KERNELADDR:
		case IF_SRC_KERNEL:
			kernel = 1;
			fallthrough;

		case IF_SRC_FILEADDR:
		case IF_SRC_FILE:
			if (state != IF_STATE_SOURCE)
				goto fail;

			*args[0].to = 0;
			ret = kstrtoul(args[0].from, 0, &filter->offset);
			if (ret)
				goto fail;

			if (token == IF_SRC_KERNEL || token == IF_SRC_FILE) {
				*args[1].to = 0;
				ret = kstrtoul(args[1].from, 0, &filter->size);
				if (ret)
					goto fail;
			}

			if (token == IF_SRC_FILE || token == IF_SRC_FILEADDR) {
				int fpos = token == IF_SRC_FILE ? 2 : 1;

				kfree(filename);
				filename = match_strdup(&args[fpos]);
				if (!filename) {
					ret = -ENOMEM;
					goto fail;
				}
			}

			state = IF_STATE_END;
			break;

		default:
			goto fail;
		}

		/*
		 * Filter definition is fully parsed, validate and install it.
		 * Make sure that it doesn't contradict itself or the event's
		 * attribute.
		 */
		if (state == IF_STATE_END) {
			ret = -EINVAL;

			/*
			 * ACTION "filter" must have a non-zero length region
			 * specified.
			 */
			if (filter->action == PERF_ADDR_FILTER_ACTION_FILTER &&
			    !filter->size)
				goto fail;

			if (!kernel) {
				if (!filename)
					goto fail;

				/*
				 * For now, we only support file-based filters
				 * in per-task events; doing so for CPU-wide
				 * events requires additional context switching
				 * trickery, since same object code will be
				 * mapped at different virtual addresses in
				 * different processes.
				 */
				ret = -EOPNOTSUPP;
				if (!event->ctx->task)
					goto fail;

				/* look up the path and grab its inode */
				ret = kern_path(filename, LOOKUP_FOLLOW,
						&filter->path);
				if (ret)
					goto fail;

				ret = -EINVAL;
				if (!filter->path.dentry ||
				    !S_ISREG(d_inode(filter->path.dentry)
					     ->i_mode))
					goto fail;

				event->addr_filters.nr_file_filters++;
			}

			/* ready to consume more filters */
			kfree(filename);
			filename = NULL;
			state = IF_STATE_ACTION;
			filter = NULL;
			kernel = 0;
		}
	}

	if (state != IF_STATE_ACTION)
		goto fail;

	kfree(filename);
	kfree(orig);

	return 0;

fail:
	kfree(filename);
	free_filters_list(filters);
	kfree(orig);

	return ret;
}

static int
perf_event_set_addr_filter(struct perf_event *event, char *filter_str)
{
	LIST_HEAD(filters);
	int ret;

	/*
	 * Since this is called in perf_ioctl() path, we're already holding
	 * ctx::mutex.
	 */
	lockdep_assert_held(&event->ctx->mutex);

	if (WARN_ON_ONCE(event->parent))
		return -EINVAL;

	ret = perf_event_parse_addr_filter(event, filter_str, &filters);
	if (ret)
		goto fail_clear_files;

	ret = event->pmu->addr_filters_validate(&filters);
	if (ret)
		goto fail_free_filters;

	/* remove existing filters, if any */
	perf_addr_filters_splice(event, &filters);

	/* install new filters */
	perf_event_for_each_child(event, perf_event_addr_filters_apply);

	return ret;

fail_free_filters:
	free_filters_list(&filters);

fail_clear_files:
	event->addr_filters.nr_file_filters = 0;

	return ret;
}

static int perf_event_set_filter(struct perf_event *event, void __user *arg)
{
	int ret = -EINVAL;
	char *filter_str;

	filter_str = strndup_user(arg, PAGE_SIZE);
	if (IS_ERR(filter_str))
		return PTR_ERR(filter_str);

#ifdef CONFIG_EVENT_TRACING
	if (perf_event_is_tracing(event)) {
		struct perf_event_context *ctx = event->ctx;

		/*
		 * Beware, here be dragons!!
		 *
		 * the tracepoint muck will deadlock against ctx->mutex, but
		 * the tracepoint stuff does not actually need it. So
		 * temporarily drop ctx->mutex. As per perf_event_ctx_lock() we
		 * already have a reference on ctx.
		 *
		 * This can result in event getting moved to a different ctx,
		 * but that does not affect the tracepoint state.
		 */
		mutex_unlock(&ctx->mutex);
		ret = ftrace_profile_set_filter(event, event->attr.config, filter_str);
		mutex_lock(&ctx->mutex);
	} else
#endif
	if (has_addr_filter(event))
		ret = perf_event_set_addr_filter(event, filter_str);

	kfree(filter_str);
	return ret;
}

/*
 * hrtimer based swevent callback
 */

static enum hrtimer_restart perf_swevent_hrtimer(struct hrtimer *hrtimer)
{
	enum hrtimer_restart ret = HRTIMER_RESTART;
	struct perf_sample_data data;
	struct pt_regs *regs;
	struct perf_event *event;
	u64 period;

	event = container_of(hrtimer, struct perf_event, hw.hrtimer);

	if (event->state != PERF_EVENT_STATE_ACTIVE)
		return HRTIMER_NORESTART;

	event->pmu->read(event);

	perf_sample_data_init(&data, 0, event->hw.last_period);
	regs = get_irq_regs();

	if (regs && !perf_exclude_event(event, regs)) {
		if (!(event->attr.exclude_idle && is_idle_task(current)))
			if (__perf_event_overflow(event, 1, &data, regs))
				ret = HRTIMER_NORESTART;
	}

	period = max_t(u64, 10000, event->hw.sample_period);
	hrtimer_forward_now(hrtimer, ns_to_ktime(period));

	return ret;
}

static void perf_swevent_start_hrtimer(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	s64 period;

	if (!is_sampling_event(event))
		return;

	period = local64_read(&hwc->period_left);
	if (period) {
		if (period < 0)
			period = 10000;

		local64_set(&hwc->period_left, 0);
	} else {
		period = max_t(u64, 10000, hwc->sample_period);
	}
	hrtimer_start(&hwc->hrtimer, ns_to_ktime(period),
		      HRTIMER_MODE_REL_PINNED_HARD);
}

static void perf_swevent_cancel_hrtimer(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;

	if (is_sampling_event(event)) {
		ktime_t remaining = hrtimer_get_remaining(&hwc->hrtimer);
		local64_set(&hwc->period_left, ktime_to_ns(remaining));

		hrtimer_cancel(&hwc->hrtimer);
	}
}

static void perf_swevent_init_hrtimer(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;

	if (!is_sampling_event(event))
		return;

	hrtimer_setup(&hwc->hrtimer, perf_swevent_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_HARD);

	/*
	 * Since hrtimers have a fixed rate, we can do a static freq->period
	 * mapping and avoid the whole period adjust feedback stuff.
	 */
	if (event->attr.freq) {
		long freq = event->attr.sample_freq;

		event->attr.sample_period = NSEC_PER_SEC / freq;
		hwc->sample_period = event->attr.sample_period;
		local64_set(&hwc->period_left, hwc->sample_period);
		hwc->last_period = hwc->sample_period;
		event->attr.freq = 0;
	}
}

/*
 * Software event: cpu wall time clock
 */

static void cpu_clock_event_update(struct perf_event *event)
{
	s64 prev;
	u64 now;

	now = local_clock();
	prev = local64_xchg(&event->hw.prev_count, now);
	local64_add(now - prev, &event->count);
}

static void cpu_clock_event_start(struct perf_event *event, int flags)
{
	local64_set(&event->hw.prev_count, local_clock());
	perf_swevent_start_hrtimer(event);
}

static void cpu_clock_event_stop(struct perf_event *event, int flags)
{
	perf_swevent_cancel_hrtimer(event);
	cpu_clock_event_update(event);
}

static int cpu_clock_event_add(struct perf_event *event, int flags)
{
	if (flags & PERF_EF_START)
		cpu_clock_event_start(event, flags);
	perf_event_update_userpage(event);

	return 0;
}

static void cpu_clock_event_del(struct perf_event *event, int flags)
{
	cpu_clock_event_stop(event, flags);
}

static void cpu_clock_event_read(struct perf_event *event)
{
	cpu_clock_event_update(event);
}

static int cpu_clock_event_init(struct perf_event *event)
{
	if (event->attr.type != perf_cpu_clock.type)
		return -ENOENT;

	if (event->attr.config != PERF_COUNT_SW_CPU_CLOCK)
		return -ENOENT;

	/*
	 * no branch sampling for software events
	 */
	if (has_branch_stack(event))
		return -EOPNOTSUPP;

	perf_swevent_init_hrtimer(event);

	return 0;
}

static struct pmu perf_cpu_clock = {
	.task_ctx_nr	= perf_sw_context,

	.capabilities	= PERF_PMU_CAP_NO_NMI,
	.dev		= PMU_NULL_DEV,

	.event_init	= cpu_clock_event_init,
	.add		= cpu_clock_event_add,
	.del		= cpu_clock_event_del,
	.start		= cpu_clock_event_start,
	.stop		= cpu_clock_event_stop,
	.read		= cpu_clock_event_read,
};

/*
 * Software event: task time clock
 */

static void task_clock_event_update(struct perf_event *event, u64 now)
{
	u64 prev;
	s64 delta;

	prev = local64_xchg(&event->hw.prev_count, now);
	delta = now - prev;
	local64_add(delta, &event->count);
}

static void task_clock_event_start(struct perf_event *event, int flags)
{
	local64_set(&event->hw.prev_count, event->ctx->time);
	perf_swevent_start_hrtimer(event);
}

static void task_clock_event_stop(struct perf_event *event, int flags)
{
	perf_swevent_cancel_hrtimer(event);
	task_clock_event_update(event, event->ctx->time);
}

static int task_clock_event_add(struct perf_event *event, int flags)
{
	if (flags & PERF_EF_START)
		task_clock_event_start(event, flags);
	perf_event_update_userpage(event);

	return 0;
}

static void task_clock_event_del(struct perf_event *event, int flags)
{
	task_clock_event_stop(event, PERF_EF_UPDATE);
}

static void task_clock_event_read(struct perf_event *event)
{
	u64 now = perf_clock();
	u64 delta = now - event->ctx->timestamp;
	u64 time = event->ctx->time + delta;

	task_clock_event_update(event, time);
}

static int task_clock_event_init(struct perf_event *event)
{
	if (event->attr.type != perf_task_clock.type)
		return -ENOENT;

	if (event->attr.config != PERF_COUNT_SW_TASK_CLOCK)
		return -ENOENT;

	/*
	 * no branch sampling for software events
	 */
	if (has_branch_stack(event))
		return -EOPNOTSUPP;

	perf_swevent_init_hrtimer(event);

	return 0;
}

static struct pmu perf_task_clock = {
	.task_ctx_nr	= perf_sw_context,

	.capabilities	= PERF_PMU_CAP_NO_NMI,
	.dev		= PMU_NULL_DEV,

	.event_init	= task_clock_event_init,
	.add		= task_clock_event_add,
	.del		= task_clock_event_del,
	.start		= task_clock_event_start,
	.stop		= task_clock_event_stop,
	.read		= task_clock_event_read,
};

static void perf_pmu_nop_void(struct pmu *pmu)
{
}

static void perf_pmu_nop_txn(struct pmu *pmu, unsigned int flags)
{
}

static int perf_pmu_nop_int(struct pmu *pmu)
{
	return 0;
}

static int perf_event_nop_int(struct perf_event *event, u64 value)
{
	return 0;
}

static DEFINE_PER_CPU(unsigned int, nop_txn_flags);

static void perf_pmu_start_txn(struct pmu *pmu, unsigned int flags)
{
	__this_cpu_write(nop_txn_flags, flags);

	if (flags & ~PERF_PMU_TXN_ADD)
		return;

	perf_pmu_disable(pmu);
}

static int perf_pmu_commit_txn(struct pmu *pmu)
{
	unsigned int flags = __this_cpu_read(nop_txn_flags);

	__this_cpu_write(nop_txn_flags, 0);

	if (flags & ~PERF_PMU_TXN_ADD)
		return 0;

	perf_pmu_enable(pmu);
	return 0;
}

static void perf_pmu_cancel_txn(struct pmu *pmu)
{
	unsigned int flags =  __this_cpu_read(nop_txn_flags);

	__this_cpu_write(nop_txn_flags, 0);

	if (flags & ~PERF_PMU_TXN_ADD)
		return;

	perf_pmu_enable(pmu);
}

static int perf_event_idx_default(struct perf_event *event)
{
	return 0;
}

/*
 * Let userspace know that this PMU supports address range filtering:
 */
static ssize_t nr_addr_filters_show(struct device *dev,
				    struct device_attribute *attr,
				    char *page)
{
	struct pmu *pmu = dev_get_drvdata(dev);

	return sysfs_emit(page, "%d\n", pmu->nr_addr_filters);
}
DEVICE_ATTR_RO(nr_addr_filters);

static struct idr pmu_idr;

static ssize_t
type_show(struct device *dev, struct device_attribute *attr, char *page)
{
	struct pmu *pmu = dev_get_drvdata(dev);

	return sysfs_emit(page, "%d\n", pmu->type);
}
static DEVICE_ATTR_RO(type);

static ssize_t
perf_event_mux_interval_ms_show(struct device *dev,
				struct device_attribute *attr,
				char *page)
{
	struct pmu *pmu = dev_get_drvdata(dev);

	return sysfs_emit(page, "%d\n", pmu->hrtimer_interval_ms);
}

static DEFINE_MUTEX(mux_interval_mutex);

static ssize_t
perf_event_mux_interval_ms_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct pmu *pmu = dev_get_drvdata(dev);
	int timer, cpu, ret;

	ret = kstrtoint(buf, 0, &timer);
	if (ret)
		return ret;

	if (timer < 1)
		return -EINVAL;

	/* same value, noting to do */
	if (timer == pmu->hrtimer_interval_ms)
		return count;

	mutex_lock(&mux_interval_mutex);
	pmu->hrtimer_interval_ms = timer;

	/* update all cpuctx for this PMU */
	cpus_read_lock();
	for_each_online_cpu(cpu) {
		struct perf_cpu_pmu_context *cpc;
		cpc = *per_cpu_ptr(pmu->cpu_pmu_context, cpu);
		cpc->hrtimer_interval = ns_to_ktime(NSEC_PER_MSEC * timer);

		cpu_function_call(cpu, perf_mux_hrtimer_restart_ipi, cpc);
	}
	cpus_read_unlock();
	mutex_unlock(&mux_interval_mutex);

	return count;
}
static DEVICE_ATTR_RW(perf_event_mux_interval_ms);

static inline const struct cpumask *perf_scope_cpu_topology_cpumask(unsigned int scope, int cpu)
{
	switch (scope) {
	case PERF_PMU_SCOPE_CORE:
		return topology_sibling_cpumask(cpu);
	case PERF_PMU_SCOPE_DIE:
		return topology_die_cpumask(cpu);
	case PERF_PMU_SCOPE_CLUSTER:
		return topology_cluster_cpumask(cpu);
	case PERF_PMU_SCOPE_PKG:
		return topology_core_cpumask(cpu);
	case PERF_PMU_SCOPE_SYS_WIDE:
		return cpu_online_mask;
	}

	return NULL;
}

static inline struct cpumask *perf_scope_cpumask(unsigned int scope)
{
	switch (scope) {
	case PERF_PMU_SCOPE_CORE:
		return perf_online_core_mask;
	case PERF_PMU_SCOPE_DIE:
		return perf_online_die_mask;
	case PERF_PMU_SCOPE_CLUSTER:
		return perf_online_cluster_mask;
	case PERF_PMU_SCOPE_PKG:
		return perf_online_pkg_mask;
	case PERF_PMU_SCOPE_SYS_WIDE:
		return perf_online_sys_mask;
	}

	return NULL;
}

static ssize_t cpumask_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct pmu *pmu = dev_get_drvdata(dev);
	struct cpumask *mask = perf_scope_cpumask(pmu->scope);

	if (mask)
		return cpumap_print_to_pagebuf(true, buf, mask);
	return 0;
}

static DEVICE_ATTR_RO(cpumask);

static struct attribute *pmu_dev_attrs[] = {
	&dev_attr_type.attr,
	&dev_attr_perf_event_mux_interval_ms.attr,
	&dev_attr_nr_addr_filters.attr,
	&dev_attr_cpumask.attr,
	NULL,
};

static umode_t pmu_dev_is_visible(struct kobject *kobj, struct attribute *a, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct pmu *pmu = dev_get_drvdata(dev);

	if (n == 2 && !pmu->nr_addr_filters)
		return 0;

	/* cpumask */
	if (n == 3 && pmu->scope == PERF_PMU_SCOPE_NONE)
		return 0;

	return a->mode;
}

static struct attribute_group pmu_dev_attr_group = {
	.is_visible = pmu_dev_is_visible,
	.attrs = pmu_dev_attrs,
};

static const struct attribute_group *pmu_dev_groups[] = {
	&pmu_dev_attr_group,
	NULL,
};

static int pmu_bus_running;
static struct bus_type pmu_bus = {
	.name		= "event_source",
	.dev_groups	= pmu_dev_groups,
};

static void pmu_dev_release(struct device *dev)
{
	kfree(dev);
}

static int pmu_dev_alloc(struct pmu *pmu)
{
	int ret = -ENOMEM;

	pmu->dev = kzalloc(sizeof(struct device), GFP_KERNEL);
	if (!pmu->dev)
		goto out;

	pmu->dev->groups = pmu->attr_groups;
	device_initialize(pmu->dev);

	dev_set_drvdata(pmu->dev, pmu);
	pmu->dev->bus = &pmu_bus;
	pmu->dev->parent = pmu->parent;
	pmu->dev->release = pmu_dev_release;

	ret = dev_set_name(pmu->dev, "%s", pmu->name);
	if (ret)
		goto free_dev;

	ret = device_add(pmu->dev);
	if (ret)
		goto free_dev;

	if (pmu->attr_update) {
		ret = sysfs_update_groups(&pmu->dev->kobj, pmu->attr_update);
		if (ret)
			goto del_dev;
	}

out:
	return ret;

del_dev:
	device_del(pmu->dev);

free_dev:
	put_device(pmu->dev);
	pmu->dev = NULL;
	goto out;
}

static struct lock_class_key cpuctx_mutex;
static struct lock_class_key cpuctx_lock;

static bool idr_cmpxchg(struct idr *idr, unsigned long id, void *old, void *new)
{
	void *tmp, *val = idr_find(idr, id);

	if (val != old)
		return false;

	tmp = idr_replace(idr, new, id);
	if (IS_ERR(tmp))
		return false;

	WARN_ON_ONCE(tmp != val);
	return true;
}

static void perf_pmu_free(struct pmu *pmu)
{
	if (pmu_bus_running && pmu->dev && pmu->dev != PMU_NULL_DEV) {
		if (pmu->nr_addr_filters)
			device_remove_file(pmu->dev, &dev_attr_nr_addr_filters);
		device_del(pmu->dev);
		put_device(pmu->dev);
	}

	if (pmu->cpu_pmu_context) {
		int cpu;

		for_each_possible_cpu(cpu) {
			struct perf_cpu_pmu_context *cpc;

			cpc = *per_cpu_ptr(pmu->cpu_pmu_context, cpu);
			if (!cpc)
				continue;
			if (cpc->epc.embedded) {
				/* refcount managed */
				put_pmu_ctx(&cpc->epc);
				continue;
			}
			kfree(cpc);
		}
		free_percpu(pmu->cpu_pmu_context);
	}
}

DEFINE_FREE(pmu_unregister, struct pmu *, if (_T) perf_pmu_free(_T))

int perf_pmu_register(struct pmu *_pmu, const char *name, int type)
{
	int cpu, max = PERF_TYPE_MAX;

	struct pmu *pmu __free(pmu_unregister) = _pmu;
	guard(mutex)(&pmus_lock);

	if (WARN_ONCE(!name, "Can not register anonymous pmu.\n"))
		return -EINVAL;

	if (WARN_ONCE(pmu->scope >= PERF_PMU_MAX_SCOPE,
		      "Can not register a pmu with an invalid scope.\n"))
		return -EINVAL;

	pmu->name = name;

	if (type >= 0)
		max = type;

	CLASS(idr_alloc, pmu_type)(&pmu_idr, NULL, max, 0, GFP_KERNEL);
	if (pmu_type.id < 0)
		return pmu_type.id;

	WARN_ON(type >= 0 && pmu_type.id != type);

	pmu->type = pmu_type.id;
	atomic_set(&pmu->exclusive_cnt, 0);

	if (pmu_bus_running && !pmu->dev) {
		int ret = pmu_dev_alloc(pmu);
		if (ret)
			return ret;
	}

	pmu->cpu_pmu_context = alloc_percpu(struct perf_cpu_pmu_context *);
	if (!pmu->cpu_pmu_context)
		return -ENOMEM;

	for_each_possible_cpu(cpu) {
		struct perf_cpu_pmu_context *cpc =
			kmalloc_node(sizeof(struct perf_cpu_pmu_context),
				     GFP_KERNEL | __GFP_ZERO,
				     cpu_to_node(cpu));

		if (!cpc)
			return -ENOMEM;

		*per_cpu_ptr(pmu->cpu_pmu_context, cpu) = cpc;
		__perf_init_event_pmu_context(&cpc->epc, pmu);
		__perf_mux_hrtimer_init(cpc, cpu);
	}

	if (!pmu->start_txn) {
		if (pmu->pmu_enable) {
			/*
			 * If we have pmu_enable/pmu_disable calls, install
			 * transaction stubs that use that to try and batch
			 * hardware accesses.
			 */
			pmu->start_txn  = perf_pmu_start_txn;
			pmu->commit_txn = perf_pmu_commit_txn;
			pmu->cancel_txn = perf_pmu_cancel_txn;
		} else {
			pmu->start_txn  = perf_pmu_nop_txn;
			pmu->commit_txn = perf_pmu_nop_int;
			pmu->cancel_txn = perf_pmu_nop_void;
		}
	}

	if (!pmu->pmu_enable) {
		pmu->pmu_enable  = perf_pmu_nop_void;
		pmu->pmu_disable = perf_pmu_nop_void;
	}

	if (!pmu->check_period)
		pmu->check_period = perf_event_nop_int;

	if (!pmu->event_idx)
		pmu->event_idx = perf_event_idx_default;

	/*
	 * Now that the PMU is complete, make it visible to perf_try_init_event().
	 */
	if (!idr_cmpxchg(&pmu_idr, pmu->type, NULL, pmu))
		return -EINVAL;
	list_add_rcu(&pmu->entry, &pmus);

	take_idr_id(pmu_type);
	_pmu = no_free_ptr(pmu); // let it rip
	return 0;
}
EXPORT_SYMBOL_GPL(perf_pmu_register);

void perf_pmu_unregister(struct pmu *pmu)
{
	scoped_guard (mutex, &pmus_lock) {
		list_del_rcu(&pmu->entry);
		idr_remove(&pmu_idr, pmu->type);
	}

	/*
	 * We dereference the pmu list under both SRCU and regular RCU, so
	 * synchronize against both of those.
	 */
	synchronize_srcu(&pmus_srcu);
	synchronize_rcu();

	perf_pmu_free(pmu);
}
EXPORT_SYMBOL_GPL(perf_pmu_unregister);

static inline bool has_extended_regs(struct perf_event *event)
{
	return (event->attr.sample_regs_user & PERF_REG_EXTENDED_MASK) ||
	       (event->attr.sample_regs_intr & PERF_REG_EXTENDED_MASK);
}

static int perf_try_init_event(struct pmu *pmu, struct perf_event *event)
{
	struct perf_event_context *ctx = NULL;
	int ret;

	if (!try_module_get(pmu->module))
		return -ENODEV;

	/*
	 * A number of pmu->event_init() methods iterate the sibling_list to,
	 * for example, validate if the group fits on the PMU. Therefore,
	 * if this is a sibling event, acquire the ctx->mutex to protect
	 * the sibling_list.
	 */
	if (event->group_leader != event && pmu->task_ctx_nr != perf_sw_context) {
		/*
		 * This ctx->mutex can nest when we're called through
		 * inheritance. See the perf_event_ctx_lock_nested() comment.
		 */
		ctx = perf_event_ctx_lock_nested(event->group_leader,
						 SINGLE_DEPTH_NESTING);
		BUG_ON(!ctx);
	}

	event->pmu = pmu;
	ret = pmu->event_init(event);

	if (ctx)
		perf_event_ctx_unlock(event->group_leader, ctx);

	if (ret)
		goto err_pmu;

	if (!(pmu->capabilities & PERF_PMU_CAP_EXTENDED_REGS) &&
	    has_extended_regs(event)) {
		ret = -EOPNOTSUPP;
		goto err_destroy;
	}

	if (pmu->capabilities & PERF_PMU_CAP_NO_EXCLUDE &&
	    event_has_any_exclude_flag(event)) {
		ret = -EINVAL;
		goto err_destroy;
	}

	if (pmu->scope != PERF_PMU_SCOPE_NONE && event->cpu >= 0) {
		const struct cpumask *cpumask;
		struct cpumask *pmu_cpumask;
		int cpu;

		cpumask = perf_scope_cpu_topology_cpumask(pmu->scope, event->cpu);
		pmu_cpumask = perf_scope_cpumask(pmu->scope);

		ret = -ENODEV;
		if (!pmu_cpumask || !cpumask)
			goto err_destroy;

		cpu = cpumask_any_and(pmu_cpumask, cpumask);
		if (cpu >= nr_cpu_ids)
			goto err_destroy;

		event->event_caps |= PERF_EV_CAP_READ_SCOPE;
	}

	return 0;

err_destroy:
	if (event->destroy) {
		event->destroy(event);
		event->destroy = NULL;
	}

err_pmu:
	event->pmu = NULL;
	module_put(pmu->module);
	return ret;
}

static struct pmu *perf_init_event(struct perf_event *event)
{
	bool extended_type = false;
	struct pmu *pmu;
	int type, ret;

	guard(srcu)(&pmus_srcu);

	/*
	 * Save original type before calling pmu->event_init() since certain
	 * pmus overwrites event->attr.type to forward event to another pmu.
	 */
	event->orig_type = event->attr.type;

	/* Try parent's PMU first: */
	if (event->parent && event->parent->pmu) {
		pmu = event->parent->pmu;
		ret = perf_try_init_event(pmu, event);
		if (!ret)
			return pmu;
	}

	/*
	 * PERF_TYPE_HARDWARE and PERF_TYPE_HW_CACHE
	 * are often aliases for PERF_TYPE_RAW.
	 */
	type = event->attr.type;
	if (type == PERF_TYPE_HARDWARE || type == PERF_TYPE_HW_CACHE) {
		type = event->attr.config >> PERF_PMU_TYPE_SHIFT;
		if (!type) {
			type = PERF_TYPE_RAW;
		} else {
			extended_type = true;
			event->attr.config &= PERF_HW_EVENT_MASK;
		}
	}

again:
	scoped_guard (rcu)
		pmu = idr_find(&pmu_idr, type);
	if (pmu) {
		if (event->attr.type != type && type != PERF_TYPE_RAW &&
		    !(pmu->capabilities & PERF_PMU_CAP_EXTENDED_HW_TYPE))
			return ERR_PTR(-ENOENT);

		ret = perf_try_init_event(pmu, event);
		if (ret == -ENOENT && event->attr.type != type && !extended_type) {
			type = event->attr.type;
			goto again;
		}

		if (ret)
			return ERR_PTR(ret);

		return pmu;
	}

	list_for_each_entry_rcu(pmu, &pmus, entry, lockdep_is_held(&pmus_srcu)) {
		ret = perf_try_init_event(pmu, event);
		if (!ret)
			return pmu;

		if (ret != -ENOENT)
			return ERR_PTR(ret);
	}

	return ERR_PTR(-ENOENT);
}

static void attach_sb_event(struct perf_event *event)
{
	struct pmu_event_list *pel = per_cpu_ptr(&pmu_sb_events, event->cpu);

	raw_spin_lock(&pel->lock);
	list_add_rcu(&event->sb_list, &pel->list);
	raw_spin_unlock(&pel->lock);
}

/*
 * We keep a list of all !task (and therefore per-cpu) events
 * that need to receive side-band records.
 *
 * This avoids having to scan all the various PMU per-cpu contexts
 * looking for them.
 */
static void account_pmu_sb_event(struct perf_event *event)
{
	if (is_sb_event(event))
		attach_sb_event(event);
}

/* Freq events need the tick to stay alive (see perf_event_task_tick). */
static void account_freq_event_nohz(void)
{
#ifdef CONFIG_NO_HZ_FULL
	/* Lock so we don't race with concurrent unaccount */
	spin_lock(&nr_freq_lock);
	if (atomic_inc_return(&nr_freq_events) == 1)
		tick_nohz_dep_set(TICK_DEP_BIT_PERF_EVENTS);
	spin_unlock(&nr_freq_lock);
#endif
}

static void account_freq_event(void)
{
	if (tick_nohz_full_enabled())
		account_freq_event_nohz();
	else
		atomic_inc(&nr_freq_events);
}


static void account_event(struct perf_event *event)
{
	bool inc = false;

	if (event->parent)
		return;

	if (event->attach_state & (PERF_ATTACH_TASK | PERF_ATTACH_SCHED_CB))
		inc = true;
	if (event->attr.mmap || event->attr.mmap_data)
		atomic_inc(&nr_mmap_events);
	if (event->attr.build_id)
		atomic_inc(&nr_build_id_events);
	if (event->attr.comm)
		atomic_inc(&nr_comm_events);
	if (event->attr.namespaces)
		atomic_inc(&nr_namespaces_events);
	if (event->attr.cgroup)
		atomic_inc(&nr_cgroup_events);
	if (event->attr.task)
		atomic_inc(&nr_task_events);
	if (event->attr.freq)
		account_freq_event();
	if (event->attr.context_switch) {
		atomic_inc(&nr_switch_events);
		inc = true;
	}
	if (has_branch_stack(event))
		inc = true;
	if (is_cgroup_event(event))
		inc = true;
	if (event->attr.ksymbol)
		atomic_inc(&nr_ksymbol_events);
	if (event->attr.bpf_event)
		atomic_inc(&nr_bpf_events);
	if (event->attr.text_poke)
		atomic_inc(&nr_text_poke_events);

	if (inc) {
		/*
		 * We need the mutex here because static_branch_enable()
		 * must complete *before* the perf_sched_count increment
		 * becomes visible.
		 */
		if (atomic_inc_not_zero(&perf_sched_count))
			goto enabled;

		mutex_lock(&perf_sched_mutex);
		if (!atomic_read(&perf_sched_count)) {
			static_branch_enable(&perf_sched_events);
			/*
			 * Guarantee that all CPUs observe they key change and
			 * call the perf scheduling hooks before proceeding to
			 * install events that need them.
			 */
			synchronize_rcu();
		}
		/*
		 * Now that we have waited for the sync_sched(), allow further
		 * increments to by-pass the mutex.
		 */
		atomic_inc(&perf_sched_count);
		mutex_unlock(&perf_sched_mutex);
	}
enabled:

	account_pmu_sb_event(event);
}

/*
 * Allocate and initialize an event structure
 */
static struct perf_event *
perf_event_alloc(struct perf_event_attr *attr, int cpu,
		 struct task_struct *task,
		 struct perf_event *group_leader,
		 struct perf_event *parent_event,
		 perf_overflow_handler_t overflow_handler,
		 void *context, int cgroup_fd)
{
	struct pmu *pmu;
	struct hw_perf_event *hwc;
	long err = -EINVAL;
	int node;

	if ((unsigned)cpu >= nr_cpu_ids) {
		if (!task || cpu != -1)
			return ERR_PTR(-EINVAL);
	}
	if (attr->sigtrap && !task) {
		/* Requires a task: avoid signalling random tasks. */
		return ERR_PTR(-EINVAL);
	}

	node = (cpu >= 0) ? cpu_to_node(cpu) : -1;
	struct perf_event *event __free(__free_event) =
		kmem_cache_alloc_node(perf_event_cache, GFP_KERNEL | __GFP_ZERO, node);
	if (!event)
		return ERR_PTR(-ENOMEM);

	/*
	 * Single events are their own group leaders, with an
	 * empty sibling list:
	 */
	if (!group_leader)
		group_leader = event;

	mutex_init(&event->child_mutex);
	INIT_LIST_HEAD(&event->child_list);

	INIT_LIST_HEAD(&event->event_entry);
	INIT_LIST_HEAD(&event->sibling_list);
	INIT_LIST_HEAD(&event->active_list);
	init_event_group(event);
	INIT_LIST_HEAD(&event->rb_entry);
	INIT_LIST_HEAD(&event->active_entry);
	INIT_LIST_HEAD(&event->addr_filters.list);
	INIT_HLIST_NODE(&event->hlist_entry);


	init_waitqueue_head(&event->waitq);
	init_irq_work(&event->pending_irq, perf_pending_irq);
	event->pending_disable_irq = IRQ_WORK_INIT_HARD(perf_pending_disable);
	init_task_work(&event->pending_task, perf_pending_task);

	mutex_init(&event->mmap_mutex);
	raw_spin_lock_init(&event->addr_filters.lock);

	atomic_long_set(&event->refcount, 1);
	event->cpu		= cpu;
	event->attr		= *attr;
	event->group_leader	= group_leader;
	event->pmu		= NULL;
	event->oncpu		= -1;

	event->parent		= parent_event;

	event->ns		= get_pid_ns(task_active_pid_ns(current));
	event->id		= atomic64_inc_return(&perf_event_id);

	event->state		= PERF_EVENT_STATE_INACTIVE;

	if (parent_event)
		event->event_caps = parent_event->event_caps;

	if (task) {
		event->attach_state = PERF_ATTACH_TASK;
		/*
		 * XXX pmu::event_init needs to know what task to account to
		 * and we cannot use the ctx information because we need the
		 * pmu before we get a ctx.
		 */
		event->hw.target = get_task_struct(task);
	}

	event->clock = &local_clock;
	if (parent_event)
		event->clock = parent_event->clock;

	if (!overflow_handler && parent_event) {
		overflow_handler = parent_event->overflow_handler;
		context = parent_event->overflow_handler_context;
#if defined(CONFIG_BPF_SYSCALL) && defined(CONFIG_EVENT_TRACING)
		if (parent_event->prog) {
			struct bpf_prog *prog = parent_event->prog;

			bpf_prog_inc(prog);
			event->prog = prog;
		}
#endif
	}

	if (overflow_handler) {
		event->overflow_handler	= overflow_handler;
		event->overflow_handler_context = context;
	} else if (is_write_backward(event)){
		event->overflow_handler = perf_event_output_backward;
		event->overflow_handler_context = NULL;
	} else {
		event->overflow_handler = perf_event_output_forward;
		event->overflow_handler_context = NULL;
	}

	perf_event__state_init(event);

	pmu = NULL;

	hwc = &event->hw;
	hwc->sample_period = attr->sample_period;
	if (attr->freq && attr->sample_freq)
		hwc->sample_period = 1;
	hwc->last_period = hwc->sample_period;

	local64_set(&hwc->period_left, hwc->sample_period);

	/*
	 * We do not support PERF_SAMPLE_READ on inherited events unless
	 * PERF_SAMPLE_TID is also selected, which allows inherited events to
	 * collect per-thread samples.
	 * See perf_output_read().
	 */
	if (has_inherit_and_sample_read(attr) && !(attr->sample_type & PERF_SAMPLE_TID))
		return ERR_PTR(-EINVAL);

	if (!has_branch_stack(event))
		event->attr.branch_sample_type = 0;

	pmu = perf_init_event(event);
	if (IS_ERR(pmu))
		return (void*)pmu;

	/*
	 * The PERF_ATTACH_TASK_DATA is set in the event_init()->hw_config().
	 * The attach should be right after the perf_init_event().
	 * Otherwise, the __free_event() would mistakenly detach the non-exist
	 * perf_ctx_data because of the other errors between them.
	 */
	if (event->attach_state & PERF_ATTACH_TASK_DATA) {
		err = attach_perf_ctx_data(event);
		if (err)
			return ERR_PTR(err);
	}

	/*
	 * Disallow uncore-task events. Similarly, disallow uncore-cgroup
	 * events (they don't make sense as the cgroup will be different
	 * on other CPUs in the uncore mask).
	 */
	if (pmu->task_ctx_nr == perf_invalid_context && (task || cgroup_fd != -1))
		return ERR_PTR(-EINVAL);

	if (event->attr.aux_output &&
	    (!(pmu->capabilities & PERF_PMU_CAP_AUX_OUTPUT) ||
	     event->attr.aux_pause || event->attr.aux_resume))
		return ERR_PTR(-EOPNOTSUPP);

	if (event->attr.aux_pause && event->attr.aux_resume)
		return ERR_PTR(-EINVAL);

	if (event->attr.aux_start_paused) {
		if (!(pmu->capabilities & PERF_PMU_CAP_AUX_PAUSE))
			return ERR_PTR(-EOPNOTSUPP);
		event->hw.aux_paused = 1;
	}

	if (cgroup_fd != -1) {
		err = perf_cgroup_connect(cgroup_fd, event, attr, group_leader);
		if (err)
			return ERR_PTR(err);
	}

	err = exclusive_event_init(event);
	if (err)
		return ERR_PTR(err);

	if (has_addr_filter(event)) {
		event->addr_filter_ranges = kcalloc(pmu->nr_addr_filters,
						    sizeof(struct perf_addr_filter_range),
						    GFP_KERNEL);
		if (!event->addr_filter_ranges)
			return ERR_PTR(-ENOMEM);

		/*
		 * Clone the parent's vma offsets: they are valid until exec()
		 * even if the mm is not shared with the parent.
		 */
		if (event->parent) {
			struct perf_addr_filters_head *ifh = perf_event_addr_filters(event);

			raw_spin_lock_irq(&ifh->lock);
			memcpy(event->addr_filter_ranges,
			       event->parent->addr_filter_ranges,
			       pmu->nr_addr_filters * sizeof(struct perf_addr_filter_range));
			raw_spin_unlock_irq(&ifh->lock);
		}

		/* force hw sync on the address filters */
		event->addr_filters_gen = 1;
	}

	if (!event->parent) {
		if (event->attr.sample_type & PERF_SAMPLE_CALLCHAIN) {
			err = get_callchain_buffers(attr->sample_max_stack);
			if (err)
				return ERR_PTR(err);
			event->attach_state |= PERF_ATTACH_CALLCHAIN;
		}
	}

	err = security_perf_event_alloc(event);
	if (err)
		return ERR_PTR(err);

	/* symmetric to unaccount_event() in _free_event() */
	account_event(event);

	return_ptr(event);
}

static int perf_copy_attr(struct perf_event_attr __user *uattr,
			  struct perf_event_attr *attr)
{
	u32 size;
	int ret;

	/* Zero the full structure, so that a short copy will be nice. */
	memset(attr, 0, sizeof(*attr));

	ret = get_user(size, &uattr->size);
	if (ret)
		return ret;

	/* ABI compatibility quirk: */
	if (!size)
		size = PERF_ATTR_SIZE_VER0;
	if (size < PERF_ATTR_SIZE_VER0 || size > PAGE_SIZE)
		goto err_size;

	ret = copy_struct_from_user(attr, sizeof(*attr), uattr, size);
	if (ret) {
		if (ret == -E2BIG)
			goto err_size;
		return ret;
	}

	attr->size = size;

	if (attr->__reserved_1 || attr->__reserved_2 || attr->__reserved_3)
		return -EINVAL;

	if (attr->sample_type & ~(PERF_SAMPLE_MAX-1))
		return -EINVAL;

	if (attr->read_format & ~(PERF_FORMAT_MAX-1))
		return -EINVAL;

	if (attr->sample_type & PERF_SAMPLE_BRANCH_STACK) {
		u64 mask = attr->branch_sample_type;

		/* only using defined bits */
		if (mask & ~(PERF_SAMPLE_BRANCH_MAX-1))
			return -EINVAL;

		/* at least one branch bit must be set */
		if (!(mask & ~PERF_SAMPLE_BRANCH_PLM_ALL))
			return -EINVAL;

		/* propagate priv level, when not set for branch */
		if (!(mask & PERF_SAMPLE_BRANCH_PLM_ALL)) {

			/* exclude_kernel checked on syscall entry */
			if (!attr->exclude_kernel)
				mask |= PERF_SAMPLE_BRANCH_KERNEL;

			if (!attr->exclude_user)
				mask |= PERF_SAMPLE_BRANCH_USER;

			if (!attr->exclude_hv)
				mask |= PERF_SAMPLE_BRANCH_HV;
			/*
			 * adjust user setting (for HW filter setup)
			 */
			attr->branch_sample_type = mask;
		}
		/* privileged levels capture (kernel, hv): check permissions */
		if (mask & PERF_SAMPLE_BRANCH_PERM_PLM) {
			ret = perf_allow_kernel();
			if (ret)
				return ret;
		}
	}

	if (attr->sample_type & PERF_SAMPLE_REGS_USER) {
		ret = perf_reg_validate(attr->sample_regs_user);
		if (ret)
			return ret;
	}

	if (attr->sample_type & PERF_SAMPLE_STACK_USER) {
		if (!arch_perf_have_user_stack_dump())
			return -ENOSYS;

		/*
		 * We have __u32 type for the size, but so far
		 * we can only use __u16 as maximum due to the
		 * __u16 sample size limit.
		 */
		if (attr->sample_stack_user >= USHRT_MAX)
			return -EINVAL;
		else if (!IS_ALIGNED(attr->sample_stack_user, sizeof(u64)))
			return -EINVAL;
	}

	if (!attr->sample_max_stack)
		attr->sample_max_stack = sysctl_perf_event_max_stack;

	if (attr->sample_type & PERF_SAMPLE_REGS_INTR)
		ret = perf_reg_validate(attr->sample_regs_intr);

#ifndef CONFIG_CGROUP_PERF
	if (attr->sample_type & PERF_SAMPLE_CGROUP)
		return -EINVAL;
#endif
	if ((attr->sample_type & PERF_SAMPLE_WEIGHT) &&
	    (attr->sample_type & PERF_SAMPLE_WEIGHT_STRUCT))
		return -EINVAL;

	if (!attr->inherit && attr->inherit_thread)
		return -EINVAL;

	if (attr->remove_on_exec && attr->enable_on_exec)
		return -EINVAL;

	if (attr->sigtrap && !attr->remove_on_exec)
		return -EINVAL;

out:
	return ret;

err_size:
	put_user(sizeof(*attr), &uattr->size);
	ret = -E2BIG;
	goto out;
}

static void mutex_lock_double(struct mutex *a, struct mutex *b)
{
	if (b < a)
		swap(a, b);

	mutex_lock(a);
	mutex_lock_nested(b, SINGLE_DEPTH_NESTING);
}

static int
perf_event_set_output(struct perf_event *event, struct perf_event *output_event)
{
	struct perf_buffer *rb = NULL;
	int ret = -EINVAL;

	if (!output_event) {
		mutex_lock(&event->mmap_mutex);
		goto set;
	}

	/* don't allow circular references */
	if (event == output_event)
		goto out;

	/*
	 * Don't allow cross-cpu buffers
	 */
	if (output_event->cpu != event->cpu)
		goto out;

	/*
	 * If its not a per-cpu rb, it must be the same task.
	 */
	if (output_event->cpu == -1 && output_event->hw.target != event->hw.target)
		goto out;

	/*
	 * Mixing clocks in the same buffer is trouble you don't need.
	 */
	if (output_event->clock != event->clock)
		goto out;

	/*
	 * Either writing ring buffer from beginning or from end.
	 * Mixing is not allowed.
	 */
	if (is_write_backward(output_event) != is_write_backward(event))
		goto out;

	/*
	 * If both events generate aux data, they must be on the same PMU
	 */
	if (has_aux(event) && has_aux(output_event) &&
	    event->pmu != output_event->pmu)
		goto out;

	/*
	 * Hold both mmap_mutex to serialize against perf_mmap_close().  Since
	 * output_event is already on rb->event_list, and the list iteration
	 * restarts after every removal, it is guaranteed this new event is
	 * observed *OR* if output_event is already removed, it's guaranteed we
	 * observe !rb->mmap_count.
	 */
	mutex_lock_double(&event->mmap_mutex, &output_event->mmap_mutex);
set:
	/* Can't redirect output if we've got an active mmap() */
	if (atomic_read(&event->mmap_count))
		goto unlock;

	if (output_event) {
		/* get the rb we want to redirect to */
		rb = ring_buffer_get(output_event);
		if (!rb)
			goto unlock;

		/* did we race against perf_mmap_close() */
		if (!atomic_read(&rb->mmap_count)) {
			ring_buffer_put(rb);
			goto unlock;
		}
	}

	ring_buffer_attach(event, rb);

	ret = 0;
unlock:
	mutex_unlock(&event->mmap_mutex);
	if (output_event)
		mutex_unlock(&output_event->mmap_mutex);

out:
	return ret;
}

static int perf_event_set_clock(struct perf_event *event, clockid_t clk_id)
{
	bool nmi_safe = false;

	switch (clk_id) {
	case CLOCK_MONOTONIC:
		event->clock = &ktime_get_mono_fast_ns;
		nmi_safe = true;
		break;

	case CLOCK_MONOTONIC_RAW:
		event->clock = &ktime_get_raw_fast_ns;
		nmi_safe = true;
		break;

	case CLOCK_REALTIME:
		event->clock = &ktime_get_real_ns;
		break;

	case CLOCK_BOOTTIME:
		event->clock = &ktime_get_boottime_ns;
		break;

	case CLOCK_TAI:
		event->clock = &ktime_get_clocktai_ns;
		break;

	default:
		return -EINVAL;
	}

	if (!nmi_safe && !(event->pmu->capabilities & PERF_PMU_CAP_NO_NMI))
		return -EINVAL;

	return 0;
}

static bool
perf_check_permission(struct perf_event_attr *attr, struct task_struct *task)
{
	unsigned int ptrace_mode = PTRACE_MODE_READ_REALCREDS;
	bool is_capable = perfmon_capable();

	if (attr->sigtrap) {
		/*
		 * perf_event_attr::sigtrap sends signals to the other task.
		 * Require the current task to also have CAP_KILL.
		 */
		rcu_read_lock();
		is_capable &= ns_capable(__task_cred(task)->user_ns, CAP_KILL);
		rcu_read_unlock();

		/*
		 * If the required capabilities aren't available, checks for
		 * ptrace permissions: upgrade to ATTACH, since sending signals
		 * can effectively change the target task.
		 */
		ptrace_mode = PTRACE_MODE_ATTACH_REALCREDS;
	}

	/*
	 * Preserve ptrace permission check for backwards compatibility. The
	 * ptrace check also includes checks that the current task and other
	 * task have matching uids, and is therefore not done here explicitly.
	 */
	return is_capable || ptrace_may_access(task, ptrace_mode);
}

/**
 * sys_perf_event_open - open a performance event, associate it to a task/cpu
 *
 * @attr_uptr:	event_id type attributes for monitoring/sampling
 * @pid:		target pid
 * @cpu:		target cpu
 * @group_fd:		group leader event fd
 * @flags:		perf event open flags
 */
SYSCALL_DEFINE5(perf_event_open,
		struct perf_event_attr __user *, attr_uptr,
		pid_t, pid, int, cpu, int, group_fd, unsigned long, flags)
{
	struct perf_event *group_leader = NULL, *output_event = NULL;
	struct perf_event_pmu_context *pmu_ctx;
	struct perf_event *event, *sibling;
	struct perf_event_attr attr;
	struct perf_event_context *ctx;
	struct file *event_file = NULL;
	struct task_struct *task = NULL;
	struct pmu *pmu;
	int event_fd;
	int move_group = 0;
	int err;
	int f_flags = O_RDWR;
	int cgroup_fd = -1;

	/* for future expandability... */
	if (flags & ~PERF_FLAG_ALL)
		return -EINVAL;

	err = perf_copy_attr(attr_uptr, &attr);
	if (err)
		return err;

	/* Do we allow access to perf_event_open(2) ? */
	err = security_perf_event_open(PERF_SECURITY_OPEN);
	if (err)
		return err;

	if (!attr.exclude_kernel) {
		err = perf_allow_kernel();
		if (err)
			return err;
	}

	if (attr.namespaces) {
		if (!perfmon_capable())
			return -EACCES;
	}

	if (attr.freq) {
		if (attr.sample_freq > sysctl_perf_event_sample_rate)
			return -EINVAL;
	} else {
		if (attr.sample_period & (1ULL << 63))
			return -EINVAL;
	}

	/* Only privileged users can get physical addresses */
	if ((attr.sample_type & PERF_SAMPLE_PHYS_ADDR)) {
		err = perf_allow_kernel();
		if (err)
			return err;
	}

	/* REGS_INTR can leak data, lockdown must prevent this */
	if (attr.sample_type & PERF_SAMPLE_REGS_INTR) {
		err = security_locked_down(LOCKDOWN_PERF);
		if (err)
			return err;
	}

	/*
	 * In cgroup mode, the pid argument is used to pass the fd
	 * opened to the cgroup directory in cgroupfs. The cpu argument
	 * designates the cpu on which to monitor threads from that
	 * cgroup.
	 */
	if ((flags & PERF_FLAG_PID_CGROUP) && (pid == -1 || cpu == -1))
		return -EINVAL;

	if (flags & PERF_FLAG_FD_CLOEXEC)
		f_flags |= O_CLOEXEC;

	event_fd = get_unused_fd_flags(f_flags);
	if (event_fd < 0)
		return event_fd;

	CLASS(fd, group)(group_fd);     // group_fd == -1 => empty
	if (group_fd != -1) {
		if (!is_perf_file(group)) {
			err = -EBADF;
			goto err_fd;
		}
		group_leader = fd_file(group)->private_data;
		if (flags & PERF_FLAG_FD_OUTPUT)
			output_event = group_leader;
		if (flags & PERF_FLAG_FD_NO_GROUP)
			group_leader = NULL;
	}

	if (pid != -1 && !(flags & PERF_FLAG_PID_CGROUP)) {
		task = find_lively_task_by_vpid(pid);
		if (IS_ERR(task)) {
			err = PTR_ERR(task);
			goto err_fd;
		}
	}

	if (task && group_leader &&
	    group_leader->attr.inherit != attr.inherit) {
		err = -EINVAL;
		goto err_task;
	}

	if (flags & PERF_FLAG_PID_CGROUP)
		cgroup_fd = pid;

	event = perf_event_alloc(&attr, cpu, task, group_leader, NULL,
				 NULL, NULL, cgroup_fd);
	if (IS_ERR(event)) {
		err = PTR_ERR(event);
		goto err_task;
	}

	if (is_sampling_event(event)) {
		if (event->pmu->capabilities & PERF_PMU_CAP_NO_INTERRUPT) {
			err = -EOPNOTSUPP;
			goto err_alloc;
		}
	}

	/*
	 * Special case software events and allow them to be part of
	 * any hardware group.
	 */
	pmu = event->pmu;

	if (attr.use_clockid) {
		err = perf_event_set_clock(event, attr.clockid);
		if (err)
			goto err_alloc;
	}

	if (pmu->task_ctx_nr == perf_sw_context)
		event->event_caps |= PERF_EV_CAP_SOFTWARE;

	if (task) {
		err = down_read_interruptible(&task->signal->exec_update_lock);
		if (err)
			goto err_alloc;

		/*
		 * We must hold exec_update_lock across this and any potential
		 * perf_install_in_context() call for this new event to
		 * serialize against exec() altering our credentials (and the
		 * perf_event_exit_task() that could imply).
		 */
		err = -EACCES;
		if (!perf_check_permission(&attr, task))
			goto err_cred;
	}

	/*
	 * Get the target context (task or percpu):
	 */
	ctx = find_get_context(task, event);
	if (IS_ERR(ctx)) {
		err = PTR_ERR(ctx);
		goto err_cred;
	}

	mutex_lock(&ctx->mutex);

	if (ctx->task == TASK_TOMBSTONE) {
		err = -ESRCH;
		goto err_locked;
	}

	if (!task) {
		/*
		 * Check if the @cpu we're creating an event for is online.
		 *
		 * We use the perf_cpu_context::ctx::mutex to serialize against
		 * the hotplug notifiers. See perf_event_{init,exit}_cpu().
		 */
		struct perf_cpu_context *cpuctx = per_cpu_ptr(&perf_cpu_context, event->cpu);

		if (!cpuctx->online) {
			err = -ENODEV;
			goto err_locked;
		}
	}

	if (group_leader) {
		err = -EINVAL;

		/*
		 * Do not allow a recursive hierarchy (this new sibling
		 * becoming part of another group-sibling):
		 */
		if (group_leader->group_leader != group_leader)
			goto err_locked;

		/* All events in a group should have the same clock */
		if (group_leader->clock != event->clock)
			goto err_locked;

		/*
		 * Make sure we're both events for the same CPU;
		 * grouping events for different CPUs is broken; since
		 * you can never concurrently schedule them anyhow.
		 */
		if (group_leader->cpu != event->cpu)
			goto err_locked;

		/*
		 * Make sure we're both on the same context; either task or cpu.
		 */
		if (group_leader->ctx != ctx)
			goto err_locked;

		/*
		 * Only a group leader can be exclusive or pinned
		 */
		if (attr.exclusive || attr.pinned)
			goto err_locked;

		if (is_software_event(event) &&
		    !in_software_context(group_leader)) {
			/*
			 * If the event is a sw event, but the group_leader
			 * is on hw context.
			 *
			 * Allow the addition of software events to hw
			 * groups, this is safe because software events
			 * never fail to schedule.
			 *
			 * Note the comment that goes with struct
			 * perf_event_pmu_context.
			 */
			pmu = group_leader->pmu_ctx->pmu;
		} else if (!is_software_event(event)) {
			if (is_software_event(group_leader) &&
			    (group_leader->group_caps & PERF_EV_CAP_SOFTWARE)) {
				/*
				 * In case the group is a pure software group, and we
				 * try to add a hardware event, move the whole group to
				 * the hardware context.
				 */
				move_group = 1;
			}

			/* Don't allow group of multiple hw events from different pmus */
			if (!in_software_context(group_leader) &&
			    group_leader->pmu_ctx->pmu != pmu)
				goto err_locked;
		}
	}

	/*
	 * Now that we're certain of the pmu; find the pmu_ctx.
	 */
	pmu_ctx = find_get_pmu_context(pmu, ctx, event);
	if (IS_ERR(pmu_ctx)) {
		err = PTR_ERR(pmu_ctx);
		goto err_locked;
	}
	event->pmu_ctx = pmu_ctx;

	if (output_event) {
		err = perf_event_set_output(event, output_event);
		if (err)
			goto err_context;
	}

	if (!perf_event_validate_size(event)) {
		err = -E2BIG;
		goto err_context;
	}

	if (perf_need_aux_event(event) && !perf_get_aux_event(event, group_leader)) {
		err = -EINVAL;
		goto err_context;
	}

	/*
	 * Must be under the same ctx::mutex as perf_install_in_context(),
	 * because we need to serialize with concurrent event creation.
	 */
	if (!exclusive_event_installable(event, ctx)) {
		err = -EBUSY;
		goto err_context;
	}

	WARN_ON_ONCE(ctx->parent_ctx);

	event_file = anon_inode_getfile("[perf_event]", &perf_fops, event, f_flags);
	if (IS_ERR(event_file)) {
		err = PTR_ERR(event_file);
		event_file = NULL;
		goto err_context;
	}

	/*
	 * This is the point on no return; we cannot fail hereafter. This is
	 * where we start modifying current state.
	 */

	if (move_group) {
		perf_remove_from_context(group_leader, 0);
		put_pmu_ctx(group_leader->pmu_ctx);

		for_each_sibling_event(sibling, group_leader) {
			perf_remove_from_context(sibling, 0);
			put_pmu_ctx(sibling->pmu_ctx);
		}

		/*
		 * Install the group siblings before the group leader.
		 *
		 * Because a group leader will try and install the entire group
		 * (through the sibling list, which is still in-tact), we can
		 * end up with siblings installed in the wrong context.
		 *
		 * By installing siblings first we NO-OP because they're not
		 * reachable through the group lists.
		 */
		for_each_sibling_event(sibling, group_leader) {
			sibling->pmu_ctx = pmu_ctx;
			get_pmu_ctx(pmu_ctx);
			perf_event__state_init(sibling);
			perf_install_in_context(ctx, sibling, sibling->cpu);
		}

		/*
		 * Removing from the context ends up with disabled
		 * event. What we want here is event in the initial
		 * startup state, ready to be add into new context.
		 */
		group_leader->pmu_ctx = pmu_ctx;
		get_pmu_ctx(pmu_ctx);
		perf_event__state_init(group_leader);
		perf_install_in_context(ctx, group_leader, group_leader->cpu);
	}

	/*
	 * Precalculate sample_data sizes; do while holding ctx::mutex such
	 * that we're serialized against further additions and before
	 * perf_install_in_context() which is the point the event is active and
	 * can use these values.
	 */
	perf_event__header_size(event);
	perf_event__id_header_size(event);

	event->owner = current;

	perf_install_in_context(ctx, event, event->cpu);
	perf_unpin_context(ctx);

	mutex_unlock(&ctx->mutex);

	if (task) {
		up_read(&task->signal->exec_update_lock);
		put_task_struct(task);
	}

	mutex_lock(&current->perf_event_mutex);
	list_add_tail(&event->owner_entry, &current->perf_event_list);
	mutex_unlock(&current->perf_event_mutex);

	/*
	 * File reference in group guarantees that group_leader has been
	 * kept alive until we place the new event on the sibling_list.
	 * This ensures destruction of the group leader will find
	 * the pointer to itself in perf_group_detach().
	 */
	fd_install(event_fd, event_file);
	return event_fd;

err_context:
	put_pmu_ctx(event->pmu_ctx);
	event->pmu_ctx = NULL; /* _free_event() */
err_locked:
	mutex_unlock(&ctx->mutex);
	perf_unpin_context(ctx);
	put_ctx(ctx);
err_cred:
	if (task)
		up_read(&task->signal->exec_update_lock);
err_alloc:
	free_event(event);
err_task:
	if (task)
		put_task_struct(task);
err_fd:
	put_unused_fd(event_fd);
	return err;
}

/**
 * perf_event_create_kernel_counter
 *
 * @attr: attributes of the counter to create
 * @cpu: cpu in which the counter is bound
 * @task: task to profile (NULL for percpu)
 * @overflow_handler: callback to trigger when we hit the event
 * @context: context data could be used in overflow_handler callback
 */
struct perf_event *
perf_event_create_kernel_counter(struct perf_event_attr *attr, int cpu,
				 struct task_struct *task,
				 perf_overflow_handler_t overflow_handler,
				 void *context)
{
	struct perf_event_pmu_context *pmu_ctx;
	struct perf_event_context *ctx;
	struct perf_event *event;
	struct pmu *pmu;
	int err;

	/*
	 * Grouping is not supported for kernel events, neither is 'AUX',
	 * make sure the caller's intentions are adjusted.
	 */
	if (attr->aux_output || attr->aux_action)
		return ERR_PTR(-EINVAL);

	event = perf_event_alloc(attr, cpu, task, NULL, NULL,
				 overflow_handler, context, -1);
	if (IS_ERR(event)) {
		err = PTR_ERR(event);
		goto err;
	}

	/* Mark owner so we could distinguish it from user events. */
	event->owner = TASK_TOMBSTONE;
	pmu = event->pmu;

	if (pmu->task_ctx_nr == perf_sw_context)
		event->event_caps |= PERF_EV_CAP_SOFTWARE;

	/*
	 * Get the target context (task or percpu):
	 */
	ctx = find_get_context(task, event);
	if (IS_ERR(ctx)) {
		err = PTR_ERR(ctx);
		goto err_alloc;
	}

	WARN_ON_ONCE(ctx->parent_ctx);
	mutex_lock(&ctx->mutex);
	if (ctx->task == TASK_TOMBSTONE) {
		err = -ESRCH;
		goto err_unlock;
	}

	pmu_ctx = find_get_pmu_context(pmu, ctx, event);
	if (IS_ERR(pmu_ctx)) {
		err = PTR_ERR(pmu_ctx);
		goto err_unlock;
	}
	event->pmu_ctx = pmu_ctx;

	if (!task) {
		/*
		 * Check if the @cpu we're creating an event for is online.
		 *
		 * We use the perf_cpu_context::ctx::mutex to serialize against
		 * the hotplug notifiers. See perf_event_{init,exit}_cpu().
		 */
		struct perf_cpu_context *cpuctx =
			container_of(ctx, struct perf_cpu_context, ctx);
		if (!cpuctx->online) {
			err = -ENODEV;
			goto err_pmu_ctx;
		}
	}

	if (!exclusive_event_installable(event, ctx)) {
		err = -EBUSY;
		goto err_pmu_ctx;
	}

	perf_install_in_context(ctx, event, event->cpu);
	perf_unpin_context(ctx);
	mutex_unlock(&ctx->mutex);

	return event;

err_pmu_ctx:
	put_pmu_ctx(pmu_ctx);
	event->pmu_ctx = NULL; /* _free_event() */
err_unlock:
	mutex_unlock(&ctx->mutex);
	perf_unpin_context(ctx);
	put_ctx(ctx);
err_alloc:
	free_event(event);
err:
	return ERR_PTR(err);
}
EXPORT_SYMBOL_GPL(perf_event_create_kernel_counter);

static void __perf_pmu_remove(struct perf_event_context *ctx,
			      int cpu, struct pmu *pmu,
			      struct perf_event_groups *groups,
			      struct list_head *events)
{
	struct perf_event *event, *sibling;

	perf_event_groups_for_cpu_pmu(event, groups, cpu, pmu) {
		perf_remove_from_context(event, 0);
		put_pmu_ctx(event->pmu_ctx);
		list_add(&event->migrate_entry, events);

		for_each_sibling_event(sibling, event) {
			perf_remove_from_context(sibling, 0);
			put_pmu_ctx(sibling->pmu_ctx);
			list_add(&sibling->migrate_entry, events);
		}
	}
}

static void __perf_pmu_install_event(struct pmu *pmu,
				     struct perf_event_context *ctx,
				     int cpu, struct perf_event *event)
{
	struct perf_event_pmu_context *epc;
	struct perf_event_context *old_ctx = event->ctx;

	get_ctx(ctx); /* normally find_get_context() */

	event->cpu = cpu;
	epc = find_get_pmu_context(pmu, ctx, event);
	event->pmu_ctx = epc;

	if (event->state >= PERF_EVENT_STATE_OFF)
		event->state = PERF_EVENT_STATE_INACTIVE;
	perf_install_in_context(ctx, event, cpu);

	/*
	 * Now that event->ctx is updated and visible, put the old ctx.
	 */
	put_ctx(old_ctx);
}

static void __perf_pmu_install(struct perf_event_context *ctx,
			       int cpu, struct pmu *pmu, struct list_head *events)
{
	struct perf_event *event, *tmp;

	/*
	 * Re-instate events in 2 passes.
	 *
	 * Skip over group leaders and only install siblings on this first
	 * pass, siblings will not get enabled without a leader, however a
	 * leader will enable its siblings, even if those are still on the old
	 * context.
	 */
	list_for_each_entry_safe(event, tmp, events, migrate_entry) {
		if (event->group_leader == event)
			continue;

		list_del(&event->migrate_entry);
		__perf_pmu_install_event(pmu, ctx, cpu, event);
	}

	/*
	 * Once all the siblings are setup properly, install the group leaders
	 * to make it go.
	 */
	list_for_each_entry_safe(event, tmp, events, migrate_entry) {
		list_del(&event->migrate_entry);
		__perf_pmu_install_event(pmu, ctx, cpu, event);
	}
}

void perf_pmu_migrate_context(struct pmu *pmu, int src_cpu, int dst_cpu)
{
	struct perf_event_context *src_ctx, *dst_ctx;
	LIST_HEAD(events);

	/*
	 * Since per-cpu context is persistent, no need to grab an extra
	 * reference.
	 */
	src_ctx = &per_cpu_ptr(&perf_cpu_context, src_cpu)->ctx;
	dst_ctx = &per_cpu_ptr(&perf_cpu_context, dst_cpu)->ctx;

	/*
	 * See perf_event_ctx_lock() for comments on the details
	 * of swizzling perf_event::ctx.
	 */
	mutex_lock_double(&src_ctx->mutex, &dst_ctx->mutex);

	__perf_pmu_remove(src_ctx, src_cpu, pmu, &src_ctx->pinned_groups, &events);
	__perf_pmu_remove(src_ctx, src_cpu, pmu, &src_ctx->flexible_groups, &events);

	if (!list_empty(&events)) {
		/*
		 * Wait for the events to quiesce before re-instating them.
		 */
		synchronize_rcu();

		__perf_pmu_install(dst_ctx, dst_cpu, pmu, &events);
	}

	mutex_unlock(&dst_ctx->mutex);
	mutex_unlock(&src_ctx->mutex);
}
EXPORT_SYMBOL_GPL(perf_pmu_migrate_context);

static void sync_child_event(struct perf_event *child_event)
{
	struct perf_event *parent_event = child_event->parent;
	u64 child_val;

	if (child_event->attr.inherit_stat) {
		struct task_struct *task = child_event->ctx->task;

		if (task && task != TASK_TOMBSTONE)
			perf_event_read_event(child_event, task);
	}

	child_val = perf_event_count(child_event, false);

	/*
	 * Add back the child's count to the parent's count:
	 */
	atomic64_add(child_val, &parent_event->child_count);
	atomic64_add(child_event->total_time_enabled,
		     &parent_event->child_total_time_enabled);
	atomic64_add(child_event->total_time_running,
		     &parent_event->child_total_time_running);
}

static void
perf_event_exit_event(struct perf_event *event, struct perf_event_context *ctx)
{
	struct perf_event *parent_event = event->parent;
	unsigned long detach_flags = 0;

	if (parent_event) {
		/*
		 * Do not destroy the 'original' grouping; because of the
		 * context switch optimization the original events could've
		 * ended up in a random child task.
		 *
		 * If we were to destroy the original group, all group related
		 * operations would cease to function properly after this
		 * random child dies.
		 *
		 * Do destroy all inherited groups, we don't care about those
		 * and being thorough is better.
		 */
		detach_flags = DETACH_GROUP | DETACH_CHILD;
		mutex_lock(&parent_event->child_mutex);
	}

	perf_remove_from_context(event, detach_flags | DETACH_EXIT);

	/*
	 * Child events can be freed.
	 */
	if (parent_event) {
		mutex_unlock(&parent_event->child_mutex);
		/*
		 * Kick perf_poll() for is_event_hup();
		 */
		perf_event_wakeup(parent_event);
		put_event(event);
		return;
	}

	/*
	 * Parent events are governed by their filedesc, retain them.
	 */
	perf_event_wakeup(event);
}

static void perf_event_exit_task_context(struct task_struct *child)
{
	struct perf_event_context *child_ctx, *clone_ctx = NULL;
	struct perf_event *child_event, *next;

	WARN_ON_ONCE(child != current);

	child_ctx = perf_pin_task_context(child);
	if (!child_ctx)
		return;

	/*
	 * In order to reduce the amount of tricky in ctx tear-down, we hold
	 * ctx::mutex over the entire thing. This serializes against almost
	 * everything that wants to access the ctx.
	 *
	 * The exception is sys_perf_event_open() /
	 * perf_event_create_kernel_count() which does find_get_context()
	 * without ctx::mutex (it cannot because of the move_group double mutex
	 * lock thing). See the comments in perf_install_in_context().
	 */
	mutex_lock(&child_ctx->mutex);

	/*
	 * In a single ctx::lock section, de-schedule the events and detach the
	 * context from the task such that we cannot ever get it scheduled back
	 * in.
	 */
	raw_spin_lock_irq(&child_ctx->lock);
	task_ctx_sched_out(child_ctx, NULL, EVENT_ALL);

	/*
	 * Now that the context is inactive, destroy the task <-> ctx relation
	 * and mark the context dead.
	 */
	RCU_INIT_POINTER(child->perf_event_ctxp, NULL);
	put_ctx(child_ctx); /* cannot be last */
	WRITE_ONCE(child_ctx->task, TASK_TOMBSTONE);
	put_task_struct(current); /* cannot be last */

	clone_ctx = unclone_ctx(child_ctx);
	raw_spin_unlock_irq(&child_ctx->lock);

	if (clone_ctx)
		put_ctx(clone_ctx);

	/*
	 * Report the task dead after unscheduling the events so that we
	 * won't get any samples after PERF_RECORD_EXIT. We can however still
	 * get a few PERF_RECORD_READ events.
	 */
	perf_event_task(child, child_ctx, 0);

	list_for_each_entry_safe(child_event, next, &child_ctx->event_list, event_entry)
		perf_event_exit_event(child_event, child_ctx);

	mutex_unlock(&child_ctx->mutex);

	put_ctx(child_ctx);
}

/*
 * When a child task exits, feed back event values to parent events.
 *
 * Can be called with exec_update_lock held when called from
 * setup_new_exec().
 */
void perf_event_exit_task(struct task_struct *child)
{
	struct perf_event *event, *tmp;

	mutex_lock(&child->perf_event_mutex);
	list_for_each_entry_safe(event, tmp, &child->perf_event_list,
				 owner_entry) {
		list_del_init(&event->owner_entry);

		/*
		 * Ensure the list deletion is visible before we clear
		 * the owner, closes a race against perf_release() where
		 * we need to serialize on the owner->perf_event_mutex.
		 */
		smp_store_release(&event->owner, NULL);
	}
	mutex_unlock(&child->perf_event_mutex);

	perf_event_exit_task_context(child);

	/*
	 * The perf_event_exit_task_context calls perf_event_task
	 * with child's task_ctx, which generates EXIT events for
	 * child contexts and sets child->perf_event_ctxp[] to NULL.
	 * At this point we need to send EXIT events to cpu contexts.
	 */
	perf_event_task(child, NULL, 0);

	/*
	 * Detach the perf_ctx_data for the system-wide event.
	 */
	guard(percpu_read)(&global_ctx_data_rwsem);
	detach_task_ctx_data(child);
}

static void perf_free_event(struct perf_event *event,
			    struct perf_event_context *ctx)
{
	struct perf_event *parent = event->parent;

	if (WARN_ON_ONCE(!parent))
		return;

	mutex_lock(&parent->child_mutex);
	list_del_init(&event->child_list);
	mutex_unlock(&parent->child_mutex);

	raw_spin_lock_irq(&ctx->lock);
	perf_group_detach(event);
	list_del_event(event, ctx);
	raw_spin_unlock_irq(&ctx->lock);
	put_event(event);
}

/*
 * Free a context as created by inheritance by perf_event_init_task() below,
 * used by fork() in case of fail.
 *
 * Even though the task has never lived, the context and events have been
 * exposed through the child_list, so we must take care tearing it all down.
 */
void perf_event_free_task(struct task_struct *task)
{
	struct perf_event_context *ctx;
	struct perf_event *event, *tmp;

	ctx = rcu_access_pointer(task->perf_event_ctxp);
	if (!ctx)
		return;

	mutex_lock(&ctx->mutex);
	raw_spin_lock_irq(&ctx->lock);
	/*
	 * Destroy the task <-> ctx relation and mark the context dead.
	 *
	 * This is important because even though the task hasn't been
	 * exposed yet the context has been (through child_list).
	 */
	RCU_INIT_POINTER(task->perf_event_ctxp, NULL);
	WRITE_ONCE(ctx->task, TASK_TOMBSTONE);
	put_task_struct(task); /* cannot be last */
	raw_spin_unlock_irq(&ctx->lock);


	list_for_each_entry_safe(event, tmp, &ctx->event_list, event_entry)
		perf_free_event(event, ctx);

	mutex_unlock(&ctx->mutex);

	/*
	 * perf_event_release_kernel() could've stolen some of our
	 * child events and still have them on its free_list. In that
	 * case we must wait for these events to have been freed (in
	 * particular all their references to this task must've been
	 * dropped).
	 *
	 * Without this copy_process() will unconditionally free this
	 * task (irrespective of its reference count) and
	 * _free_event()'s put_task_struct(event->hw.target) will be a
	 * use-after-free.
	 *
	 * Wait for all events to drop their context reference.
	 */
	wait_var_event(&ctx->refcount, refcount_read(&ctx->refcount) == 1);
	put_ctx(ctx); /* must be last */
}

void perf_event_delayed_put(struct task_struct *task)
{
	WARN_ON_ONCE(task->perf_event_ctxp);
}

struct file *perf_event_get(unsigned int fd)
{
	struct file *file = fget(fd);
	if (!file)
		return ERR_PTR(-EBADF);

	if (file->f_op != &perf_fops) {
		fput(file);
		return ERR_PTR(-EBADF);
	}

	return file;
}

const struct perf_event *perf_get_event(struct file *file)
{
	if (file->f_op != &perf_fops)
		return ERR_PTR(-EINVAL);

	return file->private_data;
}

const struct perf_event_attr *perf_event_attrs(struct perf_event *event)
{
	if (!event)
		return ERR_PTR(-EINVAL);

	return &event->attr;
}

int perf_allow_kernel(void)
{
	if (sysctl_perf_event_paranoid > 1 && !perfmon_capable())
		return -EACCES;

	return security_perf_event_open(PERF_SECURITY_KERNEL);
}
EXPORT_SYMBOL_GPL(perf_allow_kernel);

/*
 * Inherit an event from parent task to child task.
 *
 * Returns:
 *  - valid pointer on success
 *  - NULL for orphaned events
 *  - IS_ERR() on error
 */
static struct perf_event *
inherit_event(struct perf_event *parent_event,
	      struct task_struct *parent,
	      struct perf_event_context *parent_ctx,
	      struct task_struct *child,
	      struct perf_event *group_leader,
	      struct perf_event_context *child_ctx)
{
	enum perf_event_state parent_state = parent_event->state;
	struct perf_event_pmu_context *pmu_ctx;
	struct perf_event *child_event;
	unsigned long flags;

	/*
	 * Instead of creating recursive hierarchies of events,
	 * we link inherited events back to the original parent,
	 * which has a filp for sure, which we use as the reference
	 * count:
	 */
	if (parent_event->parent)
		parent_event = parent_event->parent;

	child_event = perf_event_alloc(&parent_event->attr,
					   parent_event->cpu,
					   child,
					   group_leader, parent_event,
					   NULL, NULL, -1);
	if (IS_ERR(child_event))
		return child_event;

	get_ctx(child_ctx);
	child_event->ctx = child_ctx;

	pmu_ctx = find_get_pmu_context(child_event->pmu, child_ctx, child_event);
	if (IS_ERR(pmu_ctx)) {
		free_event(child_event);
		return ERR_CAST(pmu_ctx);
	}
	child_event->pmu_ctx = pmu_ctx;

	/*
	 * is_orphaned_event() and list_add_tail(&parent_event->child_list)
	 * must be under the same lock in order to serialize against
	 * perf_event_release_kernel(), such that either we must observe
	 * is_orphaned_event() or they will observe us on the child_list.
	 */
	mutex_lock(&parent_event->child_mutex);
	if (is_orphaned_event(parent_event) ||
	    !atomic_long_inc_not_zero(&parent_event->refcount)) {
		mutex_unlock(&parent_event->child_mutex);
		free_event(child_event);
		return NULL;
	}

	/*
	 * Make the child state follow the state of the parent event,
	 * not its attr.disabled bit.  We hold the parent's mutex,
	 * so we won't race with perf_event_{en, dis}able_family.
	 */
	if (parent_state >= PERF_EVENT_STATE_INACTIVE)
		child_event->state = PERF_EVENT_STATE_INACTIVE;
	else
		child_event->state = PERF_EVENT_STATE_OFF;

	if (parent_event->attr.freq) {
		u64 sample_period = parent_event->hw.sample_period;
		struct hw_perf_event *hwc = &child_event->hw;

		hwc->sample_period = sample_period;
		hwc->last_period   = sample_period;

		local64_set(&hwc->period_left, sample_period);
	}

	child_event->overflow_handler = parent_event->overflow_handler;
	child_event->overflow_handler_context
		= parent_event->overflow_handler_context;

	/*
	 * Precalculate sample_data sizes
	 */
	perf_event__header_size(child_event);
	perf_event__id_header_size(child_event);

	/*
	 * Link it up in the child's context:
	 */
	raw_spin_lock_irqsave(&child_ctx->lock, flags);
	add_event_to_ctx(child_event, child_ctx);
	child_event->attach_state |= PERF_ATTACH_CHILD;
	raw_spin_unlock_irqrestore(&child_ctx->lock, flags);

	/*
	 * Link this into the parent event's child list
	 */
	list_add_tail(&child_event->child_list, &parent_event->child_list);
	mutex_unlock(&parent_event->child_mutex);

	return child_event;
}

/*
 * Inherits an event group.
 *
 * This will quietly suppress orphaned events; !inherit_event() is not an error.
 * This matches with perf_event_release_kernel() removing all child events.
 *
 * Returns:
 *  - 0 on success
 *  - <0 on error
 */
static int inherit_group(struct perf_event *parent_event,
	      struct task_struct *parent,
	      struct perf_event_context *parent_ctx,
	      struct task_struct *child,
	      struct perf_event_context *child_ctx)
{
	struct perf_event *leader;
	struct perf_event *sub;
	struct perf_event *child_ctr;

	leader = inherit_event(parent_event, parent, parent_ctx,
				 child, NULL, child_ctx);
	if (IS_ERR(leader))
		return PTR_ERR(leader);
	/*
	 * @leader can be NULL here because of is_orphaned_event(). In this
	 * case inherit_event() will create individual events, similar to what
	 * perf_group_detach() would do anyway.
	 */
	for_each_sibling_event(sub, parent_event) {
		child_ctr = inherit_event(sub, parent, parent_ctx,
					    child, leader, child_ctx);
		if (IS_ERR(child_ctr))
			return PTR_ERR(child_ctr);

		if (sub->aux_event == parent_event && child_ctr &&
		    !perf_get_aux_event(child_ctr, leader))
			return -EINVAL;
	}
	if (leader)
		leader->group_generation = parent_event->group_generation;
	return 0;
}

/*
 * Creates the child task context and tries to inherit the event-group.
 *
 * Clears @inherited_all on !attr.inherited or error. Note that we'll leave
 * inherited_all set when we 'fail' to inherit an orphaned event; this is
 * consistent with perf_event_release_kernel() removing all child events.
 *
 * Returns:
 *  - 0 on success
 *  - <0 on error
 */
static int
inherit_task_group(struct perf_event *event, struct task_struct *parent,
		   struct perf_event_context *parent_ctx,
		   struct task_struct *child,
		   u64 clone_flags, int *inherited_all)
{
	struct perf_event_context *child_ctx;
	int ret;

	if (!event->attr.inherit ||
	    (event->attr.inherit_thread && !(clone_flags & CLONE_THREAD)) ||
	    /* Do not inherit if sigtrap and signal handlers were cleared. */
	    (event->attr.sigtrap && (clone_flags & CLONE_CLEAR_SIGHAND))) {
		*inherited_all = 0;
		return 0;
	}

	child_ctx = child->perf_event_ctxp;
	if (!child_ctx) {
		/*
		 * This is executed from the parent task context, so
		 * inherit events that have been marked for cloning.
		 * First allocate and initialize a context for the
		 * child.
		 */
		child_ctx = alloc_perf_context(child);
		if (!child_ctx)
			return -ENOMEM;

		child->perf_event_ctxp = child_ctx;
	}

	ret = inherit_group(event, parent, parent_ctx, child, child_ctx);
	if (ret)
		*inherited_all = 0;

	return ret;
}

/*
 * Initialize the perf_event context in task_struct
 */
static int perf_event_init_context(struct task_struct *child, u64 clone_flags)
{
	struct perf_event_context *child_ctx, *parent_ctx;
	struct perf_event_context *cloned_ctx;
	struct perf_event *event;
	struct task_struct *parent = current;
	int inherited_all = 1;
	unsigned long flags;
	int ret = 0;

	if (likely(!parent->perf_event_ctxp))
		return 0;

	/*
	 * If the parent's context is a clone, pin it so it won't get
	 * swapped under us.
	 */
	parent_ctx = perf_pin_task_context(parent);
	if (!parent_ctx)
		return 0;

	/*
	 * No need to check if parent_ctx != NULL here; since we saw
	 * it non-NULL earlier, the only reason for it to become NULL
	 * is if we exit, and since we're currently in the middle of
	 * a fork we can't be exiting at the same time.
	 */

	/*
	 * Lock the parent list. No need to lock the child - not PID
	 * hashed yet and not running, so nobody can access it.
	 */
	mutex_lock(&parent_ctx->mutex);

	/*
	 * We dont have to disable NMIs - we are only looking at
	 * the list, not manipulating it:
	 */
	perf_event_groups_for_each(event, &parent_ctx->pinned_groups) {
		ret = inherit_task_group(event, parent, parent_ctx,
					 child, clone_flags, &inherited_all);
		if (ret)
			goto out_unlock;
	}

	/*
	 * We can't hold ctx->lock when iterating the ->flexible_group list due
	 * to allocations, but we need to prevent rotation because
	 * rotate_ctx() will change the list from interrupt context.
	 */
	raw_spin_lock_irqsave(&parent_ctx->lock, flags);
	parent_ctx->rotate_disable = 1;
	raw_spin_unlock_irqrestore(&parent_ctx->lock, flags);

	perf_event_groups_for_each(event, &parent_ctx->flexible_groups) {
		ret = inherit_task_group(event, parent, parent_ctx,
					 child, clone_flags, &inherited_all);
		if (ret)
			goto out_unlock;
	}

	raw_spin_lock_irqsave(&parent_ctx->lock, flags);
	parent_ctx->rotate_disable = 0;

	child_ctx = child->perf_event_ctxp;

	if (child_ctx && inherited_all) {
		/*
		 * Mark the child context as a clone of the parent
		 * context, or of whatever the parent is a clone of.
		 *
		 * Note that if the parent is a clone, the holding of
		 * parent_ctx->lock avoids it from being uncloned.
		 */
		cloned_ctx = parent_ctx->parent_ctx;
		if (cloned_ctx) {
			child_ctx->parent_ctx = cloned_ctx;
			child_ctx->parent_gen = parent_ctx->parent_gen;
		} else {
			child_ctx->parent_ctx = parent_ctx;
			child_ctx->parent_gen = parent_ctx->generation;
		}
		get_ctx(child_ctx->parent_ctx);
	}

	raw_spin_unlock_irqrestore(&parent_ctx->lock, flags);
out_unlock:
	mutex_unlock(&parent_ctx->mutex);

	perf_unpin_context(parent_ctx);
	put_ctx(parent_ctx);

	return ret;
}

/*
 * Initialize the perf_event context in task_struct
 */
int perf_event_init_task(struct task_struct *child, u64 clone_flags)
{
	int ret;

	memset(child->perf_recursion, 0, sizeof(child->perf_recursion));
	child->perf_event_ctxp = NULL;
	mutex_init(&child->perf_event_mutex);
	INIT_LIST_HEAD(&child->perf_event_list);
	child->perf_ctx_data = NULL;

	ret = perf_event_init_context(child, clone_flags);
	if (ret) {
		perf_event_free_task(child);
		return ret;
	}

	return 0;
}

static void __init perf_event_init_all_cpus(void)
{
	struct swevent_htable *swhash;
	struct perf_cpu_context *cpuctx;
	int cpu;

	zalloc_cpumask_var(&perf_online_mask, GFP_KERNEL);
	zalloc_cpumask_var(&perf_online_core_mask, GFP_KERNEL);
	zalloc_cpumask_var(&perf_online_die_mask, GFP_KERNEL);
	zalloc_cpumask_var(&perf_online_cluster_mask, GFP_KERNEL);
	zalloc_cpumask_var(&perf_online_pkg_mask, GFP_KERNEL);
	zalloc_cpumask_var(&perf_online_sys_mask, GFP_KERNEL);


	for_each_possible_cpu(cpu) {
		swhash = &per_cpu(swevent_htable, cpu);
		mutex_init(&swhash->hlist_mutex);

		INIT_LIST_HEAD(&per_cpu(pmu_sb_events.list, cpu));
		raw_spin_lock_init(&per_cpu(pmu_sb_events.lock, cpu));

		INIT_LIST_HEAD(&per_cpu(sched_cb_list, cpu));

		cpuctx = per_cpu_ptr(&perf_cpu_context, cpu);
		__perf_event_init_context(&cpuctx->ctx);
		lockdep_set_class(&cpuctx->ctx.mutex, &cpuctx_mutex);
		lockdep_set_class(&cpuctx->ctx.lock, &cpuctx_lock);
		cpuctx->online = cpumask_test_cpu(cpu, perf_online_mask);
		cpuctx->heap_size = ARRAY_SIZE(cpuctx->heap_default);
		cpuctx->heap = cpuctx->heap_default;
	}
}

static void perf_swevent_init_cpu(unsigned int cpu)
{
	struct swevent_htable *swhash = &per_cpu(swevent_htable, cpu);

	mutex_lock(&swhash->hlist_mutex);
	if (swhash->hlist_refcount > 0 && !swevent_hlist_deref(swhash)) {
		struct swevent_hlist *hlist;

		hlist = kzalloc_node(sizeof(*hlist), GFP_KERNEL, cpu_to_node(cpu));
		WARN_ON(!hlist);
		rcu_assign_pointer(swhash->swevent_hlist, hlist);
	}
	mutex_unlock(&swhash->hlist_mutex);
}

#if defined CONFIG_HOTPLUG_CPU || defined CONFIG_KEXEC_CORE
static void __perf_event_exit_context(void *__info)
{
	struct perf_cpu_context *cpuctx = this_cpu_ptr(&perf_cpu_context);
	struct perf_event_context *ctx = __info;
	struct perf_event *event;

	raw_spin_lock(&ctx->lock);
	ctx_sched_out(ctx, NULL, EVENT_TIME);
	list_for_each_entry(event, &ctx->event_list, event_entry)
		__perf_remove_from_context(event, cpuctx, ctx, (void *)DETACH_GROUP);
	raw_spin_unlock(&ctx->lock);
}

static void perf_event_clear_cpumask(unsigned int cpu)
{
	int target[PERF_PMU_MAX_SCOPE];
	unsigned int scope;
	struct pmu *pmu;

	cpumask_clear_cpu(cpu, perf_online_mask);

	for (scope = PERF_PMU_SCOPE_NONE + 1; scope < PERF_PMU_MAX_SCOPE; scope++) {
		const struct cpumask *cpumask = perf_scope_cpu_topology_cpumask(scope, cpu);
		struct cpumask *pmu_cpumask = perf_scope_cpumask(scope);

		target[scope] = -1;
		if (WARN_ON_ONCE(!pmu_cpumask || !cpumask))
			continue;

		if (!cpumask_test_and_clear_cpu(cpu, pmu_cpumask))
			continue;
		target[scope] = cpumask_any_but(cpumask, cpu);
		if (target[scope] < nr_cpu_ids)
			cpumask_set_cpu(target[scope], pmu_cpumask);
	}

	/* migrate */
	list_for_each_entry(pmu, &pmus, entry) {
		if (pmu->scope == PERF_PMU_SCOPE_NONE ||
		    WARN_ON_ONCE(pmu->scope >= PERF_PMU_MAX_SCOPE))
			continue;

		if (target[pmu->scope] >= 0 && target[pmu->scope] < nr_cpu_ids)
			perf_pmu_migrate_context(pmu, cpu, target[pmu->scope]);
	}
}

static void perf_event_exit_cpu_context(int cpu)
{
	struct perf_cpu_context *cpuctx;
	struct perf_event_context *ctx;

	// XXX simplify cpuctx->online
	mutex_lock(&pmus_lock);
	/*
	 * Clear the cpumasks, and migrate to other CPUs if possible.
	 * Must be invoked before the __perf_event_exit_context.
	 */
	perf_event_clear_cpumask(cpu);
	cpuctx = per_cpu_ptr(&perf_cpu_context, cpu);
	ctx = &cpuctx->ctx;

	mutex_lock(&ctx->mutex);
	smp_call_function_single(cpu, __perf_event_exit_context, ctx, 1);
	cpuctx->online = 0;
	mutex_unlock(&ctx->mutex);
	mutex_unlock(&pmus_lock);
}
#else

static void perf_event_exit_cpu_context(int cpu) { }

#endif

static void perf_event_setup_cpumask(unsigned int cpu)
{
	struct cpumask *pmu_cpumask;
	unsigned int scope;

	/*
	 * Early boot stage, the cpumask hasn't been set yet.
	 * The perf_online_<domain>_masks includes the first CPU of each domain.
	 * Always unconditionally set the boot CPU for the perf_online_<domain>_masks.
	 */
	if (cpumask_empty(perf_online_mask)) {
		for (scope = PERF_PMU_SCOPE_NONE + 1; scope < PERF_PMU_MAX_SCOPE; scope++) {
			pmu_cpumask = perf_scope_cpumask(scope);
			if (WARN_ON_ONCE(!pmu_cpumask))
				continue;
			cpumask_set_cpu(cpu, pmu_cpumask);
		}
		goto end;
	}

	for (scope = PERF_PMU_SCOPE_NONE + 1; scope < PERF_PMU_MAX_SCOPE; scope++) {
		const struct cpumask *cpumask = perf_scope_cpu_topology_cpumask(scope, cpu);

		pmu_cpumask = perf_scope_cpumask(scope);

		if (WARN_ON_ONCE(!pmu_cpumask || !cpumask))
			continue;

		if (!cpumask_empty(cpumask) &&
		    cpumask_any_and(pmu_cpumask, cpumask) >= nr_cpu_ids)
			cpumask_set_cpu(cpu, pmu_cpumask);
	}
end:
	cpumask_set_cpu(cpu, perf_online_mask);
}

int perf_event_init_cpu(unsigned int cpu)
{
	struct perf_cpu_context *cpuctx;
	struct perf_event_context *ctx;

	perf_swevent_init_cpu(cpu);

	mutex_lock(&pmus_lock);
	perf_event_setup_cpumask(cpu);
	cpuctx = per_cpu_ptr(&perf_cpu_context, cpu);
	ctx = &cpuctx->ctx;

	mutex_lock(&ctx->mutex);
	cpuctx->online = 1;
	mutex_unlock(&ctx->mutex);
	mutex_unlock(&pmus_lock);

	return 0;
}

int perf_event_exit_cpu(unsigned int cpu)
{
	perf_event_exit_cpu_context(cpu);
	return 0;
}

static int
perf_reboot(struct notifier_block *notifier, unsigned long val, void *v)
{
	int cpu;

	for_each_online_cpu(cpu)
		perf_event_exit_cpu(cpu);

	return NOTIFY_OK;
}

/*
 * Run the perf reboot notifier at the very last possible moment so that
 * the generic watchdog code runs as long as possible.
 */
static struct notifier_block perf_reboot_notifier = {
	.notifier_call = perf_reboot,
	.priority = INT_MIN,
};

void __init perf_event_init(void)
{
	int ret;

	idr_init(&pmu_idr);

	perf_event_init_all_cpus();
	init_srcu_struct(&pmus_srcu);
	perf_pmu_register(&perf_swevent, "software", PERF_TYPE_SOFTWARE);
	perf_pmu_register(&perf_cpu_clock, "cpu_clock", -1);
	perf_pmu_register(&perf_task_clock, "task_clock", -1);
	perf_tp_register();
	perf_event_init_cpu(smp_processor_id());
	register_reboot_notifier(&perf_reboot_notifier);

	ret = init_hw_breakpoint();
	WARN(ret, "hw_breakpoint initialization failed with: %d", ret);

	perf_event_cache = KMEM_CACHE(perf_event, SLAB_PANIC);

	/*
	 * Build time assertion that we keep the data_head at the intended
	 * location.  IOW, validation we got the __reserved[] size right.
	 */
	BUILD_BUG_ON((offsetof(struct perf_event_mmap_page, data_head))
		     != 1024);
}

ssize_t perf_event_sysfs_show(struct device *dev, struct device_attribute *attr,
			      char *page)
{
	struct perf_pmu_events_attr *pmu_attr =
		container_of(attr, struct perf_pmu_events_attr, attr);

	if (pmu_attr->event_str)
		return sprintf(page, "%s\n", pmu_attr->event_str);

	return 0;
}
EXPORT_SYMBOL_GPL(perf_event_sysfs_show);

static int __init perf_event_sysfs_init(void)
{
	struct pmu *pmu;
	int ret;

	mutex_lock(&pmus_lock);

	ret = bus_register(&pmu_bus);
	if (ret)
		goto unlock;

	list_for_each_entry(pmu, &pmus, entry) {
		if (pmu->dev)
			continue;

		ret = pmu_dev_alloc(pmu);
		WARN(ret, "Failed to register pmu: %s, reason %d\n", pmu->name, ret);
	}
	pmu_bus_running = 1;
	ret = 0;

unlock:
	mutex_unlock(&pmus_lock);

	return ret;
}
device_initcall(perf_event_sysfs_init);

#ifdef CONFIG_CGROUP_PERF
static struct cgroup_subsys_state *
perf_cgroup_css_alloc(struct cgroup_subsys_state *parent_css)
{
	struct perf_cgroup *jc;

	jc = kzalloc(sizeof(*jc), GFP_KERNEL);
	if (!jc)
		return ERR_PTR(-ENOMEM);

	jc->info = alloc_percpu(struct perf_cgroup_info);
	if (!jc->info) {
		kfree(jc);
		return ERR_PTR(-ENOMEM);
	}

	return &jc->css;
}

static void perf_cgroup_css_free(struct cgroup_subsys_state *css)
{
	struct perf_cgroup *jc = container_of(css, struct perf_cgroup, css);

	free_percpu(jc->info);
	kfree(jc);
}

static int perf_cgroup_css_online(struct cgroup_subsys_state *css)
{
	perf_event_cgroup(css->cgroup);
	return 0;
}

static int __perf_cgroup_move(void *info)
{
	struct task_struct *task = info;

	preempt_disable();
	perf_cgroup_switch(task);
	preempt_enable();

	return 0;
}

static void perf_cgroup_attach(struct cgroup_taskset *tset)
{
	struct task_struct *task;
	struct cgroup_subsys_state *css;

	cgroup_taskset_for_each(task, css, tset)
		task_function_call(task, __perf_cgroup_move, task);
}

struct cgroup_subsys perf_event_cgrp_subsys = {
	.css_alloc	= perf_cgroup_css_alloc,
	.css_free	= perf_cgroup_css_free,
	.css_online	= perf_cgroup_css_online,
	.attach		= perf_cgroup_attach,
	/*
	 * Implicitly enable on dfl hierarchy so that perf events can
	 * always be filtered by cgroup2 path as long as perf_event
	 * controller is not mounted on a legacy hierarchy.
	 */
	.implicit_on_dfl = true,
	.threaded	= true,
};
#endif /* CONFIG_CGROUP_PERF */

DEFINE_STATIC_CALL_RET0(perf_snapshot_branch_stack, perf_snapshot_branch_stack_t);
