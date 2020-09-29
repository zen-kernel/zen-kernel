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

static const unsigned char dl_level_map[] = {
/*       0               4               8              12           */
	19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 18,
/*      16              20              24              28           */
	18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 17, 17, 17, 17, 17,
/*      32              36              40              44           */
	17, 17, 17, 17, 16, 16, 16, 16, 16, 16, 16, 16, 15, 15, 15, 15,
/*      48              52              56              60           */
	15, 15, 15, 14, 14, 14, 14, 14, 14, 13, 13, 13, 13, 12, 12, 12,
/*      64              68              72              76           */
	12, 11, 11, 11, 10, 10, 10,  9,  9,  8,  7,  6,  5,  4,  3,  2,
/*      80              84              88              92           */
	 1,  0
};

static inline int
task_sched_prio(const struct task_struct *p, const struct rq *rq)
{
	size_t delta;

	if (p == rq->idle)
		return IDLE_TASK_SCHED_PRIO;

	if (p->prio < MAX_RT_PRIO)
		return p->prio;

	delta = (rq->clock + user_prio2deadline[39] - p->deadline) >> 21;
	delta = min((size_t)delta, ARRAY_SIZE(dl_level_map) - 1);

	return MAX_RT_PRIO + dl_level_map[delta];
}

static inline void update_task_priodl(struct task_struct *p)
{
	p->priodl = (((u64) (p->prio))<<56) | ((p->deadline)>>8);
}

static inline void requeue_task(struct task_struct *p, struct rq *rq);

static inline void time_slice_expired(struct task_struct *p, struct rq *rq)
{
	/*printk(KERN_INFO "sched: time_slice_expired(%d) - %px\n", cpu_of(rq), p);*/
	p->time_slice = sched_timeslice_ns;

	if (p->prio >= MAX_RT_PRIO)
		p->deadline = rq->clock + user_prio2deadline[TASK_USER_PRIO(p)];
	update_task_priodl(p);

	if (SCHED_FIFO != p->policy && task_on_rq_queued(p))
		requeue_task(p, rq);
}

/*
 * pds_skiplist_task_search -- search function used in PDS run queue skip list
 * node insert operation.
 * @it: iterator pointer to the node in the skip list
 * @node: pointer to the skiplist_node to be inserted
 *
 * Returns true if key of @it is less or equal to key value of @node, otherwise
 * false.
 */
static inline bool
pds_skiplist_task_search(struct skiplist_node *it, struct skiplist_node *node)
{
	return (skiplist_entry(it, struct task_struct, sl_node)->priodl <=
		skiplist_entry(node, struct task_struct, sl_node)->priodl);
}

/*
 * Define the skip list insert function for PDS
 */
DEFINE_SKIPLIST_INSERT_FUNC(pds_skiplist_insert, pds_skiplist_task_search);

/*
 * Init the queue structure in rq
 */
static inline void sched_queue_init(struct rq *rq)
{
	INIT_SKIPLIST_NODE(&rq->sl_header);
}

/*
 * Init idle task and put into queue structure of rq
 * IMPORTANT: may be called multiple times for a single cpu
 */
static inline void sched_queue_init_idle(struct rq *rq, struct task_struct *idle)
{
	/*printk(KERN_INFO "sched: init(%d) - %px\n", cpu_of(rq), idle);*/
	int default_prio = idle->prio;

	idle->prio = MAX_PRIO;
	idle->deadline = 0ULL;
	update_task_priodl(idle);

	INIT_SKIPLIST_NODE(&rq->sl_header);

	idle->sl_node.level = idle->sl_level;
	pds_skiplist_insert(&rq->sl_header, &idle->sl_node);

	idle->prio = default_prio;
}

/*
 * This routine assume that the idle task always in queue
 */
static inline struct task_struct *sched_rq_first_task(struct rq *rq)
{
	struct skiplist_node *node = rq->sl_header.next[0];

	BUG_ON(node == &rq->sl_header);
	return skiplist_entry(node, struct task_struct, sl_node);
}

static inline struct task_struct *
sched_rq_next_task(struct task_struct *p, struct rq *rq)
{
	struct skiplist_node *next = p->sl_node.next[0];

	BUG_ON(next == &rq->sl_header);
	return skiplist_entry(next, struct task_struct, sl_node);
}

static inline unsigned long sched_queue_watermark(struct rq *rq)
{
	return task_sched_prio(sched_rq_first_task(rq), rq);
}

#define __SCHED_DEQUEUE_TASK(p, rq, flags, func)		\
	psi_dequeue(p, flags & DEQUEUE_SLEEP);			\
	sched_info_dequeued(rq, p);				\
								\
	if (skiplist_del_init(&rq->sl_header, &p->sl_node)) {	\
		func;						\
	}

#define __SCHED_ENQUEUE_TASK(p, rq, flags)				\
	sched_info_queued(rq, p);					\
	psi_enqueue(p, flags);						\
									\
	p->sl_node.level = p->sl_level;					\
	pds_skiplist_insert(&rq->sl_header, &p->sl_node)

/*
 * Requeue a task @p to @rq
 */
#define __SCHED_REQUEUE_TASK(p, rq, func)					\
{\
	bool b_first = skiplist_del_init(&rq->sl_header, &p->sl_node);		\
\
	p->sl_node.level = p->sl_level;						\
	if (pds_skiplist_insert(&rq->sl_header, &p->sl_node) || b_first) {	\
		func;								\
	}									\
}

static inline bool sched_task_need_requeue(struct task_struct *p, struct rq *rq)
{
	struct skiplist_node *node = p->sl_node.prev[0];

	if (node != &rq->sl_header) {
		struct task_struct *t = skiplist_entry(node, struct task_struct, sl_node);

		if (t->priodl > p->priodl)
			return true;
	}

	node = p->sl_node.next[0];
	if (node != &rq->sl_header) {
		struct task_struct *t = skiplist_entry(node, struct task_struct, sl_node);

		if (t->priodl < p->priodl)
			return true;
	}

	return false;
}

/*
 * pds_skiplist_random_level -- Returns a pseudo-random level number for skip
 * list node which is used in PDS run queue.
 *
 * In current implementation, based on testing, the first 8 bits in microseconds
 * of niffies are suitable for random level population.
 * find_first_bit() is used to satisfy p = 0.5 between each levels, and there
 * should be platform hardware supported instruction(known as ctz/clz) to speed
 * up this function.
 * The skiplist level for a task is populated when task is created and doesn't
 * change in task's life time. When task is being inserted into run queue, this
 * skiplist level is set to task's sl_node->level, the skiplist insert function
 * may change it based on current level of the skip lsit.
 */
static inline int pds_skiplist_random_level(const struct task_struct *p)
{
	long unsigned int randseed;

	/*
	 * 1. Some architectures don't have better than microsecond resolution
	 * so mask out ~microseconds as a factor of the random seed for skiplist
	 * insertion.
	 * 2. Use address of task structure pointer as another factor of the
	 * random seed for task burst forking scenario.
	 */
	randseed = (task_rq(p)->clock ^ (long unsigned int)p) >> 10;

	return find_first_bit(&randseed, NUM_SKIPLIST_LEVEL - 1);
}

static void sched_task_fork(struct task_struct *p, struct rq *rq)
{
	p->sl_level = pds_skiplist_random_level(p);
	if (p->prio >= MAX_RT_PRIO)
		p->deadline = rq->clock + user_prio2deadline[TASK_USER_PRIO(p)];
	update_task_priodl(p);
}

/**
 * task_prio - return the priority value of a given task.
 * @p: the task in question.
 *
 * Return: The priority value as seen by users in /proc.
 * RT tasks are offset by -100. Normal tasks are centered around 1, value goes
 * from 0(SCHED_ISO) up to 82 (nice +19 SCHED_IDLE).
 */
int task_prio(const struct task_struct *p)
{
	int ret;

	if (p->prio < MAX_RT_PRIO)
		return (p->prio - MAX_RT_PRIO);

	preempt_disable();
	ret = task_sched_prio(p, this_rq()) - MAX_RT_PRIO;
	preempt_enable();

	return ret;
}

static void do_sched_yield_type_1(struct task_struct *p, struct rq *rq)
{
	time_slice_expired(p, rq);
}

static void sched_task_ttwu(struct task_struct *p) {}
static void sched_task_deactivate(struct task_struct *p, struct rq *rq) {}
