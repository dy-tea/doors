#include "client.h"
#include "floating.h"
#include "layout.h"
#include "master_stack.h"
#include "scroller.h"
#include "toplevel.h"
#include "transaction.h"
#include "tree.h"
#include "tree_layout.h"
#include "types.h"
#include <stdlib.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

void arrange(output_t *m, desktop_t *d, bool use_transaction) {
	if (d == NULL)
		return;

	if (m != NULL) {
		struct wlr_box rect = desktop_usable_area(m, d);

		const layout_impl_t *impl = layout_get_impl(d->layout);
		if (impl && impl->arrange)
			impl->arrange(m, d, rect);
	}

	if (use_transaction)
		transaction_commit_dirty();
}

static void tiled_arrange(output_t *m, desktop_t *d, struct wlr_box available) {
	int wg = compute_window_gap(d);
	available.x += wg;
	available.y += wg;
	available.width -= wg;
	available.height -= wg;
	apply_layout(m, d, d->root, available, available);
}

static void monocle_arrange(output_t *m, desktop_t *d, struct wlr_box available) {
	available.x += settings.monocle_padding.left;
	available.y += settings.monocle_padding.top;
	available.width -= settings.monocle_padding.left + settings.monocle_padding.right;
	available.height -= settings.monocle_padding.top + settings.monocle_padding.bottom;
	if (!settings.gapless_monocle) {
		int wg = compute_window_gap(d);
		available.x += wg;
		available.y += wg;
		available.width -= wg;
		available.height -= wg;
	}
	apply_layout(m, d, d->root, available, available);
}

// monocle keeps exactly one window on screen, so focusing reveals n and hides
// every other leaf on the desktop
static void monocle_on_focus(output_t *m, desktop_t *d, node_t *n) {
	(void)m;
	if (!d->root)
		return;

	FOR_EACH_LEAF(node, d->root)
		client_set_visible(node->client, node == n);
}

static void monocle_on_client_state(output_t *m, desktop_t *d, node_t *n, client_state_t old) {
	(void)n;
	(void)old;

	if (d->root == NULL)
		return;

	node_t *reveal = d->focus;
	FOR_EACH_LEAF(node, d->root) {
		if (node->client != NULL && (client_is_maximized(node->client) ||
				node->client->state == STATE_FULLSCREEN)) {
			reveal = node;
			break;
		}
	}

	FOR_EACH_LEAF(node, d->root)
		client_set_visible(node->client, node == reveal);

	if (m != NULL && reveal != NULL && reveal->client != NULL && reveal->client->toplevel != NULL &&
		reveal->client->toplevel->configured)
		arrange(m, d, true);
}

static void scroller_on_focus(output_t *m, desktop_t *d, node_t *n) {
	if (!d)
		return;

	if (!d->scroller_state)
		d->scroller_state = scroller_create();

	scroller_state_t *s = d->scroller_state;
	if (!s)
		return;

	if (n != NULL && n->client != NULL)
		scroller_sync_focus(s, n->client);

	if (s->column_count == 0 && d->root) {
		FOR_EACH_LEAF(leaf, d->root)
			if (leaf->client && !node_is_minimized(leaf))
				leaf->client->flags.shown = true;
	} else {
		for (int i = 0; i < s->column_count; i++) {
			for (int j = 0; j < s->columns[i].tile_count; j++) {
				client_t *c = s->columns[i].tiles[j].client;
				if (c && !c->flags.minimized)
					c->flags.shown = true;
			}
		}
	}

	if (n != NULL && n->client->toplevel && n->client->toplevel->configured) {
		wlr_log(WLR_DEBUG, "scroller_on_focus: triggering arrange");
		arrange(m, d, true);
	} else {
		wlr_log(WLR_DEBUG, "scroller_on_focus: skipping arrange (initial map)");
	}
}

static void scroller_on_client_state(output_t *m, desktop_t *d, node_t *n, client_state_t old) {
	(void)m;
	(void)old;

	if (n == NULL || n->client == NULL)
		return;

	scroller_set_maximized(d, n, client_is_maximized(n->client));
}

static int tiled_collect(desktop_t *d, node_t ***out_nodes) {
	return collect_tiled_leaves(d, out_nodes);
}

static int scroller_collect_fn(desktop_t *d, node_t ***out_nodes) {
	return scroller_collect(d, out_nodes);
}

static bool scroller_focus(desktop_t *d, direction_t dir) {
	switch (dir) {
	case DIR_WEST:
		return scroller_focus_prev(d);
	case DIR_EAST:
		return scroller_focus_next(d);
	case DIR_NORTH:
		return scroller_focus_up(d);
	case DIR_SOUTH:
		return scroller_focus_down(d);
	}
	return false;
}

// pick the leaf of `sub` that is adjacent to `from` in direction `dir`
static node_t *closest_leaf(node_t *sub, node_t *from, direction_t dir) {
	bool horiz = dir == DIR_WEST || dir == DIR_EAST;
	struct wlr_box f = from->rectangle;
	node_t *best = NULL;
	long best_overlap = -1, best_dist = 0;

	FOR_EACH_LEAF(leaf, sub) {
		if (node_is_invisible(leaf))
			continue;

		struct wlr_box r = leaf->rectangle;
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
		long overlap = hi - lo;
		if (best == NULL || overlap > best_overlap || (overlap == best_overlap && dist < best_dist)) {
			best = leaf;
			best_overlap = overlap;
			best_dist = dist;
		}
	}
	return best;
}

static node_t *focus_fence_leaf(node_t *from, direction_t dir) {
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

static node_t *wrapped_focus_leaf(desktop_t *d, direction_t dir) {
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

static bool tiled_focus(desktop_t *d, direction_t dir) {
	if (d->focus == NULL)
		return false;

	node_t *n = focus_fence_leaf(d->focus, dir);
	if (n != NULL)
		return focus_node(mon, d, n);

	n = wrapped_focus_leaf(d, dir);
	if (n != NULL)
		return focus_node(mon, d, n);

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

static const layout_impl_t tiled_impl = {
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

static const layout_impl_t monocle_impl = {
	.name = "monocle",
	.arrange = monocle_arrange,
	.on_focus = monocle_on_focus,
	.focus = tiled_focus,
	.swap = NULL,
	.enter = NULL,
	.leave = NULL,
	.init_client = NULL,
	.on_client_state = monocle_on_client_state,
	.places_floating = NULL,
	.collect = tiled_collect,
	.single_visible = true,
	.has_directional_nav = false,
};

static const layout_impl_t scroller_impl = {
	.name = "scroller",
	.arrange = scroller_arrange,
	.on_focus = scroller_on_focus,
	.focus = scroller_focus,
	.swap = scroller_swap,
	.enter = NULL,
	.leave = scroller_leave,
	.init_client = NULL,
	.on_client_state = scroller_on_client_state,
	.places_floating = NULL,
	.collect = scroller_collect_fn,
	.single_visible = false,
	.has_directional_nav = true,
};

static const layout_impl_t master_stack_impl = {
	.name = "master_stack",
	.arrange = master_stack_arrange,
	.on_focus = NULL,
	.focus = master_stack_focus,
	.swap = master_stack_swap,
	.enter = NULL,
	.leave = NULL,
	.init_client = NULL,
	.on_client_state = NULL,
	.places_floating = NULL,
	.collect = master_stack_collect,
	.single_visible = false,
	.has_directional_nav = true,
};

static const layout_impl_t floating_impl = {
	.name = "floating",
	.arrange = floating_arrange,
	.on_focus = floating_on_focus,
	.focus = floating_focus,
	.swap = NULL,
	.enter = floating_enter,
	.leave = NULL,
	.init_client = floating_init_client,
	.on_client_state = NULL,
	.places_floating = floating_places_floating,
	.collect = floating_collect,
	.single_visible = false,
	.has_directional_nav = true,
};

static const layout_impl_t *registry[] = {
	[LAYOUT_TILED] = &tiled_impl,
	[LAYOUT_MONOCLE] = &monocle_impl,
	[LAYOUT_SCROLLER] = &scroller_impl,
	[LAYOUT_MASTER_STACK] = &master_stack_impl,
	[LAYOUT_FLOATING] = &floating_impl,
};

const layout_impl_t *layout_get_impl(layout_t layout) {
	if ((size_t)layout >= sizeof(registry) / sizeof(registry[0]))
		return NULL;
	return registry[layout];
}

bool layout_init_client(output_t *m, desktop_t *d, client_t *c) {
	if (!d || !c)
		return false;

	const layout_impl_t *impl = layout_get_impl(d->layout);
	if (impl && impl->init_client)
		return impl->init_client(m, d, c);

	return false;
}

void layout_client_state_changed(output_t *m, desktop_t *d, node_t *n, client_state_t old_state) {
	if (d == NULL)
		return;

	const layout_impl_t *impl = layout_get_impl(d->layout);
	if (impl != NULL && impl->on_client_state != NULL)
		impl->on_client_state(m, d, n, old_state);
}

void layout_desktop_changed(output_t *m, desktop_t *d) {
	if (d == NULL)
		return;

	layout_client_state_changed(m, d, NULL, STATE_TILED);
}

void layout_set(desktop_t *d, layout_t new_layout) {
	if (d == NULL || new_layout >= sizeof(registry) / sizeof(registry[0]) ||
		registry[new_layout] == NULL)
		return;

	layout_t old_layout = d->layout;

	if (old_layout == new_layout) {
		layout_desktop_changed(d->output ? d->output : mon, d);
		return;
	}

	d->user_layout = old_layout;
	output_t *m = d->output != NULL ? d->output : mon;

	// let the outgoing layout drop the per-desktop state it owns
	const layout_impl_t *old_impl = layout_get_impl(old_layout);
	if (m != NULL && old_impl != NULL && old_impl->leave != NULL)
		old_impl->leave(m, d);

	d->layout = new_layout;

	// let the incoming layout adopt the toplevels already on the desktop
	const layout_impl_t *impl = registry[new_layout];
	if (m != NULL && impl != NULL && impl->enter != NULL)
		impl->enter(m, d);

	layout_desktop_changed(m, d);
}

void layout_toggle(desktop_t *d, layout_t target) {
	if (d == NULL)
		return;

	layout_set(d, d->layout == target ? d->user_layout : target);
}

void layout_cycle(output_t *m, desktop_t *d, int direction) {
	if (d == NULL)
		return;

	int num_layouts = sizeof(registry) / sizeof(registry[0]);
	int current = (int)d->layout;
	int next = (current + direction) % num_layouts;
	if (next < 0)
		next += num_layouts;

	layout_set(d, (layout_t)next);
	arrange(m, d, true);
	if (d->focus)
		focus_node(m, d, d->focus);
}
