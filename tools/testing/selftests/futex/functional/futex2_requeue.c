// SPDX-License-Identifier: GPL-2.0-or-later
/******************************************************************************
 *
 *   Copyright Collabora Ltd., 2021
 *
 * DESCRIPTION
 *	Test requeue mechanism of futex2, using 32bit sized futexes.
 *
 * AUTHOR
 *	André Almeida <andrealmeid@collabora.com>
 *
 * HISTORY
 *      2021-Feb-5: Initial version by André <andrealmeid@collabora.com>
 *
 *****************************************************************************/

#include <errno.h>
#include <error.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sys/shm.h>
#include <limits.h>
#include "futex2test.h"
#include "logging.h"

#define TEST_NAME "futex2-wait"
#define timeout_ns  30000000
#define WAKE_WAIT_US 10000
volatile futex_t *f1;

void usage(char *prog)
{
	printf("Usage: %s\n", prog);
	printf("  -c	Use color\n");
	printf("  -h	Display this help message\n");
	printf("  -v L	Verbosity level: %d=QUIET %d=CRITICAL %d=INFO\n",
	       VQUIET, VCRITICAL, VINFO);
}

void *waiterfn(void *arg)
{
	struct timespec64 to64;

	/* setting absolute timeout for futex2 */
	if (gettime64(CLOCK_MONOTONIC, &to64))
		error("gettime64 failed\n", errno);

	to64.tv_nsec += timeout_ns;

	if (to64.tv_nsec >= 1000000000) {
		to64.tv_sec++;
		to64.tv_nsec -= 1000000000;
	}

	if (futex2_wait(f1, *f1, FUTEX_32, &to64))
		printf("waiter failed errno %d\n", errno);

	return NULL;
}

int main(int argc, char *argv[])
{
	pthread_t waiter[10];
	int res, ret = RET_PASS;
	int c, i;
	volatile futex_t _f1 = 0;
	volatile futex_t f2 = 0;
	struct futex_requeue r1, r2;

	f1 = &_f1;

	r1.flags = FUTEX_32;
	r2.flags = FUTEX_32;

	r1.uaddr = f1;
	r2.uaddr = &f2;

	while ((c = getopt(argc, argv, "cht:v:")) != -1) {
		switch (c) {
		case 'c':
			log_color(1);
			break;
		case 'h':
			usage(basename(argv[0]));
			exit(0);
		case 'v':
			log_verbosity(atoi(optarg));
			break;
		default:
			usage(basename(argv[0]));
			exit(1);
		}
	}

	ksft_print_header();
	ksft_set_plan(2);
	ksft_print_msg("%s: Test FUTEX2_REQUEUE\n",
		       basename(argv[0]));

	/*
	 * Requeue a waiter from f1 to f2, and wake f2.
	 */
	if (pthread_create(&waiter[0], NULL, waiterfn, NULL))
		error("pthread_create failed\n", errno);

	usleep(WAKE_WAIT_US);

	res = futex2_requeue(&r1, &r2, 0, 1, 0, 0);
	if (res != 1) {
		ksft_test_result_fail("futex2_requeue private returned: %d %s\n",
				      res ? errno : res,
				      res ? strerror(errno) : "");
		ret = RET_FAIL;
	}


	info("Calling private futex2_wake on f2: %u @ %p with val=%u\n", f2, &f2, f2);
	res = futex2_wake(&f2, 1, FUTEX_32);
	if (res != 1) {
		ksft_test_result_fail("futex2_requeue private returned: %d %s\n",
				      res ? errno : res,
				      res ? strerror(errno) : "");
		ret = RET_FAIL;
	} else {
		ksft_test_result_pass("futex2_requeue simple succeeds\n");
	}


	/*
	 * Create 10 waiters at f1. At futex_requeue, wake 3 and requeue 7.
	 * At futex_wake, wake INT_MAX (should be exaclty 7).
	 */
	for (i = 0; i < 10; i++) {
		if (pthread_create(&waiter[i], NULL, waiterfn, NULL))
			error("pthread_create failed\n", errno);
	}

	usleep(WAKE_WAIT_US);

	res = futex2_requeue(&r1, &r2, 3, 7, 0, 0);
	if (res != 10) {
		ksft_test_result_fail("futex2_requeue private returned: %d %s\n",
				      res ? errno : res,
				      res ? strerror(errno) : "");
		ret = RET_FAIL;
	}

	res = futex2_wake(&f2, INT_MAX, FUTEX_32);
	if (res != 7) {
		ksft_test_result_fail("futex2_requeue private returned: %d %s\n",
				      res ? errno : res,
				      res ? strerror(errno) : "");
		ret = RET_FAIL;
	} else {
		ksft_test_result_pass("futex2_requeue succeeds\n");
	}

	ksft_print_cnts();
	return ret;
}
