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
		rmmod syscall_throttle
	fi
}

trap unload_module EXIT INT TERM

if [ -d "$module_directory" ]; then
	printf 'The syscall_throttle module is already loaded.\n' >&2
	exit 1
fi

"$control" -h | grep -q 'configure \[OPTIONS\]'
"$control" configure -h | grep -q -- '--syscall NAME|NUMBER'
"$control" help show | grep -q -- '--raw'

insmod "$module_path"

if "$control" configure --enable --disable >/dev/null 2>&1; then
	printf 'Conflicting monitor states unexpectedly succeeded.\n' >&2
	exit 1
fi
if "$control" configure --syscall definitely_not_a_syscall \
	>/dev/null 2>&1; then
	printf 'Unknown symbolic syscall unexpectedly succeeded.\n' >&2
	exit 1
fi

"$control" add-program retained
"$control" configure --program batch_added --program retained \
	--uid 1000 --syscall getpid --max 3 --enable >/dev/null
configuration=$("$control" show --raw)
printf '%s\n' "$configuration" | grep -q '^monitor=on$'
printf '%s\n' "$configuration" | grep -q '^max_per_second=3$'
printf '%s\n' "$configuration" | grep -q '^program=retained$'
printf '%s\n' "$configuration" | grep -q '^program=batch_added$'
printf '%s\n' "$configuration" | grep -q '^uid=1000$'
printf '%s\n' "$configuration" | grep -q '^syscall=39$'
printf '%s\n' "$configuration" | grep -q '^peak_delay_ns='

"$control" configure --clear --program replaced --syscall read \
	--max 9 --disable >/dev/null
configuration=$("$control" show --raw)
printf '%s\n' "$configuration" | grep -q '^monitor=off$'
printf '%s\n' "$configuration" | grep -q '^max_per_second=9$'
printf '%s\n' "$configuration" | grep -q '^program=replaced$'
printf '%s\n' "$configuration" | grep -q '^syscall=0$'
if printf '%s\n' "$configuration" | grep -q '^program=retained$'; then
	printf 'The --clear configuration retained an old entry.\n' >&2
	exit 1
fi

human=$("$control" show)
printf '%s\n' "$human" | grep -q 'read (0)'
printf '%s\n' "$human" | grep -q "Use 'show --raw'"

if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != root ]; then
	if runuser -u "$SUDO_USER" -- "$control" clear >/dev/null 2>&1; then
		printf 'Unprivileged clear unexpectedly succeeded.\n' >&2
		exit 1
	fi
	if runuser -u "$SUDO_USER" -- "$control" configure --max 2 \
		>/dev/null 2>&1; then
		printf 'Unprivileged configure unexpectedly succeeded.\n' >&2
		exit 1
	fi
	runuser -u "$SUDO_USER" -- "$control" show >/dev/null
fi

"$control" clear >/dev/null
configuration=$("$control" show --raw)
printf '%s\n' "$configuration" | grep -q '^monitor=off$'
printf '%s\n' "$configuration" | grep -q '^max_per_second=1$'
if printf '%s\n' "$configuration" | grep -Eq '^(program|uid|syscall)='; then
	printf 'Clear left a registry entry behind.\n' >&2
	exit 1
fi

unload_module
trap - EXIT INT TERM
printf 'Control interface verification passed.\n'
