#pragma once

#include "ipc/args.h"
#include "ipc/ipc.h"
#include "output/output.h"
#include "types.h"

#include <stdbool.h>

bool streq(const char *a, const char *b);

bool ipc_parse_color(const char *hex, float *rgba);
bool ipc_parse_color_float(const char *str, float rgba[4]);
void ipc_format_color_float(char *buf, size_t bufsz, const float rgba[4]);
bool ipc_parse_gradient(const char *str, float out[BORDER_GRADIENT_MAX_STOPS * 4], int *count,
	float *angle_out);
void ipc_format_gradient(char *buf, size_t bufsz, const float *colors, int count, float angle);

/*
 * Resolve the focused node, reporting the failure itself when there is no
 * focused desktop or node. `out` receives the focused output.
 */
node_t *ipc_focused(ipc_args_t *a, output_t **out);
desktop_t *ipc_focused_desk(ipc_args_t *a, output_t **out);
