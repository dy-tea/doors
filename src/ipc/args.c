#include "ipc/args.h"
#include "ipc/ipc.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ipc_args_init(ipc_args_t *a, char **v, int n, int fd, const char *cmd) {
	a->v = v;
	a->n = n;
	a->fd = fd;
	a->cmd = cmd;
	a->ctx = NULL;
}

int ipc_left(const ipc_args_t *a) {
	return a->n;
}

const char *ipc_peek(const ipc_args_t *a) {
	return a->n > 0 ? a->v[0] : NULL;
}

const char *ipc_take(ipc_args_t *a) {
	if (a->n <= 0)
		return NULL;
	a->v++;
	a->n--;
	return a->v[-1];
}

const char *ipc_next(ipc_args_t *a) {
	ipc_take(a);
	return ipc_peek(a);
}

bool ipc_need(ipc_args_t *a, const char *what, const char **out) {
	const char *arg = ipc_take(a);
	if (!arg) {
		ipc_fail(a, "Missing %s\n", what);
		return false;
	}
	*out = arg;
	return true;
}

bool ipc_end(ipc_args_t *a) {
	const char *extra = ipc_peek(a);
	if (!extra)
		return true;
	ipc_fail(a, "Unexpected argument \"%s\"\n", extra);
	return false;
}

void ipc_ok(ipc_args_t *a, const char *msg) {
	send_success(a->fd, msg);
}

void ipc_okf(ipc_args_t * a, const char *fmt, ...) {
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	send_success(a->fd, buf);
}

void ipc_fail(ipc_args_t * a, const char *fmt, ...) {
	char msg[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	char buf[320];
	snprintf(buf, sizeof(buf), "%s: %s", a->cmd ? a->cmd : "ipc", msg);
	send_failure(a->fd, buf);
}

static bool parse_long(const char *s, long *out) {
	if (!s || !*s)
		return false;

	errno = 0;
	char *end;
	long v = strtol(s, &end, 10);
	if (end == s || *end != '\0' || errno == ERANGE)
		return false;

	*out = v;
	return true;
}

static bool parse_double(const char *s, double *out) {
	if (!s || !*s)
		return false;

	errno = 0;
	char *end;
	double v = strtod(s, &end);
	if (end == s || *end != '\0')
		return false;

	*out = v;
	return true;
}

bool ipc_parse_float(const char *s, float min, float max, float *out) {
	double v;
	if (!parse_double(s, &v))
		return false;
	if (v < min || v > max)
		return false;

	*out = (float)v;
	return true;
}

bool ipc_parse_bool(const char *s, bool *out) {
	static const cfg_enum_value_t bools[] = {
		{"true", 1},
		{"on", 1},
		{"1", 1},
		{"yes", 1},
		{"false", 0},
		{"off", 0},
		{"0", 0},
		{"no", 0},
		IPC_ENUM_END,
	};

	if (s == NULL)
		return false;

	for (const cfg_enum_value_t *e = bools; e->name; e++) {
		if (strcmp(s, e->name) == 0) {
			*out = e->value != 0;
			return true;
		}
	}
	return false;
}

static void fail_range(ipc_args_t *a, const char *what, double min, double max) {
	if (min == max)
		ipc_fail(a, "%s must be %g\n", what, min);
	else
		ipc_fail(a, "%s must be between %g and %g\n", what, min, max);
}

bool ipc_int(ipc_args_t *a, const char *what, long min, long max, int *out) {
	const char *arg;
	if (!ipc_need(a, what, &arg))
		return false;

	return ipc_int_str(a, arg, what, min, max, out);
}

bool ipc_int_str(ipc_args_t *a, const char *arg, const char *what, long min, long max, int *out) {
	long v;
	if (!parse_long(arg, &v)) {
		ipc_fail(a, "Invalid %s \"%s\"\n", what, arg);
		return false;
	}
	if (v < min || v > max) {
		if (min == max)
			ipc_fail(a, "%s must be %ld\n", what, min);
		else
			ipc_fail(a, "%s must be between %ld and %ld\n", what, min, max);
		return false;
	}

	*out = (int)v;
	return true;
}

bool ipc_double(ipc_args_t *a, const char *what, double min, double max, double *out) {
	const char *arg;
	if (!ipc_need(a, what, &arg))
		return false;

	return ipc_double_str(a, arg, what, min, max, out);
}

bool ipc_double_str(ipc_args_t *a, const char *arg, const char *what, double min, double max,
		double *out) {
	double v;
	if (!parse_double(arg, &v)) {
		ipc_fail(a, "Invalid %s \"%s\"\n", what, arg);
		return false;
	}
	if (v < min || v > max) {
		fail_range(a, what, min, max);
		return false;
	}

	*out = v;
	return true;
}

bool ipc_float(ipc_args_t *a, const char *what, float min, float max, float *out) {
	const char *arg;
	if (!ipc_need(a, what, &arg))
		return false;

	return ipc_float_str(a, arg, what, min, max, out);
}

bool ipc_float_str(ipc_args_t *a, const char *arg, const char *what, float min, float max,
		float *out) {
	double v;
	if (!parse_double(arg, &v)) {
		ipc_fail(a, "Invalid %s \"%s\"\n", what, arg);
		return false;
	}
	if (v < min || v > max) {
		fail_range(a, what, min, max);
		return false;
	}

	*out = (float)v;
	return true;
}

bool ipc_str(ipc_args_t *a, const char *what, char *dst, size_t dstsz) {
	const char *arg;
	if (!ipc_need(a, what, &arg))
		return false;

	snprintf(dst, dstsz, "%s", arg);
	return true;
}

bool ipc_bool(ipc_args_t *a, const char *what, bool *out) {
	const char *arg;
	if (!ipc_need(a, what, &arg))
		return false;

	return ipc_bool_str(a, arg, what, out);
}

bool ipc_bool_str(ipc_args_t *a, const char *arg, const char *what, bool *out) {
	if (!ipc_parse_bool(arg, out)) {
		ipc_fail(a, "Expected true or false for %s, got \"%s\"\n", what, arg);
		return false;
	}
	return true;
}

bool ipc_delta_str(ipc_args_t *a, const char *what, const char *arg, double *out) {
	if (arg[0] != '+' && arg[0] != '-') {
		ipc_fail(a, "%s must start with '+' or '-'\n", what);
		return false;
	}

	double v;
	if (!parse_double(arg, &v)) {
		ipc_fail(a, "Invalid %s \"%s\"\n", what, arg);
		return false;
	}

	*out = v;
	return true;
}

bool ipc_toggle(ipc_args_t *a, bool *cur) {
	const char *arg = ipc_peek(a);
	if (!arg) {
		*cur = !*cur;
		return true;
	}

	bool v;
	if (!ipc_parse_bool(arg, &v)) {
		ipc_fail(a, "Expected true or false, got \"%s\"\n", arg);
		return false;
	}

	ipc_take(a);
	*cur = v;
	return true;
}

bool ipc_enum(ipc_args_t *a, const char *what, const cfg_enum_value_t *tbl, long *out) {
	const char *arg;
	if (!ipc_need(a, what, &arg))
		return false;

	return ipc_enum_names(a, what, arg, tbl, out);
}

bool ipc_enum_names(ipc_args_t *a, const char *what, const char *arg, const cfg_enum_value_t *tbl,
		long *out) {
	for (const cfg_enum_value_t *e = tbl; e->name; e++) {
		if (strcmp(arg, e->name) == 0) {
			*out = e->value;
			return true;
		}
	}

	ipc_buf_t b;
	char list[512];
	ipc_buf_init(&b, list, sizeof(list));
	for (const cfg_enum_value_t *e = tbl; e->name; e++)
		ipc_buff(&b, "%s\"%s\"", b.len > 0 ? ", " : "", e->name);

	ipc_fail(a, "Expected %s to be one of: %s\n", what, list);
	return false;
}

bool ipc_enum_opt(ipc_args_t *a, const char *what, const cfg_enum_value_t *tbl, long *out) {
	if (!ipc_peek(a))
		return false;

	return ipc_enum(a, what, tbl, out);
}

const char *ipc_enum_name(const cfg_enum_value_t *tbl, long value) {
	for (const cfg_enum_value_t *e = tbl; e->name; e++) {
		if (e->value == value)
			return e->name;
	}
	return NULL;
}

void ipc_buf_init(ipc_buf_t *b, char *buf, size_t cap) {
	b->buf = buf;
	b->cap = cap;
	b->len = 0;
	b->truncated = false;

	if (cap > 0)
		buf[0] = '\0';
}

void ipc_buff(ipc_buf_t * b, const char *fmt, ...) {
	if (b->cap == 0)
		return;

	// one byte is reserved for the terminator
	size_t room = b->cap - b->len - 1;
	if (room == 0) {
		b->truncated = true;
		return;
	}

	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(b->buf + b->len, room + 1, fmt, ap);
	va_end(ap);

	if (n < 0) {
		b->truncated = true;
		return;
	}
	if ((size_t)n > room) {
		b->len += room;
		b->truncated = true;
	} else {
		b->len += (size_t)n;
	}
}

void ipc_buf_send(ipc_args_t *a, const ipc_buf_t *b) {
	send_success(a->fd, b->buf);
}

static bool sub_matches(const ipc_sub_t *sub, const char *arg) {
	if (sub->match && sub->match(arg, sub))
		return true;

	if (sub->flag && strcmp(arg, sub->flag) == 0)
		return true;
	if (sub->lname && strcmp(arg, sub->lname) == 0)
		return true;

	for (const char *alias = sub->aliases; alias && *alias; alias += strlen(alias) + 1) {
		if (strcmp(arg, alias) == 0)
			return true;
	}
	return false;
}

void ipc_sub_usage(ipc_buf_t *b, const ipc_sub_t *subs) {
	for (const ipc_sub_t *s = subs; s->flag; s++) {
		if (s->usage)
			ipc_buff(b, "\n  %s", s->usage);
	}
}

void ipc_fail_unknown(ipc_args_t *a, const ipc_sub_t *subs) {
	char buf[1024];
	ipc_buf_t b;
	ipc_buf_init(&b, buf, sizeof(buf));

	const char *arg = ipc_peek(a);
	if (arg)
		ipc_buff(&b, "%s: unknown command \"%s\", expected one of:", a->cmd ? a->cmd : "ipc", arg);
	else
		ipc_buff(&b, "%s: missing command, expected one of:", a->cmd ? a->cmd : "ipc");
	ipc_sub_usage(&b, subs);

	send_failure(a->fd, b.buf);
}

bool ipc_sub_dispatch(ipc_args_t *a, const ipc_sub_t *subs) {
	const char *arg = ipc_peek(a);
	if (!arg)
		return false;

	for (const ipc_sub_t *s = subs; s->flag; s++) {
		if (!sub_matches(s, arg))
			continue;

		ipc_take(a);
		s->fn(a);
		return true;
	}
	return false;
}
