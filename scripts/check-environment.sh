#!/bin/sh
set -eu

kernel_release=$(uname -r)
build_directory=/lib/modules/$kernel_release/build

printf 'Kernel: %s\n' "$kernel_release"

if [ ! -d "$build_directory" ]; then
	printf 'Missing kernel build directory: %s\n' "$build_directory" >&2
	exit 1
fi

for option in CONFIG_MODULES CONFIG_FTRACE CONFIG_DYNAMIC_FTRACE CONFIG_KPROBES CONFIG_KALLSYMS; do
	if ! grep -q "^$option=y" "/boot/config-$kernel_release"; then
		printf 'Required kernel option is not enabled: %s\n' "$option" >&2
		exit 1
	fi
done

printf 'Kernel build environment is available.\n'
