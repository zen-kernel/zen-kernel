// SPDX-License-Identifier: GPL-2.0-or-later
/******************************************************************************
 *
 *   Copyright Collabora Ltd., 2021
 *
 * DESCRIPTION
 *	Test waitv/wake mechanism of futex2, using 32bit sized futexes.
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
#include "futex2test.h"
#include "logging.h"

#define TEST_NAME "futex2-wait"
#define timeout_ns  1000000000
#define WAKE_WAIT_US 10000
#define NR_FUTEXES 30
struct futex_waitv waitv[NR_FUTEXES];
u_int32_t futexes[NR_FUTEXES] = {0};

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
	int res;

	/* setting absolute timeout for futex2 */
	if (gettime64(CLOCK_MONOTONIC, &to64))
		error("gettime64 failed\n", errno);

	to64.tv_sec++;

	res = futex2_waitv(waitv, NR_FUTEXES, 0, &to64);
	if (res < 0) {
		ksft_test_result_fail("futex2_waitv private returned: %d %s\n",
				      res ? errno : res,
				      res ? strerror(errno) : "");
	} else if (res != NR_FUTEXES - 1) {
		ksft_test_result_fail("futex2_waitv private returned: %d %s\n",
				      res ? errno : res,
				      res ? strerror(errno) : "");
	}

	return NULL;
}

int main(int argc, char *argv[])
{
	pthread_t waiter;
	int res, ret = RET_PASS;
	int c, i;

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
	ksft_print_msg("%s: Test FUTEX2_WAITV\n",
		       basename(argv[0]));

	for (i = 0; i < NR_FUTEXES; i++) {
		waitv[i].uaddr = &futexes[i];
		waitv[i].flags = FUTEX_32;
		waitv[i].val = 0;
	}

	/* Private waitv */
	if (pthread_create(&waiter, NULL, waiterfn, NULL))
		error("pthread_create failed\n", errno);

	usleep(WAKE_WAIT_US);

	res = futex2_wake(waitv[NR_FUTEXES - 1].uaddr, 1, FUTEX_32);
	if (res != 1) {
		ksft_test_result_fail("futex2_waitv private returned: %d %s\n",
				      res ? errno : res,
				      res ? strerror(errno) : "");
		ret = RET_FAIL;
	} else {
		ksft_test_result_pass("futex2_waitv private succeeds\n");
	}

	/* Shared waitv */
	for (i = 0; i < NR_FUTEXES; i++) {
		int shm_id = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0666);

		if (shm_id < 0) {
			perror("shmget");
			exit(1);
		}

		unsigned int *shared_data = shmat(shm_id, NULL, 0);

		*shared_data = 0;
		waitv[i].uaddr = shared_data;
		waitv[i].flags = FUTEX_32 | FUTEX_SHARED_FLAG;
		waitv[i].val = 0;
	}

	if (pthread_create(&waiter, NULL, waiterfn, NULL))
		error("pthread_create failed\n", errno);

	usleep(WAKE_WAIT_US);

	res = futex2_wake(waitv[NR_FUTEXES - 1].uaddr, 1, FUTEX_32 | FUTEX_SHARED_FLAG);
	if (res != 1) {
		ksft_test_result_fail("futex2_waitv shared returned: %d %s\n",
				      res ? errno : res,
				      res ? strerror(errno) : "");
		ret = RET_FAIL;
	} else {
		ksft_test_result_pass("futex2_waitv shared succeeds\n");
	}

	for (i = 0; i < NR_FUTEXES; i++)
		shmdt(waitv[i].uaddr);

	ksft_print_cnts();
	return ret;
}
