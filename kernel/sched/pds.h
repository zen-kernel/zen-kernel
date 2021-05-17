#ifndef PDS_H
#define PDS_H

/* bits:
 * RT(0-99), (Low prio adj range, nice width, high prio adj range) / 2, cpu idle task */
#define SCHED_BITS	(MAX_RT_PRIO + NICE_WIDTH / 2 + 1)
#define IDLE_TASK_SCHED_PRIO	(SCHED_BITS - 1)

#endif
