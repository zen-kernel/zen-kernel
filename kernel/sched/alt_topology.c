#include "alt_core.h"
#include "alt_topology.h"

static cpumask_t sched_pcore_mask ____cacheline_aligned_in_smp;

static int __init sched_pcore_mask_setup(char *str)
{
	if (cpulist_parse(str, &sched_pcore_mask))
		pr_warn("sched/alt: pcore_cpus= incorrect CPU range\n");

	return 0;
}
__setup("pcore_cpus=", sched_pcore_mask_setup);

DEFINE_PER_CPU_READ_MOSTLY(enum cpu_topo_type, sched_cpu_topo);
DEFINE_PER_CPU_READ_MOSTLY(enum cpu_topo_balance_type, sched_cpu_topo_balance);
DEFINE_PER_CPU(struct balance_callback, active_balance_head);

/*
 * Idle cpu/rq selection functions
 */
#ifdef CONFIG_SCHED_SMT
static bool p1_idle_select_func(struct cpumask *dstp, const struct cpumask *src1p,
				 const struct cpumask *src2p)
{
	return cpumask_and(dstp, src1p, src2p + 1)	||
	       cpumask_and(dstp, src1p, src2p);
}
#endif

static bool p1p2_idle_select_func(struct cpumask *dstp, const struct cpumask *src1p,
					const struct cpumask *src2p)
{
	return cpumask_and(dstp, src1p, src2p + 1)	||
	       cpumask_and(dstp, src1p, src2p + 2)	||
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
void smt_pcore_balance(struct rq *rq)
{
	cpumask_t single_task_mask;

	if (cpumask_andnot(&single_task_mask, cpu_active_mask, sched_idle_mask) &&
	    cpumask_andnot(&single_task_mask, &single_task_mask, &sched_rq_pending_mask) &&
	    (/* smt core group balance */
	     (static_key_count(&sched_smt_present.key) > 1 &&
	      smt_pcore_source_balance(rq, &single_task_mask, sched_sg_idle_mask)
	     ) ||
	     /* e core to idle smt core balance */
	     ecore_source_balance(rq, &single_task_mask, sched_sg_idle_mask)))
		return;
}

/* smt balance functions */
void smt_balance(struct rq *rq)
{
	cpumask_t single_task_mask;

	if (cpumask_andnot(&single_task_mask, cpu_active_mask, sched_idle_mask) &&
	    cpumask_andnot(&single_task_mask, &single_task_mask, &sched_rq_pending_mask) &&
	    static_key_count(&sched_smt_present.key) > 1 &&
	    smt_pcore_source_balance(rq, &single_task_mask, sched_sg_idle_mask))
		return;
}

/* e core balance functions */
void ecore_balance(struct rq *rq)
{
	cpumask_t single_task_mask;

	if (cpumask_andnot(&single_task_mask, cpu_active_mask, sched_idle_mask) &&
	    cpumask_andnot(&single_task_mask, &single_task_mask, &sched_rq_pending_mask) &&
	    cpumask_empty(sched_pcore_idle_mask) &&
	    /* smt occupied p core to idle e core balance */
	    smt_pcore_source_balance(rq, &single_task_mask, sched_ecore_idle_mask))
		return;
}
#endif /* CONFIG_SCHED_SMT */

/* p core balance functions */
void pcore_balance(struct rq *rq)
{
	cpumask_t single_task_mask;

	if (cpumask_andnot(&single_task_mask, cpu_active_mask, sched_idle_mask) &&
	    cpumask_andnot(&single_task_mask, &single_task_mask, &sched_rq_pending_mask) &&
	    /* idle e core to p core balance */
	    ecore_source_balance(rq, &single_task_mask, sched_pcore_idle_mask))
		return;
}

#ifdef ALT_SCHED_DEBUG
#define SCHED_DEBUG_INFO(...)	printk(KERN_INFO __VA_ARGS__)
#else
#define SCHED_DEBUG_INFO(...)	do { } while(0)
#endif

#define IDLE_SELECT_FUNC_UPDATE(func)						\
{										\
	static_call_update(sched_idle_select_func, &func);			\
	printk(KERN_INFO "sched: idle select func -> "#func);			\
}

#define SET_SCHED_CPU_TOPOLOGY(cpu, topo)					\
{										\
	per_cpu(sched_cpu_topo, (cpu)) = topo;					\
	SCHED_DEBUG_INFO("sched: cpu#%02d -> "#topo, cpu);			\
}

#define SET_SCHED_CPU_TOPOLOGY_BALANCE(cpu, balance)				\
{										\
	per_cpu(sched_cpu_topo_balance, (cpu)) = balance;			\
	SCHED_DEBUG_INFO("sched: cpu#%02d -> "#balance, cpu);			\
}

void sched_init_topology(void)
{
	int cpu;
	struct rq *rq;
	cpumask_t sched_ecore_mask = { CPU_BITS_NONE };
	int ecore_present = 0;

#ifdef CONFIG_SCHED_SMT
	if (!cpumask_empty(&sched_smt_mask))
		printk(KERN_INFO "sched: smt mask: 0x%08lx\n", sched_smt_mask.bits[0]);
#endif

	if (!cpumask_empty(&sched_pcore_mask)) {
		cpumask_andnot(&sched_ecore_mask, cpu_online_mask, &sched_pcore_mask);
		printk(KERN_INFO "sched: pcore mask: 0x%08lx, ecore mask: 0x%08lx\n",
		       sched_pcore_mask.bits[0], sched_ecore_mask.bits[0]);

		ecore_present = !cpumask_empty(&sched_ecore_mask);
	}

	/* idle select function */
#ifdef CONFIG_SCHED_SMT
	if (cpumask_equal(&sched_smt_mask, cpu_online_mask)) {
		IDLE_SELECT_FUNC_UPDATE(p1_idle_select_func);
	} else
#endif
	if (!cpumask_empty(&sched_pcore_mask)) {
		IDLE_SELECT_FUNC_UPDATE(p1p2_idle_select_func);
	}

	/* CPU topology setup */
	for_each_online_cpu(cpu) {
		rq = cpu_rq(cpu);
		/* take chance to reset time slice for idle tasks */
		rq->idle->time_slice = sysctl_sched_base_slice;

#ifdef CONFIG_SCHED_SMT
		if (cpumask_weight(cpu_smt_mask(cpu)) > 1) {
			SET_SCHED_CPU_TOPOLOGY(cpu, CPU_TOPOLOGY_SMT);

			if (cpumask_test_cpu(cpu, &sched_pcore_mask) &&
			    !cpumask_intersects(&sched_ecore_mask, &sched_smt_mask)) {
				SET_SCHED_CPU_TOPOLOGY_BALANCE(cpu, CPU_TOPOLOGY_BALANCE_SMT_PCORE);
			} else {
				SET_SCHED_CPU_TOPOLOGY_BALANCE(cpu, CPU_TOPOLOGY_BALANCE_SMT);
			}

			continue;
		}
#endif
		/* !SMT or only one cpu in sg */
		if (cpumask_test_cpu(cpu, &sched_pcore_mask)) {
			SET_SCHED_CPU_TOPOLOGY(cpu, CPU_TOPOLOGY_PCORE);

			if (ecore_present)
				SET_SCHED_CPU_TOPOLOGY_BALANCE(cpu, CPU_TOPOLOGY_BALANCE_PCORE);

			continue;
		}

		if (cpumask_test_cpu(cpu, &sched_ecore_mask)) {
			SET_SCHED_CPU_TOPOLOGY(cpu, CPU_TOPOLOGY_ECORE);
#ifdef CONFIG_SCHED_SMT
			if (cpumask_intersects(&sched_pcore_mask, &sched_smt_mask))
				SET_SCHED_CPU_TOPOLOGY_BALANCE(cpu, CPU_TOPOLOGY_BALANCE_ECORE);
#endif
		}
	}
}
