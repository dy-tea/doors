#include "ipc/helpers.h"
#include "protocol/workspace.h"
#include "server.h"
#include "transaction.h"
#include "tree.h"
#include "tree_focus.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// parse a single RRGGBBAA or RRGGBB color into a float[4]
bool ipc_parse_color(const char *hex, float *rgba) {
	return str_parse_color(hex, rgba);
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
		if (str_ends(tok, "deg")) {
			char tmp[16];
			size_t tlen = str_copy(tmp, sizeof(tmp), tok);
			if (tlen >= 3) {
				tmp[tlen - 3] = '\0';
				*angle_out = (float)atof(tmp) * 3.14159265f / 180.0f;
			}
		} else if (strlen(tok) >= 6) {
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

void ipc_json_string(ipc_buf_t *b, const char *s) {
	ipc_buff(b, "\"");

	if (s != NULL) {
		for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
			switch (*p) {
			case '"':
				ipc_buff(b, "\\\"");
				break;
			case '\\':
				ipc_buff(b, "\\\\");
				break;
			case '\n':
				ipc_buff(b, "\\n");
				break;
			case '\r':
				ipc_buff(b, "\\r");
				break;
			case '\t':
				ipc_buff(b, "\\t");
				break;
			default:
				if (*p < 0x20)
					ipc_buff(b, "\\u%04x", *p);
				else
					ipc_buff(b, "%c", *p);
			}
		}
	}

	ipc_buff(b, "\"");
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

// like ipc_focused(), but a receptacle has no client to act on
node_t *ipc_focused_nonvacant(ipc_args_t *a, output_t **out) {
	node_t *n = ipc_focused(a, out);
	if (!n)
		return NULL;
	if (n->vacant) {
		ipc_fail(a, "Focused node is a receptacle\n");
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

// shared by "node -o" and "tree presel_ratio"
void ipc_presel_ratio(ipc_args_t *a) {
	double rat;
	if (!ipc_double(a, "ratio", 0, 1, &rat))
		return;
	if (rat <= 0 || rat >= 1) {
		ipc_fail(a, "Ratio must be between 0 and 1\n");
		return;
	}

	output_t *m;
	node_t *n = ipc_focused_nonvacant(a, &m);
	if (!n)
		return;

	if (!n->presel)
		n->presel = make_presel();
	n->presel->split_ratio = rat;

	transaction_commit_dirty();
	ipc_ok(a, "Presel ratio set\n");
}

static const str_pair_t circulate_values[] = {
	{"forward", 1},
	{"f", 1},
	{"backward", -1},
	{"back", -1},
	{"b", -1},
	IPC_ENUM_END,
};

// shared by "node -C" and "tree circulate"
void ipc_circulate(ipc_args_t *a) {
	long value;
	if (!ipc_enum(a, "direction", circulate_values, &value))
		return;

	output_t *m;
	node_t *n = ipc_focused(a, &m);
	if (!n)
		return;

	node_t *next = value > 0 ? next_leaf(n, m->desk->root) : prev_leaf(n, m->desk->root);
	if (next) {
		m->desk->focus = next;
		focus_node(m, m->desk, next);
	}

	ipc_ok(a, "Circulated\n");
}
