#pragma once

#include <stddef.h>
#include <stdint.h>

uint64_t diag_monotonic_ms(void);

int diag_log_fd(void);

// EINTR safe, signal handler safe
void diag_write_all(int fd, const char *buf, size_t len);
void diag_write_str(int fd, const char *text);

// crash.c
void diag_crash_init(void);
void diag_crash_fini(void);

// watchdog.c
void diag_watchdog_start(void);
void diag_watchdog_stop(void);

// status.c
void diag_status_init(void); // origin of the reported uptime
