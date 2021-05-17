#ifndef PDS_H
#define PDS_H

/* bits:
 * RT(0-99), (Low prio adj range, nice width, high prio adj range) / 2, cpu idle task */
#define SCHED_BITS	(MAX_RT_PRIO + NICE_WIDTH / 2 + 1)
#define IDLE_TASK_SCHED_PRIO	(SCHED_BITS - 1)

struct sched_queue {
	DECLARE_BITMAP(bitmap, SCHED_BITS);
	struct list_head heads[SCHED_BITS];
};

#endif
