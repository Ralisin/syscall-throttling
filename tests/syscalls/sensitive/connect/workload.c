// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
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

static void run_client(const struct sockaddr_in *address, unsigned long calls)
{
	unsigned long index;

	for (index = 0; index < calls; index++) {
		int descriptor = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

		if (descriptor == -1 ||
		    syscall(SYS_connect, descriptor, address,
			    sizeof(*address)) == -1)
			_exit(EXIT_FAILURE);
		close(descriptor);
	}
	_exit(EXIT_SUCCESS);
}

int main(int argc, char **argv)
{
	struct sockaddr_in address = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	struct timespec start;
	struct timespec end;
	socklen_t address_length = sizeof(address);
	unsigned long calls;
	unsigned long index;
	int listener;
	int child_status;
	pid_t child;

	if (parse_calls(argc, argv, &calls)) {
		fprintf(stderr, "usage: %s [CALLS:1-100]\n", argv[0]);
		return EXIT_FAILURE;
	}
	listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (listener == -1 ||
	    bind(listener, (struct sockaddr *)&address, sizeof(address)) == -1 ||
	    getsockname(listener, (struct sockaddr *)&address,
			&address_length) == -1 || listen(listener, 16) == -1) {
		perror("prepare loopback listener");
		return EXIT_FAILURE;
	}
	if (clock_gettime(CLOCK_MONOTONIC, &start))
		return EXIT_FAILURE;
	child = fork();
	if (child == -1)
		return EXIT_FAILURE;
	if (child == 0) {
		close(listener);
		run_client(&address, calls);
	}
	for (index = 0; index < calls; index++) {
		int connection = accept4(listener, NULL, NULL, SOCK_CLOEXEC);

		if (connection == -1)
			return EXIT_FAILURE;
		close(connection);
	}
	close(listener);
	if (waitpid(child, &child_status, 0) == -1 ||
	    !WIFEXITED(child_status) || WEXITSTATUS(child_status))
		return EXIT_FAILURE;
	if (clock_gettime(CLOCK_MONOTONIC, &end))
		return EXIT_FAILURE;
	printf("syscall=connect calls=%lu elapsed_ns=%llu\n", calls,
	       elapsed_ns(&start, &end));
	return EXIT_SUCCESS;
}
