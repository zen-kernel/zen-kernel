#ifndef BMQ_H
#define BMQ_H

/* bits:
 * RT(0-99), Low prio adj range, nice width, high prio adj range, cpu idle task */
#define SCHED_BITS	(MAX_RT_PRIO + NICE_WIDTH + 2 * MAX_PRIORITY_ADJ + 1)
#define IDLE_TASK_SCHED_PRIO	(SCHED_BITS - 1)

struct bmq {
	DECLARE_BITMAP(bitmap, SCHED_BITS);
	struct list_head heads[SCHED_BITS];
};

#endif
