#include "effects.h"
#include "floating.h"
#include "ipc.h"
#include "layout.h"
#include "output.h"
#include "server.h"
#include "toplevel.h"
#include "transaction.h"
#include "tree.h"
#include "types.h"
#include "xwayland.h"
#include <stdlib.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#define FLOATING_DEFAULT_WIDTH 60
#define FLOATING_DEFAULT_HEIGHT 60

static bool node_outside_tree(node_t *n, desktop_t *d) {
	for (node_t *p = n; p != NULL; p = p->parent)
		if (p == d->root)
			return false;
	return true;
}

int desktop_toplevels(desktop_t *d, node_t ***out_nodes) {
	if (out_nodes != NULL)
		*out_nodes = NULL;
	if (d == NULL || out_nodes == NULL)
		return 0;

	int count = 0;
	if (d->root != NULL)
		FOR_EACH_LEAF(n, d->root)
			if (n->client != NULL)
				count++;

	toplevel_t *toplevel;
	wl_list_for_each(toplevel, &server.toplevels, link) {
		if (toplevel->mapped && toplevel->node != NULL && toplevel->node->client != NULL &&
			toplevel->node->desktop == d && node_outside_tree(toplevel->node, d))
			count++;
	}

	xwayland_toplevel_t *xwayland_view;
	wl_list_for_each(xwayland_view, &server.xwayland.views, link) {
		if (xwayland_view->mapped && xwayland_view->node != NULL && xwayland_view->node->client != NULL &&
			xwayland_view->node->desktop == d && node_outside_tree(xwayland_view->node, d))
			count++;
	}

	if (count == 0)
		return 0;

	node_t **nodes = calloc(count, sizeof(*nodes));
	if (nodes == NULL)
		return 0;

	int index = 0;
	if (d->root != NULL) {
		FOR_EACH_LEAF(n, d->root)
			if (n->client != NULL)
				nodes[index++] = n;
	}

	wl_list_for_each(toplevel, &server.toplevels, link) {
		if (toplevel->mapped && toplevel->node != NULL && toplevel->node->client != NULL &&
			toplevel->node->desktop == d && node_outside_tree(toplevel->node, d))
			nodes[index++] = toplevel->node;
	}

	wl_list_for_each(xwayland_view, &server.xwayland.views, link) {
		if (xwayland_view->mapped && xwayland_view->node != NULL && xwayland_view->node->client != NULL &&
			xwayland_view->node->desktop == d && node_outside_tree(xwayland_view->node, d))
			nodes[index++] = xwayland_view->node;
	}

	*out_nodes = nodes;
	return count;
}

bool desktop_has_toplevels(desktop_t *d) {
	if (d == NULL)
		return false;

	if (d->root != NULL) {
		FOR_EACH_LEAF(n, d->root)
			if (n->client != NULL)
				return true;
	}

	toplevel_t *toplevel;
	wl_list_for_each(toplevel, &server.toplevels, link) {
		if (toplevel->mapped && toplevel->node != NULL && toplevel->node->client != NULL &&
			toplevel->node->desktop == d)
			return true;
	}

	xwayland_toplevel_t *xwayland_view;
	wl_list_for_each(xwayland_view, &server.xwayland.views, link) {
		if (xwayland_view->mapped && xwayland_view->node != NULL && xwayland_view->node->client != NULL &&
			xwayland_view->node->desktop == d)
			return true;
	}

	return false;
}

void desktop_clear_output(desktop_t *d, output_t *m) {
	if (d == NULL)
		return;

	node_t **toplevels = NULL;
	int count = desktop_toplevels(d, &toplevels);
	if (toplevels == NULL)
		return;

	for (int i = 0; i < count; i++) {
		if (toplevels[i] != NULL)
			toplevels[i]->output = m;
	}

	free(toplevels);
}

struct wlr_box node_current_rect(node_t *n) {
	if (n == NULL || n->client == NULL)
		return (struct wlr_box){0};

	if (n->client->state == STATE_FLOATING)
		return n->client->floating_rectangle;
	if (n->client->state == STATE_FULLSCREEN && n->output != NULL)
		return n->output->rectangle;
	return n->client->tiled_rectangle;
}

// size the client asked for, falling back to a fraction of the desktop
static struct wlr_box client_requested_size(client_t *c, struct wlr_box area) {
	struct wlr_box size = {0};

	if (c->toplevel != NULL && c->toplevel->xdg_toplevel != NULL)
		size = c->toplevel->xdg_toplevel->base->geometry;
	else if (c->xwayland_view != NULL)
		size = c->xwayland_view->geometry;

	if (size.width > MIN_WIDTH && size.height > MIN_HEIGHT)
		return size;

	// the client has nothing usable to offer, take a bit more than half of the desktop
	struct wlr_box fallback = {
		.width = area.width * FLOATING_DEFAULT_WIDTH / 100,
		.height = area.height * FLOATING_DEFAULT_HEIGHT / 100,
	};

	if (fallback.width < MIN_WIDTH)
		fallback.width = MIN_WIDTH;
	if (fallback.height < MIN_HEIGHT)
		fallback.height = MIN_HEIGHT;
	if (fallback.width > area.width)
		fallback.width = area.width;
	if (fallback.height > area.height)
		fallback.height = area.height;

	if (size.width <= MIN_WIDTH)
		size.width = fallback.width;
	if (size.height <= MIN_HEIGHT)
		size.height = fallback.height;

	return size;
}

static struct wlr_box cascade_rect(struct wlr_box area, struct wlr_box size, int index) {
	if (size.width > area.width)
		size.width = area.width;
	if (size.height > area.height)
		size.height = area.height;
	if (size.width < MIN_WIDTH)
		size.width = MIN_WIDTH;
	if (size.height < MIN_HEIGHT)
		size.height = MIN_HEIGHT;
	if (index < 0)
		index = 0;

	int max_steps_x = size.width >= area.width ? 0 : (area.width - size.width) / FLOATING_CASCADE_STEP;
	int max_steps_y = size.height >= area.height ? 0 : (area.height - size.height) /
		FLOATING_CASCADE_STEP;

	return (struct wlr_box){
		.x = area.x + (area.width - size.width) / 2 + (index % (max_steps_x + 1)) * FLOATING_CASCADE_STEP,
		.y = area.y + (area.height - size.height) / 2 + (index % (max_steps_y + 1)) *
			FLOATING_CASCADE_STEP,
		.width = size.width,
		.height = size.height,
	};
}

static struct wlr_box float_area(output_t *m, desktop_t *d) {
	if (m == NULL)
		m = d ? d->output : mon;
	if (m == NULL)
		return (struct wlr_box){0};

	return desktop_usable_area(m, d);
}

// moves a toplevel to a floating geometry
static void apply_float_rect(node_t *n, struct wlr_box r) {
	client_t *c = n->client;
	struct wlr_scene_tree *st = client_get_scene_tree(c);
	if (st != NULL && (st->node.x != r.x || st->node.y != r.y))
		effects_dirty_corner_masks(n->output);

	c->floating_rectangle = r;
	c->arranged_rectangle = r;
	n->pending.rectangle = r;

	node_set_dirty(n);
}

// pushes a new floating geometry
void float_node_set_rect(node_t *n, struct wlr_box r) {
	if (n == NULL || n->client == NULL)
		return;

	if (client_is_maximized(n->client)) {
		wlr_log(WLR_DEBUG, "float_node_set_rect: ignoring, node %u is maximized", n->id);
		return;
	}

	struct wlr_scene_tree *scene_tree = client_get_scene_tree(n->client);
	apply_float_rect(n, r);
	if (scene_tree != NULL)
		wlr_scene_node_set_position(&scene_tree->node, r.x, r.y);
	if (n->client->toplevel != NULL)
		toplevel_center_and_clip_surface(n->client->toplevel);

	transaction_commit_dirty();
}

// brings a floating toplevel back into view when it is left outside of its output, for
// instance after it was sent to a desktop on another monitor
void float_node_clamp(output_t *m, desktop_t *d, node_t *n) {
	if (n == NULL || n->client == NULL || !IS_FLOATING(n->client))
		return;

	if (m == NULL)
		m = d != NULL ? d->output : n->output;
	if (m == NULL)
		return;

	if (wlr_box_intersects(&m->rectangle, &n->client->floating_rectangle))
		return;

	struct wlr_box available = desktop_usable_area(m, d);
	struct wlr_box size = n->client->floating_rectangle;
	if (size.width > available.width)
		size.width = available.width;
	if (size.height > available.height)
		size.height = available.height;

	float_node_set_rect(n, cascade_rect(available, size, 0));
}

// `announce` is false for windows that were never announced to ipc subscribers yet
static void float_node_impl(output_t *m, desktop_t *d, node_t *n, const struct wlr_box *rect,
		bool announce) {
	if (n == NULL || n->client == NULL)
		return;

	client_t *c = n->client;
	struct wlr_scene_tree *scene_tree = client_get_scene_tree(c);
	if (scene_tree == NULL) {
		wlr_log(WLR_ERROR, "float_node: node %u has no scene tree", n->id);
		return;
	}

	struct wlr_box target;
	if (rect != NULL) {
		target = *rect;
	} else if (IS_TILED(c) && c->tiled_rectangle.width > 0 && c->tiled_rectangle.height > 0) {
		// keep the place the toplevel had in the tree, sized to its own geometry
		target = c->tiled_rectangle;
		if (c->toplevel != NULL && c->toplevel->geometry.width > 0 && c->toplevel->geometry.height > 0) {
			int off_x = (target.width - c->toplevel->geometry.width) / 2;
			int off_y = (target.height - c->toplevel->geometry.height) / 2;
			target.width = c->toplevel->geometry.width;
			target.height = c->toplevel->geometry.height;
			target.x += off_x > 0 ? off_x : 0;
			target.y += off_y > 0 ? off_y : 0;
		}
	} else {
		node_t **toplevels = NULL;
		int index = d != NULL ? desktop_toplevels(d, &toplevels) : 0;
		free(toplevels);
		struct wlr_box area = float_area(m, d);
		target = cascade_rect(area, client_requested_size(c, area), index);
	}

	if (d != NULL && (n->parent != NULL || d->root == n)) {
		bool keep_focus = d->focus == n;
		remove_node(d, n);
		n->parent = NULL;

		if (keep_focus) {
			d->focus = n;
			focus_node(m, d, n);
		}
	}

	if (m != NULL)
		n->output = m;
	if (d != NULL)
		n->desktop = d;

	node_set_hidden(n, false);

	wlr_scene_node_reparent(&scene_tree->node, server.float_tree);
	wlr_scene_node_set_position(&scene_tree->node, target.x, target.y);
	apply_float_rect(n, target);

	c->flags.shown = true;
	wlr_scene_node_set_enabled(&scene_tree->node, true);

	if (announce) {
		set_state(m, d, n, STATE_FLOATING);
	} else {
		c->last_state = c->state;
		c->state = STATE_FLOATING;
		node_set_dirty(n);
	}

	if (c->toplevel != NULL)
		toplevel_center_and_clip_surface(c->toplevel);

	node_set_dirty(n);
	if (announce)
		transaction_commit_dirty();

	wlr_log(WLR_DEBUG, "float_node: node %u now floating at (%d,%d %dx%d)", n->id, target.x, target.y,
		target.width, target.height);
}

void float_node(output_t *m, desktop_t *d, node_t *n, const struct wlr_box *rect) {
	float_node_impl(m, d, n, rect, true);
}

void tile_node(output_t *m, desktop_t *d, node_t *n) {
	if (n == NULL || n->client == NULL || d == NULL)
		return;

	client_t *c = n->client;
	struct wlr_scene_tree *scene_tree = client_get_scene_tree(c);
	if (scene_tree == NULL) {
		wlr_log(WLR_ERROR, "tile_node: node %u has no scene tree", n->id);
		return;
	}

	if (c->state == STATE_FLOATING) {
		node_set_hidden(n, false);
		wlr_scene_node_reparent(&scene_tree->node, server.tile_tree);

		c->last_state = c->state;
		c->state = STATE_TILED;

		node_t *ref = d->focus != n ? d->focus : NULL;
		insert_node(d, n, ref);

		arrange(m, d, true);

		ipc_put_status(SUB_MASK_NODE_STATE, "node_state[%s,%s,%u,%c]\n", c->app_id[0] ? c->app_id : "?",
			c->title[0] ? c->title : "?", n->id, 'T');
	} else if (c->state == STATE_TILED) {
		if (c->toplevel != NULL) {
			toplevel_t *tl = c->toplevel;
			int off_x = (c->tiled_rectangle.width - tl->geometry.width) / 2;
			int off_y = (c->tiled_rectangle.height - tl->geometry.height) / 2;
			c->floating_rectangle = (struct wlr_box){
				.x = c->tiled_rectangle.x + (off_x > 0 ? off_x : 0),
				.y = c->tiled_rectangle.y + (off_y > 0 ? off_y : 0),
				.width = tl->geometry.width,
				.height = tl->geometry.height
			};
		} else
			c->floating_rectangle = c->tiled_rectangle;

		remove_node(d, n);
		node_set_hidden(n, false);

		wlr_scene_node_set_position(&scene_tree->node, c->floating_rectangle.x, c->floating_rectangle.y);
		wlr_scene_node_reparent(&scene_tree->node, server.float_tree);

		// restore focus
		focus_node(m, d, n);

		set_state(m, d, n, STATE_FLOATING);

		if (c->toplevel != NULL)
			toplevel_center_and_clip_surface(c->toplevel);

		node_set_dirty(n);
		transaction_commit_dirty();
	}
}

bool floating_init_client(output_t *m, desktop_t *d, client_t *c) {
	if (c == NULL || d == NULL)
		return false;

	node_t *n = c->toplevel != NULL ? c->toplevel->node : (c->xwayland_view != NULL ?
		c->xwayland_view->node : NULL);
	if (n == NULL)
		return false;

	// float the new toplevel right away
	node_t **toplevels = NULL;
	int index = desktop_toplevels(d, &toplevels);
	free(toplevels);

	struct wlr_box area = float_area(m, d);
	struct wlr_box rect = cascade_rect(area, client_requested_size(c, area), index);
	float_node_impl(m, d, n, &rect, false);

	return true;
}

void floating_enter(output_t *m, desktop_t *d) {
	if (d == NULL)
		return;

	node_t **toplevels = NULL;
	int count = desktop_toplevels(d, &toplevels);
	if (toplevels == NULL)
		return;

	node_t *focus = d->focus;
	node_t *first = count > 0 ? toplevels[0] : NULL;

	int index = 0;
	struct wlr_box area = float_area(m, d);
	for (int i = 0; i < count; i++) {
		node_t *n = toplevels[i];
		if (n->client == NULL || !IS_TILED(n->client))
			continue;

		struct wlr_box size = client_requested_size(n->client, area);
		struct wlr_box rect = cascade_rect(area, size, index++);
		float_node(m, d, n, &rect);
	}

	free(toplevels);

	arrange(m, d, true);

	if (focus != NULL && focus->client != NULL) {
		d->focus = focus;
		focus_node(m, d, focus);
	} else if (d->focus == NULL && first != NULL && first->client != NULL) {
		// the last window of the tree left, focus the first one
		d->focus = first;
		focus_node(m, d, first);
	}
}

// true if the toplevel is on any output, used to bring back toplevels left behind by an output
// that went away
static bool rect_on_any_output(struct wlr_box r) {
	output_t *m;
	wl_list_for_each(m, &mon_list, link)
		if (wlr_box_intersects(&m->rectangle, &r))
			return true;

	return false;
}

static struct wlr_box gapped_area(struct wlr_box area, int wg) {
	area.x += wg;
	area.y += wg;
	area.width -= wg;
	area.height -= wg;
	return area;
}

void floating_arrange(output_t *m, desktop_t *d, struct wlr_box available) {
	if (d == NULL)
		return;

	struct wlr_box usable = available;
	if (d->root != NULL) {
		available = gapped_area(available, compute_window_gap(d));
		apply_layout(m, d, d->root, available, available);
	}

	node_t **toplevels = NULL;
	int count = desktop_toplevels(d, &toplevels);
	if (toplevels == NULL)
		return;

	int index = 0;
	for (int i = 0; i < count; i++) {
		node_t *n = toplevels[i];
		client_t *c = n->client;

		if (c == NULL || c->state != STATE_FLOATING)
			continue;
		if (n->output != NULL && m != NULL && n->output != m)
			continue;

		// a maximized window fills the usable area, with the gaps and border still applied
		if (client_is_maximized(c)) {
			int wg = compute_window_gap(d);
			struct wlr_box maxed = apply_bleed(gapped_area(usable, wg), (int)effective_border_width(d), wg);
			apply_float_rect(n, maxed);
			n->output = m;
			continue;
		}

		struct wlr_box r = c->floating_rectangle;

		if (r.width < MIN_WIDTH || r.height < MIN_HEIGHT) {
			apply_float_rect(n, cascade_rect(available, client_requested_size(c, available), index));
		} else if (!rect_on_any_output(r)) {
			struct wlr_box size = {0};
			size.width = r.width < available.width ? r.width : available.width;
			size.height = r.height < available.height ? r.height : available.height;
			apply_float_rect(n, cascade_rect(available, size, 0));
		} else {
			apply_float_rect(n, r);
			continue;
		}

		index++;
	}

	free(toplevels);
}

void floating_on_focus(output_t *m, desktop_t *d, node_t *n) {
	(void)m;
	(void)n;
	if (d == NULL)
		return;

	node_t **toplevels = NULL;
	int count = desktop_toplevels(d, &toplevels);
	if (toplevels == NULL)
		return;

	for (int i = 0; i < count; i++) {
		node_t *w = toplevels[i];
		if (w->client == NULL || w->client->state == STATE_FULLSCREEN)
			continue;

		w->client->flags.shown = true;
		struct wlr_scene_tree *st = client_get_scene_tree(w->client);
		if (st != NULL)
			wlr_scene_node_set_enabled(&st->node, true);
	}

	free(toplevels);
}

bool floating_focus(desktop_t *d, direction_t dir) {
	if (d == NULL || d->focus == NULL)
		return false;

	node_t **toplevels = NULL;
	int count = desktop_toplevels(d, &toplevels);
	if (toplevels == NULL)
		return false;

	bool horiz = dir == DIR_WEST || dir == DIR_EAST;
	struct wlr_box from = node_current_rect(d->focus);
	node_t *best = NULL;
	long best_overlap = -1, best_dist = 0;

	// with fullscreen directions cycle through the toplevels instead
	if (d->focus->client != NULL && d->focus->client->state == STATE_FULLSCREEN) {
		for (int i = 0; i < count; i++) {
			node_t *n = toplevels[i];
			if (n == d->focus || n->client == NULL || n->client->state == STATE_FULLSCREEN)
				continue;
			if (dir == DIR_EAST || dir == DIR_SOUTH)
				best = n;
			else if (best == NULL)
				best = n;
		}

		free(toplevels);

		return best != NULL && focus_node(mon, d, best);
	}

	for (int i = 0; i < count; i++) {
		node_t *n = toplevels[i];
		if (n == d->focus || n->client == NULL || n->client->state == STATE_FULLSCREEN)
			continue;

		struct wlr_box r = node_current_rect(n);
		long lo, hi, dist;
		if (horiz) {
			lo = r.y > from.y ? r.y : from.y;
			hi = (r.y + r.height) < (from.y + from.height) ? (r.y + r.height) : (from.y + from.height);
			dist = dir == DIR_EAST ? r.x - (from.x + from.width) : from.x - (r.x + r.width);
		} else {
			lo = r.x > from.x ? r.x : from.x;
			hi = (r.x + r.width) < (from.x + from.width) ? (r.x + r.width) : (from.x + from.width);
			dist = dir == DIR_SOUTH ? r.y - (from.y + from.height) : from.y - (r.y + r.height);
		}

		// only toplevels strictly in that direction, overlapping on the other axis
		if (dist < 0 || hi - lo <= 0)
			continue;

		if (best == NULL || hi - lo > best_overlap || (hi - lo == best_overlap && dist < best_dist)) {
			best = n;
			best_overlap = hi - lo;
			best_dist = dist;
		}
	}

	// wrap around to the oldest toplevel behind us, or the newest one ahead of us
	if (best == NULL && settings.focus_wrapping) {
		node_t *wrapped = NULL;
		for (int i = 0; i < count; i++) {
			node_t *n = toplevels[i];
			if (n == d->focus || n->client == NULL || n->client->state == STATE_FULLSCREEN)
				continue;
			if (dir == DIR_EAST || dir == DIR_SOUTH)
				wrapped = n;
			else if (wrapped == NULL)
				wrapped = n;
		}
		best = wrapped;
	}

	free(toplevels);

	if (best == NULL)
		return false;

	return focus_node(mon, d, best);
}

bool floating_places_floating(desktop_t *d, client_t *c) {
	(void)d;
	return IS_FLOATING(c);
}

int floating_collect(desktop_t *d, node_t ***out_nodes) {
	if (out_nodes != NULL)
		*out_nodes = NULL;
	if (d == NULL || out_nodes == NULL)
		return 0;

	node_t **toplevels = NULL;
	int count = desktop_toplevels(d, &toplevels);
	if (toplevels == NULL)
		return 0;

	int floating = 0;
	for (int i = 0; i < count; i++)
		if (IS_FLOATING(toplevels[i]->client))
			floating++;

	if (floating == 0) {
		free(toplevels);
		return 0;
	}

	node_t **nodes = calloc(floating, sizeof(*nodes));
	if (nodes == NULL) {
		free(toplevels);
		return 0;
	}

	int index = 0;
	for (int i = 0; i < count; i++)
		if (IS_FLOATING(toplevels[i]->client))
			nodes[index++] = toplevels[i];

	free(toplevels);

	*out_nodes = nodes;
	return floating;
}
