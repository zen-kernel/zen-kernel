// SPDX-License-Identifier: GPL-2.0-or-later
/******************************************************************************
 *
 *   Copyright © International Business Machines  Corp., 2009
 *
 * DESCRIPTION
 *      Block on a futex and wait for timeout.
 *
 * AUTHOR
 *      Darren Hart <dvhart@linux.intel.com>
 *
 * HISTORY
 *      2009-Nov-6: Initial version by Darren Hart <dvhart@linux.intel.com>
 *      2021-Feb-5: Add futex2 test by André <andrealmeid@collabora.com>
 *
 *****************************************************************************/

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "futex2test.h"
#include "logging.h"

#define TEST_NAME "futex-wait-timeout"

static long timeout_ns = 100000;	/* 100us default timeout */

void usage(char *prog)
{
	printf("Usage: %s\n", prog);
	printf("  -c	Use color\n");
	printf("  -h	Display this help message\n");
	printf("  -t N	Timeout in nanoseconds (default: 100,000)\n");
	printf("  -v L	Verbosity level: %d=QUIET %d=CRITICAL %d=INFO\n",
	       VQUIET, VCRITICAL, VINFO);
}

int main(int argc, char *argv[])
{
	futex_t f1 = FUTEX_INITIALIZER;
	struct timespec to = {.tv_sec = 0, .tv_nsec = timeout_ns};
	struct timespec64 to64;
	int res, ret = RET_PASS;
	int c;

	while ((c = getopt(argc, argv, "cht:v:")) != -1) {
		switch (c) {
		case 'c':
			log_color(1);
			break;
		case 'h':
			usage(basename(argv[0]));
			exit(0);
		case 't':
			timeout_ns = atoi(optarg);
			break;
		case 'v':
			log_verbosity(atoi(optarg));
			break;
		default:
			usage(basename(argv[0]));
			exit(1);
		}
	}

	ksft_print_header();
	ksft_set_plan(3);
	ksft_print_msg("%s: Block on a futex and wait for timeout\n",
	       basename(argv[0]));
	ksft_print_msg("\tArguments: timeout=%ldns\n", timeout_ns);

	info("Calling futex_wait on f1: %u @ %p\n", f1, &f1);
	res = futex_wait(&f1, f1, &to, FUTEX_PRIVATE_FLAG);
	if (!res || errno != ETIMEDOUT) {
		ksft_test_result_fail("futex_wait returned %d\n", ret < 0 ? errno : ret);
		ret = RET_FAIL;
	} else {
		ksft_test_result_pass("futex_wait timeout succeeds\n");
	}

	/* setting absolute monotonic timeout for futex2 */
	if (gettime64(CLOCK_MONOTONIC, &to64))
		error("gettime64 failed\n", errno);

	to64.tv_nsec += timeout_ns;

	if (to64.tv_nsec >= 1000000000) {
		to64.tv_sec++;
		to64.tv_nsec -= 1000000000;
	}

	info("Calling futex2_wait on f1: %u @ %p\n", f1, &f1);
	res = futex2_wait(&f1, f1, FUTEX_32, &to64);
	if (!res || errno != ETIMEDOUT) {
		ksft_test_result_fail("futex2_wait monotonic returned %d\n", ret < 0 ? errno : ret);
		ret = RET_FAIL;
	} else {
		ksft_test_result_pass("futex2_wait monotonic timeout succeeds\n");
	}

	/* setting absolute realtime timeout for futex2 */
	if (gettime64(CLOCK_REALTIME, &to64))
		error("gettime64 failed\n", errno);

	to64.tv_nsec += timeout_ns;

	if (to64.tv_nsec >= 1000000000) {
		to64.tv_sec++;
		to64.tv_nsec -= 1000000000;
	}

	info("Calling futex2_wait on f1: %u @ %p\n", f1, &f1);
	res = futex2_wait(&f1, f1, FUTEX_32 | FUTEX_CLOCK_REALTIME, &to64);
	if (!res || errno != ETIMEDOUT) {
		ksft_test_result_fail("futex2_wait realtime returned %d\n", ret < 0 ? errno : ret);
		ret = RET_FAIL;
	} else {
		ksft_test_result_pass("futex2_wait realtime timeout succeeds\n");
	}

	ksft_print_cnts();
	return ret;
}
