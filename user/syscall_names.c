// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "syscall_names.h"

static const char *const syscall_header_paths[] = {
	"/usr/include/x86_64-linux-gnu/asm/unistd_64.h",
	"/usr/include/asm/unistd_64.h",
};

static int parse_number(const char *text, __s32 *number)
{
	char *end;
	long parsed;

	errno = 0;
	parsed = strtol(text, &end, 10);
	if (errno || *text == '\0' || *end != '\0' ||
	    parsed < INT32_MIN || parsed > INT32_MAX)
		return -1;
	*number = (__s32)parsed;
	return 0;
}

static FILE *open_syscall_header(void)
{
	size_t index;

	for (index = 0; index < sizeof(syscall_header_paths) /
				      sizeof(syscall_header_paths[0]); index++) {
		FILE *stream = fopen(syscall_header_paths[index], "r");

		if (stream)
			return stream;
	}
	return NULL;
}

int st_resolve_syscall(const char *text, __s32 *number)
{
	char candidate[128];
	char line[256];
	const char *requested = text;
	long parsed;
	FILE *stream;

	if (!parse_number(text, number))
		return 0;
	if (!strncmp(requested, "__NR_", 5))
		requested += 5;
	stream = open_syscall_header();
	if (!stream) {
		errno = ENOENT;
		return -1;
	}
	while (fgets(line, sizeof(line), stream)) {
		if (sscanf(line, "#define __NR_%127s %ld", candidate, &parsed) == 2 &&
		    !strcmp(candidate, requested) && parsed >= INT32_MIN &&
		    parsed <= INT32_MAX) {
			*number = (__s32)parsed;
			fclose(stream);
			return 0;
		}
	}
	fclose(stream);
	errno = ENOENT;
	return -1;
}

int st_lookup_syscall_name(__s32 number, char *name, size_t size)
{
	char candidate[128];
	char line[256];
	long parsed;
	FILE *stream;

	if (size == 0) {
		errno = EINVAL;
		return -1;
	}
	stream = open_syscall_header();
	if (!stream) {
		errno = ENOENT;
		return -1;
	}
	while (fgets(line, sizeof(line), stream)) {
		if (sscanf(line, "#define __NR_%127s %ld", candidate, &parsed) == 2 &&
		    parsed == number) {
			if (strlen(candidate) >= size) {
				fclose(stream);
				errno = ENAMETOOLONG;
				return -1;
			}
			strcpy(name, candidate);
			fclose(stream);
			return 0;
		}
	}
	fclose(stream);
	errno = ENOENT;
	return -1;
}
