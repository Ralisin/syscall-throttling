// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static int parse_calls(int argc, char **argv, unsigned long *calls) {
	char *end;

	if (argc == 1) {
		*calls = 7;
		return 0;
	}
	if (argc != 2)
		return -1;
	errno = 0;
	*calls = strtoul(argv[1], &end, 10);
	return errno || !*argv[1] || *end || !*calls ? -1 : 0;
}

static unsigned long long elapsed_ns(const struct timespec *start, const struct timespec *end) {
	return (unsigned long long)((end->tv_sec - start->tv_sec) *
				    1000000000LL + end->tv_nsec - start->tv_nsec);
}

int main(int argc, char **argv) {
	struct timespec start;
	struct timespec end;
	unsigned long calls;
	unsigned long index;
	long page_size;
	void *mapping;

	if (parse_calls(argc, argv, &calls)) {
		fprintf(stderr, "usage: %s [CALLS]\n", argv[0]);
		return EXIT_FAILURE;
	}
	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0)
		return EXIT_FAILURE;
	mapping = mmap(NULL, (size_t)page_size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED) {
		perror("mmap");
		return EXIT_FAILURE;
	}
	if (clock_gettime(CLOCK_MONOTONIC, &start))
		return EXIT_FAILURE;
	for (index = 0; index < calls; index++) {
		int protection = index % 2 ? PROT_READ | PROT_WRITE : PROT_READ;

		if (syscall(SYS_mprotect, mapping, (size_t)page_size,
			    protection) == -1) {
			perror("mprotect");
			return EXIT_FAILURE;
		}
	}
	if (clock_gettime(CLOCK_MONOTONIC, &end))
		return EXIT_FAILURE;
	if (munmap(mapping, (size_t)page_size) == -1)
		return EXIT_FAILURE;
	printf("syscall=mprotect calls=%lu elapsed_ns=%llu\n", calls,
	       elapsed_ns(&start, &end));
	return EXIT_SUCCESS;
}
