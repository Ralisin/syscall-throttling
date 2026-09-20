// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "syscall_throttle.h"
#include "syscall_names.h"

#define ST_LIST_RETRIES 64

struct st_configuration_snapshot {
	struct st_config config;
	char programs[ST_MAX_PROGRAMS][ST_PROGRAM_PATH_LEN];
	__u32 uids[ST_MAX_UIDS];
	__s32 syscalls[ST_MAX_SYSCALLS];
};

struct st_configuration_request {
	bool clear;
	bool reset_stats;
	bool set_max;
	bool state_selected;
	bool enabled;
	__u32 max_per_second;
	__u32 program_count;
	__u32 uid_count;
	__u32 syscall_count;
	struct st_program programs[ST_MAX_PROGRAMS];
	struct st_uid uids[ST_MAX_UIDS];
	struct st_syscall syscalls[ST_MAX_SYSCALLS];
};

static void print_usage(FILE *stream, const char *program) {
	fprintf(stream,
		"Uso: %s COMANDO [OPZIONI]\n\n"
		"Comandi di configurazione:\n"
		"  configure [OPTIONS]       Disabilita, modifica e ripristina il monitor\n"
		"  clear                     Svuota e disabilita la configurazione\n"
		"  enable | disable          Attiva o disattiva il monitor\n"
		"  set-max NUMBER            Imposta il limite per secondo\n"
		"  add-program PATH          Registra il path di un eseguibile\n"
		"  remove-program PATH       Rimuove il path di un eseguibile\n"
		"  add-uid UID               Registra un EUID\n"
		"  remove-uid UID            Rimuove un EUID\n"
		"  add-syscall NAME|NUMBER   Registra una system call\n"
		"  remove-syscall NAME|NUMBER\n"
		"                            Rimuove una system call\n\n"
		"Comandi di lettura:\n"
		"  show [--raw]              Mostra configurazione e statistiche\n"
		"  status | list | stats     Mostra le singole sezioni\n"
		"  reset-stats               Azzera le statistiche\n\n"
		"Opzioni generali:\n"
		"  -h, --help                Mostra questo aiuto\n"
		"Per un comando: '%s help COMANDO' oppure '%s COMANDO --help'.\n",
		program, program, program);
}

static void print_configure_help(FILE *stream, const char *program) {
	fprintf(stream,
		"Uso: %s configure [OPTIONS]\n\n"
		"Il monitor viene disabilitato durante le modifiche. Alla fine viene\n"
		"applicato --enable/--disable o ripristinato lo stato precedente.\n"
		"Senza --clear, gli elementi gia' registrati\n"
		"restano presenti e i duplicati vengono ignorati. Le operazioni non\n"
		"sono atomiche: un errore puo' lasciare modifiche parziali e il monitor\n"
		"disabilitato.\n\n"
		"Opzioni:\n"
		"  --clear                   Parte da una configurazione vuota\n"
		"  --program PATH            Registra un eseguibile (ripetibile)\n"
		"  --uid UID                 Registra un EUID (ripetibile)\n"
		"  --syscall NAME|NUMBER     Registra una syscall (ripetibile)\n"
		"  --max NUMBER              Imposta il limite per secondo\n"
		"  --enable | --disable      Imposta lo stato del monitor\n"
		"  --reset-stats             Azzera le statistiche\n"
		"  -h, --help                Mostra questo aiuto\n\n"
		"Esempio:\n"
		"  sudo %s configure --clear --program /opt/test_throttle \\\n"
		"      --syscall getpid --max 2 --enable\n",
		program, program);
}

static void print_show_help(FILE *stream, const char *program) {
	fprintf(stream,
		"Uso: %s show [--raw]\n\n"
		"Mostra configurazione e statistiche. Con --raw stampa record\n"
		"key=value adatti agli script.\n",
		program);
}

static void print_clear_help(FILE *stream, const char *program) {
	fprintf(stream,
		"Uso: sudo %s clear\n\n"
		"Disabilita il monitor, imposta MAX a 1, svuota i registri e azzera\n"
		"la finestra corrente e le statistiche.\n",
		program);
}

static bool is_help(const char *text) {
	return !strcmp(text, "-h") || !strcmp(text, "--help");
}

static int print_command_help(const char *program, const char *command) {
	if (!strcmp(command, "configure"))
		print_configure_help(stdout, program);
	else if (!strcmp(command, "show"))
		print_show_help(stdout, program);
	else if (!strcmp(command, "clear"))
		print_clear_help(stdout, program);
	else {
		fprintf(stderr, "comando sconosciuto: %s\n", command);
		return -1;
	}
	return 0;
}

static int parse_u32(const char *text, __u32 *value) {
	char *end;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno || !*text || *end || parsed > UINT32_MAX)
		return -1;
	*value = (__u32)parsed;
	return 0;
}

static int prepare_program(const char *path, struct st_program *program) {
	size_t length = strlen(path);

	if (!length || path[0] != '/' || length >= ST_PROGRAM_PATH_LEN)
		return -1;
	memset(program, 0, sizeof(*program));
	memcpy(program->path, path, length);
	return 0;
}

static int get_config(int descriptor, struct st_config *config) {
	return ioctl(descriptor, ST_IOC_GET_CONFIG, config);
}

static void print_status(const struct st_config *config) {
	printf("monitor: %s\n", config->enabled ? "attivo" : "disattivo");
	printf("limite: %" PRIu32 "\n", config->max_per_second);
	printf("programmi: %" PRIu32 "\n", config->program_count);
	printf("UID: %" PRIu32 "\n", config->uid_count);
	printf("syscall: %" PRIu32 "\n", config->syscall_count);
	printf("generazione: %" PRIu64 "\n", (uint64_t)config->generation);
}

static double average_blocked(const struct st_stats *stats) {
	if (!stats->elapsed_ns)
		return 0.0;
	return (double)stats->blocked_thread_time_ns /
	       (double)stats->elapsed_ns;
}

static void print_stats(const struct st_stats *stats) {
	printf("interval_ns: %" PRIu64 "\n", (uint64_t)stats->elapsed_ns);
	printf("blocked_thread_time_ns: %" PRIu64 "\n",
	       (uint64_t)stats->blocked_thread_time_ns);
	printf("average_blocked_threads: %.6f\n", average_blocked(stats));
	printf("current_blocked_threads: %" PRIu32 "\n",
	       stats->current_blocked_threads);
	printf("peak_blocked_threads: %" PRIu32 "\n",
	       stats->peak_blocked_threads);
	printf("throttled_calls: %" PRIu64 "\n",
	       (uint64_t)stats->throttled_calls);
	printf("peak_delay_ns: %" PRIu64 "\n", (uint64_t)stats->peak_delay_ns);
	printf("peak_program_path: %s\n", stats->peak_program_path);
	printf("peak_uid: %" PRIu32 "\n", stats->peak_uid);
}

static void print_stats_raw(const struct st_stats *stats) {
	printf("interval_ns=%" PRIu64 "\n", (uint64_t)stats->elapsed_ns);
	printf("blocked_thread_time_ns=%" PRIu64 "\n",
	       (uint64_t)stats->blocked_thread_time_ns);
	printf("average_blocked_threads=%.6f\n", average_blocked(stats));
	printf("current_blocked_threads=%" PRIu32 "\n",
	       stats->current_blocked_threads);
	printf("peak_blocked_threads=%" PRIu32 "\n",
	       stats->peak_blocked_threads);
	printf("throttled_calls=%" PRIu64 "\n",
	       (uint64_t)stats->throttled_calls);
	printf("peak_delay_ns=%" PRIu64 "\n", (uint64_t)stats->peak_delay_ns);
	printf("peak_program_path=%s\n", stats->peak_program_path);
	printf("peak_uid=%" PRIu32 "\n", stats->peak_uid);
}

static int read_snapshot(int descriptor, struct st_configuration_snapshot *snapshot) {
	struct st_config final_config;
	unsigned int attempt;
	__u32 index;

	for (attempt = 0; attempt < ST_LIST_RETRIES; attempt++) {
		if (get_config(descriptor, &snapshot->config) == -1)
			return -1;
		if (snapshot->config.program_count > ST_MAX_PROGRAMS ||
		    snapshot->config.uid_count > ST_MAX_UIDS ||
		    snapshot->config.syscall_count > ST_MAX_SYSCALLS) {
			errno = EPROTO;
			return -1;
		}
		for (index = 0; index < snapshot->config.program_count; index++) {
			struct st_program_entry entry = {
				.generation = snapshot->config.generation,
				.index = index,
			};

			if (ioctl(descriptor, ST_IOC_GET_PROGRAM, &entry) == -1)
				goto retry;
			memcpy(snapshot->programs[index], entry.path,
			       ST_PROGRAM_PATH_LEN);
		}
		for (index = 0; index < snapshot->config.uid_count; index++) {
			struct st_uid_entry entry = {
				.generation = snapshot->config.generation,
				.index = index,
			};

			if (ioctl(descriptor, ST_IOC_GET_UID, &entry) == -1)
				goto retry;
			snapshot->uids[index] = entry.value;
		}
		for (index = 0; index < snapshot->config.syscall_count; index++) {
			struct st_syscall_entry entry = {
				.generation = snapshot->config.generation,
				.index = index,
			};

			if (ioctl(descriptor, ST_IOC_GET_SYSCALL, &entry) == -1)
				goto retry;
			snapshot->syscalls[index] = entry.number;
		}
		if (get_config(descriptor, &final_config) == -1)
			return -1;
		if (final_config.generation == snapshot->config.generation)
			return 0;
		errno = EAGAIN;
		continue;
retry:
		if (errno != EAGAIN)
			return -1;
	}
	errno = EAGAIN;
	return -1;
}

static void print_syscall(__s32 number, const char *prefix) {
	char name[128];

	if (!st_lookup_syscall_name(number, name, sizeof(name)))
		printf("%s%s (%" PRId32 ")\n", prefix, name, number);
	else
		printf("%s%" PRId32 "\n", prefix, number);
}

static int list_configuration(int descriptor) {
	struct st_configuration_snapshot snapshot = { 0 };
	__u32 index;

	if (read_snapshot(descriptor, &snapshot) == -1)
		return -1;
	print_status(&snapshot.config);
	puts("programmi registrati:");
	for (index = 0; index < snapshot.config.program_count; index++)
		printf("  %s\n", snapshot.programs[index]);
	puts("UID registrati:");
	for (index = 0; index < snapshot.config.uid_count; index++)
		printf("  %" PRIu32 "\n", snapshot.uids[index]);
	puts("syscall registrate:");
	for (index = 0; index < snapshot.config.syscall_count; index++)
		print_syscall(snapshot.syscalls[index], "  ");
	return 0;
}

static int show_configuration(int descriptor, bool raw) {
	struct st_configuration_snapshot snapshot = { 0 };
	struct st_stats stats;
	__u32 index;

	if (read_snapshot(descriptor, &snapshot) == -1 ||
	    ioctl(descriptor, ST_IOC_GET_STATS, &stats) == -1)
		return -1;
	if (raw) {
		printf("monitor=%s\n", snapshot.config.enabled ? "on" : "off");
		printf("max_per_second=%" PRIu32 "\n",
		       snapshot.config.max_per_second);
		printf("generation=%" PRIu64 "\n",
		       (uint64_t)snapshot.config.generation);
		for (index = 0; index < snapshot.config.program_count; index++)
			printf("program=%s\n", snapshot.programs[index]);
		for (index = 0; index < snapshot.config.uid_count; index++)
			printf("uid=%" PRIu32 "\n", snapshot.uids[index]);
		for (index = 0; index < snapshot.config.syscall_count; index++)
			printf("syscall=%" PRId32 "\n", snapshot.syscalls[index]);
		print_stats_raw(&stats);
		return 0;
	}

	printf("Monitor: %s\n", snapshot.config.enabled ? "attivo" : "disattivo");
	printf("Limite globale: %" PRIu32 " chiamate al secondo\n",
	       snapshot.config.max_per_second);
	printf("Generazione configurazione: %" PRIu64 "\n\n",
	       (uint64_t)snapshot.config.generation);
	puts("Programmi registrati:");
	if (!snapshot.config.program_count)
		puts("  (nessuno)");
	for (index = 0; index < snapshot.config.program_count; index++)
		printf("  %s\n", snapshot.programs[index]);
	puts("EUID registrati:");
	if (!snapshot.config.uid_count)
		puts("  (nessuno)");
	for (index = 0; index < snapshot.config.uid_count; index++)
		printf("  %" PRIu32 "\n", snapshot.uids[index]);
	puts("Syscall registrate:");
	if (!snapshot.config.syscall_count)
		puts("  (nessuna)");
	for (index = 0; index < snapshot.config.syscall_count; index++)
		print_syscall(snapshot.syscalls[index], "  ");
	puts("\nStatistiche:");
	printf("  Tempo di osservazione: %.3f s\n", (double)stats.elapsed_ns / 1e9);
	printf("  Media thread bloccati: %.3f\n", average_blocked(&stats));
	printf("  Thread bloccati ora: %" PRIu32 "\n",
	       stats.current_blocked_threads);
	printf("  Picco thread bloccati: %" PRIu32 "\n",
	       stats.peak_blocked_threads);
	printf("  Chiamate rallentate: %" PRIu64 "\n",
	       (uint64_t)stats.throttled_calls);
	printf("  Ritardo massimo: %.3f ms\n", (double)stats.peak_delay_ns / 1e6);
	if (stats.peak_delay_ns)
		printf("  Processo del ritardo massimo: %s (EUID %" PRIu32 ")\n",
		       stats.peak_program_path, stats.peak_uid);
	else
		puts("  Processo del ritardo massimo: (nessuno)");
	puts("\nUsare 'show --raw' per i valori numerici non formattati.");
	return 0;
}

static int next_option_value(int argc, char **argv, int *index, const char **value) {
	if (*index + 1 >= argc)
		return -1;
	*value = argv[++*index];
	return 0;
}

static int prepare_configuration(int argc, char **argv,
				 struct st_configuration_request *request) {
	int index;

	memset(request, 0, sizeof(*request));
	for (index = 2; index < argc; index++) {
		const char *value;

		if (!strcmp(argv[index], "--clear")) {
			request->clear = true;
		} else if (!strcmp(argv[index], "--reset-stats")) {
			request->reset_stats = true;
		} else if (!strcmp(argv[index], "--enable") ||
			   !strcmp(argv[index], "--disable")) {
			bool enabled = !strcmp(argv[index], "--enable");

			if (request->state_selected && request->enabled != enabled) {
				fprintf(stderr, "--enable e --disable non possono essere usati insieme\n");
				return -1;
			}
			request->state_selected = true;
			request->enabled = enabled;
		} else if (!strcmp(argv[index], "--max")) {
			if (next_option_value(argc, argv, &index, &value) ||
			    parse_u32(value, &request->max_per_second) ||
			    !request->max_per_second ||
			    request->max_per_second > ST_MAX_LIMIT) {
				fprintf(stderr, "valore non valido per --max\n");
				return -1;
			}
			request->set_max = true;
		} else if (!strcmp(argv[index], "--program")) {
			if (request->program_count == ST_MAX_PROGRAMS ||
			    next_option_value(argc, argv, &index, &value) ||
			    prepare_program(value,
				&request->programs[request->program_count])) {
				fprintf(stderr, "valore --program non valido o troppi programmi\n");
				return -1;
			}
			request->program_count++;
		} else if (!strcmp(argv[index], "--uid")) {
			if (request->uid_count == ST_MAX_UIDS ||
			    next_option_value(argc, argv, &index, &value) ||
			    parse_u32(value,
				      &request->uids[request->uid_count].value)) {
				fprintf(stderr, "valore --uid non valido o troppi UID\n");
				return -1;
			}
			request->uid_count++;
		} else if (!strcmp(argv[index], "--syscall")) {
			if (request->syscall_count == ST_MAX_SYSCALLS ||
			    next_option_value(argc, argv, &index, &value) ||
			    st_resolve_syscall(value,
				&request->syscalls[request->syscall_count].number)) {
				fprintf(stderr, "valore --syscall non valido o sconosciuto\n");
				return -1;
			}
			request->syscall_count++;
		} else {
			fprintf(stderr, "opzione configure sconosciuta: %s\n", argv[index]);
			return -1;
		}
	}
	if (!request->clear && !request->reset_stats && !request->set_max &&
	    !request->state_selected && !request->program_count &&
	    !request->uid_count && !request->syscall_count) {
		fprintf(stderr, "configure richiede almeno un'opzione\n");
		return -1;
	}
	return 0;
}

static int remove_snapshot(int descriptor,
			   const struct st_configuration_snapshot *snapshot) {
	struct st_program program;
	struct st_syscall syscall;
	struct st_uid uid;
	__u32 index;

	for (index = 0; index < snapshot->config.program_count; index++) {
		memset(&program, 0, sizeof(program));
		memcpy(program.path, snapshot->programs[index],
		       ST_PROGRAM_PATH_LEN);
		if (ioctl(descriptor, ST_IOC_REMOVE_PROGRAM, &program) == -1 &&
		    errno != ENOENT)
			return -1;
	}
	for (index = 0; index < snapshot->config.uid_count; index++) {
		uid.value = snapshot->uids[index];
		if (ioctl(descriptor, ST_IOC_REMOVE_UID, &uid) == -1 &&
		    errno != ENOENT)
			return -1;
	}
	for (index = 0; index < snapshot->config.syscall_count; index++) {
		syscall.number = snapshot->syscalls[index];
		if (ioctl(descriptor, ST_IOC_REMOVE_SYSCALL, &syscall) == -1 &&
		    errno != ENOENT)
			return -1;
	}
	return 0;
}

static int add_ignoring_duplicate(int descriptor, unsigned long command,
				  const void *value) {
	if (ioctl(descriptor, command, value) == 0 || errno == EEXIST)
		return 0;
	return -1;
}

static int apply_configuration(int descriptor,
			       const struct st_configuration_request *request) {
	struct st_configuration_snapshot snapshot = { 0 };
	bool final_enabled;
	__u32 default_max = 1;
	__u32 index;

	if (read_snapshot(descriptor, &snapshot) == -1 ||
	    ioctl(descriptor, ST_IOC_DISABLE) == -1)
		return -1;

	if (request->clear &&
	    (remove_snapshot(descriptor, &snapshot) == -1 ||
	     ioctl(descriptor, ST_IOC_SET_MAX, &default_max) == -1 ||
	     ioctl(descriptor, ST_IOC_RESET_STATS) == -1))
		return -1;

	for (index = 0; index < request->program_count; index++) {
		if (add_ignoring_duplicate(descriptor, ST_IOC_ADD_PROGRAM,
					   &request->programs[index]))
			return -1;
	}
	for (index = 0; index < request->uid_count; index++) {
		if (add_ignoring_duplicate(descriptor, ST_IOC_ADD_UID,
					   &request->uids[index]))
			return -1;
	}
	for (index = 0; index < request->syscall_count; index++) {
		if (add_ignoring_duplicate(descriptor, ST_IOC_ADD_SYSCALL,
					   &request->syscalls[index]))
			return -1;
	}
	if (request->set_max &&
	    ioctl(descriptor, ST_IOC_SET_MAX, &request->max_per_second) == -1)
		return -1;
	if (request->reset_stats && !request->clear &&
	    ioctl(descriptor, ST_IOC_RESET_STATS) == -1)
		return -1;

	final_enabled = request->state_selected ? request->enabled :
			request->clear ? false : snapshot.config.enabled;
	if (final_enabled && ioctl(descriptor, ST_IOC_ENABLE) == -1)
		return -1;
	return 0;
}

static int handle_configuration(int descriptor, int argc, char **argv) {
	struct st_configuration_request *request;
	int result;

	request = calloc(1, sizeof(*request));
	if (!request) {
		errno = ENOMEM;
		return -1;
	}
	if (prepare_configuration(argc, argv, request)) {
		free(request);
		errno = EINVAL;
		return -1;
	}
	result = apply_configuration(descriptor, request);
	free(request);
	if (result == -1)
		return -1;
	puts("Configurazione applicata.");
	return show_configuration(descriptor, false);
}

int main(int argc, char **argv) {
	int descriptor;
	int result = -1;

	if (argc == 2 && (is_help(argv[1]) || !strcmp(argv[1], "help"))) {
		print_usage(stdout, argv[0]);
		return EXIT_SUCCESS;
	}
	if (argc == 3 && !strcmp(argv[1], "help"))
		return print_command_help(argv[0], argv[2]) ?
		       EXIT_FAILURE : EXIT_SUCCESS;
	if (argc == 3 && is_help(argv[2]))
		return print_command_help(argv[0], argv[1]) ?
		       EXIT_FAILURE : EXIT_SUCCESS;
	if (argc < 2) {
		print_usage(stderr, argv[0]);
		return EXIT_FAILURE;
	}

	descriptor = open(ST_DEVICE_PATH, O_RDWR | O_CLOEXEC);
	if (descriptor == -1) {
		fprintf(stderr, "%s: %s\n", ST_DEVICE_PATH, strerror(errno));
		return EXIT_FAILURE;
	}

	if (!strcmp(argv[1], "configure")) {
		result = handle_configuration(descriptor, argc, argv);
	} else if (!strcmp(argv[1], "clear") && argc == 2) {
		struct st_configuration_request request = {
			.clear = true,
		};

		result = apply_configuration(descriptor, &request);
		if (result != -1)
			result = show_configuration(descriptor, false);
	} else if (!strcmp(argv[1], "show") && (argc == 2 || (argc == 3 && !strcmp(argv[2], "--raw")))) {
		result = show_configuration(descriptor, argc == 3);
	} else if (!strcmp(argv[1], "status") && argc == 2) {
		struct st_config config;

		result = get_config(descriptor, &config);
		if (result != -1)
			print_status(&config);
	} else if (!strcmp(argv[1], "stats") && argc == 2) {
		struct st_stats stats;

		result = ioctl(descriptor, ST_IOC_GET_STATS, &stats);
		if (result != -1)
			print_stats(&stats);
	} else if (!strcmp(argv[1], "reset-stats") && argc == 2) {
		result = ioctl(descriptor, ST_IOC_RESET_STATS);
	} else if (!strcmp(argv[1], "list") && argc == 2) {
		result = list_configuration(descriptor);
	} else if ((!strcmp(argv[1], "add-program") || !strcmp(argv[1], "remove-program")) && argc == 3) {
		struct st_program program;
		unsigned long command;

		if (prepare_program(argv[2], &program)) {
			fprintf(stderr, "path eseguibile non valido: %s\n", argv[2]);
			errno = EINVAL;
			goto failure;
		}
		command = !strcmp(argv[1], "add-program") ?
			ST_IOC_ADD_PROGRAM : ST_IOC_REMOVE_PROGRAM;
		result = ioctl(descriptor, command, &program);
	} else if ((!strcmp(argv[1], "add-uid") || !strcmp(argv[1], "remove-uid")) && argc == 3) {
		struct st_uid uid;
		unsigned long command;

		if (parse_u32(argv[2], &uid.value)) {
			fprintf(stderr, "UID non valido: %s\n", argv[2]);
			errno = EINVAL;
			goto failure;
		}
		command = !strcmp(argv[1], "add-uid") ?
			ST_IOC_ADD_UID : ST_IOC_REMOVE_UID;
		result = ioctl(descriptor, command, &uid);
	} else if ((!strcmp(argv[1], "add-syscall") || !strcmp(argv[1], "remove-syscall")) && argc == 3) {
		struct st_syscall syscall;
		unsigned long command;

		if (st_resolve_syscall(argv[2], &syscall.number)) {
			fprintf(stderr, "syscall non valida o sconosciuta: %s\n", argv[2]);
			errno = EINVAL;
			goto failure;
		}
		command = !strcmp(argv[1], "add-syscall") ?
			ST_IOC_ADD_SYSCALL : ST_IOC_REMOVE_SYSCALL;
		result = ioctl(descriptor, command, &syscall);
	} else if (!strcmp(argv[1], "set-max") && argc == 3) {
		__u32 max_per_second;

		if (parse_u32(argv[2], &max_per_second) || !max_per_second ||
		    max_per_second > ST_MAX_LIMIT) {
			fprintf(stderr, "valore MAX non valido: %s\n", argv[2]);
			errno = EINVAL;
			goto failure;
		}
		result = ioctl(descriptor, ST_IOC_SET_MAX, &max_per_second);
	} else if (!strcmp(argv[1], "enable") && argc == 2) {
		result = ioctl(descriptor, ST_IOC_ENABLE);
	} else if (!strcmp(argv[1], "disable") && argc == 2) {
		result = ioctl(descriptor, ST_IOC_DISABLE);
	} else {
		print_usage(stderr, argv[0]);
		errno = EINVAL;
		goto failure;
	}

	if (result == -1)
		goto failure;
	if (close(descriptor) == -1) {
		fprintf(stderr, "close: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;

failure:
	if (result == -1)
		fprintf(stderr, "%s: %s\n", argv[1], strerror(errno));
	close(descriptor);
	return EXIT_FAILURE;
}
