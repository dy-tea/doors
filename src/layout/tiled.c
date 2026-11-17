#include "layout/layout.h"
#include "layout/tiled.h"
#include "layout/tree.h"
#include "output/output.h"
#include "settings.h"
#include "tree.h"
#include "tree_focus.h"
#include "types.h"

static void tiled_arrange(output_t *m, desktop_t *d, struct wlr_box available) {
	const int wg = compute_window_gap(d);
	available.x += wg;
	available.y += wg;
	available.width -= wg;
	available.height -= wg;
	apply_layout(m, d, d->root, available, available);
}

// pick the leaf of `sub` that is adjacent to `from` in direction `dir`
static node_t *closest_leaf(node_t *sub, node_t *from, direction_t dir) {
	const bool horiz = dir == DIR_WEST || dir == DIR_EAST;
	struct wlr_box f = from->rectangle;
	node_t *best = NULL;
	long best_overlap = -1, best_dist = 0;

	FOR_EACH_LEAF(leaf, sub) {
		if (node_is_invisible(leaf))
			continue;

		const struct wlr_box r = leaf->rectangle;
		long lo, hi, dist;
		if (horiz) {
			lo = r.y > f.y ? r.y : f.y;
			hi = r.y + r.height < f.y + f.height ? r.y + r.height : f.y + f.height;
			dist = dir == DIR_EAST ? r.x : -(r.x + r.width);
		} else {
			lo = r.x > f.x ? r.x : f.x;
			hi = r.x + r.width < f.x + f.width ? r.x + r.width : f.x + f.width;
			dist = dir == DIR_SOUTH ? r.y : -(r.y + r.height);
		}
		const long overlap = hi - lo;
		if (best == NULL || overlap > best_overlap || (overlap == best_overlap && dist < best_dist)) {
			best = leaf;
			best_overlap = overlap;
			best_dist = dist;
		}
	}
	return best;
}

static node_t *focus_fence_leaf(node_t *from, const direction_t dir) {
	node_t *cursor = from;

	while (cursor != NULL) {
		node_t *sub = find_fence_from(cursor, dir);
		if (sub == NULL)
			return NULL;

		node_t *leaf = closest_leaf(sub, from, dir);
		if (leaf != NULL)
			return leaf;

		cursor = sub;
	}

	return NULL;
}

static node_t *wrapped_focus_leaf(const desktop_t *d, const direction_t dir) {
	if (!settings.focus_wrapping || d->root == NULL)
		return NULL;

	node_t *first = NULL, *last = NULL;
	FOR_EACH_LEAF(leaf, d->root) {
		if (node_is_invisible(leaf))
			continue;

		if (first == NULL)
			first = leaf;
		last = leaf;
	}

	node_t *w = dir == DIR_EAST || dir == DIR_SOUTH ? first : last;
	return w != NULL && w != d->focus ? w : NULL;
}

bool tiled_focus(desktop_t *d, const direction_t dir) {
	if (d->focus == NULL)
		return false;

	node_t *n = focus_fence_leaf(d->focus, dir);
	if (n != NULL)
		return focus_node(output_focused(), d, n);

	n = wrapped_focus_leaf(d, dir);
	if (n != NULL)
		return focus_node(output_focused(), d, n);

	return false;
}

static bool tiled_swap(output_t *m, desktop_t *d, direction_t dir) {
	if (d->focus == NULL)
		return false;

	node_t *n = focus_fence_leaf(d->focus, dir);
	if (n != NULL) {
		swap_nodes(m, d, d->focus, m, d, n);
		return true;
	}
	return false;
}

int tiled_collect(desktop_t *d, node_t ***out_nodes) {
	return collect_tiled_leaves(d, out_nodes);
}

const layout_impl_t tiled_impl = {
	.name = "tiled",
	.arrange = tiled_arrange,
	.on_focus = NULL,
	.focus = tiled_focus,
	.swap = tiled_swap,
	.enter = NULL,
	.leave = NULL,
	.init_client = NULL,
	.on_client_state = NULL,
	.places_floating = NULL,
	.collect = tiled_collect,
	.single_visible = false,
	.has_directional_nav = false,
};
