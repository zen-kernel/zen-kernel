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
 *      2019-Dec-13: Add WAIT_MULTIPLE test by Krisman <krisman@collabora.com>
 *
 *****************************************************************************/

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "futextest.h"
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
	struct timespec to;
	time_t secs;
	struct futex_wait_block fwb = {&f1, f1, 0};
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
	ksft_set_plan(2);
	ksft_print_msg("%s: Block on a futex and wait for timeout\n",
	       basename(argv[0]));
	ksft_print_msg("\tArguments: timeout=%ldns\n", timeout_ns);

	/* initialize timeout */
	to.tv_sec = 0;
	to.tv_nsec = timeout_ns;

	info("Calling futex_wait on f1: %u @ %p\n", f1, &f1);
	res = futex_wait(&f1, f1, &to, FUTEX_PRIVATE_FLAG);
	if (!res || errno != ETIMEDOUT) {
		fail("futex_wait returned %d\n", ret < 0 ? errno : ret);
		ret = RET_FAIL;
	} else
		ksft_test_result_pass("futex_wait timeout succeeds\n");

	info("Calling futex_wait_multiple on f1: %u @ %p\n", f1, &f1);

	/* Setup absolute time */
	ret = clock_gettime(CLOCK_REALTIME, &to);
	secs = (to.tv_nsec + timeout_ns) / 1000000000;
	to.tv_nsec = ((int64_t)to.tv_nsec + timeout_ns) % 1000000000;
	to.tv_sec += secs;
	info("to.tv_sec  = %ld\n", to.tv_sec);
	info("to.tv_nsec = %ld\n", to.tv_nsec);

	res = futex_wait_multiple(&fwb, 1, &to,
				  FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);

#ifdef __ILP32__
	if (res == -1 && errno == ENOSYS) {
		ksft_test_result_skip("futex_wait_multiple not supported at x32\n");
	} else {
		ksft_test_result_fail("futex_wait_multiple returned %d\n",
				      res < 0 ? errno : res);
		ret = RET_FAIL;
	}
#else
	if (!res || errno != ETIMEDOUT) {
		ksft_test_result_fail("futex_wait_multiple returned %d\n",
				      res < 0 ? errno : res);
		ret = RET_FAIL;
	} else
		ksft_test_result_pass("futex_wait_multiple timeout succeeds\n");
#endif /* __ILP32__ */

	ksft_print_cnts();
	return ret;
}
