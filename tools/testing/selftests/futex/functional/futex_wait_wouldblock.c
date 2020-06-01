// SPDX-License-Identifier: GPL-2.0-or-later
/******************************************************************************
 *
 *   Copyright © International Business Machines  Corp., 2009
 *
 * DESCRIPTION
 *      Test if FUTEX_WAIT op returns -EWOULDBLOCK if the futex value differs
 *      from the expected one.
 *
 * AUTHOR
 *      Gowrishankar <gowrishankar.m@in.ibm.com>
 *
 * HISTORY
 *      2009-Nov-14: Initial version by Gowrishankar <gowrishankar.m@in.ibm.com>
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

#define TEST_NAME "futex-wait-wouldblock"
#define timeout_ns 100000

void usage(char *prog)
{
	printf("Usage: %s\n", prog);
	printf("  -c	Use color\n");
	printf("  -h	Display this help message\n");
	printf("  -v L	Verbosity level: %d=QUIET %d=CRITICAL %d=INFO\n",
	       VQUIET, VCRITICAL, VINFO);
}

int main(int argc, char *argv[])
{
	struct timespec to = {.tv_sec = 0, .tv_nsec = timeout_ns};
	futex_t f1 = FUTEX_INITIALIZER;
	struct futex_wait_block fwb = {&f1, f1+1, 0};
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
	ksft_print_msg("%s: Test the unexpected futex value in FUTEX_WAIT\n",
	       basename(argv[0]));

	info("Calling futex_wait on f1: %u @ %p with val=%u\n", f1, &f1, f1+1);
	res = futex_wait(&f1, f1+1, &to, FUTEX_PRIVATE_FLAG);
	if (!res || errno != EWOULDBLOCK) {
		fail("futex_wait returned: %d %s\n",
		     res ? errno : res, res ? strerror(errno) : "");
		ret = RET_FAIL;
	} else
		ksft_test_result_pass("futex_wait wouldblock succeeds\n");

	info("Calling futex_wait_multiple on f1: %u @ %p with val=%u\n",
	     f1, &f1, f1+1);
	res = futex_wait_multiple(&fwb, 1, NULL, FUTEX_PRIVATE_FLAG);

#ifdef __ILP32__
	if (res != -1 || errno != ENOSYS) {
		ksft_test_result_fail("futex_wait_multiple returned %d\n",
				      res < 0 ? errno : res);
		ret = RET_FAIL;
	} else {
		ksft_test_result_skip("futex_wait_multiple not supported at x32\n");
	}
#else
	if (!res || errno != EWOULDBLOCK) {
		ksft_test_result_fail("futex_wait_multiple returned %d\n",
				      res < 0 ? errno : res);
		ret = RET_FAIL;
	}
	ksft_test_result_pass("futex_wait_multiple wouldblock succeeds\n");
#endif /* __ILP32__ */

	ksft_print_cnts();
	return ret;
}
