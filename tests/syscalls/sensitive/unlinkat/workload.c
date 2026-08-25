// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
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

int main(int argc, char **argv)
{
	char directory[] = "/tmp/st_unlinkat.XXXXXX";
	char path[PATH_MAX] = { 0 };
	struct timespec start;
	struct timespec end;
	unsigned long calls;
	unsigned long index;
	int file_exists = 0;
	int result = EXIT_FAILURE;

	if (parse_calls(argc, argv, &calls)) {
		fprintf(stderr, "usage: %s [CALLS:1-100]\n", argv[0]);
		return EXIT_FAILURE;
	}
	if (!mkdtemp(directory)) {
		perror("mkdtemp");
		return EXIT_FAILURE;
	}
	if (clock_gettime(CLOCK_MONOTONIC, &start))
		goto cleanup;
	for (index = 0; index < calls; index++) {
		int descriptor;

		if (snprintf(path, sizeof(path), "%s/file_%lu", directory,
			     index) >= (int)sizeof(path))
			goto cleanup;
		descriptor = open(path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC,
				  0600);
		if (descriptor == -1) {
			perror("create temporary file");
			goto cleanup;
		}
		file_exists = 1;
		close(descriptor);
		if (syscall(SYS_unlinkat, AT_FDCWD, path, 0) == -1) {
			perror("unlinkat");
			goto cleanup;
		}
		file_exists = 0;
	}
	if (clock_gettime(CLOCK_MONOTONIC, &end))
		goto cleanup;
	result = EXIT_SUCCESS;
cleanup:
	if (file_exists)
		unlink(path);
	if (rmdir(directory) == -1) {
		perror("rmdir temporary directory");
		return EXIT_FAILURE;
	}
	if (result != EXIT_SUCCESS)
		return result;
	printf("syscall=unlinkat calls=%lu elapsed_ns=%llu\n", calls,
	       elapsed_ns(&start, &end));
	return result;
}
