// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "syscall_throttle.h"

int main(void)
{
	struct st_config config;
	int fd;

	fd = open(ST_DEVICE_PATH, O_RDONLY | O_CLOEXEC);
	if (fd == -1) {
		fprintf(stderr, "open %s: %s\n", ST_DEVICE_PATH, strerror(errno));
		return EXIT_FAILURE;
	}

	if (ioctl(fd, ST_IOC_GET_CONFIG, &config) == -1) {
		fprintf(stderr, "get config: %s\n", strerror(errno));
		close(fd);
		return EXIT_FAILURE;
	}

	if (config.max_per_second == 0) {
		fprintf(stderr, "invalid initial MAX value\n");
		close(fd);
		return EXIT_FAILURE;
	}

	if (close(fd) == -1) {
		fprintf(stderr, "close: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}

	puts("device test passed");
	return EXIT_SUCCESS;
}
