#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
	printf 'Run the integration suite as root.\n' >&2
	exit 1
fi

project_directory=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
module_path=$project_directory/kernel/syscall_throttle.ko
module_directory=/sys/module/syscall_throttle

cleanup()
{
	if [ -d "$module_directory" ]; then
		rmmod syscall_throttle
	fi
}

if [ -d "$module_directory" ]; then
	printf 'The syscall_throttle module is already loaded.\n' >&2
	exit 1
fi
trap cleanup EXIT INT TERM

"$project_directory/scripts/check-environment.sh"

insmod "$module_path"
"$project_directory/tests/test_device"
rmmod syscall_throttle

"$project_directory/scripts/test-registry.sh"
"$project_directory/scripts/test-control.sh"
"$project_directory/scripts/test-syscall-examples.sh"
"$project_directory/scripts/test-sensitive-syscalls.sh"
"$project_directory/scripts/test-hook.sh"
"$project_directory/scripts/test-throttle.sh"
"$project_directory/scripts/test-stats.sh"
"$project_directory/scripts/test-stress.sh"

trap - EXIT INT TERM
printf 'Complete integration suite passed.\n'
