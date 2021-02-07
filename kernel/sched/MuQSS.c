// SPDX-License-Identifier: GPL-2.0
/*
 *  kernel/sched/MuQSS.c, was kernel/sched.c
 *
 *  Kernel scheduler and related syscalls
 *
 *  Copyright (C) 1991-2002  Linus Torvalds
 *
 *  1996-12-23  Modified by Dave Grothe to fix bugs in semaphores and
 *		make semaphores SMP safe
 *  1998-11-19	Implemented schedule_timeout() and related stuff
 *		by Andrea Arcangeli
 *  2002-01-04	New ultra-scalable O(1) scheduler by Ingo Molnar:
 *		hybrid priority-list and round-robin design with
 *		an array-switch method of distributing timeslices
 *		and per-CPU runqueues.  Cleanups and useful suggestions
 *		by Davide Libenzi, preemptible kernel bits by Robert Love.
 *  2003-09-03	Interactivity tuning by Con Kolivas.
 *  2004-04-02	Scheduler domains code by Nick Piggin
 *  2007-04-15  Work begun on replacing all interactivity tuning with a
 *              fair scheduling design by Con Kolivas.
 *  2007-05-05  Load balancing (smp-nice) and other improvements
 *              by Peter Williams
 *  2007-05-06  Interactivity improvements to CFS by Mike Galbraith
 *  2007-07-01  Group scheduling enhancements by Srivatsa Vaddagiri
 *  2007-11-29  RT balancing improvements by Steven Rostedt, Gregory Haskins,
 *              Thomas Gleixner, Mike Kravetz
 *  2009-08-13	Brainfuck deadline scheduling policy by Con Kolivas deletes
 *              a whole lot of those previous things.
 *  2016-10-01  Multiple Queue Skiplist Scheduler scalable evolution of BFS
 * 		scheduler by Con Kolivas.
 *  2019-08-31  LLC bits by Eduards Bezverhijs
 */
#define CREATE_TRACE_POINTS
#include <trace/events/sched.h>
#undef CREATE_TRACE_POINTS

#include <linux/sched/isolation.h>
#include <linux/sched/loadavg.h>

#include <linux/binfmts.h>
#include <linux/blkdev.h>
#include <linux/compat.h>
#include <linux/context_tracking.h>
#include <linux/cpuset.h>
#include <linux/delayacct.h>
#include <linux/init_task.h>
#include <linux/kcov.h>
#include <linux/kprobes.h>
#include <linux/mmu_context.h>
#include <linux/module.h>
#include <linux/nmi.h>
#include <linux/prefetch.h>
#include <linux/profile.h>
#include <linux/rcupdate_wait.h>
#include <linux/sched.h>
#include <linux/scs.h>
#include <linux/security.h>
#include <linux/skip_list.h>
#include <linux/syscalls.h>
#include <linux/tick.h>
#include <linux/wait_bit.h>

#include <asm/irq_regs.h>
#include <asm/switch_to.h>
#include <asm/tlb.h>

#include "../workqueue_internal.h"
#include "../../fs/io-wq.h"
#include "../smpboot.h"

#include "MuQSS.h"
#include "smp.h"

#define rt_prio(prio)		unlikely((prio) < MAX_RT_PRIO)
#define rt_task(p)		rt_prio((p)->prio)
#define batch_task(p)		(unlikely((p)->policy == SCHED_BATCH))
#define is_rt_policy(policy)	((policy) == SCHED_FIFO || \
					(policy) == SCHED_RR)
#define has_rt_policy(p)	unlikely(is_rt_policy((p)->policy))

#define is_idle_policy(policy)	((policy) == SCHED_IDLEPRIO)
#define idleprio_task(p)	unlikely(is_idle_policy((p)->policy))
#define task_running_idle(p)	unlikely((p)->prio == IDLE_PRIO)

#define is_iso_policy(policy)	((policy) == SCHED_ISO)
#define iso_task(p)		unlikely(is_iso_policy((p)->policy))
#define task_running_iso(p)	unlikely((p)->prio == ISO_PRIO)

#define rq_idle(rq)		((rq)->rq_prio == PRIO_LIMIT)

#define ISO_PERIOD		(5 * HZ)

#define STOP_PRIO		(MAX_RT_PRIO - 1)

/*
 * Some helpers for converting to/from various scales. Use shifts to get
 * approximate multiples of ten for less overhead.
 */
#define APPROX_NS_PS		(1073741824) /* Approximate ns per second */
#define JIFFIES_TO_NS(TIME)	((TIME) * (APPROX_NS_PS / HZ))
#define JIFFY_NS		(APPROX_NS_PS / HZ)
#define JIFFY_US		(1048576 / HZ)
#define NS_TO_JIFFIES(TIME)	((TIME) / JIFFY_NS)
#define HALF_JIFFY_NS		(APPROX_NS_PS / HZ / 2)
#define HALF_JIFFY_US		(1048576 / HZ / 2)
#define MS_TO_NS(TIME)		((TIME) << 20)
#define MS_TO_US(TIME)		((TIME) << 10)
#define NS_TO_MS(TIME)		((TIME) >> 20)
#define NS_TO_US(TIME)		((TIME) >> 10)
#define US_TO_NS(TIME)		((TIME) << 10)
#define TICK_APPROX_NS		((APPROX_NS_PS+HZ/2)/HZ)

#define RESCHED_US	(100) /* Reschedule if less than this many μs left */

void print_scheduler_version(void)
{
	printk(KERN_INFO "MuQSS CPU scheduler v0.205 by Con Kolivas.\n");
}

/* Define RQ share levels */
#define RQSHARE_NONE 0
#define RQSHARE_SMT 1
#define RQSHARE_MC 2
#define RQSHARE_MC_LLC 3
#define RQSHARE_SMP 4
#define RQSHARE_ALL 5

/* Define locality levels */
#define LOCALITY_SAME 0
#define LOCALITY_SMT 1
#define LOCALITY_MC_LLC 2
#define LOCALITY_MC 3
#define LOCALITY_SMP 4
#define LOCALITY_DISTANT 5

/*
 * This determines what level of runqueue sharing will be done and is
 * configurable at boot time with the bootparam rqshare =
 */
static int rqshare __read_mostly = CONFIG_SHARERQ; /* Default RQSHARE_MC */

static int __init set_rqshare(char *str)
{
	if (!strncmp(str, "none", 4)) {
		rqshare = RQSHARE_NONE;
		return 1;
	}
	if (!strncmp(str, "smt", 3)) {
		rqshare = RQSHARE_SMT;
		return 1;
	}
	if (!strncmp(str, "mc", 2)) {
		rqshare = RQSHARE_MC;
		return 1;
	}
	if (!strncmp(str, "llc", 3)) {
		rqshare = RQSHARE_MC_LLC;
		return 1;
	}
	if (!strncmp(str, "smp", 3)) {
		rqshare = RQSHARE_SMP;
		return 1;
	}
	if (!strncmp(str, "all", 3)) {
		rqshare = RQSHARE_ALL;
		return 1;
	}
	return 0;
}
__setup("rqshare=", set_rqshare);

/*
 * This is the time all tasks within the same priority round robin.
 * Value is in ms and set to a minimum of 6ms.
 * Tunable via /proc interface.
 */
#ifdef CONFIG_ZEN_INTERACTIVE
int rr_interval __read_mostly = 4;
#else
int rr_interval __read_mostly = 6;
#endif

/*
 * Tunable to choose whether to prioritise latency or throughput, simple
 * binary yes or no
 */
int sched_interactive __read_mostly = 1;

/*
 * sched_iso_cpu - sysctl which determines the cpu percentage SCHED_ISO tasks
 * are allowed to run five seconds as real time tasks. This is the total over
 * all online cpus.
 */
#ifdef CONFIG_ZEN_INTERACTIVE
int sched_iso_cpu __read_mostly = 25;
#else
int sched_iso_cpu __read_mostly = 70;
#endif

/*
 * sched_yield_type - Choose what sort of yield sched_yield will perform.
 * 0: No yield.
 * 1: Yield only to better priority/deadline tasks. (default)
 * 2: Expire timeslice and recalculate deadline.
 */

#ifdef CONFIG_ZEN_INTERACTIVE
int sched_yield_type __read_mostly = 0;
#else
int sched_yield_type __read_mostly = 1;
#endif

/*
 * The relative length of deadline for each priority(nice) level.
 */
static int prio_ratios[NICE_WIDTH] __read_mostly;


/*
 * The quota handed out to tasks of all priority levels when refilling their
 * time_slice.
 */
static inline int timeslice(void)
{
	return MS_TO_US(rr_interval);
}

DEFINE_PER_CPU_SHARED_ALIGNED(struct rq, runqueues);

#ifdef CONFIG_SMP
/*
 * Total number of runqueues. Equals number of CPUs when there is no runqueue
 * sharing but is usually less with SMT/MC sharing of runqueues.
 */
static int total_runqueues __read_mostly = 1;

static cpumask_t cpu_idle_map ____cacheline_aligned_in_smp;

struct rq *cpu_rq(int cpu)
{
	return &per_cpu(runqueues, (cpu));
}
#define cpu_curr(cpu)		(cpu_rq(cpu)->curr)

/*
 * For asym packing, by default the lower numbered cpu has higher priority.
 */
int __weak arch_asym_cpu_priority(int cpu)
{
	return -cpu;
}

int __weak arch_sd_sibling_asym_packing(void)
{
       return 0*SD_ASYM_PACKING;
}

#ifdef CONFIG_SCHED_SMT
DEFINE_STATIC_KEY_FALSE(sched_smt_present);
EXPORT_SYMBOL_GPL(sched_smt_present);
#endif

#else
struct rq *uprq;
#endif /* CONFIG_SMP */

#include "stats.h"

/*
 * All common locking functions performed on rq->lock. rq->clock is local to
 * the CPU accessing it so it can be modified just with interrupts disabled
 * when we're not updating niffies.
 * Looking up task_rq must be done under rq->lock to be safe.
 */

/*
 * RQ-clock updating methods:
 */

#ifdef HAVE_SCHED_AVG_IRQ
static void update_irq_load_avg(struct rq *rq, long delta);
#else
static inline void update_irq_load_avg(struct rq *rq, long delta) {}
#endif

static void update_rq_clock_task(struct rq *rq, s64 delta)
{
/*
 * In theory, the compile should just see 0 here, and optimize out the call
 * to sched_rt_avg_update. But I don't trust it...
 */
	s64 __maybe_unused steal = 0, irq_delta = 0;
#ifdef CONFIG_IRQ_TIME_ACCOUNTING
	irq_delta = irq_time_read(cpu_of(rq)) - rq->prev_irq_time;

	/*
	 * Since irq_time is only updated on {soft,}irq_exit, we might run into
	 * this case when a previous update_rq_clock() happened inside a
	 * {soft,}irq region.
	 *
	 * When this happens, we stop ->clock_task and only update the
	 * prev_irq_time stamp to account for the part that fit, so that a next
	 * update will consume the rest. This ensures ->clock_task is
	 * monotonic.
	 *
	 * It does however cause some slight miss-attribution of {soft,}irq
	 * time, a more accurate solution would be to update the irq_time using
	 * the current rq->clock timestamp, except that would require using
	 * atomic ops.
	 */
	if (irq_delta > delta)
		irq_delta = delta;

	rq->prev_irq_time += irq_delta;
	delta -= irq_delta;
#endif
#ifdef CONFIG_PARAVIRT_TIME_ACCOUNTING
	if (static_key_false((&paravirt_steal_rq_enabled))) {
		steal = paravirt_steal_clock(cpu_of(rq));
		steal -= rq->prev_steal_time_rq;

		if (unlikely(steal > delta))
			steal = delta;

		rq->prev_steal_time_rq += steal;
		delta -= steal;
	}
#endif
	rq->clock_task += delta;

#ifdef CONFIG_HAVE_SCHED_AVG_IRQ
	if (irq_delta + steal)
		update_irq_load_avg(rq, irq_delta + steal);
#endif
}

static inline void update_rq_clock(struct rq *rq)
{
	s64 delta = sched_clock_cpu(cpu_of(rq)) - rq->clock;

	if (unlikely(delta < 0))
		return;
	rq->clock += delta;
	update_rq_clock_task(rq, delta);
}

/*
 * Niffies are a globally increasing nanosecond counter. They're only used by
 * update_load_avg and time_slice_expired, however deadlines are based on them
 * across CPUs. Update them whenever we will call one of those functions, and
 * synchronise them across CPUs whenever we hold both runqueue locks.
 */
static inline void update_clocks(struct rq *rq)
{
	s64 ndiff, minndiff;
	long jdiff;

	update_rq_clock(rq);
	ndiff = rq->clock - rq->old_clock;
	rq->old_clock = rq->clock;
	jdiff = jiffies - rq->last_jiffy;

	/* Subtract any niffies added by balancing with other rqs */
	ndiff -= rq->niffies - rq->last_niffy;
	minndiff = JIFFIES_TO_NS(jdiff) - rq->niffies + rq->last_jiffy_niffies;
	if (minndiff < 0)
		minndiff = 0;
	ndiff = max(ndiff, minndiff);
	rq->niffies += ndiff;
	rq->last_niffy = rq->niffies;
	if (jdiff) {
		rq->last_jiffy += jdiff;
		rq->last_jiffy_niffies = rq->niffies;
	}
}

/*
 * Any time we have two runqueues locked we use that as an opportunity to
 * synchronise niffies to the highest value as idle ticks may have artificially
 * kept niffies low on one CPU and the truth can only be later.
 */
static inline void synchronise_niffies(struct rq *rq1, struct rq *rq2)
{
	if (rq1->niffies > rq2->niffies)
		rq2->niffies = rq1->niffies;
	else
		rq1->niffies = rq2->niffies;
}

/*
 * double_rq_lock - safely lock two runqueues
 *
 * Note this does not disable interrupts like task_rq_lock,
 * you need to do so manually before calling.
 */

/* For when we know rq1 != rq2 */
static inline void __double_rq_lock(struct rq *rq1, struct rq *rq2)
	__acquires(rq1->lock)
	__acquires(rq2->lock)
{
	if (rq1 < rq2) {
		raw_spin_lock(rq1->lock);
		raw_spin_lock_nested(rq2->lock, SINGLE_DEPTH_NESTING);
	} else {
		raw_spin_lock(rq2->lock);
		raw_spin_lock_nested(rq1->lock, SINGLE_DEPTH_NESTING);
	}
}

static inline void double_rq_lock(struct rq *rq1, struct rq *rq2)
	__acquires(rq1->lock)
	__acquires(rq2->lock)
{
	BUG_ON(!irqs_disabled());
	if (rq1->lock == rq2->lock) {
		raw_spin_lock(rq1->lock);
		__acquire(rq2->lock);	/* Fake it out ;) */
	} else
		__double_rq_lock(rq1, rq2);
	synchronise_niffies(rq1, rq2);
}

/*
 * double_rq_unlock - safely unlock two runqueues
 *
 * Note this does not restore interrupts like task_rq_unlock,
 * you need to do so manually after calling.
 */
static inline void double_rq_unlock(struct rq *rq1, struct rq *rq2)
	__releases(rq1->lock)
	__releases(rq2->lock)
{
	raw_spin_unlock(rq1->lock);
	if (rq1->lock != rq2->lock)
		raw_spin_unlock(rq2->lock);
	else
		__release(rq2->lock);
}

static inline void lock_all_rqs(void)
{
	int cpu;

	preempt_disable();
	for_each_possible_cpu(cpu) {
		struct rq *rq = cpu_rq(cpu);

		do_raw_spin_lock(rq->lock);
	}
}

static inline void unlock_all_rqs(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct rq *rq = cpu_rq(cpu);

		do_raw_spin_unlock(rq->lock);
	}
	preempt_enable();
}

/* Specially nest trylock an rq */
static inline bool trylock_rq(struct rq *this_rq, struct rq *rq)
{
	if (unlikely(!do_raw_spin_trylock(rq->lock)))
		return false;
	spin_acquire(&rq->lock->dep_map, SINGLE_DEPTH_NESTING, 1, _RET_IP_);
	synchronise_niffies(this_rq, rq);
	return true;
}

/* Unlock a specially nested trylocked rq */
static inline void unlock_rq(struct rq *rq)
{
	spin_release(&rq->lock->dep_map, _RET_IP_);
	do_raw_spin_unlock(rq->lock);
}

/*
 * cmpxchg based fetch_or, macro so it works for different integer types
 */
#define fetch_or(ptr, mask)						\
	({								\
		typeof(ptr) _ptr = (ptr);				\
		typeof(mask) _mask = (mask);				\
		typeof(*_ptr) _old, _val = *_ptr;			\
									\
		for (;;) {						\
			_old = cmpxchg(_ptr, _val, _val | _mask);	\
			if (_old == _val)				\
				break;					\
			_val = _old;					\
		}							\
	_old;								\
})

#if defined(CONFIG_SMP) && defined(TIF_POLLING_NRFLAG)
/*
 * Atomically set TIF_NEED_RESCHED and test for TIF_POLLING_NRFLAG,
 * this avoids any races wrt polling state changes and thereby avoids
 * spurious IPIs.
 */
static bool set_nr_and_not_polling(struct task_struct *p)
{
	struct thread_info *ti = task_thread_info(p);
	return !(fetch_or(&ti->flags, _TIF_NEED_RESCHED) & _TIF_POLLING_NRFLAG);
}

/*
 * Atomically set TIF_NEED_RESCHED if TIF_POLLING_NRFLAG is set.
 *
 * If this returns true, then the idle task promises to call
 * sched_ttwu_pending() and reschedule soon.
 */
static bool set_nr_if_polling(struct task_struct *p)
{
	struct thread_info *ti = task_thread_info(p);
	typeof(ti->flags) old, val = READ_ONCE(ti->flags);

	for (;;) {
		if (!(val & _TIF_POLLING_NRFLAG))
			return false;
		if (val & _TIF_NEED_RESCHED)
			return true;
		old = cmpxchg(&ti->flags, val, val | _TIF_NEED_RESCHED);
		if (old == val)
			break;
		val = old;
	}
	return true;
}

#else
static bool set_nr_and_not_polling(struct task_struct *p)
{
	set_tsk_need_resched(p);
	return true;
}

#ifdef CONFIG_SMP
static bool set_nr_if_polling(struct task_struct *p)
{
	return false;
}
#endif
#endif

static bool __wake_q_add(struct wake_q_head *head, struct task_struct *task)
{
	struct wake_q_node *node = &task->wake_q;

	/*
	 * Atomically grab the task, if ->wake_q is !nil already it means
	 * its already queued (either by us or someone else) and will get the
	 * wakeup due to that.
	 *
	 * In order to ensure that a pending wakeup will observe our pending
	 * state, even in the failed case, an explicit smp_mb() must be used.
	 */
	smp_mb__before_atomic();
	if (unlikely(cmpxchg_relaxed(&node->next, NULL, WAKE_Q_TAIL)))
		return false;

	/*
	 * The head is context local, there can be no concurrency.
	 */
	*head->lastp = node;
	head->lastp = &node->next;
	return true;
}

/**
 * wake_q_add() - queue a wakeup for 'later' waking.
 * @head: the wake_q_head to add @task to
 * @task: the task to queue for 'later' wakeup
 *
 * Queue a task for later wakeup, most likely by the wake_up_q() call in the
 * same context, _HOWEVER_ this is not guaranteed, the wakeup can come
 * instantly.
 *
 * This function must be used as-if it were wake_up_process(); IOW the task
 * must be ready to be woken at this location.
 */
void wake_q_add(struct wake_q_head *head, struct task_struct *task)
{
	if (__wake_q_add(head, task))
		get_task_struct(task);
}

/**
 * wake_q_add_safe() - safely queue a wakeup for 'later' waking.
 * @head: the wake_q_head to add @task to
 * @task: the task to queue for 'later' wakeup
 *
 * Queue a task for later wakeup, most likely by the wake_up_q() call in the
 * same context, _HOWEVER_ this is not guaranteed, the wakeup can come
 * instantly.
 *
 * This function must be used as-if it were wake_up_process(); IOW the task
 * must be ready to be woken at this location.
 *
 * This function is essentially a task-safe equivalent to wake_q_add(). Callers
 * that already hold reference to @task can call the 'safe' version and trust
 * wake_q to do the right thing depending whether or not the @task is already
 * queued for wakeup.
 */
void wake_q_add_safe(struct wake_q_head *head, struct task_struct *task)
{
	if (!__wake_q_add(head, task))
		put_task_struct(task);
}

void wake_up_q(struct wake_q_head *head)
{
	struct wake_q_node *node = head->first;

	while (node != WAKE_Q_TAIL) {
		struct task_struct *task;

		task = container_of(node, struct task_struct, wake_q);
		BUG_ON(!task);
		/* Task can safely be re-inserted now */
		node = node->next;
		task->wake_q.next = NULL;

		/*
		 * wake_up_process() executes a full barrier, which pairs with
		 * the queueing in wake_q_add() so as not to miss wakeups.
		 */
		wake_up_process(task);
		put_task_struct(task);
	}
}

static inline void smp_sched_reschedule(int cpu)
{
	if (likely(cpu_online(cpu)))
		smp_send_reschedule(cpu);
}

/*
 * resched_task - mark a task 'to be rescheduled now'.
 *
 * On UP this means the setting of the need_resched flag, on SMP it
 * might also involve a cross-CPU call to trigger the scheduler on
 * the target CPU.
 */
void resched_task(struct task_struct *p)
{
	int cpu;
#ifdef CONFIG_LOCKDEP
	/* Kernel threads call this when creating workqueues while still
	 * inactive from __kthread_bind_mask, holding only the pi_lock */
	if (!(p->flags & PF_KTHREAD)) {
		struct rq *rq = task_rq(p);

		lockdep_assert_held(rq->lock);
	}
#endif
	if (test_tsk_need_resched(p))
		return;

	cpu = task_cpu(p);
	if (cpu == smp_processor_id()) {
		set_tsk_need_resched(p);
		set_preempt_need_resched();
		return;
	}

	if (set_nr_and_not_polling(p))
		smp_sched_reschedule(cpu);
	else
		trace_sched_wake_idle_without_ipi(cpu);
}

/*
 * A task that is not running or queued will not have a node set.
 * A task that is queued but not running will have a node set.
 * A task that is currently running will have ->on_cpu set but no node set.
 */
static inline bool task_queued(struct task_struct *p)
{
	return !skiplist_node_empty(&p->node);
}

static void enqueue_task(struct rq *rq, struct task_struct *p, int flags);
static inline void resched_if_idle(struct rq *rq);

static inline bool deadline_before(u64 deadline, u64 time)
{
	return (deadline < time);
}

/*
 * Deadline is "now" in niffies + (offset by priority). Setting the deadline
 * is the key to everything. It distributes cpu fairly amongst tasks of the
 * same nice value, it proportions cpu according to nice level, it means the
 * task that last woke up the longest ago has the earliest deadline, thus
 * ensuring that interactive tasks get low latency on wake up. The CPU
 * proportion works out to the square of the virtual deadline difference, so
 * this equation will give nice 19 3% CPU compared to nice 0.
 */
static inline u64 prio_deadline_diff(int user_prio)
{
	return (prio_ratios[user_prio] * rr_interval * (MS_TO_NS(1) / 128));
}

static inline u64 task_deadline_diff(struct task_struct *p)
{
	return prio_deadline_diff(TASK_USER_PRIO(p));
}

static inline u64 static_deadline_diff(int static_prio)
{
	return prio_deadline_diff(USER_PRIO(static_prio));
}

static inline int longest_deadline_diff(void)
{
	return prio_deadline_diff(39);
}

static inline int ms_longest_deadline_diff(void)
{
	return NS_TO_MS(longest_deadline_diff());
}

static inline bool rq_local(struct rq *rq);

#ifndef SCHED_CAPACITY_SCALE
#define SCHED_CAPACITY_SCALE 1024
#endif

static inline int rq_load(struct rq *rq)
{
	return rq->nr_running;
}

/*
 * Update the load average for feeding into cpu frequency governors. Use a
 * rough estimate of a rolling average with ~ time constant of 32ms.
 * 80/128 ~ 0.63. * 80 / 32768 / 128 == * 5 / 262144
 * Make sure a call to update_clocks has been made before calling this to get
 * an updated rq->niffies.
 */
static void update_load_avg(struct rq *rq, unsigned int flags)
{
	long us_interval, load;

	us_interval = NS_TO_US(rq->niffies - rq->load_update);
	if (unlikely(us_interval <= 0))
		return;

	load = rq->load_avg - (rq->load_avg * us_interval * 5 / 262144);
	if (unlikely(load < 0))
		load = 0;
	load += rq_load(rq) * SCHED_CAPACITY_SCALE * us_interval * 5 / 262144;
	rq->load_avg = load;

	rq->load_update = rq->niffies;
	update_irq_load_avg(rq, 0);
	if (likely(rq_local(rq)))
		cpufreq_trigger(rq, flags);
}

#ifdef HAVE_SCHED_AVG_IRQ
/*
 * IRQ variant of update_load_avg below. delta is actually time in nanoseconds
 * here so we scale curload to how long it's been since the last update.
 */
static void update_irq_load_avg(struct rq *rq, long delta)
{
	long us_interval, load;

	us_interval = NS_TO_US(rq->niffies - rq->irq_load_update);
	if (unlikely(us_interval <= 0))
		return;

	load = rq->irq_load_avg - (rq->irq_load_avg * us_interval * 5 / 262144);
	if (unlikely(load < 0))
		load = 0;
	load += NS_TO_US(delta) * SCHED_CAPACITY_SCALE * 5 / 262144;
	rq->irq_load_avg = load;

	rq->irq_load_update = rq->niffies;
}
#endif

/*
 * Removing from the runqueue. Enter with rq locked. Deleting a task
 * from the skip list is done via the stored node reference in the task struct
 * and does not require a full look up. Thus it occurs in O(k) time where k
 * is the "level" of the list the task was stored at - usually < 4, max 8.
 */
static void dequeue_task(struct rq *rq, struct task_struct *p, int flags)
{
	skiplist_delete(rq->sl, &p->node);
	rq->best_key = rq->node->next[0]->key;
	update_clocks(rq);

	if (!(flags & DEQUEUE_SAVE)) {
		sched_info_dequeued(rq, p);
		psi_dequeue(p, flags & DEQUEUE_SLEEP);
	}
	rq->nr_running--;
	if (rt_task(p))
		rq->rt_nr_running--;
	update_load_avg(rq, flags);
}

#ifdef CONFIG_PREEMPT_RCU
static bool rcu_read_critical(struct task_struct *p)
{
	return p->rcu_read_unlock_special.b.blocked;
}
#else /* CONFIG_PREEMPT_RCU */
#define rcu_read_critical(p) (false)
#endif /* CONFIG_PREEMPT_RCU */

/*
 * To determine if it's safe for a task of SCHED_IDLEPRIO to actually run as
 * an idle task, we ensure none of the following conditions are met.
 */
static bool idleprio_suitable(struct task_struct *p)
{
	return (!(p->sched_contributes_to_load) && !(p->flags & (PF_EXITING)) &&
		!signal_pending(p) && !rcu_read_critical(p) && !freezing(p));
}

/*
 * To determine if a task of SCHED_ISO can run in pseudo-realtime, we check
 * that the iso_refractory flag is not set.
 */
static inline bool isoprio_suitable(struct rq *rq)
{
	return !rq->iso_refractory;
}

static inline void inc_nr_running(struct rq *rq)
{
	rq->nr_running++;
	if (trace_sched_update_nr_running_tp_enabled()) {
		call_trace_sched_update_nr_running(rq, 1);
	}
}

static inline void dec_nr_running(struct rq *rq)
{
	rq->nr_running--;
	if (trace_sched_update_nr_running_tp_enabled()) {
		call_trace_sched_update_nr_running(rq, -1);
	}
}

/*
 * Adding to the runqueue. Enter with rq locked.
 */
static void enqueue_task(struct rq *rq, struct task_struct *p, int flags)
{
	unsigned int randseed, cflags = 0;
	u64 sl_id;

	if (!rt_task(p)) {
		/* Check it hasn't gotten rt from PI */
		if ((idleprio_task(p) && idleprio_suitable(p)) ||
		   (iso_task(p) && isoprio_suitable(rq)))
			p->prio = p->normal_prio;
		else
			p->prio = NORMAL_PRIO;
	} else
		rq->rt_nr_running++;
	/*
	 * The sl_id key passed to the skiplist generates a sorted list.
	 * Realtime and sched iso tasks run FIFO so they only need be sorted
	 * according to priority. The skiplist will put tasks of the same
	 * key inserted later in FIFO order. Tasks of sched normal, batch
	 * and idleprio are sorted according to their deadlines. Idleprio
	 * tasks are offset by an impossibly large deadline value ensuring
	 * they get sorted into last positions, but still according to their
	 * own deadlines. This creates a "landscape" of skiplists running
	 * from priority 0 realtime in first place to the lowest priority
	 * idleprio tasks last. Skiplist insertion is an O(log n) process.
	 */
	if (p->prio <= ISO_PRIO) {
		sl_id = p->prio;
	} else {
		sl_id = p->deadline;
		if (idleprio_task(p)) {
			if (p->prio == IDLE_PRIO)
				sl_id |= 0xF000000000000000;
			else
				sl_id += longest_deadline_diff();
		}
	}
	/*
	 * Some architectures don't have better than microsecond resolution
	 * so mask out ~microseconds as the random seed for skiplist insertion.
	 */
	update_clocks(rq);
	if (!(flags & ENQUEUE_RESTORE)) {
		sched_info_queued(rq, p);
		psi_enqueue(p, flags & ENQUEUE_WAKEUP);
	}

	randseed = (rq->niffies >> 10) & 0xFFFFFFFF;
	skiplist_insert(rq->sl, &p->node, sl_id, p, randseed);
	rq->best_key = rq->node->next[0]->key;
	if (p->in_iowait)
		cflags |= SCHED_CPUFREQ_IOWAIT;
	inc_nr_running(rq);
	update_load_avg(rq, cflags);
}

/*
 * Returns the relative length of deadline all compared to the shortest
 * deadline which is that of nice -20.
 */
static inline int task_prio_ratio(struct task_struct *p)
{
	return prio_ratios[TASK_USER_PRIO(p)];
}

/*
 * task_timeslice - all tasks of all priorities get the exact same timeslice
 * length. CPU distribution is handled by giving different deadlines to
 * tasks of different priorities. Use 128 as the base value for fast shifts.
 */
static inline int task_timeslice(struct task_struct *p)
{
	return (rr_interval * task_prio_ratio(p) / 128);
}

#ifdef CONFIG_SMP
/* Entered with rq locked */
static inline void resched_if_idle(struct rq *rq)
{
	if (rq_idle(rq))
		resched_task(rq->curr);
}

static inline bool rq_local(struct rq *rq)
{
	return (rq->cpu == smp_processor_id());
}
#ifdef CONFIG_SMT_NICE
static const cpumask_t *thread_cpumask(int cpu);

/* Find the best real time priority running on any SMT siblings of cpu and if
 * none are running, the static priority of the best deadline task running.
 * The lookups to the other runqueues is done lockless as the occasional wrong
 * value would be harmless. */
static int best_smt_bias(struct rq *this_rq)
{
	int other_cpu, best_bias = 0;

	for_each_cpu(other_cpu, &this_rq->thread_mask) {
		struct rq *rq = cpu_rq(other_cpu);

		if (rq_idle(rq))
			continue;
		if (unlikely(!rq->online))
			continue;
		if (!rq->rq_mm)
			continue;
		if (likely(rq->rq_smt_bias > best_bias))
			best_bias = rq->rq_smt_bias;
	}
	return best_bias;
}

static int task_prio_bias(struct task_struct *p)
{
	if (rt_task(p))
		return 1 << 30;
	else if (task_running_iso(p))
		return 1 << 29;
	else if (task_running_idle(p))
		return 0;
	return MAX_PRIO - p->static_prio;
}

static bool smt_always_schedule(struct task_struct __maybe_unused *p, struct rq __maybe_unused *this_rq)
{
	return true;
}

static bool (*smt_schedule)(struct task_struct *p, struct rq *this_rq) = &smt_always_schedule;

/* We've already decided p can run on CPU, now test if it shouldn't for SMT
 * nice reasons. */
static bool smt_should_schedule(struct task_struct *p, struct rq *this_rq)
{
	int best_bias, task_bias;

	/* Kernel threads always run */
	if (unlikely(!p->mm))
		return true;
	if (rt_task(p))
		return true;
	if (!idleprio_suitable(p))
		return true;
	best_bias = best_smt_bias(this_rq);
	/* The smt siblings are all idle or running IDLEPRIO */
	if (best_bias < 1)
		return true;
	task_bias = task_prio_bias(p);
	if (task_bias < 1)
		return false;
	if (task_bias >= best_bias)
		return true;
	/* Dither 25% cpu of normal tasks regardless of nice difference */
	if (best_bias % 4 == 1)
		return true;
	/* Sorry, you lose */
	return false;
}
#else /* CONFIG_SMT_NICE */
#define smt_schedule(p, this_rq) (true)
#endif /* CONFIG_SMT_NICE */

static inline void atomic_set_cpu(int cpu, cpumask_t *cpumask)
{
	set_bit(cpu, (volatile unsigned long *)cpumask);
}

/*
 * The cpu_idle_map stores a bitmap of all the CPUs currently idle to
 * allow easy lookup of whether any suitable idle CPUs are available.
 * It's cheaper to maintain a binary yes/no if there are any idle CPUs on the
 * idle_cpus variable than to do a full bitmask check when we are busy. The
 * bits are set atomically but read locklessly as occasional false positive /
 * negative is harmless.
 */
static inline void set_cpuidle_map(int cpu)
{
	if (likely(cpu_online(cpu)))
		atomic_set_cpu(cpu, &cpu_idle_map);
}

static inline void atomic_clear_cpu(int cpu, cpumask_t *cpumask)
{
	clear_bit(cpu, (volatile unsigned long *)cpumask);
}

static inline void clear_cpuidle_map(int cpu)
{
	atomic_clear_cpu(cpu, &cpu_idle_map);
}

static bool suitable_idle_cpus(struct task_struct *p)
{
	return (cpumask_intersects(p->cpus_ptr, &cpu_idle_map));
}

/*
 * Resched current on rq. We don't know if rq is local to this CPU nor if it
 * is locked so we do not use an intermediate variable for the task to avoid
 * having it dereferenced.
 */
static void resched_curr(struct rq *rq)
{
	int cpu;

	if (test_tsk_need_resched(rq->curr))
		return;

	rq->preempt = rq->curr;
	cpu = rq->cpu;

	/* We're doing this without holding the rq lock if it's not task_rq */

	if (cpu == smp_processor_id()) {
		set_tsk_need_resched(rq->curr);
		set_preempt_need_resched();
		return;
	}

	if (set_nr_and_not_polling(rq->curr))
		smp_sched_reschedule(cpu);
	else
		trace_sched_wake_idle_without_ipi(cpu);
}

#define CPUIDLE_DIFF_THREAD     (1)
#define CPUIDLE_DIFF_CORE_LLC   (2)
#define CPUIDLE_DIFF_CORE       (4)
#define CPUIDLE_CACHE_BUSY      (8)
#define CPUIDLE_DIFF_CPU        (16)
#define CPUIDLE_THREAD_BUSY     (32)
#define CPUIDLE_DIFF_NODE       (64)

/*
 * The best idle CPU is chosen according to the CPUIDLE ranking above where the
 * lowest value would give the most suitable CPU to schedule p onto next. The
 * order works out to be the following:
 *
 * Same thread, idle or busy cache, idle or busy threads
 * Other core, same cache, idle or busy cache, idle threads.
 * Same node, other CPU, idle cache, idle threads.
 * Same node, other CPU, busy cache, idle threads.
 * Other core, same cache, busy threads.
 * Same node, other CPU, busy threads.
 * Other node, other CPU, idle cache, idle threads.
 * Other node, other CPU, busy cache, idle threads.
 * Other node, other CPU, busy threads.
 */
static int best_mask_cpu(int best_cpu, struct rq *rq, cpumask_t *tmpmask)
{
	int best_ranking = CPUIDLE_DIFF_NODE | CPUIDLE_THREAD_BUSY |
		CPUIDLE_DIFF_CPU | CPUIDLE_CACHE_BUSY | CPUIDLE_DIFF_CORE |
		CPUIDLE_DIFF_CORE_LLC | CPUIDLE_DIFF_THREAD;
	int cpu_tmp;

	if (cpumask_test_cpu(best_cpu, tmpmask))
		goto out;

	for_each_cpu(cpu_tmp, tmpmask) {
		int ranking, locality;
		struct rq *tmp_rq;

		ranking = 0;
		tmp_rq = cpu_rq(cpu_tmp);

		locality = rq->cpu_locality[cpu_tmp];
#ifdef CONFIG_NUMA
		if (locality > LOCALITY_SMP)
			ranking |= CPUIDLE_DIFF_NODE;
		else
#endif
			if (locality > LOCALITY_MC)
				ranking |= CPUIDLE_DIFF_CPU;
#ifdef CONFIG_SCHED_MC
			else if (locality == LOCALITY_MC_LLC)
				ranking |= CPUIDLE_DIFF_CORE_LLC;
			else if (locality == LOCALITY_MC)
				ranking |= CPUIDLE_DIFF_CORE;
		if (!(tmp_rq->cache_idle(tmp_rq)))
			ranking |= CPUIDLE_CACHE_BUSY;
#endif
#ifdef CONFIG_SCHED_SMT
		if (locality == LOCALITY_SMT)
			ranking |= CPUIDLE_DIFF_THREAD;
#endif
		if (ranking < best_ranking
#ifdef CONFIG_SCHED_SMT
			|| (ranking == best_ranking && (tmp_rq->siblings_idle(tmp_rq)))
#endif
		) {
			best_cpu = cpu_tmp;
			best_ranking = ranking;
		}
	}
out:
	return best_cpu;
}

bool cpus_share_cache(int this_cpu, int that_cpu)
{
	struct rq *this_rq = cpu_rq(this_cpu);

	return (this_rq->cpu_locality[that_cpu] < LOCALITY_SMP);
}

/* As per resched_curr but only will resched idle task */
static inline void resched_idle(struct rq *rq)
{
	if (test_tsk_need_resched(rq->idle))
		return;

	rq->preempt = rq->idle;

	set_tsk_need_resched(rq->idle);

	if (rq_local(rq)) {
		set_preempt_need_resched();
		return;
	}

	smp_sched_reschedule(rq->cpu);
}

DEFINE_PER_CPU(cpumask_t, idlemask);

static struct rq *resched_best_idle(struct task_struct *p, int cpu)
{
	cpumask_t *tmpmask = &(per_cpu(idlemask, cpu));
	struct rq *rq;
	int best_cpu;

	cpumask_and(tmpmask, p->cpus_ptr, &cpu_idle_map);
	best_cpu = best_mask_cpu(cpu, task_rq(p), tmpmask);
	rq = cpu_rq(best_cpu);
	if (!smt_schedule(p, rq))
		return NULL;
	rq->preempt = p;
	resched_idle(rq);
	return rq;
}

static inline void resched_suitable_idle(struct task_struct *p)
{
	if (suitable_idle_cpus(p))
		resched_best_idle(p, task_cpu(p));
}

static inline struct rq *rq_order(struct rq *rq, int cpu)
{
	return rq->rq_order[cpu];
}
#else /* CONFIG_SMP */
static inline void set_cpuidle_map(int cpu)
{
}

static inline void clear_cpuidle_map(int cpu)
{
}

static inline bool suitable_idle_cpus(struct task_struct *p)
{
	return uprq->curr == uprq->idle;
}

static inline void resched_suitable_idle(struct task_struct *p)
{
}

static inline void resched_curr(struct rq *rq)
{
	resched_task(rq->curr);
}

static inline void resched_if_idle(struct rq *rq)
{
}

static inline bool rq_local(struct rq *rq)
{
	return true;
}

static inline struct rq *rq_order(struct rq *rq, int cpu)
{
	return rq;
}

static inline bool smt_schedule(struct task_struct *p, struct rq *rq)
{
	return true;
}
#endif /* CONFIG_SMP */

static inline int normal_prio(struct task_struct *p)
{
	if (has_rt_policy(p))
		return MAX_RT_PRIO - 1 - p->rt_priority;
	if (idleprio_task(p))
		return IDLE_PRIO;
	if (iso_task(p))
		return ISO_PRIO;
	return NORMAL_PRIO;
}

/*
 * Calculate the current priority, i.e. the priority
 * taken into account by the scheduler. This value might
 * be boosted by RT tasks as it will be RT if the task got
 * RT-boosted. If not then it returns p->normal_prio.
 */
static int effective_prio(struct task_struct *p)
{
	p->normal_prio = normal_prio(p);
	/*
	 * If we are RT tasks or we were boosted to RT priority,
	 * keep the priority unchanged. Otherwise, update priority
	 * to the normal priority:
	 */
	if (!rt_prio(p->prio))
		return p->normal_prio;
	return p->prio;
}

/*
 * activate_task - move a task to the runqueue. Enter with rq locked.
 */
static void activate_task(struct rq *rq, struct task_struct *p, int flags)
{
	resched_if_idle(rq);

	/*
	 * Sleep time is in units of nanosecs, so shift by 20 to get a
	 * milliseconds-range estimation of the amount of time that the task
	 * spent sleeping:
	 */
	if (unlikely(prof_on == SLEEP_PROFILING)) {
		if (p->state == TASK_UNINTERRUPTIBLE)
			profile_hits(SLEEP_PROFILING, (void *)get_wchan(p),
				     (rq->niffies - p->last_ran) >> 20);
	}

	p->prio = effective_prio(p);
	enqueue_task(rq, p, flags);
	p->on_rq = TASK_ON_RQ_QUEUED;
}

/*
 * deactivate_task - If it's running, it's not on the runqueue and we can just
 * decrement the nr_running. Enter with rq locked.
 */
static inline void deactivate_task(struct task_struct *p, struct rq *rq)
{
	p->on_rq = 0;
	sched_info_dequeued(rq, p);
	/* deactivate_task is always DEQUEUE_SLEEP in muqss */
	psi_dequeue(p, DEQUEUE_SLEEP);
}

#ifdef CONFIG_SMP
void set_task_cpu(struct task_struct *p, unsigned int new_cpu)
{
	struct rq *rq;

	if (task_cpu(p) == new_cpu)
		return;

	/* Do NOT call set_task_cpu on a currently queued task as we will not
	 * be reliably holding the rq lock after changing CPU. */
	BUG_ON(task_queued(p));
	rq = task_rq(p);

#ifdef CONFIG_LOCKDEP
	/*
	 * The caller should hold either p->pi_lock or rq->lock, when changing
	 * a task's CPU. ->pi_lock for waking tasks, rq->lock for runnable tasks.
	 *
	 * Furthermore, all task_rq users should acquire both locks, see
	 * task_rq_lock().
	 */
	WARN_ON_ONCE(debug_locks && !(lockdep_is_held(&p->pi_lock) ||
				      lockdep_is_held(rq->lock)));
#endif

	trace_sched_migrate_task(p, new_cpu);
	rseq_migrate(p);
	perf_event_task_migrate(p);

	/*
	 * After ->cpu is set up to a new value, task_rq_lock(p, ...) can be
	 * successfully executed on another CPU. We must ensure that updates of
	 * per-task data have been completed by this moment.
	 */
	smp_wmb();

	p->wake_cpu = new_cpu;

	if (task_running(rq, p)) {
		/*
		 * We should only be calling this on a running task if we're
		 * holding rq lock.
		 */
		lockdep_assert_held(rq->lock);

		/*
		 * We can't change the task_thread_info CPU on a running task
		 * as p will still be protected by the rq lock of the CPU it
		 * is still running on so we only set the wake_cpu for it to be
		 * lazily updated once off the CPU.
		 */
		return;
	}

#ifdef CONFIG_THREAD_INFO_IN_TASK
	WRITE_ONCE(p->cpu, new_cpu);
#else
	WRITE_ONCE(task_thread_info(p)->cpu, new_cpu);
#endif
	/* We're no longer protecting p after this point since we're holding
	 * the wrong runqueue lock. */
}
#endif /* CONFIG_SMP */

/*
 * Move a task off the runqueue and take it to a cpu for it will
 * become the running task.
 */
static inline void take_task(struct rq *rq, int cpu, struct task_struct *p)
{
	struct rq *p_rq = task_rq(p);

	dequeue_task(p_rq, p, DEQUEUE_SAVE);
	if (p_rq != rq) {
		sched_info_dequeued(p_rq, p);
		sched_info_queued(rq, p);
	}
	set_task_cpu(p, cpu);
}

/*
 * Returns a descheduling task to the runqueue unless it is being
 * deactivated.
 */
static inline void return_task(struct task_struct *p, struct rq *rq,
			       int cpu, bool deactivate)
{
	if (deactivate)
		deactivate_task(p, rq);
	else {
#ifdef CONFIG_SMP
		/*
		 * set_task_cpu was called on the running task that doesn't
		 * want to deactivate so it has to be enqueued to a different
		 * CPU and we need its lock. Tag it to be moved with as the
		 * lock is dropped in finish_lock_switch.
		 */
		if (unlikely(p->wake_cpu != cpu))
			WRITE_ONCE(p->on_rq, TASK_ON_RQ_MIGRATING);
		else
#endif
			enqueue_task(rq, p, ENQUEUE_RESTORE);
	}
}

/* Enter with rq lock held. We know p is on the local cpu */
static inline void __set_tsk_resched(struct task_struct *p)
{
	set_tsk_need_resched(p);
	set_preempt_need_resched();
}

/**
 * task_curr - is this task currently executing on a CPU?
 * @p: the task in question.
 *
 * Return: 1 if the task is currently executing. 0 otherwise.
 */
inline int task_curr(const struct task_struct *p)
{
	return cpu_curr(task_cpu(p)) == p;
}

#ifdef CONFIG_SMP
/*
 * wait_task_inactive - wait for a thread to unschedule.
 *
 * If @match_state is nonzero, it's the @p->state value just checked and
 * not expected to change.  If it changes, i.e. @p might have woken up,
 * then return zero.  When we succeed in waiting for @p to be off its CPU,
 * we return a positive number (its total switch count).  If a second call
 * a short while later returns the same number, the caller can be sure that
 * @p has remained unscheduled the whole time.
 *
 * The caller must ensure that the task *will* unschedule sometime soon,
 * else this function might spin for a *long* time. This function can't
 * be called with interrupts off, or it may introduce deadlock with
 * smp_call_function() if an IPI is sent by the same process we are
 * waiting to become inactive.
 */
unsigned long wait_task_inactive(struct task_struct *p, long match_state)
{
	int running, queued;
	struct rq_flags rf;
	unsigned long ncsw;
	struct rq *rq;

	for (;;) {
		rq = task_rq(p);

		/*
		 * If the task is actively running on another CPU
		 * still, just relax and busy-wait without holding
		 * any locks.
		 *
		 * NOTE! Since we don't hold any locks, it's not
		 * even sure that "rq" stays as the right runqueue!
		 * But we don't care, since this will return false
		 * if the runqueue has changed and p is actually now
		 * running somewhere else!
		 */
		while (task_running(rq, p)) {
			if (match_state && unlikely(p->state != match_state))
				return 0;
			cpu_relax();
		}

		/*
		 * Ok, time to look more closely! We need the rq
		 * lock now, to be *sure*. If we're wrong, we'll
		 * just go back and repeat.
		 */
		rq = task_rq_lock(p, &rf);
		trace_sched_wait_task(p);
		running = task_running(rq, p);
		queued = task_on_rq_queued(p);
		ncsw = 0;
		if (!match_state || p->state == match_state)
			ncsw = p->nvcsw | LONG_MIN; /* sets MSB */
		task_rq_unlock(rq, p, &rf);

		/*
		 * If it changed from the expected state, bail out now.
		 */
		if (unlikely(!ncsw))
			break;

		/*
		 * Was it really running after all now that we
		 * checked with the proper locks actually held?
		 *
		 * Oops. Go back and try again..
		 */
		if (unlikely(running)) {
			cpu_relax();
			continue;
		}

		/*
		 * It's not enough that it's not actively running,
		 * it must be off the runqueue _entirely_, and not
		 * preempted!
		 *
		 * So if it was still runnable (but just not actively
		 * running right now), it's preempted, and we should
		 * yield - it could be a while.
		 */
		if (unlikely(queued)) {
			ktime_t to = NSEC_PER_SEC / HZ;

			set_current_state(TASK_UNINTERRUPTIBLE);
			schedule_hrtimeout(&to, HRTIMER_MODE_REL);
			continue;
		}

		/*
		 * Ahh, all good. It wasn't running, and it wasn't
		 * runnable, which means that it will never become
		 * running in the future either. We're all done!
		 */
		break;
	}

	return ncsw;
}

/***
 * kick_process - kick a running thread to enter/exit the kernel
 * @p: the to-be-kicked thread
 *
 * Cause a process which is running on another CPU to enter
 * kernel-mode, without any delay. (to get signals handled.)
 *
 * NOTE: this function doesn't have to take the runqueue lock,
 * because all it wants to ensure is that the remote task enters
 * the kernel. If the IPI races and the task has been migrated
 * to another CPU then no harm is done and the purpose has been
 * achieved as well.
 */
void kick_process(struct task_struct *p)
{
	int cpu;

	preempt_disable();
	cpu = task_cpu(p);
	if ((cpu != smp_processor_id()) && task_curr(p))
		smp_sched_reschedule(cpu);
	preempt_enable();
}
EXPORT_SYMBOL_GPL(kick_process);
#endif

/*
 * RT tasks preempt purely on priority. SCHED_NORMAL tasks preempt on the
 * basis of earlier deadlines. SCHED_IDLEPRIO don't preempt anything else or
 * between themselves, they cooperatively multitask. An idle rq scores as
 * prio PRIO_LIMIT so it is always preempted.
 */
static inline bool
can_preempt(struct task_struct *p, int prio, u64 deadline)
{
	/* Better static priority RT task or better policy preemption */
	if (p->prio < prio)
		return true;
	if (p->prio > prio)
		return false;
	if (p->policy == SCHED_BATCH)
		return false;
	/* SCHED_NORMAL and ISO will preempt based on deadline */
	if (!deadline_before(p->deadline, deadline))
		return false;
	return true;
}

#ifdef CONFIG_SMP

/*
 * Per-CPU kthreads are allowed to run on !active && online CPUs, see
 * __set_cpus_allowed_ptr().
 */
static inline bool is_cpu_allowed(struct task_struct *p, int cpu)
{
	if (!cpumask_test_cpu(cpu, p->cpus_ptr))
		return false;

	if (is_per_cpu_kthread(p))
		return cpu_online(cpu);

	return cpu_active(cpu);
}

/*
 * Check to see if p can run on cpu, and if not, whether there are any online
 * CPUs it can run on instead. This only happens with the hotplug threads that
 * bring up the CPUs.
 */
static inline bool sched_other_cpu(struct task_struct *p, int cpu)
{
	if (likely(cpumask_test_cpu(cpu, p->cpus_ptr)))
		return false;
	if (p->nr_cpus_allowed == 1) {
		cpumask_t valid_mask;

		cpumask_and(&valid_mask, p->cpus_ptr, cpu_online_mask);
		if (unlikely(cpumask_empty(&valid_mask)))
			return false;
	}
	return true;
}

static inline bool needs_other_cpu(struct task_struct *p, int cpu)
{
	if (cpumask_test_cpu(cpu, p->cpus_ptr))
		return false;
	return true;
}

#define cpu_online_map		(*(cpumask_t *)cpu_online_mask)

static void try_preempt(struct task_struct *p, struct rq *this_rq)
{
	int i, this_entries = rq_load(this_rq);
	cpumask_t tmp;

	if (suitable_idle_cpus(p) && resched_best_idle(p, task_cpu(p)))
		return;

	/* IDLEPRIO tasks never preempt anything but idle */
	if (p->policy == SCHED_IDLEPRIO)
		return;

	cpumask_and(&tmp, &cpu_online_map, p->cpus_ptr);

	for (i = 0; i < num_online_cpus(); i++) {
		struct rq *rq = this_rq->cpu_order[i];

		if (!cpumask_test_cpu(rq->cpu, &tmp))
			continue;

		if (!sched_interactive && rq != this_rq && rq_load(rq) <= this_entries)
			continue;
		if (smt_schedule(p, rq) && can_preempt(p, rq->rq_prio, rq->rq_deadline)) {
			/* We set rq->preempting lockless, it's a hint only */
			rq->preempting = p;
			resched_curr(rq);
			return;
		}
	}
}

static int __set_cpus_allowed_ptr(struct task_struct *p,
				  const struct cpumask *new_mask, bool check);
#else /* CONFIG_SMP */
static inline bool needs_other_cpu(struct task_struct *p, int cpu)
{
	return false;
}

static void try_preempt(struct task_struct *p, struct rq *this_rq)
{
	if (p->policy == SCHED_IDLEPRIO)
		return;
	if (can_preempt(p, uprq->rq_prio, uprq->rq_deadline))
		resched_curr(uprq);
}

static inline int __set_cpus_allowed_ptr(struct task_struct *p,
					 const struct cpumask *new_mask, bool check)
{
	return set_cpus_allowed_ptr(p, new_mask);
}
#endif /* CONFIG_SMP */

static void
ttwu_stat(struct task_struct *p, int cpu, int wake_flags)
{
	struct rq *rq;

	if (!schedstat_enabled())
		return;

	rq = this_rq();

#ifdef CONFIG_SMP
	if (cpu == rq->cpu) {
		__schedstat_inc(rq->ttwu_local);
	} else {
		struct sched_domain *sd;

		rcu_read_lock();
		for_each_domain(rq->cpu, sd) {
			if (cpumask_test_cpu(cpu, sched_domain_span(sd))) {
				__schedstat_inc(sd->ttwu_wake_remote);
				break;
			}
		}
		rcu_read_unlock();
	}

#endif /* CONFIG_SMP */

	__schedstat_inc(rq->ttwu_count);
}

/*
 * Mark the task runnable and perform wakeup-preemption.
 */
static void ttwu_do_wakeup(struct rq *rq, struct task_struct *p, int wake_flags)
{
	/*
	 * Sync wakeups (i.e. those types of wakeups where the waker
	 * has indicated that it will leave the CPU in short order)
	 * don't trigger a preemption if there are no idle cpus,
	 * instead waiting for current to deschedule.
	 */
	if (wake_flags & WF_SYNC)
		resched_suitable_idle(p);
	else
		try_preempt(p, rq);
	p->state = TASK_RUNNING;
	trace_sched_wakeup(p);
}

static void
ttwu_do_activate(struct rq *rq, struct task_struct *p, int wake_flags)
{
	int en_flags = ENQUEUE_WAKEUP;

	lockdep_assert_held(rq->lock);

	if (p->sched_contributes_to_load)
		rq->nr_uninterruptible--;

#ifdef CONFIG_SMP
	if (wake_flags & WF_MIGRATED)
		en_flags |= ENQUEUE_MIGRATED;
	else
#endif
	if (p->in_iowait) {
		delayacct_blkio_end(p);
		atomic_dec(&task_rq(p)->nr_iowait);
	}

	activate_task(rq, p, en_flags);
	ttwu_do_wakeup(rq, p, wake_flags);
}

/*
 * Consider @p being inside a wait loop:
 *
 *   for (;;) {
 *      set_current_state(TASK_UNINTERRUPTIBLE);
 *
 *      if (CONDITION)
 *         break;
 *
 *      schedule();
 *   }
 *   __set_current_state(TASK_RUNNING);
 *
 * between set_current_state() and schedule(). In this case @p is still
 * runnable, so all that needs doing is change p->state back to TASK_RUNNING in
 * an atomic manner.
 *
 * By taking task_rq(p)->lock we serialize against schedule(), if @p->on_rq
 * then schedule() must still happen and p->state can be changed to
 * TASK_RUNNING. Otherwise we lost the race, schedule() has happened, and we
 * need to do a full wakeup with enqueue.
 *
 * Returns: %true when the wakeup is done,
 *          %false otherwise.
 */
static int ttwu_runnable(struct task_struct *p, int wake_flags)
{
	struct rq *rq;
	int ret = 0;

	rq = __task_rq_lock(p, NULL);
	if (likely(task_on_rq_queued(p))) {
		ttwu_do_wakeup(rq, p, wake_flags);
		ret = 1;
	}
	__task_rq_unlock(rq, NULL);

	return ret;
}

#ifdef CONFIG_SMP
void sched_ttwu_pending(void *arg)
{
	struct llist_node *llist = arg;
	struct rq *rq = this_rq();
	struct task_struct *p, *t;
	struct rq_flags rf;

	if (!llist)
		return;

	/*
	 * rq::ttwu_pending racy indication of out-standing wakeups.
	 * Races such that false-negatives are possible, since they
	 * are shorter lived that false-positives would be.
	 */
	WRITE_ONCE(rq->ttwu_pending, 0);

	rq_lock_irqsave(rq, &rf);

	llist_for_each_entry_safe(p, t, llist, wake_entry.llist) {
		if (WARN_ON_ONCE(p->on_cpu))
			smp_cond_load_acquire(&p->on_cpu, !VAL);

		if (WARN_ON_ONCE(task_cpu(p) != cpu_of(rq)))
			set_task_cpu(p, cpu_of(rq));

		ttwu_do_activate(rq, p, 0);
	}

	rq_unlock_irqrestore(rq, &rf);
}

void send_call_function_single_ipi(int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	if (!set_nr_if_polling(rq->idle))
		arch_send_call_function_single_ipi(cpu);
	else
		trace_sched_wake_idle_without_ipi(cpu);
}

/*
 * Queue a task on the target CPUs wake_list and wake the CPU via IPI if
 * necessary. The wakee CPU on receipt of the IPI will queue the task
 * via sched_ttwu_wakeup() for activation so the wakee incurs the cost
 * of the wakeup instead of the waker.
 */
static void __ttwu_queue_wakelist(struct task_struct *p, int cpu, int wake_flags)
{
	struct rq *rq = cpu_rq(cpu);

	WRITE_ONCE(rq->ttwu_pending, 1);
	__smp_call_single_queue(cpu, &p->wake_entry.llist);
}

void wake_up_if_idle(int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	struct rq_flags rf;

	rcu_read_lock();

	if (!is_idle_task(rcu_dereference(rq->curr)))
		goto out;

	if (set_nr_if_polling(rq->idle)) {
		trace_sched_wake_idle_without_ipi(cpu);
	} else {
		rq_lock_irqsave(rq, &rf);
		if (likely(is_idle_task(rq->curr)))
			smp_sched_reschedule(cpu);
		/* Else cpu is not in idle, do nothing here */
		rq_unlock_irqrestore(rq, &rf);
	}

out:
	rcu_read_unlock();
}

static inline bool ttwu_queue_cond(int cpu, int wake_flags)
{
	/*
	 * If the CPU does not share cache, then queue the task on the
	 * remote rqs wakelist to avoid accessing remote data.
	 */
	if (!cpus_share_cache(smp_processor_id(), cpu))
		return true;

	/*
	 * If the task is descheduling and the only running task on the
	 * CPU then use the wakelist to offload the task activation to
	 * the soon-to-be-idle CPU as the current CPU is likely busy.
	 * nr_running is checked to avoid unnecessary task stacking.
	 */
	if ((wake_flags & WF_ON_CPU) && cpu_rq(cpu)->nr_running <= 1)
		return true;

	return false;
}

static bool ttwu_queue_wakelist(struct task_struct *p, int cpu, int wake_flags)
{
	if (sched_feat(TTWU_QUEUE) && ttwu_queue_cond(cpu, wake_flags)) {
		if (WARN_ON_ONCE(cpu == smp_processor_id()))
			return false;

		sched_clock_cpu(cpu); /* Sync clocks across CPUs */
		__ttwu_queue_wakelist(p, cpu, wake_flags);
		return true;
	}

	return false;
}

static int valid_task_cpu(struct task_struct *p)
{
	cpumask_t valid_mask;

	if (p->flags & PF_KTHREAD)
		cpumask_and(&valid_mask, p->cpus_ptr, cpu_all_mask);
	else
		cpumask_and(&valid_mask, p->cpus_ptr, cpu_active_mask);

	if (unlikely(!cpumask_weight(&valid_mask))) {
		/* We shouldn't be hitting this any more */
		printk(KERN_WARNING "SCHED: No cpumask for %s/%d weight %d\n", p->comm,
		       p->pid, cpumask_weight(p->cpus_ptr));
		return cpumask_any(p->cpus_ptr);
	}
	return cpumask_any(&valid_mask);
}

/*
 * For a task that's just being woken up we have a valuable balancing
 * opportunity so choose the nearest cache most lightly loaded runqueue.
 * Entered with rq locked and returns with the chosen runqueue locked.
 */
static inline int select_best_cpu(struct task_struct *p)
{
	unsigned int idlest = ~0U;
	struct rq *rq = NULL;
	int i;

	if (suitable_idle_cpus(p)) {
		int cpu = task_cpu(p);

		if (unlikely(needs_other_cpu(p, cpu)))
			cpu = valid_task_cpu(p);
		rq = resched_best_idle(p, cpu);
		if (likely(rq))
			return rq->cpu;
	}

	for (i = 0; i < num_online_cpus(); i++) {
		struct rq *other_rq = task_rq(p)->cpu_order[i];
		int entries;

		if (!other_rq->online)
			continue;
		if (needs_other_cpu(p, other_rq->cpu))
			continue;
		entries = rq_load(other_rq);
		if (entries >= idlest)
			continue;
		idlest = entries;
		rq = other_rq;
	}
	if (unlikely(!rq))
		return task_cpu(p);
	return rq->cpu;
}
#else /* CONFIG_SMP */

static inline bool ttwu_queue_wakelist(struct task_struct *p, int cpu, int wake_flags)
{
	return false;
}

static int valid_task_cpu(struct task_struct *p)
{
	return 0;
}

static inline int select_best_cpu(struct task_struct *p)
{
	return 0;
}

static struct rq *resched_best_idle(struct task_struct *p, int cpu)
{
	return NULL;
}
#endif /* CONFIG_SMP */

static void ttwu_queue(struct task_struct *p, int cpu, int wake_flags)
{
	struct rq *rq = cpu_rq(cpu);

	if (ttwu_queue_wakelist(p, cpu, wake_flags))
		return;

	rq_lock(rq);
	update_rq_clock(rq);
	ttwu_do_activate(rq, p, wake_flags);
	rq_unlock(rq);
}

/***
 * try_to_wake_up - wake up a thread
 * @p: the thread to be awakened
 * @state: the mask of task states that can be woken
 * @wake_flags: wake modifier flags (WF_*)
 *
 * Put it on the run-queue if it's not already there. The "current"
 * thread is always on the run-queue (except when the actual
 * re-schedule is in progress), and as such you're allowed to do
 * the simpler "current->state = TASK_RUNNING" to mark yourself
 * runnable without the overhead of this.
 *
 * Return: %true if @p was woken up, %false if it was already running.
 * or @state didn't match @p's state.
 */
static int
try_to_wake_up(struct task_struct *p, unsigned int state, int wake_flags)
{
	unsigned long flags;
	int cpu, success = 0;

	preempt_disable();
	if (p == current) {
		/*
		 * We're waking current, this means 'p->on_rq' and 'task_cpu(p)
		 * == smp_processor_id()'. Together this means we can special
		 * case the whole 'p->on_rq && ttwu_runnable()' case below
		 * without taking any locks.
		 *
		 * In particular:
		 *  - we rely on Program-Order guarantees for all the ordering,
		 *  - we're serialized against set_special_state() by virtue of
		 *    it disabling IRQs (this allows not taking ->pi_lock).
		 */
		if (!(p->state & state))
			goto out;

		success = 1;
		trace_sched_waking(p);
		p->state = TASK_RUNNING;
		trace_sched_wakeup(p);
		goto out;
	}

	/*
	 * If we are going to wake up a thread waiting for CONDITION we
	 * need to ensure that CONDITION=1 done by the caller can not be
	 * reordered with p->state check below. This pairs with smp_store_mb()
	 * in set_current_state() that the waiting thread does.
	 */
	raw_spin_lock_irqsave(&p->pi_lock, flags);
	smp_mb__after_spinlock();
	if (!(p->state & state))
		goto unlock;

	trace_sched_waking(p);

	/* We're going to change ->state: */
	success = 1;

	/*
	 * Ensure we load p->on_rq _after_ p->state, otherwise it would
	 * be possible to, falsely, observe p->on_rq == 0 and get stuck
	 * in smp_cond_load_acquire() below.
	 *
	 * sched_ttwu_pending()			try_to_wake_up()
	 *   STORE p->on_rq = 1			  LOAD p->state
	 *   UNLOCK rq->lock
	 *
	 * __schedule() (switch to task 'p')
	 *   LOCK rq->lock			  smp_rmb();
	 *   smp_mb__after_spinlock();
	 *   UNLOCK rq->lock
	 *
	 * [task p]
	 *   STORE p->state = UNINTERRUPTIBLE	  LOAD p->on_rq
	 *
	 * Pairs with the LOCK+smp_mb__after_spinlock() on rq->lock in
	 * __schedule().  See the comment for smp_mb__after_spinlock().
	 */
	smp_rmb();
	if (READ_ONCE(p->on_rq) && ttwu_runnable(p, wake_flags))
		goto unlock;

#ifdef CONFIG_SMP
	/*
	 * Ensure we load p->on_cpu _after_ p->on_rq, otherwise it would be
	 * possible to, falsely, observe p->on_cpu == 0.
	 *
	 * One must be running (->on_cpu == 1) in order to remove oneself
	 * from the runqueue.
	 *
	 * __schedule() (switch to task 'p')	try_to_wake_up()
	 *   STORE p->on_cpu = 1		  LOAD p->on_rq
	 *   UNLOCK rq->lock
	 *
	 * __schedule() (put 'p' to sleep)
	 *   LOCK rq->lock			  smp_rmb();
	 *   smp_mb__after_spinlock();
	 *   STORE p->on_rq = 0			  LOAD p->on_cpu
	 *
	 * Pairs with the LOCK+smp_mb__after_spinlock() on rq->lock in
	 * __schedule().  See the comment for smp_mb__after_spinlock().
	 *
	 * Form a control-dep-acquire with p->on_rq == 0 above, to ensure
	 * schedule()'s deactivate_task() has 'happened' and p will no longer
	 * care about it's own p->state. See the comment in __schedule().
	 */
	smp_acquire__after_ctrl_dep();

	/*
	 * We're doing the wakeup (@success == 1), they did a dequeue (p->on_rq
	 * == 0), which means we need to do an enqueue, change p->state to
	 * TASK_WAKING such that we can unlock p->pi_lock before doing the
	 * enqueue, such as ttwu_queue_wakelist().
	 */
	p->state = TASK_WAKING;

	/*
	 * If the owning (remote) CPU is still in the middle of schedule() with
	 * this task as prev, considering queueing p on the remote CPUs wake_list
	 * which potentially sends an IPI instead of spinning on p->on_cpu to
	 * let the waker make forward progress. This is safe because IRQs are
	 * disabled and the IPI will deliver after on_cpu is cleared.
	 *
	 * Ensure we load task_cpu(p) after p->on_cpu:
	 *
	 * set_task_cpu(p, cpu);
	 *   STORE p->cpu = @cpu
	 * __schedule() (switch to task 'p')
	 *   LOCK rq->lock
	 *   smp_mb__after_spin_lock()		smp_cond_load_acquire(&p->on_cpu)
	 *   STORE p->on_cpu = 1		LOAD p->cpu
	 *
	 * to ensure we observe the correct CPU on which the task is currently
	 * scheduling.
	 */
	if (smp_load_acquire(&p->on_cpu) &&
	    ttwu_queue_wakelist(p, task_cpu(p), wake_flags | WF_ON_CPU))
		goto unlock;

	/*
	 * If the owning (remote) CPU is still in the middle of schedule() with
	 * this task as prev, wait until its done referencing the task.
	 *
	 * Pairs with the smp_store_release() in finish_task().
	 *
	 * This ensures that tasks getting woken will be fully ordered against
	 * their previous state and preserve Program Order.
	 */
	smp_cond_load_acquire(&p->on_cpu, !VAL);

	cpu = select_best_cpu(p);
	if (task_cpu(p) != cpu) {
		if (p->in_iowait) {
			delayacct_blkio_end(p);
			atomic_dec(&task_rq(p)->nr_iowait);
		}

		wake_flags |= WF_MIGRATED;
		psi_ttwu_dequeue(p);
		set_task_cpu(p, cpu);
	}

#else
	cpu = task_cpu(p);
#endif /* CONFIG_SMP */

	ttwu_queue(p, cpu, wake_flags);
unlock:
	raw_spin_unlock_irqrestore(&p->pi_lock, flags);
out:
	if (success)
		ttwu_stat(p, task_cpu(p), wake_flags);
	preempt_enable();

	return success;
}

/**
 * try_invoke_on_locked_down_task - Invoke a function on task in fixed state
 * @p: Process for which the function is to be invoked.
 * @func: Function to invoke.
 * @arg: Argument to function.
 *
 * If the specified task can be quickly locked into a definite state
 * (either sleeping or on a given runqueue), arrange to keep it in that
 * state while invoking @func(@arg).  This function can use ->on_rq and
 * task_curr() to work out what the state is, if required.  Given that
 * @func can be invoked with a runqueue lock held, it had better be quite
 * lightweight.
 *
 * Returns:
 *	@false if the task slipped out from under the locks.
 *	@true if the task was locked onto a runqueue or is sleeping.
 *		However, @func can override this by returning @false.
 */
bool try_invoke_on_locked_down_task(struct task_struct *p, bool (*func)(struct task_struct *t, void *arg), void *arg)
{
	bool ret = false;
	struct rq *rq;

	lockdep_assert_irqs_enabled();
	raw_spin_lock_irq(&p->pi_lock);
	if (p->on_rq) {
		rq = __task_rq_lock(p, NULL);
		if (task_rq(p) == rq)
			ret = func(p, arg);
		rq_unlock(rq);
	} else {
		switch (p->state) {
		case TASK_RUNNING:
		case TASK_WAKING:
			break;
		default:
			smp_rmb(); // See smp_rmb() comment in try_to_wake_up().
			if (!p->on_rq)
				ret = func(p, arg);
		}
	}
	raw_spin_unlock_irq(&p->pi_lock);
	return ret;
}

/**
 * wake_up_process - Wake up a specific process
 * @p: The process to be woken up.
 *
 * Attempt to wake up the nominated process and move it to the set of runnable
 * processes.
 *
 * Return: 1 if the process was woken up, 0 if it was already running.
 *
 * This function executes a full memory barrier before accessing the task state.
 */
int wake_up_process(struct task_struct *p)
{
	return try_to_wake_up(p, TASK_NORMAL, 0);
}
EXPORT_SYMBOL(wake_up_process);

int wake_up_state(struct task_struct *p, unsigned int state)
{
	return try_to_wake_up(p, state, 0);
}

static void time_slice_expired(struct task_struct *p, struct rq *rq);

/*
 * Perform scheduler related setup for a newly forked process p.
 * p is forked by current.
 */
int sched_fork(unsigned long __maybe_unused clone_flags, struct task_struct *p)
{
	unsigned long flags;

#ifdef CONFIG_PREEMPT_NOTIFIERS
	INIT_HLIST_HEAD(&p->preempt_notifiers);
#endif

#ifdef CONFIG_COMPACTION
	p->capture_control = NULL;
#endif

#ifdef CONFIG_SMP
	p->wake_entry.u_flags = CSD_TYPE_TTWU;
#endif
	/*
	 * We mark the process as NEW here. This guarantees that
	 * nobody will actually run it, and a signal or other external
	 * event cannot wake it up and insert it on the runqueue either.
	 */
	p->state = TASK_NEW;

	/*
	 * The process state is set to the same value of the process executing
	 * do_fork() code. That is running. This guarantees that nobody will
	 * actually run it, and a signal or other external event cannot wake
	 * it up and insert it on the runqueue either.
	 */

	/* Should be reset in fork.c but done here for ease of MuQSS patching */
	p->on_cpu =
	p->on_rq =
	p->utime =
	p->stime =
	p->sched_time =
	p->stime_ns =
	p->utime_ns = 0;
	skiplist_node_init(&p->node);

	/*
	 * Revert to default priority/policy on fork if requested.
	 */
	if (unlikely(p->sched_reset_on_fork)) {
		if (p->policy == SCHED_FIFO || p->policy == SCHED_RR || p-> policy == SCHED_ISO) {
			p->policy = SCHED_NORMAL;
			p->normal_prio = normal_prio(p);
		}

		if (PRIO_TO_NICE(p->static_prio) < 0) {
			p->static_prio = NICE_TO_PRIO(0);
			p->normal_prio = p->static_prio;
		}

		/*
		 * We don't need the reset flag anymore after the fork. It has
		 * fulfilled its duty:
		 */
		p->sched_reset_on_fork = 0;
	}

	/*
	 * Silence PROVE_RCU.
	 */
	raw_spin_lock_irqsave(&p->pi_lock, flags);
	rseq_migrate(p);
	set_task_cpu(p, smp_processor_id());
	raw_spin_unlock_irqrestore(&p->pi_lock, flags);

#ifdef CONFIG_SCHED_INFO
	if (unlikely(sched_info_on()))
		memset(&p->sched_info, 0, sizeof(p->sched_info));
#endif
	init_task_preempt_count(p);

	return 0;
}

void sched_post_fork(struct task_struct *p)
{
}

#ifdef CONFIG_SCHEDSTATS

DEFINE_STATIC_KEY_FALSE(sched_schedstats);
static bool __initdata __sched_schedstats = false;

static void set_schedstats(bool enabled)
{
	if (enabled)
		static_branch_enable(&sched_schedstats);
	else
		static_branch_disable(&sched_schedstats);
}

void force_schedstat_enabled(void)
{
	if (!schedstat_enabled()) {
		pr_info("kernel profiling enabled schedstats, disable via kernel.sched_schedstats.\n");
		static_branch_enable(&sched_schedstats);
	}
}

static int __init setup_schedstats(char *str)
{
	int ret = 0;
	if (!str)
		goto out;

	/*
	 * This code is called before jump labels have been set up, so we can't
	 * change the static branch directly just yet.  Instead set a temporary
	 * variable so init_schedstats() can do it later.
	 */
	if (!strcmp(str, "enable")) {
		__sched_schedstats = true;
		ret = 1;
	} else if (!strcmp(str, "disable")) {
		__sched_schedstats = false;
		ret = 1;
	}
out:
	if (!ret)
		pr_warn("Unable to parse schedstats=\n");

	return ret;
}
__setup("schedstats=", setup_schedstats);

static void __init init_schedstats(void)
{
	set_schedstats(__sched_schedstats);
}

#ifdef CONFIG_PROC_SYSCTL
int sysctl_schedstats(struct ctl_table *table, int write, void *buffer,
		size_t *lenp, loff_t *ppos)
{
	struct ctl_table t;
	int err;
	int state = static_branch_likely(&sched_schedstats);

	if (write && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	t = *table;
	t.data = &state;
	err = proc_dointvec_minmax(&t, write, buffer, lenp, ppos);
	if (err < 0)
		return err;
	if (write)
		set_schedstats(state);
	return err;
}
#endif /* CONFIG_PROC_SYSCTL */
#else  /* !CONFIG_SCHEDSTATS */
static inline void init_schedstats(void) {}
#endif /* CONFIG_SCHEDSTATS */

static void update_cpu_clock_switch(struct rq *rq, struct task_struct *p);

static void account_task_cpu(struct rq *rq, struct task_struct *p)
{
	update_clocks(rq);
	/* This isn't really a context switch but accounting is the same */
	update_cpu_clock_switch(rq, p);
	p->last_ran = rq->niffies;
}

bool sched_smp_initialized __read_mostly;

static inline int hrexpiry_enabled(struct rq *rq)
{
	if (unlikely(!cpu_active(cpu_of(rq)) || !sched_smp_initialized))
		return 0;
	return hrtimer_is_hres_active(&rq->hrexpiry_timer);
}

/*
 * Use HR-timers to deliver accurate preemption points.
 */
static inline void hrexpiry_clear(struct rq *rq)
{
	if (!hrexpiry_enabled(rq))
		return;
	if (hrtimer_active(&rq->hrexpiry_timer))
		hrtimer_cancel(&rq->hrexpiry_timer);
}

/*
 * High-resolution time_slice expiry.
 * Runs from hardirq context with interrupts disabled.
 */
static enum hrtimer_restart hrexpiry(struct hrtimer *timer)
{
	struct rq *rq = container_of(timer, struct rq, hrexpiry_timer);
	struct task_struct *p;

	/* This can happen during CPU hotplug / resume */
	if (unlikely(cpu_of(rq) != smp_processor_id()))
		goto out;

	/*
	 * We're doing this without the runqueue lock but this should always
	 * be run on the local CPU. Time slice should run out in __schedule
	 * but we set it to zero here in case niffies is slightly less.
	 */
	p = rq->curr;
	p->time_slice = 0;
	__set_tsk_resched(p);
out:
	return HRTIMER_NORESTART;
}

/*
 * Called to set the hrexpiry timer state.
 *
 * called with irqs disabled from the local CPU only
 */
static void hrexpiry_start(struct rq *rq, u64 delay)
{
	if (!hrexpiry_enabled(rq))
		return;

	hrtimer_start(&rq->hrexpiry_timer, ns_to_ktime(delay),
		      HRTIMER_MODE_REL_PINNED);
}

static void init_rq_hrexpiry(struct rq *rq)
{
	hrtimer_init(&rq->hrexpiry_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	rq->hrexpiry_timer.function = hrexpiry;
}

static inline int rq_dither(struct rq *rq)
{
	if (!hrexpiry_enabled(rq))
		return HALF_JIFFY_US;
	return 0;
}

/*
 * wake_up_new_task - wake up a newly created task for the first time.
 *
 * This function will do some initial scheduler statistics housekeeping
 * that must be done for every newly created context, then puts the task
 * on the runqueue and wakes it.
 */
void wake_up_new_task(struct task_struct *p)
{
	struct task_struct *parent, *rq_curr;
	struct rq *rq, *new_rq;
	unsigned long flags;

	parent = p->parent;

	raw_spin_lock_irqsave(&p->pi_lock, flags);
	p->state = TASK_RUNNING;
	/* Task_rq can't change yet on a new task */
	new_rq = rq = task_rq(p);
	if (unlikely(needs_other_cpu(p, task_cpu(p)))) {
		set_task_cpu(p, valid_task_cpu(p));
		new_rq = task_rq(p);
	}

	double_rq_lock(rq, new_rq);
	rq_curr = rq->curr;

	/*
	 * Make sure we do not leak PI boosting priority to the child.
	 */
	p->prio = rq_curr->normal_prio;

	trace_sched_wakeup_new(p);

	/*
	 * Share the timeslice between parent and child, thus the
	 * total amount of pending timeslices in the system doesn't change,
	 * resulting in more scheduling fairness. If it's negative, it won't
	 * matter since that's the same as being 0. rq->rq_deadline is only
	 * modified within schedule() so it is always equal to
	 * current->deadline.
	 */
	account_task_cpu(rq, rq_curr);
	p->last_ran = rq_curr->last_ran;
	if (likely(rq_curr->policy != SCHED_FIFO)) {
		rq_curr->time_slice /= 2;
		if (rq_curr->time_slice < RESCHED_US) {
			/*
			 * Forking task has run out of timeslice. Reschedule it and
			 * start its child with a new time slice and deadline. The
			 * child will end up running first because its deadline will
			 * be slightly earlier.
			 */
			__set_tsk_resched(rq_curr);
			time_slice_expired(p, new_rq);
			if (suitable_idle_cpus(p))
				resched_best_idle(p, task_cpu(p));
			else if (unlikely(rq != new_rq))
				try_preempt(p, new_rq);
		} else {
			p->time_slice = rq_curr->time_slice;
			if (rq_curr == parent && rq == new_rq && !suitable_idle_cpus(p)) {
				/*
				 * The VM isn't cloned, so we're in a good position to
				 * do child-runs-first in anticipation of an exec. This
				 * usually avoids a lot of COW overhead.
				 */
				__set_tsk_resched(rq_curr);
			} else {
				/*
				 * Adjust the hrexpiry since rq_curr will keep
				 * running and its timeslice has been shortened.
				 */
				hrexpiry_start(rq, US_TO_NS(rq_curr->time_slice));
				try_preempt(p, new_rq);
			}
		}
	} else {
		time_slice_expired(p, new_rq);
		try_preempt(p, new_rq);
	}
	activate_task(new_rq, p, 0);
	double_rq_unlock(rq, new_rq);
	raw_spin_unlock_irqrestore(&p->pi_lock, flags);
}

#ifdef CONFIG_PREEMPT_NOTIFIERS

static DEFINE_STATIC_KEY_FALSE(preempt_notifier_key);

void preempt_notifier_inc(void)
{
	static_branch_inc(&preempt_notifier_key);
}
EXPORT_SYMBOL_GPL(preempt_notifier_inc);

void preempt_notifier_dec(void)
{
	static_branch_dec(&preempt_notifier_key);
}
EXPORT_SYMBOL_GPL(preempt_notifier_dec);

/**
 * preempt_notifier_register - tell me when current is being preempted & rescheduled
 * @notifier: notifier struct to register
 */
void preempt_notifier_register(struct preempt_notifier *notifier)
{
	if (!static_branch_unlikely(&preempt_notifier_key))
		WARN(1, "registering preempt_notifier while notifiers disabled\n");

	hlist_add_head(&notifier->link, &current->preempt_notifiers);
}
EXPORT_SYMBOL_GPL(preempt_notifier_register);

/**
 * preempt_notifier_unregister - no longer interested in preemption notifications
 * @notifier: notifier struct to unregister
 *
 * This is *not* safe to call from within a preemption notifier.
 */
void preempt_notifier_unregister(struct preempt_notifier *notifier)
{
	hlist_del(&notifier->link);
}
EXPORT_SYMBOL_GPL(preempt_notifier_unregister);

static void __fire_sched_in_preempt_notifiers(struct task_struct *curr)
{
	struct preempt_notifier *notifier;

	hlist_for_each_entry(notifier, &curr->preempt_notifiers, link)
		notifier->ops->sched_in(notifier, raw_smp_processor_id());
}

static __always_inline void fire_sched_in_preempt_notifiers(struct task_struct *curr)
{
	if (static_branch_unlikely(&preempt_notifier_key))
		__fire_sched_in_preempt_notifiers(curr);
}

static void
__fire_sched_out_preempt_notifiers(struct task_struct *curr,
				 struct task_struct *next)
{
	struct preempt_notifier *notifier;

	hlist_for_each_entry(notifier, &curr->preempt_notifiers, link)
		notifier->ops->sched_out(notifier, next);
}

static __always_inline void
fire_sched_out_preempt_notifiers(struct task_struct *curr,
				 struct task_struct *next)
{
	if (static_branch_unlikely(&preempt_notifier_key))
		__fire_sched_out_preempt_notifiers(curr, next);
}

#else /* !CONFIG_PREEMPT_NOTIFIERS */

static inline void fire_sched_in_preempt_notifiers(struct task_struct *curr)
{
}

static inline void
fire_sched_out_preempt_notifiers(struct task_struct *curr,
				 struct task_struct *next)
{
}

#endif /* CONFIG_PREEMPT_NOTIFIERS */

static inline void prepare_task(struct task_struct *next)
{
	/*
	 * Claim the task as running, we do this before switching to it
	 * such that any running task will have this set.
	 */
	next->on_cpu = 1;
}

static inline void finish_task(struct task_struct *prev)
{
#ifdef CONFIG_SMP
	/*
	 * This must be the very last reference to @prev from this CPU. After
	 * p->on_cpu is cleared, the task can be moved to a different CPU. We
	 * must ensure this doesn't happen until the switch is completely
	 * finished.
	 *
	 * In particular, the load of prev->state in finish_task_switch() must
	 * happen before this.
	 *
	 * Pairs with the smp_cond_load_acquire() in try_to_wake_up().
	 */
	smp_store_release(&prev->on_cpu, 0);
#endif
}

static inline void
prepare_lock_switch(struct rq *rq, struct task_struct *next)
{
	/*
	 * Since the runqueue lock will be released by the next
	 * task (which is an invalid locking op but in the case
	 * of the scheduler it's an obvious special-case), so we
	 * do an early lockdep release here:
	 */
	spin_release(&rq->lock->dep_map, _THIS_IP_);
#ifdef CONFIG_DEBUG_SPINLOCK
	/* this is a valid case when another task releases the spinlock */
	rq->lock->owner = next;
#endif
}

static inline void finish_lock_switch(struct rq *rq, struct task_struct *prev)
{
	/*
	 * If we are tracking spinlock dependencies then we have to
	 * fix up the runqueue lock - which gets 'carried over' from
	 * prev into current:
	 */
	spin_acquire(&rq->lock->dep_map, 0, 0, _THIS_IP_);

#ifdef CONFIG_SMP
	/*
	 * If prev was marked as migrating to another CPU in return_task, drop
	 * the local runqueue lock but leave interrupts disabled and grab the
	 * remote lock we're migrating it to before enabling them.
	 */
	if (unlikely(task_on_rq_migrating(prev))) {
		sched_info_dequeued(rq, prev);
		/*
		 * We move the ownership of prev to the new cpu now. ttwu can't
		 * activate prev to the wrong cpu since it has to grab this
		 * runqueue in ttwu_remote.
		 */
#ifdef CONFIG_THREAD_INFO_IN_TASK
		prev->cpu = prev->wake_cpu;
#else
		task_thread_info(prev)->cpu = prev->wake_cpu;
#endif
		raw_spin_unlock(rq->lock);

		raw_spin_lock(&prev->pi_lock);
		rq = __task_rq_lock(prev, NULL);
		/* Check that someone else hasn't already queued prev */
		if (likely(!task_queued(prev))) {
			enqueue_task(rq, prev, 0);
			prev->on_rq = TASK_ON_RQ_QUEUED;
			/* Wake up the CPU if it's not already running */
			resched_if_idle(rq);
		}
		raw_spin_unlock(&prev->pi_lock);
	}
#endif
	rq_unlock(rq);
	local_irq_enable();
}

#ifndef prepare_arch_switch
# define prepare_arch_switch(next)	do { } while (0)
#endif
#ifndef finish_arch_switch
# define finish_arch_switch(prev)	do { } while (0)
#endif
#ifndef finish_arch_post_lock_switch
# define finish_arch_post_lock_switch()	do { } while (0)
#endif

/**
 * prepare_task_switch - prepare to switch tasks
 * @rq: the runqueue preparing to switch
 * @next: the task we are going to switch to.
 *
 * This is called with the rq lock held and interrupts off. It must
 * be paired with a subsequent finish_task_switch after the context
 * switch.
 *
 * prepare_task_switch sets up locking and calls architecture specific
 * hooks.
 */
static inline void
prepare_task_switch(struct rq *rq, struct task_struct *prev,
		    struct task_struct *next)
{
	kcov_prepare_switch(prev);
	sched_info_switch(rq, prev, next);
	perf_event_task_sched_out(prev, next);
	rseq_preempt(prev);
	fire_sched_out_preempt_notifiers(prev, next);
	prepare_task(next);
	prepare_arch_switch(next);
}

/**
 * finish_task_switch - clean up after a task-switch
 * @rq: runqueue associated with task-switch
 * @prev: the thread we just switched away from.
 *
 * finish_task_switch must be called after the context switch, paired
 * with a prepare_task_switch call before the context switch.
 * finish_task_switch will reconcile locking set up by prepare_task_switch,
 * and do any other architecture-specific cleanup actions.
 *
 * Note that we may have delayed dropping an mm in context_switch(). If
 * so, we finish that here outside of the runqueue lock.  (Doing it
 * with the lock held can cause deadlocks; see schedule() for
 * details.)
 *
 * The context switch have flipped the stack from under us and restored the
 * local variables which were saved when this task called schedule() in the
 * past. prev == current is still correct but we need to recalculate this_rq
 * because prev may have moved to another CPU.
 */
static void finish_task_switch(struct task_struct *prev)
	__releases(rq->lock)
{
	struct rq *rq = this_rq();
	struct mm_struct *mm = rq->prev_mm;
	long prev_state;

	/*
	 * The previous task will have left us with a preempt_count of 2
	 * because it left us after:
	 *
	 *	schedule()
	 *	  preempt_disable();			// 1
	 *	  __schedule()
	 *	    raw_spin_lock_irq(rq->lock)	// 2
	 *
	 * Also, see FORK_PREEMPT_COUNT.
	 */
	if (WARN_ONCE(preempt_count() != 2*PREEMPT_DISABLE_OFFSET,
		      "corrupted preempt_count: %s/%d/0x%x\n",
		      current->comm, current->pid, preempt_count()))
		preempt_count_set(FORK_PREEMPT_COUNT);

	rq->prev_mm = NULL;

	/*
	 * A task struct has one reference for the use as "current".
	 * If a task dies, then it sets TASK_DEAD in tsk->state and calls
	 * schedule one last time. The schedule call will never return, and
	 * the scheduled task must drop that reference.
	 *
	 * We must observe prev->state before clearing prev->on_cpu (in
	 * finish_task), otherwise a concurrent wakeup can get prev
	 * running on another CPU and we could rave with its RUNNING -> DEAD
	 * transition, resulting in a double drop.
	 */
	prev_state = prev->state;
	vtime_task_switch(prev);
	perf_event_task_sched_in(prev, current);
	finish_task(prev);
	finish_lock_switch(rq, prev);
	finish_arch_post_lock_switch();
	kcov_finish_switch(current);

	fire_sched_in_preempt_notifiers(current);
	/*
	 * When switching through a kernel thread, the loop in
	 * membarrier_{private,global}_expedited() may have observed that
	 * kernel thread and not issued an IPI. It is therefore possible to
	 * schedule between user->kernel->user threads without passing though
	 * switch_mm(). Membarrier requires a barrier after storing to
	 * rq->curr, before returning to userspace, so provide them here:
	 *
	 * - a full memory barrier for {PRIVATE,GLOBAL}_EXPEDITED, implicitly
	 *   provided by mmdrop(),
	 * - a sync_core for SYNC_CORE.
	 */
	if (mm) {
		membarrier_mm_sync_core_before_usermode(mm);
		mmdrop(mm);
	}
	if (unlikely(prev_state == TASK_DEAD)) {
		/*
		 * Remove function-return probe instances associated with this
		 * task and put them back on the free list.
		 */
		kprobe_flush_task(prev);

		/* Task is done with its stack. */
		put_task_stack(prev);

		put_task_struct_rcu_user(prev);
	}
}

/**
 * schedule_tail - first thing a freshly forked thread must call.
 * @prev: the thread we just switched away from.
 */
asmlinkage __visible void schedule_tail(struct task_struct *prev)
{
	/*
	 * New tasks start with FORK_PREEMPT_COUNT, see there and
	 * finish_task_switch() for details.
	 *
	 * finish_task_switch() will drop rq->lock() and lower preempt_count
	 * and the preempt_enable() will end up enabling preemption (on
	 * PREEMPT_COUNT kernels).
	 */

	finish_task_switch(prev);
	preempt_enable();

	if (current->set_child_tid)
		put_user(task_pid_vnr(current), current->set_child_tid);

	calculate_sigpending();
}

/*
 * context_switch - switch to the new MM and the new thread's register state.
 */
static __always_inline void
context_switch(struct rq *rq, struct task_struct *prev,
	       struct task_struct *next)
{
	prepare_task_switch(rq, prev, next);

	/*
	 * For paravirt, this is coupled with an exit in switch_to to
	 * combine the page table reload and the switch backend into
	 * one hypercall.
	 */
	arch_start_context_switch(prev);

	/*
	 * kernel -> kernel   lazy + transfer active
	 *   user -> kernel   lazy + mmgrab() active
	 *
	 * kernel ->   user   switch + mmdrop() active
	 *   user ->   user   switch
	 */
	if (!next->mm) {                                // to kernel
		enter_lazy_tlb(prev->active_mm, next);

		next->active_mm = prev->active_mm;
		if (prev->mm)                           // from user
			mmgrab(prev->active_mm);
		else
			prev->active_mm = NULL;
	} else {                                        // to user
		membarrier_switch_mm(rq, prev->active_mm, next->mm);
		/*
		 * sys_membarrier() requires an smp_mb() between setting
		 * rq->curr / membarrier_switch_mm() and returning to userspace.
		 *
		 * The below provides this either through switch_mm(), or in
		 * case 'prev->active_mm == next->mm' through
		 * finish_task_switch()'s mmdrop().
		 */
		switch_mm_irqs_off(prev->active_mm, next->mm, next);

		if (!prev->mm) {                        // from kernel
			/* will mmdrop() in finish_task_switch(). */
			rq->prev_mm = prev->active_mm;
			prev->active_mm = NULL;
		}
	}
	prepare_lock_switch(rq, next);

	/* Here we just switch the register state and the stack. */
	switch_to(prev, next, prev);
	barrier();

	finish_task_switch(prev);
}

/*
 * nr_running, nr_uninterruptible and nr_context_switches:
 *
 * externally visible scheduler statistics: current number of runnable
 * threads, total number of context switches performed since bootup.
 */
unsigned long nr_running(void)
{
	unsigned long i, sum = 0;

	for_each_online_cpu(i)
		sum += cpu_rq(i)->nr_running;

	return sum;
}

static unsigned long nr_uninterruptible(void)
{
	unsigned long i, sum = 0;

	for_each_online_cpu(i)
		sum += cpu_rq(i)->nr_uninterruptible;

	return sum;
}

/*
 * Check if only the current task is running on the CPU.
 *
 * Caution: this function does not check that the caller has disabled
 * preemption, thus the result might have a time-of-check-to-time-of-use
 * race.  The caller is responsible to use it correctly, for example:
 *
 * - from a non-preemptible section (of course)
 *
 * - from a thread that is bound to a single CPU
 *
 * - in a loop with very short iterations (e.g. a polling loop)
 */
bool single_task_running(void)
{
	if (rq_load(raw_rq()) == 1)
		return true;
	else
		return false;
}
EXPORT_SYMBOL(single_task_running);

unsigned long long nr_context_switches(void)
{
	int cpu;
	unsigned long long sum = 0;

	for_each_possible_cpu(cpu)
		sum += cpu_rq(cpu)->nr_switches;

	return sum;
}

/*
 * Consumers of these two interfaces, like for example the cpufreq menu
 * governor are using nonsensical data. Boosting frequency for a CPU that has
 * IO-wait which might not even end up running the task when it does become
 * runnable.
 */

unsigned long nr_iowait_cpu(int cpu)
{
	return atomic_read(&cpu_rq(cpu)->nr_iowait);
}

/*
 * IO-wait accounting, and how its mostly bollocks (on SMP).
 *
 * The idea behind IO-wait account is to account the idle time that we could
 * have spend running if it were not for IO. That is, if we were to improve the
 * storage performance, we'd have a proportional reduction in IO-wait time.
 *
 * This all works nicely on UP, where, when a task blocks on IO, we account
 * idle time as IO-wait, because if the storage were faster, it could've been
 * running and we'd not be idle.
 *
 * This has been extended to SMP, by doing the same for each CPU. This however
 * is broken.
 *
 * Imagine for instance the case where two tasks block on one CPU, only the one
 * CPU will have IO-wait accounted, while the other has regular idle. Even
 * though, if the storage were faster, both could've ran at the same time,
 * utilising both CPUs.
 *
 * This means, that when looking globally, the current IO-wait accounting on
 * SMP is a lower bound, by reason of under accounting.
 *
 * Worse, since the numbers are provided per CPU, they are sometimes
 * interpreted per CPU, and that is nonsensical. A blocked task isn't strictly
 * associated with any one particular CPU, it can wake to another CPU than it
 * blocked on. This means the per CPU IO-wait number is meaningless.
 *
 * Task CPU affinities can make all that even more 'interesting'.
 */

unsigned long nr_iowait(void)
{
	unsigned long cpu, sum = 0;

	for_each_possible_cpu(cpu)
		sum += nr_iowait_cpu(cpu);

	return sum;
}

unsigned long nr_active(void)
{
	return nr_running() + nr_uninterruptible();
}

/* Variables and functions for calc_load */
static unsigned long calc_load_update;
unsigned long avenrun[3];
EXPORT_SYMBOL(avenrun);

/**
 * get_avenrun - get the load average array
 * @loads:	pointer to dest load array
 * @offset:	offset to add
 * @shift:	shift count to shift the result left
 *
 * These values are estimates at best, so no need for locking.
 */
void get_avenrun(unsigned long *loads, unsigned long offset, int shift)
{
	loads[0] = (avenrun[0] + offset) << shift;
	loads[1] = (avenrun[1] + offset) << shift;
	loads[2] = (avenrun[2] + offset) << shift;
}

/*
 * calc_load - update the avenrun load estimates every LOAD_FREQ seconds.
 */
void calc_global_load(void)
{
	long active;

	if (time_before(jiffies, READ_ONCE(calc_load_update)))
		return;
	active = nr_active() * FIXED_1;

	avenrun[0] = calc_load(avenrun[0], EXP_1, active);
	avenrun[1] = calc_load(avenrun[1], EXP_5, active);
	avenrun[2] = calc_load(avenrun[2], EXP_15, active);

	calc_load_update = jiffies + LOAD_FREQ;
}

/**
 * fixed_power_int - compute: x^n, in O(log n) time
 *
 * @x:         base of the power
 * @frac_bits: fractional bits of @x
 * @n:         power to raise @x to.
 *
 * By exploiting the relation between the definition of the natural power
 * function: x^n := x*x*...*x (x multiplied by itself for n times), and
 * the binary encoding of numbers used by computers: n := \Sum n_i * 2^i,
 * (where: n_i \elem {0, 1}, the binary vector representing n),
 * we find: x^n := x^(\Sum n_i * 2^i) := \Prod x^(n_i * 2^i), which is
 * of course trivially computable in O(log_2 n), the length of our binary
 * vector.
 */
static unsigned long
fixed_power_int(unsigned long x, unsigned int frac_bits, unsigned int n)
{
	unsigned long result = 1UL << frac_bits;

	if (n) {
		for (;;) {
			if (n & 1) {
				result *= x;
				result += 1UL << (frac_bits - 1);
				result >>= frac_bits;
			}
			n >>= 1;
			if (!n)
				break;
			x *= x;
			x += 1UL << (frac_bits - 1);
			x >>= frac_bits;
		}
	}

	return result;
}

/*
 * a1 = a0 * e + a * (1 - e)
 *
 * a2 = a1 * e + a * (1 - e)
 *    = (a0 * e + a * (1 - e)) * e + a * (1 - e)
 *    = a0 * e^2 + a * (1 - e) * (1 + e)
 *
 * a3 = a2 * e + a * (1 - e)
 *    = (a0 * e^2 + a * (1 - e) * (1 + e)) * e + a * (1 - e)
 *    = a0 * e^3 + a * (1 - e) * (1 + e + e^2)
 *
 *  ...
 *
 * an = a0 * e^n + a * (1 - e) * (1 + e + ... + e^n-1) [1]
 *    = a0 * e^n + a * (1 - e) * (1 - e^n)/(1 - e)
 *    = a0 * e^n + a * (1 - e^n)
 *
 * [1] application of the geometric series:
 *
 *              n         1 - x^(n+1)
 *     S_n := \Sum x^i = -------------
 *             i=0          1 - x
 */
unsigned long
calc_load_n(unsigned long load, unsigned long exp,
	    unsigned long active, unsigned int n)
{
	return calc_load(load, fixed_power_int(exp, FSHIFT, n), active);
}

DEFINE_PER_CPU(struct kernel_stat, kstat);
DEFINE_PER_CPU(struct kernel_cpustat, kernel_cpustat);

EXPORT_PER_CPU_SYMBOL(kstat);
EXPORT_PER_CPU_SYMBOL(kernel_cpustat);

#ifdef CONFIG_PARAVIRT
static inline u64 steal_ticks(u64 steal)
{
	if (unlikely(steal > NSEC_PER_SEC))
		return div_u64(steal, TICK_NSEC);

	return __iter_div_u64_rem(steal, TICK_NSEC, &steal);
}
#endif

#ifndef nsecs_to_cputime
# define nsecs_to_cputime(__nsecs)	nsecs_to_jiffies(__nsecs)
#endif

/*
 * On each tick, add the number of nanoseconds to the unbanked variables and
 * once one tick's worth has accumulated, account it allowing for accurate
 * sub-tick accounting and totals. Use the TICK_APPROX_NS to match the way we
 * deduct nanoseconds.
 */
static void pc_idle_time(struct rq *rq, struct task_struct *idle, unsigned long ns)
{
	u64 *cpustat = kcpustat_this_cpu->cpustat;
	unsigned long ticks;

	if (atomic_read(&rq->nr_iowait) > 0) {
		rq->iowait_ns += ns;
		if (rq->iowait_ns >= JIFFY_NS) {
			ticks = NS_TO_JIFFIES(rq->iowait_ns);
			cpustat[CPUTIME_IOWAIT] += (__force u64)TICK_APPROX_NS * ticks;
			rq->iowait_ns %= JIFFY_NS;
		}
	} else {
		rq->idle_ns += ns;
		if (rq->idle_ns >= JIFFY_NS) {
			ticks = NS_TO_JIFFIES(rq->idle_ns);
			cpustat[CPUTIME_IDLE] += (__force u64)TICK_APPROX_NS * ticks;
			rq->idle_ns %= JIFFY_NS;
		}
	}
	acct_update_integrals(idle);
}

static void pc_system_time(struct rq *rq, struct task_struct *p,
			   int hardirq_offset, unsigned long ns)
{
	u64 *cpustat = kcpustat_this_cpu->cpustat;
	unsigned long ticks;

	p->stime_ns += ns;
	if (p->stime_ns >= JIFFY_NS) {
		ticks = NS_TO_JIFFIES(p->stime_ns);
		p->stime_ns %= JIFFY_NS;
		p->stime += (__force u64)TICK_APPROX_NS * ticks;
		account_group_system_time(p, TICK_APPROX_NS * ticks);
	}
	p->sched_time += ns;
	account_group_exec_runtime(p, ns);

	if (hardirq_count() - hardirq_offset) {
		rq->irq_ns += ns;
		if (rq->irq_ns >= JIFFY_NS) {
			ticks = NS_TO_JIFFIES(rq->irq_ns);
			cpustat[CPUTIME_IRQ] += (__force u64)TICK_APPROX_NS * ticks;
			rq->irq_ns %= JIFFY_NS;
		}
	} else if (in_serving_softirq()) {
		rq->softirq_ns += ns;
		if (rq->softirq_ns >= JIFFY_NS) {
			ticks = NS_TO_JIFFIES(rq->softirq_ns);
			cpustat[CPUTIME_SOFTIRQ] += (__force u64)TICK_APPROX_NS * ticks;
			rq->softirq_ns %= JIFFY_NS;
		}
	} else {
		rq->system_ns += ns;
		if (rq->system_ns >= JIFFY_NS) {
			ticks = NS_TO_JIFFIES(rq->system_ns);
			cpustat[CPUTIME_SYSTEM] += (__force u64)TICK_APPROX_NS * ticks;
			rq->system_ns %= JIFFY_NS;
		}
	}
	acct_update_integrals(p);
}

static void pc_user_time(struct rq *rq, struct task_struct *p, unsigned long ns)
{
	u64 *cpustat = kcpustat_this_cpu->cpustat;
	unsigned long ticks;

	p->utime_ns += ns;
	if (p->utime_ns >= JIFFY_NS) {
		ticks = NS_TO_JIFFIES(p->utime_ns);
		p->utime_ns %= JIFFY_NS;
		p->utime += (__force u64)TICK_APPROX_NS * ticks;
		account_group_user_time(p, TICK_APPROX_NS * ticks);
	}
	p->sched_time += ns;
	account_group_exec_runtime(p, ns);

	if (this_cpu_ksoftirqd() == p) {
		/*
		 * ksoftirqd time do not get accounted in cpu_softirq_time.
		 * So, we have to handle it separately here.
		 */
		rq->softirq_ns += ns;
		if (rq->softirq_ns >= JIFFY_NS) {
			ticks = NS_TO_JIFFIES(rq->softirq_ns);
			cpustat[CPUTIME_SOFTIRQ] += (__force u64)TICK_APPROX_NS * ticks;
			rq->softirq_ns %= JIFFY_NS;
		}
	}

	if (task_nice(p) > 0 || idleprio_task(p)) {
		rq->nice_ns += ns;
		if (rq->nice_ns >= JIFFY_NS) {
			ticks = NS_TO_JIFFIES(rq->nice_ns);
			cpustat[CPUTIME_NICE] += (__force u64)TICK_APPROX_NS * ticks;
			rq->nice_ns %= JIFFY_NS;
		}
	} else {
		rq->user_ns += ns;
		if (rq->user_ns >= JIFFY_NS) {
			ticks = NS_TO_JIFFIES(rq->user_ns);
			cpustat[CPUTIME_USER] += (__force u64)TICK_APPROX_NS * ticks;
			rq->user_ns %= JIFFY_NS;
		}
	}
	acct_update_integrals(p);
}

/*
 * This is called on clock ticks.
 * Bank in p->sched_time the ns elapsed since the last tick or switch.
 * CPU scheduler quota accounting is also performed here in microseconds.
 */
static void update_cpu_clock_tick(struct rq *rq, struct task_struct *p)
{
	s64 account_ns = rq->niffies - p->last_ran;
	struct task_struct *idle = rq->idle;

	/* Accurate tick timekeeping */
	if (user_mode(get_irq_regs()))
		pc_user_time(rq, p, account_ns);
	else if (p != idle || (irq_count() != HARDIRQ_OFFSET)) {
		pc_system_time(rq, p, HARDIRQ_OFFSET, account_ns);
	} else
		pc_idle_time(rq, idle, account_ns);

	/* time_slice accounting is done in usecs to avoid overflow on 32bit */
	if (p->policy != SCHED_FIFO && p != idle)
		p->time_slice -= NS_TO_US(account_ns);

	p->last_ran = rq->niffies;
}

/*
 * This is called on context switches.
 * Bank in p->sched_time the ns elapsed since the last tick or switch.
 * CPU scheduler quota accounting is also performed here in microseconds.
 */
static void update_cpu_clock_switch(struct rq *rq, struct task_struct *p)
{
	s64 account_ns = rq->niffies - p->last_ran;
	struct task_struct *idle = rq->idle;

	/* Accurate subtick timekeeping */
	if (p != idle)
		pc_user_time(rq, p, account_ns);
	else
		pc_idle_time(rq, idle, account_ns);

	/* time_slice accounting is done in usecs to avoid overflow on 32bit */
	if (p->policy != SCHED_FIFO && p != idle)
		p->time_slice -= NS_TO_US(account_ns);
}

/*
 * Return any ns on the sched_clock that have not yet been accounted in
 * @p in case that task is currently running.
 *
 * Called with task_rq_lock(p) held.
 */
static inline u64 do_task_delta_exec(struct task_struct *p, struct rq *rq)
{
	u64 ns = 0;

	/*
	 * Must be ->curr _and_ ->on_rq.  If dequeued, we would
	 * project cycles that may never be accounted to this
	 * thread, breaking clock_gettime().
	 */
	if (p == rq->curr && task_on_rq_queued(p)) {
		update_clocks(rq);
		ns = rq->niffies - p->last_ran;
	}

	return ns;
}

/*
 * Return accounted runtime for the task.
 * Return separately the current's pending runtime that have not been
 * accounted yet.
 */
unsigned long long task_sched_runtime(struct task_struct *p)
{
	struct rq_flags rf;
	struct rq *rq;
	u64 ns;

#if defined(CONFIG_64BIT) && defined(CONFIG_SMP)
	/*
	 * 64-bit doesn't need locks to atomically read a 64-bit value.
	 * So we have a optimisation chance when the task's delta_exec is 0.
	 * Reading ->on_cpu is racy, but this is ok.
	 *
	 * If we race with it leaving CPU, we'll take a lock. So we're correct.
	 * If we race with it entering CPU, unaccounted time is 0. This is
	 * indistinguishable from the read occurring a few cycles earlier.
	 * If we see ->on_cpu without ->on_rq, the task is leaving, and has
	 * been accounted, so we're correct here as well.
	 */
	if (!p->on_cpu || !task_on_rq_queued(p))
		return tsk_seruntime(p);
#endif

	rq = task_rq_lock(p, &rf);
	ns = p->sched_time + do_task_delta_exec(p, rq);
	task_rq_unlock(rq, p, &rf);

	return ns;
}

/*
 * Functions to test for when SCHED_ISO tasks have used their allocated
 * quota as real time scheduling and convert them back to SCHED_NORMAL. All
 * data is modified only by the local runqueue during scheduler_tick with
 * interrupts disabled.
 */

/*
 * Test if SCHED_ISO tasks have run longer than their alloted period as RT
 * tasks and set the refractory flag if necessary. There is 10% hysteresis
 * for unsetting the flag. 115/128 is ~90/100 as a fast shift instead of a
 * slow division.
 */
static inline void iso_tick(struct rq *rq)
{
	rq->iso_ticks = rq->iso_ticks * (ISO_PERIOD - 1) / ISO_PERIOD;
	rq->iso_ticks += 100;
	if (rq->iso_ticks > ISO_PERIOD * sched_iso_cpu) {
		rq->iso_refractory = true;
		if (unlikely(rq->iso_ticks > ISO_PERIOD * 100))
			rq->iso_ticks = ISO_PERIOD * 100;
	}
}

/* No SCHED_ISO task was running so decrease rq->iso_ticks */
static inline void no_iso_tick(struct rq *rq, int ticks)
{
	if (rq->iso_ticks > 0 || rq->iso_refractory) {
		rq->iso_ticks = rq->iso_ticks * (ISO_PERIOD - ticks) / ISO_PERIOD;
		if (rq->iso_ticks < ISO_PERIOD * (sched_iso_cpu * 115 / 128)) {
			rq->iso_refractory = false;
			if (unlikely(rq->iso_ticks < 0))
				rq->iso_ticks = 0;
		}
	}
}

/* This manages tasks that have run out of timeslice during a scheduler_tick */
static void task_running_tick(struct rq *rq)
{
	struct task_struct *p = rq->curr;

	/*
	 * If a SCHED_ISO task is running we increment the iso_ticks. In
	 * order to prevent SCHED_ISO tasks from causing starvation in the
	 * presence of true RT tasks we account those as iso_ticks as well.
	 */
	if (rt_task(p) || task_running_iso(p))
		iso_tick(rq);
	else
		no_iso_tick(rq, 1);

	/* SCHED_FIFO tasks never run out of timeslice. */
	if (p->policy == SCHED_FIFO)
		return;

	if (iso_task(p)) {
		if (task_running_iso(p)) {
			if (rq->iso_refractory) {
				/*
				 * SCHED_ISO task is running as RT and limit
				 * has been hit. Force it to reschedule as
				 * SCHED_NORMAL by zeroing its time_slice
				 */
				p->time_slice = 0;
			}
		} else if (!rq->iso_refractory) {
			/* Can now run again ISO. Reschedule to pick up prio */
			goto out_resched;
		}
	}

	/*
	 * Tasks that were scheduled in the first half of a tick are not
	 * allowed to run into the 2nd half of the next tick if they will
	 * run out of time slice in the interim. Otherwise, if they have
	 * less than RESCHED_US μs of time slice left they will be rescheduled.
	 * Dither is used as a backup for when hrexpiry is disabled or high res
	 * timers not configured in.
	 */
	if (p->time_slice - rq->dither >= RESCHED_US)
		return;
out_resched:
	rq_lock(rq);
	__set_tsk_resched(p);
	rq_unlock(rq);
}

static inline void task_tick(struct rq *rq)
{
	if (!rq_idle(rq))
		task_running_tick(rq);
	else if (rq->last_jiffy > rq->last_scheduler_tick)
		no_iso_tick(rq, rq->last_jiffy - rq->last_scheduler_tick);
}

#ifdef CONFIG_NO_HZ_FULL
/*
 * We can stop the timer tick any time highres timers are active since
 * we rely entirely on highres timeouts for task expiry rescheduling.
 */
static void sched_stop_tick(struct rq *rq, int cpu)
{
	if (!hrexpiry_enabled(rq))
		return;
	if (!tick_nohz_full_enabled())
		return;
	if (!tick_nohz_full_cpu(cpu))
		return;
	tick_nohz_dep_clear_cpu(cpu, TICK_DEP_BIT_SCHED);
}

static inline void sched_start_tick(struct rq *rq, int cpu)
{
	tick_nohz_dep_set_cpu(cpu, TICK_DEP_BIT_SCHED);
}

struct tick_work {
	int			cpu;
	atomic_t		state;
	struct delayed_work	work;
};
/* Values for ->state, see diagram below. */
#define TICK_SCHED_REMOTE_OFFLINE	0
#define TICK_SCHED_REMOTE_OFFLINING	1
#define TICK_SCHED_REMOTE_RUNNING	2

/*
 * State diagram for ->state:
 *
 *
 *          TICK_SCHED_REMOTE_OFFLINE
 *                    |   ^
 *                    |   |
 *                    |   | sched_tick_remote()
 *                    |   |
 *                    |   |
 *                    +--TICK_SCHED_REMOTE_OFFLINING
 *                    |   ^
 *                    |   |
 * sched_tick_start() |   | sched_tick_stop()
 *                    |   |
 *                    V   |
 *          TICK_SCHED_REMOTE_RUNNING
 *
 *
 * Other transitions get WARN_ON_ONCE(), except that sched_tick_remote()
 * and sched_tick_start() are happy to leave the state in RUNNING.
 */

static struct tick_work __percpu *tick_work_cpu;

static void sched_tick_remote(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct tick_work *twork = container_of(dwork, struct tick_work, work);
	int cpu = twork->cpu;
	struct rq *rq = cpu_rq(cpu);
	struct task_struct *curr;
	u64 delta;
	int os;

	/*
	 * Handle the tick only if it appears the remote CPU is running in full
	 * dynticks mode. The check is racy by nature, but missing a tick or
	 * having one too much is no big deal because the scheduler tick updates
	 * statistics and checks timeslices in a time-independent way, regardless
	 * of when exactly it is running.
	 */
	if (!tick_nohz_tick_stopped_cpu(cpu))
		goto out_requeue;

	rq_lock_irq(rq);
	if (cpu_is_offline(cpu))
		goto out_unlock;

	curr = rq->curr;
	update_rq_clock(rq);

	if (!is_idle_task(curr)) {
		/*
		 * Make sure the next tick runs within a reasonable
		 * amount of time.
		 */
		delta = rq_clock_task(rq) - curr->last_ran;
		WARN_ON_ONCE(delta > (u64)NSEC_PER_SEC * 3);
	}
	task_tick(rq);

out_unlock:
	rq_unlock_irq(rq, NULL);

out_requeue:

	/*
	 * Run the remote tick once per second (1Hz). This arbitrary
	 * frequency is large enough to avoid overload but short enough
	 * to keep scheduler internal stats reasonably up to date.  But
	 * first update state to reflect hotplug activity if required.
	 */
	os = atomic_fetch_add_unless(&twork->state, -1, TICK_SCHED_REMOTE_RUNNING);
	WARN_ON_ONCE(os == TICK_SCHED_REMOTE_OFFLINE);
	if (os == TICK_SCHED_REMOTE_RUNNING)
		queue_delayed_work(system_unbound_wq, dwork, HZ);
}

static void sched_tick_start(int cpu)
{
	struct tick_work *twork;
	int os;

	if (housekeeping_cpu(cpu, HK_FLAG_TICK))
		return;

	WARN_ON_ONCE(!tick_work_cpu);

	twork = per_cpu_ptr(tick_work_cpu, cpu);
	os = atomic_xchg(&twork->state, TICK_SCHED_REMOTE_RUNNING);
	WARN_ON_ONCE(os == TICK_SCHED_REMOTE_RUNNING);
	if (os == TICK_SCHED_REMOTE_OFFLINE) {
		twork->cpu = cpu;
		INIT_DELAYED_WORK(&twork->work, sched_tick_remote);
		queue_delayed_work(system_unbound_wq, &twork->work, HZ);
	}
}

#ifdef CONFIG_HOTPLUG_CPU
static void sched_tick_stop(int cpu)
{
	struct tick_work *twork;
	int os;

	if (housekeeping_cpu(cpu, HK_FLAG_TICK))
		return;

	WARN_ON_ONCE(!tick_work_cpu);

	twork = per_cpu_ptr(tick_work_cpu, cpu);
	/* There cannot be competing actions, but don't rely on stop-machine. */
	os = atomic_xchg(&twork->state, TICK_SCHED_REMOTE_OFFLINING);
	WARN_ON_ONCE(os != TICK_SCHED_REMOTE_RUNNING);
	/* Don't cancel, as this would mess up the state machine. */
}
#endif /* CONFIG_HOTPLUG_CPU */

int __init sched_tick_offload_init(void)
{
	tick_work_cpu = alloc_percpu(struct tick_work);
	BUG_ON(!tick_work_cpu);
	return 0;
}

#else /* !CONFIG_NO_HZ_FULL */
static inline void sched_stop_tick(struct rq *rq, int cpu) {}
static inline void sched_start_tick(struct rq *rq, int cpu) {}
static inline void sched_tick_start(int cpu) { }
static inline void sched_tick_stop(int cpu) { }
#endif

/*
 * This function gets called by the timer code, with HZ frequency.
 * We call it with interrupts disabled.
 */
void scheduler_tick(void)
{
	int cpu __maybe_unused = smp_processor_id();
	struct rq *rq = cpu_rq(cpu);

	arch_scale_freq_tick();
	sched_clock_tick();
	update_clocks(rq);
	update_load_avg(rq, 0);
	update_cpu_clock_tick(rq, rq->curr);
	task_tick(rq);
	rq->last_scheduler_tick = rq->last_jiffy;
	rq->last_tick = rq->clock;
	psi_task_tick(rq);
	perf_event_task_tick();
	sched_stop_tick(rq, cpu);
}

#if defined(CONFIG_PREEMPTION) && (defined(CONFIG_DEBUG_PREEMPT) || \
				defined(CONFIG_TRACE_PREEMPT_TOGGLE))
/*
 * If the value passed in is equal to the current preempt count
 * then we just disabled preemption. Start timing the latency.
 */
static inline void preempt_latency_start(int val)
{
	if (preempt_count() == val) {
		unsigned long ip = get_lock_parent_ip();
#ifdef CONFIG_DEBUG_PREEMPT
		current->preempt_disable_ip = ip;
#endif
		trace_preempt_off(CALLER_ADDR0, ip);
	}
}

void preempt_count_add(int val)
{
#ifdef CONFIG_DEBUG_PREEMPT
	/*
	 * Underflow?
	 */
	if (DEBUG_LOCKS_WARN_ON((preempt_count() < 0)))
		return;
#endif
	__preempt_count_add(val);
#ifdef CONFIG_DEBUG_PREEMPT
	/*
	 * Spinlock count overflowing soon?
	 */
	DEBUG_LOCKS_WARN_ON((preempt_count() & PREEMPT_MASK) >=
				PREEMPT_MASK - 10);
#endif
	preempt_latency_start(val);
}
EXPORT_SYMBOL(preempt_count_add);
NOKPROBE_SYMBOL(preempt_count_add);

/*
 * If the value passed in equals to the current preempt count
 * then we just enabled preemption. Stop timing the latency.
 */
static inline void preempt_latency_stop(int val)
{
	if (preempt_count() == val)
		trace_preempt_on(CALLER_ADDR0, get_lock_parent_ip());
}

void preempt_count_sub(int val)
{
#ifdef CONFIG_DEBUG_PREEMPT
	/*
	 * Underflow?
	 */
	if (DEBUG_LOCKS_WARN_ON(val > preempt_count()))
		return;
	/*
	 * Is the spinlock portion underflowing?
	 */
	if (DEBUG_LOCKS_WARN_ON((val < PREEMPT_MASK) &&
			!(preempt_count() & PREEMPT_MASK)))
		return;
#endif

	preempt_latency_stop(val);
	__preempt_count_sub(val);
}
EXPORT_SYMBOL(preempt_count_sub);
NOKPROBE_SYMBOL(preempt_count_sub);

#else
static inline void preempt_latency_start(int val) { }
static inline void preempt_latency_stop(int val) { }
#endif

static inline unsigned long get_preempt_disable_ip(struct task_struct *p)
{
#ifdef CONFIG_DEBUG_PREEMPT
	return p->preempt_disable_ip;
#else
	return 0;
#endif
}

/*
 * The time_slice is only refilled when it is empty and that is when we set a
 * new deadline. Make sure update_clocks has been called recently to update
 * rq->niffies.
 */
static void time_slice_expired(struct task_struct *p, struct rq *rq)
{
	p->time_slice = timeslice();
	p->deadline = rq->niffies + task_deadline_diff(p);
#ifdef CONFIG_SMT_NICE
	if (!p->mm)
		p->smt_bias = 0;
	else if (rt_task(p))
		p->smt_bias = 1 << 30;
	else if (task_running_iso(p))
		p->smt_bias = 1 << 29;
	else if (idleprio_task(p)) {
		if (task_running_idle(p))
			p->smt_bias = 0;
		else
			p->smt_bias = 1;
	} else if (--p->smt_bias < 1)
		p->smt_bias = MAX_PRIO - p->static_prio;
#endif
}

/*
 * Timeslices below RESCHED_US are considered as good as expired as there's no
 * point rescheduling when there's so little time left. SCHED_BATCH tasks
 * have been flagged be not latency sensitive and likely to be fully CPU
 * bound so every time they're rescheduled they have their time_slice
 * refilled, but get a new later deadline to have little effect on
 * SCHED_NORMAL tasks.

 */
static inline void check_deadline(struct task_struct *p, struct rq *rq)
{
	if (p->time_slice < RESCHED_US || batch_task(p))
		time_slice_expired(p, rq);
}

/*
 * Task selection with skiplists is a simple matter of picking off the first
 * task in the sorted list, an O(1) operation. The lookup is amortised O(1)
 * being bound to the number of processors.
 *
 * Runqueues are selectively locked based on their unlocked data and then
 * unlocked if not needed. At most 3 locks will be held at any time and are
 * released as soon as they're no longer needed. All balancing between CPUs
 * is thus done here in an extremely simple first come best fit manner.
 *
 * This iterates over runqueues in cache locality order. In interactive mode
 * it iterates over all CPUs and finds the task with the best key/deadline.
 * In non-interactive mode it will only take a task if it's from the current
 * runqueue or a runqueue with more tasks than the current one with a better
 * key/deadline.
 */
#ifdef CONFIG_SMP
static inline struct task_struct
*earliest_deadline_task(struct rq *rq, int cpu, struct task_struct *idle)
{
	struct rq *locked = NULL, *chosen = NULL;
	struct task_struct *edt = idle;
	int i, best_entries = 0;
	u64 best_key = ~0ULL;

	for (i = 0; i < total_runqueues; i++) {
		struct rq *other_rq = rq_order(rq, i);
		skiplist_node *next;
		int entries;

		entries = other_rq->sl->entries;
		/*
		 * Check for queued entres lockless first. The local runqueue
		 * is locked so entries will always be accurate.
		 */
		if (!sched_interactive) {
			/*
			 * Don't reschedule balance across nodes unless the CPU
			 * is idle.
			 */
			if (edt != idle && rq->cpu_locality[other_rq->cpu] > LOCALITY_SMP)
				break;
			if (entries <= best_entries)
				continue;
		} else if (!entries)
			continue;

		/* if (i) implies other_rq != rq */
		if (i) {
			/* Check for best id queued lockless first */
			if (other_rq->best_key >= best_key)
				continue;

			if (unlikely(!trylock_rq(rq, other_rq)))
				continue;

			/* Need to reevaluate entries after locking */
			entries = other_rq->sl->entries;
			if (unlikely(!entries)) {
				unlock_rq(other_rq);
				continue;
			}
		}

		next = other_rq->node;
		/*
		 * In interactive mode we check beyond the best entry on other
		 * runqueues if we can't get the best for smt or affinity
		 * reasons.
		 */
		while ((next = next->next[0]) != other_rq->node) {
			struct task_struct *p;
			u64 key = next->key;

			/* Reevaluate key after locking */
			if (key >= best_key)
				break;

			p = next->value;
			if (!smt_schedule(p, rq)) {
				if (i && !sched_interactive)
					break;
				continue;
			}

			if (sched_other_cpu(p, cpu)) {
				if (sched_interactive || !i)
					continue;
				break;
			}
			/* Make sure affinity is ok */
			if (i) {
				/* From this point on p is the best so far */
				if (locked)
					unlock_rq(locked);
				chosen = locked = other_rq;
			}
			best_entries = entries;
			best_key = key;
			edt = p;
			break;
		}
		/* rq->preempting is a hint only as the state may have changed
		 * since it was set with the resched call but if we have met
		 * the condition we can break out here. */
		if (edt == rq->preempting)
			break;
		if (i && other_rq != chosen)
			unlock_rq(other_rq);
	}

	if (likely(edt != idle))
		take_task(rq, cpu, edt);

	if (locked)
		unlock_rq(locked);

	rq->preempting = NULL;

	return edt;
}
#else /* CONFIG_SMP */
static inline struct task_struct
*earliest_deadline_task(struct rq *rq, int cpu, struct task_struct *idle)
{
	struct task_struct *edt;

	if (unlikely(!rq->sl->entries))
		return idle;
	edt = rq->node->next[0]->value;
	take_task(rq, cpu, edt);
	return edt;
}
#endif /* CONFIG_SMP */

/*
 * Print scheduling while atomic bug:
 */
static noinline void __schedule_bug(struct task_struct *prev)
{
	/* Save this before calling printk(), since that will clobber it */
	unsigned long preempt_disable_ip = get_preempt_disable_ip(current);

	if (oops_in_progress)
		return;

	printk(KERN_ERR "BUG: scheduling while atomic: %s/%d/0x%08x\n",
		prev->comm, prev->pid, preempt_count());

	debug_show_held_locks(prev);
	print_modules();
	if (irqs_disabled())
		print_irqtrace_events(prev);
	if (IS_ENABLED(CONFIG_DEBUG_PREEMPT)
	    && in_atomic_preempt_off()) {
		pr_err("Preemption disabled at:");
		print_ip_sym(KERN_ERR, preempt_disable_ip);
	}
	dump_stack();
	add_taint(TAINT_WARN, LOCKDEP_STILL_OK);
}

/*
 * Various schedule()-time debugging checks and statistics:
 */
static inline void schedule_debug(struct task_struct *prev, bool preempt)
{
#ifdef CONFIG_SCHED_STACK_END_CHECK
	if (task_stack_end_corrupted(prev))
		panic("corrupted stack end detected inside scheduler\n");

	if (task_scs_end_corrupted(prev))
		panic("corrupted shadow stack detected inside scheduler\n");
#endif

#ifdef CONFIG_DEBUG_ATOMIC_SLEEP
	if (!preempt && prev->state && prev->non_block_count) {
		printk(KERN_ERR "BUG: scheduling in a non-blocking section: %s/%d/%i\n",
			prev->comm, prev->pid, prev->non_block_count);
		dump_stack();
		add_taint(TAINT_WARN, LOCKDEP_STILL_OK);
	}
#endif

	if (unlikely(in_atomic_preempt_off())) {
		__schedule_bug(prev);
		preempt_count_set(PREEMPT_DISABLED);
	}
	rcu_sleep_check();

	profile_hit(SCHED_PROFILING, __builtin_return_address(0));

	schedstat_inc(this_rq()->sched_count);
}

/*
 * The currently running task's information is all stored in rq local data
 * which is only modified by the local CPU.
 */
static inline void set_rq_task(struct rq *rq, struct task_struct *p)
{
	if (p == rq->idle || p->policy == SCHED_FIFO)
		hrexpiry_clear(rq);
	else
		hrexpiry_start(rq, US_TO_NS(p->time_slice));
	if (rq->clock - rq->last_tick > HALF_JIFFY_NS)
		rq->dither = 0;
	else
		rq->dither = rq_dither(rq);

	rq->rq_deadline = p->deadline;
	rq->rq_prio = p->prio;
#ifdef CONFIG_SMT_NICE
	rq->rq_mm = p->mm;
	rq->rq_smt_bias = p->smt_bias;
#endif
}

#ifdef CONFIG_SMT_NICE
static void check_no_siblings(struct rq __maybe_unused *this_rq) {}
static void wake_no_siblings(struct rq __maybe_unused *this_rq) {}
static void (*check_siblings)(struct rq *this_rq) = &check_no_siblings;
static void (*wake_siblings)(struct rq *this_rq) = &wake_no_siblings;

/* Iterate over smt siblings when we've scheduled a process on cpu and decide
 * whether they should continue running or be descheduled. */
static void check_smt_siblings(struct rq *this_rq)
{
	int other_cpu;

	for_each_cpu(other_cpu, &this_rq->thread_mask) {
		struct task_struct *p;
		struct rq *rq;

		rq = cpu_rq(other_cpu);
		if (rq_idle(rq))
			continue;
		p = rq->curr;
		if (!smt_schedule(p, this_rq))
			resched_curr(rq);
	}
}

static void wake_smt_siblings(struct rq *this_rq)
{
	int other_cpu;

	for_each_cpu(other_cpu, &this_rq->thread_mask) {
		struct rq *rq;

		rq = cpu_rq(other_cpu);
		if (rq_idle(rq))
			resched_idle(rq);
	}
}
#else
static void check_siblings(struct rq __maybe_unused *this_rq) {}
static void wake_siblings(struct rq __maybe_unused *this_rq) {}
#endif

/*
 * schedule() is the main scheduler function.
 *
 * The main means of driving the scheduler and thus entering this function are:
 *
 *   1. Explicit blocking: mutex, semaphore, waitqueue, etc.
 *
 *   2. TIF_NEED_RESCHED flag is checked on interrupt and userspace return
 *      paths. For example, see arch/x86/entry_64.S.
 *
 *      To drive preemption between tasks, the scheduler sets the flag in timer
 *      interrupt handler scheduler_tick().
 *
 *   3. Wakeups don't really cause entry into schedule(). They add a
 *      task to the run-queue and that's it.
 *
 *      Now, if the new task added to the run-queue preempts the current
 *      task, then the wakeup sets TIF_NEED_RESCHED and schedule() gets
 *      called on the nearest possible occasion:
 *
 *       - If the kernel is preemptible (CONFIG_PREEMPTION=y):
 *
 *         - in syscall or exception context, at the next outmost
 *           preempt_enable(). (this might be as soon as the wake_up()'s
 *           spin_unlock()!)
 *
 *         - in IRQ context, return from interrupt-handler to
 *           preemptible context
 *
 *       - If the kernel is not preemptible (CONFIG_PREEMPTION is not set)
 *         then at the next:
 *
 *          - cond_resched() call
 *          - explicit schedule() call
 *          - return from syscall or exception to user-space
 *          - return from interrupt-handler to user-space
 *
 * WARNING: must be called with preemption disabled!
 */
static void __sched notrace __schedule(bool preempt)
{
	struct task_struct *prev, *next, *idle;
	unsigned long *switch_count;
	unsigned long prev_state;
	bool deactivate = false;
	struct rq *rq;
	u64 niffies;
	int cpu;

	cpu = smp_processor_id();
	rq = cpu_rq(cpu);
	prev = rq->curr;
	idle = rq->idle;

	schedule_debug(prev, preempt);

	local_irq_disable();
	rcu_note_context_switch(preempt);

	/*
	 * Make sure that signal_pending_state()->signal_pending() below
	 * can't be reordered with __set_current_state(TASK_INTERRUPTIBLE)
	 * done by the caller to avoid the race with signal_wake_up():
	 *
	 * __set_current_state(@state)		signal_wake_up()
	 * schedule()				  set_tsk_thread_flag(p, TIF_SIGPENDING)
	 *					  wake_up_state(p, state)
	 *   LOCK rq->lock			    LOCK p->pi_state
	 *   smp_mb__after_spinlock()		    smp_mb__after_spinlock()
	 *     if (signal_pending_state())	    if (p->state & @state)
	 *
	 * Also, the membarrier system call requires a full memory barrier
	 * after coming from user-space, before storing to rq->curr.
	 */
	rq_lock(rq);
	smp_mb__after_spinlock();
#ifdef CONFIG_SMP
	if (rq->preempt) {
		/*
		 * Make sure resched_curr hasn't triggered a preemption
		 * locklessly on a task that has since scheduled away. Spurious
		 * wakeup of idle is okay though.
		 */
		if (unlikely(preempt && prev != idle && !test_tsk_need_resched(prev))) {
			rq->preempt = NULL;
			clear_preempt_need_resched();
			rq_unlock_irq(rq, NULL);
			return;
		}
		rq->preempt = NULL;
	}
#endif

	switch_count = &prev->nivcsw;

	/*
	 * We must load prev->state once (task_struct::state is volatile), such
	 * that:
	 *
	 *  - we form a control dependency vs deactivate_task() below.
	 *  - ptrace_{,un}freeze_traced() can change ->state underneath us.
	 */
	prev_state = prev->state;
	if (!preempt && prev_state) {
		if (signal_pending_state(prev_state, prev)) {
			prev->state = TASK_RUNNING;
		} else {
			prev->sched_contributes_to_load =
				(prev_state & TASK_UNINTERRUPTIBLE) &&
				!(prev_state & TASK_NOLOAD) &&
				!(prev->flags & PF_FROZEN);

			if (prev->sched_contributes_to_load)
				rq->nr_uninterruptible++;

			/*
			 * __schedule()			ttwu()
			 *   prev_state = prev->state;    if (p->on_rq && ...)
			 *   if (prev_state)		    goto out;
			 *     p->on_rq = 0;		  smp_acquire__after_ctrl_dep();
			 *				  p->state = TASK_WAKING
			 *
			 * Where __schedule() and ttwu() have matching control dependencies.
			 *
			 * After this, schedule() must not care about p->state any more.
			 */
			deactivate = true;

			if (prev->in_iowait) {
				atomic_inc(&rq->nr_iowait);
				delayacct_blkio_start();
			}
		}
		switch_count = &prev->nvcsw;
	}

	/*
	 * Store the niffy value here for use by the next task's last_ran
	 * below to avoid losing niffies due to update_clocks being called
	 * again after this point.
	 */
	update_clocks(rq);
	niffies = rq->niffies;
	update_cpu_clock_switch(rq, prev);

	clear_tsk_need_resched(prev);
	clear_preempt_need_resched();

	if (idle != prev) {
		check_deadline(prev, rq);
		return_task(prev, rq, cpu, deactivate);
	}

	next = earliest_deadline_task(rq, cpu, idle);
	if (likely(next->prio != PRIO_LIMIT))
		clear_cpuidle_map(cpu);
	else {
		set_cpuidle_map(cpu);
		update_load_avg(rq, 0);
	}

	set_rq_task(rq, next);
	next->last_ran = niffies;

	if (likely(prev != next)) {
		/*
		 * Don't reschedule an idle task or deactivated tasks
		 */
		if (prev == idle) {
			inc_nr_running(rq);
			if (rt_task(next))
				rq->rt_nr_running++;
		} else if (!deactivate)
			resched_suitable_idle(prev);
		if (unlikely(next == idle)) {
			dec_nr_running(rq);
			if (rt_task(prev))
				rq->rt_nr_running--;
			wake_siblings(rq);
		} else
			check_siblings(rq);
		rq->nr_switches++;
		/*
		 * RCU users of rcu_dereference(rq->curr) may not see
		 * changes to task_struct made by pick_next_task().
		 */
		RCU_INIT_POINTER(rq->curr, next);
		/*
		 * The membarrier system call requires each architecture
		 * to have a full memory barrier after updating
		 * rq->curr, before returning to user-space.
		 *
		 * Here are the schemes providing that barrier on the
		 * various architectures:
		 * - mm ? switch_mm() : mmdrop() for x86, s390, sparc, PowerPC.
		 *   switch_mm() rely on membarrier_arch_switch_mm() on PowerPC.
		 * - finish_lock_switch() for weakly-ordered
		 *   architectures where spin_unlock is a full barrier,
		 * - switch_to() for arm64 (weakly-ordered, spin_unlock
		 *   is a RELEASE barrier),
		 */
		++*switch_count;

		psi_sched_switch(prev, next, !task_on_rq_queued(prev));

		trace_sched_switch(preempt, prev, next);
		context_switch(rq, prev, next); /* unlocks the rq */
	} else {
		check_siblings(rq);
		rq_unlock(rq);
		local_irq_enable();
	}
}

void __noreturn do_task_dead(void)
{
	/* Causes final put_task_struct in finish_task_switch(). */
	set_special_state(TASK_DEAD);

	/* Tell freezer to ignore us: */
	current->flags |= PF_NOFREEZE;
	__schedule(false);
	BUG();

	/* Avoid "noreturn function does return" - but don't continue if BUG() is a NOP: */
	for (;;)
		cpu_relax();
}

static inline void sched_submit_work(struct task_struct *tsk)
{
	unsigned int task_flags;

	if (!tsk->state)
		return;

	task_flags = tsk->flags;
	/*
	 * If a worker went to sleep, notify and ask workqueue whether
	 * it wants to wake up a task to maintain concurrency.
	 * As this function is called inside the schedule() context,
	 * we disable preemption to avoid it calling schedule() again
	 * in the possible wakeup of a kworker and because wq_worker_sleeping()
	 * requires it.
	 */
	if (task_flags & (PF_WQ_WORKER | PF_IO_WORKER)) {
		preempt_disable();
		if (task_flags & PF_WQ_WORKER)
			wq_worker_sleeping(tsk);
		else
			io_wq_worker_sleeping(tsk);
		preempt_enable_no_resched();
	}

	if (tsk_is_pi_blocked(tsk))
		return;

	/*
	 * If we are going to sleep and we have plugged IO queued,
	 * make sure to submit it to avoid deadlocks.
	 */
	if (blk_needs_flush_plug(tsk))
		blk_schedule_flush_plug(tsk);
}

static inline void sched_update_worker(struct task_struct *tsk)
{
	if (tsk->flags & (PF_WQ_WORKER | PF_IO_WORKER)) {
		if (tsk->flags & PF_WQ_WORKER)
			wq_worker_running(tsk);
		else
			io_wq_worker_running(tsk);
	}
}

asmlinkage __visible void __sched schedule(void)
{
	struct task_struct *tsk = current;

	sched_submit_work(tsk);
	do {
		preempt_disable();
		__schedule(false);
		sched_preempt_enable_no_resched();
	} while (need_resched());
	sched_update_worker(tsk);
}

EXPORT_SYMBOL(schedule);

/*
 * synchronize_rcu_tasks() makes sure that no task is stuck in preempted
 * state (have scheduled out non-voluntarily) by making sure that all
 * tasks have either left the run queue or have gone into user space.
 * As idle tasks do not do either, they must not ever be preempted
 * (schedule out non-voluntarily).
 *
 * schedule_idle() is similar to schedule_preempt_disable() except that it
 * never enables preemption because it does not call sched_submit_work().
 */
void __sched schedule_idle(void)
{
	/*
	 * As this skips calling sched_submit_work(), which the idle task does
	 * regardless because that function is a nop when the task is in a
	 * TASK_RUNNING state, make sure this isn't used someplace that the
	 * current task can be in any other state. Note, idle is always in the
	 * TASK_RUNNING state.
	 */
	WARN_ON_ONCE(current->state);
	do {
		__schedule(false);
	} while (need_resched());
}

#ifdef CONFIG_CONTEXT_TRACKING
asmlinkage __visible void __sched schedule_user(void)
{
	/*
	 * If we come here after a random call to set_need_resched(),
	 * or we have been woken up remotely but the IPI has not yet arrived,
	 * we haven't yet exited the RCU idle mode. Do it here manually until
	 * we find a better solution.
	 *
	 * NB: There are buggy callers of this function.  Ideally we
	 * should warn if prev_state != IN_USER, but that will trigger
	 * too frequently to make sense yet.
	 */
	enum ctx_state prev_state = exception_enter();
	schedule();
	exception_exit(prev_state);
}
#endif

/**
 * schedule_preempt_disabled - called with preemption disabled
 *
 * Returns with preemption disabled. Note: preempt_count must be 1
 */
void __sched schedule_preempt_disabled(void)
{
	sched_preempt_enable_no_resched();
	schedule();
	preempt_disable();
}

static void __sched notrace preempt_schedule_common(void)
{
	do {
		/*
		 * Because the function tracer can trace preempt_count_sub()
		 * and it also uses preempt_enable/disable_notrace(), if
		 * NEED_RESCHED is set, the preempt_enable_notrace() called
		 * by the function tracer will call this function again and
		 * cause infinite recursion.
		 *
		 * Preemption must be disabled here before the function
		 * tracer can trace. Break up preempt_disable() into two
		 * calls. One to disable preemption without fear of being
		 * traced. The other to still record the preemption latency,
		 * which can also be traced by the function tracer.
		 */
		preempt_disable_notrace();
		preempt_latency_start(1);
		__schedule(true);
		preempt_latency_stop(1);
		preempt_enable_no_resched_notrace();

		/*
		 * Check again in case we missed a preemption opportunity
		 * between schedule and now.
		 */
	} while (need_resched());
}

#ifdef CONFIG_PREEMPTION
/*
 * This is the entry point to schedule() from in-kernel preemption
 * off of preempt_enable.
 */
asmlinkage __visible void __sched notrace preempt_schedule(void)
{
	/*
	 * If there is a non-zero preempt_count or interrupts are disabled,
	 * we do not want to preempt the current task. Just return..
	 */
	if (likely(!preemptible()))
		return;

	preempt_schedule_common();
}
NOKPROBE_SYMBOL(preempt_schedule);
EXPORT_SYMBOL(preempt_schedule);

/**
 * preempt_schedule_notrace - preempt_schedule called by tracing
 *
 * The tracing infrastructure uses preempt_enable_notrace to prevent
 * recursion and tracing preempt enabling caused by the tracing
 * infrastructure itself. But as tracing can happen in areas coming
 * from userspace or just about to enter userspace, a preempt enable
 * can occur before user_exit() is called. This will cause the scheduler
 * to be called when the system is still in usermode.
 *
 * To prevent this, the preempt_enable_notrace will use this function
 * instead of preempt_schedule() to exit user context if needed before
 * calling the scheduler.
 */
asmlinkage __visible void __sched notrace preempt_schedule_notrace(void)
{
	enum ctx_state prev_ctx;

	if (likely(!preemptible()))
		return;

	do {
		/*
		 * Because the function tracer can trace preempt_count_sub()
		 * and it also uses preempt_enable/disable_notrace(), if
		 * NEED_RESCHED is set, the preempt_enable_notrace() called
		 * by the function tracer will call this function again and
		 * cause infinite recursion.
		 *
		 * Preemption must be disabled here before the function
		 * tracer can trace. Break up preempt_disable() into two
		 * calls. One to disable preemption without fear of being
		 * traced. The other to still record the preemption latency,
		 * which can also be traced by the function tracer.
		 */
		preempt_disable_notrace();
		preempt_latency_start(1);
		/*
		 * Needs preempt disabled in case user_exit() is traced
		 * and the tracer calls preempt_enable_notrace() causing
		 * an infinite recursion.
		 */
		prev_ctx = exception_enter();
		__schedule(true);
		exception_exit(prev_ctx);

		preempt_latency_stop(1);
		preempt_enable_no_resched_notrace();
	} while (need_resched());
}
EXPORT_SYMBOL_GPL(preempt_schedule_notrace);

#endif /* CONFIG_PREEMPTION */

/*
 * This is the entry point to schedule() from kernel preemption
 * off of irq context.
 * Note, that this is called and return with irqs disabled. This will
 * protect us against recursive calling from irq.
 */
asmlinkage __visible void __sched preempt_schedule_irq(void)
{
	enum ctx_state prev_state;

	/* Catch callers which need to be fixed */
	BUG_ON(preempt_count() || !irqs_disabled());

	prev_state = exception_enter();

	do {
		preempt_disable();
		local_irq_enable();
		__schedule(true);
		local_irq_disable();
		sched_preempt_enable_no_resched();
	} while (need_resched());

	exception_exit(prev_state);
}

int default_wake_function(wait_queue_entry_t *curr, unsigned mode, int wake_flags,
			  void *key)
{
	WARN_ON_ONCE(IS_ENABLED(CONFIG_SCHED_DEBUG) && wake_flags & ~WF_SYNC);
	return try_to_wake_up(curr->private, mode, wake_flags);
}
EXPORT_SYMBOL(default_wake_function);

#ifdef CONFIG_RT_MUTEXES

static inline int __rt_effective_prio(struct task_struct *pi_task, int prio)
{
	if (pi_task)
		prio = min(prio, pi_task->prio);

	return prio;
}

static inline int rt_effective_prio(struct task_struct *p, int prio)
{
	struct task_struct *pi_task = rt_mutex_get_top_task(p);

	return __rt_effective_prio(pi_task, prio);
}

/*
 * rt_mutex_setprio - set the current priority of a task
 * @p: task to boost
 * @pi_task: donor task
 *
 * This function changes the 'effective' priority of a task. It does
 * not touch ->normal_prio like __setscheduler().
 *
 * Used by the rt_mutex code to implement priority inheritance
 * logic. Call site only calls if the priority of the task changed.
 */
void rt_mutex_setprio(struct task_struct *p, struct task_struct *pi_task)
{
	int prio, oldprio;
	struct rq *rq;

	/* XXX used to be waiter->prio, not waiter->task->prio */
	prio = __rt_effective_prio(pi_task, p->normal_prio);

	/*
	 * If nothing changed; bail early.
	 */
	if (p->pi_top_task == pi_task && prio == p->prio)
		return;

	rq = __task_rq_lock(p, NULL);
	update_rq_clock(rq);
	/*
	 * Set under pi_lock && rq->lock, such that the value can be used under
	 * either lock.
	 *
	 * Note that there is loads of tricky to make this pointer cache work
	 * right. rt_mutex_slowunlock()+rt_mutex_postunlock() work together to
	 * ensure a task is de-boosted (pi_task is set to NULL) before the
	 * task is allowed to run again (and can exit). This ensures the pointer
	 * points to a blocked task -- which guaratees the task is present.
	 */
	p->pi_top_task = pi_task;

	/*
	 * For FIFO/RR we only need to set prio, if that matches we're done.
	 */
	if (prio == p->prio)
		goto out_unlock;

	/*
	 * Idle task boosting is a nono in general. There is one
	 * exception, when PREEMPT_RT and NOHZ is active:
	 *
	 * The idle task calls get_next_timer_interrupt() and holds
	 * the timer wheel base->lock on the CPU and another CPU wants
	 * to access the timer (probably to cancel it). We can safely
	 * ignore the boosting request, as the idle CPU runs this code
	 * with interrupts disabled and will complete the lock
	 * protected section without being interrupted. So there is no
	 * real need to boost.
	 */
	if (unlikely(p == rq->idle)) {
		WARN_ON(p != rq->curr);
		WARN_ON(p->pi_blocked_on);
		goto out_unlock;
	}

	trace_sched_pi_setprio(p, pi_task);
	oldprio = p->prio;
	p->prio = prio;
	if (task_running(rq, p)){
		if (prio > oldprio)
			resched_task(p);
	} else if (task_queued(p)) {
		dequeue_task(rq, p, DEQUEUE_SAVE);
		enqueue_task(rq, p, ENQUEUE_RESTORE);
		if (prio < oldprio)
			try_preempt(p, rq);
	}
out_unlock:
	/* Avoid rq from going away on us: */
	preempt_disable();
	__task_rq_unlock(rq, NULL);
	preempt_enable();
}
#else
static inline int rt_effective_prio(struct task_struct *p, int prio)
{
	return prio;
}
#endif

/*
 * Adjust the deadline for when the priority is to change, before it's
 * changed.
 */
static inline void adjust_deadline(struct task_struct *p, int new_prio)
{
	p->deadline += static_deadline_diff(new_prio) - task_deadline_diff(p);
}

void set_user_nice(struct task_struct *p, long nice)
{
	int new_static, old_static;
	struct rq_flags rf;
	struct rq *rq;

	if (task_nice(p) == nice || nice < MIN_NICE || nice > MAX_NICE)
		return;
	new_static = NICE_TO_PRIO(nice);
	/*
	 * We have to be careful, if called from sys_setpriority(),
	 * the task might be in the middle of scheduling on another CPU.
	 */
	rq = task_rq_lock(p, &rf);
	update_rq_clock(rq);

	/*
	 * The RT priorities are set via sched_setscheduler(), but we still
	 * allow the 'normal' nice value to be set - but as expected
	 * it wont have any effect on scheduling until the task is
	 * not SCHED_NORMAL/SCHED_BATCH:
	 */
	if (has_rt_policy(p)) {
		p->static_prio = new_static;
		goto out_unlock;
	}

	adjust_deadline(p, new_static);
	old_static = p->static_prio;
	p->static_prio = new_static;
	p->prio = effective_prio(p);

	if (task_queued(p)) {
		dequeue_task(rq, p, DEQUEUE_SAVE);
		enqueue_task(rq, p, ENQUEUE_RESTORE);
		if (new_static < old_static)
			try_preempt(p, rq);
	} else if (task_running(rq, p)) {
		set_rq_task(rq, p);
		if (old_static < new_static)
			resched_task(p);
	}
out_unlock:
	task_rq_unlock(rq, p, &rf);
}
EXPORT_SYMBOL(set_user_nice);

/*
 * can_nice - check if a task can reduce its nice value
 * @p: task
 * @nice: nice value
 */
int can_nice(const struct task_struct *p, const int nice)
{
	/* Convert nice value [19,-20] to rlimit style value [1,40] */
	int nice_rlim = nice_to_rlimit(nice);

	return (nice_rlim <= task_rlimit(p, RLIMIT_NICE) ||
		capable(CAP_SYS_NICE));
}

#ifdef __ARCH_WANT_SYS_NICE

/*
 * sys_nice - change the priority of the current process.
 * @increment: priority increment
 *
 * sys_setpriority is a more generic, but much slower function that
 * does similar things.
 */
SYSCALL_DEFINE1(nice, int, increment)
{
	long nice, retval;

	/*
	 * Setpriority might change our priority at the same moment.
	 * We don't have to worry. Conceptually one call occurs first
	 * and we have a single winner.
	 */

	increment = clamp(increment, -NICE_WIDTH, NICE_WIDTH);
	nice = task_nice(current) + increment;

	nice = clamp_val(nice, MIN_NICE, MAX_NICE);
	if (increment < 0 && !can_nice(current, nice))
		return -EPERM;

	retval = security_task_setnice(current, nice);
	if (retval)
		return retval;

	set_user_nice(current, nice);
	return 0;
}

#endif

/**
 * task_prio - return the priority value of a given task.
 * @p: the task in question.
 *
 * Return: The priority value as seen by users in /proc.
 * RT tasks are offset by -100. Normal tasks are centered around 1, value goes
 * from 0 (SCHED_ISO) up to 82 (nice +19 SCHED_IDLEPRIO).
 */
int task_prio(const struct task_struct *p)
{
	int delta, prio = p->prio - MAX_RT_PRIO;

	/* rt tasks and iso tasks */
	if (prio <= 0)
		goto out;

	/* Convert to ms to avoid overflows */
	delta = NS_TO_MS(p->deadline - task_rq(p)->niffies);
	if (unlikely(delta < 0))
		delta = 0;
	delta = delta * 40 / ms_longest_deadline_diff();
	if (delta <= 80)
		prio += delta;
	if (idleprio_task(p))
		prio += 40;
out:
	return prio;
}

/**
 * idle_cpu - is a given CPU idle currently?
 * @cpu: the processor in question.
 *
 * Return: 1 if the CPU is currently idle. 0 otherwise.
 */
int idle_cpu(int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	if (rq->curr != rq->idle)
		return 0;

	if (rq->nr_running)
		return 0;

#ifdef CONFIG_SMP
	if (rq->ttwu_pending)
		return 0;
#endif

	return 1;
}

/**
 * available_idle_cpu - is a given CPU idle for enqueuing work.
 * @cpu: the CPU in question.
 *
 * Return: 1 if the CPU is currently idle. 0 otherwise.
 */
int available_idle_cpu(int cpu)
{
	if (!idle_cpu(cpu))
		return 0;

	if (vcpu_is_preempted(cpu))
		return 0;

	return 1;
}

/**
 * idle_task - return the idle task for a given CPU.
 * @cpu: the processor in question.
 *
 * Return: The idle task for the CPU @cpu.
 */
struct task_struct *idle_task(int cpu)
{
	return cpu_rq(cpu)->idle;
}

/**
 * find_process_by_pid - find a process with a matching PID value.
 * @pid: the pid in question.
 *
 * The task of @pid, if found. %NULL otherwise.
 */
static inline struct task_struct *find_process_by_pid(pid_t pid)
{
	return pid ? find_task_by_vpid(pid) : current;
}

/* Actually do priority change: must hold rq lock. */
static void __setscheduler(struct task_struct *p, struct rq *rq, int policy,
			   int prio, const struct sched_attr *attr,
			   bool keep_boost)
{
	int oldrtprio, oldprio;

	/*
	 * If params can't change scheduling class changes aren't allowed
	 * either.
	 */
	if (attr->sched_flags & SCHED_FLAG_KEEP_PARAMS)
		return;

	p->policy = policy;
	oldrtprio = p->rt_priority;
	p->rt_priority = prio;
	p->normal_prio = normal_prio(p);
	oldprio = p->prio;
	/*
	 * Keep a potential priority boosting if called from
	 * sched_setscheduler().
	 */
	p->prio = normal_prio(p);
	if (keep_boost)
		p->prio = rt_effective_prio(p, p->prio);

	if (task_running(rq, p)) {
		set_rq_task(rq, p);
		resched_task(p);
	} else if (task_queued(p)) {
		dequeue_task(rq, p, DEQUEUE_SAVE);
		enqueue_task(rq, p, ENQUEUE_RESTORE);
		if (p->prio < oldprio || p->rt_priority > oldrtprio)
			try_preempt(p, rq);
	}
}

/*
 * Check the target process has a UID that matches the current process's
 */
static bool check_same_owner(struct task_struct *p)
{
	const struct cred *cred = current_cred(), *pcred;
	bool match;

	rcu_read_lock();
	pcred = __task_cred(p);
	match = (uid_eq(cred->euid, pcred->euid) ||
		 uid_eq(cred->euid, pcred->uid));
	rcu_read_unlock();
	return match;
}

static int __sched_setscheduler(struct task_struct *p,
				const struct sched_attr *attr,
				bool user, bool pi)
{
	int retval, policy = attr->sched_policy, oldpolicy = -1, priority = attr->sched_priority;
	unsigned long rlim_rtprio = 0;
	struct rq_flags rf;
	int reset_on_fork;
	struct rq *rq;

	/* The pi code expects interrupts enabled */
	BUG_ON(pi && in_interrupt());

	if (is_rt_policy(policy) && !capable(CAP_SYS_NICE)) {
		unsigned long lflags;

		if (!lock_task_sighand(p, &lflags))
			return -ESRCH;
		rlim_rtprio = task_rlimit(p, RLIMIT_RTPRIO);
		unlock_task_sighand(p, &lflags);
		if (rlim_rtprio)
			goto recheck;
		/*
		 * If the caller requested an RT policy without having the
		 * necessary rights, we downgrade the policy to SCHED_ISO.
		 * We also set the parameter to zero to pass the checks.
		 */
		policy = SCHED_ISO;
		priority = 0;
	}
recheck:
	/* Double check policy once rq lock held */
	if (policy < 0) {
		reset_on_fork = p->sched_reset_on_fork;
		policy = oldpolicy = p->policy;
	} else {
		reset_on_fork = !!(policy & SCHED_RESET_ON_FORK);
		policy &= ~SCHED_RESET_ON_FORK;

		if (!SCHED_RANGE(policy))
			return -EINVAL;
	}

	if (attr->sched_flags & ~(SCHED_FLAG_ALL | SCHED_FLAG_SUGOV))
		return -EINVAL;

	/*
	 * Valid priorities for SCHED_FIFO and SCHED_RR are
	 * 1..MAX_USER_RT_PRIO-1, valid priority for SCHED_NORMAL and
	 * SCHED_BATCH is 0.
	 */
	if (priority < 0 ||
	    (p->mm && priority > MAX_USER_RT_PRIO - 1) ||
	    (!p->mm && priority > MAX_RT_PRIO - 1))
		return -EINVAL;
	if (is_rt_policy(policy) != (priority != 0))
		return -EINVAL;

	/*
	 * Allow unprivileged RT tasks to decrease priority:
	 */
	if (user && !capable(CAP_SYS_NICE)) {
		if (is_rt_policy(policy)) {
			unsigned long rlim_rtprio =
					task_rlimit(p, RLIMIT_RTPRIO);

			/* Can't set/change the rt policy */
			if (policy != p->policy && !rlim_rtprio)
				return -EPERM;

			/* Can't increase priority */
			if (priority > p->rt_priority &&
			    priority > rlim_rtprio)
				return -EPERM;
		} else {
			switch (p->policy) {
				/*
				 * Can only downgrade policies but not back to
				 * SCHED_NORMAL
				 */
				case SCHED_ISO:
					if (policy == SCHED_ISO)
						goto out;
					if (policy != SCHED_NORMAL)
						return -EPERM;
					break;
				case SCHED_BATCH:
					if (policy == SCHED_BATCH)
						goto out;
					if (policy != SCHED_IDLEPRIO)
						return -EPERM;
					break;
				case SCHED_IDLEPRIO:
					if (policy == SCHED_IDLEPRIO)
						goto out;
					return -EPERM;
				default:
					break;
			}
		}

		/* Can't change other user's priorities */
		if (!check_same_owner(p))
			return -EPERM;

		/* Normal users shall not reset the sched_reset_on_fork flag: */
		if (p->sched_reset_on_fork && !reset_on_fork)
			return -EPERM;
	}

	if (user) {
		retval = security_task_setscheduler(p);
		if (retval)
			return retval;
	}

	if (pi)
		cpuset_read_lock();

	/*
	 * Make sure no PI-waiters arrive (or leave) while we are
	 * changing the priority of the task:
	 *
	 * To be able to change p->policy safely, the runqueue lock must be
	 * held.
	 */
	rq = task_rq_lock(p, &rf);
	update_rq_clock(rq);

	/*
	 * Changing the policy of the stop threads its a very bad idea:
	 */
	if (p == rq->stop) {
		retval = -EINVAL;
		goto unlock;
	}

	/*
	 * If not changing anything there's no need to proceed further:
	 */
	if (unlikely(policy == p->policy && (!is_rt_policy(policy) ||
	    priority == p->rt_priority))) {
		retval = 0;
		goto unlock;
	}

	/* Re-check policy now with rq lock held */
	if (unlikely(oldpolicy != -1 && oldpolicy != p->policy)) {
		policy = oldpolicy = -1;
		task_rq_unlock(rq, p, &rf);
		if (pi)
			cpuset_read_unlock();
		goto recheck;
	}
	p->sched_reset_on_fork = reset_on_fork;

	__setscheduler(p, rq, policy, priority, attr, pi);

	/* Avoid rq from going away on us: */
	preempt_disable();
	task_rq_unlock(rq, p, &rf);

	if (pi) {
		cpuset_read_unlock();
		rt_mutex_adjust_pi(p);
	}
	preempt_enable();
out:
	return 0;

unlock:
	task_rq_unlock(rq, p, &rf);
	if (pi)
		cpuset_read_unlock();
	return retval;
}

static int _sched_setscheduler(struct task_struct *p, int policy,
			       const struct sched_param *param, bool check)
{
	struct sched_attr attr = {
		.sched_policy   = policy,
		.sched_priority = param->sched_priority,
		.sched_nice	= PRIO_TO_NICE(p->static_prio),
	};

	return __sched_setscheduler(p, &attr, check, true);
}
/**
 * sched_setscheduler - change the scheduling policy and/or RT priority of a thread.
 * @p: the task in question.
 * @policy: new policy.
 * @param: structure containing the new RT priority.
 *
 * Use sched_set_fifo(), read its comment.
 *
 * Return: 0 on success. An error code otherwise.
 *
 * NOTE that the task may be already dead.
 */
int sched_setscheduler(struct task_struct *p, int policy,
		       const struct sched_param *param)
{
	return _sched_setscheduler(p, policy, param, true);
}


int sched_setattr(struct task_struct *p, const struct sched_attr *attr)
{
	return __sched_setscheduler(p, attr, true, true);
}

int sched_setattr_nocheck(struct task_struct *p, const struct sched_attr *attr)
{
	return __sched_setscheduler(p, attr, false, true);
}

/**
 * sched_setscheduler_nocheck - change the scheduling policy and/or RT priority of a thread from kernelspace.
 * @p: the task in question.
 * @policy: new policy.
 * @param: structure containing the new RT priority.
 *
 * Just like sched_setscheduler, only don't bother checking if the
 * current context has permission.  For example, this is needed in
 * stop_machine(): we create temporary high priority worker threads,
 * but our caller might not have that capability.
 *
 * Return: 0 on success. An error code otherwise.
 */
int sched_setscheduler_nocheck(struct task_struct *p, int policy,
			       const struct sched_param *param)
{
	return _sched_setscheduler(p, policy, param, false);
}

/*
 * SCHED_FIFO is a broken scheduler model; that is, it is fundamentally
 * incapable of resource management, which is the one thing an OS really should
 * be doing.
 *
 * This is of course the reason it is limited to privileged users only.
 *
 * Worse still; it is fundamentally impossible to compose static priority
 * workloads. You cannot take two correctly working static prio workloads
 * and smash them together and still expect them to work.
 *
 * For this reason 'all' FIFO tasks the kernel creates are basically at:
 *
 *   MAX_RT_PRIO / 2
 *
 * The administrator _MUST_ configure the system, the kernel simply doesn't
 * know enough information to make a sensible choice.
 */
void sched_set_fifo(struct task_struct *p)
{
	struct sched_param sp = { .sched_priority = MAX_RT_PRIO / 2 };
	WARN_ON_ONCE(sched_setscheduler_nocheck(p, SCHED_FIFO, &sp) != 0);
}
EXPORT_SYMBOL_GPL(sched_set_fifo);

/*
 * For when you don't much care about FIFO, but want to be above SCHED_NORMAL.
 */
void sched_set_fifo_low(struct task_struct *p)
{
	struct sched_param sp = { .sched_priority = 1 };
	WARN_ON_ONCE(sched_setscheduler_nocheck(p, SCHED_FIFO, &sp) != 0);
}
EXPORT_SYMBOL_GPL(sched_set_fifo_low);

void sched_set_normal(struct task_struct *p, int nice)
{
	struct sched_attr attr = {
		.sched_policy = SCHED_NORMAL,
		.sched_nice = nice,
	};
	WARN_ON_ONCE(sched_setattr_nocheck(p, &attr) != 0);
}
EXPORT_SYMBOL_GPL(sched_set_normal);

static int
do_sched_setscheduler(pid_t pid, int policy, struct sched_param __user *param)
{
	struct sched_param lparam;
	struct task_struct *p;
	int retval;

	if (!param || pid < 0)
		return -EINVAL;
	if (copy_from_user(&lparam, param, sizeof(struct sched_param)))
		return -EFAULT;

	rcu_read_lock();
	retval = -ESRCH;
	p = find_process_by_pid(pid);
	if (likely(p))
		get_task_struct(p);
	rcu_read_unlock();

	if (likely(p)) {
		retval = sched_setscheduler(p, policy, &lparam);
		put_task_struct(p);
	}

	return retval;
}

/*
 * Mimics kernel/events/core.c perf_copy_attr().
 */
static int sched_copy_attr(struct sched_attr __user *uattr,
			   struct sched_attr *attr)
{
	u32 size;
	int ret;

	/* Zero the full structure, so that a short copy will be nice: */
	memset(attr, 0, sizeof(*attr));

	ret = get_user(size, &uattr->size);
	if (ret)
		return ret;

	/* ABI compatibility quirk: */
	if (!size)
		size = SCHED_ATTR_SIZE_VER0;

	if (size < SCHED_ATTR_SIZE_VER0 || size > PAGE_SIZE)
		goto err_size;

	ret = copy_struct_from_user(attr, sizeof(*attr), uattr, size);
	if (ret) {
		if (ret == -E2BIG)
			goto err_size;
		return ret;
	}

	/*
	 * XXX: Do we want to be lenient like existing syscalls; or do we want
	 * to be strict and return an error on out-of-bounds values?
	 */
	attr->sched_nice = clamp(attr->sched_nice, -20, 19);

	/* sched/core.c uses zero here but we already know ret is zero */
	return 0;

err_size:
	put_user(sizeof(*attr), &uattr->size);
	return -E2BIG;
}

/*
 * sched_setparam() passes in -1 for its policy, to let the functions
 * it calls know not to change it.
 */
#define SETPARAM_POLICY	-1

/**
 * sys_sched_setscheduler - set/change the scheduler policy and RT priority
 * @pid: the pid in question.
 * @policy: new policy.
 * @param: structure containing the new RT priority.
 *
 * Return: 0 on success. An error code otherwise.
 */
SYSCALL_DEFINE3(sched_setscheduler, pid_t, pid, int, policy, struct sched_param __user *, param)
{
	if (policy < 0)
		return -EINVAL;

	return do_sched_setscheduler(pid, policy, param);
}

/**
 * sys_sched_setparam - set/change the RT priority of a thread
 * @pid: the pid in question.
 * @param: structure containing the new RT priority.
 *
 * Return: 0 on success. An error code otherwise.
 */
SYSCALL_DEFINE2(sched_setparam, pid_t, pid, struct sched_param __user *, param)
{
	return do_sched_setscheduler(pid, SETPARAM_POLICY, param);
}

/**
 * sys_sched_setattr - same as above, but with extended sched_attr
 * @pid: the pid in question.
 * @uattr: structure containing the extended parameters.
 */
SYSCALL_DEFINE3(sched_setattr, pid_t, pid, struct sched_attr __user *, uattr,
			       unsigned int, flags)
{
	struct sched_attr attr;
	struct task_struct *p;
	int retval;

	if (!uattr || pid < 0 || flags)
		return -EINVAL;

	retval = sched_copy_attr(uattr, &attr);
	if (retval)
		return retval;

	if ((int)attr.sched_policy < 0)
		return -EINVAL;
	if (attr.sched_flags & SCHED_FLAG_KEEP_POLICY)
		attr.sched_policy = SETPARAM_POLICY;

	rcu_read_lock();
	retval = -ESRCH;
	p = find_process_by_pid(pid);
	if (likely(p))
		get_task_struct(p);
	rcu_read_unlock();

	if (likely(p)) {
		retval = sched_setattr(p, &attr);
		put_task_struct(p);
	}

	return retval;
}

/**
 * sys_sched_getscheduler - get the policy (scheduling class) of a thread
 * @pid: the pid in question.
 *
 * Return: On success, the policy of the thread. Otherwise, a negative error
 * code.
 */
SYSCALL_DEFINE1(sched_getscheduler, pid_t, pid)
{
	struct task_struct *p;
	int retval = -EINVAL;

	if (pid < 0)
		goto out_nounlock;

	retval = -ESRCH;
	rcu_read_lock();
	p = find_process_by_pid(pid);
	if (p) {
		retval = security_task_getscheduler(p);
		if (!retval)
			retval = p->policy;
	}
	rcu_read_unlock();

out_nounlock:
	return retval;
}

/**
 * sys_sched_getscheduler - get the RT priority of a thread
 * @pid: the pid in question.
 * @param: structure containing the RT priority.
 *
 * Return: On success, 0 and the RT priority is in @param. Otherwise, an error
 * code.
 */
SYSCALL_DEFINE2(sched_getparam, pid_t, pid, struct sched_param __user *, param)
{
	struct sched_param lp = { .sched_priority = 0 };
	struct task_struct *p;
	int retval = -EINVAL;

	if (!param || pid < 0)
		goto out_nounlock;

	rcu_read_lock();
	p = find_process_by_pid(pid);
	retval = -ESRCH;
	if (!p)
		goto out_unlock;

	retval = security_task_getscheduler(p);
	if (retval)
		goto out_unlock;

	if (has_rt_policy(p))
		lp.sched_priority = p->rt_priority;
	rcu_read_unlock();

	/*
	 * This one might sleep, we cannot do it with a spinlock held ...
	 */
	retval = copy_to_user(param, &lp, sizeof(*param)) ? -EFAULT : 0;

out_nounlock:
	return retval;

out_unlock:
	rcu_read_unlock();
	return retval;
}

/*
 * Copy the kernel size attribute structure (which might be larger
 * than what user-space knows about) to user-space.
 *
 * Note that all cases are valid: user-space buffer can be larger or
 * smaller than the kernel-space buffer. The usual case is that both
 * have the same size.
 */
static int
sched_attr_copy_to_user(struct sched_attr __user *uattr,
			struct sched_attr *kattr,
			unsigned int usize)
{
	unsigned int ksize = sizeof(*kattr);

	if (!access_ok(uattr, usize))
		return -EFAULT;

	/*
	 * sched_getattr() ABI forwards and backwards compatibility:
	 *
	 * If usize == ksize then we just copy everything to user-space and all is good.
	 *
	 * If usize < ksize then we only copy as much as user-space has space for,
	 * this keeps ABI compatibility as well. We skip the rest.
	 *
	 * If usize > ksize then user-space is using a newer version of the ABI,
	 * which part the kernel doesn't know about. Just ignore it - tooling can
	 * detect the kernel's knowledge of attributes from the attr->size value
	 * which is set to ksize in this case.
	 */
	kattr->size = min(usize, ksize);

	if (copy_to_user(uattr, kattr, kattr->size))
		return -EFAULT;

	return 0;
}

/**
 * sys_sched_getattr - similar to sched_getparam, but with sched_attr
 * @pid: the pid in question.
 * @uattr: structure containing the extended parameters.
 * @usize: sizeof(attr) for fwd/bwd comp.
 * @flags: for future extension.
 */
SYSCALL_DEFINE4(sched_getattr, pid_t, pid, struct sched_attr __user *, uattr,
		unsigned int, usize, unsigned int, flags)
{
	struct sched_attr kattr = { };
	struct task_struct *p;
	int retval;

	if (!uattr || pid < 0 || usize > PAGE_SIZE ||
	    usize < SCHED_ATTR_SIZE_VER0 || flags)
		return -EINVAL;

	rcu_read_lock();
	p = find_process_by_pid(pid);
	retval = -ESRCH;
	if (!p)
		goto out_unlock;

	retval = security_task_getscheduler(p);
	if (retval)
		goto out_unlock;

	kattr.sched_policy = p->policy;
	if (rt_task(p))
		kattr.sched_priority = p->rt_priority;
	else
		kattr.sched_nice = task_nice(p);

	rcu_read_unlock();

	return sched_attr_copy_to_user(uattr, &kattr, usize);

out_unlock:
	rcu_read_unlock();
	return retval;
}

long sched_setaffinity(pid_t pid, const struct cpumask *in_mask)
{
	cpumask_var_t cpus_allowed, new_mask;
	struct task_struct *p;
	int retval;

	rcu_read_lock();

	p = find_process_by_pid(pid);
	if (!p) {
		rcu_read_unlock();
		return -ESRCH;
	}

	/* Prevent p going away */
	get_task_struct(p);
	rcu_read_unlock();

	if (p->flags & PF_NO_SETAFFINITY) {
		retval = -EINVAL;
		goto out_put_task;
	}
	if (!alloc_cpumask_var(&cpus_allowed, GFP_KERNEL)) {
		retval = -ENOMEM;
		goto out_put_task;
	}
	if (!alloc_cpumask_var(&new_mask, GFP_KERNEL)) {
		retval = -ENOMEM;
		goto out_free_cpus_allowed;
	}
	retval = -EPERM;
	if (!check_same_owner(p)) {
		rcu_read_lock();
		if (!ns_capable(__task_cred(p)->user_ns, CAP_SYS_NICE)) {
			rcu_read_unlock();
			goto out_unlock;
		}
		rcu_read_unlock();
	}

	retval = security_task_setscheduler(p);
	if (retval)
		goto out_unlock;

	cpuset_cpus_allowed(p, cpus_allowed);
	cpumask_and(new_mask, in_mask, cpus_allowed);
again:
	retval = __set_cpus_allowed_ptr(p, new_mask, true);

	if (!retval) {
		cpuset_cpus_allowed(p, cpus_allowed);
		if (!cpumask_subset(new_mask, cpus_allowed)) {
			/*
			 * We must have raced with a concurrent cpuset
			 * update. Just reset the cpus_allowed to the
			 * cpuset's cpus_allowed
			 */
			cpumask_copy(new_mask, cpus_allowed);
			goto again;
		}
	}
out_unlock:
	free_cpumask_var(new_mask);
out_free_cpus_allowed:
	free_cpumask_var(cpus_allowed);
out_put_task:
	put_task_struct(p);
	return retval;
}

static int get_user_cpu_mask(unsigned long __user *user_mask_ptr, unsigned len,
			     cpumask_t *new_mask)
{
	if (len < cpumask_size())
		cpumask_clear(new_mask);
	else if (len > cpumask_size())
		len = cpumask_size();

	return copy_from_user(new_mask, user_mask_ptr, len) ? -EFAULT : 0;
}


/**
 * sys_sched_setaffinity - set the CPU affinity of a process
 * @pid: pid of the process
 * @len: length in bytes of the bitmask pointed to by user_mask_ptr
 * @user_mask_ptr: user-space pointer to the new CPU mask
 *
 * Return: 0 on success. An error code otherwise.
 */
SYSCALL_DEFINE3(sched_setaffinity, pid_t, pid, unsigned int, len,
		unsigned long __user *, user_mask_ptr)
{
	cpumask_var_t new_mask;
	int retval;

	if (!alloc_cpumask_var(&new_mask, GFP_KERNEL))
		return -ENOMEM;

	retval = get_user_cpu_mask(user_mask_ptr, len, new_mask);
	if (retval == 0)
		retval = sched_setaffinity(pid, new_mask);
	free_cpumask_var(new_mask);
	return retval;
}

long sched_getaffinity(pid_t pid, cpumask_t *mask)
{
	struct task_struct *p;
	unsigned long flags;
	int retval;

	get_online_cpus();
	rcu_read_lock();

	retval = -ESRCH;
	p = find_process_by_pid(pid);
	if (!p)
		goto out_unlock;

	retval = security_task_getscheduler(p);
	if (retval)
		goto out_unlock;

	raw_spin_lock_irqsave(&p->pi_lock, flags);
	cpumask_and(mask, &p->cpus_mask, cpu_active_mask);
	raw_spin_unlock_irqrestore(&p->pi_lock, flags);

out_unlock:
	rcu_read_unlock();
	put_online_cpus();

	return retval;
}

/**
 * sys_sched_getaffinity - get the CPU affinity of a process
 * @pid: pid of the process
 * @len: length in bytes of the bitmask pointed to by user_mask_ptr
 * @user_mask_ptr: user-space pointer to hold the current CPU mask
 *
 * Return: 0 on success. An error code otherwise.
 */
SYSCALL_DEFINE3(sched_getaffinity, pid_t, pid, unsigned int, len,
		unsigned long __user *, user_mask_ptr)
{
	int ret;
	cpumask_var_t mask;

	if ((len * BITS_PER_BYTE) < nr_cpu_ids)
		return -EINVAL;
	if (len & (sizeof(unsigned long)-1))
		return -EINVAL;

	if (!alloc_cpumask_var(&mask, GFP_KERNEL))
		return -ENOMEM;

	ret = sched_getaffinity(pid, mask);
	if (ret == 0) {
		unsigned int retlen = min(len, cpumask_size());

		if (copy_to_user(user_mask_ptr, mask, retlen))
			ret = -EFAULT;
		else
			ret = retlen;
	}
	free_cpumask_var(mask);

	return ret;
}

/**
 * sys_sched_yield - yield the current processor to other threads.
 *
 * This function yields the current CPU to other tasks. It does this by
 * scheduling away the current task. If it still has the earliest deadline
 * it will be scheduled again as the next task.
 *
 * Return: 0.
 */
static void do_sched_yield(void)
{
	struct rq *rq;

	if (!sched_yield_type)
		return;

	local_irq_disable();
	rq = this_rq();
	rq_lock(rq);

	if (sched_yield_type > 1)
		time_slice_expired(current, rq);
	schedstat_inc(rq->yld_count);

	preempt_disable();
	rq_unlock_irq(rq, NULL);
	sched_preempt_enable_no_resched();

	schedule();
}

SYSCALL_DEFINE0(sched_yield)
{
	do_sched_yield();
	return 0;
}

#ifndef CONFIG_PREEMPTION
int __sched _cond_resched(void)
{
	if (should_resched(0)) {
		preempt_schedule_common();
		return 1;
	}
	rcu_all_qs();
	return 0;
}
EXPORT_SYMBOL(_cond_resched);
#endif

/*
 * __cond_resched_lock() - if a reschedule is pending, drop the given lock,
 * call schedule, and on return reacquire the lock.
 *
 * This works OK both with and without CONFIG_PREEMPTION.  We do strange low-level
 * operations here to prevent schedule() from being called twice (once via
 * spin_unlock(), once by hand).
 */
int __cond_resched_lock(spinlock_t *lock)
{
	int resched = should_resched(PREEMPT_LOCK_OFFSET);
	int ret = 0;

	lockdep_assert_held(lock);

	if (spin_needbreak(lock) || resched) {
		spin_unlock(lock);
		if (resched)
			preempt_schedule_common();
		else
			cpu_relax();
		ret = 1;
		spin_lock(lock);
	}
	return ret;
}
EXPORT_SYMBOL(__cond_resched_lock);

/**
 * yield - yield the current processor to other threads.
 *
 * Do not ever use this function, there's a 99% chance you're doing it wrong.
 *
 * The scheduler is at all times free to pick the calling task as the most
 * eligible task to run, if removing the yield() call from your code breaks
 * it, its already broken.
 *
 * Typical broken usage is:
 *
 * while (!event)
 *	yield();
 *
 * where one assumes that yield() will let 'the other' process run that will
 * make event true. If the current task is a SCHED_FIFO task that will never
 * happen. Never use yield() as a progress guarantee!!
 *
 * If you want to use yield() to wait for something, use wait_event().
 * If you want to use yield() to be 'nice' for others, use cond_resched().
 * If you still want to use yield(), do not!
 */
void __sched yield(void)
{
	set_current_state(TASK_RUNNING);
	do_sched_yield();
}
EXPORT_SYMBOL(yield);

/**
 * yield_to - yield the current processor to another thread in
 * your thread group, or accelerate that thread toward the
 * processor it's on.
 * @p: target task
 * @preempt: whether task preemption is allowed or not
 *
 * It's the caller's job to ensure that the target task struct
 * can't go away on us before we can do any checks.
 *
 * Return:
 *	true (>0) if we indeed boosted the target task.
 *	false (0) if we failed to boost the target.
 *	-ESRCH if there's no task to yield to.
 */
int __sched yield_to(struct task_struct *p, bool preempt)
{
	struct task_struct *rq_p;
	struct rq *rq, *p_rq;
	unsigned long flags;
	int yielded = 0;

	local_irq_save(flags);
	rq = this_rq();

again:
	p_rq = task_rq(p);
	/*
	 * If we're the only runnable task on the rq and target rq also
	 * has only one task, there's absolutely no point in yielding.
	 */
	if (task_running(p_rq, p) || p->state) {
		yielded = -ESRCH;
		goto out_irq;
	}

	double_rq_lock(rq, p_rq);
	if (unlikely(task_rq(p) != p_rq)) {
		double_rq_unlock(rq, p_rq);
		goto again;
	}

	yielded = 1;
	schedstat_inc(rq->yld_count);
	rq_p = rq->curr;
	if (p->deadline > rq_p->deadline)
		p->deadline = rq_p->deadline;
	p->time_slice += rq_p->time_slice;
	if (p->time_slice > timeslice())
		p->time_slice = timeslice();
	time_slice_expired(rq_p, rq);
	if (preempt && rq != p_rq)
		resched_task(p_rq->curr);
	double_rq_unlock(rq, p_rq);
out_irq:
	local_irq_restore(flags);

	if (yielded > 0)
		schedule();
	return yielded;
}
EXPORT_SYMBOL_GPL(yield_to);

int io_schedule_prepare(void)
{
	int old_iowait = current->in_iowait;

	current->in_iowait = 1;
	blk_schedule_flush_plug(current);

	return old_iowait;
}

void io_schedule_finish(int token)
{
	current->in_iowait = token;
}

/*
 * This task is about to go to sleep on IO.  Increment rq->nr_iowait so
 * that process accounting knows that this is a task in IO wait state.
 *
 * But don't do that if it is a deliberate, throttling IO wait (this task
 * has set its backing_dev_info: the queue against which it should throttle)
 */

long __sched io_schedule_timeout(long timeout)
{
	int token;
	long ret;

	token = io_schedule_prepare();
	ret = schedule_timeout(timeout);
	io_schedule_finish(token);

	return ret;
}
EXPORT_SYMBOL(io_schedule_timeout);

void __sched io_schedule(void)
{
	int token;

	token = io_schedule_prepare();
	schedule();
	io_schedule_finish(token);
}
EXPORT_SYMBOL(io_schedule);

/**
 * sys_sched_get_priority_max - return maximum RT priority.
 * @policy: scheduling class.
 *
 * Return: On success, this syscall returns the maximum
 * rt_priority that can be used by a given scheduling class.
 * On failure, a negative error code is returned.
 */
SYSCALL_DEFINE1(sched_get_priority_max, int, policy)
{
	int ret = -EINVAL;

	switch (policy) {
	case SCHED_FIFO:
	case SCHED_RR:
		ret = MAX_USER_RT_PRIO-1;
		break;
	case SCHED_NORMAL:
	case SCHED_BATCH:
	case SCHED_ISO:
	case SCHED_IDLEPRIO:
		ret = 0;
		break;
	}
	return ret;
}

/**
 * sys_sched_get_priority_min - return minimum RT priority.
 * @policy: scheduling class.
 *
 * Return: On success, this syscall returns the minimum
 * rt_priority that can be used by a given scheduling class.
 * On failure, a negative error code is returned.
 */
SYSCALL_DEFINE1(sched_get_priority_min, int, policy)
{
	int ret = -EINVAL;

	switch (policy) {
	case SCHED_FIFO:
	case SCHED_RR:
		ret = 1;
		break;
	case SCHED_NORMAL:
	case SCHED_BATCH:
	case SCHED_ISO:
	case SCHED_IDLEPRIO:
		ret = 0;
		break;
	}
	return ret;
}

static int sched_rr_get_interval(pid_t pid, struct timespec64 *t)
{
	struct task_struct *p;
	unsigned int time_slice;
	struct rq_flags rf;
	struct rq *rq;
	int retval;

	if (pid < 0)
		return -EINVAL;

	retval = -ESRCH;
	rcu_read_lock();
	p = find_process_by_pid(pid);
	if (!p)
		goto out_unlock;

	retval = security_task_getscheduler(p);
	if (retval)
		goto out_unlock;

	rq = task_rq_lock(p, &rf);
	time_slice = p->policy == SCHED_FIFO ? 0 : MS_TO_NS(task_timeslice(p));
	task_rq_unlock(rq, p, &rf);

	rcu_read_unlock();
	*t = ns_to_timespec64(time_slice);
	return 0;

out_unlock:
	rcu_read_unlock();
	return retval;
}

/**
 * sys_sched_rr_get_interval - return the default timeslice of a process.
 * @pid: pid of the process.
 * @interval: userspace pointer to the timeslice value.
 *
 * this syscall writes the default timeslice value of a given process
 * into the user-space timespec buffer. A value of '0' means infinity.
 *
 * Return: On success, 0 and the timeslice is in @interval. Otherwise,
 * an error code.
 */
SYSCALL_DEFINE2(sched_rr_get_interval, pid_t, pid,
		struct __kernel_timespec __user *, interval)
{
	struct timespec64 t;
	int retval = sched_rr_get_interval(pid, &t);

	if (retval == 0)
		retval = put_timespec64(&t, interval);

	return retval;
}

#ifdef CONFIG_COMPAT_32BIT_TIME
SYSCALL_DEFINE2(sched_rr_get_interval_time32, pid_t, pid,
		struct old_timespec32 __user *, interval)
{
	struct timespec64 t;
	int retval = sched_rr_get_interval(pid, &t);

	if (retval == 0)
		retval = put_old_timespec32(&t, interval);
	return retval;
}
#endif

void sched_show_task(struct task_struct *p)
{
	unsigned long free = 0;
	int ppid;

	if (!try_get_task_stack(p))
		return;

	printk(KERN_INFO "%-15.15s %c", p->comm, task_state_to_char(p));

	if (p->state == TASK_RUNNING)
		printk(KERN_CONT "  running task    ");
#ifdef CONFIG_DEBUG_STACK_USAGE
	free = stack_not_used(p);
#endif
	ppid = 0;
	rcu_read_lock();
	if (pid_alive(p))
		ppid = task_pid_nr(rcu_dereference(p->real_parent));
	rcu_read_unlock();
	pr_cont(" stack:%5lu pid:%5d ppid:%6d flags:0x%08lx\n",
		free, task_pid_nr(p), ppid,
		(unsigned long)task_thread_info(p)->flags);

	print_worker_info(KERN_INFO, p);
	show_stack(p, NULL, KERN_INFO);
	put_task_stack(p);
}
EXPORT_SYMBOL_GPL(sched_show_task);

static inline bool
state_filter_match(unsigned long state_filter, struct task_struct *p)
{
	/* no filter, everything matches */
	if (!state_filter)
		return true;

	/* filter, but doesn't match */
	if (!(p->state & state_filter))
		return false;

	/*
	 * When looking for TASK_UNINTERRUPTIBLE skip TASK_IDLE (allows
	 * TASK_KILLABLE).
	 */
	if (state_filter == TASK_UNINTERRUPTIBLE && p->state == TASK_IDLE)
		return false;

	return true;
}

void show_state_filter(unsigned long state_filter)
{
	struct task_struct *g, *p;

	rcu_read_lock();
	for_each_process_thread(g, p) {
		/*
		 * reset the NMI-timeout, listing all files on a slow
		 * console might take a lot of time:
		 * Also, reset softlockup watchdogs on all CPUs, because
		 * another CPU might be blocked waiting for us to process
		 * an IPI.
		 */
		touch_nmi_watchdog();
		touch_all_softlockup_watchdogs();
		if (state_filter_match(state_filter, p))
			sched_show_task(p);
	}

	rcu_read_unlock();
	/*
	 * Only show locks if all tasks are dumped:
	 */
	if (!state_filter)
		debug_show_all_locks();
}

void dump_cpu_task(int cpu)
{
	pr_info("Task dump for CPU %d:\n", cpu);
	sched_show_task(cpu_curr(cpu));
}

#ifdef CONFIG_SMP
void set_cpus_allowed_common(struct task_struct *p, const struct cpumask *new_mask)
{
	cpumask_copy(&p->cpus_mask, new_mask);
	p->nr_cpus_allowed = cpumask_weight(new_mask);
}

void __do_set_cpus_allowed(struct task_struct *p, const struct cpumask *new_mask)
{
	struct rq *rq = task_rq(p);

	lockdep_assert_held(&p->pi_lock);

	cpumask_copy(&p->cpus_mask, new_mask);

	if (task_queued(p)) {
		/*
		 * Because __kthread_bind() calls this on blocked tasks without
		 * holding rq->lock.
		 */
		lockdep_assert_held(rq->lock);
	}
}

/*
 * Calling do_set_cpus_allowed from outside the scheduler code should not be
 * called on a running or queued task. We should be holding pi_lock.
 */
void do_set_cpus_allowed(struct task_struct *p, const struct cpumask *new_mask)
{
	__do_set_cpus_allowed(p, new_mask);
	if (needs_other_cpu(p, task_cpu(p))) {
		struct rq *rq;

		rq = __task_rq_lock(p, NULL);
		set_task_cpu(p, valid_task_cpu(p));
		resched_task(p);
		__task_rq_unlock(rq, NULL);
	}
}
#endif

/**
 * init_idle - set up an idle thread for a given CPU
 * @idle: task in question
 * @cpu: cpu the idle task belongs to
 *
 * NOTE: this function does not set the idle thread's NEED_RESCHED
 * flag, to make booting more robust.
 */
void init_idle(struct task_struct *idle, int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	unsigned long flags;

	raw_spin_lock_irqsave(&idle->pi_lock, flags);
	raw_spin_lock(rq->lock);
	idle->last_ran = rq->niffies;
	time_slice_expired(idle, rq);
	idle->state = TASK_RUNNING;
	/* Setting prio to illegal value shouldn't matter when never queued */
	idle->prio = PRIO_LIMIT;
	idle->flags |= PF_IDLE;

	scs_task_reset(idle);
	kasan_unpoison_task_stack(idle);

#ifdef CONFIG_SMP
	/*
	 * It's possible that init_idle() gets called multiple times on a task,
	 * in that case do_set_cpus_allowed() will not do the right thing.
	 *
	 * And since this is boot we can forgo the serialisation.
	 */
	set_cpus_allowed_common(idle, cpumask_of(cpu));
#ifdef CONFIG_SMT_NICE
	idle->smt_bias = 0;
#endif
#endif
	set_rq_task(rq, idle);

	/* Silence PROVE_RCU */
	rcu_read_lock();
	set_task_cpu(idle, cpu);
	rcu_read_unlock();

	rq->idle = idle;
	rcu_assign_pointer(rq->curr, idle);
	idle->on_rq = TASK_ON_RQ_QUEUED;
	raw_spin_unlock(rq->lock);
	raw_spin_unlock_irqrestore(&idle->pi_lock, flags);

	/* Set the preempt count _outside_ the spinlocks! */
	init_idle_preempt_count(idle, cpu);

	ftrace_graph_init_idle_task(idle, cpu);
	vtime_init_idle(idle, cpu);
#ifdef CONFIG_SMP
	sprintf(idle->comm, "%s/%d", INIT_TASK_COMM, cpu);
#endif
}

int cpuset_cpumask_can_shrink(const struct cpumask __maybe_unused *cur,
			      const struct cpumask __maybe_unused *trial)
{
	return 1;
}

int task_can_attach(struct task_struct *p,
		    const struct cpumask *cs_cpus_allowed)
{
	int ret = 0;

	/*
	 * Kthreads which disallow setaffinity shouldn't be moved
	 * to a new cpuset; we don't want to change their CPU
	 * affinity and isolating such threads by their set of
	 * allowed nodes is unnecessary.  Thus, cpusets are not
	 * applicable for such threads.  This prevents checking for
	 * success of set_cpus_allowed_ptr() on all attached tasks
	 * before cpus_mask may be changed.
	 */
	if (p->flags & PF_NO_SETAFFINITY)
		ret = -EINVAL;

	return ret;
}

void resched_cpu(int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	struct rq_flags rf;

	rq_lock_irqsave(rq, &rf);
	if (cpu_online(cpu) || cpu == smp_processor_id())
		resched_curr(rq);
	rq_unlock_irqrestore(rq, &rf);
}

#ifdef CONFIG_SMP
#ifdef CONFIG_NO_HZ_COMMON
void select_nohz_load_balancer(int stop_tick)
{
}

void set_cpu_sd_state_idle(void) {}
void nohz_balance_enter_idle(int cpu) {}

/*
 * In the semi idle case, use the nearest busy CPU for migrating timers
 * from an idle CPU.  This is good for power-savings.
 *
 * We don't do similar optimization for completely idle system, as
 * selecting an idle CPU will add more delays to the timers than intended
 * (as that CPU's timer base may not be uptodate wrt jiffies etc).
 */
int get_nohz_timer_target(void)
{
	int i, cpu = smp_processor_id(), default_cpu = -1;
	struct sched_domain *sd;

	if (housekeeping_cpu(cpu, HK_FLAG_TIMER)) {
		if (!idle_cpu(cpu))
			return cpu;
		default_cpu = cpu;
	}

	rcu_read_lock();
	for_each_domain(cpu, sd) {
		for_each_cpu_and(i, sched_domain_span(sd),
			housekeeping_cpumask(HK_FLAG_TIMER)) {
			if (cpu == i)
				continue;

			if (!idle_cpu(i)) {
				cpu = i;
				goto unlock;
			}
		}
	}

	if (default_cpu == -1)
		default_cpu = housekeeping_any_cpu(HK_FLAG_TIMER);
	cpu = default_cpu;
unlock:
	rcu_read_unlock();
	return cpu;
}

/*
 * When add_timer_on() enqueues a timer into the timer wheel of an
 * idle CPU then this timer might expire before the next timer event
 * which is scheduled to wake up that CPU. In case of a completely
 * idle system the next event might even be infinite time into the
 * future. wake_up_idle_cpu() ensures that the CPU is woken up and
 * leaves the inner idle loop so the newly added timer is taken into
 * account when the CPU goes back to idle and evaluates the timer
 * wheel for the next timer event.
 */
void wake_up_idle_cpu(int cpu)
{
	if (cpu == smp_processor_id())
		return;

	if (set_nr_and_not_polling(cpu_rq(cpu)->idle))
		smp_sched_reschedule(cpu);
	else
		trace_sched_wake_idle_without_ipi(cpu);
}

static bool wake_up_full_nohz_cpu(int cpu)
{
	/*
	 * We just need the target to call irq_exit() and re-evaluate
	 * the next tick. The nohz full kick at least implies that.
	 * If needed we can still optimize that later with an
	 * empty IRQ.
	 */
	if (cpu_is_offline(cpu))
		return true;  /* Don't try to wake offline CPUs. */
	if (tick_nohz_full_cpu(cpu)) {
		if (cpu != smp_processor_id() ||
		    tick_nohz_tick_stopped())
			tick_nohz_full_kick_cpu(cpu);
		return true;
	}

	return false;
}

/*
 * Wake up the specified CPU.  If the CPU is going offline, it is the
 * caller's responsibility to deal with the lost wakeup, for example,
 * by hooking into the CPU_DEAD notifier like timers and hrtimers do.
 */
void wake_up_nohz_cpu(int cpu)
{
	if (!wake_up_full_nohz_cpu(cpu))
		wake_up_idle_cpu(cpu);
}
#endif /* CONFIG_NO_HZ_COMMON */

/*
 * Change a given task's CPU affinity. Migrate the thread to a
 * proper CPU and schedule it away if the CPU it's executing on
 * is removed from the allowed bitmask.
 *
 * NOTE: the caller must have a valid reference to the task, the
 * task must not exit() & deallocate itself prematurely. The
 * call is not atomic; no spinlocks may be held.
 */
static int __set_cpus_allowed_ptr(struct task_struct *p,
				  const struct cpumask *new_mask, bool check)
{
	const struct cpumask *cpu_valid_mask = cpu_active_mask;
	bool queued = false, running_wrong = false, kthread;
	unsigned int dest_cpu;
	struct rq_flags rf;
	struct rq *rq;
	int ret = 0;

	rq = task_rq_lock(p, &rf);
	update_rq_clock(rq);

	kthread = !!(p->flags & PF_KTHREAD);
	if (kthread) {
		/*
		 * Kernel threads are allowed on online && !active CPUs
		 */
		cpu_valid_mask = cpu_online_mask;
	}

	/*
	 * Must re-check here, to close a race against __kthread_bind(),
	 * sched_setaffinity() is not guaranteed to observe the flag.
	 */
	if (check && (p->flags & PF_NO_SETAFFINITY)) {
		ret = -EINVAL;
		goto out;
	}

	if (cpumask_equal(&p->cpus_mask, new_mask))
		goto out;

	/*
	 * Picking a ~random cpu helps in cases where we are changing affinity
	 * for groups of tasks (ie. cpuset), so that load balancing is not
	 * immediately required to distribute the tasks within their new mask.
	 */
	dest_cpu = cpumask_any_and_distribute(cpu_valid_mask, new_mask);
	if (dest_cpu >= nr_cpu_ids) {
		ret = -EINVAL;
		goto out;
	}

	queued = task_queued(p);
	__do_set_cpus_allowed(p, new_mask);

	if (kthread) {
		/*
		 * For kernel threads that do indeed end up on online &&
		 * !active we want to ensure they are strict per-CPU threads.
		 */
		WARN_ON(cpumask_intersects(new_mask, cpu_online_mask) &&
			!cpumask_intersects(new_mask, cpu_active_mask) &&
			p->nr_cpus_allowed != 1);
	}

	/* Can the task run on the task's current CPU? If so, we're done */
	if (cpumask_test_cpu(task_cpu(p), new_mask))
		goto out;

	if (task_running(rq, p)) {
		/* Task is running on the wrong cpu now, reschedule it. */
		if (rq == this_rq()) {
			set_task_cpu(p, dest_cpu);
			set_tsk_need_resched(p);
			running_wrong = true;
		} else
			resched_task(p);
	} else {
		if (queued) {
			/*
			 * Switch runqueue locks after dequeueing the task
			 * here while still holding the pi_lock to be holding
			 * the correct lock for enqueueing.
			 */
			dequeue_task(rq, p, 0);
			rq_unlock(rq);

			rq = cpu_rq(dest_cpu);
			rq_lock(rq);
		}
		set_task_cpu(p, dest_cpu);
		if (queued)
			enqueue_task(rq, p, 0);
	}
	if (queued)
		try_preempt(p, rq);
	if (running_wrong)
		preempt_disable();
out:
	task_rq_unlock(rq, p, &rf);

	if (running_wrong) {
		__schedule(true);
		preempt_enable();
	}

	return ret;
}

int set_cpus_allowed_ptr(struct task_struct *p, const struct cpumask *new_mask)
{
	return __set_cpus_allowed_ptr(p, new_mask, false);
}
EXPORT_SYMBOL_GPL(set_cpus_allowed_ptr);

#ifdef CONFIG_HOTPLUG_CPU
/*
 * Run through task list and find tasks affined to the dead cpu, then remove
 * that cpu from the list, enable cpu0 and set the zerobound flag. Must hold
 * cpu 0 and src_cpu's runqueue locks. We should be holding both rq lock and
 * pi_lock to change cpus_mask but it's not going to matter here.
 */
static void bind_zero(int src_cpu)
{
	struct task_struct *p, *t;
	struct rq *rq0;
	int bound = 0;

	if (src_cpu == 0)
		return;

	rq0 = cpu_rq(0);

	do_each_thread(t, p) {
		if (cpumask_test_cpu(src_cpu, p->cpus_ptr)) {
			bool local = (task_cpu(p) == src_cpu);
			struct rq *rq = task_rq(p);

			/* task_running is the cpu stopper thread */
			if (local && task_running(rq, p))
				continue;
			atomic_clear_cpu(src_cpu, &p->cpus_mask);
			atomic_set_cpu(0, &p->cpus_mask);
			p->zerobound = true;
			bound++;
			if (local) {
				bool queued = task_queued(p);

				if (queued)
					dequeue_task(rq, p, 0);
				set_task_cpu(p, 0);
				if (queued)
					enqueue_task(rq0, p, 0);
			}
		}
	} while_each_thread(t, p);

	if (bound) {
		printk(KERN_INFO "MuQSS removed affinity for %d processes to cpu %d\n",
		       bound, src_cpu);
	}
}

/* Find processes with the zerobound flag and reenable their affinity for the
 * CPU coming alive. */
static void unbind_zero(int src_cpu)
{
	int unbound = 0, zerobound = 0;
	struct task_struct *p, *t;

	if (src_cpu == 0)
		return;

	do_each_thread(t, p) {
		if (!p->mm)
			p->zerobound = false;
		if (p->zerobound) {
			unbound++;
			cpumask_set_cpu(src_cpu, &p->cpus_mask);
			/* Once every CPU affinity has been re-enabled, remove
			 * the zerobound flag */
			if (cpumask_subset(cpu_possible_mask, p->cpus_ptr)) {
				p->zerobound = false;
				zerobound++;
			}
		}
	} while_each_thread(t, p);

	if (unbound) {
		printk(KERN_INFO "MuQSS added affinity for %d processes to cpu %d\n",
		       unbound, src_cpu);
	}
	if (zerobound) {
		printk(KERN_INFO "MuQSS released forced binding to cpu0 for %d processes\n",
		       zerobound);
	}
}

/*
 * Ensure that the idle task is using init_mm right before its cpu goes
 * offline.
 */
void idle_task_exit(void)
{
	struct mm_struct *mm = current->active_mm;

	BUG_ON(cpu_online(smp_processor_id()));
	BUG_ON(current != this_rq()->idle);

	if (mm != &init_mm) {
		switch_mm(mm, &init_mm, current);
		finish_arch_post_lock_switch();
	}

	/* finish_cpu(), as ran on the BP, will clean up the active_mm state */
}
#else /* CONFIG_HOTPLUG_CPU */
static void unbind_zero(int src_cpu) {}
#endif /* CONFIG_HOTPLUG_CPU */

void sched_set_stop_task(int cpu, struct task_struct *stop)
{
	struct sched_param stop_param = { .sched_priority = STOP_PRIO };
	struct sched_param start_param = { .sched_priority = 0 };
	struct task_struct *old_stop = cpu_rq(cpu)->stop;

	if (stop) {
		/*
		 * Make it appear like a SCHED_FIFO task, its something
		 * userspace knows about and won't get confused about.
		 *
		 * Also, it will make PI more or less work without too
		 * much confusion -- but then, stop work should not
		 * rely on PI working anyway.
		 */
		sched_setscheduler_nocheck(stop, SCHED_FIFO, &stop_param);
	}

	cpu_rq(cpu)->stop = stop;

	if (old_stop) {
		/*
		 * Reset it back to a normal scheduling policy so that
		 * it can die in pieces.
		 */
		sched_setscheduler_nocheck(old_stop, SCHED_NORMAL, &start_param);
	}
}

#if defined(CONFIG_SCHED_DEBUG) && defined(CONFIG_SYSCTL)

static struct ctl_table sd_ctl_dir[] = {
	{
		.procname	= "sched_domain",
		.mode		= 0555,
	},
	{}
};

static struct ctl_table sd_ctl_root[] = {
	{
		.procname	= "kernel",
		.mode		= 0555,
		.child		= sd_ctl_dir,
	},
	{}
};

static struct ctl_table *sd_alloc_ctl_entry(int n)
{
	struct ctl_table *entry =
		kcalloc(n, sizeof(struct ctl_table), GFP_KERNEL);

	return entry;
}

static void sd_free_ctl_entry(struct ctl_table **tablep)
{
	struct ctl_table *entry;

	/*
	 * In the intermediate directories, both the child directory and
	 * procname are dynamically allocated and could fail but the mode
	 * will always be set. In the lowest directory the names are
	 * static strings and all have proc handlers.
	 */
	for (entry = *tablep; entry->mode; entry++) {
		if (entry->child)
			sd_free_ctl_entry(&entry->child);
		if (entry->proc_handler == NULL)
			kfree(entry->procname);
	}

	kfree(*tablep);
	*tablep = NULL;
}

static void
set_table_entry(struct ctl_table *entry,
		const char *procname, void *data, int maxlen,
		umode_t mode, proc_handler *proc_handler)
{
	entry->procname = procname;
	entry->data = data;
	entry->maxlen = maxlen;
	entry->mode = mode;
	entry->proc_handler = proc_handler;
}

static struct ctl_table *
sd_alloc_ctl_domain_table(struct sched_domain *sd)
{
	struct ctl_table *table = sd_alloc_ctl_entry(9);

	if (table == NULL)
		return NULL;

	set_table_entry(&table[0], "min_interval",	  &sd->min_interval,	    sizeof(long), 0644, proc_doulongvec_minmax);
	set_table_entry(&table[1], "max_interval",	  &sd->max_interval,	    sizeof(long), 0644, proc_doulongvec_minmax);
	set_table_entry(&table[2], "busy_factor",	  &sd->busy_factor,	    sizeof(int),  0644, proc_dointvec_minmax);
	set_table_entry(&table[3], "imbalance_pct",	  &sd->imbalance_pct,	    sizeof(int),  0644, proc_dointvec_minmax);
	set_table_entry(&table[4], "cache_nice_tries",	  &sd->cache_nice_tries,    sizeof(int),  0644, proc_dointvec_minmax);
	set_table_entry(&table[5], "flags",		  &sd->flags,		    sizeof(int),  0644, proc_dointvec_minmax);
	set_table_entry(&table[6], "max_newidle_lb_cost", &sd->max_newidle_lb_cost, sizeof(long), 0644, proc_doulongvec_minmax);
	set_table_entry(&table[7], "name",		  sd->name,	       CORENAME_MAX_SIZE, 0444, proc_dostring);
	/* &table[8] is terminator */

	return table;
}

static struct ctl_table *sd_alloc_ctl_cpu_table(int cpu)
{
	struct ctl_table *entry, *table;
	struct sched_domain *sd;
	int domain_num = 0, i;
	char buf[32];

	for_each_domain(cpu, sd)
		domain_num++;
	entry = table = sd_alloc_ctl_entry(domain_num + 1);
	if (table == NULL)
		return NULL;

	i = 0;
	for_each_domain(cpu, sd) {
		snprintf(buf, 32, "domain%d", i);
		entry->procname = kstrdup(buf, GFP_KERNEL);
		entry->mode = 0555;
		entry->child = sd_alloc_ctl_domain_table(sd);
		entry++;
		i++;
	}
	return table;
}

static cpumask_var_t sd_sysctl_cpus;
static struct ctl_table_header *sd_sysctl_header;

void register_sched_domain_sysctl(void)
{
	static struct ctl_table *cpu_entries;
	static struct ctl_table **cpu_idx;
	char buf[32];
	int i;

	if (!cpu_entries) {
		cpu_entries = sd_alloc_ctl_entry(num_possible_cpus() + 1);
		if (!cpu_entries)
			return;

		WARN_ON(sd_ctl_dir[0].child);
		sd_ctl_dir[0].child = cpu_entries;
	}

	if (!cpu_idx) {
		struct ctl_table *e = cpu_entries;

		cpu_idx = kcalloc(nr_cpu_ids, sizeof(struct ctl_table*), GFP_KERNEL);
		if (!cpu_idx)
			return;

		/* deal with sparse possible map */
		for_each_possible_cpu(i) {
			cpu_idx[i] = e;
			e++;
		}
	}

	if (!cpumask_available(sd_sysctl_cpus)) {
		if (!alloc_cpumask_var(&sd_sysctl_cpus, GFP_KERNEL))
			return;

		/* init to possible to not have holes in @cpu_entries */
		cpumask_copy(sd_sysctl_cpus, cpu_possible_mask);
	}

	for_each_cpu(i, sd_sysctl_cpus) {
		struct ctl_table *e = cpu_idx[i];

		if (e->child)
			sd_free_ctl_entry(&e->child);

		if (!e->procname) {
			snprintf(buf, 32, "cpu%d", i);
			e->procname = kstrdup(buf, GFP_KERNEL);
		}
		e->mode = 0555;
		e->child = sd_alloc_ctl_cpu_table(i);

		__cpumask_clear_cpu(i, sd_sysctl_cpus);
	}

	WARN_ON(sd_sysctl_header);
	sd_sysctl_header = register_sysctl_table(sd_ctl_root);
}

void dirty_sched_domain_sysctl(int cpu)
{
	if (cpumask_available(sd_sysctl_cpus))
		__cpumask_set_cpu(cpu, sd_sysctl_cpus);
}

/* may be called multiple times per register */
void unregister_sched_domain_sysctl(void)
{
	unregister_sysctl_table(sd_sysctl_header);
	sd_sysctl_header = NULL;
}
#endif /* CONFIG_SYSCTL */

void set_rq_online(struct rq *rq)
{
	if (!rq->online) {
		cpumask_set_cpu(cpu_of(rq), rq->rd->online);
		rq->online = true;
	}
}

void set_rq_offline(struct rq *rq)
{
	if (rq->online) {
		int cpu = cpu_of(rq);

		cpumask_clear_cpu(cpu, rq->rd->online);
		rq->online = false;
		clear_cpuidle_map(cpu);
	}
}

/*
 * used to mark begin/end of suspend/resume:
 */
static int num_cpus_frozen;

/*
 * Update cpusets according to cpu_active mask.  If cpusets are
 * disabled, cpuset_update_active_cpus() becomes a simple wrapper
 * around partition_sched_domains().
 *
 * If we come here as part of a suspend/resume, don't touch cpusets because we
 * want to restore it back to its original state upon resume anyway.
 */
static void cpuset_cpu_active(void)
{
	if (cpuhp_tasks_frozen) {
		/*
		 * num_cpus_frozen tracks how many CPUs are involved in suspend
		 * resume sequence. As long as this is not the last online
		 * operation in the resume sequence, just build a single sched
		 * domain, ignoring cpusets.
		 */
		partition_sched_domains(1, NULL, NULL);
		if (--num_cpus_frozen)
			return;
		/*
		 * This is the last CPU online operation. So fall through and
		 * restore the original sched domains by considering the
		 * cpuset configurations.
		 */
		cpuset_force_rebuild();
	}

	cpuset_update_active_cpus();
}

static int cpuset_cpu_inactive(unsigned int cpu)
{
	if (!cpuhp_tasks_frozen) {
		cpuset_update_active_cpus();
	} else {
		num_cpus_frozen++;
		partition_sched_domains(1, NULL, NULL);
	}
	return 0;
}

int sched_cpu_activate(unsigned int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	struct rq_flags rf;

#ifdef CONFIG_SCHED_SMT
	/*
	 * When going up, increment the number of cores with SMT present.
	 */
	if (cpumask_weight(cpu_smt_mask(cpu)) == 2)
		static_branch_inc_cpuslocked(&sched_smt_present);
#endif
	set_cpu_active(cpu, true);

	if (sched_smp_initialized) {
		sched_domains_numa_masks_set(cpu);
		cpuset_cpu_active();
	}

	/*
	 * Put the rq online, if not already. This happens:
	 *
	 * 1) In the early boot process, because we build the real domains
	 *    after all CPUs have been brought up.
	 *
	 * 2) At runtime, if cpuset_cpu_active() fails to rebuild the
	 *    domains.
	 */
	rq_lock_irqsave(rq, &rf);
	if (rq->rd) {
		BUG_ON(!cpumask_test_cpu(cpu, rq->rd->span));
		set_rq_online(rq);
	}
	unbind_zero(cpu);
	rq_unlock_irqrestore(rq, &rf);

	return 0;
}

int sched_cpu_deactivate(unsigned int cpu)
{
	int ret;

	set_cpu_active(cpu, false);
	/*
	 * We've cleared cpu_active_mask, wait for all preempt-disabled and RCU
	 * users of this state to go away such that all new such users will
	 * observe it.
	 *
	 * Do sync before park smpboot threads to take care the rcu boost case.
	 */
	synchronize_rcu();

#ifdef CONFIG_SCHED_SMT
	/*
	 * When going down, decrement the number of cores with SMT present.
	 */
	if (cpumask_weight(cpu_smt_mask(cpu)) == 2)
		static_branch_dec_cpuslocked(&sched_smt_present);
#endif

	if (!sched_smp_initialized)
		return 0;

	ret = cpuset_cpu_inactive(cpu);
	if (ret) {
		set_cpu_active(cpu, true);
		return ret;
	}
	sched_domains_numa_masks_clear(cpu);
	return 0;
}

int sched_cpu_starting(unsigned int cpu)
{
	sched_tick_start(cpu);
	return 0;
}

#ifdef CONFIG_HOTPLUG_CPU
int sched_cpu_dying(unsigned int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	unsigned long flags;

	/* Handle pending wakeups and then migrate everything off */
	sched_tick_stop(cpu);

	local_irq_save(flags);
	double_rq_lock(rq, cpu_rq(0));
	if (rq->rd) {
		BUG_ON(!cpumask_test_cpu(cpu, rq->rd->span));
		set_rq_offline(rq);
	}
	bind_zero(cpu);
	double_rq_unlock(rq, cpu_rq(0));
	sched_start_tick(rq, cpu);
	hrexpiry_clear(rq);
	local_irq_restore(flags);

	return 0;
}
#endif

#if defined(CONFIG_SCHED_SMT) || defined(CONFIG_SCHED_MC)
/*
 * Cheaper version of the below functions in case support for SMT and MC is
 * compiled in but CPUs have no siblings.
 */
static bool sole_cpu_idle(struct rq *rq)
{
	return rq_idle(rq);
}
#endif
#ifdef CONFIG_SCHED_SMT
static const cpumask_t *thread_cpumask(int cpu)
{
	return topology_sibling_cpumask(cpu);
}
/* All this CPU's SMT siblings are idle */
static bool siblings_cpu_idle(struct rq *rq)
{
	return cpumask_subset(&rq->thread_mask, &cpu_idle_map);
}
#endif
#ifdef CONFIG_SCHED_MC
static const cpumask_t *core_cpumask(int cpu)
{
	return topology_core_cpumask(cpu);
}
/* All this CPU's shared cache siblings are idle */
static bool cache_cpu_idle(struct rq *rq)
{
	return cpumask_subset(&rq->core_mask, &cpu_idle_map);
}
/* MC siblings CPU mask which share the same LLC */
static const cpumask_t *llc_core_cpumask(int cpu)
{
#ifdef CONFIG_X86
	return per_cpu(cpu_llc_shared_map, cpu);
#else
	return topology_core_cpumask(cpu);
#endif
}
#endif

enum sched_domain_level {
	SD_LV_NONE = 0,
	SD_LV_SIBLING,
	SD_LV_MC,
	SD_LV_BOOK,
	SD_LV_CPU,
	SD_LV_NODE,
	SD_LV_ALLNODES,
	SD_LV_MAX
};

/*
 * Set up the relative cache distance of each online cpu from each
 * other in a simple array for quick lookup. Locality is determined
 * by the closest sched_domain that CPUs are separated by. CPUs with
 * shared cache in SMT and MC are treated as local. Separate CPUs
 * (within the same package or physically) within the same node are
 * treated as not local. CPUs not even in the same domain (different
 * nodes) are treated as very distant.
 */
static void __init select_leaders(void)
{
	struct rq *rq, *other_rq, *leader;
	struct sched_domain *sd;
	int cpu, other_cpu;
#ifdef CONFIG_SCHED_SMT
	bool smt_threads = false;
#endif

	for (cpu = 0; cpu < num_online_cpus(); cpu++) {
		rq = cpu_rq(cpu);
		leader = NULL;
		/* First check if this cpu is in the same node */
		for_each_domain(cpu, sd) {
			if (sd->level > SD_LV_MC)
				continue;
			if (rqshare != RQSHARE_ALL)
				leader = NULL;
			/* Set locality to local node if not already found lower */
			for_each_cpu(other_cpu, sched_domain_span(sd)) {
				if (rqshare >= RQSHARE_SMP) {
					other_rq = cpu_rq(other_cpu);

					/* Set the smp_leader to the first CPU */
					if (!leader)
						leader = rq;
					if (!other_rq->smp_leader)
						other_rq->smp_leader = leader;
				}
				if (rq->cpu_locality[other_cpu] > LOCALITY_SMP)
					rq->cpu_locality[other_cpu] = LOCALITY_SMP;
			}
		}

		/*
		 * Each runqueue has its own function in case it doesn't have
		 * siblings of its own allowing mixed topologies.
		 */
#ifdef CONFIG_SCHED_MC
		leader = NULL;
		if (cpumask_weight(core_cpumask(cpu)) > 1) {
			cpumask_copy(&rq->core_mask, llc_core_cpumask(cpu));
			cpumask_clear_cpu(cpu, &rq->core_mask);
			for_each_cpu(other_cpu, core_cpumask(cpu)) {
				if (rqshare == RQSHARE_MC ||
					(rqshare == RQSHARE_MC_LLC && cpumask_test_cpu(other_cpu, llc_core_cpumask(cpu)))) {
					other_rq = cpu_rq(other_cpu);

					/* Set the mc_leader to the first CPU */
					if (!leader)
						leader = rq;
					if (!other_rq->mc_leader)
						other_rq->mc_leader = leader;
				}
				if (rq->cpu_locality[other_cpu] > LOCALITY_MC) {
					/* this is to get LLC into play even in case LLC sharing is not used */
					if (cpumask_test_cpu(other_cpu, llc_core_cpumask(cpu)))
						rq->cpu_locality[other_cpu] = LOCALITY_MC_LLC;
					else
						rq->cpu_locality[other_cpu] = LOCALITY_MC;
				}
			}
			rq->cache_idle = cache_cpu_idle;
		}
#endif
#ifdef CONFIG_SCHED_SMT
		leader = NULL;
		if (cpumask_weight(thread_cpumask(cpu)) > 1) {
			cpumask_copy(&rq->thread_mask, thread_cpumask(cpu));
			cpumask_clear_cpu(cpu, &rq->thread_mask);
			for_each_cpu(other_cpu, thread_cpumask(cpu)) {
				if (rqshare == RQSHARE_SMT) {
					other_rq = cpu_rq(other_cpu);

					/* Set the smt_leader to the first CPU */
					if (!leader)
						leader = rq;
					if (!other_rq->smt_leader)
						other_rq->smt_leader = leader;
				}
				if (rq->cpu_locality[other_cpu] > LOCALITY_SMT)
					rq->cpu_locality[other_cpu] = LOCALITY_SMT;
			}
			rq->siblings_idle = siblings_cpu_idle;
			smt_threads = true;
		}
#endif
	}

#ifdef CONFIG_SMT_NICE
	if (smt_threads) {
		check_siblings = &check_smt_siblings;
		wake_siblings = &wake_smt_siblings;
		smt_schedule = &smt_should_schedule;
	}
#endif

	for_each_online_cpu(cpu) {
		rq = cpu_rq(cpu);
		for_each_online_cpu(other_cpu) {
			printk(KERN_DEBUG "MuQSS locality CPU %d to %d: %d\n", cpu, other_cpu, rq->cpu_locality[other_cpu]);
		}
	}
}

/* FIXME freeing locked spinlock */
static void __init share_and_free_rq(struct rq *leader, struct rq *rq)
{
	WARN_ON(rq->nr_running > 0);

	kfree(rq->node);
	kfree(rq->sl);
	kfree(rq->lock);
	rq->node = leader->node;
	rq->sl = leader->sl;
	rq->lock = leader->lock;
	rq->is_leader = false;
	barrier();
	/* To make up for not unlocking the freed runlock */
	preempt_enable();
}

static void __init share_rqs(void)
{
	struct rq *rq, *leader;
	int cpu;

	for_each_online_cpu(cpu) {
		rq = cpu_rq(cpu);
		leader = rq->smp_leader;

		rq_lock(rq);
		if (leader && rq != leader) {
			printk(KERN_INFO "MuQSS sharing SMP runqueue from CPU %d to CPU %d\n",
			       leader->cpu, rq->cpu);
			share_and_free_rq(leader, rq);
		} else
			rq_unlock(rq);
	}

#ifdef CONFIG_SCHED_MC
	for_each_online_cpu(cpu) {
		rq = cpu_rq(cpu);
		leader = rq->mc_leader;

		rq_lock(rq);
		if (leader && rq != leader) {
			printk(KERN_INFO "MuQSS sharing MC runqueue from CPU %d to CPU %d\n",
			       leader->cpu, rq->cpu);
			share_and_free_rq(leader, rq);
		} else
			rq_unlock(rq);
	}
#endif /* CONFIG_SCHED_MC */

#ifdef CONFIG_SCHED_SMT
	for_each_online_cpu(cpu) {
		rq = cpu_rq(cpu);
		leader = rq->smt_leader;

		rq_lock(rq);
		if (leader && rq != leader) {
			printk(KERN_INFO "MuQSS sharing SMT runqueue from CPU %d to CPU %d\n",
			       leader->cpu, rq->cpu);
			share_and_free_rq(leader, rq);
		} else
			rq_unlock(rq);
	}
#endif /* CONFIG_SCHED_SMT */
}

static void __init setup_rq_orders(void)
{
	int *selected_cpus, *ordered_cpus;
	struct rq *rq, *other_rq;
	int cpu, other_cpu, i;

	selected_cpus = kmalloc(sizeof(int) * NR_CPUS, GFP_ATOMIC);
	ordered_cpus = kmalloc(sizeof(int) * NR_CPUS, GFP_ATOMIC);

	total_runqueues = 0;
	for_each_online_cpu(cpu) {
		int locality, total_rqs = 0, total_cpus = 0;

		rq = cpu_rq(cpu);
		if (rq->is_leader)
			total_runqueues++;

		for (locality = LOCALITY_SAME; locality <= LOCALITY_DISTANT; locality++) {
			int selected_cpu_cnt, selected_cpu_idx, test_cpu_idx, cpu_idx, best_locality, test_cpu;
			int ordered_cpus_idx;

			ordered_cpus_idx = -1;
			selected_cpu_cnt = 0;

			for_each_online_cpu(test_cpu) {
				if (cpu < num_online_cpus() / 2)
					other_cpu = cpu + test_cpu;
				else
					other_cpu = cpu - test_cpu;
				if (other_cpu < 0)
					other_cpu += num_online_cpus();
				else
					other_cpu %= num_online_cpus();
				/* gather CPUs of the same locality */
				if (rq->cpu_locality[other_cpu] == locality) {
					selected_cpus[selected_cpu_cnt] = other_cpu;
					selected_cpu_cnt++;
				}
			}

			/* reserve first CPU as starting point */
			if (selected_cpu_cnt > 0) {
				ordered_cpus_idx++;
				ordered_cpus[ordered_cpus_idx] = selected_cpus[ordered_cpus_idx];
				selected_cpus[ordered_cpus_idx] = -1;
			}

			/* take each CPU and sort it within the same locality based on each inter-CPU localities */
			for (test_cpu_idx = 1; test_cpu_idx < selected_cpu_cnt; test_cpu_idx++) {
				/* starting point with worst locality and current CPU */
				best_locality = LOCALITY_DISTANT;
				selected_cpu_idx = test_cpu_idx;

				/* try to find the best locality within group */
				for (cpu_idx = 1; cpu_idx < selected_cpu_cnt; cpu_idx++) {
					/* if CPU has not been used and locality is better */
					if (selected_cpus[cpu_idx] > -1) {
						other_rq = cpu_rq(ordered_cpus[ordered_cpus_idx]);
						if (best_locality > other_rq->cpu_locality[selected_cpus[cpu_idx]]) {
							/* assign best locality and best CPU idx in array */
							best_locality = other_rq->cpu_locality[selected_cpus[cpu_idx]];
							selected_cpu_idx = cpu_idx;
						}
					}
				}

				/* add our next best CPU to ordered list */
				ordered_cpus_idx++;
				ordered_cpus[ordered_cpus_idx] = selected_cpus[selected_cpu_idx];
				/* mark this CPU as used */
				selected_cpus[selected_cpu_idx] =  -1;
			}

			/* set up RQ and CPU orders */
			for (test_cpu = 0; test_cpu <= ordered_cpus_idx; test_cpu++) {
				other_rq = cpu_rq(ordered_cpus[test_cpu]);
				/* set up cpu orders */
				rq->cpu_order[total_cpus++] = other_rq;
				if (other_rq->is_leader) {
					/* set up RQ orders */
					rq->rq_order[total_rqs++] = other_rq;
				}
			}
		}
	}

	kfree(selected_cpus);
	kfree(ordered_cpus);

#ifdef CONFIG_X86
	for_each_online_cpu(cpu) {
		rq = cpu_rq(cpu);
		for (i = 0; i < total_runqueues; i++) {
			printk(KERN_DEBUG "MuQSS CPU %d llc %d RQ order %d RQ %d llc %d\n", cpu, per_cpu(cpu_llc_id, cpu), i,
			       rq->rq_order[i]->cpu, per_cpu(cpu_llc_id, rq->rq_order[i]->cpu));
		}
	}

	for_each_online_cpu(cpu) {
		rq = cpu_rq(cpu);
		for (i = 0; i < num_online_cpus(); i++) {
			printk(KERN_DEBUG "MuQSS CPU %d llc %d CPU order %d RQ %d llc %d\n", cpu, per_cpu(cpu_llc_id, cpu), i,
			       rq->cpu_order[i]->cpu, per_cpu(cpu_llc_id, rq->cpu_order[i]->cpu));
		}
	}
#endif
}

void __init sched_init_smp(void)
{
	sched_init_numa();

	/*
	 * There's no userspace yet to cause hotplug operations; hence all the
	 * cpu masks are stable and all blatant races in the below code cannot
	 * happen.
	 */
	mutex_lock(&sched_domains_mutex);
	sched_init_domains(cpu_active_mask);
	mutex_unlock(&sched_domains_mutex);

	/* Move init over to a non-isolated CPU */
	if (set_cpus_allowed_ptr(current, housekeeping_cpumask(HK_FLAG_DOMAIN)) < 0)
		BUG();

	local_irq_disable();
	mutex_lock(&sched_domains_mutex);
	lock_all_rqs();

	printk(KERN_INFO "MuQSS possible/present/online CPUs: %d/%d/%d\n",
		num_possible_cpus(), num_present_cpus(), num_online_cpus());

	select_leaders();

	unlock_all_rqs();
	mutex_unlock(&sched_domains_mutex);

	share_rqs();

	local_irq_enable();

	setup_rq_orders();

	switch (rqshare) {
		case RQSHARE_ALL:
			/* This should only ever read 1 */
			printk(KERN_INFO "MuQSS runqueue share type ALL total runqueues: %d\n",
			       total_runqueues);
			break;
		case RQSHARE_SMP:
			printk(KERN_INFO "MuQSS runqueue share type SMP total runqueues: %d\n",
			       total_runqueues);
			break;
		case RQSHARE_MC:
			printk(KERN_INFO "MuQSS runqueue share type MC total runqueues: %d\n",
			       total_runqueues);
			break;
		case RQSHARE_MC_LLC:
			printk(KERN_INFO "MuQSS runqueue share type LLC total runqueues: %d\n",
			       total_runqueues);
			break;
		case RQSHARE_SMT:
			printk(KERN_INFO "MuQSS runqueue share type SMT total runqueues: %d\n",
			       total_runqueues);
			break;
		case RQSHARE_NONE:
			printk(KERN_INFO "MuQSS runqueue share type NONE total runqueues: %d\n",
			       total_runqueues);
			break;
	}

	sched_smp_initialized = true;
}
#else
void __init sched_init_smp(void)
{
	sched_smp_initialized = true;
}
#endif /* CONFIG_SMP */

int in_sched_functions(unsigned long addr)
{
	return in_lock_functions(addr) ||
		(addr >= (unsigned long)__sched_text_start
		&& addr < (unsigned long)__sched_text_end);
}

#ifdef CONFIG_CGROUP_SCHED
/* task group related information */
struct task_group {
	struct cgroup_subsys_state css;

	struct rcu_head rcu;
	struct list_head list;

	struct task_group *parent;
	struct list_head siblings;
	struct list_head children;
};

/*
 * Default task group.
 * Every task in system belongs to this group at bootup.
 */
struct task_group root_task_group;
LIST_HEAD(task_groups);

/* Cacheline aligned slab cache for task_group */
static struct kmem_cache *task_group_cache __read_mostly;
#endif /* CONFIG_CGROUP_SCHED */

void __init sched_init(void)
{
#ifdef CONFIG_SMP
	int cpu_ids;
#endif
	int i;
	struct rq *rq;

	wait_bit_init();

	prio_ratios[0] = 128;
	for (i = 1 ; i < NICE_WIDTH ; i++)
		prio_ratios[i] = prio_ratios[i - 1] * 11 / 10;

	skiplist_node_init(&init_task.node);

#ifdef CONFIG_SMP
	init_defrootdomain();
	cpumask_clear(&cpu_idle_map);
#else
	uprq = &per_cpu(runqueues, 0);
#endif

#ifdef CONFIG_CGROUP_SCHED
	task_group_cache = KMEM_CACHE(task_group, 0);

	list_add(&root_task_group.list, &task_groups);
	INIT_LIST_HEAD(&root_task_group.children);
	INIT_LIST_HEAD(&root_task_group.siblings);
#endif /* CONFIG_CGROUP_SCHED */
	for_each_possible_cpu(i) {
		rq = cpu_rq(i);
		rq->node = kmalloc(sizeof(skiplist_node), GFP_ATOMIC);
		skiplist_init(rq->node);
		rq->sl = new_skiplist(rq->node);
		rq->lock = kmalloc(sizeof(raw_spinlock_t), GFP_ATOMIC);
		raw_spin_lock_init(rq->lock);
		rq->nr_running = 0;
		rq->nr_uninterruptible = 0;
		rq->nr_switches = 0;
		rq->clock = rq->old_clock = rq->last_niffy = rq->niffies = 0;
		rq->last_jiffy = jiffies;
		rq->user_ns = rq->nice_ns = rq->softirq_ns = rq->system_ns =
			      rq->iowait_ns = rq->idle_ns = 0;
		rq->dither = 0;
		set_rq_task(rq, &init_task);
		rq->iso_ticks = 0;
		rq->iso_refractory = false;
#ifdef CONFIG_SMP
		rq->is_leader = true;
		rq->smp_leader = NULL;
#ifdef CONFIG_SCHED_MC
		rq->mc_leader = NULL;
#endif
#ifdef CONFIG_SCHED_SMT
		rq->smt_leader = NULL;
#endif
		rq->sd = NULL;
		rq->rd = NULL;
		rq->online = false;
		rq->cpu = i;
		rq_attach_root(rq, &def_root_domain);
#endif
		init_rq_hrexpiry(rq);
		atomic_set(&rq->nr_iowait, 0);
	}

#ifdef CONFIG_SMP
	cpu_ids = i;
	/*
	 * Set the base locality for cpu cache distance calculation to
	 * "distant" (3). Make sure the distance from a CPU to itself is 0.
	 */
	for_each_possible_cpu(i) {
		int j;

		rq = cpu_rq(i);
#ifdef CONFIG_SCHED_SMT
		rq->siblings_idle = sole_cpu_idle;
#endif
#ifdef CONFIG_SCHED_MC
		rq->cache_idle = sole_cpu_idle;
#endif
		rq->cpu_locality = kmalloc(cpu_ids * sizeof(int *), GFP_ATOMIC);
		for_each_possible_cpu(j) {
			if (i == j)
				rq->cpu_locality[j] = LOCALITY_SAME;
			else
				rq->cpu_locality[j] = LOCALITY_DISTANT;
		}
		rq->rq_order = kmalloc(cpu_ids * sizeof(struct rq *), GFP_ATOMIC);
		rq->cpu_order = kmalloc(cpu_ids * sizeof(struct rq *), GFP_ATOMIC);
		rq->rq_order[0] = rq->cpu_order[0] = rq;
		for (j = 1; j < cpu_ids; j++)
			rq->rq_order[j] = rq->cpu_order[j] = cpu_rq(j);
	}
#endif

	/*
	 * The boot idle thread does lazy MMU switching as well:
	 */
	mmgrab(&init_mm);
	enter_lazy_tlb(&init_mm, current);

	/*
	 * Make us the idle thread. Technically, schedule() should not be
	 * called from this thread, however somewhere below it might be,
	 * but because we are the idle thread, we just pick up running again
	 * when this runqueue becomes "idle".
	 */
	init_idle(current, smp_processor_id());

#ifdef CONFIG_SMP
	idle_thread_set_boot_cpu();
#endif /* SMP */

	init_schedstats();

	psi_init();
}

#ifdef CONFIG_DEBUG_ATOMIC_SLEEP
static inline int preempt_count_equals(int preempt_offset)
{
	int nested = preempt_count() + rcu_preempt_depth();

	return (nested == preempt_offset);
}

void __might_sleep(const char *file, int line, int preempt_offset)
{
	/*
	 * Blocking primitives will set (and therefore destroy) current->state,
	 * since we will exit with TASK_RUNNING make sure we enter with it,
	 * otherwise we will destroy state.
	 */
	WARN_ONCE(current->state != TASK_RUNNING && current->task_state_change,
			"do not call blocking ops when !TASK_RUNNING; "
			"state=%lx set at [<%p>] %pS\n",
			current->state,
			(void *)current->task_state_change,
			(void *)current->task_state_change);

	___might_sleep(file, line, preempt_offset);
}
EXPORT_SYMBOL(__might_sleep);

void __cant_sleep(const char *file, int line, int preempt_offset)
{
	static unsigned long prev_jiffy;

	if (irqs_disabled())
		return;

	if (!IS_ENABLED(CONFIG_PREEMPT_COUNT))
		return;

	if (preempt_count() > preempt_offset)
		return;

	if (time_before(jiffies, prev_jiffy + HZ) && prev_jiffy)
		return;
	prev_jiffy = jiffies;

	printk(KERN_ERR "BUG: assuming atomic context at %s:%d\n", file, line);
	printk(KERN_ERR "in_atomic(): %d, irqs_disabled(): %d, pid: %d, name: %s\n",
			in_atomic(), irqs_disabled(),
			current->pid, current->comm);

	debug_show_held_locks(current);
	dump_stack();
	add_taint(TAINT_WARN, LOCKDEP_STILL_OK);
}
EXPORT_SYMBOL_GPL(__cant_sleep);

void ___might_sleep(const char *file, int line, int preempt_offset)
{
	/* Ratelimiting timestamp: */
	static unsigned long prev_jiffy;

	unsigned long preempt_disable_ip;

	/* WARN_ON_ONCE() by default, no rate limit required: */
	rcu_sleep_check();

	if ((preempt_count_equals(preempt_offset) && !irqs_disabled() &&
	     !is_idle_task(current) && !current->non_block_count) ||
	    system_state == SYSTEM_BOOTING || system_state > SYSTEM_RUNNING ||
	    oops_in_progress)
		return;

	if (time_before(jiffies, prev_jiffy + HZ) && prev_jiffy)
		return;
	prev_jiffy = jiffies;

	/* Save this before calling printk(), since that will clobber it: */
	preempt_disable_ip = get_preempt_disable_ip(current);

	printk(KERN_ERR
		"BUG: sleeping function called from invalid context at %s:%d\n",
			file, line);
	printk(KERN_ERR
		"in_atomic(): %d, irqs_disabled(): %d, non_block: %d, pid: %d, name: %s\n",
			in_atomic(), irqs_disabled(), current->non_block_count,
			current->pid, current->comm);

	if (task_stack_end_corrupted(current))
		printk(KERN_EMERG "Thread overran stack, or stack corrupted\n");

	debug_show_held_locks(current);
	if (irqs_disabled())
		print_irqtrace_events(current);
	if (IS_ENABLED(CONFIG_DEBUG_PREEMPT)
	    && !preempt_count_equals(preempt_offset)) {
		pr_err("Preemption disabled at:");
		print_ip_sym(KERN_ERR, preempt_disable_ip);
	}
	dump_stack();
	add_taint(TAINT_WARN, LOCKDEP_STILL_OK);
}
EXPORT_SYMBOL(___might_sleep);
#endif

#ifdef CONFIG_MAGIC_SYSRQ
static inline void normalise_rt_tasks(void)
{
	struct sched_attr attr = {};
	struct task_struct *g, *p;
	struct rq_flags rf;
	struct rq *rq;

	read_lock(&tasklist_lock);
	for_each_process_thread(g, p) {
		/*
		 * Only normalize user tasks:
		 */
		if (p->flags & PF_KTHREAD)
			continue;

		if (!rt_task(p) && !iso_task(p))
			continue;

		rq = task_rq_lock(p, &rf);
		__setscheduler(p, rq, SCHED_NORMAL, 0, &attr, false);
		task_rq_unlock(rq, p, &rf);
	}
	read_unlock(&tasklist_lock);
}

void normalize_rt_tasks(void)
{
	normalise_rt_tasks();
}
#endif /* CONFIG_MAGIC_SYSRQ */

#if defined(CONFIG_IA64) || defined(CONFIG_KGDB_KDB)
/*
 * These functions are only useful for the IA64 MCA handling, or kdb.
 *
 * They can only be called when the whole system has been
 * stopped - every CPU needs to be quiescent, and no scheduling
 * activity can take place. Using them for anything else would
 * be a serious bug, and as a result, they aren't even visible
 * under any other configuration.
 */

/**
 * curr_task - return the current task for a given CPU.
 * @cpu: the processor in question.
 *
 * ONLY VALID WHEN THE WHOLE SYSTEM IS STOPPED!
 *
 * Return: The current task for @cpu.
 */
struct task_struct *curr_task(int cpu)
{
	return cpu_curr(cpu);
}

#endif /* defined(CONFIG_IA64) || defined(CONFIG_KGDB_KDB) */

#ifdef CONFIG_IA64
/**
 * ia64_set_curr_task - set the current task for a given CPU.
 * @cpu: the processor in question.
 * @p: the task pointer to set.
 *
 * Description: This function must only be used when non-maskable interrupts
 * are serviced on a separate stack.  It allows the architecture to switch the
 * notion of the current task on a CPU in a non-blocking manner.  This function
 * must be called with all CPU's synchronised, and interrupts disabled, the
 * and caller must save the original value of the current task (see
 * curr_task() above) and restore that value before reenabling interrupts and
 * re-starting the system.
 *
 * ONLY VALID WHEN THE WHOLE SYSTEM IS STOPPED!
 */
void ia64_set_curr_task(int cpu, struct task_struct *p)
{
	cpu_curr(cpu) = p;
}

#endif

void init_idle_bootup_task(struct task_struct *idle)
{}

#ifdef CONFIG_SCHED_DEBUG
__read_mostly bool sched_debug_enabled;

void proc_sched_show_task(struct task_struct *p, struct pid_namespace *ns,
			  struct seq_file *m)
{
	seq_printf(m, "%s (%d, #threads: %d)\n", p->comm, task_pid_nr_ns(p, ns),
		   get_nr_threads(p));
}

void proc_sched_set_task(struct task_struct *p)
{}
#endif

#ifdef CONFIG_CGROUP_SCHED
static void sched_free_group(struct task_group *tg)
{
	kmem_cache_free(task_group_cache, tg);
}

/* allocate runqueue etc for a new task group */
struct task_group *sched_create_group(struct task_group *parent)
{
	struct task_group *tg;

	tg = kmem_cache_alloc(task_group_cache, GFP_KERNEL | __GFP_ZERO);
	if (!tg)
		return ERR_PTR(-ENOMEM);

	return tg;
}

void sched_online_group(struct task_group *tg, struct task_group *parent)
{
}

/* rcu callback to free various structures associated with a task group */
static void sched_free_group_rcu(struct rcu_head *rhp)
{
	/* Now it should be safe to free those cfs_rqs */
	sched_free_group(container_of(rhp, struct task_group, rcu));
}

void sched_destroy_group(struct task_group *tg)
{
	/* Wait for possible concurrent references to cfs_rqs complete */
	call_rcu(&tg->rcu, sched_free_group_rcu);
}

void sched_offline_group(struct task_group *tg)
{
}

static inline struct task_group *css_tg(struct cgroup_subsys_state *css)
{
	return css ? container_of(css, struct task_group, css) : NULL;
}

static struct cgroup_subsys_state *
cpu_cgroup_css_alloc(struct cgroup_subsys_state *parent_css)
{
	struct task_group *parent = css_tg(parent_css);
	struct task_group *tg;

	if (!parent) {
		/* This is early initialization for the top cgroup */
		return &root_task_group.css;
	}

	tg = sched_create_group(parent);
	if (IS_ERR(tg))
		return ERR_PTR(-ENOMEM);
	return &tg->css;
}

/* Expose task group only after completing cgroup initialization */
static int cpu_cgroup_css_online(struct cgroup_subsys_state *css)
{
	struct task_group *tg = css_tg(css);
	struct task_group *parent = css_tg(css->parent);

	if (parent)
		sched_online_group(tg, parent);
	return 0;
}

static void cpu_cgroup_css_released(struct cgroup_subsys_state *css)
{
	struct task_group *tg = css_tg(css);

	sched_offline_group(tg);
}

static void cpu_cgroup_css_free(struct cgroup_subsys_state *css)
{
	struct task_group *tg = css_tg(css);

	/*
	 * Relies on the RCU grace period between css_released() and this.
	 */
	sched_free_group(tg);
}

static void cpu_cgroup_fork(struct task_struct *task)
{
}

static int cpu_cgroup_can_attach(struct cgroup_taskset *tset)
{
	return 0;
}

static void cpu_cgroup_attach(struct cgroup_taskset *tset)
{
}

static struct cftype cpu_legacy_files[] = {
	{ }	/* Terminate */
};

static struct cftype cpu_files[] = {
	{ }	/* terminate */
};

static int cpu_extra_stat_show(struct seq_file *sf,
			       struct cgroup_subsys_state *css)
{
	return 0;
}

struct cgroup_subsys cpu_cgrp_subsys = {
	.css_alloc	= cpu_cgroup_css_alloc,
	.css_online	= cpu_cgroup_css_online,
	.css_released	= cpu_cgroup_css_released,
	.css_free	= cpu_cgroup_css_free,
	.css_extra_stat_show = cpu_extra_stat_show,
	.fork		= cpu_cgroup_fork,
	.can_attach	= cpu_cgroup_can_attach,
	.attach		= cpu_cgroup_attach,
	.legacy_cftypes	= cpu_files,
	.legacy_cftypes	= cpu_legacy_files,
	.dfl_cftypes	= cpu_files,
	.early_init	= true,
	.threaded	= true,
};
#endif	/* CONFIG_CGROUP_SCHED */

void call_trace_sched_update_nr_running(struct rq *rq, int count)
{
        trace_sched_update_nr_running_tp(rq, count);
}

/* CFS Compat */
#ifdef CONFIG_RCU_TORTURE_TEST
int sysctl_sched_rt_runtime;
#endif
