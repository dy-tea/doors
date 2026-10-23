#define _GNU_SOURCE

#include "diagnostics.h"
#include "diagnostics_internal.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define STALL_DEFAULT_MS 500
#define WATCHDOG_TICK_MS 25

static _Atomic int current_stage;
static _Atomic uint64_t current_start; // monotonic ms, 0 when no stage is open
static _Atomic uint64_t reported_start; // start of the stall already reported
static _Atomic size_t stall_total;

static diag_stage_t stage_stack[8];
static int stage_depth;

static pthread_t watchdog;
static bool watchdog_started;
static _Atomic bool watchdog_stop;
static uint32_t stall_ms = STALL_DEFAULT_MS;

const char *diag_stage_name(diag_stage_t stage) {
	switch (stage) {
	case DIAG_STAGE_NONE:
		return "none";
	case DIAG_STAGE_COMMIT:
		return "commit";
	case DIAG_STAGE_RENDER:
		return "render";
	case DIAG_STAGE_IPC:
		return "ipc";
	}
	return "unknown";
}

size_t diag_stall_count(void) {
	return atomic_load_explicit(&stall_total, memory_order_relaxed);
}

static void stall_line(const char *fmt, ...) {
	char text[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(text, sizeof(text) - 2, fmt, ap);
	va_end(ap);

	if (n <= 0)
		return;
	if ((size_t)n > sizeof(text) - 2)
		n = (int)sizeof(text) - 2;
	text[n++] = '\n';
	text[n] = '\0';

	diag_write_all(diag_log_fd() >= 0 ? diag_log_fd() : STDERR_FILENO, text, (size_t)n);
	diag_log_line(text);
}

void diag_stage_enter(diag_stage_t stage) {
	if (stage == DIAG_STAGE_NONE)
		return;

	if (stage_depth < (int)(sizeof(stage_stack) / sizeof(stage_stack[0])))
		stage_stack[stage_depth++] = (diag_stage_t)atomic_load_explicit(&current_stage,
			memory_order_relaxed);

	atomic_store_explicit(&current_start, diag_monotonic_ms(), memory_order_release);
	atomic_store_explicit(&current_stage, stage, memory_order_release);
}

void diag_stage_leave(void) {
	uint64_t start = atomic_load_explicit(&current_start, memory_order_acquire);
	int stage = atomic_load_explicit(&current_stage, memory_order_relaxed);

	if (start != 0 && atomic_load_explicit(&reported_start, memory_order_relaxed) == start) {
		uint64_t now = diag_monotonic_ms();
		stall_line("[WARN] stage \"%s\" finished after %llums", diag_stage_name((diag_stage_t)stage),
			(unsigned long long)(now - start));
	}

	diag_stage_t previous = stage_depth > 0 ? stage_stack[--stage_depth] : DIAG_STAGE_NONE;
	atomic_store_explicit(&current_stage, previous, memory_order_release);
	atomic_store_explicit(&current_start, diag_monotonic_ms(), memory_order_release);
}

static void *watchdog_main(void *data) {
	(void)data;

	while (!atomic_load_explicit(&watchdog_stop, memory_order_relaxed)) {
		struct timespec tick = {
			.tv_sec = 0,
			.tv_nsec = WATCHDOG_TICK_MS * 1000000L
		};
		nanosleep(&tick, NULL);

		if (stall_ms == 0)
			continue;

		int stage = atomic_load_explicit(&current_stage, memory_order_acquire);
		if (stage == DIAG_STAGE_NONE)
			continue;

		uint64_t start = atomic_load_explicit(&current_start, memory_order_acquire);
		if (start == 0)
			continue;

		uint64_t now = diag_monotonic_ms();
		if (now - start < stall_ms)
			continue;

		if (atomic_load_explicit(&reported_start, memory_order_relaxed) == start)
			continue;

		atomic_store_explicit(&reported_start, start, memory_order_relaxed);
		atomic_fetch_add_explicit(&stall_total, 1, memory_order_relaxed);

		stall_line("[WARN] event loop stuck in \"%s\" for %llums", diag_stage_name((diag_stage_t)stage),
			(unsigned long long)(now - start));
	}

	return NULL;
}

void diag_watchdog_start(void) {
	const char *stall_env = getenv("DOORS_STALL_MS");
	stall_ms = stall_env ? (uint32_t)strtoul(stall_env, NULL, 10) : STALL_DEFAULT_MS;

	atomic_store(&watchdog_stop, false);
	if (pthread_create(&watchdog, NULL, watchdog_main, NULL) != 0) {
		fprintf(stderr, "Error: failed to start the stall watchdog\n");
		return;
	}

	watchdog_started = true;
}

void diag_watchdog_stop(void) {
	if (!watchdog_started)
		return;

	atomic_store(&watchdog_stop, true);
	pthread_join(watchdog, NULL);
	watchdog_started = false;
}
