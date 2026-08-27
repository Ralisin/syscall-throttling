#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
	printf 'Run this test as root.\n' >&2
	exit 1
fi

project_directory=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
module_path=$project_directory/kernel/syscall_throttle.ko
control=$project_directory/user/throttle_ctl
workload=$project_directory/tests/test_stress
module_directory=/sys/module/syscall_throttle
temporary_directory=$(mktemp -d)

unload_module()
{
	if [ -d "$module_directory" ]; then
		rmmod syscall_throttle
	fi
}

cleanup()
{
	unload_module
	rm -rf -- "$temporary_directory"
}

configure_program()
{
	max=$1
	syscall_number=${2:-39}
	"$control" add-program test_stress
	"$control" add-syscall "$syscall_number"
	"$control" set-max "$max"
}

elapsed_from()
{
	sed -n 's/^elapsed_ns=\([0-9][0-9]*\).*/\1/p' "$1"
}

require_fast()
{
	value=$(elapsed_from "$1")
	if [ "$value" -gt 800000000 ]; then
		printf '%s took too long: %s ns\n' "$2" "$value" >&2
		exit 1
	fi
}

require_throttled()
{
	value=$(elapsed_from "$1")
	if [ "$value" -lt 850000000 ] || [ "$value" -gt 2200000000 ]; then
		printf '%s had unexpected duration: %s ns\n' "$2" "$value" >&2
		exit 1
	fi
}

trap cleanup EXIT INT TERM
unload_module

insmod "$module_path"
configure_program 1
"$workload" getpid 100 100 >"$temporary_directory/off"
require_fast "$temporary_directory/off" 'monitor-off workload'
unload_module

insmod "$module_path"
"$control" add-program not_selected
"$control" add-syscall 39
"$control" set-max 1
"$control" enable
"$workload" getpid 100 10 >"$temporary_directory/unregistered"
require_fast "$temporary_directory/unregistered" 'unregistered workload'
unload_module

insmod "$module_path"
configure_program 100
"$control" enable
"$workload" getpid 10 5 >"$temporary_directory/below"
require_fast "$temporary_directory/below" 'below-MAX workload'
unload_module

for case_value in '2 1' '10 5' '100 50'; do
	set -- $case_value
	threads=$1
	max=$2
	insmod "$module_path"
	configure_program "$max"
	"$control" enable
	output="$temporary_directory/above-$threads"
	"$workload" getpid "$threads" 1 >"$output"
	require_throttled "$output" "$threads-thread workload"
	unload_module
done

insmod "$module_path"
configure_program 1000000 0
"$control" enable
"$workload" read 100 1 100 >"$temporary_directory/read"
require_fast "$temporary_directory/read" 'blocking-read workload'
unload_module

insmod "$module_path"
configure_program 1
"$control" enable
"$workload" getpid 100 1 >"$temporary_directory/disable" &
workload_pid=$!
sleep 0.2
"$control" disable
wait "$workload_pid"
require_fast "$temporary_directory/disable" 'disable release'
unload_module

insmod "$module_path"
configure_program 1
"$control" enable
"$workload" getpid 100 1 >"$temporary_directory/unload" 2>&1 &
workload_pid=$!
sleep 0.2
unload_module
if wait "$workload_pid"; then
	printf 'unload unexpectedly executed every waiting syscall\n' >&2
	exit 1
fi

insmod "$module_path"
configure_program 1
"$control" enable
"$workload" getpid 100 1000 >"$temporary_directory/churn" &
workload_pid=$!
iteration=1
while [ "$iteration" -le 100 ]; do
	"$control" disable
	"$control" set-max 1
	"$control" enable
	"$control" remove-program test_stress
	"$control" add-program test_stress
	"$control" set-max 1000000
	"$control" reset-stats
	iteration=$((iteration + 1))
done
"$control" disable
wait "$workload_pid"
unload_module

trap - EXIT INT TERM
rm -rf -- "$temporary_directory"
printf 'Stress runtime verification passed.\n'
