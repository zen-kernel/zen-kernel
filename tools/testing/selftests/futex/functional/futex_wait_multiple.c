// SPDX-License-Identifier: GPL-2.0-or-later
/******************************************************************************
 *
 *   Copyright © Collabora, Ltd., 2019
 *
 * DESCRIPTION
 *      Test basic semantics of FUTEX_WAIT_MULTIPLE
 *
 * AUTHOR
 *      Gabriel Krisman Bertazi <krisman@collabora.com>
 *
 * HISTORY
 *      2019-Dec-13: Initial version by Krisman <krisman@collabora.com>
 *
 *****************************************************************************/

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include "futextest.h"
#include "logging.h"

#define TEST_NAME "futex-wait-multiple"
#define timeout_ns 100000
#define MAX_COUNT 128
#define WAKE_WAIT_US 3000000

int ret = RET_PASS;
char *progname;
futex_t f[MAX_COUNT] = {0};
struct futex_wait_block fwb[MAX_COUNT];

void usage(char *prog)
{
	printf("Usage: %s\n", prog);
	printf("  -c	Use color\n");
	printf("  -h	Display this help message\n");
	printf("  -v L	Verbosity level: %d=QUIET %d=CRITICAL %d=INFO\n",
	       VQUIET, VCRITICAL, VINFO);
}

void test_count_overflow(void)
{
	futex_t f = FUTEX_INITIALIZER;
	struct futex_wait_block fwb[MAX_COUNT+1];
	int res, i;

	ksft_print_msg("%s: Test a too big number of futexes\n", progname);

	for (i = 0; i < MAX_COUNT+1; i++) {
		fwb[i].uaddr = &f;
		fwb[i].val = f;
		fwb[i].bitset = 0;
	}

	res = futex_wait_multiple(fwb, MAX_COUNT+1, NULL, FUTEX_PRIVATE_FLAG);

#ifdef __ILP32__
	if (res != -1 || errno != ENOSYS) {
		ksft_test_result_fail("futex_wait_multiple returned %d\n",
				      res < 0 ? errno : res);
		ret = RET_FAIL;
	} else {
		ksft_test_result_skip("futex_wait_multiple not supported at x32\n");
	}
#else
	if (res != -1 || errno != EINVAL) {
		ksft_test_result_fail("futex_wait_multiple returned %d\n",
				      res < 0 ? errno : res);
		ret = RET_FAIL;
	} else {
		ksft_test_result_pass("futex_wait_multiple count overflow succeed\n");
	}

#endif /* __ILP32__ */
}

void *waiterfn(void *arg)
{
	int res;

	res = futex_wait_multiple(fwb, MAX_COUNT, NULL, FUTEX_PRIVATE_FLAG);

#ifdef __ILP32__
	if (res != -1 || errno != ENOSYS) {
		ksft_test_result_fail("futex_wait_multiple returned %d\n",
				      res < 0 ? errno : res);
		ret = RET_FAIL;
	} else {
		ksft_test_result_skip("futex_wait_multiple not supported at x32\n");
	}
#else
	if (res < 0)
		ksft_print_msg("waiter failed %d\n", res);

	info("futex_wait_multiple: Got hint futex %d was freed\n", res);
#endif /* __ILP32__ */

	return NULL;
}

void test_fwb_wakeup(void)
{
	int res, i;
	pthread_t waiter;

	ksft_print_msg("%s: Test wake up in a list of futex\n", progname);

	for (i = 0; i < MAX_COUNT; i++) {
		fwb[i].uaddr = &f[i];
		fwb[i].val = f[i];
		fwb[i].bitset = 0xffffffff;
	}

	res = pthread_create(&waiter, NULL, waiterfn, NULL);
	if (res) {
		ksft_test_result_fail("Creating waiting thread failed");
		ksft_exit_fail();
	}

	usleep(WAKE_WAIT_US);
	res = futex_wake(&(f[MAX_COUNT-1]), 1, FUTEX_PRIVATE_FLAG);
	if (res != 1) {
		ksft_test_result_fail("Failed to wake thread res=%d\n", res);
		ksft_exit_fail();
	}

	pthread_join(waiter, NULL);
	ksft_test_result_pass("%s succeed\n", __func__);
}

int main(int argc, char *argv[])
{
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

	progname = basename(argv[0]);

	ksft_print_header();
	ksft_set_plan(2);

	test_count_overflow();

#ifdef __ILP32__
	// if it's a 32x binary, there's no futex to wakeup
	ksft_test_result_skip("futex_wait_multiple not supported at x32\n");
#else
	test_fwb_wakeup();
#endif /* __ILP32__ */

	ksft_print_cnts();
	return ret;
}
