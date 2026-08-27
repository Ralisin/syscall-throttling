// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t signal_seen;

static void handle_signal(int signal_number) {
	(void)signal_number;
	signal_seen = 1;
}

static uint64_t elapsed_nanoseconds(const struct timespec *start, const struct timespec *end) {
	return (uint64_t)(end->tv_sec - start->tv_sec) * 1000000000ULL +
	       (uint64_t)(end->tv_nsec - start->tv_nsec);
}

static int run_burst(unsigned long calls) {
	struct timespec start;
	struct timespec end;
	unsigned long index;
	unsigned long errors = 0;

	if (clock_gettime(CLOCK_MONOTONIC, &start))
		return -1;
	for (index = 0; index < calls; index++) {
		if (syscall(SYS_getpid) == -1)
			errors++;
	}
	if (clock_gettime(CLOCK_MONOTONIC, &end))
		return -1;

	printf("elapsed_ns=%llu errors=%lu\n",
	       (unsigned long long)elapsed_nanoseconds(&start, &end), errors);
	return errors ? -1 : 0;
}

static int run_signal_wait(void) {
	struct sigaction action;
	struct timespec start;
	struct timespec end;
	long result;
	int saved_errno;

	memset(&action, 0, sizeof(action));
	action.sa_handler = handle_signal;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGUSR1, &action, NULL))
		return -1;

	if (syscall(SYS_getpid) == -1)
		return -1;
	puts("ready");
	fflush(stdout);

	if (clock_gettime(CLOCK_MONOTONIC, &start))
		return -1;
	errno = 0;
	result = syscall(SYS_getpid);
	saved_errno = errno;
	if (clock_gettime(CLOCK_MONOTONIC, &end))
		return -1;

	printf("elapsed_ns=%llu result=%ld errno=%d signal=%d\n",
	       (unsigned long long)elapsed_nanoseconds(&start, &end), result,
	       saved_errno, signal_seen != 0);
	return result == -1 && saved_errno == EINTR && signal_seen ? 0 : -1;
}

int main(int argument_count, char **arguments) {
	char *end;
	unsigned long calls;

	if (argument_count == 2 && !strcmp(arguments[1], "signal-wait"))
		return run_signal_wait() ? EXIT_FAILURE : EXIT_SUCCESS;

	if (argument_count != 3 || strcmp(arguments[1], "burst")) {
		fprintf(stderr, "usage: %s burst CALLS | signal-wait\n",
			arguments[0]);
		return EXIT_FAILURE;
	}

	errno = 0;
	calls = strtoul(arguments[2], &end, 10);
	if (errno || *end != '\0' || calls == 0) {
		fprintf(stderr, "invalid call count\n");
		return EXIT_FAILURE;
	}

	return run_burst(calls) ? EXIT_FAILURE : EXIT_SUCCESS;
}
