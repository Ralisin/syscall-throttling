#!/bin/sh
set -eu

kernel_release=$(uname -r)
config=/boot/config-$kernel_release

printf 'kernel: %s\n' "$kernel_release"
grep -E '^CONFIG_(KPROBES|FTRACE|FUNCTION_TRACER)=' "$config" || true

printf '\nsymbols:\n'
grep -E ' (x64_sys_call|__x64_sys_getpid)$' /proc/kallsyms || true

if [ -r /sys/kernel/tracing/available_filter_functions ]; then
	printf '\nftrace:\n'
	grep -E '(^| )(__x64_sys_getpid|x64_sys_call)$' \
		/sys/kernel/tracing/available_filter_functions || true
fi

