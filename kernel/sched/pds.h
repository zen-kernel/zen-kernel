#ifndef PDS_H
#define PDS_H

/* bits:
 * RT(0-99), (Low prio adj range, nice width, high prio adj range) / 2, cpu idle task */
#define SCHED_BITS	(MAX_RT_PRIO + MAX_PRIORITY_ADJ * 2 + 8 + 1)
#define IDLE_TASK_SCHED_PRIO	(SCHED_BITS - 1)

static inline int task_running_nice(struct task_struct *p)
{
	return (p->prio > DEFAULT_PRIO);
}

#endif
