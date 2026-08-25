// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

struct context {
	atomic_int state;
	atomic_int failed;
	unsigned long rounds;
};

static int futex_wait(atomic_int *address, int expected)
{
	int result = syscall(SYS_futex, address, FUTEX_WAIT_PRIVATE, expected,
			     NULL, NULL, 0);

	if (result == -1 && errno != EAGAIN && errno != EINTR)
		return -1;
	return 0;
}

static int futex_wake(atomic_int *address)
{
	return syscall(SYS_futex, address, FUTEX_WAKE_PRIVATE, 1,
		       NULL, NULL, 0) == -1 ? -1 : 0;
}

static void *peer_thread(void *argument)
{
	struct context *context = argument;
	unsigned long round;

	for (round = 0; round < context->rounds; round++) {
		while (atomic_load(&context->state) != 0) {
			if (futex_wait(&context->state, 1)) {
				atomic_store(&context->failed, 1);
				return NULL;
			}
		}
		atomic_store(&context->state, 1);
		if (futex_wake(&context->state)) {
			atomic_store(&context->failed, 1);
			return NULL;
		}
	}
	return NULL;
}

static int parse_rounds(int argc, char **argv, unsigned long *rounds)
{
	char *end;

	if (argc == 1) {
		*rounds = 5;
		return 0;
	}
	if (argc != 2)
		return -1;
	errno = 0;
	*rounds = strtoul(argv[1], &end, 10);
	return errno || !*argv[1] || *end || !*rounds ? -1 : 0;
}

static unsigned long long elapsed_ns(const struct timespec *start,
				     const struct timespec *end)
{
	return (unsigned long long)((end->tv_sec - start->tv_sec) *
				    1000000000LL + end->tv_nsec - start->tv_nsec);
}

int main(int argc, char **argv)
{
	struct context context;
	struct timespec start;
	struct timespec end;
	unsigned long round;
	pthread_t peer;

	if (parse_rounds(argc, argv, &context.rounds)) {
		fprintf(stderr, "usage: %s [ROUNDS]\n", argv[0]);
		return EXIT_FAILURE;
	}
	atomic_init(&context.state, 0);
	atomic_init(&context.failed, 0);
	if (clock_gettime(CLOCK_MONOTONIC, &start))
		return EXIT_FAILURE;
	if (pthread_create(&peer, NULL, peer_thread, &context))
		return EXIT_FAILURE;
	for (round = 0; round < context.rounds; round++) {
		while (atomic_load(&context.state) != 1) {
			if (futex_wait(&context.state, 0)) {
				atomic_store(&context.failed, 1);
				break;
			}
		}
		atomic_store(&context.state, 0);
		if (futex_wake(&context.state))
			atomic_store(&context.failed, 1);
	}
	if (pthread_join(peer, NULL))
		return EXIT_FAILURE;
	if (clock_gettime(CLOCK_MONOTONIC, &end))
		return EXIT_FAILURE;
	if (atomic_load(&context.failed)) {
		fprintf(stderr, "futex synchronization failed\n");
		return EXIT_FAILURE;
	}
	printf("syscall=futex rounds=%lu elapsed_ns=%llu\n", context.rounds,
	       elapsed_ns(&start, &end));
	return EXIT_SUCCESS;
}
