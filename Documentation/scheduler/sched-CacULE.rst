======================================
The CacULE Scheduler by Hamad Al Marri.
======================================

1.  Overview
=============

The CacULE CPU scheduler is based on interactivity score mechanism.
The interactivity score is inspired by the ULE scheduler (FreeBSD
scheduler).

1.1 About CacULE Scheduler
--------------------------

  - Each CPU has its own runqueue.

  - NORMAL runqueue is a linked list of sched_entities (instead of RB-Tree).

  - RT and other runqueues are just the same as the CFS's.

  - Wake up tasks preempt currently running tasks if its interactivity score value
    is higher.


1.2. Complexity
----------------

The complexity of Enqueue and Dequeue a task is O(1).

The complexity of pick the next task is in O(n), where n is the number of tasks
in a runqueue (each CPU has its own runqueue).

Note: O(n) sounds scary, but usually for a machine with 4 CPUS where it is used
for desktop or mobile jobs, the maximum number of runnable tasks might not
exceeds 10 (at the pick next run time) - the idle tasks are excluded since they
are dequeued when sleeping and enqueued when they wake up.


2. The CacULE Interactivity Score
=======================================================

The interactivity score is inspired by the ULE scheduler (FreeBSD scheduler).
For more information see: https://web.cs.ucdavis.edu/~roper/ecs150/ULE.pdf
CacULE doesn't replace CFS with ULE, it only changes the CFS' pick next task
mechanism to ULE's interactivity score mechanism for picking next task to run.


2.3 sched_interactivity_factor
=================
Sets the value *m* for interactivity score calculations. See Figure 1 in
https://web.cs.ucdavis.edu/~roper/ecs150/ULE.pdf
The default value of in CacULE is 10 which means that the Maximum Interactive
Score is 20 (since m = Maximum Interactive Score / 2).
You can tune sched_interactivity_factor with sysctl command:

	sysctl kernel.sched_interactivity_factor=50

This command changes the sched_interactivity_factor from 10 to 50.


3. Scheduling policies
=======================

CacULE some CFS, implements three scheduling policies:

  - SCHED_NORMAL (traditionally called SCHED_OTHER): The scheduling
    policy that is used for regular tasks.

  - SCHED_BATCH: Does not preempt nearly as often as regular tasks
    would, thereby allowing tasks to run longer and make better use of
    caches but at the cost of interactivity. This is well suited for
    batch jobs.

  - SCHED_IDLE: This is even weaker than nice 19, but its not a true
    idle timer scheduler in order to avoid to get into priority
    inversion problems which would deadlock the machine.
