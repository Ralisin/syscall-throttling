// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uidgid.h>
#include <linux/user_namespace.h>

#include <asm/unistd.h>

#include "internal.h"

struct st_state {
	struct st_config config;
	struct st_program programs[ST_MAX_PROGRAMS];
	struct st_uid uids[ST_MAX_UIDS];
	struct st_syscall syscalls[ST_MAX_SYSCALLS];
};

static DEFINE_SPINLOCK(st_config_lock);
static struct st_state st_state;

static void st_advance_generation(void)
{
	st_state.config.generation++;
	if (st_state.config.generation == 0)
		st_state.config.generation = 1;
}

static int st_normalize_program(const struct st_program *program,
				struct st_program *normalized)
{
	size_t length = strnlen(program->name, ST_PROGRAM_NAME_LEN);

	if (length == 0)
		return -EINVAL;
	if (length == ST_PROGRAM_NAME_LEN)
		return -ENAMETOOLONG;

	memset(normalized, 0, sizeof(*normalized));
	memcpy(normalized->name, program->name, length);
	return 0;
}

void st_state_initialize(void)
{
	unsigned long flags;

	spin_lock_irqsave(&st_config_lock, flags);
	memset(&st_state, 0, sizeof(st_state));
	st_state.config.max_per_second = 1;
	st_state.config.generation = 1;
	spin_unlock_irqrestore(&st_config_lock, flags);
}

void st_state_get_config(struct st_config *config)
{
	unsigned long flags;

	spin_lock_irqsave(&st_config_lock, flags);
	*config = st_state.config;
	spin_unlock_irqrestore(&st_config_lock, flags);
}

int st_state_set_max(__u32 max_per_second)
{
	unsigned long flags;

	if (max_per_second == 0 || max_per_second > ST_MAX_LIMIT)
		return -ERANGE;

	spin_lock_irqsave(&st_config_lock, flags);
	st_state.config.max_per_second = max_per_second;
	st_advance_generation();
	spin_unlock_irqrestore(&st_config_lock, flags);

	return 0;
}

void st_state_set_enabled(bool enabled)
{
	unsigned long flags;

	spin_lock_irqsave(&st_config_lock, flags);
	st_state.config.enabled = enabled;
	st_advance_generation();
	spin_unlock_irqrestore(&st_config_lock, flags);
}

int st_state_add_program(const struct st_program *program)
{
	struct st_program normalized;
	unsigned long flags;
	__u32 index;
	int result;

	result = st_normalize_program(program, &normalized);
	if (result)
		return result;

	spin_lock_irqsave(&st_config_lock, flags);
	for (index = 0; index < st_state.config.program_count; index++) {
		if (!memcmp(st_state.programs[index].name, normalized.name,
			    ST_PROGRAM_NAME_LEN)) {
			result = -EEXIST;
			goto out;
		}
	}

	if (st_state.config.program_count == ST_MAX_PROGRAMS) {
		result = -ENOSPC;
		goto out;
	}

	st_state.programs[st_state.config.program_count++] = normalized;
	st_advance_generation();
	result = 0;
out:
	spin_unlock_irqrestore(&st_config_lock, flags);
	return result;
}

int st_state_remove_program(const struct st_program *program)
{
	struct st_program normalized;
	unsigned long flags;
	__u32 index;
	int result;

	result = st_normalize_program(program, &normalized);
	if (result)
		return result;

	spin_lock_irqsave(&st_config_lock, flags);
	for (index = 0; index < st_state.config.program_count; index++) {
		if (!memcmp(st_state.programs[index].name, normalized.name,
			    ST_PROGRAM_NAME_LEN))
			break;
	}

	if (index == st_state.config.program_count) {
		result = -ENOENT;
		goto out;
	}

	st_state.config.program_count--;
	if (index != st_state.config.program_count)
		st_state.programs[index] =
			st_state.programs[st_state.config.program_count];
	memset(&st_state.programs[st_state.config.program_count], 0,
	       sizeof(st_state.programs[0]));
	st_advance_generation();
	result = 0;
out:
	spin_unlock_irqrestore(&st_config_lock, flags);
	return result;
}

int st_state_add_uid(const struct st_uid *uid)
{
	unsigned long flags;
	__u32 index;
	int result;

	if (!uid_valid(make_kuid(&init_user_ns, uid->value)))
		return -EINVAL;

	spin_lock_irqsave(&st_config_lock, flags);
	for (index = 0; index < st_state.config.uid_count; index++) {
		if (st_state.uids[index].value == uid->value) {
			result = -EEXIST;
			goto out;
		}
	}

	if (st_state.config.uid_count == ST_MAX_UIDS) {
		result = -ENOSPC;
		goto out;
	}

	st_state.uids[st_state.config.uid_count++] = *uid;
	st_advance_generation();
	result = 0;
out:
	spin_unlock_irqrestore(&st_config_lock, flags);
	return result;
}

int st_state_remove_uid(const struct st_uid *uid)
{
	unsigned long flags;
	__u32 index;
	int result;

	if (!uid_valid(make_kuid(&init_user_ns, uid->value)))
		return -EINVAL;

	spin_lock_irqsave(&st_config_lock, flags);
	for (index = 0; index < st_state.config.uid_count; index++) {
		if (st_state.uids[index].value == uid->value)
			break;
	}

	if (index == st_state.config.uid_count) {
		result = -ENOENT;
		goto out;
	}

	st_state.config.uid_count--;
	if (index != st_state.config.uid_count)
		st_state.uids[index] = st_state.uids[st_state.config.uid_count];
	memset(&st_state.uids[st_state.config.uid_count], 0,
	       sizeof(st_state.uids[0]));
	st_advance_generation();
	result = 0;
out:
	spin_unlock_irqrestore(&st_config_lock, flags);
	return result;
}

int st_state_add_syscall(const struct st_syscall *syscall)
{
	unsigned long flags;
	__u32 index;
	int result;

	if (syscall->number < 0 || syscall->number >= NR_syscalls)
		return -ERANGE;

	spin_lock_irqsave(&st_config_lock, flags);
	for (index = 0; index < st_state.config.syscall_count; index++) {
		if (st_state.syscalls[index].number == syscall->number) {
			result = -EEXIST;
			goto out;
		}
	}

	if (st_state.config.syscall_count == ST_MAX_SYSCALLS) {
		result = -ENOSPC;
		goto out;
	}

	st_state.syscalls[st_state.config.syscall_count++] = *syscall;
	st_advance_generation();
	result = 0;
out:
	spin_unlock_irqrestore(&st_config_lock, flags);
	return result;
}

int st_state_remove_syscall(const struct st_syscall *syscall)
{
	unsigned long flags;
	__u32 index;
	int result;

	if (syscall->number < 0 || syscall->number >= NR_syscalls)
		return -ERANGE;

	spin_lock_irqsave(&st_config_lock, flags);
	for (index = 0; index < st_state.config.syscall_count; index++) {
		if (st_state.syscalls[index].number == syscall->number)
			break;
	}

	if (index == st_state.config.syscall_count) {
		result = -ENOENT;
		goto out;
	}

	st_state.config.syscall_count--;
	if (index != st_state.config.syscall_count)
		st_state.syscalls[index] =
			st_state.syscalls[st_state.config.syscall_count];
	memset(&st_state.syscalls[st_state.config.syscall_count], 0,
	       sizeof(st_state.syscalls[0]));
	st_advance_generation();
	result = 0;
out:
	spin_unlock_irqrestore(&st_config_lock, flags);
	return result;
}

int st_state_get_program(struct st_program_entry *entry)
{
	unsigned long flags;
	int result;

	spin_lock_irqsave(&st_config_lock, flags);
	if (entry->generation != st_state.config.generation) {
		result = -EAGAIN;
	} else if (entry->index >= st_state.config.program_count) {
		result = -ENOENT;
	} else {
		entry->reserved = 0;
		memcpy(entry->name, st_state.programs[entry->index].name,
		       ST_PROGRAM_NAME_LEN);
		result = 0;
	}
	spin_unlock_irqrestore(&st_config_lock, flags);
	return result;
}

int st_state_get_uid(struct st_uid_entry *entry)
{
	unsigned long flags;
	int result;

	spin_lock_irqsave(&st_config_lock, flags);
	if (entry->generation != st_state.config.generation) {
		result = -EAGAIN;
	} else if (entry->index >= st_state.config.uid_count) {
		result = -ENOENT;
	} else {
		entry->value = st_state.uids[entry->index].value;
		result = 0;
	}
	spin_unlock_irqrestore(&st_config_lock, flags);
	return result;
}

int st_state_get_syscall(struct st_syscall_entry *entry)
{
	unsigned long flags;
	int result;

	spin_lock_irqsave(&st_config_lock, flags);
	if (entry->generation != st_state.config.generation) {
		result = -EAGAIN;
	} else if (entry->index >= st_state.config.syscall_count) {
		result = -ENOENT;
	} else {
		entry->number = st_state.syscalls[entry->index].number;
		result = 0;
	}
	spin_unlock_irqrestore(&st_config_lock, flags);
	return result;
}

bool st_state_matches(__u32 syscall_number, const char *program_name,
		      __u32 effective_uid)
{
	unsigned long flags;
	bool identity_matches = false;
	bool syscall_matches = false;
	__u32 index;

	spin_lock_irqsave(&st_config_lock, flags);
	if (!st_state.config.enabled)
		goto out;

	for (index = 0; index < st_state.config.syscall_count; index++) {
		if ((__u32)st_state.syscalls[index].number == syscall_number) {
			syscall_matches = true;
			break;
		}
	}
	if (!syscall_matches)
		goto out;

	for (index = 0; index < st_state.config.program_count; index++) {
		if (!strncmp(st_state.programs[index].name, program_name,
			     ST_PROGRAM_NAME_LEN)) {
			identity_matches = true;
			goto out;
		}
	}

	for (index = 0; index < st_state.config.uid_count; index++) {
		if (st_state.uids[index].value == effective_uid) {
			identity_matches = true;
			break;
		}
	}
out:
	spin_unlock_irqrestore(&st_config_lock, flags);
	return syscall_matches && identity_matches;
}
