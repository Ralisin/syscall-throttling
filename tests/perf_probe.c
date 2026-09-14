// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_SAMPLES 100000UL
#define MAX_SAMPLES 400000UL
#define WARMUP_CALLS 10000UL

typedef int (*operation_function)(void *context);

static uint64_t elapsed_nanoseconds(const struct timespec *start,
				    const struct timespec *end) {
	int64_t seconds = (int64_t)end->tv_sec - (int64_t)start->tv_sec;
	int64_t nanoseconds = (int64_t)end->tv_nsec - (int64_t)start->tv_nsec;

	return (uint64_t)(seconds * 1000000000LL + nanoseconds);
}

static int compare_u64(const void *left, const void *right) {
	uint64_t first = *(const uint64_t *)left;
	uint64_t second = *(const uint64_t *)right;

	return (first > second) - (first < second);
}

static int parse_unsigned(const char *text, unsigned long *value) {
	char *end;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno || *text == '\0' || *end != '\0')
		return -1;
	*value = parsed;
	return 0;
}

static int run_getpid(void *context) {
	(void)context;
	return syscall(SYS_getpid) < 0 ? -1 : 0;
}

static int run_read(void *context) {
	int descriptor = *(const int *)context;
	char byte;

	return syscall(SYS_read, descriptor, &byte, sizeof(byte)) == sizeof(byte) ?
		0 : -1;
}

static int pin_to_cpu(unsigned int cpu) {
	cpu_set_t affinity;

	if (cpu >= CPU_SETSIZE) {
		errno = EINVAL;
		return -1;
	}
	CPU_ZERO(&affinity);
	CPU_SET(cpu, &affinity);
	return sched_setaffinity(0, sizeof(affinity), &affinity);
}

static int measure(const char *name, operation_function operation,
		   void *context, unsigned long sample_count, unsigned int cpu) {
	long double sum = 0.0L;
	long double squared_error_sum = 0.0L;
	long double mean;
	long double median;
	long double standard_deviation;
	struct timespec start;
	struct timespec end;
	uint64_t *samples;
	unsigned long index;

	for (index = 0; index < WARMUP_CALLS; index++) {
		if (operation(context))
			return -1;
	}

	samples = malloc(sample_count * sizeof(*samples));
	if (!samples)
		return -1;

	for (index = 0; index < sample_count; index++) {
		if (clock_gettime(CLOCK_MONOTONIC_RAW, &start) ||
		    operation(context) ||
		    clock_gettime(CLOCK_MONOTONIC_RAW, &end)) {
			free(samples);
			return -1;
		}
		samples[index] = elapsed_nanoseconds(&start, &end);
		sum += samples[index];
	}

	mean = sum / sample_count;
	for (index = 0; index < sample_count; index++) {
		long double difference = samples[index] - mean;

		squared_error_sum += difference * difference;
	}
	standard_deviation = sqrtl(squared_error_sum / sample_count);
	qsort(samples, sample_count, sizeof(*samples), compare_u64);
	if (sample_count % 2)
		median = samples[sample_count / 2];
	else
		median = ((long double)samples[sample_count / 2 - 1] +
			  samples[sample_count / 2]) / 2.0L;

	printf("%s,%u,%lu,%llu,%.2Lf,%.2Lf,%llu,%llu,%llu,%.2Lf\n",
	       name, cpu, sample_count,
	       (unsigned long long)samples[0], median, mean,
	       (unsigned long long)samples[(sample_count - 1) * 95 / 100],
	       (unsigned long long)samples[(sample_count - 1) * 99 / 100],
	       (unsigned long long)samples[sample_count - 1],
	       standard_deviation);
	free(samples);
	return 0;
}

int main(int argc, char **argv) {
	unsigned long sample_count = DEFAULT_SAMPLES;
	unsigned long requested_cpu;
	int current_cpu;
	int zero_descriptor;

	if (argc > 3 ||
	    (argc >= 2 && (parse_unsigned(argv[1], &sample_count) ||
			   sample_count == 0 || sample_count > MAX_SAMPLES))) {
		fprintf(stderr, "uso: %s [CAMPIONI fino a %lu] [CPU]\n",
			argv[0], MAX_SAMPLES);
		return EXIT_FAILURE;
	}

	current_cpu = sched_getcpu();
	if (current_cpu < 0) {
		perror("sched_getcpu");
		return EXIT_FAILURE;
	}
	requested_cpu = (unsigned long)current_cpu;
	if (argc == 3 && parse_unsigned(argv[2], &requested_cpu)) {
		fprintf(stderr, "CPU non valida: %s\n", argv[2]);
		return EXIT_FAILURE;
	}
	if (pin_to_cpu((unsigned int)requested_cpu)) {
		perror("sched_setaffinity");
		return EXIT_FAILURE;
	}

	zero_descriptor = open("/dev/zero", O_RDONLY | O_CLOEXEC);
	if (zero_descriptor == -1) {
		perror("/dev/zero");
		return EXIT_FAILURE;
	}

	puts("syscall,cpu,samples,min_ns,median_ns,mean_ns,p95_ns,p99_ns,max_ns,stddev_ns");
	if (measure("getpid", run_getpid, NULL, sample_count,
		    (unsigned int)requested_cpu) ||
	    measure("read", run_read, &zero_descriptor, sample_count,
		    (unsigned int)requested_cpu)) {
		perror("benchmark");
		close(zero_descriptor);
		return EXIT_FAILURE;
	}

	close(zero_descriptor);
	return EXIT_SUCCESS;
}
