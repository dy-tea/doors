#include "ipc/json.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 64

typedef struct {
	const char *p;
	const char *start;
	char *err;
	size_t errsz;
	bool failed;
} json_parser_t;

static bool json_fail(json_parser_t * ps, const char *fmt, ...) {
	if (!ps->failed) {
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(ps->err, ps->errsz, fmt, ap);
		va_end(ap);
	}
	ps->failed = true;
	return false;
}

static bool json_fail_at(json_parser_t * ps, const char *fmt, ...) {
	if (!ps->failed) {
		size_t left = ps->errsz;
		int n = snprintf(ps->err, left, "at offset %zu: ", (size_t)(ps->p - ps->start));
		if (n > 0 && (size_t)n < left) {
			va_list ap;
			va_start(ap, fmt);
			vsnprintf(ps->err + n, left - (size_t)n, fmt, ap);
			va_end(ap);
		}
	}
	ps->failed = true;
	return false;
}

static bool json_skip_ws(json_parser_t *ps) {
	while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')
		ps->p++;
	return true;
}

static size_t json_utf8_encode(char *out, unsigned cp) {
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xE0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	out[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

static bool json_hex4(json_parser_t *ps, unsigned *out) {
	unsigned value = 0;
	for (int i = 0; i < 4; i++) {
		char c = ps->p[i];
		unsigned digit;
		if (c >= '0' && c <= '9')
			digit = (unsigned)(c - '0');
		else if (c >= 'a' && c <= 'f')
			digit = (unsigned)(c - 'a') + 10;
		else if (c >= 'A' && c <= 'F')
			digit = (unsigned)(c - 'A') + 10;
		else
			return json_fail_at(ps, "invalid \\u escape");
		value = (value << 4) | digit;
	}

	ps->p += 4;
	*out = value;
	return true;
}

static char *json_parse_string(json_parser_t *ps) {
	if (*ps->p != '"') {
		json_fail_at(ps, "expected a string");
		return NULL;
	}

	ps->p++;

	// escapes never make the result longer than the input
	char *out = malloc((size_t)(strlen(ps->p)) + 1);
	if (out == NULL) {
		json_fail(ps, "out of memory");
		return NULL;
	}

	size_t len = 0;
	while (*ps->p != '"') {
		if (*ps->p == '\0') {
			free(out);
			json_fail_at(ps, "unterminated string");
			return NULL;
		}

		if ((unsigned char)*ps->p < 0x20) {
			free(out);
			json_fail_at(ps, "control character in string");
			return NULL;
		}

		if (*ps->p != '\\') {
			out[len++] = *ps->p++;
			continue;
		}

		ps->p++;
		switch (*ps->p) {
		case '"':
		case '\\':
		case '/':
			out[len++] = *ps->p++;
			break;
		case 'b':
			out[len++] = '\b', ps->p++;
			break;
		case 'f':
			out[len++] = '\f', ps->p++;
			break;
		case 'n':
			out[len++] = '\n', ps->p++;
			break;
		case 'r':
			out[len++] = '\r', ps->p++;
			break;
		case 't':
			out[len++] = '\t', ps->p++;
			break;
		case 'u': {
			unsigned cp;
			ps->p++;
			if (!json_hex4(ps, &cp)) {
				free(out);
				return NULL;
			}

			// a surrogate pair encodes one code point
			if (cp >= 0xD800 && cp <= 0xDBFF && ps->p[0] == '\\' && ps->p[1] == 'u') {
				const char *save = ps->p;
				ps->p += 2;
				unsigned low;
				if (!json_hex4(ps, &low)) {
					free(out);
					return NULL;
				}
				if (low >= 0xDC00 && low <= 0xDFFF)
					cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
				else
					ps->p = save;
			}

			len += json_utf8_encode(out + len, cp);
			break;
		}
		default:
			free(out);
			json_fail_at(ps, "invalid escape");
			return NULL;
		}
	}

	ps->p++; // closing quote
	out[len] = '\0';
	return out;
}

static bool json_parse_number(json_parser_t *ps, double *out) {
	const char *p = ps->p;

	if (*p == '-')
		p++;

	if (*p == '0') {
		p++;
	} else if (*p >= '1' && *p <= '9') {
		while (isdigit((unsigned char)*p))
			p++;
	} else
		return json_fail_at(ps, "expected a number");

	if (*p == '.') {
		p++;
		if (!isdigit((unsigned char)*p))
			return json_fail_at(ps, "expected a digit");
		while (isdigit((unsigned char)*p))
			p++;
	}

	if (*p == 'e' || *p == 'E') {
		p++;
		if (*p == '+' || *p == '-')
			p++;
		if (!isdigit((unsigned char)*p))
			return json_fail_at(ps, "expected a digit");
		while (isdigit((unsigned char)*p))
			p++;
	}

	size_t len = (size_t)(p - ps->p);
	char buf[64];
	if (len >= sizeof(buf))
		return json_fail_at(ps, "number is too long");

	memcpy(buf, ps->p, len);
	buf[len] = '\0';
	ps->p = p;

	*out = strtod(buf, NULL);
	return true;
}

// appends a member to an object or an item to an array, keys stays NULL for arrays
static bool json_list_push(char ***keys, json_value_t ***items, size_t *count, char *key,
		json_value_t *item) {
	if (*count == 0 || (*count & (*count - 1)) == 0) {
		size_t cap = *count == 0 ? 2 : *count * 2;

		if (keys != NULL) {
			char **new_keys = realloc(*keys, cap * sizeof(char *));
			if (new_keys == NULL)
				return false;
			*keys = new_keys;
		}

		json_value_t **new_items = realloc(*items, cap * sizeof(json_value_t *));
		if (new_items == NULL)
			return false;
		*items = new_items;
	}

	if (keys != NULL)
		(*keys)[*count] = key;
	(*items)[*count] = item;
	(*count)++;
	return true;
}

static json_value_t *json_new(json_type_t type) {
	json_value_t *value = calloc(1, sizeof(json_value_t));
	if (value != NULL)
		value->type = type;
	return value;
}

static json_value_t *json_parse_value(json_parser_t *ps, int depth);

static json_value_t *json_parse_array(json_parser_t *ps, int depth) {
	ps->p++; // '['

	json_value_t *array = json_new(JSON_ARRAY);
	if (array == NULL) {
		json_fail(ps, "out of memory");
		return NULL;
	}

	json_skip_ws(ps);
	if (*ps->p == ']') {
		ps->p++;
		return array;
	} while (true) {
		json_value_t *item = json_parse_value(ps, depth + 1);
		if (item == NULL) {
			json_free(array);
			return NULL;
		}

		if (!json_list_push(NULL, &array->value.list.items, &array->value.list.count, NULL, item)) {
			json_fail(ps, "out of memory");
			json_free(item);
			json_free(array);
			return NULL;
		}

		json_skip_ws(ps);
		if (*ps->p == ',') {
			ps->p++;
			json_skip_ws(ps);
			continue;
		}

		if (*ps->p == ']') {
			ps->p++;
			return array;
		}

		json_free(array);
		json_fail_at(ps, "expected ',' or ']'");
		return NULL;
	}
}

static json_value_t *json_parse_object(json_parser_t *ps, int depth) {
	ps->p++; // '{'

	json_value_t *object = json_new(JSON_OBJECT);
	if (object == NULL) {
		json_fail(ps, "out of memory");
		return NULL;
	}

	json_skip_ws(ps);
	if (*ps->p == '}') {
		ps->p++;
		return object;
	} while (true) {
		char *key = json_parse_string(ps);
		if (key == NULL) {
			json_free(object);
			return NULL;
		}

		json_skip_ws(ps);
		if (*ps->p != ':') {
			free(key);
			json_free(object);
			json_fail_at(ps, "expected ':'");
			return NULL;
		}
		ps->p++;
		json_skip_ws(ps);

		json_value_t *item = json_parse_value(ps, depth + 1);
		if (item == NULL) {
			free(key);
			json_free(object);
			return NULL;
		}

		if (!json_list_push(&object->value.list.keys, &object->value.list.items, &object->value.list.count,
				key, item)) {
			json_fail(ps, "out of memory");
			free(key);
			json_free(item);
			json_free(object);
			return NULL;
		}

		json_skip_ws(ps);
		if (*ps->p == ',') {
			ps->p++;
			json_skip_ws(ps);
			continue;
		}

		if (*ps->p == '}') {
			ps->p++;
			return object;
		}

		json_free(object);
		json_fail_at(ps, "expected ',' or '}'");
		return NULL;
	}
}

static json_value_t *json_parse_value(json_parser_t *ps, int depth) {
	if (depth > JSON_MAX_DEPTH) {
		json_fail(ps, "input is nested too deeply");
		return NULL;
	}

	json_skip_ws(ps);

	if (*ps->p == '\0') {
		json_fail_at(ps, "expected a value");
		return NULL;
	}

	switch (*ps->p) {
	case '{':
		return json_parse_object(ps, depth);
	case '[':
		return json_parse_array(ps, depth);
	case '"': {
		char *string = json_parse_string(ps);
		if (string == NULL)
			return NULL;

		json_value_t *value = json_new(JSON_STRING);
		if (value == NULL) {
			free(string);
			json_fail(ps, "out of memory");
			return NULL;
		}

		value->value.string = string;
		return value;
	}
	case 't':
		if (strncmp(ps->p, "true", 4) == 0) {
			ps->p += 4;
			json_value_t *value = json_new(JSON_BOOL);
			if (value == NULL) {
				json_fail(ps, "out of memory");
				return NULL;
			}
			value->value.boolean = true;
			return value;
		}
		break;
	case 'f':
		if (strncmp(ps->p, "false", 5) == 0) {
			ps->p += 5;
			json_value_t *value = json_new(JSON_BOOL);
			if (value == NULL) {
				json_fail(ps, "out of memory");
				return NULL;
			}
			value->value.boolean = false;
			return value;
		}
		break;
	case 'n':
		if (strncmp(ps->p, "null", 4) == 0) {
			ps->p += 4;
			json_value_t *value = json_new(JSON_NULL);
			if (value == NULL)
				json_fail(ps, "out of memory");
			return value;
		}
		break;
	default: {
		double number;
		if (!json_parse_number(ps, &number))
			return NULL; // the reason is already recorded

		json_value_t *value = json_new(JSON_NUMBER);
		if (value == NULL) {
			json_fail(ps, "out of memory");
			return NULL;
		}

		value->value.number = number;
		return value;
	}
	}

	json_fail_at(ps, "expected a value");
	return NULL;
}

json_value_t *json_parse(const char *text, char *err, size_t errsz) {
	if (text == NULL) {
		if (err != NULL && errsz > 0)
			snprintf(err, errsz, "no input");
		return NULL;
	}

	json_parser_t ps = {
		.p = text,
		.start = text,
		.err = err,
		.errsz = errsz,
		.failed = false,
	};

	if (err != NULL && errsz > 0)
		err[0] = '\0';

	json_skip_ws(&ps);
	json_value_t *root = json_parse_value(&ps, 0);
	if (root == NULL)
		return NULL;

	json_skip_ws(&ps);
	if (*ps.p != '\0') {
		json_free(root);
		json_fail_at(&ps, "trailing garbage");
		return NULL;
	}

	return root;
}

void json_free(json_value_t *value) {
	if (value == NULL)
		return;

	if (value->type == JSON_STRING) {
		free(value->value.string);
	} else if (value->type == JSON_ARRAY || value->type == JSON_OBJECT) {
		for (size_t i = 0; i < value->value.list.count; i++) {
			if (value->value.list.keys != NULL)
				free(value->value.list.keys[i]);
			json_free(value->value.list.items[i]);
		}
		free(value->value.list.keys);
		free(value->value.list.items);
	}

	free(value);
}

const json_value_t *json_get(const json_value_t *value, const char *key) {
	if (value == NULL || value->type != JSON_OBJECT || key == NULL)
		return NULL;

	for (size_t i = 0; i < value->value.list.count; i++) {
		if (value->value.list.keys[i] != NULL && strcmp(value->value.list.keys[i], key) == 0)
			return value->value.list.items[i];
	}

	return NULL;
}

const char *json_get_string(const json_value_t *value) {
	if (value == NULL || value->type != JSON_STRING)
		return NULL;
	return value->value.string;
}

bool json_get_number(const json_value_t *value, double *out) {
	if (value == NULL || value->type != JSON_NUMBER)
		return false;
	*out = value->value.number;
	return true;
}
