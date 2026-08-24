// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include <errno.h>
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
	return errno || !*argv[1] || *end || !*calls ? -1 : 0;
}

static unsigned long long elapsed_ns(const struct timespec *start,
				     const struct timespec *end)
{
	return (unsigned long long)((end->tv_sec - start->tv_sec) *
				    1000000000LL + end->tv_nsec - start->tv_nsec);
}

static void delayed_writer(int descriptor, unsigned long calls)
{
	const struct timespec delay = {
		.tv_nsec = 200000000L,
	};
	unsigned long index;

	for (index = 0; index < calls; index++) {
		if (nanosleep(&delay, NULL) == -1 ||
		    write(descriptor, "x", 1) != 1)
			_exit(EXIT_FAILURE);
	}
	close(descriptor);
	_exit(EXIT_SUCCESS);
}

int main(int argc, char **argv)
{
	struct timespec start;
	struct timespec end;
	unsigned long calls;
	unsigned long index;
	int descriptors[2];
	int child_status;
	pid_t child;

	if (parse_calls(argc, argv, &calls)) {
		fprintf(stderr, "usage: %s [CALLS]\n", argv[0]);
		return EXIT_FAILURE;
	}
	if (pipe(descriptors) == -1) {
		perror("pipe");
		return EXIT_FAILURE;
	}
	child = fork();
	if (child == -1) {
		perror("fork");
		return EXIT_FAILURE;
	}
	if (child == 0) {
		close(descriptors[0]);
		delayed_writer(descriptors[1], calls);
	}
	close(descriptors[1]);

	if (clock_gettime(CLOCK_MONOTONIC, &start))
		return EXIT_FAILURE;
	for (index = 0; index < calls; index++) {
		char value;

		if (syscall(SYS_read, descriptors[0], &value, 1) != 1) {
			perror("read");
			close(descriptors[0]);
			return EXIT_FAILURE;
		}
	}
	if (clock_gettime(CLOCK_MONOTONIC, &end))
		return EXIT_FAILURE;
	close(descriptors[0]);
	if (waitpid(child, &child_status, 0) == -1 ||
	    !WIFEXITED(child_status) || WEXITSTATUS(child_status)) {
		fprintf(stderr, "writer process failed\n");
		return EXIT_FAILURE;
	}
	printf("syscall=read calls=%lu elapsed_ns=%llu\n", calls,
	       elapsed_ns(&start, &end));
	return EXIT_SUCCESS;
}
