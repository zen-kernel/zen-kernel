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
futex_t *f1;

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

	if (futex2_wait(f1, *f1, FUTEX_32 | flags, &to64))
		printf("waiter failed errno %d\n", errno);

	return NULL;
}

void *waitershm(void *arg)
{
	futex2_wait(arg, 0, FUTEX_32 | FUTEX_SHARED_FLAG, NULL);

	return NULL;
}

int main(int argc, char *argv[])
{
	pthread_t waiter;
	unsigned int flags = FUTEX_SHARED_FLAG;
	int res, ret = RET_PASS;
	int c;
	futex_t f_private = 0;

	f1 = &f_private;

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
	ksft_print_msg("%s: Test FUTEX2_WAIT\n",
		       basename(argv[0]));

	/* Testing a private futex */
	info("Calling private futex2_wait on f1: %u @ %p with val=%u\n", *f1, f1, *f1);

	if (pthread_create(&waiter, NULL, waiterfn, NULL))
		error("pthread_create failed\n", errno);

	usleep(WAKE_WAIT_US);

	info("Calling private futex2_wake on f1: %u @ %p with val=%u\n", *f1, f1, *f1);
	res = futex2_wake(f1, 1, FUTEX_32);
	if (res != 1) {
		ksft_test_result_fail("futex2_wake private returned: %d %s\n",
				      res ? errno : res,
				      res ? strerror(errno) : "");
		ret = RET_FAIL;
	} else {
		ksft_test_result_pass("futex2_wake private succeeds\n");
	}

	int shm_id = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0666);

	if (shm_id < 0) {
		perror("shmget");
		exit(1);
	}

	/* Testing an anon page shared memory */
	unsigned int *shared_data = shmat(shm_id, NULL, 0);

	*shared_data = 0;
	f1 = shared_data;

	info("Calling shared futex2_wait on f1: %u @ %p with val=%u\n", *f1, f1, *f1);

	if (pthread_create(&waiter, NULL, waiterfn, &flags))
		error("pthread_create failed\n", errno);

	usleep(WAKE_WAIT_US);

	info("Calling shared futex2_wake on f1: %u @ %p with val=%u\n", *f1, f1, *f1);
	res = futex2_wake(f1, 1, FUTEX_32 | FUTEX_SHARED_FLAG);
	if (res != 1) {
		ksft_test_result_fail("futex2_wake shared (shmget) returned: %d %s\n",
				      res ? errno : res,
				      res ? strerror(errno) : "");
		ret = RET_FAIL;
	} else {
		ksft_test_result_pass("futex2_wake shared (shmget) succeeds\n");
	}

	shmdt(shared_data);

	/* Testing a file backed shared memory */
	void *shm;
	int fd, pid;

	f_private = 0;

	fd = open(SHM_PATH, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		perror("open");
		exit(1);
	}

	res = ftruncate(fd, sizeof(f_private));
	if (res) {
		perror("ftruncate");
		exit(1);
	}

	shm = mmap(NULL, sizeof(f_private), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (shm == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}

	memcpy(shm, &f_private, sizeof(f_private));

	pthread_create(&waiter, NULL, waitershm, shm);

	usleep(WAKE_WAIT_US);

	res = futex2_wake(shm, 1, FUTEX_32 | FUTEX_SHARED_FLAG);
	if (res != 1) {
		ksft_test_result_fail("futex2_wake shared (mmap) returned: %d %s\n",
				      res ? errno : res,
				      res ? strerror(errno) : "");
		ret = RET_FAIL;
	} else {
		ksft_test_result_pass("futex2_wake shared (mmap) succeeds\n");
	}

	munmap(shm, sizeof(f_private));

	remove(SHM_PATH);

	ksft_print_cnts();
	return ret;
}
