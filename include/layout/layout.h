#pragma once

#include "str.h"
#include "types.h"

#include <stdbool.h>
#include <stdint.h>
#include <wlr/util/box.h>

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

	// called when the layout stops being active, so it can drop its per-desktop state
	void (*leave)(output_t *m, desktop_t *d);

	// called when a new client is mapped on a desktop using this layout (returns true if layout took
	// over client)
	bool (*init_client)(output_t *m, desktop_t *d, client_t *c);

	// called after a new client has been placed on the desktop and before it is focused
	void (*client_mapped)(output_t *m, desktop_t *d, client_t *c, bool focused);

	// called before a client is removed from the desktop
	void (*client_unmapped)(output_t *m, desktop_t *d, client_t *c);

	// called after a client was removed from the desktop and the desktop was rearranged; return
	// true when the layout picked the next focus itself
	bool (*focus_after_unmap)(output_t *m, desktop_t *d, node_t *removed);

	// called after a client changes state (fullscreen, maximized, minimized, tiled/floating)
	// and before the desktop is rearranged
	void (*on_client_state)(output_t *m, desktop_t *d, node_t *n, client_state_t old_state);

	// called when the resize bind is executed; return true if the layout resized the focused
	// toplevel itself, false to resize the split tree
	bool (*resize)(output_t *m, desktop_t *d, direction_t dir, double delta);

	// true when this layout places floating clients itself, so a floating client that gets
	// maximized stays floating instead of being pulled into the split tree
	bool (*places_floating)(desktop_t *d, client_t *c);

	// true when the layout wants node `n` shown while its desktop is visible (NULL shows every
	// node)
	bool (*wants_visible)(const desktop_t *d, const node_t *n);

	// true when `c` should be drawn without a border
	bool (*borderless)(const desktop_t *d, const client_t *c);

	// true when `c` counts as maximized even though the client does not report it
	bool (*reports_maximized)(const desktop_t *d, const client_t *c);

	// window gap around the layout's tiled leaves; NULL uses the global window gap
	int (*window_gap)(const desktop_t *d);

	// allowed edges for a pointer resize of `n`; return false to use the split tree's edges
	bool (*resize_edges)(const desktop_t *d, const node_t *n, uint32_t *edges);

	// called when a pointer resize of `n` starts; return true when the layout owns the drag and
	// write the size the drag starts from to `initial`
	bool (*pointer_resize_begin)(output_t *m, desktop_t *d, node_t *n, double *initial);

	// called with the pointer delta since the grab started; return true when the layout handled it
	bool (*pointer_resize_update)(output_t *m, desktop_t *d, node_t *n, double delta);

	// fraction of the viewport that has to be scrolled to bring `n` into view, so focus follows
	// mouse does not jump to a toplevel that is still off screen
	double (*focus_scroll_distance)(const desktop_t *d, const node_t *n);

	// number of fingers the layout consumes for panning its view, 0 when it does not pan
	uint32_t (*pan_fingers)(const desktop_t *d);

	// pan the layout's view by `delta`; return true when the layout handled it
	bool (*pan_update)(output_t *m, desktop_t *d, double delta);

	// finish a pan, snapping the layout's view; return true when the layout handled it
	bool (*pan_end)(output_t *m, desktop_t *d);

	// get a list of nodes (returns count of nodes)
	int (*collect)(desktop_t *d, node_t ***out_nodes);

	// true if layout only shows a single toplevel at once
	bool single_visible;

	// true if layout supports directional navigation (nsew)
	bool has_directional_nav;
} layout_impl_t;

void layout_init(void);
layout_t layout_register(const layout_impl_t *impl);
size_t layout_count(void);
const char *layout_name(layout_t layout);
bool layout_find_by_name(const char *name, layout_t *out);
const str_pair_t *layout_ipc_values(void);

const layout_impl_t *layout_get_impl(layout_t layout);

// per-desktop layout state
void *layout_state_get(desktop_t *d, layout_t layout, size_t size, void (*destroy)(void *state));

// state stored on the desktop or NULL
void *layout_state_peek(const desktop_t *d, layout_t layout);

// drops state stored on the desktop if any
void layout_state_drop(desktop_t *d, layout_t layout);

// drops every layout state of the desktop
void layout_states_clear(desktop_t *d);

void arrange(output_t *m, desktop_t *d, bool use_transaction);
void layout_set(desktop_t *d, layout_t new_layout);
void layout_toggle(desktop_t *d, layout_t target);
bool layout_init_client(output_t *m, desktop_t *d, client_t *c);

// tells the active layout that one of its clients changed state
void layout_client_state_changed(output_t *m, desktop_t *d, node_t *n, client_state_t old_state);

// reconciles every client on the desktop with the layout that is now active
void layout_desktop_changed(output_t *m, desktop_t *d);

// tells the active layout that a client was placed on the desktop
void layout_client_mapped(output_t *m, desktop_t *d, client_t *c, bool focused);

// tells the active layout that a client is about to be removed from the desktop
void layout_client_unmapped(output_t *m, desktop_t *d, client_t *c);

// lets the active layout pick the next focus after a removal (true when it did)
bool layout_focus_after_unmap(output_t *m, desktop_t *d, node_t *removed);

// lets the active layout resize the focused toplevel (true when it did)
bool layout_resize(output_t *m, desktop_t *d, direction_t dir, double delta);

// true unless the active layout hides `n` from a visible desktop
bool layout_wants_visible(const desktop_t *d, const node_t *n);

// true when the active layout draws `c` without a border
bool layout_borderless(const desktop_t *d, const client_t *c);

// true when the active layout reports `c` as maximized
bool layout_reports_maximized(const desktop_t *d, const client_t *c);

// window gap the active layout wants around tiled leaves
int layout_window_gap(const desktop_t *d);

// true when the active layout shows a single toplevel at once
bool layout_single_visible(const desktop_t *d);

// allowed pointer resize edges for `n` (true when the layout provided them)
bool layout_resize_edges(const desktop_t *d, const node_t *n, uint32_t *edges);

// pointer resize hooks
bool layout_pointer_resize_begin(output_t *m, desktop_t *d, node_t *n, double *initial);
bool layout_pointer_resize_update(output_t *m, desktop_t *d, node_t *n, double delta);

// scroll distance to bring `n` into view, used by focus follows mouse
double layout_focus_scroll_distance(const desktop_t *d, const node_t *n);

// pan hooks
uint32_t layout_pan_fingers(const desktop_t *d);
bool layout_pan_update(output_t *m, desktop_t *d, double delta);
bool layout_pan_end(output_t *m, desktop_t *d);
