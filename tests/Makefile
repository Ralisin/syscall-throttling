CC ?= cc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -I../include
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c11

.PHONY: all clean

all: test_device test_hook test_registry test_throttle test_stats

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

clean:
	$(RM) test_device test_hook test_registry test_throttle test_stats
