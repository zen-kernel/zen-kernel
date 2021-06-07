#define ALT_SCHED_VERSION_MSG "sched/bmq: BMQ CPU Scheduler "ALT_SCHED_VERSION" by Alfred Chen.\n"

/*
 * BMQ only routines
 */
#define rq_switch_time(rq)	((rq)->clock - (rq)->last_ts_switch)
#define boost_threshold(p)	(sched_timeslice_ns >>\
				 (15 - MAX_PRIORITY_ADJ -  (p)->boost_prio))

static inline void boost_task(struct task_struct *p)
{
	int limit;

	switch (p->policy) {
	case SCHED_NORMAL:
		limit = -MAX_PRIORITY_ADJ;
		break;
	case SCHED_BATCH:
	case SCHED_IDLE:
		limit = 0;
		break;
	default:
		return;
	}

	if (p->boost_prio > limit)
		p->boost_prio--;
}

static inline void deboost_task(struct task_struct *p)
{
	if (p->boost_prio < MAX_PRIORITY_ADJ)
		p->boost_prio++;
}

/*
 * Common interfaces
 */
static inline void sched_timeslice_imp(const int timeslice_ms) {}

static inline int
task_sched_prio_normal(const struct task_struct *p, const struct rq *rq)
{
	return p->prio + p->boost_prio - MAX_RT_PRIO;
}

static inline int task_sched_prio(const struct task_struct *p)
{
	return (p->prio < MAX_RT_PRIO)? p->prio : MAX_RT_PRIO / 2 + (p->prio + p->boost_prio) / 2;
}

static inline int
task_sched_prio_idx(const struct task_struct *p, const struct rq *rq)
{
	return task_sched_prio(p);
}

static inline unsigned long sched_prio2idx(unsigned long prio, struct rq *rq)
{
	return prio;
}

static inline unsigned long sched_idx2prio(unsigned long idx, struct rq *rq)
{
	return idx;
}

static inline int normal_prio(struct task_struct *p)
{
	if (task_has_rt_policy(p))
		return MAX_RT_PRIO - 1 - p->rt_priority;

	return p->static_prio + MAX_PRIORITY_ADJ;
}

static inline void time_slice_expired(struct task_struct *p, struct rq *rq)
{
	p->time_slice = sched_timeslice_ns;

	if (SCHED_FIFO != p->policy && task_on_rq_queued(p)) {
		if (SCHED_RR != p->policy)
			deboost_task(p);
		requeue_task(p, rq);
	}
}

static inline void sched_task_sanity_check(struct task_struct *p, struct rq *rq) {}

inline int task_running_nice(struct task_struct *p)
{
	return (p->prio + p->boost_prio > DEFAULT_PRIO + MAX_PRIORITY_ADJ);
}

static void sched_task_fork(struct task_struct *p, struct rq *rq)
{
	p->boost_prio = (p->boost_prio < 0) ?
		p->boost_prio + MAX_PRIORITY_ADJ : MAX_PRIORITY_ADJ;
}

static void do_sched_yield_type_1(struct task_struct *p, struct rq *rq)
{
	p->boost_prio = MAX_PRIORITY_ADJ;
}

#ifdef CONFIG_SMP
static void sched_task_ttwu(struct task_struct *p)
{
	if(this_rq()->clock_task - p->last_ran > sched_timeslice_ns)
		boost_task(p);
}
#endif

static void sched_task_deactivate(struct task_struct *p, struct rq *rq)
{
	if (rq_switch_time(rq) < boost_threshold(p))
		boost_task(p);
}

static inline void update_rq_time_edge(struct rq *rq) {}
