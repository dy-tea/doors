#pragma once

#include <stddef.h>
#include <stdbool.h>

bool streq(const char *a, const char *b);
bool streqi(const char *a, const char *b); // case-insensitive

// copy src into fixed buffer, return stored length
size_t str_copy(char *dst, size_t cap, const char *src);

// append src at end of fixed-buffer string, return stored length
size_t str_append(char *dst, size_t cap, const char *src);

// append item to dst, append sep when non empty, return false when item did not fit
bool str_append_sep(char *dst, size_t cap, const char *sep, const char *item);

#define str_starts(s, prefix) str_starts_impl((s), ("" prefix ""), sizeof("" prefix "") - 1)
#define str_skip_prefix(s, prefix) \
	str_skip_prefix_impl((s), ("" prefix ""), sizeof("" prefix "") - 1)
#define str_ends(s, suffix) str_ends_impl((s), ("" suffix ""), sizeof("" suffix "") - 1)
bool str_starts_impl(const char *s, const char *prefix, size_t prefix_len);
const char *str_skip_prefix_impl(const char *s, const char *prefix, size_t prefix_len);
bool str_ends_impl(const char *s, const char *suffix, size_t suffix_len);

// whitespace helpers
const char *str_ltrim(const char *s); // skip leading whitespace
char *str_rtrim(char *s); // trim trailing whitespace in place
char *str_trim(char *s); // trim both ends in place
void str_chomp(char *s); // remove one trailing \n

bool str_parse_bool(const char *s, bool *out);
const char *str_bool_text(bool value);

// tokenize s in place, at most max tokens, return number of tokens written
int str_split(char *s, const char *delims, char *out[], int max);

// hex digit value, -1 when c is not a hex digit
int str_hex_digit(char c);

typedef struct {
	const char *name;
	long value;
} str_pair_t;

const str_pair_t *str_table_find(const str_pair_t *tbl, const char *name);
const str_pair_t *str_table_find_value(const str_pair_t *tbl, long value);
