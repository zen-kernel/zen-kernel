#ifndef _KERNEL_SCHED_ALT_TOPOLOGY_H
#define _KERNEL_SCHED_ALT_TOPOLOGY_H

/*
 * CPU topology type
 */
enum cpu_topo_type {
	CPU_TOPOLOGY_DEFAULT = 0,
	CPU_TOPOLOGY_PCORE,
	CPU_TOPOLOGY_ECORE,
};

DECLARE_PER_CPU_READ_MOSTLY(enum cpu_topo_type, sched_cpu_topo);

#ifdef CONFIG_SCHED_SMT
static inline bool sched_smt_active_group_idle(const unsigned int cpu)
{
	unsigned int sibling;

	if (!cpumask_test_cpu(cpu, &sched_smt_mask))
		return false;

	for_each_cpu_and(sibling, cpu_smt_mask(cpu), &sched_smt_mask)
		if (!cpumask_test_cpu(sibling, sched_idle_mask))
			return false;

	return true;
}

static inline void sched_set_smt_idle_masks(const unsigned int cpu)
{
	enum cpu_topo_type class = per_cpu(sched_cpu_topo, cpu);
	unsigned int sibling;

	for_each_cpu_and(sibling, cpu_smt_mask(cpu), &sched_smt_mask) {
		cpumask_set_cpu(sibling, sched_sg_idle_mask);
		if (class == CPU_TOPOLOGY_PCORE)
			cpumask_set_cpu(sibling, sched_pcore_idle_mask);
	}
}

static inline void sched_clear_smt_idle_masks(const unsigned int cpu)
{
	enum cpu_topo_type class = per_cpu(sched_cpu_topo, cpu);
	unsigned int sibling;

	for_each_cpu_and(sibling, cpu_smt_mask(cpu), &sched_smt_mask) {
		cpumask_clear_cpu(sibling, sched_sg_idle_mask);
		if (class == CPU_TOPOLOGY_PCORE)
			cpumask_clear_cpu(sibling, sched_pcore_idle_mask);
	}
}
#endif

static inline void sched_set_idle_mask(const unsigned int cpu)
{
#ifdef CONFIG_SCHED_SMT
	if (cpumask_test_cpu(cpu, &sched_smt_mask)) {
		unsigned int leader =
			cpumask_first_and(cpu_smt_mask(cpu), &sched_smt_mask);
		raw_spinlock_t *lock = &per_cpu(sched_smt_idle_lock, leader);

		raw_spin_lock(lock);
		cpumask_set_cpu(cpu, sched_idle_mask);
		if (sched_smt_active_group_idle(cpu))
			sched_set_smt_idle_masks(cpu);
		raw_spin_unlock(lock);
		return;
	}
#endif

	cpumask_set_cpu(cpu, sched_idle_mask);

	switch (per_cpu(sched_cpu_topo, cpu)) {
	case CPU_TOPOLOGY_DEFAULT:
		break;
	case CPU_TOPOLOGY_PCORE:
		cpumask_set_cpu(cpu, sched_pcore_idle_mask);
		break;
	case CPU_TOPOLOGY_ECORE:
		cpumask_set_cpu(cpu, sched_ecore_idle_mask);
		break;
	}
}

static inline void sched_clear_idle_mask(const unsigned int cpu)
{
#ifdef CONFIG_SCHED_SMT
	if (cpumask_test_cpu(cpu, &sched_smt_mask)) {
		unsigned int leader =
			cpumask_first_and(cpu_smt_mask(cpu), &sched_smt_mask);
		raw_spinlock_t *lock = &per_cpu(sched_smt_idle_lock, leader);

		raw_spin_lock(lock);
		if (cpumask_test_cpu(cpu, sched_sg_idle_mask))
			sched_clear_smt_idle_masks(cpu);
		cpumask_clear_cpu(cpu, sched_idle_mask);
		raw_spin_unlock(lock);
		return;
	}
#endif

	cpumask_clear_cpu(cpu, sched_idle_mask);

	switch (per_cpu(sched_cpu_topo, cpu)) {
	case CPU_TOPOLOGY_DEFAULT:
		break;
	case CPU_TOPOLOGY_PCORE:
		cpumask_clear_cpu(cpu, sched_pcore_idle_mask);
		break;
	case CPU_TOPOLOGY_ECORE:
		cpumask_clear_cpu(cpu, sched_ecore_idle_mask);
		break;
	}
}

/*
 * CPU topology balance type
 */
enum cpu_topo_balance_type {
	CPU_TOPOLOGY_BALANCE_NONE = 0,
	CPU_TOPOLOGY_BALANCE_PCORE,
#ifdef CONFIG_SCHED_SMT
	CPU_TOPOLOGY_BALANCE_ECORE,
	CPU_TOPOLOGY_BALANCE_SMT,
	CPU_TOPOLOGY_BALANCE_SMT_PCORE,
#endif
};

DECLARE_PER_CPU_READ_MOSTLY(enum cpu_topo_balance_type, sched_cpu_topo_balance);
DECLARE_PER_CPU(struct balance_callback, active_balance_head);

extern void pcore_balance(struct rq *rq);
#ifdef CONFIG_SCHED_SMT
extern void ecore_balance(struct rq *rq);
extern void smt_balance(struct rq *rq);
extern void smt_pcore_balance(struct rq *rq);
#endif

static inline void sched_cpu_topology_balance(const unsigned int cpu, struct rq *rq)
{
	struct balance_callback *head = &per_cpu(active_balance_head, cpu);

	if (!rq->online)
		return;

	switch (per_cpu(sched_cpu_topo_balance, cpu)) {
	case CPU_TOPOLOGY_BALANCE_NONE:
		break;
	case CPU_TOPOLOGY_BALANCE_PCORE:
		queue_balance_callback(rq, head, pcore_balance);
		break;
#ifdef CONFIG_SCHED_SMT
	case CPU_TOPOLOGY_BALANCE_ECORE:
		queue_balance_callback(rq, head, ecore_balance);
		break;
	case CPU_TOPOLOGY_BALANCE_SMT:
		if (cpumask_test_cpu(cpu, sched_sg_idle_mask) &&
		    sched_smt_active_group_idle(cpu))
			queue_balance_callback(rq, head, smt_balance);
		break;
	case CPU_TOPOLOGY_BALANCE_SMT_PCORE:
		if (cpumask_test_cpu(cpu, sched_sg_idle_mask) &&
		    sched_smt_active_group_idle(cpu))
			queue_balance_callback(rq, head, smt_pcore_balance);
		break;
#endif
	}
}

void sched_topology_prepare(const struct cpumask *active_mask);
void sched_topology_apply(const struct cpumask *active_mask);
void sched_topology_report(void);
void sched_topology_init_idle_select(void);
void sched_topology_queue_idle_select(void);

#endif /* _KERNEL_SCHED_ALT_TOPOLOGY_H */
