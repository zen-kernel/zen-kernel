#ifndef _KERNEL_SCHED_ALT_TOPOLOGY_H
#define _KERNEL_SCHED_ALT_TOPOLOGY_H

enum cpu_topo_type {
	CPU_TOPOLOGY_DEFAULT = 0,
	CPU_TOPOLOGY_PCORE,
	CPU_TOPOLOGY_ECORE,
#ifdef CONFIG_SCHED_SMT
	CPU_TOPOLOGY_SMT,
#endif
};

DECLARE_PER_CPU_READ_MOSTLY(enum cpu_topo_type, sched_cpu_topo);

static inline void sched_set_idle_mask(const unsigned int cpu)
{
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
#ifdef CONFIG_SCHED_SMT
	case CPU_TOPOLOGY_SMT:
		if (cpumask_subset(cpu_smt_mask(cpu), sched_idle_mask))
			cpumask_or(sched_sg_idle_mask, sched_sg_idle_mask, cpu_smt_mask(cpu));
		break;
#endif
	}
}

static inline void sched_clear_idle_mask(const unsigned int cpu)
{
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
#ifdef CONFIG_SCHED_SMT
	case CPU_TOPOLOGY_SMT:
		cpumask_andnot(sched_sg_idle_mask, sched_sg_idle_mask, cpu_smt_mask(cpu));
		break;
#endif
	}
}

extern void sched_init_topology(void);

#endif /* _KERNEL_SCHED_ALT_TOPOLOGY_H */
