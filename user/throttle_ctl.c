// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "syscall_throttle.h"
#include "syscall_names.h"

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
		"Usage: %s COMMAND [OPTIONS]\n\n"
		"Configuration commands:\n"
		"  configure [OPTIONS]       Apply one atomic configuration update\n"
		"  clear                     Restore an empty, disabled configuration\n"
		"  enable | disable          Turn throttling on or off\n"
		"  set-max NUMBER            Set the per-second limit\n"
		"  add-program NAME          Register an executable name\n"
		"  remove-program NAME       Deregister an executable name\n"
		"  add-uid UID               Register an effective UID\n"
		"  remove-uid UID            Deregister an effective UID\n"
		"  add-syscall NAME|NUMBER   Register a system call\n"
		"  remove-syscall NAME|NUMBER\n"
		"                            Deregister a system call\n\n"
		"Inspection commands:\n"
		"  show [--raw]              Show configuration and statistics\n"
		"  status | list | stats     Original inspection interfaces\n"
		"  reset-stats               Reset collected statistics\n\n"
		"General options:\n"
		"  -h, --help                Show help\n"
		"Run '%s help COMMAND' or '%s COMMAND --help' for command help.\n",
		program, program, program);
}

static void print_configure_help(FILE *stream, const char *program)
{
	fprintf(stream,
		"Usage: %s configure [OPTIONS]\n\n"
		"Apply all requested changes atomically. Existing registrations are\n"
		"retained and duplicate additions are harmless unless --clear is used.\n\n"
		"Options:\n"
		"  --clear                   Start from an empty, disabled configuration\n"
		"  --program NAME            Register a program (repeatable)\n"
		"  --uid UID                 Register an effective UID (repeatable)\n"
		"  --syscall NAME|NUMBER     Register a syscall (repeatable)\n"
		"  --max NUMBER              Set the per-second limit\n"
		"  --enable | --disable      Set monitor state\n"
		"  --reset-stats             Reset statistics\n"
		"  -h, --help                Show this help\n\n"
		"Example:\n"
		"  sudo %s configure --clear --program test_throttle \\\n"
		"      --syscall getpid --max 2 --enable\n",
		program, program);
}

static void print_show_help(FILE *stream, const char *program)
{
	fprintf(stream,
		"Usage: %s show [--raw]\n\n"
		"Show configuration and statistics. --raw emits stable key=value\n"
		"records with exact numeric values for scripts.\n",
		program);
}

static void print_clear_help(FILE *stream, const char *program)
{
	fprintf(stream,
		"Usage: sudo %s clear\n\n"
		"Atomically disable the monitor, set MAX to 1, empty all registries,\n"
		"clear the admission window, and reset statistics.\n",
		program);
}

static bool is_help(const char *text)
{
	return !strcmp(text, "-h") || !strcmp(text, "--help");
}

static int print_command_help(const char *program, const char *command)
{
	if (!strcmp(command, "configure"))
		print_configure_help(stdout, program);
	else if (!strcmp(command, "show"))
		print_show_help(stdout, program);
	else if (!strcmp(command, "clear"))
		print_clear_help(stdout, program);
	else {
		fprintf(stderr, "unknown command: %s\n", command);
		return -1;
	}
	return 0;
}

static int parse_u32(const char *text, __u32 *value)
{
	char *end;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno || !*text || *end || parsed > UINT32_MAX)
		return -1;
	*value = (__u32)parsed;
	return 0;
}

static int prepare_program(const char *name, struct st_program *program)
{
	size_t length = strlen(name);

	if (!length || length >= ST_PROGRAM_NAME_LEN)
		return -1;
	memset(program, 0, sizeof(*program));
	memcpy(program->name, name, length);
	return 0;
}

static int get_config(int descriptor, struct st_config *config)
{
	return ioctl(descriptor, ST_IOC_GET_CONFIG, config);
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

static double average_blocked(const struct st_stats *stats)
{
	if (!stats->elapsed_ns)
		return 0.0;
	return (double)stats->blocked_thread_time_ns /
	       (double)stats->elapsed_ns;
}

static void print_stats(const struct st_stats *stats)
{
	printf("interval_ns: %" PRIu64 "\n", (uint64_t)stats->elapsed_ns);
	printf("blocked_thread_time_ns: %" PRIu64 "\n",
	       (uint64_t)stats->blocked_thread_time_ns);
	printf("average_blocked_threads: %.6f\n", average_blocked(stats));
	printf("current_blocked_threads: %" PRIu32 "\n",
	       stats->current_blocked_threads);
	printf("peak_blocked_threads: %" PRIu32 "\n",
	       stats->peak_blocked_threads);
	printf("throttled_calls: %" PRIu64 "\n",
	       (uint64_t)stats->throttled_calls);
	printf("peak_delay_ns: %" PRIu64 "\n", (uint64_t)stats->peak_delay_ns);
	printf("peak_program: %s\n", stats->peak_program);
	printf("peak_uid: %" PRIu32 "\n", stats->peak_uid);
}

static void print_stats_raw(const struct st_stats *stats)
{
	printf("interval_ns=%" PRIu64 "\n", (uint64_t)stats->elapsed_ns);
	printf("blocked_thread_time_ns=%" PRIu64 "\n",
	       (uint64_t)stats->blocked_thread_time_ns);
	printf("average_blocked_threads=%.6f\n", average_blocked(stats));
	printf("current_blocked_threads=%" PRIu32 "\n",
	       stats->current_blocked_threads);
	printf("peak_blocked_threads=%" PRIu32 "\n",
	       stats->peak_blocked_threads);
	printf("throttled_calls=%" PRIu64 "\n",
	       (uint64_t)stats->throttled_calls);
	printf("peak_delay_ns=%" PRIu64 "\n", (uint64_t)stats->peak_delay_ns);
	printf("peak_program=%s\n", stats->peak_program);
	printf("peak_uid=%" PRIu32 "\n", stats->peak_uid);
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
		for (index = 0; index < snapshot->config.program_count; index++) {
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
		for (index = 0; index < snapshot->config.syscall_count; index++) {
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
		if (final_config.generation == snapshot->config.generation)
			return 0;
		errno = EAGAIN;
		continue;
retry:
		if (errno != EAGAIN)
			return -1;
	}
	errno = EAGAIN;
	return -1;
}

static void print_syscall(__s32 number, const char *prefix)
{
	char name[128];

	if (!st_lookup_syscall_name(number, name, sizeof(name)))
		printf("%s%s (%" PRId32 ")\n", prefix, name, number);
	else
		printf("%s%" PRId32 "\n", prefix, number);
}

static int list_configuration(int descriptor)
{
	struct st_configuration_snapshot snapshot = { 0 };
	__u32 index;

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
		print_syscall(snapshot.syscalls[index], "  ");
	return 0;
}

static int show_configuration(int descriptor, bool raw)
{
	struct st_configuration_snapshot snapshot = { 0 };
	struct st_stats stats;
	__u32 index;

	if (read_snapshot(descriptor, &snapshot) == -1 ||
	    ioctl(descriptor, ST_IOC_GET_STATS, &stats) == -1)
		return -1;
	if (raw) {
		printf("monitor=%s\n", snapshot.config.enabled ? "on" : "off");
		printf("max_per_second=%" PRIu32 "\n",
		       snapshot.config.max_per_second);
		printf("generation=%" PRIu64 "\n",
		       (uint64_t)snapshot.config.generation);
		for (index = 0; index < snapshot.config.program_count; index++)
			printf("program=%s\n", snapshot.programs[index]);
		for (index = 0; index < snapshot.config.uid_count; index++)
			printf("uid=%" PRIu32 "\n", snapshot.uids[index]);
		for (index = 0; index < snapshot.config.syscall_count; index++)
			printf("syscall=%" PRId32 "\n", snapshot.syscalls[index]);
		print_stats_raw(&stats);
		return 0;
	}

	printf("Monitor: %s\n", snapshot.config.enabled ? "enabled" : "disabled");
	printf("Global limit: %" PRIu32 " registered calls per second\n",
	       snapshot.config.max_per_second);
	printf("Configuration generation: %" PRIu64 "\n\n",
	       (uint64_t)snapshot.config.generation);
	puts("Registered programs:");
	if (!snapshot.config.program_count)
		puts("  (none)");
	for (index = 0; index < snapshot.config.program_count; index++)
		printf("  %s\n", snapshot.programs[index]);
	puts("Registered effective UIDs:");
	if (!snapshot.config.uid_count)
		puts("  (none)");
	for (index = 0; index < snapshot.config.uid_count; index++)
		printf("  %" PRIu32 "\n", snapshot.uids[index]);
	puts("Registered syscalls:");
	if (!snapshot.config.syscall_count)
		puts("  (none)");
	for (index = 0; index < snapshot.config.syscall_count; index++)
		print_syscall(snapshot.syscalls[index], "  ");
	puts("\nStatistics:");
	printf("  Observation time: %.3f s\n", (double)stats.elapsed_ns / 1e9);
	printf("  Average blocked threads: %.3f\n", average_blocked(&stats));
	printf("  Currently blocked: %" PRIu32 "\n",
	       stats.current_blocked_threads);
	printf("  Peak blocked threads: %" PRIu32 "\n",
	       stats.peak_blocked_threads);
	printf("  Throttled calls: %" PRIu64 "\n",
	       (uint64_t)stats.throttled_calls);
	printf("  Peak delay: %.3f ms\n", (double)stats.peak_delay_ns / 1e6);
	if (stats.peak_delay_ns)
		printf("  Peak identity: %s (effective UID %" PRIu32 ")\n",
		       stats.peak_program, stats.peak_uid);
	else
		puts("  Peak identity: (none)");
	puts("\nUse 'show --raw' for exact numeric values.");
	return 0;
}

static int next_option_value(int argc, char **argv, int *index,
			     const char **value)
{
	if (*index + 1 >= argc)
		return -1;
	*value = argv[++*index];
	return 0;
}

static int prepare_configuration(int argc, char **argv,
				 struct st_configuration_update *update)
{
	bool state_selected = false;
	int index;

	memset(update, 0, sizeof(*update));
	for (index = 2; index < argc; index++) {
		const char *value;

		if (!strcmp(argv[index], "--clear")) {
			update->flags |= ST_CONFIGURE_CLEAR;
		} else if (!strcmp(argv[index], "--reset-stats")) {
			update->flags |= ST_CONFIGURE_RESET_STATS;
		} else if (!strcmp(argv[index], "--enable") ||
			   !strcmp(argv[index], "--disable")) {
			__u8 enabled = !strcmp(argv[index], "--enable");

			if (state_selected && update->enabled != enabled) {
				fprintf(stderr, "--enable and --disable conflict\n");
				return -1;
			}
			state_selected = true;
			update->flags |= ST_CONFIGURE_SET_ENABLED;
			update->enabled = enabled;
		} else if (!strcmp(argv[index], "--max")) {
			if (next_option_value(argc, argv, &index, &value) ||
			    parse_u32(value, &update->max_per_second) ||
			    !update->max_per_second ||
			    update->max_per_second > ST_MAX_LIMIT) {
				fprintf(stderr, "invalid value for --max\n");
				return -1;
			}
			update->flags |= ST_CONFIGURE_SET_MAX;
		} else if (!strcmp(argv[index], "--program")) {
			if (update->program_count == ST_MAX_PROGRAMS ||
			    next_option_value(argc, argv, &index, &value) ||
			    prepare_program(value,
				&update->programs[update->program_count])) {
				fprintf(stderr, "invalid or excessive --program value\n");
				return -1;
			}
			update->program_count++;
		} else if (!strcmp(argv[index], "--uid")) {
			if (update->uid_count == ST_MAX_UIDS ||
			    next_option_value(argc, argv, &index, &value) ||
			    parse_u32(value, &update->uids[update->uid_count].value)) {
				fprintf(stderr, "invalid or excessive --uid value\n");
				return -1;
			}
			update->uid_count++;
		} else if (!strcmp(argv[index], "--syscall")) {
			if (update->syscall_count == ST_MAX_SYSCALLS ||
			    next_option_value(argc, argv, &index, &value) ||
			    st_resolve_syscall(value,
				&update->syscalls[update->syscall_count].number)) {
				fprintf(stderr, "invalid or unknown --syscall value\n");
				return -1;
			}
			update->syscall_count++;
		} else {
			fprintf(stderr, "unknown configure option: %s\n", argv[index]);
			return -1;
		}
	}
	if (!update->flags && !update->program_count && !update->uid_count &&
	    !update->syscall_count) {
		fprintf(stderr, "configure requires at least one option\n");
		return -1;
	}
	return 0;
}

static int handle_configuration(int descriptor, int argc, char **argv)
{
	struct st_configuration_update *update;
	int result;

	update = calloc(1, sizeof(*update));
	if (!update) {
		errno = ENOMEM;
		return -1;
	}
	if (prepare_configuration(argc, argv, update)) {
		free(update);
		errno = EINVAL;
		return -1;
	}
	result = ioctl(descriptor, ST_IOC_CONFIGURE, update);
	free(update);
	if (result == -1)
		return -1;
	puts("Configuration applied.");
	return show_configuration(descriptor, false);
}

int main(int argc, char **argv)
{
	struct st_program program;
	struct st_syscall syscall;
	struct st_uid uid;
	struct st_config config;
	struct st_stats stats;
	struct st_configuration_update clear_update = {
		.flags = ST_CONFIGURE_CLEAR,
	};
	__u32 max_per_second;
	unsigned long command;
	int descriptor;
	int result = -1;

	if (argc == 2 && (is_help(argv[1]) || !strcmp(argv[1], "help"))) {
		print_usage(stdout, argv[0]);
		return EXIT_SUCCESS;
	}
	if (argc == 3 && !strcmp(argv[1], "help"))
		return print_command_help(argv[0], argv[2]) ?
		       EXIT_FAILURE : EXIT_SUCCESS;
	if (argc == 3 && is_help(argv[2]))
		return print_command_help(argv[0], argv[1]) ?
		       EXIT_FAILURE : EXIT_SUCCESS;
	if (argc < 2) {
		print_usage(stderr, argv[0]);
		return EXIT_FAILURE;
	}

	descriptor = open(ST_DEVICE_PATH, O_RDWR | O_CLOEXEC);
	if (descriptor == -1) {
		fprintf(stderr, "%s: %s\n", ST_DEVICE_PATH, strerror(errno));
		return EXIT_FAILURE;
	}

	if (!strcmp(argv[1], "configure")) {
		result = handle_configuration(descriptor, argc, argv);
	} else if (!strcmp(argv[1], "clear") && argc == 2) {
		result = ioctl(descriptor, ST_IOC_CONFIGURE, &clear_update);
		if (result != -1)
			result = show_configuration(descriptor, false);
	} else if (!strcmp(argv[1], "show") &&
		   (argc == 2 || (argc == 3 && !strcmp(argv[2], "--raw")))) {
		result = show_configuration(descriptor, argc == 3);
	} else if (!strcmp(argv[1], "status") && argc == 2) {
		result = get_config(descriptor, &config);
		if (result != -1)
			print_status(&config);
	} else if (!strcmp(argv[1], "stats") && argc == 2) {
		result = ioctl(descriptor, ST_IOC_GET_STATS, &stats);
		if (result != -1)
			print_stats(&stats);
	} else if (!strcmp(argv[1], "reset-stats") && argc == 2) {
		result = ioctl(descriptor, ST_IOC_RESET_STATS);
	} else if (!strcmp(argv[1], "list") && argc == 2) {
		result = list_configuration(descriptor);
	} else if ((!strcmp(argv[1], "add-program") ||
		    !strcmp(argv[1], "remove-program")) && argc == 3) {
		if (prepare_program(argv[2], &program)) {
			fprintf(stderr, "invalid program name: %s\n", argv[2]);
			errno = EINVAL;
			goto failure;
		}
		command = !strcmp(argv[1], "add-program") ?
			ST_IOC_ADD_PROGRAM : ST_IOC_REMOVE_PROGRAM;
		result = ioctl(descriptor, command, &program);
	} else if ((!strcmp(argv[1], "add-uid") ||
		    !strcmp(argv[1], "remove-uid")) && argc == 3) {
		if (parse_u32(argv[2], &uid.value)) {
			fprintf(stderr, "invalid UID: %s\n", argv[2]);
			errno = EINVAL;
			goto failure;
		}
		command = !strcmp(argv[1], "add-uid") ?
			ST_IOC_ADD_UID : ST_IOC_REMOVE_UID;
		result = ioctl(descriptor, command, &uid);
	} else if ((!strcmp(argv[1], "add-syscall") ||
		    !strcmp(argv[1], "remove-syscall")) && argc == 3) {
		if (st_resolve_syscall(argv[2], &syscall.number)) {
			fprintf(stderr, "invalid or unknown syscall: %s\n", argv[2]);
			errno = EINVAL;
			goto failure;
		}
		command = !strcmp(argv[1], "add-syscall") ?
			ST_IOC_ADD_SYSCALL : ST_IOC_REMOVE_SYSCALL;
		result = ioctl(descriptor, command, &syscall);
	} else if (!strcmp(argv[1], "set-max") && argc == 3) {
		if (parse_u32(argv[2], &max_per_second) || !max_per_second ||
		    max_per_second > ST_MAX_LIMIT) {
			fprintf(stderr, "invalid MAX value: %s\n", argv[2]);
			errno = EINVAL;
			goto failure;
		}
		result = ioctl(descriptor, ST_IOC_SET_MAX, &max_per_second);
	} else if (!strcmp(argv[1], "enable") && argc == 2) {
		result = ioctl(descriptor, ST_IOC_ENABLE);
	} else if (!strcmp(argv[1], "disable") && argc == 2) {
		result = ioctl(descriptor, ST_IOC_DISABLE);
	} else {
		print_usage(stderr, argv[0]);
		errno = EINVAL;
		goto failure;
	}

	if (result == -1)
		goto failure;
	if (close(descriptor) == -1) {
		fprintf(stderr, "close: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;

failure:
	if (result == -1)
		fprintf(stderr, "%s: %s\n", argv[1], strerror(errno));
	close(descriptor);
	return EXIT_FAILURE;
}
