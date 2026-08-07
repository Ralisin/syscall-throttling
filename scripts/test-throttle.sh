#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
	printf 'Run this test as root.\n' >&2
	exit 1
fi

project_directory=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
module_path=$project_directory/kernel/syscall_throttle.ko
control=$project_directory/user/throttle_ctl
workload=$project_directory/tests/test_throttle
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

load_module()
{
	insmod "$module_path"
}

configure()
{
	max=$1
	program=${2:-test_throttle}
	"$control" add-program "$program"
	"$control" add-syscall 39
	"$control" set-max "$max"
	"$control" enable
}

elapsed_from()
{
	sed -n 's/^elapsed_ns=\([0-9][0-9]*\).*/\1/p' "$1"
}

assert_between()
{
	value=$1
	minimum=$2
	maximum=$3
	label=$4
	if [ "$value" -lt "$minimum" ] || [ "$value" -gt "$maximum" ]; then
		printf '%s: elapsed %s ns outside [%s, %s]\n' \
			"$label" "$value" "$minimum" "$maximum" >&2
		exit 1
	fi
}

trap cleanup EXIT INT TERM
unload_module

load_module
configure 5
"$workload" burst 6 >"$temporary_directory/max5"
assert_between "$(elapsed_from "$temporary_directory/max5")" \
	850000000 2000000000 'MAX=5 rolling window'
unload_module

load_module
configure 3
"$workload" burst 7 >"$temporary_directory/max3"
assert_between "$(elapsed_from "$temporary_directory/max3")" \
	1800000000 3200000000 'MAX=3 rolling window'
unload_module

load_module
configure 1 not_selected
"$workload" burst 10000 >"$temporary_directory/nonmatching"
assert_between "$(elapsed_from "$temporary_directory/nonmatching")" \
	0 500000000 'nonmatching pass-through'
unload_module

load_module
configure 4
"$workload" burst 4 >"$temporary_directory/global-a" &
first=$!
"$workload" burst 4 >"$temporary_directory/global-b" &
second=$!
wait "$first"
wait "$second"
first_elapsed=$(elapsed_from "$temporary_directory/global-a")
second_elapsed=$(elapsed_from "$temporary_directory/global-b")
if [ "$first_elapsed" -gt "$second_elapsed" ]; then
	global_elapsed=$first_elapsed
else
	global_elapsed=$second_elapsed
fi
assert_between "$global_elapsed" 850000000 2000000000 \
	'global aggregate limit'
unload_module

load_module
configure 1
"$workload" burst 2 >"$temporary_directory/disable" &
waiter=$!
sleep 0.2
state=$(ps -o stat= -p "$waiter" | tr -d ' ')
case "$state" in
	S*) ;;
	*) printf 'throttled task was not sleeping: %s\n' "$state" >&2; exit 1 ;;
esac
"$control" disable
wait "$waiter"
assert_between "$(elapsed_from "$temporary_directory/disable")" \
	100000000 800000000 'disable wake-up'
unload_module

load_module
configure 1
"$workload" burst 2 >"$temporary_directory/deregister" &
waiter=$!
sleep 0.2
"$control" remove-program test_throttle
wait "$waiter"
assert_between "$(elapsed_from "$temporary_directory/deregister")" \
	100000000 800000000 'deregistration wake-up'
unload_module

load_module
configure 1
"$workload" burst 2 >"$temporary_directory/increase" &
waiter=$!
sleep 0.2
"$control" set-max 2
wait "$waiter"
assert_between "$(elapsed_from "$temporary_directory/increase")" \
	100000000 800000000 'MAX increase wake-up'
unload_module

load_module
configure 3
"$workload" burst 5 >"$temporary_directory/decrease" &
waiter=$!
sleep 0.2
"$control" set-max 1
wait "$waiter"
assert_between "$(elapsed_from "$temporary_directory/decrease")" \
	1800000000 3200000000 'MAX decrease enforcement'
unload_module

load_module
configure 1
"$workload" signal-wait >"$temporary_directory/signal" &
waiter=$!
attempt=0
while ! grep -q '^ready$' "$temporary_directory/signal"; do
	attempt=$((attempt + 1))
	if [ "$attempt" -gt 50 ]; then
		printf 'signal test did not enter its wait\n' >&2
		exit 1
	fi
	sleep 0.02
done
kill -USR1 "$waiter"
wait "$waiter"
grep -q 'errno=4 signal=1' "$temporary_directory/signal"
unload_module

load_module
configure 1
"$workload" burst 2 >"$temporary_directory/unload" 2>&1 &
waiter=$!
sleep 0.2
unload_module
if wait "$waiter"; then
	printf 'unload test unexpectedly completed without interrupting the call\n' >&2
	exit 1
fi

trap - EXIT INT TERM
rm -rf -- "$temporary_directory"
printf 'Throttling runtime verification passed.\n'
