/* SPDX-License-Identifier: GPL-2.0 */
#ifndef MUQSS_SCHED_H
#define MUQSS_SCHED_H

#include <linux/sched/clock.h>
#include <linux/sched/cpufreq.h>
#include <linux/sched/cputime.h>
#include <linux/sched/debug.h>
#include <linux/sched/hotplug.h>
#include <linux/sched/init.h>
#include <linux/sched/isolation.h>
#include <linux/sched/mm.h>
#include <linux/sched/nohz.h>
#include <linux/sched/signal.h>
#include <linux/sched/smt.h>
#include <linux/sched/stat.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/sched/topology.h>
#include <linux/sched/wake_q.h>

#include <uapi/linux/sched/types.h>

#include <linux/cgroup.h>
#include <linux/cpufreq.h>
#include <linux/cpuidle.h>
#include <linux/cpuset.h>
#include <linux/ctype.h>
#include <linux/energy_model.h>
#include <linux/freezer.h>
#include <linux/interrupt.h>
#include <linux/kernel_stat.h>
#include <linux/kthread.h>
#include <linux/membarrier.h>
#include <linux/livepatch.h>
#include <linux/proc_fs.h>
#include <linux/psi.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/skip_list.h>
#include <linux/stop_machine.h>
#include <linux/suspend.h>
#include <linux/swait.h>
#include <linux/syscalls.h>
#include <linux/tick.h>
#include <linux/tsacct_kern.h>
#include <linux/u64_stats_sync.h>

#ifdef CONFIG_PARAVIRT
#include <asm/paravirt.h>
#endif

#include "cpupri.h"

#ifdef CONFIG_SCHED_DEBUG
# define SCHED_WARN_ON(x)	WARN_ONCE(x, #x)
#else
# define SCHED_WARN_ON(x)	((void)(x))
#endif

/* task_struct::on_rq states: */
#define TASK_ON_RQ_QUEUED	1
#define TASK_ON_RQ_MIGRATING	2

struct rq;

#ifdef CONFIG_SMP

static inline bool sched_asym_prefer(int a, int b)
{
	return arch_asym_cpu_priority(a) > arch_asym_cpu_priority(b);
}

struct perf_domain {
	struct em_perf_domain *em_pd;
	struct perf_domain *next;
	struct rcu_head rcu;
};

/* Scheduling group status flags */
#define SG_OVERLOAD		0x1 /* More than one runnable task on a CPU. */
#define SG_OVERUTILIZED		0x2 /* One or more CPUs are over-utilized. */

/*
 * We add the notion of a root-domain which will be used to define per-domain
 * variables. Each exclusive cpuset essentially defines an island domain by
 * fully partitioning the member cpus from any other cpuset. Whenever a new
 * exclusive cpuset is created, we also create and attach a new root-domain
 * object.
 *
 */
struct root_domain {
	atomic_t refcount;
	atomic_t rto_count;
	struct rcu_head rcu;
	cpumask_var_t span;
	cpumask_var_t online;

	/*
	 * Indicate pullable load on at least one CPU, e.g:
	 * - More than one runnable task
	 * - Running task is misfit
	 */
	int			overload;

	/* Indicate one or more cpus over-utilized (tipping point) */
	int			overutilized;

	/*
	 * The bit corresponding to a CPU gets set here if such CPU has more
	 * than one runnable -deadline task (as it is below for RT tasks).
	 */
	cpumask_var_t dlo_mask;
	atomic_t dlo_count;
	/* Replace unused CFS structures with void */
	//struct dl_bw dl_bw;
	//struct cpudl cpudl;
	void *dl_bw;
	void *cpudl;

	/*
	 * The "RT overload" flag: it gets set if a CPU has more than
	 * one runnable RT task.
	 */
	cpumask_var_t rto_mask;
	//struct cpupri cpupri;
	void *cpupri;

	unsigned long max_cpu_capacity;

	/*
	 * NULL-terminated list of performance domains intersecting with the
	 * CPUs of the rd. Protected by RCU.
	 */
	struct perf_domain	*pd;
};

extern struct root_domain def_root_domain;
extern struct mutex sched_domains_mutex;

extern void init_defrootdomain(void);
extern int sched_init_domains(const struct cpumask *cpu_map);
extern void rq_attach_root(struct rq *rq, struct root_domain *rd);

static inline void cpupri_cleanup(void __maybe_unused *cpupri)
{
}

static inline void cpudl_cleanup(void __maybe_unused *cpudl)
{
}

static inline void init_dl_bw(void __maybe_unused *dl_bw)
{
}

static inline int cpudl_init(void __maybe_unused *dl_bw)
{
	return 0;
}

static inline int cpupri_init(void __maybe_unused *cpupri)
{
	return 0;
}
#endif /* CONFIG_SMP */

/*
 * This is the main, per-CPU runqueue data structure.
 * This data should only be modified by the local cpu.
 */
struct rq {
	raw_spinlock_t *lock;
	raw_spinlock_t *orig_lock;

	struct task_struct *curr, *idle, *stop;
	struct mm_struct *prev_mm;

	unsigned int nr_running;
	/*
	 * This is part of a global counter where only the total sum
	 * over all CPUs matters. A task can increase this counter on
	 * one CPU and if it got migrated afterwards it may decrease
	 * it on another CPU. Always updated under the runqueue lock:
	 */
	unsigned long nr_uninterruptible;
	u64 nr_switches;

	/* Stored data about rq->curr to work outside rq lock */
	u64 rq_deadline;
	int rq_prio;

	/* Best queued id for use outside lock */
	u64 best_key;

	unsigned long last_scheduler_tick; /* Last jiffy this RQ ticked */
	unsigned long last_jiffy; /* Last jiffy this RQ updated rq clock */
	u64 niffies; /* Last time this RQ updated rq clock */
	u64 last_niffy; /* Last niffies as updated by local clock */
	u64 last_jiffy_niffies; /* Niffies @ last_jiffy */

	u64 load_update; /* When we last updated load */
	unsigned long load_avg; /* Rolling load average */
#ifdef CONFIG_HAVE_SCHED_AVG_IRQ
	u64 irq_load_update; /* When we last updated IRQ load */
	unsigned long irq_load_avg; /* Rolling IRQ load average */
#endif
#ifdef CONFIG_SMT_NICE
	struct mm_struct *rq_mm;
	int rq_smt_bias; /* Policy/nice level bias across smt siblings */
#endif
	/* Accurate timekeeping data */
	unsigned long user_ns, nice_ns, irq_ns, softirq_ns, system_ns,
		iowait_ns, idle_ns;
	atomic_t nr_iowait;

	skiplist_node *node;
	skiplist *sl;
#ifdef CONFIG_SMP
	struct task_struct *preempt; /* Preempt triggered on this task */
	struct task_struct *preempting; /* Hint only, what task is preempting */

	int cpu;		/* cpu of this runqueue */
	bool online;

	struct root_domain *rd;
	struct sched_domain *sd;

	unsigned long cpu_capacity_orig;

	int *cpu_locality; /* CPU relative cache distance */
	struct rq **rq_order; /* Shared RQs ordered by relative cache distance */
	struct rq **cpu_order; /* RQs of discrete CPUs ordered by distance */

	struct rq *smp_leader; /* First physical CPU per node */
#ifdef CONFIG_SCHED_SMT
	struct rq *smt_leader; /* First logical CPU in SMT siblings */
	cpumask_t thread_mask;
	bool (*siblings_idle)(struct rq *rq);
	/* See if all smt siblings are idle */
#endif /* CONFIG_SCHED_SMT */
#ifdef CONFIG_SCHED_MC
	struct rq *mc_leader; /* First logical CPU in MC siblings */
	cpumask_t core_mask;
	bool (*cache_idle)(struct rq *rq);
	/* See if all cache siblings are idle */
#endif /* CONFIG_SCHED_MC */
#endif /* CONFIG_SMP */
#ifdef CONFIG_IRQ_TIME_ACCOUNTING
	u64 prev_irq_time;
#endif /* CONFIG_IRQ_TIME_ACCOUNTING */
#ifdef CONFIG_PARAVIRT
	u64 prev_steal_time;
#endif /* CONFIG_PARAVIRT */
#ifdef CONFIG_PARAVIRT_TIME_ACCOUNTING
	u64 prev_steal_time_rq;
#endif /* CONFIG_PARAVIRT_TIME_ACCOUNTING */

	u64 clock, old_clock, last_tick;
	/* Ensure that all clocks are in the same cache line */
	u64 clock_task ____cacheline_aligned;
	int dither;

	int iso_ticks;
	bool iso_refractory;

#ifdef CONFIG_HIGH_RES_TIMERS
	struct hrtimer hrexpiry_timer;
#endif

	int rt_nr_running; /* Number real time tasks running */
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

#ifdef CONFIG_SMP
	struct llist_head wake_list;
#endif

#ifdef CONFIG_CPU_IDLE
	/* Must be inspected within a rcu lock section */
	struct cpuidle_state *idle_state;
#endif
};

struct rq_flags {
	unsigned long flags;
};

#ifdef CONFIG_SMP
struct rq *cpu_rq(int cpu);
#endif

#ifndef CONFIG_SMP
extern struct rq *uprq;
#define cpu_rq(cpu)	(uprq)
#define this_rq()	(uprq)
#define raw_rq()	(uprq)
#define task_rq(p)	(uprq)
#define cpu_curr(cpu)	((uprq)->curr)
#else /* CONFIG_SMP */
DECLARE_PER_CPU_SHARED_ALIGNED(struct rq, runqueues);
#define this_rq()		this_cpu_ptr(&runqueues)
#define raw_rq()		raw_cpu_ptr(&runqueues)
#define task_rq(p)		cpu_rq(task_cpu(p))
#endif /* CONFIG_SMP */

static inline int task_current(struct rq *rq, struct task_struct *p)
{
	return rq->curr == p;
}

static inline int task_running(struct rq *rq, struct task_struct *p)
{
#ifdef CONFIG_SMP
	return p->on_cpu;
#else
	return task_current(rq, p);
#endif
}

static inline int task_on_rq_queued(struct task_struct *p)
{
	return p->on_rq == TASK_ON_RQ_QUEUED;
}

static inline int task_on_rq_migrating(struct task_struct *p)
{
	return READ_ONCE(p->on_rq) == TASK_ON_RQ_MIGRATING;
}

static inline void rq_lock(struct rq *rq)
	__acquires(rq->lock)
{
	raw_spin_lock(rq->lock);
}

static inline void rq_unlock(struct rq *rq)
	__releases(rq->lock)
{
	raw_spin_unlock(rq->lock);
}

static inline void rq_lock_irq(struct rq *rq)
	__acquires(rq->lock)
{
	raw_spin_lock_irq(rq->lock);
}

static inline void rq_unlock_irq(struct rq *rq, struct rq_flags __always_unused *rf)
	__releases(rq->lock)
{
	raw_spin_unlock_irq(rq->lock);
}

static inline void rq_lock_irqsave(struct rq *rq, struct rq_flags *rf)
	__acquires(rq->lock)
{
	raw_spin_lock_irqsave(rq->lock, rf->flags);
}

static inline void rq_unlock_irqrestore(struct rq *rq, struct rq_flags *rf)
	__releases(rq->lock)
{
	raw_spin_unlock_irqrestore(rq->lock, rf->flags);
}

static inline struct rq *task_rq_lock(struct task_struct *p, struct rq_flags *rf)
	__acquires(p->pi_lock)
	__acquires(rq->lock)
{
	struct rq *rq;

	while (42) {
		raw_spin_lock_irqsave(&p->pi_lock, rf->flags);
		rq = task_rq(p);
		raw_spin_lock(rq->lock);
		if (likely(rq == task_rq(p)))
			break;
		raw_spin_unlock(rq->lock);
		raw_spin_unlock_irqrestore(&p->pi_lock, rf->flags);
	}
	return rq;
}

static inline void task_rq_unlock(struct rq *rq, struct task_struct *p, struct rq_flags *rf)
	__releases(rq->lock)
	__releases(p->pi_lock)
{
	rq_unlock(rq);
	raw_spin_unlock_irqrestore(&p->pi_lock, rf->flags);
}

static inline struct rq *__task_rq_lock(struct task_struct *p, struct rq_flags __always_unused *rf)
	__acquires(rq->lock)
{
	struct rq *rq;

	lockdep_assert_held(&p->pi_lock);

	while (42) {
		rq = task_rq(p);
		raw_spin_lock(rq->lock);
		if (likely(rq == task_rq(p)))
			break;
		raw_spin_unlock(rq->lock);
	}
	return rq;
}

static inline void __task_rq_unlock(struct rq *rq, struct rq_flags __always_unused *rf)
{
	rq_unlock(rq);
}

static inline struct rq *
this_rq_lock_irq(struct rq_flags *rf)
	__acquires(rq->lock)
{
	struct rq *rq;

	local_irq_disable();
	rq = this_rq();
	rq_lock(rq);
	return rq;
}

/*
 * {de,en}queue flags: Most not used on MuQSS.
 *
 * DEQUEUE_SLEEP  - task is no longer runnable
 * ENQUEUE_WAKEUP - task just became runnable
 *
 * SAVE/RESTORE - an otherwise spurious dequeue/enqueue, done to ensure tasks
 *                are in a known state which allows modification. Such pairs
 *                should preserve as much state as possible.
 *
 * MOVE - paired with SAVE/RESTORE, explicitly does not preserve the location
 *        in the runqueue.
 *
 * ENQUEUE_HEAD      - place at front of runqueue (tail if not specified)
 * ENQUEUE_REPLENISH - CBS (replenish runtime and postpone deadline)
 * ENQUEUE_MIGRATED  - the task was migrated during wakeup
 *
 */

#define DEQUEUE_SLEEP		0x01
#define DEQUEUE_SAVE		0x02 /* matches ENQUEUE_RESTORE */

#define ENQUEUE_WAKEUP		0x01
#define ENQUEUE_RESTORE		0x02

#ifdef CONFIG_SMP
#define ENQUEUE_MIGRATED	0x40
#else
#define ENQUEUE_MIGRATED	0x00
#endif

static inline u64 __rq_clock_broken(struct rq *rq)
{
	return READ_ONCE(rq->clock);
}

static inline u64 rq_clock(struct rq *rq)
{
	lockdep_assert_held(rq->lock);

	return rq->clock;
}

static inline u64 rq_clock_task(struct rq *rq)
{
	lockdep_assert_held(rq->lock);

	return rq->clock_task;
}

#ifdef CONFIG_NUMA
enum numa_topology_type {
	NUMA_DIRECT,
	NUMA_GLUELESS_MESH,
	NUMA_BACKPLANE,
};
extern enum numa_topology_type sched_numa_topology_type;
extern int sched_max_numa_distance;
extern bool find_numa_distance(int distance);

extern void sched_init_numa(void);
extern void sched_domains_numa_masks_set(unsigned int cpu);
extern void sched_domains_numa_masks_clear(unsigned int cpu);
#else
static inline void sched_init_numa(void) { }
static inline void sched_domains_numa_masks_set(unsigned int cpu) { }
static inline void sched_domains_numa_masks_clear(unsigned int cpu) { }
#endif

extern struct mutex sched_domains_mutex;
extern struct static_key_false sched_schedstats;

#define rcu_dereference_check_sched_domain(p) \
	rcu_dereference_check((p), \
			      lockdep_is_held(&sched_domains_mutex))

#ifdef CONFIG_SMP

/*
 * The domain tree (rq->sd) is protected by RCU's quiescent state transition.
 * See destroy_sched_domains: call_rcu for details.
 *
 * The domain tree of any CPU may only be accessed from within
 * preempt-disabled sections.
 */
#define for_each_domain(cpu, __sd) \
	for (__sd = rcu_dereference_check_sched_domain(cpu_rq(cpu)->sd); \
			__sd; __sd = __sd->parent)

#define for_each_lower_domain(sd) for (; sd; sd = sd->child)

/**
 * highest_flag_domain - Return highest sched_domain containing flag.
 * @cpu:	The cpu whose highest level of sched domain is to
 *		be returned.
 * @flag:	The flag to check for the highest sched_domain
 *		for the given cpu.
 *
 * Returns the highest sched_domain of a cpu which contains the given flag.
 */
static inline struct sched_domain *highest_flag_domain(int cpu, int flag)
{
	struct sched_domain *sd, *hsd = NULL;

	for_each_domain(cpu, sd) {
		if (!(sd->flags & flag))
			break;
		hsd = sd;
	}

	return hsd;
}

static inline struct sched_domain *lowest_flag_domain(int cpu, int flag)
{
	struct sched_domain *sd;

	for_each_domain(cpu, sd) {
		if (sd->flags & flag)
			break;
	}

	return sd;
}

DECLARE_PER_CPU(struct sched_domain *, sd_llc);
DECLARE_PER_CPU(int, sd_llc_size);
DECLARE_PER_CPU(int, sd_llc_id);
DECLARE_PER_CPU(struct sched_domain_shared *, sd_llc_shared);
DECLARE_PER_CPU(struct sched_domain *, sd_numa);
DECLARE_PER_CPU(struct sched_domain *, sd_asym_packing);
DECLARE_PER_CPU(struct sched_domain *, sd_asym_cpucapacity);

struct sched_group_capacity {
	atomic_t ref;
	/*
	 * CPU capacity of this group, SCHED_CAPACITY_SCALE being max capacity
	 * for a single CPU.
	 */
	unsigned long		capacity;
	unsigned long		min_capacity;		/* Min per-CPU capacity in group */
	unsigned long		max_capacity;		/* Max per-CPU capacity in group */
	unsigned long		next_update;
	int			imbalance;		/* XXX unrelated to capacity but shared group state */

#ifdef CONFIG_SCHED_DEBUG
	int id;
#endif

	unsigned long cpumask[0]; /* balance mask */
};

struct sched_group {
	struct sched_group *next;	/* Must be a circular list */
	atomic_t ref;

	unsigned int group_weight;
	struct sched_group_capacity *sgc;
	int asym_prefer_cpu;		/* cpu of highest priority in group */

	/*
	 * The CPUs this group covers.
	 *
	 * NOTE: this field is variable length. (Allocated dynamically
	 * by attaching extra space to the end of the structure,
	 * depending on how many CPUs the kernel has booted up with)
	 */
	unsigned long cpumask[0];
};

static inline struct cpumask *sched_group_span(struct sched_group *sg)
{
	return to_cpumask(sg->cpumask);
}

/*
 * See build_balance_mask().
 */
static inline struct cpumask *group_balance_mask(struct sched_group *sg)
{
	return to_cpumask(sg->sgc->cpumask);
}

/**
 * group_first_cpu - Returns the first cpu in the cpumask of a sched_group.
 * @group: The group whose first cpu is to be returned.
 */
static inline unsigned int group_first_cpu(struct sched_group *group)
{
	return cpumask_first(sched_group_span(group));
}


#if defined(CONFIG_SCHED_DEBUG) && defined(CONFIG_SYSCTL)
void register_sched_domain_sysctl(void);
void dirty_sched_domain_sysctl(int cpu);
void unregister_sched_domain_sysctl(void);
#else
static inline void register_sched_domain_sysctl(void)
{
}
static inline void dirty_sched_domain_sysctl(int cpu)
{
}
static inline void unregister_sched_domain_sysctl(void)
{
}
#endif

extern void sched_ttwu_pending(void);
extern void set_cpus_allowed_common(struct task_struct *p, const struct cpumask *new_mask);
extern void set_rq_online (struct rq *rq);
extern void set_rq_offline(struct rq *rq);
extern bool sched_smp_initialized;

static inline void update_group_capacity(struct sched_domain *sd, int cpu)
{
}

static inline void trigger_load_balance(struct rq *rq)
{
}

#define sched_feat(x) 0

#else /* CONFIG_SMP */

static inline void sched_ttwu_pending(void) { }

#endif /* CONFIG_SMP */

#ifdef CONFIG_CPU_IDLE
static inline void idle_set_state(struct rq *rq,
				  struct cpuidle_state *idle_state)
{
	rq->idle_state = idle_state;
}

static inline struct cpuidle_state *idle_get_state(struct rq *rq)
{
	SCHED_WARN_ON(!rcu_read_lock_held());
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

#ifdef CONFIG_SCHED_DEBUG
extern bool sched_debug_enabled;
#endif

extern void schedule_idle(void);

#ifdef CONFIG_IRQ_TIME_ACCOUNTING
struct irqtime {
	u64			total;
	u64			tick_delta;
	u64			irq_start_time;
	struct u64_stats_sync	sync;
};

DECLARE_PER_CPU(struct irqtime, cpu_irqtime);

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
#endif /* CONFIG_IRQ_TIME_ACCOUNTING */

#ifdef CONFIG_SMP
static inline int cpu_of(struct rq *rq)
{
	return rq->cpu;
}
#else /* CONFIG_SMP */
static inline int cpu_of(struct rq *rq)
{
	return 0;
}
#endif

#ifdef CONFIG_CPU_FREQ
DECLARE_PER_CPU(struct update_util_data *, cpufreq_update_util_data);

static inline void cpufreq_trigger(struct rq *rq, unsigned int flags)
{
	struct update_util_data *data;

	data = rcu_dereference_sched(*per_cpu_ptr(&cpufreq_update_util_data,
						  cpu_of(rq)));

	if (data)
		data->func(data, rq->niffies, flags);
}
#else
static inline void cpufreq_trigger(struct rq *rq, unsigned int flag)
{
}
#endif /* CONFIG_CPU_FREQ */

#ifdef arch_scale_freq_capacity
#ifndef arch_scale_freq_invariant
#define arch_scale_freq_invariant()	(true)
#endif
#else /* arch_scale_freq_capacity */
#define arch_scale_freq_invariant()	(false)
#endif

/*
 * This should only be called when current == rq->idle. Dodgy workaround for
 * when softirqs are pending and we are in the idle loop. Setting current to
 * resched will kick us out of the idle loop and the softirqs will be serviced
 * on our next pass through schedule().
 */
static inline bool softirq_pending(int cpu)
{
	if (likely(!local_softirq_pending()))
		return false;
	set_tsk_need_resched(current);
	return true;
}

#ifdef CONFIG_64BIT
static inline u64 read_sum_exec_runtime(struct task_struct *t)
{
	return tsk_seruntime(t);
}
#else
static inline u64 read_sum_exec_runtime(struct task_struct *t)
{
	struct rq_flags rf;
	u64 ns;
	struct rq *rq;

	rq = task_rq_lock(t, &rf);
	ns = tsk_seruntime(t);
	task_rq_unlock(rq, t, &rf);

	return ns;
}
#endif

#ifndef arch_scale_freq_capacity
static __always_inline
unsigned long arch_scale_freq_capacity(int cpu)
{
	return SCHED_CAPACITY_SCALE;
}
#endif

#ifdef CONFIG_NO_HZ_FULL
extern bool sched_can_stop_tick(struct rq *rq);
extern int __init sched_tick_offload_init(void);

/*
 * Tick may be needed by tasks in the runqueue depending on their policy and
 * requirements. If tick is needed, lets send the target an IPI to kick it out of
 * nohz mode if necessary.
 */
static inline void sched_update_tick_dependency(struct rq *rq)
{
	int cpu;

	if (!tick_nohz_full_enabled())
		return;

	cpu = cpu_of(rq);

	if (!tick_nohz_full_cpu(cpu))
		return;

	if (sched_can_stop_tick(rq))
		tick_nohz_dep_clear_cpu(cpu, TICK_DEP_BIT_SCHED);
	else
		tick_nohz_dep_set_cpu(cpu, TICK_DEP_BIT_SCHED);
}
#else
static inline int sched_tick_offload_init(void) { return 0; }
static inline void sched_update_tick_dependency(struct rq *rq) { }
#endif

#define SCHED_FLAG_SUGOV	0x10000000

static inline bool rt_rq_is_runnable(struct rq *rt_rq)
{
	return rt_rq->rt_nr_running;
}

#ifdef CONFIG_CPU_FREQ_GOV_SCHEDUTIL
/**
 * enum schedutil_type - CPU utilization type
 * @FREQUENCY_UTIL:	Utilization used to select frequency
 * @ENERGY_UTIL:	Utilization used during energy calculation
 *
 * The utilization signals of all scheduling classes (CFS/RT/DL) and IRQ time
 * need to be aggregated differently depending on the usage made of them. This
 * enum is used within schedutil_freq_util() to differentiate the types of
 * utilization expected by the callers, and adjust the aggregation accordingly.
 */
enum schedutil_type {
	FREQUENCY_UTIL,
	ENERGY_UTIL,
};

unsigned long schedutil_freq_util(int cpu, unsigned long util_cfs,
				  unsigned long max, enum schedutil_type type);

static inline unsigned long schedutil_energy_util(int cpu, unsigned long cfs)
{
	unsigned long max = arch_scale_cpu_capacity(NULL, cpu);

	return schedutil_freq_util(cpu, cfs, max, ENERGY_UTIL);
}

static inline unsigned long cpu_bw_dl(struct rq *rq)
{
	return 0;
}

static inline unsigned long cpu_util_dl(struct rq *rq)
{
	return 0;
}

static inline unsigned long cpu_util_cfs(struct rq *rq)
{
	unsigned long ret = READ_ONCE(rq->load_avg);

	if (ret > SCHED_CAPACITY_SCALE)
		ret = SCHED_CAPACITY_SCALE;
	return ret;
}

static inline unsigned long cpu_util_rt(struct rq *rq)
{
	unsigned long ret = READ_ONCE(rq->rt_nr_running);

	if (ret > SCHED_CAPACITY_SCALE)
		ret = SCHED_CAPACITY_SCALE;
	return ret;
}

#ifdef CONFIG_HAVE_SCHED_AVG_IRQ
static inline unsigned long cpu_util_irq(struct rq *rq)
{
	unsigned long ret = READ_ONCE(rq->irq_load_avg);

	if (ret > SCHED_CAPACITY_SCALE)
		ret = SCHED_CAPACITY_SCALE;
	return ret;
}

static inline
unsigned long scale_irq_capacity(unsigned long util, unsigned long irq, unsigned long max)
{
	util *= (max - irq);
	util /= max;

	return util;

}
#else
static inline unsigned long cpu_util_irq(struct rq *rq)
{
	return 0;
}

static inline
unsigned long scale_irq_capacity(unsigned long util, unsigned long irq, unsigned long max)
{
	return util;
}
#endif
#endif

#if defined(CONFIG_ENERGY_MODEL) && defined(CONFIG_CPU_FREQ_GOV_SCHEDUTIL)
#define perf_domain_span(pd) (to_cpumask(((pd)->em_pd->cpus)))

DECLARE_STATIC_KEY_FALSE(sched_energy_present);

static inline bool sched_energy_enabled(void)
{
	return static_branch_unlikely(&sched_energy_present);
}

#else /* ! (CONFIG_ENERGY_MODEL && CONFIG_CPU_FREQ_GOV_SCHEDUTIL) */

#define perf_domain_span(pd) NULL
static inline bool sched_energy_enabled(void) { return false; }

#endif /* CONFIG_ENERGY_MODEL && CONFIG_CPU_FREQ_GOV_SCHEDUTIL */
#endif /* MUQSS_SCHED_H */
