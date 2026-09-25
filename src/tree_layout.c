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

	if (rect.width < 1 || rect.height < 1) {
		n->client->arranged_rectangle = (struct wlr_box){0};
		return;
	}

	if (IS_FLOATING(n->client)) {
		r = n->client->floating_rectangle;
	} else if (n->client->state == STATE_FULLSCREEN) {
		r = m->rectangle;
	} else if (client_is_maximized(n->client)) {
		r = root_rect;
		slot = root_rect;
		use_centering = true;
		r = apply_bleed(r, bw, omit_window_gap ? 0 : compute_window_gap(d));
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

// what a split child does with the slot the layout hands it
typedef enum {
	CHILD_ACTIVE, // takes part in the split normally
	CHILD_HIDDEN, // slot collapses
	CHILD_EXPANDED,// covers the output, fills slot
	CHILD_MAXIMIZED // hides sibling, fills slot
} child_slot_t;

static child_slot_t child_slot(node_t *c) {
	if (c == NULL)
		return CHILD_HIDDEN;
	if (c->client != NULL && c->client->state == STATE_FULLSCREEN)
		return CHILD_EXPANDED;
	if (c->client != NULL && client_is_maximized(c->client))
		return CHILD_MAXIMIZED;
	if (c->hidden || node_is_detached(c))
		return CHILD_HIDDEN;
	return CHILD_ACTIVE;
}

static void compute_split_rects(node_t *n, desktop_t *d, struct wlr_box rect,
		struct wlr_box *first_rect, struct wlr_box *second_rect) {
	if (d->layout == LAYOUT_MONOCLE) {
		*first_rect = rect;
		*second_rect = rect;
		return;
	}

	child_slot_t a = child_slot(n->first_child);
	child_slot_t b = child_slot(n->second_child);

	if (a == CHILD_EXPANDED || b == CHILD_EXPANDED) {
		*first_rect = a == CHILD_HIDDEN ? (struct wlr_box){0} : rect;
		*second_rect = b == CHILD_HIDDEN ? (struct wlr_box){0} : rect;
		return;
	}

	if (a == CHILD_MAXIMIZED || b == CHILD_MAXIMIZED) {
		*first_rect = a == CHILD_MAXIMIZED ? rect : (struct wlr_box){0};
		*second_rect = b == CHILD_MAXIMIZED ? rect : (struct wlr_box){0};
		return;
	}

	if (a != CHILD_ACTIVE && b == CHILD_ACTIVE) {
		*first_rect = (struct wlr_box){0};
		*second_rect = rect;
		return;
	}

	if (b != CHILD_ACTIVE && a == CHILD_ACTIVE) {
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

	if (n->hidden || node_is_detached(n))
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
		if (node_is_minimized(n) || n->hidden)
			return 0;

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
