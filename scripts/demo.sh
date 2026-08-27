#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
	printf 'Run this demonstration as root.\n' >&2
	exit 1
fi

project_directory=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
module_path=$project_directory/kernel/syscall_throttle.ko
control=$project_directory/user/throttle_ctl
workload=$project_directory/tests/test_throttle
module_directory=/sys/module/syscall_throttle

unload_module()
{
	if [ -d "$module_directory" ]; then
		"$control" disable >/dev/null 2>&1 || true
		rmmod syscall_throttle
	fi
}

if [ -d "$module_directory" ]; then
	printf 'The syscall_throttle module is already loaded.\n' >&2
	exit 1
fi
if [ ! -f "$module_path" ] || [ ! -x "$control" ] ||
   [ ! -x "$workload" ]; then
	printf 'Build the project with make before running the demonstration.\n' >&2
	exit 1
fi
trap unload_module EXIT INT TERM

printf '[load]\n'
insmod "$module_path"

printf '[configure]\n'
"$control" configure --clear --program test_throttle --syscall getpid \
	--max 5 --reset-stats --enable

printf '[six matching getpid calls]\n'
"$workload" burst 6

printf '[statistics]\n'
"$control" show

printf '[safe unload]\n'
unload_module
trap - EXIT INT TERM
printf 'Demonstration completed.\n'
