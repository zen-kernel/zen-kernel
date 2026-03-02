#ifndef _KERNEL_SCHED_ALT_SCHED_H
#define _KERNEL_SCHED_ALT_SCHED_H

#include <linux/context_tracking.h>
#include <linux/memblock.h>
#include <linux/profile.h>
#include <linux/stop_machine.h>
#include <linux/sched/rseq_api.h>
#include <linux/syscalls.h>
#include <linux/tick.h>

#include <trace/events/power.h>
#include <trace/events/sched.h>

#include "../workqueue_internal.h"

#include "cpupri.h"

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

extern struct task_group *sched_create_group(struct task_group *parent);
extern void sched_online_group(struct task_group *tg,
			       struct task_group *parent);
extern void sched_destroy_group(struct task_group *tg);
extern void sched_release_group(struct task_group *tg);
#endif /* CONFIG_CGROUP_SCHED */

#define MIN_SCHED_NORMAL_PRIO	(32)
/*
 * levels: RT(0-24), reserved(25-31), NORMAL(32-63), cpu idle task(64)
 *
 * -- BMQ --
 * NORMAL: (lower boost range 12, NICE_WIDTH 40, higher boost range 12) / 2
 * -- PDS --
 * NORMAL: SCHED_EDGE_DELTA + ((NICE_WIDTH 40) / 2)
 */
#define SCHED_LEVELS		(64 + 1)

#define IDLE_TASK_SCHED_PRIO	(SCHED_LEVELS - 1)

/*
 * Increase resolution of nice-level calculations for 64-bit architectures.
 * The extra resolution improves shares distribution and load balancing of
 * low-weight task groups (eg. nice +19 on an autogroup), deeper taskgroup
 * hierarchies, especially on larger systems. This is not a user-visible change
 * and does not change the user-interface for setting shares/weights.
 *
 * We increase resolution only if we have enough bits to allow this increased
 * resolution (i.e. 64-bit). The costs for increasing resolution when 32-bit
 * are pretty high and the returns do not justify the increased costs.
 *
 * Really only required when CONFIG_FAIR_GROUP_SCHED=y is also set, but to
 * increase coverage and consistency always enable it on 64-bit platforms.
 */
#ifdef CONFIG_64BIT
# define NICE_0_LOAD_SHIFT	(SCHED_FIXEDPOINT_SHIFT + SCHED_FIXEDPOINT_SHIFT)
# define scale_load(w)		((w) << SCHED_FIXEDPOINT_SHIFT)
# define scale_load_down(w) \
({ \
	unsigned long __w = (w); \
	if (__w) \
		__w = max(2UL, __w >> SCHED_FIXEDPOINT_SHIFT); \
	__w; \
})
#else
# define NICE_0_LOAD_SHIFT	(SCHED_FIXEDPOINT_SHIFT)
# define scale_load(w)		(w)
# define scale_load_down(w)	(w)
#endif

/* task_struct::on_rq states: */
#define TASK_ON_RQ_QUEUED	1
#define TASK_ON_RQ_MIGRATING	2

static inline int task_on_rq_queued(struct task_struct *p)
{
	return READ_ONCE(p->on_rq) == TASK_ON_RQ_QUEUED;
}

static inline int task_on_rq_migrating(struct task_struct *p)
{
	return READ_ONCE(p->on_rq) == TASK_ON_RQ_MIGRATING;
}

/* Wake flags. The first three directly map to some SD flag value */
#define WF_EXEC         0x02 /* Wakeup after exec; maps to SD_BALANCE_EXEC */
#define WF_FORK         0x04 /* Wakeup after fork; maps to SD_BALANCE_FORK */
#define WF_TTWU         0x08 /* Wakeup;            maps to SD_BALANCE_WAKE */

#define WF_SYNC         0x10 /* Waker goes to sleep after wakeup */
#define WF_MIGRATED     0x20 /* Internal use, task got migrated */
#define WF_CURRENT_CPU  0x40 /* Prefer to move the wakee to the current CPU. */

static_assert(WF_EXEC == SD_BALANCE_EXEC);
static_assert(WF_FORK == SD_BALANCE_FORK);
static_assert(WF_TTWU == SD_BALANCE_WAKE);

/*
 * {de,en}queue flags:
 *
 * SLEEP/WAKEUP - task is no-longer/just-became runnable
 *
 * SAVE/RESTORE - an otherwise spurious dequeue/enqueue, done to ensure tasks
 *                are in a known state which allows modification. Such pairs
 *                should preserve as much state as possible.
 *
 * MOVE - paired with SAVE/RESTORE, explicitly does not preserve the location
 *        in the runqueue.
 *
 * NOCLOCK - skip the update_rq_clock() (avoids double updates)
 *
 * MIGRATION - p->on_rq == TASK_ON_RQ_MIGRATING (used for DEADLINE)
 *
 * DELAYED - de/re-queue a sched_delayed task
 *
 * CLASS - going to update p->sched_class; makes sched_change call the
 *         various switch methods.
 *
 * ENQUEUE_HEAD      - place at front of runqueue (tail if not specified)
 * ENQUEUE_REPLENISH - CBS (replenish runtime and postpone deadline)
 * ENQUEUE_MIGRATED  - the task was migrated during wakeup
 * ENQUEUE_RQ_SELECTED - ->select_task_rq() was called
 *
 * XXX SAVE/RESTORE in combination with CLASS doesn't really make sense, but
 * SCHED_DEADLINE seems to rely on this for now.
 */

#define DEQUEUE_SLEEP		0x0001 /* Matches ENQUEUE_WAKEUP */
#define DEQUEUE_SAVE		0x0002 /* Matches ENQUEUE_RESTORE */
#define DEQUEUE_MOVE		0x0004 /* Matches ENQUEUE_MOVE */
#define DEQUEUE_NOCLOCK		0x0008 /* Matches ENQUEUE_NOCLOCK */

#define DEQUEUE_MIGRATING	0x0010 /* Matches ENQUEUE_MIGRATING */
#define DEQUEUE_DELAYED		0x0020 /* Matches ENQUEUE_DELAYED */
#define DEQUEUE_CLASS		0x0040 /* Matches ENQUEUE_CLASS */

#define DEQUEUE_SPECIAL		0x00010000
#define DEQUEUE_THROTTLE	0x00020000

#define ENQUEUE_WAKEUP		0x0001
#define ENQUEUE_RESTORE		0x0002
#define ENQUEUE_MOVE		0x0004
#define ENQUEUE_NOCLOCK		0x0008

#define ENQUEUE_MIGRATING	0x0010
#define ENQUEUE_DELAYED		0x0020
#define ENQUEUE_CLASS		0x0040

#define ENQUEUE_HEAD		0x00010000
#define ENQUEUE_REPLENISH	0x00020000
#define ENQUEUE_MIGRATED	0x00040000
#define ENQUEUE_INITIAL		0x00080000
#define ENQUEUE_RQ_SELECTED	0x00100000


#define SCHED_QUEUE_BITS	(SCHED_LEVELS - 1)

struct sched_queue {
	DECLARE_BITMAP(bitmap, SCHED_QUEUE_BITS);
	struct list_head heads[SCHED_LEVELS];
};

struct rq;
struct cpuidle_state;

struct balance_callback {
	struct balance_callback *next;
	void (*func)(struct rq *rq);
};

typedef void (*balance_func_t)(struct rq *rq, int cpu);

struct balance_arg {
	struct task_struct	*task;
	int			active;
	cpumask_t		*cpumask;
};

/*
 * This is the main, per-CPU runqueue data structure.
 * This data should only be modified by the local cpu.
 */
struct rq {
	/* runqueue lock: */
	raw_spinlock_t			lock;

	struct task_struct __rcu	*curr;
	struct task_struct		*idle;
	struct task_struct		*stop;
	struct mm_struct		*prev_mm;

	struct sched_queue		queue		____cacheline_aligned;

	int				prio;
#ifdef CONFIG_SCHED_PDS
	int				prio_idx;
	u64				time_edge;
#endif

	/* switch count */
	u64 nr_switches;

	atomic_t nr_iowait;

	u64 last_seen_need_resched_ns;
	int ticks_without_resched;

#ifdef CONFIG_MEMBARRIER
	int membarrier_state;
#endif

	int cpu;		/* cpu of this runqueue */
	bool online;

	unsigned int		ttwu_pending;
	unsigned char		nohz_idle_balance;
	unsigned char		idle_balance;

#ifdef CONFIG_HAVE_SCHED_AVG_IRQ
	struct sched_avg	avg_irq;
#endif

	balance_func_t		balance_func;
	struct balance_arg	active_balance_arg		____cacheline_aligned;
	struct cpu_stop_work	active_balance_work;

	struct balance_callback	*balance_callback;

#ifdef CONFIG_HOTPLUG_CPU
	struct rcuwait		hotplug_wait;
#endif
	unsigned int		nr_pinned;

#ifdef CONFIG_IRQ_TIME_ACCOUNTING
	u64 prev_irq_time;
#endif /* CONFIG_IRQ_TIME_ACCOUNTING */
#ifdef CONFIG_PARAVIRT
	u64 prev_steal_time;
#endif /* CONFIG_PARAVIRT */
#ifdef CONFIG_PARAVIRT_TIME_ACCOUNTING
	u64 prev_steal_time_rq;
#endif /* CONFIG_PARAVIRT_TIME_ACCOUNTING */

	/* For genenal cpu load util */
	s32 load_history;
	u64 load_block;
	u64 load_stamp;

	/* calc_load related fields */
	unsigned long calc_load_update;
	long calc_load_active;

	/* Ensure that all clocks are in the same cache line */
	u64			clock ____cacheline_aligned;
	u64			clock_task;
	u64			prio_balance_time;

	unsigned int  nr_running;
	unsigned long nr_uninterruptible;

#ifdef CONFIG_SCHED_HRTICK
	call_single_data_t hrtick_csd;
	struct hrtimer		hrtick_timer;
	ktime_t			hrtick_time;
#endif

#ifdef CONFIG_SCHEDSTATS

	/* latency stats */
	struct sched_info rq_sched_info;
	unsigned long long rq_cpu_time;
	/* could above be rq->cfs_rq.exec_clock + rq->rt_rq.rt_runtime ? */

	/* sys_sched_yield() stats */
	unsigned int yld_count;

	/* schedule() stats */
	unsigned int sched_switch;
	unsigned int sched_count;
	unsigned int sched_goidle;

	/* try_to_wake_up() stats */
	unsigned int ttwu_count;
	unsigned int ttwu_local;
#endif /* CONFIG_SCHEDSTATS */

#ifdef CONFIG_CPU_IDLE
	/* Must be inspected within a rcu lock section */
	struct cpuidle_state *idle_state;
#endif

#ifdef CONFIG_NO_HZ_COMMON
	call_single_data_t	nohz_csd;
	atomic_t		nohz_flags;
#endif /* CONFIG_NO_HZ_COMMON */

	/* Scratch cpumask to be temporarily used under rq_lock */
	cpumask_var_t		scratch_mask;
};

extern unsigned int sysctl_sched_base_slice;

extern unsigned long rq_load_util(struct rq *rq, unsigned long max);

extern unsigned long calc_load_update;
extern atomic_long_t calc_load_tasks;

extern void calc_global_load_tick(struct rq *this_rq);
extern long calc_load_fold_active(struct rq *this_rq, long adjust);

DECLARE_PER_CPU_SHARED_ALIGNED(struct rq, runqueues);
#define cpu_rq(cpu)		(&per_cpu(runqueues, (cpu)))
#define this_rq()		this_cpu_ptr(&runqueues)
#define task_rq(p)		cpu_rq(task_cpu(p))
#define cpu_curr(cpu)		(cpu_rq(cpu)->curr)
#define raw_rq()		raw_cpu_ptr(&runqueues)

static inline bool idle_rq(struct rq *rq)
{
	return rq->curr == rq->idle && !rq->nr_running && !rq->ttwu_pending;
}

/**
 * available_idle_cpu - is a given CPU idle for enqueuing work.
 * @cpu: the CPU in question.
 *
 * Return: 1 if the CPU is currently idle. 0 otherwise.
 */
static inline bool available_idle_cpu(int cpu)
{
	if (!idle_rq(cpu_rq(cpu)))
		return 0;

	if (vcpu_is_preempted(cpu))
		return 0;

	return 1;
}

#ifdef CONFIG_SYSCTL
void register_sched_domain_sysctl(void);
void unregister_sched_domain_sysctl(void);
#else
static inline void register_sched_domain_sysctl(void)
{
}
static inline void unregister_sched_domain_sysctl(void)
{
}
#endif

extern bool sched_smp_initialized;

enum {
#ifdef CONFIG_SCHED_SMT
	SMT_LEVEL_SPACE_HOLDER,
#endif
	CLUSTER_LEVEL_SPACE_HOLDER,
	COREGROUP_LEVEL_SPACE_HOLDER,
	CORE_LEVEL_SPACE_HOLDER,
	OTHER_LEVEL_SPACE_HOLDER,
	NR_CPU_AFFINITY_LEVELS
};

DECLARE_PER_CPU_ALIGNED(cpumask_t [NR_CPU_AFFINITY_LEVELS], sched_cpu_topo_masks);

static inline int
__best_mask_cpu(const cpumask_t *cpumask, const cpumask_t *mask)
{
	int cpu;

	while ((cpu = cpumask_any_and(cpumask, mask)) >= nr_cpu_ids)
		mask++;

	return cpu;
}

static inline int best_mask_cpu(int cpu, const cpumask_t *mask)
{
	return __best_mask_cpu(mask, per_cpu(sched_cpu_topo_masks, cpu));
}

extern void resched_latency_warn(int cpu, u64 latency);

#ifndef arch_scale_freq_tick
static __always_inline
void arch_scale_freq_tick(void)
{
}
#endif

#ifndef arch_scale_freq_capacity
static __always_inline
unsigned long arch_scale_freq_capacity(int cpu)
{
	return SCHED_CAPACITY_SCALE;
}
#endif

static inline u64 __rq_clock_broken(struct rq *rq)
{
	return READ_ONCE(rq->clock);
}

static inline u64 rq_clock(struct rq *rq)
{
	/*
	 * Relax lockdep_assert_held() checking as in VRQ, call to
	 * sched_info_xxxx() may not held rq->lock
	 * lockdep_assert_held(&rq->lock);
	 */
	return rq->clock;
}

static inline u64 rq_clock_task(struct rq *rq)
{
	/*
	 * Relax lockdep_assert_held() checking as in VRQ, call to
	 * sched_info_xxxx() may not held rq->lock
	 * lockdep_assert_held(&rq->lock);
	 */
	return rq->clock_task;
}

/*
 * Below are scheduler API which using in other kernel code
 * It use the dummy rq_flags
 * ToDo : BMQ need to support these APIs for compatibility with mainline
 * scheduler code.
 */
struct rq_flags {
	unsigned long flags;
	raw_spinlock_t *lock;
};

struct rq *__task_rq_lock(struct task_struct *p, struct rq_flags *rf)
	__acquires(rq->lock);

struct rq *task_rq_lock(struct task_struct *p, struct rq_flags *rf)
	__acquires(p->pi_lock)
	__acquires(rq->lock);

static inline void __task_rq_unlock(struct rq *rq, struct rq_flags *rf)
	__releases(rq->lock)
{
	raw_spin_unlock(&rq->lock);
}

static inline void
task_rq_unlock(struct rq *rq, struct task_struct *p, struct rq_flags *rf)
	__releases(rq->lock)
	__releases(p->pi_lock)
{
	raw_spin_unlock(&rq->lock);
	raw_spin_unlock_irqrestore(&p->pi_lock, rf->flags);
}

DEFINE_LOCK_GUARD_1(task_rq_lock, struct task_struct,
		    _T->rq = task_rq_lock(_T->lock, &_T->rf),
		    task_rq_unlock(_T->rq, _T->lock, &_T->rf),
		    struct rq *rq; struct rq_flags rf)

static inline void
rq_lock(struct rq *rq, struct rq_flags *rf)
	__acquires(rq->lock)
{
	raw_spin_lock(&rq->lock);
}

static inline void
rq_unlock(struct rq *rq, struct rq_flags *rf)
	__releases(rq->lock)
{
	raw_spin_unlock(&rq->lock);
}

static inline void
rq_lock_irq(struct rq *rq, struct rq_flags *rf)
	__acquires(rq->lock)
{
	raw_spin_lock_irq(&rq->lock);
}

static inline void
rq_unlock_irq(struct rq *rq, struct rq_flags *rf)
	__releases(rq->lock)
{
	raw_spin_unlock_irq(&rq->lock);
}

DEFINE_LOCK_GUARD_1(rq_lock_irq, struct rq,
		    rq_lock_irq(_T->lock, &_T->rf),
		    rq_unlock_irq(_T->lock, &_T->rf),
		    struct rq_flags rf)

static inline struct rq *
this_rq_lock_irq(struct rq_flags *rf)
	__acquires(rq->lock)
{
	struct rq *rq;

	local_irq_disable();
	rq = this_rq();
	raw_spin_lock(&rq->lock);

	return rq;
}

static inline raw_spinlock_t *__rq_lockp(struct rq *rq)
{
	return &rq->lock;
}

static inline raw_spinlock_t *rq_lockp(struct rq *rq)
{
	return __rq_lockp(rq);
}

static inline void lockdep_assert_rq_held(struct rq *rq)
{
	lockdep_assert_held(__rq_lockp(rq));
}

extern void raw_spin_rq_lock_nested(struct rq *rq, int subclass);
extern void raw_spin_rq_unlock(struct rq *rq);

static inline void raw_spin_rq_lock(struct rq *rq)
{
	raw_spin_rq_lock_nested(rq, 0);
}

static inline void raw_spin_rq_lock_irq(struct rq *rq)
{
	local_irq_disable();
	raw_spin_rq_lock(rq);
}

static inline void raw_spin_rq_unlock_irq(struct rq *rq)
{
	raw_spin_rq_unlock(rq);
	local_irq_enable();
}

static inline int task_current(struct rq *rq, struct task_struct *p)
{
	return rq->curr == p;
}

static inline bool task_on_cpu(struct task_struct *p)
{
	return p->on_cpu;
}

extern struct static_key_false sched_schedstats;

#ifdef CONFIG_CPU_IDLE
static inline void idle_set_state(struct rq *rq,
				  struct cpuidle_state *idle_state)
{
	rq->idle_state = idle_state;
}

static inline struct cpuidle_state *idle_get_state(struct rq *rq)
{
	WARN_ON(!rcu_read_lock_held());
	return rq->idle_state;
}
#else
static inline void idle_set_state(struct rq *rq,
				  struct cpuidle_state *idle_state)
{
}

static inline struct cpuidle_state *idle_get_state(struct rq *rq)
{
	return NULL;
}
#endif

static inline int cpu_of(const struct rq *rq)
{
	return rq->cpu;
}

extern void resched_cpu(int cpu);

#include "stats.h"

#ifdef CONFIG_NO_HZ_COMMON
#define NOHZ_BALANCE_KICK_BIT	0
#define NOHZ_STATS_KICK_BIT	1

#define NOHZ_BALANCE_KICK	BIT(NOHZ_BALANCE_KICK_BIT)
#define NOHZ_STATS_KICK		BIT(NOHZ_STATS_KICK_BIT)

#define NOHZ_KICK_MASK	(NOHZ_BALANCE_KICK | NOHZ_STATS_KICK)

#define nohz_flags(cpu)	(&cpu_rq(cpu)->nohz_flags)

/* TODO: needed?
extern void nohz_balance_exit_idle(struct rq *rq);
#else
static inline void nohz_balance_exit_idle(struct rq *rq) { }
*/
#endif

#ifdef CONFIG_IRQ_TIME_ACCOUNTING
struct irqtime {
	u64			total;
	u64			tick_delta;
	u64			irq_start_time;
	struct u64_stats_sync	sync;
};

DECLARE_PER_CPU(struct irqtime, cpu_irqtime);
DECLARE_STATIC_KEY_FALSE(sched_clock_irqtime);

static inline int irqtime_enabled(void)
{
	return static_branch_likely(&sched_clock_irqtime);
}

/*
 * Returns the irqtime minus the softirq time computed by ksoftirqd.
 * Otherwise ksoftirqd's sum_exec_runtime is substracted its own runtime
 * and never move forward.
 */
static inline u64 irq_time_read(int cpu)
{
	struct irqtime *irqtime = &per_cpu(cpu_irqtime, cpu);
	unsigned int seq;
	u64 total;

	do {
		seq = __u64_stats_fetch_begin(&irqtime->sync);
		total = irqtime->total;
	} while (__u64_stats_fetch_retry(&irqtime->sync, seq));

	return total;
}
#else

static inline int irqtime_enabled(void)
{
	return 0;
}

#endif /* CONFIG_IRQ_TIME_ACCOUNTING */

#ifdef CONFIG_CPU_FREQ
DECLARE_PER_CPU(struct update_util_data __rcu *, cpufreq_update_util_data);
#endif /* CONFIG_CPU_FREQ */

#ifdef CONFIG_NO_HZ_FULL
extern int __init sched_tick_offload_init(void);
#else
static inline int sched_tick_offload_init(void) { return 0; }
#endif

#ifdef arch_scale_freq_capacity
#ifndef arch_scale_freq_invariant
#define arch_scale_freq_invariant()	(true)
#endif
#else /* arch_scale_freq_capacity */
#define arch_scale_freq_invariant()	(false)
#endif

unsigned long sugov_effective_cpu_perf(int cpu, unsigned long actual,
				 unsigned long min,
				 unsigned long max);

extern void schedule_idle(void);

#define cap_scale(v, s) ((v)*(s) >> SCHED_CAPACITY_SHIFT)

/*
 * !! For sched_setattr_nocheck() (kernel) only !!
 *
 * This is actually gross. :(
 *
 * It is used to make schedutil kworker(s) higher priority than SCHED_DEADLINE
 * tasks, but still be able to sleep. We need this on platforms that cannot
 * atomically change clock frequency. Remove once fast switching will be
 * available on such platforms.
 *
 * SUGOV stands for SchedUtil GOVernor.
 */
#define SCHED_FLAG_SUGOV	0x10000000

#ifdef CONFIG_MEMBARRIER
/*
 * The scheduler provides memory barriers required by membarrier between:
 * - prior user-space memory accesses and store to rq->membarrier_state,
 * - store to rq->membarrier_state and following user-space memory accesses.
 * In the same way it provides those guarantees around store to rq->curr.
 */
static inline void membarrier_switch_mm(struct rq *rq,
					struct mm_struct *prev_mm,
					struct mm_struct *next_mm)
{
	int membarrier_state;

	if (prev_mm == next_mm)
		return;

	membarrier_state = atomic_read(&next_mm->membarrier_state);
	if (READ_ONCE(rq->membarrier_state) == membarrier_state)
		return;

	WRITE_ONCE(rq->membarrier_state, membarrier_state);
}
#else
static inline void membarrier_switch_mm(struct rq *rq,
					struct mm_struct *prev_mm,
					struct mm_struct *next_mm)
{
}
#endif

#ifdef CONFIG_NUMA
extern int sched_numa_find_closest(const struct cpumask *cpus, int cpu);
#else
static inline int sched_numa_find_closest(const struct cpumask *cpus, int cpu)
{
	return nr_cpu_ids;
}
#endif

extern void swake_up_all_locked(struct swait_queue_head *q);
extern void __prepare_to_swait(struct swait_queue_head *q, struct swait_queue *wait);

extern int try_to_wake_up(struct task_struct *tsk, unsigned int state, int wake_flags);

#ifdef CONFIG_PREEMPT_DYNAMIC
extern int preempt_dynamic_mode;
extern int sched_dynamic_mode(const char *str);
extern void sched_dynamic_update(int mode);
#endif
extern const char *preempt_modes[];

static inline void nohz_run_idle_balance(int cpu) { }

static inline unsigned long
uclamp_eff_value(struct task_struct *p, enum uclamp_id clamp_id)
{
	if (clamp_id == UCLAMP_MIN)
		return 0;

	return SCHED_CAPACITY_SCALE;
}

static inline bool uclamp_rq_is_capped(struct rq *rq) { return false; }

static inline bool uclamp_is_used(void)
{
	return false;
}

static inline unsigned long
uclamp_rq_get(struct rq *rq, enum uclamp_id clamp_id)
{
	if (clamp_id == UCLAMP_MIN)
		return 0;

	return SCHED_CAPACITY_SCALE;
}

static inline void
uclamp_rq_set(struct rq *rq, enum uclamp_id clamp_id, unsigned int value)
{
}

static inline bool uclamp_rq_is_idle(struct rq *rq)
{
	return false;
}

#ifdef CONFIG_SCHED_MM_CID

static __always_inline bool cid_on_cpu(unsigned int cid)
{
	return cid & MM_CID_ONCPU;
}

static __always_inline bool cid_in_transit(unsigned int cid)
{
	return cid & MM_CID_TRANSIT;
}

static __always_inline unsigned int cpu_cid_to_cid(unsigned int cid)
{
	return cid & ~MM_CID_ONCPU;
}

static __always_inline unsigned int cid_to_cpu_cid(unsigned int cid)
{
	return cid | MM_CID_ONCPU;
}

static __always_inline unsigned int cid_to_transit_cid(unsigned int cid)
{
	return cid | MM_CID_TRANSIT;
}

static __always_inline unsigned int cid_from_transit_cid(unsigned int cid)
{
	return cid & ~MM_CID_TRANSIT;
}

static __always_inline bool cid_on_task(unsigned int cid)
{
	/* True if none of the MM_CID_ONCPU, MM_CID_TRANSIT, MM_CID_UNSET bits is set */
	return cid < MM_CID_TRANSIT;
}

static __always_inline void mm_drop_cid(struct mm_struct *mm, unsigned int cid)
{
	clear_bit(cid, mm_cidmask(mm));
}

static __always_inline void mm_unset_cid_on_task(struct task_struct *t)
{
	unsigned int cid = t->mm_cid.cid;

	t->mm_cid.cid = MM_CID_UNSET;
	if (cid_on_task(cid))
		mm_drop_cid(t->mm, cid);
}

static __always_inline void mm_drop_cid_on_cpu(struct mm_struct *mm, struct mm_cid_pcpu *pcp)
{
	/* Clear the ONCPU bit, but do not set UNSET in the per CPU storage */
	if (cid_on_cpu(pcp->cid)) {
		pcp->cid = cpu_cid_to_cid(pcp->cid);
		mm_drop_cid(mm, pcp->cid);
	}
}

static inline unsigned int __mm_get_cid(struct mm_struct *mm, unsigned int max_cids)
{
	unsigned int cid = find_first_zero_bit(mm_cidmask(mm), max_cids);

	if (cid >= max_cids)
		return MM_CID_UNSET;
	if (test_and_set_bit(cid, mm_cidmask(mm)))
		return MM_CID_UNSET;
	return cid;
}

static inline unsigned int mm_get_cid(struct mm_struct *mm)
{
	unsigned int cid = __mm_get_cid(mm, READ_ONCE(mm->mm_cid.max_cids));

	while (cid == MM_CID_UNSET) {
		cpu_relax();
		cid = __mm_get_cid(mm, num_possible_cpus());
	}
	return cid;
}

static inline unsigned int mm_cid_converge(struct mm_struct *mm, unsigned int orig_cid,
					   unsigned int max_cids)
{
	unsigned int new_cid, cid = cpu_cid_to_cid(orig_cid);

	/* Is it in the optimal CID space? */
	if (likely(cid < max_cids))
		return orig_cid;

	/* Try to find one in the optimal space. Otherwise keep the provided. */
	new_cid = __mm_get_cid(mm, max_cids);
	if (new_cid != MM_CID_UNSET) {
		mm_drop_cid(mm, cid);
		/* Preserve the ONCPU mode of the original CID */
		return new_cid | (orig_cid & MM_CID_ONCPU);
	}
	return orig_cid;
}

static __always_inline void mm_cid_update_task_cid(struct task_struct *t, unsigned int cid)
{
	if (t->mm_cid.cid != cid) {
		t->mm_cid.cid = cid;
		rseq_sched_set_ids_changed(t);
	}
}

static __always_inline void mm_cid_update_pcpu_cid(struct mm_struct *mm, unsigned int cid)
{
	__this_cpu_write(mm->mm_cid.pcpu->cid, cid);
}

static __always_inline void mm_cid_from_cpu(struct task_struct *t, unsigned int cpu_cid,
					    unsigned int mode)
{
	unsigned int max_cids, tcid = t->mm_cid.cid;
	struct mm_struct *mm = t->mm;

	max_cids = READ_ONCE(mm->mm_cid.max_cids);
	/* Optimize for the common case where both have the ONCPU bit set */
	if (likely(cid_on_cpu(cpu_cid & tcid))) {
		if (likely(cpu_cid_to_cid(cpu_cid) < max_cids)) {
			mm_cid_update_task_cid(t, cpu_cid);
			return;
		}
		/* Try to converge into the optimal CID space */
		cpu_cid = mm_cid_converge(mm, cpu_cid, max_cids);
	} else {
		/* Hand over or drop the task owned CID */
		if (cid_on_task(tcid)) {
			if (cid_on_cpu(cpu_cid))
				mm_unset_cid_on_task(t);
			else
				cpu_cid = cid_to_cpu_cid(tcid);
		}
		/* Still nothing, allocate a new one */
		if (!cid_on_cpu(cpu_cid))
			cpu_cid = cid_to_cpu_cid(mm_get_cid(mm));

		/* Handle the transition mode flag if required */
		if (mode & MM_CID_TRANSIT)
			cpu_cid = cpu_cid_to_cid(cpu_cid) | MM_CID_TRANSIT;
	}
	mm_cid_update_pcpu_cid(mm, cpu_cid);
	mm_cid_update_task_cid(t, cpu_cid);
}

static __always_inline void mm_cid_from_task(struct task_struct *t, unsigned int cpu_cid,
					     unsigned int mode)
{
	unsigned int max_cids, tcid = t->mm_cid.cid;
	struct mm_struct *mm = t->mm;

	max_cids = READ_ONCE(mm->mm_cid.max_cids);
	/* Optimize for the common case, where both have the ONCPU bit clear */
	if (likely(cid_on_task(tcid | cpu_cid))) {
		if (likely(tcid < max_cids)) {
			mm_cid_update_pcpu_cid(mm, tcid);
			return;
		}
		/* Try to converge into the optimal CID space */
		tcid = mm_cid_converge(mm, tcid, max_cids);
	} else {
		/* Hand over or drop the CPU owned CID */
		if (cid_on_cpu(cpu_cid)) {
			if (cid_on_task(tcid))
				mm_drop_cid_on_cpu(mm, this_cpu_ptr(mm->mm_cid.pcpu));
			else
				tcid = cpu_cid_to_cid(cpu_cid);
		}
		/* Still nothing, allocate a new one */
		if (!cid_on_task(tcid))
			tcid = mm_get_cid(mm);
		/* Set the transition mode flag if required */
		tcid |= mode & MM_CID_TRANSIT;
	}
	mm_cid_update_pcpu_cid(mm, tcid);
	mm_cid_update_task_cid(t, tcid);
}

static __always_inline void mm_cid_schedin(struct task_struct *next)
{
	struct mm_struct *mm = next->mm;
	unsigned int cpu_cid, mode;

	if (!next->mm_cid.active)
		return;

	cpu_cid = __this_cpu_read(mm->mm_cid.pcpu->cid);
	mode = READ_ONCE(mm->mm_cid.mode);
	if (likely(!cid_on_cpu(mode)))
		mm_cid_from_task(next, cpu_cid, mode);
	else
		mm_cid_from_cpu(next, cpu_cid, mode);
}

static __always_inline void mm_cid_schedout(struct task_struct *prev)
{
	/* During mode transitions CIDs are temporary and need to be dropped */
	if (likely(!cid_in_transit(prev->mm_cid.cid)))
		return;

	mm_drop_cid(prev->mm, cid_from_transit_cid(prev->mm_cid.cid));
	prev->mm_cid.cid = MM_CID_UNSET;
}

static inline void mm_cid_switch_to(struct task_struct *prev, struct task_struct *next)
{
	mm_cid_schedout(prev);
	mm_cid_schedin(next);
}

#else /* !CONFIG_SCHED_MM_CID: */
static inline void mm_cid_switch_to(struct task_struct *prev, struct task_struct *next) { }
#endif /* !CONFIG_SCHED_MM_CID */

extern struct balance_callback balance_push_callback;

static inline void
queue_balance_callback(struct rq *rq,
		       struct balance_callback *head,
		       void (*func)(struct rq *rq))
{
	lockdep_assert_rq_held(rq);

	/*
	 * Don't (re)queue an already queued item; nor queue anything when
	 * balance_push() is active, see the comment with
	 * balance_push_callback.
	 */
	if (unlikely(head->next || rq->balance_callback == &balance_push_callback))
		return;

	head->func = func;
	head->next = rq->balance_callback;
	rq->balance_callback = head;
}

/*
 * The 'sched_change' pattern is the safe, easy and slow way of changing a
 * task's scheduling properties. It dequeues a task, such that the scheduler
 * is fully unaware of it; at which point its properties can be modified;
 * after which it is enqueued again.
 *
 * Typically this must be called while holding task_rq_lock, since most/all
 * properties are serialized under those locks. There is currently one
 * exception to this rule in sched/ext which only holds rq->lock.
 */

/*
 * This structure is a temporary, used to preserve/convey the queueing state
 * of the task between sched_change_begin() and sched_change_end(). Ensuring
 * the task's queueing state is idempotent across the operation.
 */
struct sched_change_ctx {
	u64			prio;
	struct task_struct	*p;
	int			flags;
	bool			queued;
	bool			running;
};

struct sched_change_ctx *sched_change_begin(struct task_struct *p, unsigned int flags);
void sched_change_end(struct sched_change_ctx *ctx);

DEFINE_CLASS(sched_change, struct sched_change_ctx *,
	     sched_change_end(_T),
	     sched_change_begin(p, flags),
	     struct task_struct *p, unsigned int flags)

DEFINE_CLASS_IS_UNCONDITIONAL(sched_change)

#ifdef CONFIG_SCHED_BMQ
#include "bmq.h"
#endif
#ifdef CONFIG_SCHED_PDS
#include "pds.h"
#endif

#endif /* _KERNEL_SCHED_ALT_SCHED_H */
