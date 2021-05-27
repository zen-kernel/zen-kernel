#define ALT_SCHED_VERSION_MSG "sched/pds: PDS CPU Scheduler "ALT_SCHED_VERSION" by Alfred Chen.\n"

static const u64 user_prio2deadline[NICE_WIDTH] = {
/* -20 */	  4194304,   4613734,   5075107,   5582617,   6140878,
/* -15 */	  6754965,   7430461,   8173507,   8990857,   9889942,
/* -10 */	 10878936,  11966829,  13163511,  14479862,  15927848,
/*  -5 */	 17520632,  19272695,  21199964,  23319960,  25651956,
/*   0 */	 28217151,  31038866,  34142752,  37557027,  41312729,
/*   5 */	 45444001,  49988401,  54987241,  60485965,  66534561,
/*  10 */	 73188017,  80506818,  88557499,  97413248, 107154572,
/*  15 */	117870029, 129657031, 142622734, 156885007, 172573507
};

#define SCHED_PRIO_SLOT		(4ULL << 20)
#define DEFAULT_SCHED_PRIO (MIN_NORMAL_PRIO + SCHED_NORMAL_PRIO_NUM / 2)

extern int alt_debug[20];

static inline int
task_sched_prio_normal(const struct task_struct *p, const struct rq *rq)
{
	s64 delta = (p->deadline >> 22) - rq->time_edge  - 1;

	if (unlikely(delta > SCHED_NORMAL_PRIO_NUM - 1)) {
		pr_info("pds: task_sched_prio_normal delta %lld, deadline %llu(%llu), time_edge %llu\n",
			delta, p->deadline, p->deadline >> 22, rq->time_edge);
		delta = SCHED_NORMAL_PRIO_NUM - 1ULL;
	}

	return (delta < 0)? 0:delta;
}

static inline int
task_sched_prio(const struct task_struct *p)
{
	return (p->prio < MAX_RT_PRIO) ? p->prio :
		MIN_NORMAL_PRIO + task_sched_prio_normal(p, task_rq(p));
}

static inline int
task_sched_prio_idx(const struct task_struct *p, const struct rq *rq)
{
	return (p->prio < MAX_RT_PRIO) ? p->prio : MIN_NORMAL_PRIO +
		(task_sched_prio_normal(p, rq) + rq->time_edge) %
		SCHED_NORMAL_PRIO_NUM;
}

static inline unsigned long sched_prio2idx(unsigned long idx, struct rq *rq)
{
	return (IDLE_TASK_SCHED_PRIO == idx || idx < MAX_RT_PRIO) ? idx :
		MIN_NORMAL_PRIO +
		((idx - MIN_NORMAL_PRIO) + rq->time_edge) %
		SCHED_NORMAL_PRIO_NUM;
}

static inline unsigned long sched_idx2prio(unsigned long idx, struct rq *rq)
{
	return (idx < MAX_RT_PRIO) ? idx : MIN_NORMAL_PRIO +
		((idx - MIN_NORMAL_PRIO) + SCHED_NORMAL_PRIO_NUM -
		 rq->time_edge % SCHED_NORMAL_PRIO_NUM) %
		SCHED_NORMAL_PRIO_NUM;
}

static inline void sched_renew_deadline(struct task_struct *p, const struct rq *rq)
{
	if (p->prio >= MAX_RT_PRIO)
		p->deadline = rq->clock + SCHED_PRIO_SLOT *
			(p->static_prio - MIN_NORMAL_PRIO + 1);
}

/*
 * Common interfaces
 */
static inline int normal_prio(struct task_struct *p)
{
	if (task_has_rt_policy(p))
		return MAX_RT_PRIO - 1 - p->rt_priority;

	return MAX_RT_PRIO;
}

int task_running_nice(struct task_struct *p)
{
	return task_sched_prio(p) > DEFAULT_SCHED_PRIO;
}

static inline void update_rq_time_edge(struct rq *rq)
{
	struct list_head head;
	u64 old = rq->time_edge;
	u64 now = rq->clock >> 22;
	u64 prio, delta;

	if (now == old)
		return;

	delta = min_t(u64, SCHED_NORMAL_PRIO_NUM, now - old);
	INIT_LIST_HEAD(&head);

	for_each_set_bit(prio, &rq->queue.bitmap[2], delta) {
		u64 idx;

		idx = MIN_NORMAL_PRIO +
			(prio + rq->time_edge) % SCHED_NORMAL_PRIO_NUM;
		list_splice_tail_init(rq->queue.heads + idx, &head);
	}
	rq->queue.bitmap[2] >>= delta;
	rq->time_edge = now;
	if (!list_empty(&head)) {
		u64 new_idx = MIN_NORMAL_PRIO + now % SCHED_NORMAL_PRIO_NUM;
		struct task_struct *p;

		list_for_each_entry(p, &head, sq_node)
			p->sq_idx = new_idx;

		list_splice(&head, rq->queue.heads + new_idx);
		rq->queue.bitmap[2] |= 1UL;
	}
}

static inline void requeue_task(struct task_struct *p, struct rq *rq);

static inline void time_slice_expired(struct task_struct *p, struct rq *rq)
{
	/*printk(KERN_INFO "sched: time_slice_expired(%d) - %px\n", cpu_of(rq), p);*/
	p->time_slice = sched_timeslice_ns;
	sched_renew_deadline(p, rq);
	if (SCHED_FIFO != p->policy && task_on_rq_queued(p))
		requeue_task(p, rq);
}

static inline void sched_task_sanity_check(struct task_struct *p, struct rq *rq)
{
	if (unlikely(p->deadline > rq->clock + 40 * SCHED_PRIO_SLOT))
		p->deadline = rq->clock + 40 * SCHED_PRIO_SLOT;
}

/*
 * This routine assume that the idle task always in queue
 */
static inline struct task_struct *sched_rq_first_task(struct rq *rq)
{
	unsigned long idx = find_first_bit(rq->queue.bitmap, SCHED_QUEUE_BITS);
	const struct list_head *head = &rq->queue.heads[sched_prio2idx(idx, rq)];

	return list_first_entry(head, struct task_struct, sq_node);
}

static inline struct task_struct *
sched_rq_next_task(struct task_struct *p, struct rq *rq)
{
	unsigned long idx = p->sq_idx;
	struct list_head *head = &rq->queue.heads[idx];

	if (list_is_last(&p->sq_node, head)) {
		idx = find_next_bit(rq->queue.bitmap, SCHED_QUEUE_BITS,
				    sched_idx2prio(idx, rq) + 1);
		head = &rq->queue.heads[sched_prio2idx(idx, rq)];

		return list_first_entry(head, struct task_struct, sq_node);
	}

	return list_next_entry(p, sq_node);
}

#define __SCHED_DEQUEUE_TASK(p, rq, flags, func)		\
	psi_dequeue(p, flags & DEQUEUE_SLEEP);			\
	sched_info_dequeued(rq, p);				\
								\
	list_del(&p->sq_node);					\
	if (list_empty(&rq->queue.heads[p->sq_idx])) {		\
		clear_bit(sched_idx2prio(p->sq_idx, rq),	\
			  rq->queue.bitmap);			\
		func;						\
	}

#define __SCHED_ENQUEUE_TASK(p, rq, flags)				\
	sched_info_queued(rq, p);					\
	psi_enqueue(p, flags);						\
									\
	p->sq_idx = task_sched_prio_idx(p, rq);				\
	list_add_tail(&p->sq_node, &rq->queue.heads[p->sq_idx]);	\
	set_bit(sched_idx2prio(p->sq_idx, rq), rq->queue.bitmap);

/*
 * Requeue a task @p to @rq
 */
#define __SCHED_REQUEUE_TASK(p, rq, func)					\
{\
	int idx = task_sched_prio_idx(p, rq);					\
\
	list_del(&p->sq_node);							\
	list_add_tail(&p->sq_node, &rq->queue.heads[idx]);			\
	if (idx != p->sq_idx) {							\
		if (list_empty(&rq->queue.heads[p->sq_idx]))			\
			clear_bit(sched_idx2prio(p->sq_idx, rq),		\
				  rq->queue.bitmap);				\
		p->sq_idx = idx;						\
		set_bit(sched_idx2prio(p->sq_idx, rq), rq->queue.bitmap);	\
		func;								\
	}									\
}

static inline bool sched_task_need_requeue(struct task_struct *p, struct rq *rq)
{
	return (task_sched_prio_idx(p, rq) != p->sq_idx);
}

static void sched_task_fork(struct task_struct *p, struct rq *rq)
{
	sched_renew_deadline(p, rq);
}

/**
 * task_prio - return the priority value of a given task.
 * @p: the task in question.
 *
 * Return: The priority value as seen by users in /proc.
 *
 * sched policy         return value   kernel prio    user prio/nice
 *
 * normal, batch, idle     [0 ... 39]            100          0/[-20 ... 19]
 * fifo, rr             [-1 ... -100]     [99 ... 0]  [0 ... 99]
 */
int task_prio(const struct task_struct *p)
{
	int ret;

	if (p->prio < MAX_RT_PRIO)
		return (p->prio - MAX_RT_PRIO);

	ret = task_sched_prio(p) - MIN_NORMAL_PRIO;

	return ret;
}

static void do_sched_yield_type_1(struct task_struct *p, struct rq *rq)
{
	time_slice_expired(p, rq);
}

#ifdef CONFIG_SMP
static void sched_task_ttwu(struct task_struct *p) {}
#endif
static void sched_task_deactivate(struct task_struct *p, struct rq *rq) {}
