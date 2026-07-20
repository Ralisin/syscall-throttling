CC ?= cc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -I../include
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c11

.PHONY: all clean

all: throttle_ctl

throttle_ctl: throttle_ctl.c ../include/syscall_throttle.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ throttle_ctl.c

clean:
	$(RM) throttle_ctl
