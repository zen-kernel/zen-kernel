#ifndef BMQ_H
#define BMQ_H

/* bits:
 * RT(0-99), (Low prio adj range, nice width, high prio adj range) / 2, cpu idle task */
#define SCHED_BITS	(MAX_RT_PRIO + NICE_WIDTH / 2 + MAX_PRIORITY_ADJ + 1)
#define IDLE_TASK_SCHED_PRIO	(SCHED_BITS - 1)

struct bmq {
	DECLARE_BITMAP(bitmap, SCHED_BITS);
	struct list_head heads[SCHED_BITS];
};


static inline int task_running_nice(struct task_struct *p)
{
	return (p->prio + p->boost_prio > DEFAULT_PRIO + MAX_PRIORITY_ADJ);
}

#endif
