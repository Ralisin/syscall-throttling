// SPDX-License-Identifier: GPL-2.0-only
#include <linux/atomic.h>
#include <linux/cred.h>
#include <linux/dcache.h>
#include <linux/errno.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/user_namespace.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>

#include "internal.h"

#define ST_WINDOW_NS NSEC_PER_SEC

struct st_monitor {
	struct mutex lock;
	wait_queue_head_t wait_queue;
	__u64 *timestamps;
	atomic64_t wake_generation;
	__u32 head;
	__u32 count;
	bool stopping;
	struct st_stats stats;
	__u64 stats_start_ns;
	__u64 stats_update_ns;
};

/*
 * lock protegge sia l'anello dei timestamp sia le statistiche. La condizione
 * della wait queue legge generation e stopping senza prendere il mutex, per
 * questo vengono usati un contatore atomico e READ_ONCE/WRITE_ONCE.
 */

static struct st_monitor st_monitor;

static void st_monitor_reset_stats_locked(__u64 now) {
	__u32 current_blocked_threads = st_monitor.stats.current_blocked_threads;

	memset(&st_monitor.stats, 0, sizeof(st_monitor.stats));
	st_monitor.stats.current_blocked_threads = current_blocked_threads;
	st_monitor.stats.peak_blocked_threads = current_blocked_threads;
	st_monitor.stats_start_ns = now;
	st_monitor.stats_update_ns = now;
}

static void st_monitor_integrate_blocked_time(__u64 now) {
	__u64 increment;
	__u64 total;
	__u64 duration = now - st_monitor.stats_update_ns;

	if (check_mul_overflow(
			duration,
			(__u64)st_monitor.stats.current_blocked_threads, &increment) || check_add_overflow(st_monitor.stats.blocked_thread_time_ns,
			increment,
			&total
		))
		st_monitor.stats.blocked_thread_time_ns = U64_MAX;
	else
		st_monitor.stats.blocked_thread_time_ns = total;
	st_monitor.stats_update_ns = now;
}

static void st_monitor_wait_started(__u64 now) {
	st_monitor_integrate_blocked_time(now);
	st_monitor.stats.current_blocked_threads++;
	if (st_monitor.stats.current_blocked_threads > st_monitor.stats.peak_blocked_threads)
		st_monitor.stats.peak_blocked_threads = st_monitor.stats.current_blocked_threads;
}

static void st_monitor_wait_finished(__u64 now, __u64 wait_started_ns,
				     const char *program_path,
				     __u32 effective_uid, bool executed) {
	__u64 measured_start;
	__u64 delay;

	st_monitor_integrate_blocked_time(now);
	if (st_monitor.stats.current_blocked_threads > 0)
		st_monitor.stats.current_blocked_threads--;

	if (!executed)
		return;

	measured_start = max(wait_started_ns, st_monitor.stats_start_ns);
	delay = now - measured_start;
	if (st_monitor.stats.throttled_calls != U64_MAX)
		st_monitor.stats.throttled_calls++;
	if (delay > st_monitor.stats.peak_delay_ns) {
		st_monitor.stats.peak_delay_ns = delay;
		st_monitor.stats.peak_uid = effective_uid;
		memcpy(st_monitor.stats.peak_program_path, program_path,
		       ST_PROGRAM_PATH_LEN);
	}
}

static void st_monitor_capture_path(const struct path *executable,
				    char *program_path) {
	char *resolved;

	memset(program_path, 0, ST_PROGRAM_PATH_LEN);
	if (!executable) {
		strscpy(program_path, "<unavailable>", ST_PROGRAM_PATH_LEN);
		return;
	}

	resolved = d_path(executable, program_path, ST_PROGRAM_PATH_LEN);
	if (IS_ERR(resolved)) {
		memset(program_path, 0, ST_PROGRAM_PATH_LEN);
		strscpy(program_path, "<unavailable>", ST_PROGRAM_PATH_LEN);
		return;
	}
	memmove(program_path, resolved, strlen(resolved) + 1);
}

static void st_monitor_prune(__u64 now) {
	while (st_monitor.count > 0) {
		__u64 oldest = st_monitor.timestamps[st_monitor.head];

		if (now - oldest < ST_WINDOW_NS)
			break;
		st_monitor.head = (st_monitor.head + 1) % ST_MAX_LIMIT;
		st_monitor.count--;
	}
}

int st_monitor_initialize(void) {
	__u64 now;

	mutex_init(&st_monitor.lock);
	init_waitqueue_head(&st_monitor.wait_queue);
	st_monitor.timestamps = kvcalloc(ST_MAX_LIMIT, sizeof(*st_monitor.timestamps), GFP_KERNEL);
	if (!st_monitor.timestamps)
		return -ENOMEM;

	atomic64_set(&st_monitor.wake_generation, 1);
	st_monitor.head = 0;
	st_monitor.count = 0;
	st_monitor.stopping = false;
	memset(&st_monitor.stats, 0, sizeof(st_monitor.stats));
	now = ktime_get_ns();
	st_monitor.stats_start_ns = now;
	st_monitor.stats_update_ns = now;
	return 0;
}

void st_monitor_destroy(void) {
	kvfree(st_monitor.timestamps);
	st_monitor.timestamps = NULL;
}

void st_monitor_configuration_changed(bool reset_window) {
	mutex_lock(&st_monitor.lock);
	if (reset_window) {
		st_monitor.head = 0;
		st_monitor.count = 0;
	}
	atomic64_inc(&st_monitor.wake_generation);
	mutex_unlock(&st_monitor.lock);
	wake_up_all(&st_monitor.wait_queue);
}

void st_monitor_stop(void) {
	mutex_lock(&st_monitor.lock);
	WRITE_ONCE(st_monitor.stopping, true);
	atomic64_inc(&st_monitor.wake_generation);
	mutex_unlock(&st_monitor.lock);
	wake_up_all(&st_monitor.wait_queue);
}

int st_monitor_admit(__u32 syscall_number, const struct path *executable) {
	char program_path[ST_PROGRAM_PATH_LEN] = { 0 };
	__u32 effective_uid;
	__u32 max_per_second;
	__u64 captured_generation;
	__u64 deadline;
	__u64 now;
	long wait_result;
	bool blocked = false;
	__u64 wait_started_ns = 0;

	effective_uid = from_kuid(&init_user_ns, current_euid());

	for (;;) {
		mutex_lock(&st_monitor.lock);

		if (st_monitor.stopping) {
			if (blocked)
				st_monitor_wait_finished(ktime_get_ns(), wait_started_ns, program_path, effective_uid, false);
			mutex_unlock(&st_monitor.lock);
			return -EINTR;
		}

		if (!st_state_get_admission(syscall_number, executable, effective_uid, &max_per_second)) {
			if (blocked)
				st_monitor_wait_finished(ktime_get_ns(), wait_started_ns, program_path, effective_uid, true);
			mutex_unlock(&st_monitor.lock);
			return 0;
		}

		now = ktime_get_ns();
		st_monitor_prune(now);
		if (st_monitor.count < max_per_second) {
			__u32 tail = (st_monitor.head + st_monitor.count) % ST_MAX_LIMIT;

			st_monitor.timestamps[tail] = now;
			st_monitor.count++;
			if (blocked)
				st_monitor_wait_finished(now, wait_started_ns, program_path, effective_uid, true);
			mutex_unlock(&st_monitor.lock);
			return 0;
		}
		if (!blocked) {
			blocked = true;
			wait_started_ns = now;
			st_monitor_capture_path(executable, program_path);
			st_monitor_wait_started(now);
		}

		deadline = st_monitor.timestamps[st_monitor.head] + ST_WINDOW_NS;
		captured_generation = atomic64_read(&st_monitor.wake_generation);
		mutex_unlock(&st_monitor.lock);

		wait_result = wait_event_interruptible_hrtimeout(
			st_monitor.wait_queue,
			READ_ONCE(st_monitor.stopping) || atomic64_read(&st_monitor.wake_generation) != captured_generation,
			ns_to_ktime(deadline - now));
		if (wait_result == -ERESTARTSYS) {
			mutex_lock(&st_monitor.lock);
			st_monitor_wait_finished(ktime_get_ns(), wait_started_ns, program_path, effective_uid, false);
			mutex_unlock(&st_monitor.lock);
			return -ERESTARTSYS;
		}
	}
}

void st_monitor_get_stats(struct st_stats *stats) {
	__u64 now;

	mutex_lock(&st_monitor.lock);
	now = ktime_get_ns();
	st_monitor_integrate_blocked_time(now);
	*stats = st_monitor.stats;
	stats->elapsed_ns = now - st_monitor.stats_start_ns;
	stats->reserved = 0;
	mutex_unlock(&st_monitor.lock);
}

void st_monitor_reset_stats(void) {
	mutex_lock(&st_monitor.lock);
	st_monitor_reset_stats_locked(ktime_get_ns());
	mutex_unlock(&st_monitor.lock);
}
