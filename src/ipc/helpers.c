#include "ipc/helpers.h"
#include "protocol/workspace.h"
#include "server.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool streq(const char *a, const char *b) {
	return strcmp(a, b) == 0;
}

// parse a hex-digit character
static int ipc_parse_hex_digit(char c) {
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return 10 + (c - 'a');
	if (c >= 'A' && c <= 'F')
		return 10 + (c - 'A');
	return 0;
}

// parse a single RRGGBBAA or RRGGBB color into a float[4]
bool ipc_parse_color(const char *hex, float *rgba) {
	if (!hex)
		return false;
	if (*hex == '#')
		hex++;
	size_t len = strlen(hex);
	if (len != 6 && len != 8)
		return false;
	rgba[0] = (float)(ipc_parse_hex_digit(hex[0]) * 16 + ipc_parse_hex_digit(hex[1])) / 255.0f;
	rgba[1] = (float)(ipc_parse_hex_digit(hex[2]) * 16 + ipc_parse_hex_digit(hex[3])) / 255.0f;
	rgba[2] = (float)(ipc_parse_hex_digit(hex[4]) * 16 + ipc_parse_hex_digit(hex[5])) / 255.0f;
	rgba[3] = (len == 8) ? (float)(ipc_parse_hex_digit(hex[6]) * 16 + ipc_parse_hex_digit(hex[7])) /
		255.0f : 1.0f;
	return true;
}

bool ipc_parse_color_float(const char *str, float rgba[4]) {
	if (!str)
		return false;
	rgba[3] = 1.0f;
	int n = sscanf(str, "%f %f %f %f", &rgba[0], &rgba[1], &rgba[2], &rgba[3]);
	return n >= 3;
}

void ipc_format_color_float(char *buf, size_t bufsz, const float rgba[4]) {
	snprintf(buf, bufsz, "%.3f %.3f %.3f %.3f\n", rgba[0], rgba[1], rgba[2], rgba[3]);
}

bool ipc_parse_gradient(const char *str, float out[BORDER_GRADIENT_MAX_STOPS * 4], int *count,
		float *angle_out) {
	if (!str)
		return false;
	char buf[512];
	strncpy(buf, str, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	*count = 0;
	*angle_out = 0.0f;

	char *tok = strtok(buf, " \t");
	while (tok) {
		size_t tlen = strlen(tok);
		if (tlen > 3 && strcmp(tok + tlen - 3, "deg") == 0) {
			char tmp[16];
			strncpy(tmp, tok, sizeof(tmp) - 1);
			tmp[tlen - 3] = '\0';
			*angle_out = (float)atof(tmp) * 3.14159265f / 180.0f;
		} else if (tlen >= 6) {
			if (*count >= BORDER_GRADIENT_MAX_STOPS)
				break;
			float rgba[4];
			if (ipc_parse_color(tok, rgba)) {
				memcpy(out + (*count) * 4, rgba, 4 * sizeof(float));
				(*count)++;
			}
		}
		tok = strtok(NULL, " \t");
	}
	return (*count >= 1);
}

// serialise a gradient back to a string for IPC get queries.
void ipc_format_gradient(char *buf, size_t bufsz, const float *colors, int count, float angle) {
	ipc_buf_t b;
	ipc_buf_init(&b, buf, bufsz);
	for (int i = 0; i < count; i++) {
		unsigned r = (unsigned)(colors[i * 4 + 0] * 255.0f + 0.5f);
		unsigned g = (unsigned)(colors[i * 4 + 1] * 255.0f + 0.5f);
		unsigned bl = (unsigned)(colors[i * 4 + 2] * 255.0f + 0.5f);
		unsigned a = (unsigned)(colors[i * 4 + 3] * 255.0f + 0.5f);
		ipc_buff(&b, "%02x%02x%02x%02x ", r, g, bl, a);
	}
	ipc_buff(&b, "%ddeg", (int)(angle * 180.0f / 3.14159265f));
}

node_t *ipc_focused(ipc_args_t *a, output_t **out) {
	output_t *m = server.focused_output;
	desktop_t *d = m ? m->desk : NULL;
	node_t *n = d ? d->focus : NULL;

	if (out)
		*out = m;

	if (!d) {
		ipc_fail(a, "No focused desktop\n");
		return NULL;
	}
	if (!n) {
		ipc_fail(a, "No focused node\n");
		return NULL;
	}
	return n;
}

desktop_t *ipc_focused_desk(ipc_args_t *a, output_t **out) {
	output_t *m = server.focused_output;
	desktop_t *d = m ? m->desk : NULL;

	if (out)
		*out = m;

	if (!d) {
		ipc_fail(a, "No focused desktop\n");
		return NULL;
	}
	return d;
}

output_t *ipc_output_by_name(ipc_args_t *a, const char *name) {
	output_t *m = find_output_by_name(name);
	if (!m)
		ipc_fail(a, "Monitor \"%s\" not found\n", name);
	return m;
}

desktop_t *ipc_desktop_by_name(ipc_args_t *a, const char *name) {
	desktop_t *d = find_desktop_by_name(name);
	if (!d)
		ipc_fail(a, "Desktop \"%s\" not found\n", name);
	return d;
}
