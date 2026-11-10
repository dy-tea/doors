#pragma once

#include "types.h"

#include <stdbool.h>
#include <stddef.h>
#include <wlr/util/box.h>

// full string as a base 10 integer, ERANGE aware
bool parse_i64(const char *s, long long *out);
bool parse_long(const char *s, long *out);

// full string as a number; NaN and infinity are rejected
bool parse_double(const char *s, double *out);

// as parse_double, then require min <= value <= max
bool parse_double_range(const char *s, double min, double max, double *out);
bool parse_float_range(const char *s, float min, float max, float *out);

// true/false, on/off, yes/no, 1/0, enable(d)/disable(d), case insensitive
bool parse_bool(const char *s, bool *out);

// RRGGBB or RRGGBBAA with an optional leading '#', alpha defaults to 1
bool parse_color_hex(const char *hex, float rgba[4]);

// "R G B [A]" as space separated decimal numbers, alpha defaults to 1
bool parse_color_floats(const char *str, float rgba[4]);

// one or more hex stops plus an optional "<degrees>deg" token, angle is returned in radians
bool parse_gradient(const char *str, float out[BORDER_GRADIENT_MAX_STOPS * 4], int *count,
	float *angle_out);

// a proportion in (0, 1] or a fixed "Npx" of at most 65535
bool parse_scroller_size(const char *tok, scroller_size_t *out);

// exactly two whitespace separated numbers in [min, max]
bool parse_vec2(const char *s, float out[2], float min, float max);

// "key value", (value - arg - 1) is the key length
bool parse_kv_sep(const char *arg, char sep, const char **value);

// "key=value"
bool parse_kv(const char *arg, const char **value);

// true only when arg is "name=value"
bool parse_kv_named(const char *arg, const char *name, const char **value);

// strip literal prefix
bool parse_prefixed(const char *s, const char *prefix, const char **rest);

// tokenize s in place, at most max tokens, returns the number written
int parse_split(char *s, const char *delims, char *out[], int max);

// WIDTHxHEIGHT
bool parse_size(const char *spec, int *width, int *height);

// WIDTHxHEIGHT[:X,Y], X,Y,WIDTH,HEIGHT, or WIDTHxHEIGHT
bool parse_rect(const char *spec, struct wlr_box *out);
