#pragma once

#include "types.h"
#include <stdbool.h>
#include <wlr/util/box.h>

typedef struct desktop_t desktop_t;
typedef struct output_t output_t;
typedef struct node_t node_t;

void master_stack_arrange(output_t *m, desktop_t *d, struct wlr_box available);
int master_stack_collect(desktop_t *d, node_t ***out_nodes);
bool master_stack_focus(desktop_t *d, direction_t direction);
bool master_stack_swap(output_t *m, desktop_t *d, direction_t direction);

bool master_stack_increment(desktop_t *d);
bool master_stack_decrement(desktop_t *d);
bool master_stack_promote(desktop_t *d);
bool master_stack_demote(desktop_t *d);
void master_stack_set_count(desktop_t *d, int count);
bool master_stack_adjust_ratio(desktop_t *d, float delta);
void master_stack_set_orientation(desktop_t *d, master_area_orientation_t orientation);
void master_stack_flip_orientation(desktop_t *d);
void master_stack_cycle_orientation(desktop_t *d);
void master_stack_cycle_stack_layout(desktop_t *d);
