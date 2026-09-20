// SPDX-License-Identifier: GPL-2.0-only
#include <linux/atomic.h>
#include <linux/cred.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mm.h>
#include <linux/ptrace.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/user_namespace.h>
#include <linux/wait.h>

#include <asm/unistd.h>

#include "internal.h"

typedef long (*st_dispatch_function)(const struct pt_regs *registers, unsigned int syscall_number);

struct st_dispatch_hook {
	const char *symbol;
	st_dispatch_function original;
	struct kprobe probe;
	atomic_t active_calls;
	atomic64_t total_hits;
	atomic64_t getpid_hits;
	atomic64_t read_hits;
	bool registered;
};

/*
 * registered viene pubblicato solo dopo aver salvato il dispatcher originale.
 * active_calls serve invece durante l'unload: il modulo non puo' essere
 * rimosso finche' esiste ancora un wrapper che usa il suo codice.
 */

static long st_dispatch_wrapper(const struct pt_regs *registers, unsigned int syscall_number);

static struct st_dispatch_hook st_dispatch = {
	.symbol = "x64_sys_call",
	.active_calls = ATOMIC_INIT(0),
	.total_hits = ATOMIC64_INIT(0),
	.getpid_hits = ATOMIC64_INIT(0),
	.read_hits = ATOMIC64_INIT(0),
};

static DECLARE_WAIT_QUEUE_HEAD(st_hook_drain_queue);

static struct file *st_current_executable(void) {
	struct mm_struct *mm = current->mm;
	struct file *executable;

	if (!mm)
		return NULL;
	rcu_read_lock();
	executable = get_file_rcu(&mm->exe_file);
	rcu_read_unlock();
	return executable;
}

static bool st_should_redirect(const struct pt_regs *registers) {
	struct file *executable;
	unsigned int syscall_number = (unsigned int)registers->si;
	__u32 effective_uid;
	bool matches;

	effective_uid = from_kuid(&init_user_ns, current_euid());
	executable = st_current_executable();
	matches = st_state_matches(syscall_number,
				   executable ? &executable->f_path : NULL,
				   effective_uid);
	if (executable)
		fput(executable);
	return matches;
}

static int st_dispatch_pre_handler(struct kprobe *probe, struct pt_regs *registers) {
	unsigned long return_address;

	(void)probe;

	if (!smp_load_acquire(&st_dispatch.registered))
		return 0;

	if (!st_should_redirect(registers))
		return 0;

	return_address = regs_get_kernel_stack_nth(registers, 0);
	if (within_module(return_address, THIS_MODULE))
		return 0;

	atomic_inc(&st_dispatch.active_calls);
	instruction_pointer_set(registers, (unsigned long)st_dispatch_wrapper);

	return 1;
}

static void st_dispatch_post_handler(struct kprobe *probe, struct pt_regs *registers, unsigned long flags) {
	(void)probe;
	(void)registers;
	(void)flags;
}

static long st_dispatch_wrapper(const struct pt_regs *registers, unsigned int syscall_number) {
	struct file *executable;
	long result;

	atomic64_inc(&st_dispatch.total_hits);
	if (syscall_number == __NR_getpid)
		atomic64_inc(&st_dispatch.getpid_hits);
	else if (syscall_number == __NR_read)
		atomic64_inc(&st_dispatch.read_hits);

	executable = st_current_executable();
	result = st_monitor_admit(syscall_number, executable ? &executable->f_path : NULL);
	if (executable)
		fput(executable);
	if (!result)
		result = st_dispatch.original(registers, syscall_number);

	if (atomic_dec_and_test(&st_dispatch.active_calls))
		wake_up_all(&st_hook_drain_queue);

	return result;
}

static int st_counter_get(char *buffer, const struct kernel_param *parameter) {
	const atomic64_t *counter = parameter->arg;

	return scnprintf(buffer, PAGE_SIZE, "%lld\n", (long long)atomic64_read(counter));
}

static const struct kernel_param_ops st_counter_operations = {
	.get = st_counter_get,
};

module_param_cb(dispatcher_hook_hits, &st_counter_operations, &st_dispatch.total_hits, 0444);
MODULE_PARM_DESC(dispatcher_hook_hits, "Number of syscall dispatches redirected by kprobe");

module_param_cb(getpid_hook_hits, &st_counter_operations, &st_dispatch.getpid_hits, 0444);
MODULE_PARM_DESC(getpid_hook_hits, "Number of redirected getpid calls");

module_param_cb(read_hook_hits, &st_counter_operations, &st_dispatch.read_hits, 0444);
MODULE_PARM_DESC(read_hook_hits, "Number of redirected read calls");

int st_hooks_register(void) {
	int result;

	st_dispatch.probe.symbol_name = st_dispatch.symbol;
	st_dispatch.probe.pre_handler = st_dispatch_pre_handler;
	st_dispatch.probe.post_handler = st_dispatch_post_handler;

	result = register_kprobe(&st_dispatch.probe);
	if (result) {
		pr_err("impossibile registrare la kprobe su %s: %d\n", st_dispatch.symbol, result);
		return result;
	}

	st_dispatch.original = (st_dispatch_function)st_dispatch.probe.addr;
	smp_store_release(&st_dispatch.registered, true);
	pr_info("deviazione del dispatcher attivata\n");

	return 0;
}

void st_hooks_unregister(void) {
	if (!st_dispatch.registered)
		return;

	unregister_kprobe(&st_dispatch.probe);
	st_dispatch.registered = false;
	st_monitor_stop();

	wait_event(st_hook_drain_queue, atomic_read(&st_dispatch.active_calls) == 0);
	pr_info("deviazione del dispatcher disattivata\n");
}
