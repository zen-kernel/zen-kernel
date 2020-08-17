#define ALT_SCHED_VERSION_MSG "sched/bmq: BMQ CPU Scheduler 5.8-r1 by Alfred Chen.\n"

static inline void sched_queue_init(struct rq *rq)
{
	struct bmq *q = &rq->queue;
	int i;

	bitmap_zero(q->bitmap, SCHED_BITS);
	for(i = 0; i < SCHED_BITS; i++)
		INIT_LIST_HEAD(&q->heads[i]);
}

static inline void sched_queue_init_idle(struct rq *rq, struct task_struct *idle)
{
	struct bmq *q = &rq->queue;

	idle->bmq_idx = IDLE_TASK_SCHED_PRIO;
	INIT_LIST_HEAD(&q->heads[idle->bmq_idx]);
	list_add(&idle->bmq_node, &q->heads[idle->bmq_idx]);
	set_bit(idle->bmq_idx, q->bitmap);
}

/*
 * This routine used in bmq scheduler only which assume the idle task in the bmq
 */
static inline struct task_struct *sched_rq_first_task(struct rq *rq)
{
	unsigned long idx = find_first_bit(rq->queue.bitmap, SCHED_BITS);
	const struct list_head *head = &rq->queue.heads[idx];

	return list_first_entry(head, struct task_struct, bmq_node);
}

static inline struct task_struct *
sched_rq_next_task(struct task_struct *p, struct rq *rq)
{
	unsigned long idx = p->bmq_idx;
	struct list_head *head = &rq->queue.heads[idx];

	if (list_is_last(&p->bmq_node, head)) {
		idx = find_next_bit(rq->queue.bitmap, SCHED_BITS, idx + 1);
		head = &rq->queue.heads[idx];

		return list_first_entry(head, struct task_struct, bmq_node);
	}

	return list_next_entry(p, bmq_node);
}

#define __SCHED_DEQUEUE_TASK(p, rq, flags, func)	\
	psi_dequeue(p, flags & DEQUEUE_SLEEP);		\
	sched_info_dequeued(rq, p);			\
							\
	list_del(&p->bmq_node);				\
	if (list_empty(&rq->queue.heads[p->bmq_idx])) {	\
		clear_bit(p->bmq_idx, rq->queue.bitmap);\
		func;					\
	}

#define __SCHED_ENQUEUE_TASK(p, rq, flags)				\
	sched_info_queued(rq, p);					\
	psi_enqueue(p, flags);						\
									\
	p->bmq_idx = task_sched_prio(p);				\
	list_add_tail(&p->bmq_node, &rq->queue.heads[p->bmq_idx]);	\
	set_bit(p->bmq_idx, rq->queue.bitmap)

static inline void __requeue_task(struct task_struct *p, struct rq *rq)
{
	int idx = task_sched_prio(p);

	list_del(&p->bmq_node);
	list_add_tail(&p->bmq_node, &rq->queue.heads[idx]);
	if (idx != p->bmq_idx) {
		if (list_empty(&rq->queue.heads[p->bmq_idx]))
			clear_bit(p->bmq_idx, rq->queue.bitmap);
		p->bmq_idx = idx;
		set_bit(p->bmq_idx, rq->queue.bitmap);
		update_sched_rq_watermark(rq);
	}
}

static inline bool sched_task_need_requeue(struct task_struct *p)
{
	return (task_sched_prio(p) != p->bmq_idx);
}
