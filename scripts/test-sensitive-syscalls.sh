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
	directory=$project_directory/tests/syscalls/sensitive/$name

	"$directory/configure.sh" >/dev/null
	timeout --kill-after=5s 30s "$directory/$binary"
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
run_example unlinkat st_unlinkat
run_example mprotect st_mprotect
run_example futex st_futex
run_example connect st_connect
run_example kill st_kill
run_example execve st_execve

unload_module
trap - EXIT INT TERM
printf 'Sensitive syscall verification passed.\n'
