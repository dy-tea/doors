#pragma once

#include "types.h"

#include <stdbool.h>
#include <wlr/util/box.h>

typedef struct output_t output_t;
typedef struct desktop_t desktop_t;
typedef struct client_t client_t;
typedef struct node_t node_t;

typedef struct layout_impl_t {
	const char *name;

	// called when an automatic arrange occurs (e.g. when output size changes)
	void (*arrange)(output_t *m, desktop_t *d, struct wlr_box available);

	// called when focus_node is called
	void (*on_focus)(output_t *m, desktop_t *d, node_t *n);

	// called when the focus bind is executed (returns true if focus succeeded)
	bool (*focus)(desktop_t *d, direction_t dir);

	// called when the swap bind is executed (returns true if swap succeeded)
	bool (*swap)(output_t *m, desktop_t *d, direction_t dir);

	// called when the layout becomes active on a desktop, to adopt the toplevels it finds
	void (*enter)(output_t *m, desktop_t *d);

	// called when a new client is mapped on a desktop using this layout (returns true if layout took
	// over client)
	bool (*init_client)(output_t *m, desktop_t *d, client_t *c);

	// get a list of nodes (returns count of nodes)
	int (*collect)(desktop_t *d, node_t ***out_nodes);

	// true if layout only shows a single toplevel at once
	bool single_visible;

	// true if layout supports directional navigation (nsew)
	bool has_directional_nav;
} layout_impl_t;

const layout_impl_t *layout_get_impl(layout_t layout);

void arrange(output_t *m, desktop_t *d, bool use_transaction);
void layout_set(desktop_t *d, layout_t new_layout);
void layout_toggle(desktop_t *d, layout_t target);
void layout_cycle(output_t *m, desktop_t *d, int direction);
bool layout_init_client(output_t *m, desktop_t *d, client_t *c);
