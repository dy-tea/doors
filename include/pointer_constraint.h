#pragma once

#include "types.h"
#include <wayland-server-core.h>

struct wlr_surface;

typedef struct {
	struct wlr_pointer_constraint_v1 *constraint;
	struct wl_listener set_region;
	struct wl_listener destroy;
} pointer_constraint_t;

void pointer_constrain(struct wlr_pointer_constraint_v1 *constraint);
void pointer_constraint_focus(struct wlr_surface *surface);
node_t *pointer_constraint_node(struct wlr_surface *surface);

void pointer_constraint_init(void);
void pointer_constraint_fini(void);
