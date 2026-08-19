/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SYSCALL_THROTTLE_INTERNAL_H
#define SYSCALL_THROTTLE_INTERNAL_H

#include <linux/types.h>

#include "syscall_throttle.h"

int st_device_register(void);
void st_device_unregister(void);

int st_hooks_register(void);
void st_hooks_unregister(void);

int st_monitor_initialize(void);
void st_monitor_destroy(void);
void st_monitor_configuration_changed(bool reset_window);
void st_monitor_configuration_applied(bool reset_window, bool reset_stats);
void st_monitor_stop(void);
int st_monitor_admit(__u32 syscall_number);
void st_monitor_get_stats(struct st_stats *stats);
void st_monitor_reset_stats(void);

void st_state_initialize(void);
void st_state_get_config(struct st_config *config);
int st_state_set_max(__u32 max_per_second);
void st_state_set_enabled(bool enabled);
int st_state_add_program(const struct st_program *program);
int st_state_remove_program(const struct st_program *program);
int st_state_add_uid(const struct st_uid *uid);
int st_state_remove_uid(const struct st_uid *uid);
int st_state_add_syscall(const struct st_syscall *syscall);
int st_state_remove_syscall(const struct st_syscall *syscall);
int st_state_get_program(struct st_program_entry *entry);
int st_state_get_uid(struct st_uid_entry *entry);
int st_state_get_syscall(struct st_syscall_entry *entry);
int st_state_configure(const struct st_configuration_update *update);
bool st_state_matches(__u32 syscall_number, const char *program_name,
		      __u32 effective_uid);
bool st_state_get_admission(__u32 syscall_number, const char *program_name,
			    __u32 effective_uid, __u32 *max_per_second);

#endif
