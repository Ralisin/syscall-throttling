#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
	printf 'Run this test as root.\n' >&2
	exit 1
fi

project_directory=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
module_path=$project_directory/kernel/syscall_throttle.ko
control_path=$project_directory/user/throttle_ctl
test_path=$project_directory/tests/test_registry
module_directory=/sys/module/syscall_throttle

unload_module()
{
	if [ -d "$module_directory" ]; then
		rmmod syscall_throttle
	fi
}

trap unload_module EXIT INT TERM

if [ -d "$module_directory" ]; then
	printf 'The syscall_throttle module is already loaded.\n' >&2
	exit 1
fi

insmod "$module_path"

if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != root ]; then
	if runuser -u "$SUDO_USER" -- "$control_path" add-program denied; then
		printf 'Unprivileged registry update unexpectedly succeeded.\n' >&2
		exit 1
	fi
	runuser -u "$SUDO_USER" -- "$control_path" list >/dev/null
fi

"$test_path"

(
	iteration=1
	while [ "$iteration" -le 50 ]; do
		"$control_path" add-program cli_race
		"$control_path" remove-program cli_race
		iteration=$((iteration + 1))
	done
) &
writer_pid=$!
while kill -0 "$writer_pid" 2>/dev/null; do
	"$control_path" list >/dev/null
done
wait "$writer_pid"

"$control_path" add-program cli_probe
"$control_path" add-uid 1000
"$control_path" add-syscall 39
configuration=$("$control_path" list)
printf '%s\n' "$configuration" | grep -q '  cli_probe$'
printf '%s\n' "$configuration" | grep -q '  1000$'
printf '%s\n' "$configuration" | grep -q '  39$'
"$control_path" remove-program cli_probe
"$control_path" remove-uid 1000
"$control_path" remove-syscall 39

unload_module
trap - EXIT INT TERM
printf 'Registry runtime verification passed.\n'
