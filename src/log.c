#include "log.h"
#include "once.h"
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__) && (defined(__GLIBC__) || defined(__GNU_LIBRARY__))
#include <execinfo.h>
#endif

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

static int mkdir_p(const char *path) {
	char tmp[sizeof(log_dir)];
	snprintf(tmp, sizeof(tmp), "%s", path);

	for (char *p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}

	if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
		return -1;

	return 0;
}

static int log_compare(const void *a, const void *b) {
	const char *const *ea = a;
	const char *const *eb = b;
	return strcmp(*eb, *ea);
}

static void cleanup_old_logs(void) {
	DIR *dir = opendir(log_dir);
	if (!dir)
		return;

	int count = 0;
	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		if (strstr(entry->d_name, ".log"))
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
		if (!strstr(entry->d_name, ".log"))
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
	struct tm *tm_info = localtime(&now);
	char time_str[32];
	strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);

	// map importance level to string and color
	const char *level_str = verbosity_headers[importance];
	const char *color = verbosity_colors[importance];

	// print to stdout (colored)
	fprintf(stdout, "[%s] %s%s ", time_str, color, level_str);
	va_list args_copy;
	va_copy(args_copy, args);
	vfprintf(stdout, fmt, args_copy);
	va_end(args_copy);
	fprintf(stdout, "\033[0m\n");
	fflush(stdout);

	// print to file (uncolored)
	if (log_file) {
		fprintf(log_file, "[%s] %s ", time_str, level_str);
		va_copy(args_copy, args);
		vfprintf(log_file, fmt, args_copy);
		va_end(args_copy);
		fprintf(log_file, "\n");
		fflush(log_file);
	}
}

static void signal_handler(int sig) {
#if defined(__linux__) && (defined(__GLIBC__) || defined(__GNU_LIBRARY__))
	const char *sig_name = "UNKNOWN";
	switch (sig) {
	case SIGSEGV:
		sig_name = "SIGSEGV (Segmentation Fault)";
		break;
	case SIGBUS:
		sig_name = "SIGBUS (Bus Error)";
		break;
	case SIGABRT:
		sig_name = "SIGABRT (Abort)";
		break;
	case SIGFPE:
		sig_name = "SIGFPE (Floating Point Error)";
		break;
	case SIGILL:
		sig_name = "SIGILL (Illegal Instruction)";
		break;
	}

	// get backtrace
	void *addrlist[32];
	int addrlen = (int)backtrace(addrlist, 32);

	// log to stdout
	write(STDOUT_FILENO, "\n########## CRASH REPORT ##########\n", 36);
	write(STDOUT_FILENO, "Signal: ", 8);
	write(STDOUT_FILENO, sig_name, strlen(sig_name));
	write(STDOUT_FILENO, "\nBacktrace:\n", 12);
	backtrace_symbols_fd(addrlist, addrlen, STDOUT_FILENO);
	write(STDOUT_FILENO, "##################################\n\n", 35);

	// log to file
	if (log_file) {
		fprintf(log_file, "\n########## CRASH REPORT ##########\n");
		fprintf(log_file, "Signal: %s (%d)\n", sig_name, sig);
		fprintf(log_file, "Backtrace:\n");
		backtrace_symbols_fd(addrlist, addrlen, fileno(log_file));
		fprintf(log_file, "##################################\n\n");
		fflush(log_file);
	}
#else
	write(STDOUT_FILENO, "Backtrace not available on musl.", 32);
#endif

	// exit with error code
	_exit(128 + sig);
}

int log_setup_signals(void) {
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = signal_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_NODEFER;

	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGABRT, &sa, NULL);
	sigaction(SIGFPE, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	signal(SIGPIPE, SIG_IGN); // TO PREVENT LOG CRASHES

	return 0;
}

const char *log_get_path(void) {
	return log_path[0] != '\0' ? log_path : NULL;
}

int log_init(const char *log_file_path) {
	ONCE();
	// determine log file path and directory
	if (log_file_path) {
		snprintf(log_path, sizeof(log_path), "%s", log_file_path);

		// extract dir from path
		strcpy(log_dir, log_file_path);
		char *last_slash = strrchr(log_dir, '/');
		if (last_slash)
			*last_slash = '\0';
		else
			strcpy(log_dir, ".");
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
			if (mkdir_p(log_dir) != 0) {
				fprintf(stderr, "ERROR: Failed to create log directory: %s\n", log_dir);
				return -1;
			}
		}

		cleanup_old_logs();

		time_t now = time(NULL);
		struct tm *tm_info = localtime(&now);
		char time_str[32];
		strftime(time_str, sizeof(time_str), "%Y.%m.%d_%H-%M-%S", tm_info);
		snprintf(log_path, sizeof(log_path), "%s/%s_%d.log", log_dir, time_str, getpid());
	}

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
	struct tm *tm_info = localtime(&now);
	char time_str[32];
	strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
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
