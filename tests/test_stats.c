// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "syscall_throttle.h"

#define NSEC_PER_SEC 1000000000ULL

static int descriptor;
static pthread_barrier_t start_barrier;
static atomic_int worker_failure;
static atomic_int reader_stop;
static atomic_int reader_failure;

static void sleep_milliseconds(long milliseconds)
{
	struct timespec delay = {
		.tv_sec = milliseconds / 1000,
		.tv_nsec = milliseconds % 1000 * 1000000L,
	};

	while (nanosleep(&delay, &delay) && errno == EINTR)
		;
}

static int get_stats(struct st_stats *stats)
{
	if (ioctl(descriptor, ST_IOC_GET_STATS, stats) == -1) {
		perror("get stats");
		return -1;
	}
	return 0;
}

static int stats_are_consistent(const struct st_stats *stats)
{
	uint64_t maximum_blocked_time;

	if (stats->reserved != 0 ||
	    stats->current_blocked_threads > stats->peak_blocked_threads ||
	    !memchr(stats->peak_program, '\0', sizeof(stats->peak_program)))
		return 0;

	if (stats->peak_blocked_threads != 0 &&
	    stats->elapsed_ns <= UINT64_MAX / stats->peak_blocked_threads) {
		maximum_blocked_time =
			stats->elapsed_ns * stats->peak_blocked_threads;
		if (stats->blocked_thread_time_ns > maximum_blocked_time)
			return 0;
	}

	return 1;
}

static int expect_identity(const struct st_stats *stats)
{
	return !strcmp(stats->peak_program, "test_stats") &&
	       stats->peak_uid == 0;
}

static void *single_getpid(void *argument)
{
	(void)argument;
	if (syscall(SYS_getpid) == -1)
		atomic_store(&worker_failure, 1);
	return NULL;
}

static void *barrier_getpid(void *argument)
{
	int result;

	(void)argument;
	result = pthread_barrier_wait(&start_barrier);
	if (result != 0 && result != PTHREAD_BARRIER_SERIAL_THREAD) {
		atomic_store(&worker_failure, 1);
		return NULL;
	}
	return single_getpid(NULL);
}

static void *read_stats_repeatedly(void *argument)
{
	struct st_stats stats;

	(void)argument;
	while (!atomic_load(&reader_stop)) {
		if (get_stats(&stats) || !stats_are_consistent(&stats)) {
			atomic_store(&reader_failure, 1);
			break;
		}
	}
	return NULL;
}

static int configure_monitor(void)
{
	struct st_program program = { .name = "test_stats" };
	struct st_syscall syscall = { .number = SYS_getpid };
	uint32_t max_per_second = 1;

	if (ioctl(descriptor, ST_IOC_ADD_PROGRAM, &program) == -1 ||
	    ioctl(descriptor, ST_IOC_ADD_SYSCALL, &syscall) == -1 ||
	    ioctl(descriptor, ST_IOC_SET_MAX, &max_per_second) == -1) {
		perror("configure stats test");
		return -1;
	}
	return 0;
}

static int test_single_wait(void)
{
	struct st_stats stats;

	if (ioctl(descriptor, ST_IOC_RESET_STATS) == -1 ||
	    ioctl(descriptor, ST_IOC_ENABLE) == -1)
		return -1;
	if (syscall(SYS_getpid) == -1 || syscall(SYS_getpid) == -1)
		return -1;
	if (get_stats(&stats))
		return -1;

	if (!stats_are_consistent(&stats) ||
	    stats.current_blocked_threads != 0 ||
	    stats.peak_blocked_threads != 1 || stats.throttled_calls != 1 ||
	    stats.peak_delay_ns < 850000000ULL ||
	    stats.peak_delay_ns > 2000000000ULL || !expect_identity(&stats)) {
		fprintf(stderr, "single-wait statistics are invalid\n");
		return -1;
	}
	return ioctl(descriptor, ST_IOC_DISABLE);
}

static int wait_until_blocked(void)
{
	struct st_stats stats;
	int attempt;

	for (attempt = 0; attempt < 100; attempt++) {
		if (get_stats(&stats))
			return -1;
		if (stats.current_blocked_threads == 1)
			return 0;
		sleep_milliseconds(10);
	}
	return -1;
}

static int test_reset_while_blocked(void)
{
	struct st_stats stats;
	pthread_t worker;

	if (ioctl(descriptor, ST_IOC_RESET_STATS) == -1 ||
	    ioctl(descriptor, ST_IOC_ENABLE) == -1 ||
	    syscall(SYS_getpid) == -1 ||
	    pthread_create(&worker, NULL, single_getpid, NULL))
		return -1;

	if (wait_until_blocked() || ioctl(descriptor, ST_IOC_RESET_STATS) == -1 ||
	    get_stats(&stats) || stats.current_blocked_threads != 1 ||
	    stats.peak_blocked_threads != 1) {
		ioctl(descriptor, ST_IOC_DISABLE);
		pthread_join(worker, NULL);
		return -1;
	}

	sleep_milliseconds(200);
	if (ioctl(descriptor, ST_IOC_DISABLE) == -1) {
		pthread_join(worker, NULL);
		return -1;
	}
	pthread_join(worker, NULL);
	if (get_stats(&stats))
		return -1;

	if (!stats_are_consistent(&stats) ||
	    stats.current_blocked_threads != 0 ||
	    stats.peak_blocked_threads != 1 || stats.throttled_calls != 1 ||
	    stats.peak_delay_ns < 150000000ULL ||
	    stats.peak_delay_ns > 700000000ULL || !expect_identity(&stats)) {
		fprintf(stderr, "active-reset statistics are invalid\n");
		return -1;
	}
	return 0;
}

static int test_concurrent_waiters(void)
{
	struct st_stats stats;
	pthread_t workers[3];
	pthread_t reader;
	int index;

	atomic_store(&worker_failure, 0);
	atomic_store(&reader_failure, 0);
	atomic_store(&reader_stop, 0);
	if (ioctl(descriptor, ST_IOC_RESET_STATS) == -1 ||
	    ioctl(descriptor, ST_IOC_ENABLE) == -1 ||
	    pthread_barrier_init(&start_barrier, NULL, 4))
		return -1;

	if (pthread_create(&reader, NULL, read_stats_repeatedly, NULL))
		return -1;
	for (index = 0; index < 3; index++) {
		if (pthread_create(&workers[index], NULL, barrier_getpid, NULL))
			return -1;
	}
	pthread_barrier_wait(&start_barrier);
	for (index = 0; index < 3; index++)
		pthread_join(workers[index], NULL);
	atomic_store(&reader_stop, 1);
	pthread_join(reader, NULL);
	pthread_barrier_destroy(&start_barrier);

	if (get_stats(&stats) || ioctl(descriptor, ST_IOC_DISABLE) == -1)
		return -1;
	if (atomic_load(&worker_failure) || atomic_load(&reader_failure) ||
	    !stats_are_consistent(&stats) ||
	    stats.current_blocked_threads != 0 ||
	    stats.peak_blocked_threads < 2 || stats.throttled_calls != 2 ||
	    stats.peak_delay_ns < 1800000000ULL ||
	    stats.peak_delay_ns > 3200000000ULL || !expect_identity(&stats)) {
		fprintf(stderr, "concurrent statistics are invalid\n");
		return -1;
	}
	return 0;
}

int main(void)
{
	if (geteuid() != 0) {
		fprintf(stderr, "statistics test must run as root\n");
		return EXIT_FAILURE;
	}

	descriptor = open(ST_DEVICE_PATH, O_RDWR | O_CLOEXEC);
	if (descriptor == -1) {
		perror("open device");
		return EXIT_FAILURE;
	}

	if (configure_monitor() || test_single_wait() ||
	    test_reset_while_blocked() || test_concurrent_waiters()) {
		close(descriptor);
		return EXIT_FAILURE;
	}

	close(descriptor);
	puts("statistics test passed");
	return EXIT_SUCCESS;
}
