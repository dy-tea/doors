#pragma once

#include "types.h"

#include <stdbool.h>
#include <wlr/util/box.h>

typedef struct output_t output_t;
typedef struct desktop_t desktop_t;
typedef struct node_t node_t;
typedef struct client_t client_t;

// distance between two toplevels of the same cascade, in logical pixels
#define FLOATING_CASCADE_STEP 32

// layout impl
void floating_enter(output_t *m, desktop_t *d);
bool floating_init_client(output_t *m, desktop_t *d, client_t *c);
void floating_arrange(output_t *m, desktop_t *d, struct wlr_box available);
void floating_on_focus(output_t *m, desktop_t *d, node_t *n);
bool floating_focus(desktop_t *d, direction_t dir);
int floating_collect(desktop_t *d, node_t ***out_nodes);
bool floating_places_floating(desktop_t *d, client_t *c);

// toplevel state transitions
void float_node(output_t *m, desktop_t *d, node_t *n, const struct wlr_box *rect);
void tile_node(output_t *m, desktop_t *d, node_t *n);

// moves or resizes a floating toplevel
void float_node_set_rect(node_t *n, struct wlr_box r);

// pulls a floating toplevel back into view when it is left outside of its output
void float_node_clamp(output_t *m, desktop_t *d, node_t *n);

// toplevels of a desktop
int desktop_toplevels(desktop_t *d, node_t ***out_nodes);

// true when the desktop holds any toplevel
bool desktop_has_toplevels(desktop_t *d);

// clears the output of every toplevel of a desktop
void desktop_clear_output(desktop_t *d, output_t *m);

struct wlr_box node_current_rect(node_t *n);
