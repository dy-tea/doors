#define _GNU_SOURCE

#include "commit.h"
#include "diagnostics.h"
#include "diagnostics_internal.h"
#include "fs.h"
#include "log.h"
#include "str.h"
#include "version.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__) && (defined(__GLIBC__) || defined(__GNU_LIBRARY__))
#include <execinfo.h>
#endif

#define LOG_TAIL_CAP (64 * 1024)
#define ALTSTACK_SIZE (64 * 1024)
#define CRASH_MAX_BACKTRACE 64

static char tail_ring[LOG_TAIL_CAP];
static _Atomic size_t tail_head; // next byte to write
static _Atomic size_t tail_len; // bytes stored, capped at LOG_TAIL_CAP
static pthread_mutex_t tail_lock = PTHREAD_MUTEX_INITIALIZER;

void diag_log_line(const char *line) {
	if (line == NULL)
		return;

	size_t n = strlen(line);
	if (n > LOG_TAIL_CAP)
		line += n - LOG_TAIL_CAP, n = LOG_TAIL_CAP;

	pthread_mutex_lock(&tail_lock);

	size_t head = atomic_load_explicit(&tail_head, memory_order_relaxed);
	size_t stored = atomic_load_explicit(&tail_len, memory_order_relaxed);

	size_t first = n < LOG_TAIL_CAP - head ? n : LOG_TAIL_CAP - head;
	memcpy(tail_ring + head, line, first);
	memcpy(tail_ring, line + first, n - first);

	atomic_store_explicit(&tail_head, (head + n) % LOG_TAIL_CAP, memory_order_relaxed);
	atomic_store_explicit(&tail_len, stored + n > LOG_TAIL_CAP ? LOG_TAIL_CAP : stored + n,
		memory_order_release);

	pthread_mutex_unlock(&tail_lock);
}

void diag_log_tail_write(int fd) {
	if (fd < 0)
		return;

	size_t len = atomic_load_explicit(&tail_len, memory_order_acquire);
	size_t head = atomic_load_explicit(&tail_head, memory_order_relaxed);

	if (len < LOG_TAIL_CAP) {
		diag_write_all(fd, tail_ring, len);
		return;
	}

	// oldest byte is at head
	diag_write_all(fd, tail_ring + head, LOG_TAIL_CAP - head);
	diag_write_all(fd, tail_ring, head);
}

static char crash_dir[1024];
static bool crash_ready;

const char *diag_crash_dir(void) {
	return crash_ready ? crash_dir : NULL;
}

static void prepare_crash_dir(void) {
	const char *base = getenv("XDG_STATE_HOME");
	char root[1024];

	if (base != NULL && base[0] == '/') {
		snprintf(root, sizeof(root), "%s/doors/crash", base);
	} else {
		const char *home = getenv("HOME");
		if (home == NULL || home[0] != '/')
			return;
		snprintf(root, sizeof(root), "%s/.local/state/doors/crash", home);
	}

	if (!mkdir_p(root, 0700))
		return;

	time_t now = time(NULL);
	struct tm tm_info;
	localtime_r(&now, &tm_info);
	char stamp[32];
	strftime(stamp, sizeof(stamp), "%Y.%m.%d_%H-%M-%S", &tm_info);

	char pid[16];
	snprintf(pid, sizeof(pid), "%d", (int)getpid());

	str_copy(crash_dir, sizeof(crash_dir), root);
	str_append(crash_dir, sizeof(crash_dir), "/");
	str_append(crash_dir, sizeof(crash_dir), stamp);
	str_append(crash_dir, sizeof(crash_dir), "_");
	str_append(crash_dir, sizeof(crash_dir), pid);
	crash_ready = true;
}

static int crash_open(const char *name) {
	if (!crash_ready)
		return -1;

	// the first file creates the directory, the rest find it there
	if (mkdir(crash_dir, 0700) == -1 && errno != EEXIST)
		return -1;

	char path[1200];
	str_copy(path, sizeof(path), crash_dir);
	str_append(path, sizeof(path), "/");
	str_append(path, sizeof(path), name);

	return open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
}

static void write_int(int fd, long value) {
	char buf[24];
	size_t i = sizeof(buf) - 1;

	if (value < 0) {
		diag_write_str(fd, "-");
		value = -value;
	}

	buf[sizeof(buf) - 1] = '\0';
	do {
		buf[--i] = (char)('0' + (value % 10));
		value /= 10;
	} while (value > 0 && i > 0);

	diag_write_str(fd, buf + i);
}

static const char *signal_name(int sig) {
	switch (sig) {
	case SIGSEGV:
		return "SIGSEGV (segmentation fault)";
	case SIGBUS:
		return "SIGBUS (bus error)";
	case SIGABRT:
		return "SIGABRT (abort)";
	case SIGFPE:
		return "SIGFPE (floating point error)";
	case SIGILL:
		return "SIGILL (illegal instruction)";
	}
	return "unknown signal";
}

static void crash_header(int fd, int sig) {
	diag_write_str(fd, "########## doors crash ##########\n");
	diag_write_str(fd, "signal: ");
	diag_write_str(fd, signal_name(sig));
	diag_write_str(fd, " (");
	write_int(fd, sig);
	diag_write_str(fd, ")\nversion: ");
	diag_write_str(fd, DOORS_VERSION);
	diag_write_str(fd, "\ncommit: ");
	diag_write_str(fd, DOORS_COMMIT);
	diag_write_str(fd, "\npid: ");
	write_int(fd, getpid());
	diag_write_str(fd, "\nlog: ");
	diag_write_str(fd, log_get_path() ? log_get_path() : "(none)");
	diag_write_str(fd, "\n#################################\n\n");
}

void diag_crash_bundle(int sig) {
#if defined(__linux__) && (defined(__GLIBC__) || defined(__GNU_LIBRARY__))
	void *addrs[CRASH_MAX_BACKTRACE];
	int count = (int)backtrace(addrs, CRASH_MAX_BACKTRACE);
#endif

	crash_header(STDOUT_FILENO, sig);
#if defined(__linux__) && (defined(__GLIBC__) || defined(__GNU_LIBRARY__))
	backtrace_symbols_fd(addrs, count, STDOUT_FILENO);
#endif

	int log_fd = diag_log_fd();
	if (log_fd >= 0 && log_fd != STDOUT_FILENO) {
		crash_header(log_fd, sig);
#if defined(__linux__) && (defined(__GLIBC__) || defined(__GNU_LIBRARY__))
		backtrace_symbols_fd(addrs, count, log_fd);
#endif
	}

	int fd = crash_open("backtrace.txt");
	if (fd >= 0) {
		crash_header(fd, sig);
		diag_write_str(fd, "backtrace:\n");
#if defined(__linux__) && (defined(__GLIBC__) || defined(__GNU_LIBRARY__))
		backtrace_symbols_fd(addrs, count, fd);
#endif
		close(fd);
	}

	fd = crash_open("log-tail.txt");
	if (fd >= 0) {
		diag_write_str(fd, "########## last log lines ##########\n\n");
		diag_log_tail_write(fd);
		close(fd);
	}

	size_t status_len = 0;
	const char *status = diag_status_snapshot(&status_len);
	if (status_len > 0 && (fd = crash_open("status.json")) >= 0) {
		diag_write_all(fd, status, status_len);
		close(fd);
	}
}

static void crash_handler(int sig) {
	diag_crash_bundle(sig);
	_exit(128 + sig);
}

static void install_signals(void) {
	static char altstack[ALTSTACK_SIZE];

	stack_t ss = {
		.ss_sp = altstack,
		.ss_size = sizeof(altstack),
		.ss_flags = 0,
	};
	sigaltstack(&ss, NULL);

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = crash_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESETHAND | SA_ONSTACK;

#if defined(__linux__) && (defined(__GLIBC__) || defined(__GNU_LIBRARY__))
	void *prime[4];
	int primelen = (int)backtrace(prime, 4);
	backtrace_symbols_fd(prime, primelen, -1);
#endif

	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGABRT, &sa, NULL);
	sigaction(SIGFPE, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	signal(SIGPIPE, SIG_IGN); // a dead subscriber must not kill the compositor
}

void diag_crash_init(void) {
	prepare_crash_dir();
	install_signals();
}

void diag_crash_fini(void) {
	if (crash_ready)
		rmdir(crash_dir);

	struct sigaction dfl;
	memset(&dfl, 0, sizeof(dfl));
	dfl.sa_handler = SIG_DFL;
	sigaction(SIGSEGV, &dfl, NULL);
	sigaction(SIGBUS, &dfl, NULL);
	sigaction(SIGABRT, &dfl, NULL);
	sigaction(SIGFPE, &dfl, NULL);
	sigaction(SIGILL, &dfl, NULL);
}
