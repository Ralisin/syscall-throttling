#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
	printf 'Run this test as root.\n' >&2
	exit 1
fi

project_directory=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
module_path=$project_directory/kernel/syscall_throttle.ko
control=$project_directory/user/throttle_ctl
test_program=$project_directory/tests/test_stats
module_directory=/sys/module/syscall_throttle
test_user=${SUDO_USER:-root}

unload_module()
{
	if [ -d "$module_directory" ]; then
		rmmod syscall_throttle
	fi
}

trap unload_module EXIT INT TERM
unload_module
insmod "$module_path"

if [ "$test_user" != root ]; then
	runuser -u "$test_user" -- "$control" stats >/dev/null
	if runuser -u "$test_user" -- "$control" reset-stats; then
		printf 'Unprivileged statistics reset unexpectedly succeeded.\n' >&2
		exit 1
	fi
fi

"$test_program"
"$control" stats | grep -q '^average_blocked_threads: '

unload_module
trap - EXIT INT TERM
printf 'Statistics runtime verification passed.\n'
