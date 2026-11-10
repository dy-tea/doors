#pragma once

#include "ipc/args.h"
#include "ipc/ipc.h"
#include "output/output.h"
#include "types.h"

#include <stdbool.h>

void ipc_format_color_float(char *buf, size_t bufsz, const float rgba[4]);
void ipc_format_gradient(char *buf, size_t bufsz, const float *colors, int count, float angle);

void ipc_json_string(ipc_buf_t *b, const char *s);

node_t *ipc_focused(ipc_args_t *a, output_t **out);
node_t *ipc_focused_nonvacant(ipc_args_t *a, output_t **out);
desktop_t *ipc_focused_desk(ipc_args_t *a, output_t **out);
output_t *ipc_output_by_name(ipc_args_t *a, const char *name);
desktop_t *ipc_desktop_by_name(ipc_args_t *a, const char *name);
void ipc_presel_ratio(ipc_args_t *a);
void ipc_circulate(ipc_args_t *a);
