#define ALT_SCHED_VERSION_MSG "sched/bmq: PDS CPU Scheduler 5.8-r0 by Alfred Chen.\n"

static const u64 user_prio2deadline[NICE_WIDTH] = {
/* -20 */	  6291456,   6920601,   7612661,   8373927,   9211319,
/* -15 */	 10132450,  11145695,  12260264,  13486290,  14834919,
/* -10 */	 16318410,  17950251,  19745276,  21719803,  23891783,
/*  -5 */	 26280961,  28909057,  31799962,  34979958,  38477953,
/*   0 */	 42325748,  46558322,  51214154,  56335569,  61969125,
/*   5 */	 68166037,  74982640,  82480904,  90728994,  99801893,
/*  10 */	109782082, 120760290, 132836319, 146119950, 160731945,
/*  15 */	176805139, 194485652, 213934217, 235327638, 258860401
};

static const int dl_level_map[] = {
/*      0           4           8           12           */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1,
/*      16          20          24          28           */
	1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 5, 6, 7
};

static inline int
task_sched_prio(const struct task_struct *p, const struct rq *rq)
{
	u64 delta = (rq->clock + user_prio2deadline[39] - p->deadline) >> 23;

	delta = min((size_t)delta, ARRAY_SIZE(dl_level_map) - 1);

	return (p->prio < MAX_RT_PRIO)? p->prio : MAX_RT_PRIO + dl_level_map[delta];
}

static inline void update_task_priodl(struct task_struct *p)
{
	p->priodl = (((u64) (p->prio))<<56) | ((p->deadline)>>8);
}

static inline void requeue_task(struct task_struct *p, struct rq *rq);

static inline void time_slice_expired(struct task_struct *p, struct rq *rq)
{
	/*printk(KERN_INFO "sched: time_slice_expired(%d) - %px\n", cpu_of(rq), p);*/

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
	FULL_INIT_SKIPLIST_NODE(&rq->sl_header);
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

	FULL_INIT_SKIPLIST_NODE(&rq->sl_header);

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

static void sched_task_fork(struct task_struct *p) {}

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
