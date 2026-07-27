#include "alt_core.h"
#include "alt_topology.h"

/*
 * The override has been removed, but keep an obsolete setup entry so stale
 * bootloader configurations are not passed to PID 1 as environment variables.
 */
__setup_param("pcore_cpus=", pcore_cpus_obsolete, NULL, 0);

enum sched_idle_select_mode {
	SCHED_IDLE_SELECT_GENERIC,
#ifdef CONFIG_SCHED_SMT
	SCHED_IDLE_SELECT_SMT,
#endif
	SCHED_IDLE_SELECT_HYBRID,
};

static cpumask_t sched_pcore_mask ____cacheline_aligned_in_smp;
static cpumask_t sched_ecore_mask;
static cpumask_t sched_unknown_mask;
static cpumask_t sched_topology_active_mask;

/* CPU hotplug is serialized, so one set of rebuild masks is sufficient. */
static cpumask_t sched_rebuild_pcore_mask;
static cpumask_t sched_rebuild_ecore_mask;
static cpumask_t sched_rebuild_unknown_mask;
static cpumask_t sched_rebuild_smt_mask;
static cpumask_t sched_rebuild_visited_mask;
#ifdef CONFIG_SCHED_SMT
static cpumask_t sched_rebuild_group_mask;
#endif

static enum sched_idle_select_mode sched_idle_select_mode __read_mostly;
static enum sched_idle_select_mode sched_idle_select_applied __read_mostly;
static bool sched_heterogeneous __read_mostly;
static bool sched_mixed_cpu_types __read_mostly;
static bool sched_unsupported_ecore_smt __read_mostly;
static bool sched_topology_initialized;
static bool sched_topology_changed;
static bool sched_topology_active_changed;
static bool sched_topology_warn_unknown;
static bool sched_topology_warn_ecore_smt;
static bool sched_topology_warn_smt_class;

#ifdef CONFIG_X86
static void sched_detect_cpu_types(const struct cpumask *active_mask)
{
	int cpu;

	for_each_cpu(cpu, active_mask) {
		struct cpuinfo_x86 *c = &cpu_data(cpu);
		enum x86_topology_cpu_type type;

		/*
		 * cpuinfo_topology stores all vendor formats in a union.  The
		 * generic UNKNOWN initializer looks like AMD type 0 unless the
		 * heterogeneous topology leaf is known to be valid.
		 */
		if (c->x86_vendor == X86_VENDOR_AMD &&
		    !cpu_has(c, X86_FEATURE_AMD_HTR_CORES))
			type = TOPO_CPU_TYPE_UNKNOWN;
		else
			type = get_topology_cpu_type(c);

		switch (type) {
		case TOPO_CPU_TYPE_PERFORMANCE:
			cpumask_set_cpu(cpu, &sched_rebuild_pcore_mask);
			break;
		case TOPO_CPU_TYPE_EFFICIENCY:
			cpumask_set_cpu(cpu, &sched_rebuild_ecore_mask);
			break;
		case TOPO_CPU_TYPE_UNKNOWN:
		default:
			cpumask_set_cpu(cpu, &sched_rebuild_unknown_mask);
			break;
		}
	}
}
#else
static inline void sched_detect_cpu_types(const struct cpumask *active_mask)
{
	cpumask_copy(&sched_rebuild_unknown_mask, active_mask);
}
#endif

#ifdef CONFIG_SCHED_SMT
static bool
sched_group_has_single_class(const struct cpumask *group,
			     const struct cpumask *pcore_mask,
			     const struct cpumask *ecore_mask,
			     const struct cpumask *unknown_mask)
{
	return cpumask_subset(group, pcore_mask) ||
	       cpumask_subset(group, ecore_mask) ||
	       cpumask_subset(group, unknown_mask);
}

static void sched_detect_smt(const struct cpumask *active_mask)
{
	int cpu, sibling;

	cpumask_clear(&sched_rebuild_visited_mask);

	for_each_cpu(cpu, active_mask) {
		int threads = 0;

		if (cpumask_test_cpu(cpu, &sched_rebuild_visited_mask))
			continue;

		cpumask_clear(&sched_rebuild_group_mask);
		for_each_cpu(sibling, cpu_smt_mask(cpu)) {
			if (!cpumask_test_cpu(sibling, active_mask))
				continue;

			cpumask_set_cpu(sibling, &sched_rebuild_group_mask);
			cpumask_set_cpu(sibling, &sched_rebuild_visited_mask);
			threads++;
		}

		if (!threads) {
			cpumask_set_cpu(cpu, &sched_rebuild_group_mask);
			cpumask_set_cpu(cpu, &sched_rebuild_visited_mask);
			threads = 1;
		}

		if (threads < 2)
			continue;

		cpumask_or(&sched_rebuild_smt_mask, &sched_rebuild_smt_mask,
			   &sched_rebuild_group_mask);

		if (sched_group_has_single_class(&sched_rebuild_group_mask,
						 &sched_rebuild_pcore_mask,
						 &sched_rebuild_ecore_mask,
						 &sched_rebuild_unknown_mask))
			continue;

		cpumask_andnot(&sched_rebuild_pcore_mask,
			       &sched_rebuild_pcore_mask,
			       &sched_rebuild_group_mask);
		cpumask_andnot(&sched_rebuild_ecore_mask,
			       &sched_rebuild_ecore_mask,
			       &sched_rebuild_group_mask);
		cpumask_or(&sched_rebuild_unknown_mask,
			   &sched_rebuild_unknown_mask,
			   &sched_rebuild_group_mask);
	}
}
#else
static inline void sched_detect_smt(const struct cpumask *active_mask) { }
#endif

struct sched_topology_policy {
	enum sched_idle_select_mode idle_select_mode;
	bool heterogeneous;
	bool mixed_cpu_types;
	bool unsupported_ecore_smt;
};

static struct sched_topology_policy sched_rebuild_policy;

static void
sched_select_topology_policy(const struct cpumask *active_mask,
			     const struct cpumask *pcore_mask,
			     const struct cpumask *ecore_mask,
			     const struct cpumask *unknown_mask,
			     const struct cpumask *smt_mask,
			     struct sched_topology_policy *policy)
{
	bool have_known = !cpumask_empty(pcore_mask) ||
			  !cpumask_empty(ecore_mask);
	bool complete_hybrid =
		!cpumask_empty(pcore_mask) &&
		!cpumask_empty(ecore_mask) &&
		cpumask_empty(unknown_mask);

	policy->idle_select_mode = SCHED_IDLE_SELECT_GENERIC;
	policy->mixed_cpu_types =
		have_known && !cpumask_empty(unknown_mask);
#ifdef CONFIG_SCHED_SMT
	policy->unsupported_ecore_smt =
		cpumask_intersects(ecore_mask, smt_mask);
#else
	policy->unsupported_ecore_smt = false;
#endif
	policy->heterogeneous =
		complete_hybrid && !policy->unsupported_ecore_smt;

	if (policy->heterogeneous) {
		policy->idle_select_mode = SCHED_IDLE_SELECT_HYBRID;
		return;
	}

#ifdef CONFIG_SCHED_SMT
	if (!cpumask_empty(active_mask) &&
	    cpumask_equal(smt_mask, active_mask))
		policy->idle_select_mode = SCHED_IDLE_SELECT_SMT;
#endif
}

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
	return (cpumask_and(dstp, src1p, sched_sg_idle_mask) &&
		cpumask_and(dstp, dstp, src2p))			||
	       cpumask_and(dstp, src1p, src2p);
}
#endif

static bool p1p2_idle_select_func(struct cpumask *dstp, const struct cpumask *src1p,
					const struct cpumask *src2p)
{
	return (cpumask_and(dstp, src1p, sched_pcore_idle_mask) &&
		cpumask_and(dstp, dstp, src2p))			||
	       (cpumask_and(dstp, src1p, sched_ecore_idle_mask) &&
		cpumask_and(dstp, dstp, src2p))			||
	       cpumask_and(dstp, src1p, src2p);
}

/* common balance functions */
static int active_balance_cpu_stop(void *data)
{
	struct balance_arg *arg = data;
	struct task_struct *p = arg->task;
	struct rq *rq = this_rq();
	struct rq_flags rf;
	cpumask_t tmp;

	local_irq_save(rf.flags);

	raw_spin_lock(&p->pi_lock);
	rq_lock(rq, &rf);

	arg->active = 0;

	if (task_on_rq_queued(p) && task_rq(p) == rq &&
	    cpumask_and(&tmp, p->cpus_ptr, arg->cpumask) &&
	    cpumask_and(&tmp, &tmp, sched_idle_mask) &&
	    cpumask_and(&tmp, &tmp, cpu_active_mask) &&
	    !is_migration_disabled(p)) {
		int dcpu = __best_mask_cpu(&tmp, per_cpu(sched_cpu_llc_mask, cpu_of(rq)),
					   per_cpu(sched_cpu_topo_end_mask, cpu_of(rq)));
		rq = move_queued_task(rq, &rf, p, dcpu);
	}

	rq_unlock(rq, &rf);
	raw_spin_unlock_irqrestore(&p->pi_lock, rf.flags);

	put_task_struct(p);

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

	local_irq_save(flags);
	if (!raw_spin_rq_trylock(rq)) {
		local_irq_restore(flags);
		return 0;
	}

	arg = &rq->active_balance_arg;
	res = (1 == rq->nr_running) &&					\
	      !is_migration_disabled((p = sched_rq_first_task(rq))) &&	\
	      cpumask_intersects(p->cpus_ptr, target_mask) &&		\
	      !arg->active;
	if (res) {
		arg->task = p;
		arg->cpumask = target_mask;
		get_task_struct(p);

		arg->active = 1;
	}

	raw_spin_rq_unlock_irqrestore(rq, flags);

	if (res) {
		preempt_disable();
		raw_spin_rq_unlock(src_rq);

		if (!stop_one_cpu_nowait(cpu_of(rq), active_balance_cpu_stop, arg,
					 &rq->active_balance_work)) {
			raw_spin_rq_lock_irqsave(rq, flags);
			if (arg->active) {
				put_task_struct(arg->task);
				arg->active = 0;
			}
			raw_spin_rq_unlock_irqrestore(rq, flags);
		}

		preempt_enable();
		raw_spin_rq_lock(src_rq);
	}

	return res;
}

static inline int
ecore_source_balance(struct rq *rq, cpumask_t *single_task_mask, cpumask_t *target_mask)
{
	if (cpumask_and(single_task_mask, single_task_mask, &sched_ecore_mask)) {
		int i, cpu = cpu_of(rq);

		for_each_cpu_wrap(i, single_task_mask, cpu)
			if (trigger_active_balance(rq, cpu_rq(i), target_mask))
				return 1;
	}

	return 0;
}

#ifdef CONFIG_SCHED_SMT
static bool sched_smt_group_busy(int cpu, const struct cpumask *busy_mask)
{
	int sibling;

	for_each_cpu_and(sibling, cpu_smt_mask(cpu), &sched_smt_mask)
		if (!cpumask_test_cpu(sibling, busy_mask))
			return false;

	return true;
}

static inline int
smt_source_balance(struct rq *rq, cpumask_t *single_task_mask,
		   const struct cpumask *source_mask, cpumask_t *target_mask)
{
	cpumask_t smt_single_mask;

	if (cpumask_and(&smt_single_mask, single_task_mask, &sched_smt_mask)) {
		int i, cpu = cpu_of(rq);

		if (source_mask &&
		    !cpumask_and(&smt_single_mask, &smt_single_mask,
				 source_mask))
			return 0;

		for_each_cpu_wrap(i, &smt_single_mask, cpu) {
			if (sched_smt_group_busy(i, &smt_single_mask) &&
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
	     smt_source_balance(rq, &single_task_mask, &sched_pcore_mask,
				sched_pcore_idle_mask) ||
	     /* e core to idle smt core balance */
	     ecore_source_balance(rq, &single_task_mask,
				  sched_pcore_idle_mask)))
		return;
}

/* smt balance functions */
void smt_balance(struct rq *rq)
{
	cpumask_t single_task_mask;

	if (cpumask_andnot(&single_task_mask, cpu_active_mask, sched_idle_mask) &&
	    cpumask_andnot(&single_task_mask, &single_task_mask, &sched_rq_pending_mask) &&
	    smt_source_balance(rq, &single_task_mask, NULL,
			       sched_sg_idle_mask))
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
	    smt_source_balance(rq, &single_task_mask, &sched_pcore_mask,
			       sched_ecore_idle_mask))
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

static const char *sched_idle_select_mode_name(enum sched_idle_select_mode mode)
{
	switch (mode) {
	case SCHED_IDLE_SELECT_GENERIC:
		return "generic";
#ifdef CONFIG_SCHED_SMT
	case SCHED_IDLE_SELECT_SMT:
		return "SMT";
#endif
	case SCHED_IDLE_SELECT_HYBRID:
		return "hybrid";
	}

	return "unknown";
}

static void sched_idle_select_update(enum sched_idle_select_mode mode)
{
	if (mode == READ_ONCE(sched_idle_select_applied))
		return;

	switch (mode) {
	case SCHED_IDLE_SELECT_GENERIC:
		static_call_update(sched_idle_select_func, cpumask_and);
		break;
#ifdef CONFIG_SCHED_SMT
	case SCHED_IDLE_SELECT_SMT:
		static_call_update(sched_idle_select_func, p1_idle_select_func);
		break;
#endif
	case SCHED_IDLE_SELECT_HYBRID:
		static_call_update(sched_idle_select_func, p1p2_idle_select_func);
		break;
	}

	WRITE_ONCE(sched_idle_select_applied, mode);
}

static void sched_idle_select_workfn(struct work_struct *work)
{
	enum sched_idle_select_mode mode;

	do {
		/* static_call_update() takes cpus_read_lock() itself. */
		mode = READ_ONCE(sched_idle_select_mode);
		sched_idle_select_update(mode);
	} while (mode != READ_ONCE(sched_idle_select_mode));
}

static DECLARE_WORK(sched_idle_select_work, sched_idle_select_workfn);

void sched_topology_queue_idle_select(void)
{
	if (READ_ONCE(sched_idle_select_mode) !=
	    READ_ONCE(sched_idle_select_applied))
		schedule_work(&sched_idle_select_work);
}

void __init sched_topology_init_idle_select(void)
{
	sched_idle_select_update(READ_ONCE(sched_idle_select_mode));
}

/* Reports outside the stopped machine; CPU hotplug serializes against itself. */
void sched_topology_report(void)
{
	bool changed = sched_topology_changed;
	bool active_changed = sched_topology_active_changed;
	bool warn_ecore_smt = sched_topology_warn_ecore_smt;
	bool warn_smt_class = sched_topology_warn_smt_class;
	bool warn_unknown = sched_topology_warn_unknown;

	sched_topology_changed = false;
	sched_topology_active_changed = false;
	sched_topology_warn_ecore_smt = false;
	sched_topology_warn_smt_class = false;
	sched_topology_warn_unknown = false;

	WARN_ONCE(warn_smt_class,
		  "sched/alt: P/E scheduling with a non-performance SMT group in %*pbl\n",
		  cpumask_pr_args(&sched_smt_mask));

	if (warn_unknown)
		pr_warn_once("sched/alt: UNKNOWN CPU types in %*pbl; disabling P/E auto-detection\n",
			     cpumask_pr_args(&sched_unknown_mask));

	if (warn_ecore_smt)
		pr_warn_once("sched/alt: E-core SMT is unsupported; using homogeneous scheduling\n");

	if (!changed) {
		if (active_changed)
			pr_debug_ratelimited("sched/alt: active CPUs: %*pbl\n",
					     cpumask_pr_args(&sched_topology_active_mask));
		return;
	}

	pr_info("sched/alt: %s topology, idle select: %s, active: %*pbl, SMT: %*pbl, P: %*pbl, E: %*pbl, UNKNOWN: %*pbl\n",
		sched_heterogeneous ? "heterogeneous" : "homogeneous",
		sched_idle_select_mode_name(READ_ONCE(sched_idle_select_mode)),
		cpumask_pr_args(&sched_topology_active_mask),
		cpumask_pr_args(&sched_smt_mask),
		cpumask_pr_args(&sched_pcore_mask),
		cpumask_pr_args(&sched_ecore_mask),
		cpumask_pr_args(&sched_unknown_mask));
}

#ifdef CONFIG_SCHED_SMT
static bool sched_rebuild_smt_group_idle(int cpu)
{
	int sibling;

	for_each_cpu_and(sibling, cpu_smt_mask(cpu), &sched_smt_mask)
		if (!cpumask_test_cpu(sibling, sched_idle_mask))
			return false;

	return true;
}
#endif

static void sched_rebuild_idle_masks(const struct cpumask *active_mask)
{
	int cpu;

	/*
	 * Every active CPU runs a stopper here, so a remaining bit belongs to
	 * a CPU which is not active and cannot be a placement candidate.
	 */
	cpumask_and(sched_idle_mask, sched_idle_mask, active_mask);
	cpumask_clear(sched_sg_idle_mask);
	cpumask_clear(sched_pcore_idle_mask);
	cpumask_clear(sched_ecore_idle_mask);
	cpumask_clear(&sched_rebuild_visited_mask);

	for_each_cpu(cpu, active_mask) {
#ifdef CONFIG_SCHED_SMT
		if (cpumask_test_cpu(cpu, &sched_smt_mask)) {
			int sibling;

			if (cpumask_test_cpu(cpu, &sched_rebuild_visited_mask))
				continue;

			cpumask_clear(&sched_rebuild_group_mask);
			for_each_cpu_and(sibling, cpu_smt_mask(cpu),
					 &sched_smt_mask) {
				cpumask_set_cpu(sibling,
						&sched_rebuild_group_mask);
				cpumask_set_cpu(sibling,
						&sched_rebuild_visited_mask);
			}

			if (!sched_rebuild_smt_group_idle(cpu))
				continue;

			cpumask_or(sched_sg_idle_mask, sched_sg_idle_mask,
				   &sched_rebuild_group_mask);
			if (per_cpu(sched_cpu_topo, cpu) ==
			    CPU_TOPOLOGY_PCORE)
				cpumask_or(sched_pcore_idle_mask,
					   sched_pcore_idle_mask,
					   &sched_rebuild_group_mask);
			continue;
		}
#endif

		if (!cpumask_test_cpu(cpu, sched_idle_mask))
			continue;

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
}

void sched_topology_prepare(const struct cpumask *active_mask)
{
	cpumask_clear(&sched_rebuild_pcore_mask);
	cpumask_clear(&sched_rebuild_ecore_mask);
	cpumask_clear(&sched_rebuild_unknown_mask);
	cpumask_clear(&sched_rebuild_smt_mask);

	sched_detect_cpu_types(active_mask);
	sched_detect_smt(active_mask);
	sched_select_topology_policy(active_mask,
				     &sched_rebuild_pcore_mask,
				     &sched_rebuild_ecore_mask,
				     &sched_rebuild_unknown_mask,
				     &sched_rebuild_smt_mask,
				     &sched_rebuild_policy);
}

void sched_topology_apply(const struct cpumask *active_mask)
{
	bool active_changed =
		!cpumask_equal(active_mask, &sched_topology_active_mask);
	bool semantic_changed =
		!sched_topology_initialized ||
		sched_rebuild_policy.heterogeneous != sched_heterogeneous ||
		sched_rebuild_policy.mixed_cpu_types != sched_mixed_cpu_types ||
		sched_rebuild_policy.unsupported_ecore_smt !=
			sched_unsupported_ecore_smt ||
		sched_rebuild_policy.idle_select_mode != sched_idle_select_mode;
#ifdef CONFIG_SCHED_SMT
	bool pcore_smt =
		cpumask_intersects(&sched_rebuild_pcore_mask,
				   &sched_rebuild_smt_mask);
#endif
	int cpu;

	cpumask_or(&sched_rebuild_visited_mask, &sched_topology_active_mask,
		   active_mask);

	if (sched_rebuild_policy.mixed_cpu_types &&
	    !sched_mixed_cpu_types)
		sched_topology_warn_unknown = true;
	if (sched_rebuild_policy.unsupported_ecore_smt &&
	    !sched_unsupported_ecore_smt)
		sched_topology_warn_ecore_smt = true;

	cpumask_copy(&sched_topology_active_mask, active_mask);
	cpumask_copy(&sched_smt_mask, &sched_rebuild_smt_mask);
	cpumask_copy(&sched_pcore_mask, &sched_rebuild_pcore_mask);
	cpumask_copy(&sched_ecore_mask, &sched_rebuild_ecore_mask);
	cpumask_copy(&sched_unknown_mask, &sched_rebuild_unknown_mask);
	sched_heterogeneous = sched_rebuild_policy.heterogeneous;
	sched_mixed_cpu_types = sched_rebuild_policy.mixed_cpu_types;
	sched_unsupported_ecore_smt =
		sched_rebuild_policy.unsupported_ecore_smt;
	WRITE_ONCE(sched_idle_select_mode,
		   sched_rebuild_policy.idle_select_mode);

	for_each_cpu(cpu, &sched_rebuild_visited_mask) {
		per_cpu(sched_cpu_topo, cpu) = CPU_TOPOLOGY_DEFAULT;
		per_cpu(sched_cpu_topo_balance, cpu) = CPU_TOPOLOGY_BALANCE_NONE;
	}

	for_each_cpu(cpu, active_mask) {
		if (sched_heterogeneous) {
			if (cpumask_test_cpu(cpu, &sched_pcore_mask))
				per_cpu(sched_cpu_topo, cpu) =
					CPU_TOPOLOGY_PCORE;
			else if (cpumask_test_cpu(cpu, &sched_ecore_mask))
				per_cpu(sched_cpu_topo, cpu) =
					CPU_TOPOLOGY_ECORE;
		}

#ifdef CONFIG_SCHED_SMT
		if (cpumask_test_cpu(cpu, &sched_smt_mask)) {
			if (sched_heterogeneous) {
				if (per_cpu(sched_cpu_topo, cpu) !=
				    CPU_TOPOLOGY_PCORE)
					sched_topology_warn_smt_class = true;
				per_cpu(sched_cpu_topo_balance, cpu) =
					CPU_TOPOLOGY_BALANCE_SMT_PCORE;
			} else {
				per_cpu(sched_cpu_topo_balance, cpu) =
					CPU_TOPOLOGY_BALANCE_SMT;
			}

			continue;
		}
#endif
		if (!sched_heterogeneous)
			continue;

		if (per_cpu(sched_cpu_topo, cpu) == CPU_TOPOLOGY_PCORE) {
			per_cpu(sched_cpu_topo_balance, cpu) =
				CPU_TOPOLOGY_BALANCE_PCORE;
			continue;
		}

		if (per_cpu(sched_cpu_topo, cpu) == CPU_TOPOLOGY_ECORE) {
#ifdef CONFIG_SCHED_SMT
			if (pcore_smt)
				per_cpu(sched_cpu_topo_balance, cpu) =
					CPU_TOPOLOGY_BALANCE_ECORE;
#endif
		}
	}

	sched_rebuild_idle_masks(active_mask);

	sched_topology_initialized = true;
	sched_topology_changed |= semantic_changed;
	sched_topology_active_changed |= active_changed;
}

#ifdef CONFIG_SCHED_ALT_TOPOLOGY_KUNIT_TEST
#include "alt_topology_test.c"
#endif
