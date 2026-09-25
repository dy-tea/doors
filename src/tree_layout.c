#include "settings.h"
#include "tabs.h"
#include "transaction.h"
#include "tree.h"
#include "tree_layout.h"
#include "types.h"
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

struct wlr_box desktop_usable_area(output_t *m, desktop_t *d) {
	if (m == NULL || d == NULL)
		return (struct wlr_box){0};

	struct wlr_box rect = m->usable_area;

	rect.x += m->padding.left + d->padding.left;
	rect.y += m->padding.top + d->padding.top;
	rect.width -= m->padding.left + d->padding.left + d->padding.right + m->padding.right;
	rect.height -= m->padding.top + d->padding.top + d->padding.bottom + m->padding.bottom;

	return rect;
}

void render_leaf(output_t *m, desktop_t *d, node_t *n, struct wlr_box rect, struct wlr_box root_rect,
		bool omit_window_gap) {
	if (n == NULL || n->client == NULL)
		return;

	unsigned int bw = effective_border_width(d);

	struct wlr_box r;
	struct wlr_box slot = {0};
	bool use_centering = false;

	if (IS_FLOATING(n->client)) {
		r = n->client->floating_rectangle;
	} else if (n->client->state == STATE_FULLSCREEN) {
		r = m->rectangle;
	} else if (d->layout == LAYOUT_MONOCLE && IS_TILED(n->client) && !omit_window_gap) {
		r = root_rect;
		slot = root_rect;
		use_centering = true;
		r = apply_bleed(r, bw, settings.gapless_monocle ? 0 : compute_window_gap(d));
	} else {
		r = rect;
		slot = rect;
		use_centering = true;

		int wg;
		if (omit_window_gap)
			wg = 0;
		else
			wg = (settings.gapless_monocle && d->layout == LAYOUT_MONOCLE) ? 0 : compute_window_gap(d);

		r = apply_bleed(r, bw, wg);
	}

	// pseudo tile
	if (n->client->state == STATE_PSEUDO_TILED) {
		if (r.width > n->client->floating_rectangle.width)
			r.width = n->client->floating_rectangle.width;
		if (r.height > n->client->floating_rectangle.height)
			r.height = n->client->floating_rectangle.height;
	}

	// clamp up to constraints and center within the tile slot
	if (settings.respect_tiled_min_size && use_centering) {
		if ((int)n->constraints.min_width > MIN_WIDTH && r.width < (int)n->constraints.min_width &&
				slot.width > 0) {
			r.x = slot.x + (slot.width - (int)n->constraints.min_width) / 2;
			r.width = n->constraints.min_width;
		}
		if ((int)n->constraints.min_height > MIN_HEIGHT && r.height < (int)n->constraints.min_height &&
				slot.height > 0) {
			r.y = slot.y + (slot.height - (int)n->constraints.min_height) / 2;
			r.height = n->constraints.min_height;
		}
	}

	if (r.width < MIN_WIDTH)
		r.width = MIN_WIDTH;
	if (r.height < MIN_HEIGHT)
		r.height = MIN_HEIGHT;

	n->client->arranged_rectangle = r;
}

static void validate_split_children(node_t *n) {
	bool first_valid = n->first_child != NULL && !n->first_child->freed && !n->first_child->destroying;
	bool second_valid = n->second_child != NULL && !n->second_child->freed &&
		!n->second_child->destroying;

	if (n->first_child != NULL && !first_valid) {
		wlr_log(WLR_ERROR, "Node %u has invalid/destroying/freed first_child pointer %p "
			"(destroying=%d), nulling", n->id, (void *)n->first_child,
				n->first_child && !n->first_child->freed ? n->first_child->destroying : -1);
		n->first_child = NULL;
	}

	if (n->second_child != NULL && !second_valid) {
		wlr_log(WLR_ERROR, "Node %u has invalid/destroying/freed second_child pointer %p "
			"(destroying=%d), nulling", n->id, (void *)n->second_child,
				n->second_child && !n->second_child->freed ? n->second_child->destroying : -1);
		n->second_child = NULL;
	}
}

static bool repair_split_node(node_t *n, desktop_t *d, output_t *m, struct wlr_box rect,
		struct wlr_box root_rect) {
	bool first_ok = n->first_child != NULL;
	bool second_ok = n->second_child != NULL;

	if ((first_ok && !second_ok) || (!first_ok && second_ok)) {
		node_t *valid = first_ok ? n->first_child : n->second_child;
		wlr_log(WLR_ERROR, "apply_layout: node %u has only one valid child - "
			"promoting child %u; this indicates a tree inconsistency that "
			"should have been resolved by remove_node", n->id, valid->id);

		if (n->parent != NULL) {
			if (is_first_child(n))
				n->parent->first_child = valid;
			else
				n->parent->second_child = valid;
			valid->parent = n->parent;
		} else {
			d->root = valid;
			valid->parent = NULL;
		}

		n->first_child = NULL;
		n->second_child = NULL;
		n->parent = NULL;

		apply_layout(m, d, valid, rect, root_rect);
		return true;
	}
	return false;
}

static void split_dimension(int total, double split_ratio, uint16_t first_min, uint16_t second_min,
		int *first_out, int *second_out, int *offset_out) {
	int fence = (int)(split_ratio * total);

	if (first_min + second_min <= total) {
		if (fence < first_min)
			fence = first_min;
		else if (fence > total - second_min)
			fence = total - second_min;
	} else if (total >= 2) {
		fence = total / 2;
	} else if (total == 1) {
		fence = 1;
	} else {
		fence = 0;
	}

	*first_out = fence;
	*offset_out += fence;
	*second_out = total - fence;
	if (*second_out < 0)
		*second_out = 0;
}

static void compute_split_rects(node_t *n, desktop_t *d, struct wlr_box rect,
		struct wlr_box *first_rect, struct wlr_box *second_rect) {
	bool first_fullscreen = n->first_child && n->first_child->client &&
		n->first_child->client->state == STATE_FULLSCREEN;
	bool second_fullscreen = n->second_child && n->second_child->client &&
		n->second_child->client->state == STATE_FULLSCREEN;
	bool first_hidden = n->first_child && n->first_child->hidden;
	bool second_hidden = n->second_child && n->second_child->hidden;

	if (d->layout == LAYOUT_MONOCLE) {
		*first_rect = rect;
		*second_rect = rect;
		return;
	}

	if ((first_hidden || first_fullscreen) && n->second_child && !(second_hidden ||
			second_fullscreen)) {
		*first_rect = (struct wlr_box){0};
		*second_rect = rect;
		return;
	}

	if ((second_hidden || second_fullscreen) && n->first_child && !(first_hidden ||
			first_fullscreen)) {
		*first_rect = rect;
		*second_rect = (struct wlr_box){0};
		return;
	}

	*first_rect = rect;
	*second_rect = rect;

	uint16_t first_min_w = settings.respect_tiled_min_size &&
		n->first_child ? n->first_child->constraints.min_width : 0;
	uint16_t second_min_w = settings.respect_tiled_min_size &&
		n->second_child ? n->second_child->constraints.min_width : 0;
	uint16_t first_min_h = settings.respect_tiled_min_size &&
		n->first_child ? n->first_child->constraints.min_height : 0;
	uint16_t second_min_h = settings.respect_tiled_min_size &&
		n->second_child ? n->second_child->constraints.min_height : 0;

	if (n->split_type == TYPE_VERTICAL) {
		split_dimension(rect.width, n->split_ratio, first_min_w, second_min_w, &first_rect->width,
			&second_rect->width, &second_rect->x);
	} else {
		split_dimension(rect.height, n->split_ratio, first_min_h, second_min_h, &first_rect->height,
			&second_rect->height, &second_rect->y);
	}
}

void apply_layout(output_t *m, desktop_t *d, node_t *n, struct wlr_box rect,
		struct wlr_box root_rect) {
	if (n == NULL)
		return;

	// skip hidden or floating nodes from tiled layout
	if (n->hidden)
		return;
	if (n->client && n->client->state == STATE_FLOATING)
		return;

	// set pending
	n->pending.rectangle = rect;
	n->output = m;
	node_set_dirty(n);

	if (is_leaf(n)) {
		if (n->client == NULL) {
			wlr_log(WLR_ERROR, "apply_layout: node %u has NULL client, returning early", n->id);
			return;
		}

		render_leaf(m, d, n, rect, root_rect, false);
	} else if (n->split_type == TYPE_TABBED && d->layout != LAYOUT_MONOCLE) {
		tabs_arrange_group(m, d, n, rect, root_rect);
		return;
	} else {
		struct wlr_box first_rect;
		struct wlr_box second_rect;

		validate_split_children(n);

		if (repair_split_node(n, d, m, rect, root_rect))
			return;

		compute_split_rects(n, d, rect, &first_rect, &second_rect);

		apply_layout(m, d, n->first_child, first_rect, root_rect);
		apply_layout(m, d, n->second_child, second_rect, root_rect);
	}
}

// tiles a desktop shows, optionally counting empty leaves as tiles too
int tiled_count(node_t *n, bool include_receptacles) {
	if (n == NULL)
		return 0;

	if (is_leaf(n)) {
		if (n->client == NULL)
			return include_receptacles ? 1 : 0;

		return IS_TILED(n->client) ? 1 : 0;
	}

	return tiled_count(n->first_child, include_receptacles) + tiled_count(n->second_child,
		include_receptacles);
}

int visible_tiled_count(desktop_t *d) {
	if (!d || !d->root)
		return 0;
	return tiled_count(d->root, false);
}

unsigned int node_area(node_t *n) {
	if (n == NULL)
		return 0;

	return n->rectangle.width * n->rectangle.height;
}
