// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/limits.h>
#include <linux/mutex.h>
#include <linux/namei.h>
#include <linux/path.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uidgid.h>
#include <linux/user_namespace.h>

#include <asm/unistd.h>

#include "internal.h"

struct st_registered_program {
	struct st_program visible;
	struct path parent;
	char basename[NAME_MAX + 1];
};

struct st_state {
	struct st_config config;
	struct st_registered_program programs[ST_MAX_PROGRAMS];
	struct st_uid uids[ST_MAX_UIDS];
	struct st_syscall syscalls[ST_MAX_SYSCALLS];
};

static DEFINE_SPINLOCK(st_config_lock);
static DEFINE_MUTEX(st_update_lock);
/*
 * Il pre-handler legge lo stato sotto st_config_lock. Le scritture vengono
 * anche serializzate da st_update_lock, perche' la risoluzione dei path puo'
 * dormire e deve quindi avvenire fuori dallo spinlock.
 */
static struct st_state st_state;

static void st_advance_generation(void) {
	st_state.config.generation++;
	if (st_state.config.generation == 0)
		st_state.config.generation = 1;
}

static int st_normalize_program(const struct st_program *program, struct st_program *normalized) {
	size_t length = strnlen(program->path, ST_PROGRAM_PATH_LEN);

	if (length == 0)
		return -EINVAL;
	if (length == ST_PROGRAM_PATH_LEN)
		return -ENAMETOOLONG;
	if (program->path[0] != '/')
		return -EINVAL;

	memset(normalized, 0, sizeof(*normalized));
	memcpy(normalized->path, program->path, length);
	return 0;
}

static void st_release_program(struct st_registered_program *program) {
	if (program->parent.dentry)
		path_put(&program->parent);
	memset(program, 0, sizeof(*program));
}

static void st_release_programs(struct st_state *state) {
	__u32 index;

	for (index = 0; index < state->config.program_count; index++)
		st_release_program(&state->programs[index]);
	state->config.program_count = 0;
}

static int st_resolve_program(const struct st_program *program, struct st_registered_program *resolved) {
	struct path target;
	struct dentry *parent;
	int result;

	memset(resolved, 0, sizeof(*resolved));
	result = st_normalize_program(program, &resolved->visible);
	if (result)
		return result;

	result = kern_path(resolved->visible.path, LOOKUP_FOLLOW, &target);
	if (result)
		return result;
	if (!S_ISREG(d_inode(target.dentry)->i_mode)) {
		path_put(&target);
		return -EINVAL;
	}

	for (;;) {
		parent = dget_parent(target.dentry);
		spin_lock(&target.dentry->d_lock);
		if (target.dentry->d_parent == parent) {
			memcpy(resolved->basename, target.dentry->d_name.name, target.dentry->d_name.len);
			resolved->basename[target.dentry->d_name.len] = '\0';
			spin_unlock(&target.dentry->d_lock);
			break;
		}
		spin_unlock(&target.dentry->d_lock);
		dput(parent);
	}
	resolved->parent.mnt = mntget(target.mnt);
	resolved->parent.dentry = parent;
	path_put(&target);
	return 0;
}

static bool st_same_program_slot(const struct st_registered_program *left, const struct st_registered_program *right) {
	return path_equal(&left->parent, &right->parent) && !strcmp(left->basename, right->basename);
}

static bool st_program_matches(const struct st_registered_program *program, const struct path *executable) {
	struct dentry *dentry;
	bool matches;

	if (!executable)
		return false;
	
	dentry = executable->dentry;
	spin_lock(&dentry->d_lock);
	matches = executable->mnt == program->parent.mnt &&
		  dentry->d_parent == program->parent.dentry &&
		  dentry->d_name.len == strlen(program->basename) &&
		  !memcmp(dentry->d_name.name, program->basename,
			  dentry->d_name.len);
	spin_unlock(&dentry->d_lock);
	return matches;
}

static int st_validate_syscall(const struct st_syscall *syscall) {
	if (syscall->number < 0 || syscall->number >= NR_syscalls)
		return -ERANGE;
	if (syscall->number == __NR_exit ||
	    syscall->number == __NR_exit_group ||
	    syscall->number == __NR_rt_sigreturn)
		return -EOPNOTSUPP;
	return 0;
}

void st_state_initialize(void) {
	unsigned long flags;

	spin_lock_irqsave(&st_config_lock, flags);
	memset(&st_state, 0, sizeof(st_state));
	st_state.config.max_per_second = 1;
	st_state.config.generation = 1;
	spin_unlock_irqrestore(&st_config_lock, flags);
}

void st_state_destroy(void) {
	mutex_lock(&st_update_lock);
	st_release_programs(&st_state);
	mutex_unlock(&st_update_lock);
}

void st_state_get_config(struct st_config *config) {
	unsigned long flags;

	spin_lock_irqsave(&st_config_lock, flags);
	*config = st_state.config;
	spin_unlock_irqrestore(&st_config_lock, flags);
}

int st_state_set_max(__u32 max_per_second) {
	unsigned long flags;

	if (max_per_second == 0 || max_per_second > ST_MAX_LIMIT)
		return -ERANGE;

	mutex_lock(&st_update_lock);
	spin_lock_irqsave(&st_config_lock, flags);
	st_state.config.max_per_second = max_per_second;
	st_advance_generation();
	spin_unlock_irqrestore(&st_config_lock, flags);
	mutex_unlock(&st_update_lock);
	st_monitor_configuration_changed(false);

	return 0;
}

void st_state_set_enabled(bool enabled) {
	unsigned long flags;

	mutex_lock(&st_update_lock);
	spin_lock_irqsave(&st_config_lock, flags);
	st_state.config.enabled = enabled;
	st_advance_generation();
	spin_unlock_irqrestore(&st_config_lock, flags);
	mutex_unlock(&st_update_lock);
	st_monitor_configuration_changed(!enabled);
}

int st_state_add_program(const struct st_program *program) {
	struct st_registered_program resolved;
	unsigned long flags;
	__u32 index;
	int result;

	result = st_resolve_program(program, &resolved);
	if (result)
		return result;

	mutex_lock(&st_update_lock);
	spin_lock_irqsave(&st_config_lock, flags);
	for (index = 0; index < st_state.config.program_count; index++) {
		if (st_same_program_slot(&st_state.programs[index], &resolved)) {
			result = -EEXIST;
			goto out;
		}
	}

	if (st_state.config.program_count == ST_MAX_PROGRAMS) {
		result = -ENOSPC;
		goto out;
	}

	st_state.programs[st_state.config.program_count++] = resolved;
	memset(&resolved, 0, sizeof(resolved));
	st_advance_generation();
	result = 0;
out:
	spin_unlock_irqrestore(&st_config_lock, flags);
	mutex_unlock(&st_update_lock);
	st_release_program(&resolved);
	if (!result)
		st_monitor_configuration_changed(false);
	return result;
}

int st_state_remove_program(const struct st_program *program) {
	struct st_program normalized;
	struct st_registered_program removed = { 0 };
	unsigned long flags;
	__u32 index;
	int result;

	result = st_normalize_program(program, &normalized);
	if (result)
		return result;

	mutex_lock(&st_update_lock);
	spin_lock_irqsave(&st_config_lock, flags);
	for (index = 0; index < st_state.config.program_count; index++) {
		if (!memcmp(st_state.programs[index].visible.path,
			    normalized.path, ST_PROGRAM_PATH_LEN))
			break;
	}

	if (index == st_state.config.program_count) {
		result = -ENOENT;
		goto out;
	}

	removed = st_state.programs[index];
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
	mutex_unlock(&st_update_lock);
	st_release_program(&removed);
	if (!result)
		st_monitor_configuration_changed(false);
	return result;
}

int st_state_add_uid(const struct st_uid *uid) {
	unsigned long flags;
	__u32 index;
	int result;

	if (!uid_valid(make_kuid(&init_user_ns, uid->value)))
		return -EINVAL;

	mutex_lock(&st_update_lock);
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
	mutex_unlock(&st_update_lock);
	if (!result)
		st_monitor_configuration_changed(false);
	return result;
}

int st_state_remove_uid(const struct st_uid *uid) {
	unsigned long flags;
	__u32 index;
	int result;

	if (!uid_valid(make_kuid(&init_user_ns, uid->value)))
		return -EINVAL;

	mutex_lock(&st_update_lock);
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
	mutex_unlock(&st_update_lock);
	if (!result)
		st_monitor_configuration_changed(false);
	return result;
}

int st_state_add_syscall(const struct st_syscall *syscall) {
	unsigned long flags;
	__u32 index;
	int result;

	result = st_validate_syscall(syscall);
	if (result)
		return result;

	mutex_lock(&st_update_lock);
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
	mutex_unlock(&st_update_lock);
	if (!result)
		st_monitor_configuration_changed(false);
	return result;
}

int st_state_remove_syscall(const struct st_syscall *syscall) {
	unsigned long flags;
	__u32 index;
	int result;

	result = st_validate_syscall(syscall);
	if (result)
		return result;

	mutex_lock(&st_update_lock);
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
	mutex_unlock(&st_update_lock);
	if (!result)
		st_monitor_configuration_changed(false);
	return result;
}

int st_state_get_program(struct st_program_entry *entry) {
	unsigned long flags;
	int result;

	spin_lock_irqsave(&st_config_lock, flags);
	if (entry->generation != st_state.config.generation) {
		result = -EAGAIN;
	} else if (entry->index >= st_state.config.program_count) {
		result = -ENOENT;
	} else {
		entry->reserved = 0;
		memcpy(entry->path,
		       st_state.programs[entry->index].visible.path,
		       ST_PROGRAM_PATH_LEN);
		result = 0;
	}
	spin_unlock_irqrestore(&st_config_lock, flags);
	return result;
}

int st_state_get_uid(struct st_uid_entry *entry) {
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

int st_state_get_syscall(struct st_syscall_entry *entry) {
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

bool st_state_matches(__u32 syscall_number, const struct path *executable,
		      __u32 effective_uid) {
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
		if (st_program_matches(&st_state.programs[index], executable)) {
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

bool st_state_get_admission(__u32 syscall_number,
			    const struct path *executable,
			    __u32 effective_uid,
			    __u32 *max_per_second) {
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
		if (st_program_matches(&st_state.programs[index], executable)) {
			identity_matches = true;
			break;
		}
	}
	if (!identity_matches) {
		for (index = 0; index < st_state.config.uid_count; index++) {
			if (st_state.uids[index].value == effective_uid) {
				identity_matches = true;
				break;
			}
		}
	}

	if (identity_matches)
		*max_per_second = st_state.config.max_per_second;
out:
	spin_unlock_irqrestore(&st_config_lock, flags);
	return syscall_matches && identity_matches;
}
