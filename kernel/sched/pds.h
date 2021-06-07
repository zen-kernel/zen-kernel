#define ALT_SCHED_VERSION_MSG "sched/pds: PDS CPU Scheduler "ALT_SCHED_VERSION" by Alfred Chen.\n"

static int sched_timeslice_shift = 22;

#define NORMAL_PRIO_MOD(x)	((x) & (NORMAL_PRIO_NUM - 1))

/*
 * Common interfaces
 */
static inline void sched_timeslice_imp(const int timeslice_ms)
{
	if (2 == timeslice_ms)
		sched_timeslice_shift = 21;
}

static inline int
task_sched_prio_normal(const struct task_struct *p, const struct rq *rq)
{
	s64 delta = p->deadline - rq->time_edge + NORMAL_PRIO_NUM - NICE_WIDTH;

	if (unlikely(delta > NORMAL_PRIO_NUM - 1)) {
		pr_info("pds: task_sched_prio_normal delta %lld, deadline %llu, time_edge %llu\n",
			delta, p->deadline, rq->time_edge);
		return NORMAL_PRIO_NUM - 1;
	}

	return (delta < 0) ? 0 : delta;
}

static inline int task_sched_prio(const struct task_struct *p)
{
	return (p->prio < MAX_RT_PRIO) ? p->prio :
		MIN_NORMAL_PRIO + task_sched_prio_normal(p, task_rq(p));
}

static inline int
task_sched_prio_idx(const struct task_struct *p, const struct rq *rq)
{
	return (p->prio < MAX_RT_PRIO) ? p->prio : MIN_NORMAL_PRIO +
		NORMAL_PRIO_MOD(task_sched_prio_normal(p, rq) + rq->time_edge);
}

static inline unsigned long sched_prio2idx(unsigned long prio, struct rq *rq)
{
	return (IDLE_TASK_SCHED_PRIO == prio || prio < MAX_RT_PRIO) ? prio :
		MIN_NORMAL_PRIO + NORMAL_PRIO_MOD((prio - MIN_NORMAL_PRIO) +
						  rq->time_edge);
}

static inline unsigned long sched_idx2prio(unsigned long idx, struct rq *rq)
{
	return (idx < MAX_RT_PRIO) ? idx : MIN_NORMAL_PRIO +
		NORMAL_PRIO_MOD((idx - MIN_NORMAL_PRIO) + NORMAL_PRIO_NUM -
				NORMAL_PRIO_MOD(rq->time_edge));
}

static inline void sched_renew_deadline(struct task_struct *p, const struct rq *rq)
{
	if (p->prio >= MAX_RT_PRIO)
		p->deadline = (rq->clock >> sched_timeslice_shift) +
			p->static_prio - (MAX_PRIO - NICE_WIDTH);
}

static inline int normal_prio(struct task_struct *p)
{
	return task_has_rt_policy(p) ? (MAX_RT_PRIO - 1 - p->rt_priority) :
		MAX_RT_PRIO;
}

int task_running_nice(struct task_struct *p)
{
	return task_sched_prio(p) > DEFAULT_PRIO;
}

static inline void update_rq_time_edge(struct rq *rq)
{
	struct list_head head;
	u64 old = rq->time_edge;
	u64 now = rq->clock >> sched_timeslice_shift;
	u64 prio, delta;

	if (now == old)
		return;

	delta = min_t(u64, NORMAL_PRIO_NUM, now - old);
	INIT_LIST_HEAD(&head);

	for_each_set_bit(prio, &rq->queue.bitmap[2], delta)
		list_splice_tail_init(rq->queue.heads + MIN_NORMAL_PRIO +
				      NORMAL_PRIO_MOD(prio + old), &head);

	rq->queue.bitmap[2] = (NORMAL_PRIO_NUM == delta) ? 0UL :
		rq->queue.bitmap[2] >> delta;
	rq->time_edge = now;
	if (!list_empty(&head)) {
		u64 idx = MIN_NORMAL_PRIO + NORMAL_PRIO_MOD(now);
		struct task_struct *p;

		list_for_each_entry(p, &head, sq_node)
			p->sq_idx = idx;

		list_splice(&head, rq->queue.heads + idx);
		rq->queue.bitmap[2] |= 1UL;
	}
}

static inline void time_slice_expired(struct task_struct *p, struct rq *rq)
{
	p->time_slice = sched_timeslice_ns;
	sched_renew_deadline(p, rq);
	if (SCHED_FIFO != p->policy && task_on_rq_queued(p))
		requeue_task(p, rq);
}

static inline void sched_task_sanity_check(struct task_struct *p, struct rq *rq)
{
	u64 max_dl = rq->time_edge + NICE_WIDTH - 1;
	if (unlikely(p->deadline > max_dl))
		p->deadline = max_dl;
}

static void sched_task_fork(struct task_struct *p, struct rq *rq)
{
	sched_renew_deadline(p, rq);
}

static void do_sched_yield_type_1(struct task_struct *p, struct rq *rq)
{
	time_slice_expired(p, rq);
}

#ifdef CONFIG_SMP
static void sched_task_ttwu(struct task_struct *p) {}
#endif
static void sched_task_deactivate(struct task_struct *p, struct rq *rq) {}
