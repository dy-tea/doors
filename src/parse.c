#include "parse.h"
#include "str.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool scan_long(const char *s, long long *out, const char **end) {
	if (!s || !*s)
		return false;

	errno = 0;
	char *stop;
	long long v = strtoll(s, &stop, 10);
	if (stop == s || errno == ERANGE)
		return false;

	*out = v;
	if (end)
		*end = stop;
	return true;
}

static bool scan_double(const char *s, double *out, const char **end) {
	if (!s || !*s)
		return false;

	char *stop;
	double v = strtod(s, &stop);
	if (stop == s || !isfinite(v))
		return false;

	*out = v;
	if (end)
		*end = stop;
	return true;
}

bool parse_i64(const char *s, long long *out) {
	long long v;
	const char *end;
	if (!scan_long(s, &v, &end) || *end != '\0')
		return false;

	*out = v;
	return true;
}

bool parse_long(const char *s, long *out) {
	long long v;
	if (!parse_i64(s, &v))
		return false;
	if (v < LONG_MIN || v > LONG_MAX)
		return false;

	*out = (long)v;
	return true;
}

bool parse_double(const char *s, double *out) {
	double v;
	const char *end;
	if (!scan_double(s, &v, &end) || *end != '\0')
		return false;

	*out = v;
	return true;
}

bool parse_double_range(const char *s, double min, double max, double *out) {
	double v;
	if (!parse_double(s, &v) || v < min || v > max)
		return false;

	*out = v;
	return true;
}

bool parse_float_range(const char *s, float min, float max, float *out) {
	double v;
	if (!parse_double_range(s, (double)min, (double)max, &v))
		return false;

	*out = (float)v;
	return true;
}

static const str_pair_t bool_texts[] = {
	{"true", 1},
	{"on", 1},
	{"yes", 1},
	{"1", 1},
	{"enable", 1},
	{"enabled", 1},
	{"false", 0},
	{"off", 0},
	{"no", 0},
	{"0", 0},
	{"disable", 0},
	{"disabled", 0},
	{NULL, 0}
};

bool parse_bool(const char *s, bool *out) {
	const str_pair_t *e = str_table_find(bool_texts, s);
	if (!e)
		return false;

	*out = e->value != 0;
	return true;
}

bool parse_color_hex(const char *hex, float rgba[4]) {
	if (!hex || !rgba)
		return false;
	if (*hex == '#')
		hex++;

	const size_t len = strlen(hex);
	if (len != 6 && len != 8)
		return false;

	float parsed[4] = {
		0.0f,
		0.0f,
		0.0f,
		1.0f
	};
	for (size_t i = 0; i < len / 2; i++) {
		const int hi = str_hex_digit(hex[i * 2]);
		const int lo = str_hex_digit(hex[i * 2 + 1]);
		if (hi < 0 || lo < 0)
			return false;

		parsed[i] = (float)(hi * 16 + lo) / 255.0f;
	}

	memcpy(rgba, parsed, sizeof(parsed));
	return true;
}

bool parse_color_floats(const char *str, float rgba[4]) {
	if (!str || !rgba)
		return false;

	const char *cursor = str_ltrim(str);
	int n = 0;

	while (n < 4) {
		double v;
		const char *end;
		if (!scan_double(cursor, &v, &end))
			break;

		rgba[n++] = (float)v;
		cursor = str_ltrim(end);
	}

	// without alpha
	if (n < 3 || *cursor != '\0')
		return false;

	if (n == 3)
		rgba[3] = 1.0f;

	return true;
}

bool parse_gradient(const char *str, float out[BORDER_GRADIENT_MAX_STOPS * 4], int *count,
		float *angle_out) {
	if (!str || !count || !angle_out || !out)
		return false;

	char buf[512];
	str_copy(buf, sizeof(buf), str);

	*count = 0;
	*angle_out = 0.0f;

	char *saveptr = NULL;
	for (char *tok = strtok_r(buf, " \t", &saveptr); tok; tok = strtok_r(NULL, " \t", &saveptr)) {
		if (str_ends(tok, "deg")) {
			char tmp[16];
			const size_t tlen = str_copy(tmp, sizeof(tmp), tok);
			if (tlen >= 3) {
				double degrees;
				tmp[tlen - 3] = '\0';
				if (parse_double(tmp, &degrees))
					*angle_out = (float)degrees * 3.14159265f / 180.0f;
			}
		} else if (strlen(tok) >= 6) {
			if (*count >= BORDER_GRADIENT_MAX_STOPS)
				break;

			float rgba[4];
			if (parse_color_hex(tok, rgba)) {
				memcpy(out + (*count) * 4, rgba, 4 * sizeof(float));
				(*count)++;
			}
		}
	}

	return *count >= 1;
}

bool parse_scroller_size(const char *tok, scroller_size_t *out) {
	if (!tok || !out)
		return false;

	const size_t len = strlen(tok);

	if (len > 2 && streq(tok + len - 2, "px")) {
		double value;
		const char *end;
		if (!scan_double(tok, &value, &end) || end != tok + len - 2)
			return false;
		if (value <= 0.0 || value > 65535.0)
			return false;

		out->type = SCROLLER_SIZE_FIXED;
		out->value = value;
		return true;
	}

	double value;
	if (!parse_double(tok, &value) || value <= 0.0 || value > 1.0)
		return false;

	out->type = SCROLLER_SIZE_PROPORTION;
	out->value = value;
	return true;
}

bool parse_vec2(const char *s, float out[2], float min, float max) {
	if (!s || !out)
		return false;

	const char *cursor = str_ltrim(s);

	for (int i = 0; i < 2; i++) {
		double v;
		const char *end;
		if (!scan_double(cursor, &v, &end) || v < min || v > max)
			return false;

		out[i] = (float)v;
		cursor = str_ltrim(end);
	}

	return *cursor == '\0';
}

bool parse_kv_sep(const char *arg, char sep, const char **value) {
	if (!arg)
		return false;

	const char *at = strchr(arg, sep);
	if (at == NULL || at == arg)
		return false;

	if (value)
		*value = at + 1;
	return true;
}

bool parse_kv(const char *arg, const char **value) {
	return parse_kv_sep(arg, '=', value);
}

bool parse_kv_named(const char *arg, const char *name, const char **value) {
	if (!arg || !name)
		return false;

	const size_t n = strlen(name);
	if (strncmp(arg, name, n) != 0 || arg[n] != '=')
		return false;

	if (value)
		*value = arg + n + 1;
	return true;
}

bool parse_prefixed(const char *s, const char *prefix, const char **rest) {
	if (!s || !prefix)
		return false;

	const size_t len = strlen(prefix);
	if (strncmp(s, prefix, len) != 0)
		return false;
	if (s[len] == '\0')
		return false;

	if (rest)
		*rest = s + len;
	return true;
}

int parse_split(char *s, const char *delims, char *out[], const int max) {
	if (!s || !out || max == 0)
		return 0;

	int n = 0;
	char *saveptr = NULL;
	for (char *tok = strtok_r(s, delims, &saveptr); tok; tok = strtok_r(NULL, delims, &saveptr)) {
		out[n++] = tok;
		if (n == max)
			break;
	}

	return n;
}

bool parse_size(const char *spec, int *width, int *height) {
	if (!spec || !width || !height)
		return false;

	long long w;
	const char *cursor;
	if (!scan_long(spec, &w, &cursor) || *cursor != 'x')
		return false;

	long long h;
	const char *end;
	if (!scan_long(cursor + 1, &h, &end) || *end != '\0')
		return false;

	if (w <= 0 || h <= 0 || w > INT_MAX || h > INT_MAX)
		return false;

	*width = (int)w;
	*height = (int)h;
	return true;
}

// parses exactly count comma separated integers
static bool parse_int_list(const char *spec, int *out, int count) {
	const char *cursor = spec;

	for (int i = 0; i < count; i++) {
		long long v;
		const char *end;
		if (!scan_long(cursor, &v, &end) || v < INT_MIN || v > INT_MAX)
			return false;

		out[i] = (int)v;

		if (i + 1 < count) {
			if (*end != ',')
				return false;
			cursor = end + 1;
		} else if (*end != '\0') {
			return false;
		}
	}

	return true;
}

bool parse_rect(const char *spec, struct wlr_box *out) {
	if (!spec || !out)
		return false;

	struct wlr_box rect = *out;

	const char *colon = strchr(spec, ':');
	if (colon) {
		char size[64];
		const size_t len = (size_t)(colon - spec);
		if (len >= sizeof(size))
			return false;

		memcpy(size, spec, len);
		size[len] = '\0';

		if (!parse_size(size, &rect.width, &rect.height))
			return false;

		int position[2];
		if (!parse_int_list(colon + 1, position, 2))
			return false;

		rect.x = position[0];
		rect.y = position[1];
		*out = rect;
		return true;
	}

	if (parse_size(spec, &rect.width, &rect.height)) {
		*out = rect;
		return true;
	}

	// X,Y,WIDTH,HEIGHT
	int values[4];
	if (!parse_int_list(spec, values, 4))
		return false;
	if (values[2] <= 0 || values[3] <= 0)
		return false;

	rect.x = values[0];
	rect.y = values[1];
	rect.width = values[2];
	rect.height = values[3];
	*out = rect;
	return true;
}
