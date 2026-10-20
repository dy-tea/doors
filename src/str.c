#include "str.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

bool streq(const char *a, const char *b) {
	return a == b || (a && b && strcmp(a, b) == 0);
}

bool streqi(const char *a, const char *b) {
	return a == b || (a && b && strcasecmp(a, b) == 0);
}

size_t str_copy(char *dst, size_t cap, const char *src) {
	if (!dst || cap == 0)
		return 0;

	if (!src)
		src = "";

	size_t len = strlen(src);
	if (len > cap - 1)
		len = cap - 1;

	memcpy(dst, src, len);
	dst[len] = '\0';
	return len;
}

size_t str_append(char *dst, size_t cap, const char *src) {
	if (!dst || cap == 0)
		return 0;

	size_t at = strlen(dst);
	if (at >= cap - 1)
		return 0;

	return str_copy(dst + at, cap - at, src);
}

bool str_append_sep(char *dst, size_t cap, const char *sep, const char *item) {
	if (!dst || cap == 0 || !item)
		return false;

	size_t len = strlen(dst);
	if (len > 0 && sep && *sep) {
		size_t sep_len = strlen(sep);
		if (sep_len > cap - 1 - len)
			return false;
		memcpy(dst + len, sep, sep_len);
		len += sep_len;
	}

	size_t left = cap - 1 - len;
	size_t item_len = strlen(item);
	item_len = item_len > left ? left : item_len;

	memcpy(dst + len, item, item_len);
	dst[len + item_len] = '\0';

	return item_len == strlen(item);
}

bool str_starts_impl(const char *s, const char *prefix, size_t prefix_len) {
	if (!s)
		return false;

	return strncmp(s, prefix, prefix_len) == 0;
}

const char *str_skip_prefix_impl(const char *s, const char *prefix, size_t prefix_len) {
	if (!str_starts_impl(s, prefix, prefix_len))
		return NULL;

	return s + prefix_len;
}

bool str_ends_impl(const char *s, const char *suffix, size_t suffix_len) {
	if (!s)
		return false;

	size_t len = strlen(s);
	if (len < suffix_len)
		return false;

	return strcmp(s + len - suffix_len, suffix) == 0;
}

const char *str_ltrim(const char *s) {
	if (!s)
		return s;

	while (isspace((unsigned char)*s))
		s++;

	return s;
}

char *str_rtrim(char *s) {
	if (!s)
		return s;

	size_t len = strlen(s);
	while (len > 0 && isspace((unsigned char)s[len - 1]))
		s[--len] = '\0';
	return s;
}

char *str_trim(char *s) {
	if (!s)
		return s;

	const char *start = str_ltrim(s);
	size_t len = strlen(start);
	while (len > 0 && isspace((unsigned char)start[len - 1]))
		len--;

	if (start != s)
		memmove(s, start, len);
	s[len] = '\0';
	return s;
}

void str_chomp(char *s) {
	if (!s)
		return;

	size_t len = strlen(s);
	if (len > 0 && s[len - 1] == '\n') {
		s[--len] = '\0';
		if (len > 0 && s[len - 1] == '\r')
			s[--len] = '\0';
	}
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

bool str_parse_bool(const char *s, bool *out) {
	const str_pair_t *e = str_table_find(bool_texts, s);
	if (!e)
		return false;

	*out = e->value != 0;
	return true;
}

const char *str_bool_text(bool value) {
	static const char *const texts[] = {
		"false",
		"true"
	};
	return texts[value];
}

int str_split(char *s, const char *delims, char *out[], int max) {
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

int str_hex_digit(char c) {
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return 10 + c - 'a';
	if (c >= 'A' && c <= 'F')
		return 10 + c - 'A';
	return -1;
}

const str_pair_t *str_table_find(const str_pair_t *tbl, const char *name) {
	if (!tbl || !name)
		return NULL;

	for (const str_pair_t *e = tbl; e->name; e++) {
		if (streqi(e->name, name))
			return e;
	}
	return NULL;
}

const str_pair_t *str_table_find_value(const str_pair_t *tbl, long value) {
	if (!tbl)
		return NULL;

	for (const str_pair_t *e = tbl; e->name; e++) {
		if (e->value == value)
			return e;
	}
	return NULL;
}
