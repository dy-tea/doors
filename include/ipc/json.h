#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
	JSON_NULL,
	JSON_BOOL,
	JSON_NUMBER,
	JSON_STRING,
	JSON_ARRAY,
	JSON_OBJECT,
} json_type_t;

typedef struct json_value json_value_t;

struct json_value {
	json_type_t type;
	union {
		bool boolean;
		double number;
		char *string; // JSON_STRING, owned
		struct {
			char **keys; // only set for JSON_OBJECT, owned
			json_value_t **items;
			size_t count;
		} list;
	} value;
};

// parses a NUL terminated document, returns NULL and fills err on failure
json_value_t *json_parse(const char *text, char *err, size_t errsz);
void json_free(json_value_t *value);

// object member, NULL when the value is no object or has no such member
const json_value_t *json_get(const json_value_t *value, const char *key);

// NULL when the value is not of the requested type
const char *json_get_string(const json_value_t *value);
bool json_get_number(const json_value_t *value, double *out);
