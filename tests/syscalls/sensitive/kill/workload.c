// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int parse_calls(int argc, char **argv, unsigned long *calls)
{
	char *end;

	if (argc == 1) {
		*calls = 5;
		return 0;
	}
	if (argc != 2)
		return -1;
	errno = 0;
	*calls = strtoul(argv[1], &end, 10);
	return errno || !*argv[1] || *end || !*calls || *calls > 100 ? -1 : 0;
}

static unsigned long long elapsed_ns(const struct timespec *start,
				     const struct timespec *end)
{
	return (unsigned long long)((end->tv_sec - start->tv_sec) *
				    1000000000LL + end->tv_nsec - start->tv_nsec);
}

static void terminate_children(pid_t *children, unsigned long count)
{
	unsigned long index;

	for (index = 0; index < count; index++)
		kill(children[index], SIGKILL);
	for (index = 0; index < count; index++)
		waitpid(children[index], NULL, 0);
}

int main(int argc, char **argv)
{
	struct timespec start;
	struct timespec end;
	unsigned long calls;
	unsigned long created = 0;
	unsigned long index;
	pid_t *children;

	if (parse_calls(argc, argv, &calls)) {
		fprintf(stderr, "usage: %s [CALLS:1-100]\n", argv[0]);
		return EXIT_FAILURE;
	}
	children = calloc(calls, sizeof(*children));
	if (!children)
		return EXIT_FAILURE;
	for (created = 0; created < calls; created++) {
		children[created] = fork();
		if (children[created] == -1) {
			terminate_children(children, created);
			free(children);
			return EXIT_FAILURE;
		}
		if (children[created] == 0) {
			for (;;)
				pause();
		}
	}
	if (clock_gettime(CLOCK_MONOTONIC, &start)) {
		terminate_children(children, calls);
		free(children);
		return EXIT_FAILURE;
	}
	for (index = 0; index < calls; index++) {
		if (syscall(SYS_kill, children[index], SIGTERM) == -1) {
			terminate_children(children, calls);
			free(children);
			return EXIT_FAILURE;
		}
	}
	for (index = 0; index < calls; index++) {
		int status;

		if (waitpid(children[index], &status, 0) == -1 ||
		    !WIFSIGNALED(status) || WTERMSIG(status) != SIGTERM) {
			terminate_children(children + index + 1,
					   calls - index - 1);
			free(children);
			return EXIT_FAILURE;
		}
	}
	if (clock_gettime(CLOCK_MONOTONIC, &end)) {
		free(children);
		return EXIT_FAILURE;
	}
	free(children);
	printf("syscall=kill calls=%lu elapsed_ns=%llu\n", calls,
	       elapsed_ns(&start, &end));
	return EXIT_SUCCESS;
}
