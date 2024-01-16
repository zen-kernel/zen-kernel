#define ALT_SCHED_NAME "BMQ"

/*
 * BMQ only routines
 */
#define rq_switch_time(rq)	((rq)->clock - (rq)->last_ts_switch)
#define boost_threshold(p)	(sysctl_sched_base_slice >> ((14 - (p)->boost_prio) / 2))

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
	return (p->prio < MAX_RT_PRIO)? (p->prio >> 2) :
		MIN_SCHED_NORMAL_PRIO + (p->prio + p->boost_prio - MAX_RT_PRIO) / 2;
}

static inline int
task_sched_prio_idx(const struct task_struct *p, const struct rq *rq)
{
	return task_sched_prio(p);
}

static inline int sched_prio2idx(int prio, struct rq *rq)
{
	return prio;
}

static inline int sched_idx2prio(int idx, struct rq *rq)
{
	return idx;
}

inline int task_running_nice(struct task_struct *p)
{
	return (p->prio + p->boost_prio > DEFAULT_PRIO + MAX_PRIORITY_ADJ);
}

static inline void sched_update_rq_clock(struct rq *rq) {}
static inline void sched_task_renew(struct task_struct *p, const struct rq *rq) {}
static inline void sched_task_sanity_check(struct task_struct *p, struct rq *rq) {}

static void sched_task_fork(struct task_struct *p, struct rq *rq)
{
	p->boost_prio = MAX_PRIORITY_ADJ;
}

static inline void do_sched_yield_type_1(struct task_struct *p, struct rq *rq)
{
	p->boost_prio = MAX_PRIORITY_ADJ;
}

static inline void sched_task_ttwu(struct task_struct *p)
{
	if(this_rq()->clock_task - p->last_ran > sysctl_sched_base_slice)
		boost_task(p);
}

static inline void sched_task_deactivate(struct task_struct *p, struct rq *rq)
{
	u64 switch_ns = rq_switch_time(rq);

	if (switch_ns < boost_threshold(p))
		boost_task(p);
	else if (switch_ns > sysctl_sched_base_slice)
		deboost_task(p);
}
