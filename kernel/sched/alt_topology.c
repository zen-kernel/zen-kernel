#include "alt_core.h"
#include "alt_topology.h"

#ifdef CONFIG_SMP

static cpumask_t sched_pcore_mask ____cacheline_aligned_in_smp;

static int __init sched_pcore_mask_setup(char *str)
{
	if (cpulist_parse(str, &sched_pcore_mask))
		pr_warn("sched/alt: pcore_cpus= incorrect CPU range\n");

	return 0;
}
__setup("pcore_cpus=", sched_pcore_mask_setup);

/*
 * set/clear idle mask functions
 */
#ifdef CONFIG_SCHED_SMT
static void set_idle_mask_smt(unsigned int cpu, struct cpumask *dstp)
{
	cpumask_set_cpu(cpu, dstp);
	if (cpumask_subset(cpu_smt_mask(cpu), sched_idle_mask))
		cpumask_or(sched_pcore_idle_mask, sched_pcore_idle_mask, cpu_smt_mask(cpu));
}

static void clear_idle_mask_smt(int cpu, struct cpumask *dstp)
{
	cpumask_clear_cpu(cpu, dstp);
	cpumask_andnot(sched_pcore_idle_mask, sched_pcore_idle_mask, cpu_smt_mask(cpu));
}
#endif

static void set_idle_mask_pcore(unsigned int cpu, struct cpumask *dstp)
{
	cpumask_set_cpu(cpu, dstp);
	cpumask_set_cpu(cpu, sched_pcore_idle_mask);
}

static void clear_idle_mask_pcore(int cpu, struct cpumask *dstp)
{
	cpumask_clear_cpu(cpu, dstp);
	cpumask_clear_cpu(cpu, sched_pcore_idle_mask);
}

static void set_idle_mask_ecore(unsigned int cpu, struct cpumask *dstp)
{
	cpumask_set_cpu(cpu, dstp);
	cpumask_set_cpu(cpu, sched_ecore_idle_mask);
}

static void clear_idle_mask_ecore(int cpu, struct cpumask *dstp)
{
	cpumask_clear_cpu(cpu, dstp);
	cpumask_clear_cpu(cpu, sched_ecore_idle_mask);
}

/*
 * Idle cpu/rq selection functions
 */
static bool idle_select_func_smt(struct cpumask *dstp, const struct cpumask *src1p,
				 const struct cpumask *src2p)
{
	return cpumask_and(dstp, src1p, sched_pcore_idle_mask) || cpumask_and(dstp, src1p, src2p);
}

static bool idle_select_func_hybrid_smt(struct cpumask *dstp, const struct cpumask *src1p,
					const struct cpumask *src2p)
{
	return cpumask_and(dstp, src1p, sched_pcore_idle_mask) ||
	       cpumask_and(dstp, src1p, sched_ecore_idle_mask) ||
	       cpumask_and(dstp, src1p, src2p);
}

/* common balance functions */
static int active_balance_cpu_stop(void *data)
{
	struct balance_arg *arg = data;
	struct task_struct *p = arg->task;
	struct rq *rq = this_rq();
	unsigned long flags;
	cpumask_t tmp;

	local_irq_save(flags);

	raw_spin_lock(&p->pi_lock);
	raw_spin_lock(&rq->lock);

	arg->active = 0;

	if (task_on_rq_queued(p) && task_rq(p) == rq &&
	    cpumask_and(&tmp, p->cpus_ptr, arg->cpumask) &&
	    !is_migration_disabled(p)) {
		int dcpu = __best_mask_cpu(&tmp, per_cpu(sched_cpu_llc_mask, cpu_of(rq)));
		rq = move_queued_task(rq, p, dcpu);
	}

	raw_spin_unlock(&rq->lock);
	raw_spin_unlock_irqrestore(&p->pi_lock, flags);

	return 0;
}

/* trigger_active_balance - for @rq */
static inline int
trigger_active_balance(struct rq *src_rq, struct rq *rq, cpumask_t *target_mask)
{
	struct balance_arg *arg;
	unsigned long flags;
	struct task_struct *p;
	int res;

	if (!raw_spin_trylock_irqsave(&rq->lock, flags))
		return 0;

	arg = &rq->active_balance_arg;
	res = (1 == rq->nr_running) &&					\
	      !is_migration_disabled((p = sched_rq_first_task(rq))) &&	\
	      cpumask_intersects(p->cpus_ptr, target_mask) &&		\
	      !arg->active;
	if (res) {
		arg->task = p;
		arg->cpumask = target_mask;

		arg->active = 1;
	}

	raw_spin_unlock_irqrestore(&rq->lock, flags);

	if (res) {
		preempt_disable();
		raw_spin_unlock(&src_rq->lock);

		stop_one_cpu_nowait(cpu_of(rq), active_balance_cpu_stop, arg,
				    &rq->active_balance_work);

		preempt_enable();
		raw_spin_lock(&src_rq->lock);
	}

	return res;
}

static inline int
ecore_source_balance(struct rq *rq, cpumask_t *single_task_mask, cpumask_t *target_mask)
{
	if (cpumask_andnot(single_task_mask, single_task_mask, &sched_pcore_mask)) {
		int i, cpu = cpu_of(rq);

		for_each_cpu_wrap(i, single_task_mask, cpu)
			if (trigger_active_balance(rq, cpu_rq(i), target_mask))
				return 1;
	}

	return 0;
}

static DEFINE_PER_CPU(struct balance_callback, active_balance_head);

#ifdef CONFIG_SCHED_SMT
static inline int
smt_pcore_source_balance(struct rq *rq, cpumask_t *single_task_mask, cpumask_t *target_mask)
{
	cpumask_t smt_single_mask;

	if (cpumask_and(&smt_single_mask, single_task_mask, &sched_smt_mask)) {
		int i, cpu = cpu_of(rq);

		for_each_cpu_wrap(i, &smt_single_mask, cpu) {
			if (cpumask_subset(cpu_smt_mask(i), &smt_single_mask) &&
			    trigger_active_balance(rq, cpu_rq(i), target_mask))
				return 1;
		}
	}

	return 0;
}

/* smt p core balance functions */
static inline void smt_pcore_balance(struct rq *rq)
{
	cpumask_t single_task_mask;

	if (cpumask_andnot(&single_task_mask, cpu_active_mask, sched_idle_mask) &&
	    cpumask_andnot(&single_task_mask, &single_task_mask, &sched_rq_pending_mask) &&
	    (/* smt core group balance */
	     (static_key_count(&sched_smt_present.key) > 1 &&
	      smt_pcore_source_balance(rq, &single_task_mask, sched_pcore_idle_mask)
	     ) ||
	     /* e core to idle smt core balance */
	     ecore_source_balance(rq, &single_task_mask, sched_pcore_idle_mask)))
		return;
}

static void smt_pcore_balance_func(struct rq *rq, const int cpu)
{
	if (cpumask_test_cpu(cpu, sched_pcore_idle_mask))
		queue_balance_callback(rq, &per_cpu(active_balance_head, cpu), smt_pcore_balance);
}

/* smt balance functions */
static inline void smt_balance(struct rq *rq)
{
	cpumask_t single_task_mask;

	if (cpumask_andnot(&single_task_mask, cpu_active_mask, sched_idle_mask) &&
	    cpumask_andnot(&single_task_mask, &single_task_mask, &sched_rq_pending_mask) &&
	    static_key_count(&sched_smt_present.key) > 1 &&
	    smt_pcore_source_balance(rq, &single_task_mask, sched_pcore_idle_mask))
		return;
}

static void smt_balance_func(struct rq *rq, const int cpu)
{
	if (cpumask_test_cpu(cpu, sched_pcore_idle_mask))
		queue_balance_callback(rq, &per_cpu(active_balance_head, cpu), smt_balance);
}

/* e core balance functions */
static inline void ecore_balance(struct rq *rq)
{
	cpumask_t single_task_mask;

	if (cpumask_andnot(&single_task_mask, cpu_active_mask, sched_idle_mask) &&
	    cpumask_andnot(&single_task_mask, &single_task_mask, &sched_rq_pending_mask) &&
	    /* smt occupied p core to idle e core balance */
	    smt_pcore_source_balance(rq, &single_task_mask, sched_ecore_idle_mask))
		return;
}

static void ecore_balance_func(struct rq *rq, const int cpu)
{
	queue_balance_callback(rq, &per_cpu(active_balance_head, cpu), ecore_balance);
}
#endif /* CONFIG_SCHED_SMT */

/* p core balance functions */
static inline void pcore_balance(struct rq *rq)
{
	cpumask_t single_task_mask;

	if (cpumask_andnot(&single_task_mask, cpu_active_mask, sched_idle_mask) &&
	    cpumask_andnot(&single_task_mask, &single_task_mask, &sched_rq_pending_mask) &&
	    /* idle e core to p core balance */
	    ecore_source_balance(rq, &single_task_mask, sched_pcore_idle_mask))
		return;
}

static void pcore_balance_func(struct rq *rq, const int cpu)
{
	queue_balance_callback(rq, &per_cpu(active_balance_head, cpu), pcore_balance);
}


#define SET_RQ_BALANCE_FUNC(rq, cpu, func)					\
{										\
	rq->balance_func = func;						\
	printk(KERN_INFO "sched: cpu#%02d -> "#func, cpu);			\
}

void sched_init_topology(void)
{
	int cpu;
	struct rq *rq;
	cpumask_t sched_ecore_mask = { CPU_BITS_NONE };
	int ecore_present;

#ifdef CONFIG_SCHED_SMT
	printk(KERN_INFO "sched: smt mask: 0x%08lx\n", sched_smt_mask.bits[0]);
	cpumask_or(&sched_pcore_mask, &sched_pcore_mask, &sched_smt_mask);
#endif

	if (!cpumask_empty(&sched_pcore_mask)) {
		cpumask_andnot(&sched_ecore_mask, cpu_online_mask, &sched_pcore_mask);
		printk(KERN_INFO "sched: pcore mask: 0x%08lx, ecore mask: 0x%08lx\n",
		       sched_pcore_mask.bits[0], sched_ecore_mask.bits[0]);

		ecore_present = !cpumask_empty(&sched_ecore_mask);
		idle_select_func = (ecore_present)? idle_select_func_hybrid_smt:idle_select_func_smt;
	}

	for_each_online_cpu(cpu) {
		rq = cpu_rq(cpu);
		/* take chance to reset time slice for idle tasks */
		rq->idle->time_slice = sysctl_sched_base_slice;

		if (cpumask_test_cpu(cpu, &sched_pcore_mask)) {
			rq->set_idle_mask_func = set_idle_mask_pcore;
			rq->clear_idle_mask_func = clear_idle_mask_pcore;

#ifdef CONFIG_SCHED_SMT
			if (cpumask_weight(cpu_smt_mask(cpu)) > 1) {
				rq->set_idle_mask_func = set_idle_mask_smt;
				rq->clear_idle_mask_func = clear_idle_mask_smt;

				if (ecore_present) {
					SET_RQ_BALANCE_FUNC(rq, cpu, smt_pcore_balance_func);
				} else {
					SET_RQ_BALANCE_FUNC(rq, cpu, smt_balance_func);
				}
			} else
#endif
			if (ecore_present)
				SET_RQ_BALANCE_FUNC(rq, cpu, pcore_balance_func);
		}
		if (cpumask_test_cpu(cpu, &sched_ecore_mask)) {
			rq->set_idle_mask_func = set_idle_mask_ecore;
			rq->clear_idle_mask_func = clear_idle_mask_ecore;
#ifdef CONFIG_SCHED_SMT
			if (static_branch_likely(&sched_smt_present))
				SET_RQ_BALANCE_FUNC(rq, cpu, ecore_balance_func);
#endif
		}
	}
}
#endif /* CONFIG_SMP */
