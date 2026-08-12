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

#define ST_LIST_RETRIES 64

struct st_configuration_snapshot {
	struct st_config config;
	char programs[ST_MAX_PROGRAMS][ST_PROGRAM_NAME_LEN];
	__u32 uids[ST_MAX_UIDS];
	__s32 syscalls[ST_MAX_SYSCALLS];
};

static void print_usage(FILE *stream, const char *program)
{
	fprintf(stream,
		"Usage:\n"
		"  %s status\n"
		"  %s stats\n"
		"  %s reset-stats\n"
		"  %s list\n"
		"  %s add-program NAME\n"
		"  %s remove-program NAME\n"
		"  %s add-uid UID\n"
		"  %s remove-uid UID\n"
		"  %s add-syscall NUMBER\n"
		"  %s remove-syscall NUMBER\n"
		"  %s set-max NUMBER\n"
		"  %s enable\n"
		"  %s disable\n",
		program, program, program, program, program, program, program,
		program, program, program, program, program, program);
}

static int parse_u32(const char *text, __u32 *value)
{
	char *end;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno != 0 || *text == '\0' || *end != '\0' ||
	    parsed > UINT32_MAX)
		return -1;

	*value = (__u32)parsed;
	return 0;
}

static int parse_syscall(const char *text, __s32 *value)
{
	char *end;
	long parsed;

	errno = 0;
	parsed = strtol(text, &end, 10);
	if (errno != 0 || *text == '\0' || *end != '\0' ||
	    parsed < INT32_MIN || parsed > INT32_MAX)
		return -1;

	*value = (__s32)parsed;
	return 0;
}

static int prepare_program(const char *name, struct st_program *program)
{
	size_t length = strlen(name);

	if (length == 0 || length >= ST_PROGRAM_NAME_LEN)
		return -1;

	memset(program, 0, sizeof(*program));
	memcpy(program->name, name, length);
	return 0;
}

static void print_status(const struct st_config *config)
{
	printf("monitor: %s\n", config->enabled ? "on" : "off");
	printf("max: %" PRIu32 "\n", config->max_per_second);
	printf("programs: %" PRIu32 "\n", config->program_count);
	printf("uids: %" PRIu32 "\n", config->uid_count);
	printf("syscalls: %" PRIu32 "\n", config->syscall_count);
	printf("generation: %" PRIu64 "\n", (uint64_t)config->generation);
}

static int get_config(int descriptor, struct st_config *config)
{
	return ioctl(descriptor, ST_IOC_GET_CONFIG, config);
}

static void print_stats(const struct st_stats *stats)
{
	double average = 0.0;

	if (stats->elapsed_ns != 0)
		average = (double)stats->blocked_thread_time_ns /
			  (double)stats->elapsed_ns;

	printf("interval_ns: %" PRIu64 "\n", (uint64_t)stats->elapsed_ns);
	printf("blocked_thread_time_ns: %" PRIu64 "\n",
	       (uint64_t)stats->blocked_thread_time_ns);
	printf("average_blocked_threads: %.6f\n", average);
	printf("current_blocked_threads: %" PRIu32 "\n",
	       stats->current_blocked_threads);
	printf("peak_blocked_threads: %" PRIu32 "\n",
	       stats->peak_blocked_threads);
	printf("throttled_calls: %" PRIu64 "\n",
	       (uint64_t)stats->throttled_calls);
	printf("peak_delay_ns: %" PRIu64 "\n",
	       (uint64_t)stats->peak_delay_ns);
	printf("peak_program: %s\n", stats->peak_program);
	printf("peak_uid: %" PRIu32 "\n", stats->peak_uid);
}

static int read_snapshot(int descriptor,
			 struct st_configuration_snapshot *snapshot)
{
	struct st_config final_config;
	unsigned int attempt;
	__u32 index;

	for (attempt = 0; attempt < ST_LIST_RETRIES; attempt++) {
		if (get_config(descriptor, &snapshot->config) == -1)
			return -1;

		if (snapshot->config.program_count > ST_MAX_PROGRAMS ||
		    snapshot->config.uid_count > ST_MAX_UIDS ||
		    snapshot->config.syscall_count > ST_MAX_SYSCALLS) {
			errno = EPROTO;
			return -1;
		}

		for (index = 0; index < snapshot->config.program_count;
		     index++) {
			struct st_program_entry entry = {
				.generation = snapshot->config.generation,
				.index = index,
			};

			if (ioctl(descriptor, ST_IOC_GET_PROGRAM, &entry) == -1)
				goto retry;
			memcpy(snapshot->programs[index], entry.name,
			       ST_PROGRAM_NAME_LEN);
		}

		for (index = 0; index < snapshot->config.uid_count; index++) {
			struct st_uid_entry entry = {
				.generation = snapshot->config.generation,
				.index = index,
			};

			if (ioctl(descriptor, ST_IOC_GET_UID, &entry) == -1)
				goto retry;
			snapshot->uids[index] = entry.value;
		}

		for (index = 0; index < snapshot->config.syscall_count;
		     index++) {
			struct st_syscall_entry entry = {
				.generation = snapshot->config.generation,
				.index = index,
			};

			if (ioctl(descriptor, ST_IOC_GET_SYSCALL, &entry) == -1)
				goto retry;
			snapshot->syscalls[index] = entry.number;
		}

		if (get_config(descriptor, &final_config) == -1)
			return -1;
		if (final_config.generation != snapshot->config.generation) {
			errno = EAGAIN;
			continue;
		}

		return 0;

retry:
		if (errno != EAGAIN)
			return -1;
	}

	errno = EAGAIN;
	return -1;
}

static int list_configuration(int descriptor)
{
	struct st_configuration_snapshot snapshot;
	__u32 index;

	memset(&snapshot, 0, sizeof(snapshot));
	if (read_snapshot(descriptor, &snapshot) == -1)
		return -1;

	print_status(&snapshot.config);
	puts("program registry:");
	for (index = 0; index < snapshot.config.program_count; index++)
		printf("  %s\n", snapshot.programs[index]);

	puts("UID registry:");
	for (index = 0; index < snapshot.config.uid_count; index++)
		printf("  %" PRIu32 "\n", snapshot.uids[index]);

	puts("syscall registry:");
	for (index = 0; index < snapshot.config.syscall_count; index++)
		printf("  %" PRId32 "\n", snapshot.syscalls[index]);

	return 0;
}

int main(int argument_count, char **arguments)
{
	struct st_program program;
	struct st_syscall syscall;
	struct st_uid uid;
	struct st_config config;
	struct st_stats stats;
	__u32 max_per_second;
	unsigned long command;
	int descriptor;
	int result;

	if (argument_count < 2) {
		print_usage(stderr, arguments[0]);
		return EXIT_FAILURE;
	}

	descriptor = open(ST_DEVICE_PATH, O_RDWR | O_CLOEXEC);
	if (descriptor == -1) {
		fprintf(stderr, "%s: %s\n", ST_DEVICE_PATH, strerror(errno));
		return EXIT_FAILURE;
	}

	if (!strcmp(arguments[1], "status") && argument_count == 2) {
		result = get_config(descriptor, &config);
		if (result != -1)
			print_status(&config);
	} else if (!strcmp(arguments[1], "stats") && argument_count == 2) {
		result = ioctl(descriptor, ST_IOC_GET_STATS, &stats);
		if (result != -1)
			print_stats(&stats);
	} else if (!strcmp(arguments[1], "reset-stats") &&
		   argument_count == 2) {
		result = ioctl(descriptor, ST_IOC_RESET_STATS);
	} else if (!strcmp(arguments[1], "list") && argument_count == 2) {
		result = list_configuration(descriptor);
	} else if ((!strcmp(arguments[1], "add-program") ||
		    !strcmp(arguments[1], "remove-program")) &&
		   argument_count == 3) {
		if (prepare_program(arguments[2], &program)) {
			fprintf(stderr, "invalid program name: %s\n", arguments[2]);
			close(descriptor);
			return EXIT_FAILURE;
		}
		command = !strcmp(arguments[1], "add-program") ?
			ST_IOC_ADD_PROGRAM : ST_IOC_REMOVE_PROGRAM;
		result = ioctl(descriptor, command, &program);
	} else if ((!strcmp(arguments[1], "add-uid") ||
		    !strcmp(arguments[1], "remove-uid")) &&
		   argument_count == 3) {
		if (parse_u32(arguments[2], &uid.value)) {
			fprintf(stderr, "invalid UID: %s\n", arguments[2]);
			close(descriptor);
			return EXIT_FAILURE;
		}
		command = !strcmp(arguments[1], "add-uid") ?
			ST_IOC_ADD_UID : ST_IOC_REMOVE_UID;
		result = ioctl(descriptor, command, &uid);
	} else if ((!strcmp(arguments[1], "add-syscall") ||
		    !strcmp(arguments[1], "remove-syscall")) &&
		   argument_count == 3) {
		if (parse_syscall(arguments[2], &syscall.number)) {
			fprintf(stderr, "invalid syscall number: %s\n", arguments[2]);
			close(descriptor);
			return EXIT_FAILURE;
		}
		command = !strcmp(arguments[1], "add-syscall") ?
			ST_IOC_ADD_SYSCALL : ST_IOC_REMOVE_SYSCALL;
		result = ioctl(descriptor, command, &syscall);
	} else if (!strcmp(arguments[1], "set-max") && argument_count == 3) {
		if (parse_u32(arguments[2], &max_per_second) ||
		    max_per_second == 0 || max_per_second > ST_MAX_LIMIT) {
			fprintf(stderr, "invalid MAX value: %s\n", arguments[2]);
			close(descriptor);
			return EXIT_FAILURE;
		}
		result = ioctl(descriptor, ST_IOC_SET_MAX, &max_per_second);
	} else if (!strcmp(arguments[1], "enable") && argument_count == 2) {
		result = ioctl(descriptor, ST_IOC_ENABLE);
	} else if (!strcmp(arguments[1], "disable") && argument_count == 2) {
		result = ioctl(descriptor, ST_IOC_DISABLE);
	} else {
		print_usage(stderr, arguments[0]);
		close(descriptor);
		return EXIT_FAILURE;
	}

	if (result == -1) {
		fprintf(stderr, "%s: %s\n", arguments[1], strerror(errno));
		close(descriptor);
		return EXIT_FAILURE;
	}

	if (close(descriptor) == -1) {
		fprintf(stderr, "close: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
