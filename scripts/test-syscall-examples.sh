#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
	printf 'Run this test as root.\n' >&2
	exit 1
fi

project_directory=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
module_path=$project_directory/kernel/syscall_throttle.ko
control=$project_directory/user/throttle_ctl
module_directory=/sys/module/syscall_throttle

unload_module()
{
	if [ -d "$module_directory" ]; then
		"$control" clear >/dev/null 2>&1 || true
		rmmod syscall_throttle
	fi
}

run_example()
{
	name=$1
	binary=$2
	configuration=$project_directory/tests/syscalls/$name/configure.sh
	workload=$project_directory/tests/syscalls/$name/$binary

	"$configuration" >/dev/null
	"$workload"
	throttled_calls=$("$control" show --raw |
		sed -n 's/^throttled_calls=//p')
	if [ -z "$throttled_calls" ] || [ "$throttled_calls" -eq 0 ]; then
		printf '%s example did not throttle any call.\n' "$name" >&2
		exit 1
	fi
}

if [ -d "$module_directory" ]; then
	printf 'The syscall_throttle module is already loaded.\n' >&2
	exit 1
fi

trap unload_module EXIT INT TERM
insmod "$module_path"
run_example getpid st_getpid
run_example getuid st_getuid
run_example write st_write
run_example openat st_openat
run_example read st_read

unload_module
trap - EXIT INT TERM
printf 'Syscall example verification passed.\n'
