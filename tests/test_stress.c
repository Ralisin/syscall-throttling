// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

enum workload_kind {
	WORKLOAD_GETPID,
	WORKLOAD_READ,
};

struct worker_context {
	enum workload_kind kind;
	unsigned long calls;
	int read_descriptor;
};

static pthread_mutex_t start_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t start_condition = PTHREAD_COND_INITIALIZER;
static int start_workers;
static atomic_int worker_failure;

static uint64_t elapsed_nanoseconds(const struct timespec *start, const struct timespec *end) {
	return (uint64_t)(end->tv_sec - start->tv_sec) * 1000000000ULL +
	       (uint64_t)(end->tv_nsec - start->tv_nsec);
}

static int parse_positive(const char *text, unsigned long *value) {
	char *end;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno || *text == '\0' || *end != '\0' || parsed == 0)
		return -1;
	*value = parsed;
	return 0;
}

static void *run_worker(void *argument) {
	const struct worker_context *context = argument;
	unsigned long index;

	pthread_mutex_lock(&start_lock);
	while (!start_workers)
		pthread_cond_wait(&start_condition, &start_lock);
	pthread_mutex_unlock(&start_lock);

	for (index = 0; index < context->calls; index++) {
		if (context->kind == WORKLOAD_GETPID) {
			if (syscall(SYS_getpid) == -1) {
				atomic_store(&worker_failure, 1);
				break;
			}
		} else {
			char byte;

			if (syscall(SYS_read, context->read_descriptor,
				    &byte, sizeof(byte)) != sizeof(byte)) {
				atomic_store(&worker_failure, 1);
				break;
			}
		}
	}
	return NULL;
}

static int write_all(int descriptor, const char *buffer, size_t length) {
	size_t offset = 0;

	while (offset < length) {
		ssize_t written = write(descriptor, buffer + offset,
					length - offset);

		if (written > 0) {
			offset += (size_t)written;
			continue;
		}
		if (written == -1 && errno == EINTR)
			continue;
		return -1;
	}
	return 0;
}

static int run_workload(enum workload_kind kind, unsigned long thread_count, unsigned long calls, unsigned long release_delay_ms) {
	struct worker_context context = {
		.kind = kind,
		.calls = calls,
		.read_descriptor = -1,
	};
	struct timespec start;
	struct timespec end;
	struct timespec delay;
	pthread_t *threads;
	char *bytes = NULL;
	int pipe_descriptors[2] = { -1, -1 };
	unsigned long created = 0;
	unsigned long index;
	int result = -1;
	size_t byte_count;

	threads = calloc(thread_count, sizeof(*threads));
	if (!threads)
		return -1;
	if (kind == WORKLOAD_READ) {
		if (calls > SIZE_MAX / thread_count)
			goto out;
		byte_count = thread_count * calls;
		if (pipe(pipe_descriptors))
			goto out;
		context.read_descriptor = pipe_descriptors[0];
		bytes = malloc(byte_count);
		if (!bytes)
			goto out;
		memset(bytes, 'x', byte_count);
	}

	start_workers = 0;
	atomic_store(&worker_failure, 0);
	for (index = 0; index < thread_count; index++) {
		if (pthread_create(&threads[index], NULL, run_worker, &context))
			break;
		created++;
	}
	if (created != thread_count)
		goto release;

	clock_gettime(CLOCK_MONOTONIC, &start);
release:
	pthread_mutex_lock(&start_lock);
	start_workers = 1;
	pthread_cond_broadcast(&start_condition);
	pthread_mutex_unlock(&start_lock);

	if (kind == WORKLOAD_READ && created == thread_count) {
		delay.tv_sec = release_delay_ms / 1000;
		delay.tv_nsec = release_delay_ms % 1000 * 1000000L;
		while (nanosleep(&delay, &delay) && errno == EINTR)
			;
		if (write_all(pipe_descriptors[1], bytes, byte_count))
			atomic_store(&worker_failure, 1);
	}

	for (index = 0; index < created; index++)
		pthread_join(threads[index], NULL);
	if (created != thread_count || atomic_load(&worker_failure))
		goto out;
	clock_gettime(CLOCK_MONOTONIC, &end);
	printf("elapsed_ns=%llu threads=%lu calls=%lu\n",
	       (unsigned long long)elapsed_nanoseconds(&start, &end),
	       thread_count, calls);
	result = 0;
out:
	if (pipe_descriptors[0] != -1)
		close(pipe_descriptors[0]);
	if (pipe_descriptors[1] != -1)
		close(pipe_descriptors[1]);
	free(bytes);
	free(threads);
	return result;
}

int main(int argument_count, char **arguments) {
	unsigned long thread_count;
	unsigned long calls;
	unsigned long delay_ms = 0;
	enum workload_kind kind;

	if ((argument_count != 4 && argument_count != 5) ||
	    parse_positive(arguments[2], &thread_count) ||
	    parse_positive(arguments[3], &calls) || thread_count > 1000) {
		fprintf(stderr,
			"usage: %s getpid THREADS CALLS | read THREADS CALLS DELAY_MS\n",
			arguments[0]);
		return EXIT_FAILURE;
	}

	if (!strcmp(arguments[1], "getpid") && argument_count == 4) {
		kind = WORKLOAD_GETPID;
	} else if (!strcmp(arguments[1], "read") && argument_count == 5 &&
		   !parse_positive(arguments[4], &delay_ms)) {
		kind = WORKLOAD_READ;
	} else {
		fprintf(stderr, "invalid workload\n");
		return EXIT_FAILURE;
	}

	return run_workload(kind, thread_count, calls, delay_ms) ?
		EXIT_FAILURE : EXIT_SUCCESS;
}
