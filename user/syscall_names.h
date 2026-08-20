/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SYSCALL_NAMES_H
#define SYSCALL_NAMES_H

#include <stddef.h>

#include "syscall_throttle.h"

int st_resolve_syscall(const char *text, __s32 *number);
int st_lookup_syscall_name(__s32 number, char *name, size_t size);

#endif
