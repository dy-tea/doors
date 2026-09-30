#pragma once

#include "ipc/ipc.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
	char **v; // next unconsumed argument
	int n; // number of unconsumed arguments
	int fd; // client socket the reply is written to
	const char *cmd; // command name, used as the error prefix
	void *ctx; // command-scoped state, for handlers that resolve arguments up front
} ipc_args_t;

void ipc_args_init(ipc_args_t *a, char **v, int n, int fd, const char *cmd);

int ipc_left(const ipc_args_t *a); // arguments not yet consumed
const char *ipc_peek(const ipc_args_t *a); // NULL when exhausted
const char *ipc_take(ipc_args_t *a); // NULL when exhausted
const char *ipc_next(ipc_args_t *a); // consume, then return the next
bool ipc_need(ipc_args_t *a, const char *what, const char **out);

// iterate the remaining arguments, consuming each exactly once
#define ipc_foreach(a, arg) \
	for (const char *arg = ipc_peek(a); arg != NULL; arg = ipc_next(a))

void ipc_ok(ipc_args_t *a, const char *msg);
void ipc_okf(ipc_args_t * a, const char *fmt, ...);
void ipc_fail(ipc_args_t * a, const char *fmt, ...);

bool ipc_end(ipc_args_t *a);

bool ipc_int(ipc_args_t *a, const char *what, long min, long max, int *out);
bool ipc_double(ipc_args_t *a, const char *what, double min, double max, double *out);
bool ipc_float(ipc_args_t *a, const char *what, float min, float max, float *out);
bool ipc_bool(ipc_args_t *a, const char *what, bool *out);
bool ipc_str(ipc_args_t *a, const char *what, char *dst, size_t dstsz);

bool ipc_delta(ipc_args_t *a, const char *what, double *out);
bool ipc_delta_str(ipc_args_t *a, const char *what, const char *arg, double *out);

bool ipc_toggle(ipc_args_t *a, bool *cur);

typedef struct {
	const char *name;
	long value;
} cfg_enum_value_t;

bool ipc_enum(ipc_args_t *a, const char *what, const cfg_enum_value_t *tbl, long *out);
bool ipc_enum_opt(ipc_args_t *a, const char *what, const cfg_enum_value_t *tbl, long *out);

const char *ipc_enum_name(const cfg_enum_value_t *tbl, long value);

bool ipc_enum_names(ipc_args_t *a, const char *what, const char *arg, const cfg_enum_value_t *tbl,
	long *out);
bool ipc_int_str(ipc_args_t *a, const char *arg, const char *what, long min, long max, int *out);
bool ipc_bool_str(ipc_args_t *a, const char *arg, const char *what, bool *out);
bool ipc_float_str(ipc_args_t *a, const char *arg, const char *what, float min, float max,
	float *out);
bool ipc_double_str(ipc_args_t *a, const char *arg, const char *what, double min, double max,
	double *out);

typedef struct {
	char *buf;
	size_t cap;
	size_t len;
	bool truncated;
} ipc_buf_t;

void ipc_buf_init(ipc_buf_t *b, char *buf, size_t cap);
void ipc_buff(ipc_buf_t * b, const char *fmt, ...);
void ipc_buf_send(ipc_args_t *a, const ipc_buf_t *b);

// declares a reply buffer and its writer, ready to be sent with ipc_buf_send
#define IPC_REPLY(name) \
	char name##_storage[DOORS_BUFSIZ] = {0}; \
	ipc_buf_t name = {.buf = name##_storage, .cap = sizeof(name##_storage)}

// appends `"a", "b", "c"` for a fixed-length array whose first member is `name`
#define IPC_FORMAT_NAMES(b, arr, n) \
	for (size_t _i = 0; _i < (size_t)(n); _i++) \
		ipc_buff((b), "%s\"%s\"", _i > 0 ? ", " : "", (arr)[_i].name)

typedef struct ipc_sub_t ipc_sub_t;

typedef void (*ipc_sub_fn)(ipc_args_t *a);
typedef bool (*ipc_sub_match)(const char *arg, const ipc_sub_t *sub);

struct ipc_sub_t {
	const char *flag; // short form
	const char *lname; // long form
	const char *aliases; // further names
	const char *usage;
	ipc_sub_fn fn;
	ipc_sub_match match;
};


#define IPC_ARRAY_LEN(arr) (sizeof(arr) / sizeof((arr)[0]))

#define IPC_ENUM_END {NULL, 0}
#define IPC_SUB_END {0}

#define IPC_SUB(flag, lname, usage, fn) {flag, lname, NULL, usage, fn, NULL}
#define IPC_SUBA(flag, lname, aliases, usage, fn) {flag, lname, aliases "\0", usage, fn, NULL}
#define IPC_SUBM(flag, lname, usage, fn, match) {flag, lname, NULL, usage, fn, match}

bool ipc_sub_dispatch(ipc_args_t *a, const ipc_sub_t *subs);
void ipc_fail_unknown(ipc_args_t *a, const ipc_sub_t *subs);
void ipc_sub_usage(ipc_buf_t *b, const ipc_sub_t *subs);
