#ifndef _KERNEL_SCHED_ALT_CORE_H
#define _KERNEL_SCHED_ALT_CORE_H

/*
 * Compile time debug macro
 * #define ALT_SCHED_DEBUG
 */

/* CONFIG_SCHED_CLASS_EXT is not supported */
#define scx_switched_all()	false

/*
 * Context API
 */
static inline struct rq *__task_access_lock(struct task_struct *p, raw_spinlock_t **plock)
	__context_unsafe(/* conditionally returns with the rq lock held */)
{
	struct rq *rq;
	for (;;) {
		rq = task_rq(p);
		if (p->on_cpu || task_on_rq_queued(p)) {
			raw_spin_rq_lock(rq);
			if (likely((p->on_cpu || task_on_rq_queued(p)) && rq == task_rq(p))) {
				*plock = rq_lockp(rq);
				return rq;
			}
			raw_spin_rq_unlock(rq);
		} else if (task_on_rq_migrating(p)) {
			do {
				cpu_relax();
			} while (unlikely(task_on_rq_migrating(p)));
		} else {
			*plock = NULL;
			return rq;
		}
	}
}

static inline void __task_access_unlock(struct task_struct *p, raw_spinlock_t *lock)
	__context_unsafe(/* conditionally releases the returned rq lock */)
{
	if (NULL != lock)
		raw_spin_unlock(lock);
}

void check_task_changed(struct task_struct *p, struct rq *rq);

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

static __always_inline struct task_struct * sched_rq_next_task(struct task_struct *p, struct rq *rq)
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

extern void requeue_task(struct task_struct *p, struct rq *rq, int flags);

#ifdef ALT_SCHED_DEBUG
extern void alt_sched_debug(void);
#else
static inline void alt_sched_debug(void) {}
#endif

extern int sched_yield_type;

extern cpumask_t sched_rq_pending_mask ____cacheline_aligned_in_smp;

DECLARE_STATIC_KEY_FALSE(sched_smt_present);
DECLARE_PER_CPU_ALIGNED(cpumask_t *, sched_cpu_llc_mask);

extern cpumask_t sched_smt_mask ____cacheline_aligned_in_smp;
#ifdef CONFIG_SCHED_SMT
DECLARE_PER_CPU_ALIGNED(raw_spinlock_t, sched_smt_idle_lock);
#endif

extern cpumask_t *const sched_idle_mask;
extern cpumask_t *const sched_sg_idle_mask;
extern cpumask_t *const sched_pcore_idle_mask;
extern cpumask_t *const sched_ecore_idle_mask;

extern struct rq *move_queued_task(struct rq *rq, struct rq_flags *rf,
				   struct task_struct *p, int new_cpu)
	__must_hold(__rq_lockp(rq));

DECLARE_STATIC_CALL(sched_idle_select_func, cpumask_and);

#endif /* _KERNEL_SCHED_ALT_CORE_H */
