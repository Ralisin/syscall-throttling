#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
	printf 'Run this test as root.\n' >&2
	exit 1
fi

project_directory=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
module_path=$project_directory/kernel/syscall_throttle.ko
workload_path=$project_directory/tests/test_hook
module_directory=/sys/module/syscall_throttle

unload_module()
{
	if [ -d "$module_directory" ]; then
		rmmod syscall_throttle
	fi
}

load_module()
{
	if ! insmod "$module_path"; then
		printf 'Recent kernel messages:\n' >&2
		dmesg | tail -n 20 >&2
		exit 1
	fi
}

trap unload_module EXIT INT TERM

if [ -d "$module_directory" ]; then
	printf 'The syscall_throttle module is already loaded.\n' >&2
	exit 1
fi

if ! grep -q ' x64_sys_call$' /proc/kallsyms; then
	printf 'Required syscall dispatcher symbol is unavailable.\n' >&2
	exit 1
fi

load_module
getpid_before=$(cat "$module_directory/parameters/getpid_hook_hits")
read_before=$(cat "$module_directory/parameters/read_hook_hits")

"$workload_path"

getpid_after=$(cat "$module_directory/parameters/getpid_hook_hits")
read_after=$(cat "$module_directory/parameters/read_hook_hits")

if [ $((getpid_after - getpid_before)) -lt 80000 ]; then
	printf 'The getpid hook did not observe the complete workload.\n' >&2
	exit 1
fi

if [ "$read_after" -le "$read_before" ]; then
	printf 'The read hook did not observe the blocking read.\n' >&2
	exit 1
fi

unload_module

iteration=1
while [ "$iteration" -le 5 ]; do
	load_module
	"$workload_path" >/dev/null
	unload_module
	iteration=$((iteration + 1))
done

load_module
"$workload_path" blocking-read &
workload_pid=$!
sleep 0.2
unload_module
wait "$workload_pid"

trap - EXIT INT TERM
printf 'Hook runtime verification passed.\n'
