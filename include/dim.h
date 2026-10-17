#pragma once

#include "types.h"

struct output_t;
struct view_t;

// recompute scrims of output
void dim_update_output(struct output_t *output);

// recompute scrims for every output
void dim_update(void);

void dim_update_view(struct view_t *view);

// release scrims owned by view or output
void dim_destroy_view(struct view_t *view);
void dim_destroy_output(struct output_t *output);

bool dim_output_visible(const struct output_t *output);
