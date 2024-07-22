#ifndef _KERNEL_SCHED_ALT_CORE_H
#define _KERNEL_SCHED_ALT_CORE_H

/*
 * Compile time debug macro
 * #define ALT_SCHED_DEBUG
 */

/*
 * Task related inlined functions
 */
static inline bool is_migration_disabled(struct task_struct *p)
{
#ifdef CONFIG_SMP
	return p->migration_disabled;
#else
	return false;
#endif
}

/*
 * RQ related inlined functions
 */

/*
 * This routine assume that the idle task always in queue
 */
static inline struct task_struct *sched_rq_first_task(struct rq *rq)
{
	const struct list_head *head = &rq->queue.heads[sched_rq_prio_idx(rq)];

	return list_first_entry(head, struct task_struct, sq_node);
}

static inline struct task_struct * sched_rq_next_task(struct task_struct *p, struct rq *rq)
{
	struct list_head *next = p->sq_node.next;

	if (&rq->queue.heads[0] <= next && next < &rq->queue.heads[SCHED_LEVELS]) {
		struct list_head *head;
		unsigned long idx = next - &rq->queue.heads[0];

		idx = find_next_bit(rq->queue.bitmap, SCHED_QUEUE_BITS,
				    sched_idx2prio(idx, rq) + 1);
		head = &rq->queue.heads[sched_prio2idx(idx, rq)];

		return list_first_entry(head, struct task_struct, sq_node);
	}

	return list_next_entry(p, sq_node);
}

#ifdef CONFIG_SMP
extern cpumask_t sched_rq_pending_mask ____cacheline_aligned_in_smp;

DECLARE_STATIC_KEY_FALSE(sched_smt_present);
DECLARE_PER_CPU_ALIGNED(cpumask_t *, sched_cpu_llc_mask);

extern cpumask_t sched_smt_mask ____cacheline_aligned_in_smp;

extern cpumask_t *const sched_idle_mask;
extern cpumask_t *const sched_sg_idle_mask;
extern cpumask_t *const sched_pcore_idle_mask;
extern cpumask_t *const sched_ecore_idle_mask;

extern struct rq *move_queued_task(struct rq *rq, struct task_struct *p, int new_cpu);

typedef bool (*idle_select_func_t)(struct cpumask *dstp, const struct cpumask *src1p,
				   const struct cpumask *src2p);

extern idle_select_func_t idle_select_func;
#endif

#endif /* _KERNEL_SCHED_ALT_CORE_H */
