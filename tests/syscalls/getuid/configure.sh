#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
	printf 'Run this configuration with sudo.\n' >&2
	exit 1
fi

project_directory=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)

exec "$project_directory/user/throttle_ctl" configure --clear \
	--program st_getuid --syscall getuid --max "${1:-3}" \
	--reset-stats --enable
