#define ALT_SCHED_NAME "PDS"

#define MIN_SCHED_NORMAL_PRIO	(32)
static const u64 RT_MASK = ((1ULL << MIN_SCHED_NORMAL_PRIO) - 1);

#define SCHED_NORMAL_PRIO_NUM	(32)
#define SCHED_EDGE_DELTA	(SCHED_NORMAL_PRIO_NUM - NICE_WIDTH / 2)

/* PDS assume NORMAL_PRIO_NUM is power of 2 */
#define SCHED_NORMAL_PRIO_MOD(x)	((x) & (SCHED_NORMAL_PRIO_NUM - 1))

/* default time slice 4ms -> shift 22, 2 time slice slots -> shift 23 */
static __read_mostly int sched_timeslice_shift = 23;

/*
 * Common interfaces
 */
static inline void sched_timeslice_imp(const int timeslice_ms)
{
	if (2 == timeslice_ms)
		sched_timeslice_shift = 22;
}

static inline int
task_sched_prio_normal(const struct task_struct *p, const struct rq *rq)
{
	s64 delta = p->deadline - rq->time_edge + SCHED_EDGE_DELTA;

#ifdef ALT_SCHED_DEBUG
	if (WARN_ONCE(delta > NORMAL_PRIO_NUM - 1,
		      "pds: task_sched_prio_normal() delta %lld\n", delta))
		return SCHED_NORMAL_PRIO_NUM - 1;
#endif

	return max(0LL, delta);
}

static inline int task_sched_prio(const struct task_struct *p)
{
	return (p->prio < MIN_NORMAL_PRIO) ? (p->prio >> 2) :
		MIN_SCHED_NORMAL_PRIO + task_sched_prio_normal(p, task_rq(p));
}

static inline int
task_sched_prio_idx(const struct task_struct *p, const struct rq *rq)
{
	u64 idx;

	if (p->prio < MIN_NORMAL_PRIO)
		return p->prio >> 2;

	idx = max(p->deadline + SCHED_EDGE_DELTA, rq->time_edge);
	/*printk(KERN_INFO "sched: task_sched_prio_idx edge:%llu, deadline=%llu idx=%llu\n", rq->time_edge, p->deadline, idx);*/
	return MIN_SCHED_NORMAL_PRIO + SCHED_NORMAL_PRIO_MOD(idx);
}

static inline int sched_prio2idx(int sched_prio, struct rq *rq)
{
	return (IDLE_TASK_SCHED_PRIO == sched_prio || sched_prio < MIN_SCHED_NORMAL_PRIO) ?
		sched_prio :
		MIN_SCHED_NORMAL_PRIO + SCHED_NORMAL_PRIO_MOD(sched_prio + rq->time_edge);
}

static inline int sched_idx2prio(int sched_idx, struct rq *rq)
{
	return (sched_idx < MIN_SCHED_NORMAL_PRIO) ?
		sched_idx :
		MIN_SCHED_NORMAL_PRIO + SCHED_NORMAL_PRIO_MOD(sched_idx - rq->time_edge);
}

static inline void sched_renew_deadline(struct task_struct *p, const struct rq *rq)
{
	if (p->prio >= MIN_NORMAL_PRIO)
		p->deadline = rq->time_edge + (p->static_prio - (MAX_PRIO - NICE_WIDTH)) / 2;
}

int task_running_nice(struct task_struct *p)
{
	return (p->prio > DEFAULT_PRIO);
}

static inline void update_rq_time_edge(struct rq *rq)
{
	struct list_head head;
	u64 old = rq->time_edge;
	u64 now = rq->clock >> sched_timeslice_shift;
	u64 prio, delta;
	DECLARE_BITMAP(normal, SCHED_QUEUE_BITS);

	if (now == old)
		return;

	rq->time_edge = now;
	delta = min_t(u64, SCHED_NORMAL_PRIO_NUM, now - old);
	INIT_LIST_HEAD(&head);

	/*printk(KERN_INFO "sched: update_rq_time_edge 0x%016lx %llu\n", rq->queue.bitmap[0], delta);*/
	prio = MIN_SCHED_NORMAL_PRIO;
	for_each_set_bit_from(prio, rq->queue.bitmap, MIN_SCHED_NORMAL_PRIO + delta)
		list_splice_tail_init(rq->queue.heads + MIN_SCHED_NORMAL_PRIO +
				      SCHED_NORMAL_PRIO_MOD(prio + old), &head);

	bitmap_shift_right(normal, rq->queue.bitmap, delta, SCHED_QUEUE_BITS);
	if (!list_empty(&head)) {
		struct task_struct *p;
		u64 idx = MIN_SCHED_NORMAL_PRIO + SCHED_NORMAL_PRIO_MOD(now);

		list_for_each_entry(p, &head, sq_node)
			p->sq_idx = idx;

		list_splice(&head, rq->queue.heads + idx);
		set_bit(MIN_SCHED_NORMAL_PRIO, normal);
	}
	bitmap_replace(rq->queue.bitmap, normal, rq->queue.bitmap,
		       (const unsigned long *)&RT_MASK, SCHED_QUEUE_BITS);

	if (rq->prio < MIN_SCHED_NORMAL_PRIO || IDLE_TASK_SCHED_PRIO == rq->prio)
		return;

	rq->prio = (rq->prio < MIN_SCHED_NORMAL_PRIO + delta) ?
		MIN_SCHED_NORMAL_PRIO : rq->prio - delta;
}

static inline void time_slice_expired(struct task_struct *p, struct rq *rq)
{
	p->time_slice = sched_timeslice_ns;
	sched_renew_deadline(p, rq);
	if (SCHED_FIFO != p->policy && task_on_rq_queued(p))
		requeue_task(p, rq, task_sched_prio_idx(p, rq));
}

static inline void sched_task_sanity_check(struct task_struct *p, struct rq *rq)
{
	u64 max_dl = rq->time_edge + NICE_WIDTH / 2 - 1;
	if (unlikely(p->deadline > max_dl))
		p->deadline = max_dl;
}

static void sched_task_fork(struct task_struct *p, struct rq *rq)
{
	sched_renew_deadline(p, rq);
}

static inline void do_sched_yield_type_1(struct task_struct *p, struct rq *rq)
{
	time_slice_expired(p, rq);
}

#ifdef CONFIG_SMP
static inline void sched_task_ttwu(struct task_struct *p) {}
#endif
static inline void sched_task_deactivate(struct task_struct *p, struct rq *rq) {}
