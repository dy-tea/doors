#define _GNU_SOURCE

#include "diagnostics.h"
#include "diagnostics_internal.h"
#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int log_fd = -1;

int diag_log_fd(void) {
	return log_fd;
}

uint64_t diag_monotonic_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

void diag_write_all(int fd, const char *buf, size_t len) {
	while (len > 0) {
		ssize_t n = write(fd, buf, len);
		if (n > 0) {
			buf += n;
			len -= (size_t)n;
		} else if (n < 0 && errno == EINTR)
			continue;
		else
			return;
	}
}

void diag_write_str(int fd, const char *text) {
	diag_write_all(fd, text, strlen(text));
}

void diag_init(int log_fd_in) {
	log_fd = log_fd_in;

	diag_status_init();
	diag_crash_init();
	diag_watchdog_start();
}

void diag_fini(void) {
	diag_watchdog_stop();
	diag_crash_fini();
}
