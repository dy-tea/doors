#pragma once

#include "settings.h"
#include "types.h"

#include <wlr/util/box.h>

typedef struct output_t output_t;
typedef struct desktop_t desktop_t;
typedef struct node_t node_t;

#define MIN_WIDTH 1
#define MIN_HEIGHT 1

// the area of a desktop that tiles may occupy, after output and desktop padding
struct wlr_box desktop_usable_area(output_t *m, desktop_t *d);

// writes pending rectangles for the subtree rooted at n, and each tiled leaf's arranged_rectangle
void apply_layout(output_t *m, desktop_t *d, node_t *n, struct wlr_box rect,
	struct wlr_box root_rect);

// resolves a single leaf's final rect (gaps, borders, min size, centring)
// from the slot the layout gave it
void render_leaf(output_t *m, desktop_t *d, node_t *n, struct wlr_box rect, struct wlr_box root_rect,
	bool omit_window_gap);

unsigned int node_area(node_t *n);

// tiles in a subtree, counting empty leaves as tiles when asked
int tiled_count(node_t *n, bool include_receptacles);

// number of tiled leaves currently on the desktop
int visible_tiled_count(desktop_t *d);

static inline int effective_border_width(desktop_t *d) {
	if (settings.smart_borders && d && visible_tiled_count(d) <= 1)
		return 0;
	return settings.border_width;
}

static inline int compute_window_gap(desktop_t *d) {
	if (settings.smart_gaps && visible_tiled_count(d) <= 1)
		return 0;
	return d->window_gap;
}

static inline struct wlr_box apply_bleed(struct wlr_box r, int bw, int wg) {
	int bleed = wg + 2 * bw;
	r.x += bw;
	r.y += bw;
	r.width = (bleed < r.width ? r.width - bleed : 0);
	r.height = (bleed < r.height ? r.height - bleed : 0);
	return r;
}
