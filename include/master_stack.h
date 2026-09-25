#pragma once

#include "tree.h"
#include <stdbool.h>
#include <wlr/util/box.h>

typedef enum {
	MASTER_LEFT,
	MASTER_TOP,
	MASTER_RIGHT,
	MASTER_BOTTOM,
	MASTER_CENTER,
} master_area_orientation_t;

typedef enum {
	STACK_VERTICAL,
	STACK_HORIZONTAL,
} stack_layout_t;

void master_stack_arrange(struct output_t *m, desktop_t *d, struct wlr_box available);
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
