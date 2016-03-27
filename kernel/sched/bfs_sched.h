#include <linux/sched.h>
#include <linux/cpuidle.h>
#include <linux/stop_machine.h>

#ifndef BFS_SCHED_H
#define BFS_SCHED_H

/*
 * This is the main, per-CPU runqueue data structure.
 * This data should only be modified by the local cpu.
 */
struct rq {
	struct task_struct *curr, *idle, *stop;
	struct mm_struct *prev_mm;

	/* Pointer to grq spinlock */
	raw_spinlock_t *grq_lock;

	/* Stored data about rq->curr to work outside grq lock */
	u64 rq_deadline;
	unsigned int rq_policy;
	int rq_time_slice;
	u64 rq_last_ran;
	int rq_prio;
	bool rq_running; /* There is a task running */
	int soft_affined; /* Running or queued tasks with this set as their rq */
#ifdef CONFIG_SMT_NICE
	struct mm_struct *rq_mm;
	int rq_smt_bias; /* Policy/nice level bias across smt siblings */
#endif
	/* Accurate timekeeping data */
	u64 timekeep_clock;
	unsigned long user_pc, nice_pc, irq_pc, softirq_pc, system_pc,
		iowait_pc, idle_pc;
	atomic_t nr_iowait;

#ifdef CONFIG_SMP
	int cpu;		/* cpu of this runqueue */
	bool online;
	bool scaling; /* This CPU is managed by a scaling CPU freq governor */
	struct task_struct *sticky_task;

	struct root_domain *rd;
	struct sched_domain *sd;
	int *cpu_locality; /* CPU relative cache distance */
#ifdef CONFIG_SCHED_SMT
	bool (*siblings_idle)(int cpu);
	/* See if all smt siblings are idle */
#endif /* CONFIG_SCHED_SMT */
#ifdef CONFIG_SCHED_MC
	bool (*cache_idle)(int cpu);
	/* See if all cache siblings are idle */
#endif /* CONFIG_SCHED_MC */
	u64 last_niffy; /* Last time this RQ updated grq.niffies */
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
	u64 clock_task;
	bool dither;

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
#endif /* CONFIG_SMP */

static inline u64 __rq_clock_broken(struct rq *rq)
{
	return READ_ONCE(rq->clock);
}

static inline u64 rq_clock(struct rq *rq)
{
	lockdep_assert_held(rq->grq_lock);
	return rq->clock;
}

static inline u64 rq_clock_task(struct rq *rq)
{
	lockdep_assert_held(rq->grq_lock);
	return rq->clock_task;
}

extern struct mutex sched_domains_mutex;

#define rcu_dereference_check_sched_domain(p) \
	rcu_dereference_check((p), \
			      lockdep_is_held(&sched_domains_mutex))

/*
 * The domain tree (rq->sd) is protected by RCU's quiescent state transition.
 * See detach_destroy_domains: synchronize_sched for details.
 *
 * The domain tree of any CPU may only be accessed from within
 * preempt-disabled sections.
 */
#define for_each_domain(cpu, __sd) \
	for (__sd = rcu_dereference_check_sched_domain(cpu_rq(cpu)->sd); __sd; __sd = __sd->parent)

static inline void sched_ttwu_pending(void) { }

static inline int task_on_rq_queued(struct task_struct *p)
{
	return p->on_rq;
}

#ifdef CONFIG_SMP

extern void set_cpus_allowed_common(struct task_struct *p, const struct cpumask *new_mask);

#endif

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
#endif /* BFS_SCHED_H */
