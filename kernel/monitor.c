// SPDX-License-Identifier: GPL-2.0-only
#include <linux/atomic.h>
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
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
};

/*
 * lock protects the timestamp ring and stopping transitions. wake_generation
 * is atomic because wait conditions read it without lock; stopping is read
 * locklessly only with READ_ONCE after being published with WRITE_ONCE.
 */

static struct st_monitor st_monitor;

static void st_monitor_prune(__u64 now)
{
	while (st_monitor.count > 0) {
		__u64 oldest = st_monitor.timestamps[st_monitor.head];

		if (now - oldest < ST_WINDOW_NS)
			break;
		st_monitor.head = (st_monitor.head + 1) % ST_MAX_LIMIT;
		st_monitor.count--;
	}
}

int st_monitor_initialize(void)
{
	mutex_init(&st_monitor.lock);
	init_waitqueue_head(&st_monitor.wait_queue);
	st_monitor.timestamps = kvcalloc(ST_MAX_LIMIT,
					 sizeof(*st_monitor.timestamps), GFP_KERNEL);
	if (!st_monitor.timestamps)
		return -ENOMEM;

	atomic64_set(&st_monitor.wake_generation, 1);
	st_monitor.head = 0;
	st_monitor.count = 0;
	st_monitor.stopping = false;
	return 0;
}

void st_monitor_destroy(void)
{
	kvfree(st_monitor.timestamps);
	st_monitor.timestamps = NULL;
}

void st_monitor_configuration_changed(bool reset_window)
{
	mutex_lock(&st_monitor.lock);
	if (reset_window) {
		st_monitor.head = 0;
		st_monitor.count = 0;
	}
	atomic64_inc(&st_monitor.wake_generation);
	mutex_unlock(&st_monitor.lock);
	wake_up_all(&st_monitor.wait_queue);
}

void st_monitor_stop(void)
{
	mutex_lock(&st_monitor.lock);
	WRITE_ONCE(st_monitor.stopping, true);
	atomic64_inc(&st_monitor.wake_generation);
	mutex_unlock(&st_monitor.lock);
	wake_up_all(&st_monitor.wait_queue);
}

int st_monitor_admit(__u32 syscall_number)
{
	__u32 effective_uid;
	__u32 max_per_second;
	__u64 captured_generation;
	__u64 deadline;
	__u64 now;
	long wait_result;

	for (;;) {
		effective_uid = from_kuid(&init_user_ns, current_euid());
		mutex_lock(&st_monitor.lock);

		if (st_monitor.stopping) {
			mutex_unlock(&st_monitor.lock);
			return -EINTR;
		}

		if (!st_state_get_admission(syscall_number, current->comm,
					    effective_uid, &max_per_second)) {
			mutex_unlock(&st_monitor.lock);
			return 0;
		}

		now = ktime_get_ns();
		st_monitor_prune(now);
		if (st_monitor.count < max_per_second) {
			__u32 tail = (st_monitor.head + st_monitor.count) %
				     ST_MAX_LIMIT;

			st_monitor.timestamps[tail] = now;
			st_monitor.count++;
			mutex_unlock(&st_monitor.lock);
			return 0;
		}

		deadline = st_monitor.timestamps[st_monitor.head] + ST_WINDOW_NS;
		captured_generation = atomic64_read(&st_monitor.wake_generation);
		mutex_unlock(&st_monitor.lock);

		wait_result = wait_event_interruptible_hrtimeout(
			st_monitor.wait_queue,
			READ_ONCE(st_monitor.stopping) ||
			atomic64_read(&st_monitor.wake_generation) !=
				captured_generation,
			ns_to_ktime(deadline - now));
		if (wait_result == -ERESTARTSYS)
			return -ERESTARTSYS;
	}
}
