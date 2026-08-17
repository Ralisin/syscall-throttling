// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "syscall_throttle.h"

#define CONCURRENT_ITERATIONS 1000

static atomic_int concurrent_failure;

struct concurrent_writer_context {
	struct st_program program;
};

static int expect_error(int descriptor, unsigned long command, void *argument,
			int expected_errno, const char *label)
{
	errno = 0;
	if (ioctl(descriptor, command, argument) != -1 ||
	    errno != expected_errno) {
		fprintf(stderr, "%s: expected errno %d, got %d\n",
			label, expected_errno, errno);
		return -1;
	}
	return 0;
}

static int get_config(int descriptor, struct st_config *config)
{
	if (ioctl(descriptor, ST_IOC_GET_CONFIG, config) == -1) {
		perror("get config");
		return -1;
	}
	return 0;
}

static int test_validation(int descriptor)
{
	struct st_program empty_program = { 0 };
	struct st_program long_program;
	struct st_program_entry entry = {
		.generation = 1,
		.reserved = 1,
	};
	struct st_syscall invalid_syscall = {
		.number = INT32_MAX,
	};
	struct st_syscall unsupported_syscall = {
		.number = SYS_exit,
	};
	struct st_syscall unsupported_exit_group = {
		.number = SYS_exit_group,
	};
	struct st_syscall unsupported_sigreturn = {
		.number = SYS_rt_sigreturn,
	};
	struct st_uid invalid_uid = {
		.value = UINT32_MAX,
	};
	unsigned long unknown_command = _IO(ST_IOC_MAGIC, 0x7f);
	unsigned long wrong_magic = _IO(ST_IOC_MAGIC + 1, 0x01);
	unsigned long wrong_size = _IOW(ST_IOC_MAGIC, 0x07, uint64_t);
	uint32_t zero_max = 0;
	uint32_t excessive_max = ST_MAX_LIMIT + 1U;
	uint32_t valid_max = 1;

	memset(&long_program, 'x', sizeof(long_program));

	if (expect_error(descriptor, ST_IOC_ADD_PROGRAM, (void *)1, EFAULT,
			 "invalid user pointer") ||
	    expect_error(descriptor, ST_IOC_ADD_PROGRAM, &empty_program, EINVAL,
			 "empty program") ||
	    expect_error(descriptor, ST_IOC_ADD_PROGRAM, &long_program,
			 ENAMETOOLONG, "unterminated program") ||
	    expect_error(descriptor, ST_IOC_ADD_UID, &invalid_uid, EINVAL,
			 "invalid UID") ||
	    expect_error(descriptor, ST_IOC_ADD_SYSCALL, &invalid_syscall,
			 ERANGE, "invalid syscall") ||
	    expect_error(descriptor, ST_IOC_ADD_SYSCALL, &unsupported_syscall,
			 EOPNOTSUPP, "non-returning syscall") ||
	    expect_error(descriptor, ST_IOC_ADD_SYSCALL,
			 &unsupported_exit_group, EOPNOTSUPP,
			 "non-returning exit-group syscall") ||
	    expect_error(descriptor, ST_IOC_ADD_SYSCALL,
			 &unsupported_sigreturn, EOPNOTSUPP,
			 "special sigreturn syscall") ||
	    expect_error(descriptor, ST_IOC_SET_MAX, (void *)1, EFAULT,
			 "invalid MAX pointer") ||
	    expect_error(descriptor, ST_IOC_SET_MAX, &zero_max, ERANGE,
			 "zero MAX") ||
	    expect_error(descriptor, ST_IOC_SET_MAX, &excessive_max, ERANGE,
			 "excessive MAX") ||
	    expect_error(descriptor, ST_IOC_GET_PROGRAM, &entry, EINVAL,
			 "nonzero reserved field") ||
	    expect_error(descriptor, ST_IOC_GET_STATS, (void *)1, EFAULT,
			 "invalid stats pointer") ||
	    expect_error(descriptor, wrong_magic, NULL, ENOTTY,
			 "wrong ioctl magic") ||
	    expect_error(descriptor, wrong_size, &valid_max, ENOTTY,
			 "wrong ioctl size") ||
	    expect_error(descriptor, unknown_command, NULL, ENOTTY,
			 "unknown command"))
		return -1;
	if (ioctl(descriptor, ST_IOC_SET_MAX, &valid_max) == -1)
		return -1;

	return 0;
}

static int test_basic_operations(int descriptor)
{
	struct st_program program = { .name = "registry_test" };
	struct st_program_entry program_entry;
	struct st_syscall syscall = { .number = SYS_getpid };
	struct st_syscall_entry syscall_entry;
	struct st_uid uid = { .value = 1000 };
	struct st_uid_entry uid_entry;
	struct st_config initial;
	struct st_config populated;

	if (get_config(descriptor, &initial))
		return -1;
	if (initial.program_count || initial.uid_count || initial.syscall_count) {
		fprintf(stderr, "registries are not initially empty\n");
		return -1;
	}

	if (ioctl(descriptor, ST_IOC_ADD_PROGRAM, &program) == -1 ||
	    ioctl(descriptor, ST_IOC_ADD_UID, &uid) == -1 ||
	    ioctl(descriptor, ST_IOC_ADD_SYSCALL, &syscall) == -1) {
		perror("add basic entry");
		return -1;
	}

	if (expect_error(descriptor, ST_IOC_ADD_PROGRAM, &program, EEXIST,
			 "duplicate program") ||
	    expect_error(descriptor, ST_IOC_ADD_UID, &uid, EEXIST,
			 "duplicate UID") ||
	    expect_error(descriptor, ST_IOC_ADD_SYSCALL, &syscall, EEXIST,
			 "duplicate syscall"))
		return -1;

	if (get_config(descriptor, &populated))
		return -1;
	if (populated.program_count != 1 || populated.uid_count != 1 ||
	    populated.syscall_count != 1) {
		fprintf(stderr, "unexpected populated counts\n");
		return -1;
	}

	memset(&program_entry, 0, sizeof(program_entry));
	program_entry.generation = populated.generation;
	if (ioctl(descriptor, ST_IOC_GET_PROGRAM, &program_entry) == -1 ||
	    strcmp(program_entry.name, program.name)) {
		fprintf(stderr, "program readback failed\n");
		return -1;
	}

	memset(&uid_entry, 0, sizeof(uid_entry));
	uid_entry.generation = populated.generation;
	if (ioctl(descriptor, ST_IOC_GET_UID, &uid_entry) == -1 ||
	    uid_entry.value != uid.value) {
		fprintf(stderr, "UID readback failed\n");
		return -1;
	}

	memset(&syscall_entry, 0, sizeof(syscall_entry));
	syscall_entry.generation = populated.generation;
	if (ioctl(descriptor, ST_IOC_GET_SYSCALL, &syscall_entry) == -1 ||
	    syscall_entry.number != syscall.number) {
		fprintf(stderr, "syscall readback failed\n");
		return -1;
	}

	program_entry.generation = initial.generation;
	if (expect_error(descriptor, ST_IOC_GET_PROGRAM, &program_entry, EAGAIN,
			 "stale generation"))
		return -1;

	program_entry.generation = populated.generation;
	program_entry.index = populated.program_count;
	if (expect_error(descriptor, ST_IOC_GET_PROGRAM, &program_entry, ENOENT,
			 "out-of-range index"))
		return -1;

	if (ioctl(descriptor, ST_IOC_REMOVE_PROGRAM, &program) == -1 ||
	    ioctl(descriptor, ST_IOC_REMOVE_UID, &uid) == -1 ||
	    ioctl(descriptor, ST_IOC_REMOVE_SYSCALL, &syscall) == -1) {
		perror("remove basic entry");
		return -1;
	}

	if (expect_error(descriptor, ST_IOC_REMOVE_PROGRAM, &program, ENOENT,
			 "absent program") ||
	    expect_error(descriptor, ST_IOC_REMOVE_UID, &uid, ENOENT,
			 "absent UID") ||
	    expect_error(descriptor, ST_IOC_REMOVE_SYSCALL, &syscall, ENOENT,
			 "absent syscall"))
		return -1;

	return 0;
}

static int test_capacities(int descriptor)
{
	struct st_program program;
	struct st_syscall syscall;
	struct st_uid uid;
	unsigned int index;

	for (index = 0; index < ST_MAX_PROGRAMS; index++) {
		memset(&program, 0, sizeof(program));
		snprintf(program.name, sizeof(program.name), "program_%02u", index);
		if (ioctl(descriptor, ST_IOC_ADD_PROGRAM, &program) == -1) {
			perror("fill program registry");
			return -1;
		}
	}
	memset(&program, 0, sizeof(program));
	strcpy(program.name, "overflow");
	if (expect_error(descriptor, ST_IOC_ADD_PROGRAM, &program, ENOSPC,
			 "full program registry"))
		return -1;

	for (index = 0; index < ST_MAX_UIDS; index++) {
		uid.value = index;
		if (ioctl(descriptor, ST_IOC_ADD_UID, &uid) == -1) {
			perror("fill UID registry");
			return -1;
		}
	}
	uid.value = ST_MAX_UIDS;
	if (expect_error(descriptor, ST_IOC_ADD_UID, &uid, ENOSPC,
			 "full UID registry"))
		return -1;

	for (index = 0; index < ST_MAX_SYSCALLS; index++) {
		syscall.number = 100 + index;
		if (ioctl(descriptor, ST_IOC_ADD_SYSCALL, &syscall) == -1) {
			perror("fill syscall registry");
			return -1;
		}
	}
	syscall.number = 100 + ST_MAX_SYSCALLS;
	if (expect_error(descriptor, ST_IOC_ADD_SYSCALL, &syscall, ENOSPC,
			 "full syscall registry"))
		return -1;

	for (index = 0; index < ST_MAX_PROGRAMS; index++) {
		memset(&program, 0, sizeof(program));
		snprintf(program.name, sizeof(program.name), "program_%02u", index);
		if (ioctl(descriptor, ST_IOC_REMOVE_PROGRAM, &program) == -1)
			return -1;
	}
	for (index = 0; index < ST_MAX_UIDS; index++) {
		uid.value = index;
		if (ioctl(descriptor, ST_IOC_REMOVE_UID, &uid) == -1)
			return -1;
	}
	for (index = 0; index < ST_MAX_SYSCALLS; index++) {
		syscall.number = 100 + index;
		if (ioctl(descriptor, ST_IOC_REMOVE_SYSCALL, &syscall) == -1)
			return -1;
	}

	return 0;
}

static void *concurrent_writer(void *argument)
{
	const struct concurrent_writer_context *context = argument;
	int descriptor;
	int iteration;

	descriptor = open(ST_DEVICE_PATH, O_RDWR | O_CLOEXEC);
	if (descriptor == -1) {
		atomic_store(&concurrent_failure, 1);
		return NULL;
	}

	for (iteration = 0; iteration < CONCURRENT_ITERATIONS; iteration++) {
		if (ioctl(descriptor, ST_IOC_ADD_PROGRAM,
			  &context->program) == -1 ||
		    ioctl(descriptor, ST_IOC_REMOVE_PROGRAM,
			  &context->program) == -1) {
			atomic_store(&concurrent_failure, 1);
			break;
		}
		sched_yield();
	}

	close(descriptor);
	return NULL;
}

static void *concurrent_reader(void *argument)
{
	struct st_program_entry entry;
	struct st_config config;
	int descriptor;
	int iteration;

	(void)argument;
	descriptor = open(ST_DEVICE_PATH, O_RDWR | O_CLOEXEC);
	if (descriptor == -1) {
		atomic_store(&concurrent_failure, 1);
		return NULL;
	}

	for (iteration = 0; iteration < CONCURRENT_ITERATIONS; iteration++) {
		if (ioctl(descriptor, ST_IOC_GET_CONFIG, &config) == -1) {
			atomic_store(&concurrent_failure, 1);
			break;
		}
		if (config.program_count == 0)
			continue;

		memset(&entry, 0, sizeof(entry));
		entry.generation = config.generation;
		if (ioctl(descriptor, ST_IOC_GET_PROGRAM, &entry) == -1) {
			if (errno == EAGAIN)
				continue;
			atomic_store(&concurrent_failure, 1);
			break;
		}
		if (!memchr(entry.name, '\0', sizeof(entry.name))) {
			atomic_store(&concurrent_failure, 1);
			break;
		}
	}

	close(descriptor);
	return NULL;
}

static int test_concurrency(void)
{
	static const char writer_names[3][ST_PROGRAM_NAME_LEN] = {
		"concurrent_0",
		"concurrent_1",
		"concurrent_2",
	};
	struct concurrent_writer_context contexts[3];
	pthread_t writers[3];
	pthread_t readers[3];
	int readers_created = 0;
	int writers_created = 0;
	int index;

	for (index = 0; index < 3; index++) {
		memset(&contexts[index], 0, sizeof(contexts[index]));
		memcpy(contexts[index].program.name, writer_names[index],
		       ST_PROGRAM_NAME_LEN);
		if (pthread_create(&writers[index], NULL, concurrent_writer,
				   &contexts[index]))
			break;
		writers_created++;
	}

	for (index = 0; index < 3; index++) {
		if (pthread_create(&readers[index], NULL, concurrent_reader, NULL))
			break;
		readers_created++;
	}

	for (index = 0; index < writers_created; index++)
		pthread_join(writers[index], NULL);
	for (index = 0; index < readers_created; index++)
		pthread_join(readers[index], NULL);

	if (writers_created != 3 || readers_created != 3 ||
	    atomic_load(&concurrent_failure)) {
		fprintf(stderr, "concurrent registry test failed\n");
		return -1;
	}

	return 0;
}

int main(void)
{
	struct st_config config;
	int descriptor;

	if (geteuid() != 0) {
		fprintf(stderr, "registry test must run as root\n");
		return EXIT_FAILURE;
	}

	descriptor = open(ST_DEVICE_PATH, O_RDWR | O_CLOEXEC);
	if (descriptor == -1) {
		perror("open device");
		return EXIT_FAILURE;
	}

	if (test_validation(descriptor) ||
	    test_basic_operations(descriptor) ||
	    test_capacities(descriptor) ||
	    test_concurrency() || get_config(descriptor, &config) ||
	    config.program_count || config.uid_count || config.syscall_count) {
		close(descriptor);
		return EXIT_FAILURE;
	}

	close(descriptor);
	puts("registry test passed");
	return EXIT_SUCCESS;
}
