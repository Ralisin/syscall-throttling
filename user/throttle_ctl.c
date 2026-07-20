// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "syscall_throttle.h"

static void print_usage(FILE *stream, const char *program)
{
	fprintf(stream,
		"Usage:\n"
		"  %s status\n"
		"  %s set-max NUMBER\n"
		"  %s enable\n"
		"  %s disable\n",
		program, program, program, program);
}

static int parse_max(const char *text, __u32 *value)
{
	char *end;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno != 0 || *text == '\0' || *end != '\0' ||
	    parsed == 0 || parsed > ST_MAX_LIMIT)
		return -1;

	*value = (__u32)parsed;
	return 0;
}

static int show_status(int fd)
{
	struct st_config config;

	if (ioctl(fd, ST_IOC_GET_CONFIG, &config) == -1)
		return -1;

	printf("monitor: %s\n", config.enabled ? "on" : "off");
	printf("max: %" PRIu32 "\n", config.max_per_second);
	printf("programs: %" PRIu32 "\n", config.program_count);
	printf("uids: %" PRIu32 "\n", config.uid_count);
	printf("syscalls: %" PRIu32 "\n", config.syscall_count);
	printf("generation: %" PRIu64 "\n", (uint64_t)config.generation);
	return 0;
}

int main(int argc, char **argv)
{
	__u32 max_per_second;
	unsigned long command;
	int fd;
	int result;

	if (argc < 2) {
		print_usage(stderr, argv[0]);
		return EXIT_FAILURE;
	}

	fd = open(ST_DEVICE_PATH, O_RDWR | O_CLOEXEC);
	if (fd == -1) {
		fprintf(stderr, "%s: %s\n", ST_DEVICE_PATH, strerror(errno));
		return EXIT_FAILURE;
	}

	if (strcmp(argv[1], "status") == 0 && argc == 2) {
		result = show_status(fd);
	} else if (strcmp(argv[1], "set-max") == 0 && argc == 3) {
		if (parse_max(argv[2], &max_per_second) != 0) {
			fprintf(stderr, "invalid MAX value: %s\n", argv[2]);
			close(fd);
			return EXIT_FAILURE;
		}
		result = ioctl(fd, ST_IOC_SET_MAX, &max_per_second);
	} else if (strcmp(argv[1], "enable") == 0 && argc == 2) {
		command = ST_IOC_ENABLE;
		result = ioctl(fd, command);
	} else if (strcmp(argv[1], "disable") == 0 && argc == 2) {
		command = ST_IOC_DISABLE;
		result = ioctl(fd, command);
	} else {
		print_usage(stderr, argv[0]);
		close(fd);
		return EXIT_FAILURE;
	}

	if (result == -1) {
		fprintf(stderr, "%s: %s\n", argv[1], strerror(errno));
		close(fd);
		return EXIT_FAILURE;
	}

	if (close(fd) == -1) {
		fprintf(stderr, "close: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
