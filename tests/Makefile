CC ?= cc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -I../include
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c11

.PHONY: all clean

SYSCALL_EXAMPLES := \
	syscalls/getpid/st_getpid \
	syscalls/getuid/st_getuid \
	syscalls/write/st_write \
	syscalls/openat/st_openat \
	syscalls/read/st_read \
	syscalls/sensitive/unlinkat/st_unlinkat \
	syscalls/sensitive/mprotect/st_mprotect \
	syscalls/sensitive/futex/st_futex \
	syscalls/sensitive/connect/st_connect \
	syscalls/sensitive/kill/st_kill \
	syscalls/sensitive/execve/st_execve

all: test_device test_hook test_registry test_throttle test_stats test_stress \
	$(SYSCALL_EXAMPLES)

test_device: test_device.c ../include/syscall_throttle.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_device.c

test_hook: test_hook.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -o $@ test_hook.c

test_registry: test_registry.c ../include/syscall_throttle.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -o $@ test_registry.c

test_throttle: test_throttle.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_throttle.c

test_stats: test_stats.c ../include/syscall_throttle.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -o $@ test_stats.c

test_stress: test_stress.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -o $@ test_stress.c

syscalls/getpid/st_getpid: syscalls/getpid/workload.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $<

syscalls/getuid/st_getuid: syscalls/getuid/workload.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $<

syscalls/write/st_write: syscalls/write/workload.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $<

syscalls/openat/st_openat: syscalls/openat/workload.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $<

syscalls/read/st_read: syscalls/read/workload.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $<

syscalls/sensitive/unlinkat/st_unlinkat: \
		syscalls/sensitive/unlinkat/workload.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $<

syscalls/sensitive/mprotect/st_mprotect: \
		syscalls/sensitive/mprotect/workload.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $<

syscalls/sensitive/futex/st_futex: syscalls/sensitive/futex/workload.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -o $@ $<

syscalls/sensitive/connect/st_connect: syscalls/sensitive/connect/workload.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $<

syscalls/sensitive/kill/st_kill: syscalls/sensitive/kill/workload.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $<

syscalls/sensitive/execve/st_execve: syscalls/sensitive/execve/workload.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $<

clean:
	$(RM) test_device test_hook test_registry test_throttle test_stats test_stress \
		$(SYSCALL_EXAMPLES)
