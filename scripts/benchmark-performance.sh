#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
	printf 'Eseguire il benchmark come root.\n' >&2
	exit 1
fi

project_directory=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
module_path=$project_directory/kernel/syscall_throttle.ko
control=$project_directory/user/throttle_ctl
benchmark=$project_directory/tests/perf_probe
module_directory=/sys/module/syscall_throttle
samples=${SAMPLES:-100000}
repetitions=${REPETITIONS:-5}
allowed_cpus=$(sed -n 's/^Cpus_allowed_list:[[:space:]]*//p' /proc/self/status)
first_cpu=${allowed_cpus%%[-,]*}
cpu=${BENCHMARK_CPU:-$first_cpu}
module_loaded=0

case $samples in
	''|*[!0-9]*) printf 'SAMPLES deve essere un intero positivo.\n' >&2; exit 1 ;;
esac
case $repetitions in
	''|*[!0-9]*) printf 'REPETITIONS deve essere un intero positivo.\n' >&2; exit 1 ;;
esac
if [ "$samples" -lt 1 ] || [ "$samples" -gt 400000 ]; then
	printf 'SAMPLES deve essere compreso tra 1 e 400000.\n' >&2
	exit 1
fi
if [ "$repetitions" -lt 1 ]; then
	printf 'REPETITIONS deve essere maggiore di zero.\n' >&2
	exit 1
fi
if [ -d "$module_directory" ]; then
	printf 'Il modulo syscall_throttle deve essere assente prima del benchmark.\n' >&2
	exit 1
fi
if [ ! -x "$control" ] || [ ! -x "$benchmark" ] || [ ! -f "$module_path" ]; then
	printf 'Compilare il progetto con make prima del benchmark.\n' >&2
	exit 1
fi

cleanup()
{
	if [ "$module_loaded" -eq 1 ] && [ -d "$module_directory" ]; then
		rmmod syscall_throttle
	fi
}

load_module()
{
	insmod "$module_path"
	module_loaded=1
}

unload_module()
{
	rmmod syscall_throttle
	module_loaded=0
}

run_repetitions()
{
	scenario=$1
	reset_window=${2:-0}
	repetition=1

	while [ "$repetition" -le "$repetitions" ]; do
		if [ "$reset_window" -eq 1 ]; then
			"$control" disable >/dev/null
			"$control" enable >/dev/null
		fi
		benchmark_output=$("$benchmark" "$samples" "$cpu")
		printf '%s\n' "$benchmark_output" |
			awk -F, -v scenario="$scenario" -v run="$repetition" \
				'NR > 1 { print scenario "," run "," $0 }'
		repetition=$((repetition + 1))
	done
}

trap cleanup EXIT INT TERM

printf '# date=%s\n' "$(date --iso-8601=seconds)"
printf '# kernel=%s\n' "$(uname -r)"
printf '# machine=%s\n' "$(uname -m)"
printf '# cpu_model=%s\n' "$(sed -n 's/^model name[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo | sed -n '1p')"
printf '# samples=%s repetitions=%s cpu=%s\n' "$samples" "$repetitions" "$cpu"
printf 'scenario,run,syscall,cpu,samples,min_ns,median_ns,mean_ns,p95_ns,p99_ns,max_ns,stddev_ns\n'

run_repetitions module_absent

load_module
run_repetitions module_loaded_disabled
unload_module

load_module
"$control" configure --clear --program not_selected \
	--syscall getpid --syscall read --max 1000000 --enable >/dev/null
run_repetitions enabled_no_match
unload_module

load_module
"$control" configure --clear --program perf_probe \
	--syscall getpid --syscall read --max 1000000 --enable >/dev/null
run_repetitions selected_no_throttling 1
unload_module

trap - EXIT INT TERM
