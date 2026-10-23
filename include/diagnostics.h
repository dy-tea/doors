#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
	DIAG_STAGE_NONE,
	DIAG_STAGE_COMMIT,
	DIAG_STAGE_RENDER,
	DIAG_STAGE_IPC,
} diag_stage_t;

void diag_init(int log_fd);
void diag_fini(void);

void diag_log_line(const char *line);
void diag_log_tail_write(int fd);

// mark start and the end of stage that can block event loop
void diag_stage_enter(diag_stage_t stage);
void diag_stage_leave(void);

const char *diag_stage_name(diag_stage_t stage);
// stalls reported since startup
size_t diag_stall_count(void);

// re-render status document and expose it for the crash bundle. false when the document did not fit
bool diag_status_refresh(void);
const char *diag_status_snapshot(size_t *len);

// write backtrace.txt, log-tail.txt and status.json for `sig`
void diag_crash_bundle(int sig);

// directory this run would write its bundle to
const char *diag_crash_dir(void);
