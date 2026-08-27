// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static int parse_rounds(int argc, char **argv, unsigned long *rounds)
{
	char *end;

	if (argc == 1) {
		*rounds = 3;
		return 0;
	}
	if (argc != 2)
		return -1;
	errno = 0;
	*rounds = strtoul(argv[1], &end, 10);
	return errno || !*argv[1] || *end || !*rounds || *rounds > 100 ?
	       -1 : 0;
}

static unsigned long long elapsed_ns(const struct timespec *start,
				     const struct timespec *end)
{
	return (unsigned long long)((end->tv_sec - start->tv_sec) *
				    1000000000LL + end->tv_nsec - start->tv_nsec);
}

int main(int argc, char **argv)
{
	static const char payload[] = "payload";
	char path[] = "/tmp/st_mixed.XXXXXX";
	struct timespec start;
	struct timespec end;
	unsigned long rounds;
	unsigned long round;
	int descriptor = -1;
	int file_exists = 0;
	int result = EXIT_FAILURE;

	if (parse_rounds(argc, argv, &rounds)) {
		fprintf(stderr, "usage: %s [ROUNDS:1-100]\n", argv[0]);
		return EXIT_FAILURE;
	}
	descriptor = mkstemp(path);
	if (descriptor == -1) {
		perror("mkstemp");
		return EXIT_FAILURE;
	}
	file_exists = 1;
	if (clock_gettime(CLOCK_MONOTONIC, &start))
		goto cleanup;
	for (round = 0; round < rounds; round++) {
		if (syscall(SYS_write, descriptor, payload,
			    sizeof(payload) - 1) !=
		    (long)(sizeof(payload) - 1)) {
			perror("write");
			goto cleanup;
		}
		if (syscall(SYS_fsync, descriptor) == -1) {
			perror("fsync");
			goto cleanup;
		}
		if (syscall(SYS_ftruncate, descriptor, 0) == -1) {
			perror("ftruncate");
			goto cleanup;
		}
	}
	if (close(descriptor) == -1) {
		descriptor = -1;
		goto cleanup;
	}
	descriptor = -1;
	if (syscall(SYS_unlinkat, AT_FDCWD, path, 0) == -1) {
		perror("unlinkat");
		goto cleanup;
	}
	file_exists = 0;
	if (clock_gettime(CLOCK_MONOTONIC, &end))
		goto cleanup;
	result = EXIT_SUCCESS;
cleanup:
	if (descriptor != -1)
		close(descriptor);
	if (file_exists)
		unlink(path);
	if (result != EXIT_SUCCESS)
		return result;

	/* Printing must not add another registered write to the measurement. */
	if (prctl(PR_SET_NAME, "st_report", 0, 0, 0) == -1)
		return EXIT_FAILURE;
	printf("syscalls=write,fsync,ftruncate,unlinkat rounds=%lu "
	       "registered_calls=%lu elapsed_ns=%llu\n",
	       rounds, rounds * 3 + 1, elapsed_ns(&start, &end));
	return result;
}
