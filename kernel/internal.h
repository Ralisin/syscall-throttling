/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SYSCALL_THROTTLE_INTERNAL_H
#define SYSCALL_THROTTLE_INTERNAL_H

#include <linux/types.h>

#include "syscall_throttle.h"

int st_device_register(void);
void st_device_unregister(void);

void st_state_initialize(void);
void st_state_get_config(struct st_config *config);
int st_state_set_max(__u32 max_per_second);
void st_state_set_enabled(bool enabled);

#endif
