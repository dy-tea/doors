#include "diagnostics.h"
#include "fs.h"
#include "log.h"
#include "once.h"
#include "str.h"
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static FILE *log_file = NULL;
static char log_path[2048] = {0};
static char log_dir[1024] = {0};

static const char *verbosity_colors[] = {
	[WLR_SILENT] = "",
	[WLR_ERROR] = "\x1B[1;31m",
	[WLR_INFO] = "\x1B[1;34m",
	[WLR_DEBUG] = "\x1B[1;90m",
};

static const char *verbosity_headers[] = {
	[WLR_SILENT] = "",
	[WLR_ERROR] = "[ERROR]",
	[WLR_INFO] = "[INFO]",
	[WLR_DEBUG] = "[DEBUG]",
};


#define LOGS_TO_KEEP 25

static int log_compare(const void *a, const void *b) {
	const char *const *ea = a;
	const char *const *eb = b;
	return strcmp(*eb, *ea);
}

static bool is_log_entry(const struct dirent *entry) {
	if (entry->d_type == DT_DIR)
		return false;
	size_t len = strlen(entry->d_name);
	return len > 4 && streq(entry->d_name + len - 4, ".log");
}

static void cleanup_old_logs(void) {
	DIR *dir = opendir(log_dir);
	if (!dir)
		return;

	int count = 0;
	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		if (is_log_entry(entry))
			count++;
	}
	rewinddir(dir);

	if (count <= LOGS_TO_KEEP) {
		closedir(dir);
		return;
	}

	char **names = malloc((size_t)count * sizeof(char *));
	if (!names) {
		closedir(dir);
		return;
	}

	int i = 0;
	while (i < count && (entry = readdir(dir)) != NULL) {
		if (!is_log_entry(entry))
			continue;
		names[i] = strdup(entry->d_name);
		if (!names[i])
			break;
		i++;
	}
	closedir(dir);

	qsort(names, (size_t)i, sizeof(char *), log_compare);

	for (int j = LOGS_TO_KEEP; j < i; j++) {
		char full_path[2048];
		snprintf(full_path, sizeof(full_path), "%s/%s", log_dir, names[j]);
		unlink(full_path);
	}

	for (int j = 0; j < i; j++)
		free(names[j]);
	free(names);
}

static void log_callback(enum wlr_log_importance importance, const char *fmt, va_list args) {
	// get current time
	time_t now = time(NULL);
	struct tm tm_info;
	localtime_r(&now, &tm_info);
	char time_str[32];
	strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_info);

	// map importance level to string and color
	if (importance < 0 || importance >= WLR_LOG_IMPORTANCE_LAST)
		importance = WLR_ERROR;
	const char *level_str = verbosity_headers[importance];
	const char *color = verbosity_colors[importance];

	char message[1900];
	va_list args_copy;
	va_copy(args_copy, args);
	int n = vsnprintf(message, sizeof(message), fmt, args_copy);
	va_end(args_copy);

	if (n < 0)
		return;
	if ((size_t)n >= sizeof(message))
		n = (int)sizeof(message) - 1;
	message[n] = '\0';

	// print to stdout (colored)
	fprintf(stdout, "[%s] %s%s %s\033[0m\n", time_str, color, level_str, message);
	fflush(stdout);

	char line[2048];
	snprintf(line, sizeof(line), "[%s] %s %s\n", time_str, level_str, message);

	diag_log_line(line);

	if (log_file) {
		fputs(line, log_file);
		fflush(log_file);
	}
}

const char *log_get_path(void) {
	return log_path[0] != '\0' ? log_path : NULL;
}

int log_get_fd(void) {
	return log_file ? fileno(log_file) : -1;
}

int log_init(const char *log_file_path) {
	ONCE();
	// determine log file path and directory
	if (log_file_path) {
		str_copy(log_path, sizeof(log_path), log_file_path);

		// extract dir from path
		str_copy(log_dir, sizeof(log_dir), log_file_path);
		char *last_slash = strrchr(log_dir, '/');
		if (last_slash)
			*last_slash = '\0';
		else
			snprintf(log_dir, sizeof(log_dir), "%s", ".");
	} else {
		const char *home = getenv("HOME");
		if (!home) {
			fprintf(stderr, "ERROR: HOME environment variable not set\n");
			return -1;
		}

		snprintf(log_dir, sizeof(log_dir), "%s/.cache/doors", home);

		// try to create directories
		struct stat st = {0};
		if (stat(log_dir, &st) == -1) {
			if (!mkdir_p(log_dir, 0700)) {
				fprintf(stderr, "ERROR: Failed to create log directory: %s\n", log_dir);
				return -1;
			}
		}

		time_t now = time(NULL);
		struct tm tm_info;
		localtime_r(&now, &tm_info);
		char time_str[32];
		strftime(time_str, sizeof(time_str), "%Y.%m.%d_%H-%M-%S", &tm_info);
		snprintf(log_path, sizeof(log_path), "%s/%s_%d.log", log_dir, time_str, getpid());
	}

	// rotate old logs for both the default and the caller-supplied location
	cleanup_old_logs();

	// open log file for appending
	log_file = fopen(log_path, "a");
	if (!log_file) {
		fprintf(stderr, "ERROR: Failed to open log file: %s (%s)\n", log_path, strerror(errno));
		return -1;
	}

	fprintf(stdout, "Logging to: %s\n", log_path);

	// log startup
	fprintf(log_file, "\n");
	time_t now = time(NULL);
	struct tm tm_info;
	localtime_r(&now, &tm_info);
	char time_str[32];
	strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_info);
	fprintf(log_file, "########## doors startup (%s) ##########\n", time_str);
	fflush(log_file);

	wlr_log_init(WLR_DEBUG, log_callback);

	return 0;
}

void log_fini(void) {
	ONCE();
	if (log_file) {
		fprintf(log_file, "########## doors shutdown ##########\n\n");
		fflush(log_file);
		fclose(log_file);
		log_file = NULL;
	}
}
