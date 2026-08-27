// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define GETPID_THREAD_COUNT 8
#define GETPID_CALLS_PER_THREAD 10000

static atomic_int getpid_failed;

struct read_context {
	int descriptor;
	ssize_t result;
	char value;
};

static void *run_getpid_calls(void *argument) {
	pid_t expected = getpid();
	int call;

	(void)argument;

	for (call = 0; call < GETPID_CALLS_PER_THREAD; call++) {
		if ((pid_t)syscall(SYS_getpid) != expected) {
			atomic_store(&getpid_failed, 1);
			break;
		}
	}

	return NULL;
}

static void *run_blocking_read(void *argument) {
	struct read_context *context = argument;

	context->result = syscall(SYS_read, context->descriptor,
				  &context->value, sizeof(context->value));
	return NULL;
}

static int test_concurrent_getpid(void) {
	pthread_t threads[GETPID_THREAD_COUNT];
	int created = 0;
	int index;

	for (index = 0; index < GETPID_THREAD_COUNT; index++) {
		if (pthread_create(&threads[index], NULL, run_getpid_calls, NULL))
			break;
		created++;
	}

	for (index = 0; index < created; index++)
		pthread_join(threads[index], NULL);

	if (created != GETPID_THREAD_COUNT || atomic_load(&getpid_failed)) {
		fprintf(stderr, "concurrent getpid test failed\n");
		return -1;
	}

	return 0;
}

static int test_blocking_read(long delay_nanoseconds) {
	const struct timespec delay = {
		.tv_sec = delay_nanoseconds / 1000000000L,
		.tv_nsec = delay_nanoseconds % 1000000000L,
	};
	struct read_context context;
	pthread_t reader;
	int descriptors[2];
	char value = 'x';

	if (pipe(descriptors)) {
		perror("pipe");
		return -1;
	}

	context.descriptor = descriptors[0];
	context.result = -1;
	context.value = 0;

	if (pthread_create(&reader, NULL, run_blocking_read, &context)) {
		fprintf(stderr, "unable to create read thread\n");
		close(descriptors[0]);
		close(descriptors[1]);
		return -1;
	}

	nanosleep(&delay, NULL);
	if (write(descriptors[1], &value, sizeof(value)) != sizeof(value)) {
		perror("write");
		close(descriptors[1]);
		pthread_join(reader, NULL);
		close(descriptors[0]);
		return -1;
	}

	pthread_join(reader, NULL);
	close(descriptors[0]);
	close(descriptors[1]);

	if (context.result != sizeof(context.value) || context.value != value) {
		fprintf(stderr, "blocking read test failed\n");
		return -1;
	}

	return 0;
}

int main(int argument_count, char **arguments) {
	if (argument_count == 2 && !strcmp(arguments[1], "blocking-read")) {
		if (test_blocking_read(1000000000L))
			return EXIT_FAILURE;
		puts("blocking read workload passed");
		return EXIT_SUCCESS;
	}

	if (argument_count != 1) {
		fprintf(stderr, "usage: %s [blocking-read]\n", arguments[0]);
		return EXIT_FAILURE;
	}

	if (test_concurrent_getpid() || test_blocking_read(100000000L))
		return EXIT_FAILURE;

	puts("hook workload passed");
	return EXIT_SUCCESS;
}
