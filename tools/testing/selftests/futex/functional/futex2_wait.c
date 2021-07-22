// SPDX-License-Identifier: GPL-2.0-or-later
/******************************************************************************
 *
 *   Copyright Collabora Ltd., 2021
 *
 * DESCRIPTION
 *	Test wait/wake mechanism of futex2, using 32bit sized futexes.
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
#include <sys/mman.h>
#include <fcntl.h>
#include <string.h>
#include "futex2test.h"
#include "logging.h"

#define TEST_NAME "futex2-wait"
#define timeout_ns  30000000
#define WAKE_WAIT_US 10000
#define SHM_PATH "futex2_shm_file"

void *futex;

void usage(char *prog)
{
	printf("Usage: %s\n", prog);
	printf("  -c	Use color\n");
	printf("  -h	Display this help message\n");
	printf("  -v L	Verbosity level: %d=QUIET %d=CRITICAL %d=INFO\n",
	       VQUIET, VCRITICAL, VINFO);
}

static void *waiterfn(void *arg)
{
	struct timespec64 to64;
	unsigned int flags = 0;

	if (arg)
		flags = *((unsigned int *) arg);

	/* setting absolute timeout for futex2 */
	if (gettime64(CLOCK_MONOTONIC, &to64))
		error("gettime64 failed\n", errno);

	to64.tv_nsec += timeout_ns;

	if (to64.tv_nsec >= 1000000000) {
		to64.tv_sec++;
		to64.tv_nsec -= 1000000000;
	}

	if (futex2_wait(futex, 0, FUTEX_32 | flags, &to64))
		printf("waiter failed errno %d\n", errno);

	return NULL;
}

int main(int argc, char *argv[])
{
	unsigned int flags = FUTEX_SHARED_FLAG;
	int res, ret = RET_PASS, fd, c, shm_id;
	u_int32_t f_private = 0, *shared_data;
	pthread_t waiter;
	void *shm;

	futex = &f_private;

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
	ksft_set_plan(3);
	ksft_print_msg("%s: Test FUTEX2_WAIT\n", basename(argv[0]));

	/* Testing a private futex */
	info("Calling private futex2_wait on futex: %p\n", futex);
	if (pthread_create(&waiter, NULL, waiterfn, NULL))
		error("pthread_create failed\n", errno);

	usleep(WAKE_WAIT_US);

	info("Calling private futex2_wake on futex: %p\n", futex);
	res = futex2_wake(futex, 1, FUTEX_32);
	if (res != 1) {
		ksft_test_result_fail("futex2_wake private returned: %d %s\n",
				      errno, strerror(errno));
		ret = RET_FAIL;
	} else {
		ksft_test_result_pass("futex2_wake private succeeds\n");
	}

	/* Testing an anon page shared memory */
	shm_id = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0666);
	if (shm_id < 0) {
		perror("shmget");
		exit(1);
	}

	shared_data = shmat(shm_id, NULL, 0);

	*shared_data = 0;
	futex = shared_data;

	info("Calling (page anon) shared futex2_wait on futex: %p\n", futex);
	if (pthread_create(&waiter, NULL, waiterfn, &flags))
		error("pthread_create failed\n", errno);

	usleep(WAKE_WAIT_US);

	info("Calling (page anon) shared futex2_wake on futex: %p\n", futex);
	res = futex2_wake(futex, 1, FUTEX_32 | FUTEX_SHARED_FLAG);
	if (res != 1) {
		ksft_test_result_fail("futex2_wake shared (page anon) returned: %d %s\n",
				      errno, strerror(errno));
		ret = RET_FAIL;
	} else {
		ksft_test_result_pass("futex2_wake shared (page anon) succeeds\n");
	}


	/* Testing a file backed shared memory */
	fd = open(SHM_PATH, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		perror("open");
		exit(1);
	}

	if (ftruncate(fd, sizeof(f_private))) {
		perror("ftruncate");
		exit(1);
	}

	shm = mmap(NULL, sizeof(f_private), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (shm == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}

	memcpy(shm, &f_private, sizeof(f_private));

	futex = shm;

	info("Calling shared (file backed) futex2_wait on futex: %p\n", futex);
	if (pthread_create(&waiter, NULL, waiterfn, &flags))
		error("pthread_create failed\n", errno);

	usleep(WAKE_WAIT_US);

	info("Calling shared (file backed) futex2_wake on futex: %p\n", futex);
	res = futex2_wake(shm, 1, FUTEX_32 | FUTEX_SHARED_FLAG);
	if (res != 1) {
		ksft_test_result_fail("futex2_wake shared (file backed) returned: %d %s\n",
				      errno, strerror(errno));
		ret = RET_FAIL;
	} else {
		ksft_test_result_pass("futex2_wake shared (file backed) succeeds\n");
	}

	/* Freeing resources */
	shmdt(shared_data);
	munmap(shm, sizeof(f_private));
	remove(SHM_PATH);

	ksft_print_cnts();
	return ret;
}
