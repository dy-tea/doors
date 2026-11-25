#include "ipc/args.h"
#include "layout/layout.h"
#include "layout/master_stack.h"
#include "output/output.h"
#include "tree.h"
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <wlr/util/log.h>

// per-desktop master-stack configuration
typedef struct {
	float ratio; // master area fraction, 0 = default
	master_area_orientation_t orientation;
	stack_layout_t stack_layout;
	int count;
} master_stack_state_t;

// the desktop's master-stack state, created with defaults on first use
static master_stack_state_t *ms_state(desktop_t *d) {
	if (d == NULL)
		return NULL;

	master_stack_state_t *s = layout_state_peek(d, LAYOUT_MASTER_STACK);
	if (s != NULL)
		return s;

	s = layout_state_get(d, LAYOUT_MASTER_STACK, sizeof(*s), NULL);
	if (s != NULL) {
		s->ratio = 0.5f;
		s->orientation = MASTER_LEFT;
		s->stack_layout = STACK_VERTICAL;
		s->count = 1;
	}
	return s;
}

static float ms_ratio(desktop_t *d) {
	const master_stack_state_t *s = ms_state(d);
	return s != NULL && s->ratio > 0.0f ? s->ratio : 0.5f;
}

static master_area_orientation_t ms_orientation(desktop_t *d) {
	const master_stack_state_t *s = ms_state(d);
	return s != NULL ? s->orientation : MASTER_LEFT;
}

static stack_layout_t ms_stack_layout(desktop_t *d) {
	const master_stack_state_t *s = ms_state(d);
	return s != NULL ? s->stack_layout : STACK_VERTICAL;
}

static int ms_count(desktop_t *d) {
	const master_stack_state_t *s = ms_state(d);
	return s != NULL ? s->count : 0;
}

static int compare_node_order(const void *lhs, const void *rhs) {
	const node_t *a = *(node_t *const *)lhs;
	const node_t *b = *(node_t *const *)rhs;
	const uint64_t a_order = a->client->master_stack_order;
	const uint64_t b_order = b->client->master_stack_order;

	if (a_order < b_order)
		return -1;
	if (a_order > b_order)
		return 1;
	return 0;
}

static int compare_master_then_order(const void *lhs, const void *rhs) {
	const node_t *a = *(node_t *const *)lhs;
	const node_t *b = *(node_t *const *)rhs;
	const bool a_master = a->client->flags.master_stack_master;
	const bool b_master = b->client->flags.master_stack_master;

	if (a_master != b_master)
		return a_master ? -1 : 1;
	return compare_node_order(lhs, rhs);
}

static int master_count_clamped(desktop_t *d, int total_nodes) {
	if (!d || total_nodes <= 0)
		return 0;
	if (ms_count(d) < 0)
		return 0;
	if (ms_count(d) > total_nodes)
		return total_nodes;
	return ms_count(d);
}

static void reconcile_master_membership(desktop_t *d, node_t **nodes, int count) {
	int target = master_count_clamped(d, count);
	int actual = 0;

	// count number of actual masters
	for (int i = 0; i < count; ++i)
		actual += nodes[i]->client->flags.master_stack_master;

	if (actual > target) {
		for (int i = count - 1; i >= 0 && actual > target; --i) {
			client_t *client = nodes[i]->client;

			if (client->flags.master_stack_master) {
				client->flags.master_stack_master = false;
				--actual;
			}
		}
	} else if (actual < target) {
		for (int i = 0; i < count && actual < target; ++i) {
			client_t *client = nodes[i]->client;

			if (!client->flags.master_stack_master) {
				client->flags.master_stack_master = true;
				++actual;
			}
		}
	}
}

static int collect_tiled_nodes(desktop_t *d, node_t ***out_nodes) {
	const int count = collect_tiled_leaves(d, out_nodes);
	if (count == 0)
		return 0;

	node_t **nodes = *out_nodes;
	qsort(nodes, (size_t)count, sizeof(*nodes), compare_node_order);
	reconcile_master_membership(d, nodes, count);
	qsort(nodes, (size_t)count, sizeof(*nodes), compare_master_then_order);

	return count;
}

static void set_node_geom(node_t *n, struct wlr_box geom, output_t *m, desktop_t *d) {
	if (!n || !n->client)
		return;

	if (geom.width < 1 || geom.height < 1) {
		n->client->tiled_rectangle = (struct wlr_box){0};
		n->client->arranged_rectangle = (struct wlr_box){0};
		n->output = m;
		node_set_pending_rectangle(n, geom);
		node_set_dirty(n);
		return;
	}

	const unsigned int bw = effective_border_width(d);
	struct wlr_box r = geom;
	r.x += bw;
	r.y += bw;
	r.width = (r.width > (int)(2 * bw)) ? r.width - 2 * bw : 0;
	r.height = (r.height > (int)(2 * bw)) ? r.height - 2 * bw : 0;

	if (r.width < MIN_RENDER_WIDTH)
		r.width = MIN_RENDER_WIDTH;
	if (r.height < MIN_RENDER_HEIGHT)
		r.height = MIN_RENDER_HEIGHT;

	if (n->client->state == STATE_PSEUDO_TILED) {
		if (r.width > n->client->floating_rectangle.width)
			r.width = n->client->floating_rectangle.width;
		if (r.height > n->client->floating_rectangle.height)
			r.height = n->client->floating_rectangle.height;

		if (r.width < MIN_RENDER_WIDTH)
			r.width = MIN_RENDER_WIDTH;
		if (r.height < MIN_RENDER_HEIGHT)
			r.height = MIN_RENDER_HEIGHT;
	}

	if (settings.respect_tiled_min_size) {
		if (n->constraints.min_width > 0 && r.width < (int)n->constraints.min_width && geom.width > 0) {
			r.x = geom.x + (geom.width - (int)n->constraints.min_width) / 2;
			r.width = n->constraints.min_width;
		}
		if (n->constraints.min_height > 0 && r.height < (int)n->constraints.min_height &&
				geom.height > 0) {
			r.y = geom.y + (geom.height - (int)n->constraints.min_height) / 2;
			r.height = n->constraints.min_height;
		}
	}

	n->client->tiled_rectangle = r;
	n->client->arranged_rectangle = r;
	node_set_pending_rectangle(n, geom);
	n->output = m;
	node_set_dirty(n);
}

static void distribute_area(const struct wlr_box area, struct wlr_box *out, const int count,
		const int gap, const bool vertical) {
	if (count <= 0)
		return;

	const int span = vertical ? area.height : area.width;
	int actual_gap = gap > 0 ? gap : 0;
	if (count > 1 && (long long)actual_gap * (count - 1) >= span) {
		const int room_for_gaps = span - count;
		actual_gap = room_for_gaps > 0 ? room_for_gaps / (count - 1) : 0;
	}

	int usable = span - actual_gap * (count - 1);
	if (usable < 0)
		usable = 0;
	const int base = usable / count;
	const int remainder = usable % count;
	int cursor = vertical ? area.y : area.x;

	for (int i = 0; i < count; i++) {
		const int size = base + (i < remainder ? 1 : 0);
		out[i] = area;
		if (vertical) {
			out[i].y = cursor;
			out[i].height = size;
		} else {
			out[i].x = cursor;
			out[i].width = size;
		}
		cursor += size + actual_gap;
	}
}

static int master_span_size(desktop_t *d, int span, int gap_count, int gap) {
	int usable = span - gap_count * (gap > 0 ? gap : 0);
	if (usable < 1)
		usable = 1;

	int size = (int)(usable * ms_ratio(d));
	if (size < 1)
		size = 1;
	if (size > usable)
		size = usable;
	return size;
}

static void apply_group(output_t *m, desktop_t *d, node_t **nodes, const int count,
		const struct wlr_box area, const int gap, const bool vertical, struct wlr_box *geoms) {
	distribute_area(area, geoms, count, gap, vertical);
	for (int i = 0; i < count; i++)
		set_node_geom(nodes[i], geoms[i], m, d);
}

static bool arrange_center(output_t *m, desktop_t *d, node_t **nodes, const int mc, const int sc,
		const struct wlr_box rect, const int gap, struct wlr_box *geoms) {
	if (sc == 0) {
		apply_group(m, d, nodes, mc, rect, gap, true, geoms);
		return true;
	}

	const int master_width = master_span_size(d, rect.width, 2, gap);
	const int master_x = rect.x + (rect.width - master_width) / 2;
	const struct wlr_box master_area = {
		.x = master_x,
		.y = rect.y,
		.width = master_width,
		.height = rect.height,
	};
	struct wlr_box left_area = {
		.x = rect.x,
		.y = rect.y,
		.width = master_x - rect.x - gap,
		.height = rect.height,
	};
	struct wlr_box right_area = {
		.x = master_x + master_width + gap,
		.y = rect.y,
		.width = rect.x + rect.width - (master_x + master_width + gap),
		.height = rect.height,
	};

	if (left_area.width < 1)
		left_area.width = 1;
	if (right_area.width < 1)
		right_area.width = 1;

	apply_group(m, d, nodes, mc, master_area, gap, true, geoms);

	const int left_count = (sc + 1) / 2;
	const int right_count = sc / 2;
	const int side_capacity = left_count > right_count ? left_count : right_count;
	node_t **side_nodes = calloc((size_t)side_capacity, sizeof(*side_nodes));
	if (!side_nodes) {
		wlr_log(WLR_ERROR, "Master-stack side allocation failed");
		return false;
	}

	int left_index = 0;
	for (int i = 0; i < sc; i += 2)
		side_nodes[left_index++] = nodes[mc + i];
	apply_group(m, d, side_nodes, left_count, left_area, gap, true, geoms);

	int right_index = 0;
	for (int i = 1; i < sc; i += 2)
		side_nodes[right_index++] = nodes[mc + i];
	apply_group(m, d, side_nodes, right_count, right_area, gap, true, geoms);

	free(side_nodes);
	return true;
}

void master_stack_arrange(output_t *m, desktop_t *d, struct wlr_box available) {
	if (!d || !d->root)
		return;

	node_t **nodes = NULL;
	const int count = collect_tiled_nodes(d, &nodes);
	if (count == 0)
		return;

	const int gap = compute_window_gap(d);
	struct wlr_box rect = available;
	rect.x += gap;
	rect.y += gap;
	rect.width -= 2 * gap;
	rect.height -= 2 * gap;
	if (rect.width < 1)
		rect.width = 1;
	if (rect.height < 1)
		rect.height = 1;

	struct wlr_box *geoms = calloc((size_t)count, sizeof(*geoms));
	if (!geoms) {
		wlr_log(WLR_ERROR, "Master-stack geometry allocation failed");
		free(nodes);
		return;
	}

	int maximized_index = -1;
	for (int i = 0; i < count; i++) {
		if (client_is_maximized(nodes[i]->client)) {
			maximized_index = i;
			break;
		}
	}

	if (maximized_index >= 0) {
		for (int i = 0; i < count; i++)
			geoms[i] = i == maximized_index ? rect : (struct wlr_box){0};
		for (int i = 0; i < count; i++)
			set_node_geom(nodes[i], geoms[i], m, d);
		free(geoms);
		free(nodes);
		return;
	}

	int mc = master_count_clamped(d, count);
	int sc = count - mc;

	if (mc == 0) {
		apply_group(m, d, nodes, count, rect, gap, ms_stack_layout(d) == STACK_VERTICAL, geoms);
		free(geoms);
		free(nodes);
		return;
	}

	master_area_orientation_t orientation = ms_orientation(d);
	if (orientation == MASTER_CENTER && sc < 2)
		orientation = MASTER_LEFT;

	if (orientation == MASTER_CENTER) {
		arrange_center(m, d, nodes, mc, sc, rect, gap, geoms);
		free(geoms);
		free(nodes);
		return;
	}

	const bool horizontal_split = orientation == MASTER_LEFT || orientation == MASTER_RIGHT;
	const bool master_first = orientation == MASTER_LEFT || orientation == MASTER_TOP;

	if (sc == 0) {
		apply_group(m, d, nodes, mc, rect, gap, horizontal_split, geoms);
		free(geoms);
		free(nodes);
		return;
	}

	const int span = horizontal_split ? rect.width : rect.height;
	const int master_size = master_span_size(d, span, 1, gap);
	struct wlr_box master_area = rect;
	struct wlr_box stack_area = rect;

	if (horizontal_split) {
		master_area.width = master_size;
		stack_area.width = rect.width - master_size - gap;
		if (master_first) {
			stack_area.x = rect.x + master_size + gap;
		} else {
			master_area.x = rect.x + rect.width - master_size;
		}
	} else {
		master_area.height = master_size;
		stack_area.height = rect.height - master_size - gap;
		if (master_first) {
			stack_area.y = rect.y + master_size + gap;
		} else {
			master_area.y = rect.y + rect.height - master_size;
		}
	}

	if (stack_area.width < 1)
		stack_area.width = 1;
	if (stack_area.height < 1)
		stack_area.height = 1;

	apply_group(m, d, nodes, mc, master_area, gap, horizontal_split, geoms);
	apply_group(m, d, nodes + mc, sc, stack_area, gap, ms_stack_layout(d) == STACK_VERTICAL, geoms);

	free(geoms);
	free(nodes);
}

static int find_node_index(node_t **nodes, int count, const node_t *node) {
	for (int i = 0; i < count; i++)
		if (nodes[i] == node)
			return i;
	return -1;
}

bool master_stack_increment(desktop_t *d) {
	node_t **nodes = NULL;
	const int count = collect_tiled_nodes(d, &nodes);
	const int mc = master_count_clamped(d, count);
	if (count == 0 || mc >= count) {
		free(nodes);
		return false;
	}

	const node_t *target = d->focus && d->focus->client &&
		!d->focus->client->flags.master_stack_master && IS_TILED(d->focus->client) ? d->focus : nodes[mc];
	target->client->flags.master_stack_master = true;

	master_stack_state_t *s = ms_state(d);
	if (s != NULL)
		s->count = mc + 1;

	free(nodes);
	return true;
}

bool master_stack_decrement(desktop_t *d) {
	node_t **nodes = NULL;
	const int count = collect_tiled_nodes(d, &nodes);
	const int mc = master_count_clamped(d, count);
	if (mc == 0) {
		free(nodes);
		return false;
	}

	const node_t *target = d->focus && d->focus->client && d->focus->client->flags.master_stack_master &&
		IS_TILED(d->focus->client) ? d->focus : nodes[mc - 1];
	target->client->flags.master_stack_master = false;

	master_stack_state_t *s = ms_state(d);
	if (s != NULL)
		s->count = mc - 1;

	free(nodes);
	return true;
}

bool master_stack_promote(desktop_t *d) {
	node_t **nodes = NULL;
	const int count = collect_tiled_nodes(d, &nodes);
	const int mc = master_count_clamped(d, count);
	const int focus_index = find_node_index(nodes, count, d ? d->focus : NULL);
	if (focus_index < 0 || nodes[focus_index]->client->flags.master_stack_master) {
		free(nodes);
		return false;
	}

	nodes[focus_index]->client->flags.master_stack_master = true;

	master_stack_state_t *s = ms_state(d);
	if (s != NULL)
		s->count = mc + 1;

	free(nodes);
	return true;
}

bool master_stack_demote(desktop_t *d) {
	if (!d || !d->focus || !d->focus->client || !IS_TILED(d->focus->client) ||
		!d->focus->client->flags.master_stack_master)
		return false;

	node_t **nodes = NULL;
	const int count = collect_tiled_nodes(d, &nodes);
	const int mc = master_count_clamped(d, count);
	if (mc == 0) {
		free(nodes);
		return false;
	}

	d->focus->client->flags.master_stack_master = false;

	master_stack_state_t *s = ms_state(d);
	if (s != NULL)
		s->count = mc - 1;

	free(nodes);
	return true;
}

void master_stack_set_count(desktop_t *d, const int count) {
	master_stack_state_t *s = ms_state(d);
	if (s != NULL)
		s->count = count < 0 ? 0 : count;
}

bool master_stack_adjust_ratio(desktop_t *d, const float delta) {
	master_stack_state_t *s = ms_state(d);
	if (s == NULL)
		return false;

	float ratio = (s->ratio > 0.0f ? s->ratio : 0.5f) + delta;
	if (ratio < 0.1f)
		ratio = 0.1f;
	if (ratio > 0.9f)
		ratio = 0.9f;
	if (ratio == s->ratio)
		return false;

	s->ratio = ratio;
	return true;
}

void master_stack_set_ratio(desktop_t *d, const float ratio) {
	master_stack_state_t *s = ms_state(d);
	if (s == NULL)
		return;

	s->ratio = ratio < 0.1f ? 0.1f : (ratio > 0.9f ? 0.9f : ratio);
}

master_area_orientation_t master_stack_orientation(desktop_t *d) {
	return ms_orientation(d);
}

void master_stack_set_orientation(desktop_t *d, const master_area_orientation_t orientation) {
	if (d == NULL || orientation < MASTER_LEFT || orientation > MASTER_CENTER)
		return;

	master_stack_state_t *s = ms_state(d);
	if (s == NULL)
		return;

	s->orientation = orientation;
	s->stack_layout = orientation == MASTER_TOP ||
		orientation == MASTER_BOTTOM ? STACK_HORIZONTAL : STACK_VERTICAL;
}

void master_stack_flip_orientation(desktop_t *d) {
	master_stack_state_t *s = ms_state(d);
	if (s == NULL)
		return;

	const master_area_orientation_t orient = s->orientation;
	s->orientation = orient == MASTER_CENTER ? orient : (master_area_orientation_t)((orient + 2) %
		MASTER_CENTER);
}

void master_stack_cycle_orientation(desktop_t *d) {
	if (d == NULL)
		return;
	master_stack_set_orientation(d, (master_area_orientation_t)((ms_orientation(d) + 1) % 5));
}

void master_stack_cycle_stack_layout(desktop_t *d) {
	master_stack_state_t *s = ms_state(d);
	if (s != NULL)
		s->stack_layout = (ms_stack_layout(d) + 1) % 2;
}

static node_t **gather_focused(desktop_t *d, int *out_total, int *out_index) {
	node_t **nodes = NULL;
	const int count = collect_tiled_nodes(d, &nodes);

	int visible = 0;
	for (int i = 0; i < count; i++) {
		if (node_is_invisible(nodes[i]))
			continue;

		if (visible != i)
			nodes[visible] = nodes[i];

		visible++;
	}

	const int index = visible > 0 ? find_node_index(nodes, visible, d ? d->focus : NULL) : -1;
	if (index < 0) {
		free(nodes);
		return NULL;
	}

	*out_total = visible;
	*out_index = index;
	return nodes;
}

static struct wlr_box node_layout_box(const node_t *node) {
	if (node->pending.rectangle.width > 0 && node->pending.rectangle.height > 0)
		return node->pending.rectangle;
	return node->rectangle;
}

static bool aligned_with(const struct wlr_box *source, const struct wlr_box *candidate,
		const direction_t direction) {
	if (direction == DIR_NORTH || direction == DIR_SOUTH)
		return source->x < candidate->x + candidate->width && candidate->x < source->x + source->width;
	return source->y < candidate->y + candidate->height && candidate->y < source->y + source->height;
}

static int directional_target(node_t **nodes, const int count, const int source_index,
		const direction_t direction, const bool wrap) {
	const struct wlr_box source = node_layout_box(nodes[source_index]);
	const int64_t source_x = (int64_t)source.x * 2 + source.width;
	const int64_t source_y = (int64_t)source.y * 2 + source.height;
	int aligned_target = -1;
	int64_t aligned_primary = INT64_MAX, aligned_secondary = INT64_MAX;
	int target = -1;
	int64_t best_primary = INT64_MAX, best_secondary = INT64_MAX;

	for (int i = 0; i < count; i++) {
		if (i == source_index)
			continue;
		struct wlr_box candidate = node_layout_box(nodes[i]);
		const int64_t x = (int64_t)candidate.x * 2 + candidate.width;
		const int64_t y = (int64_t)candidate.y * 2 + candidate.height;
		const int64_t dx = x - source_x;
		const int64_t dy = y - source_y;
		int64_t primary = 0, secondary = 0;
		bool eligible = false;

		switch (direction) {
		case DIR_WEST:
			eligible = dx < 0;
			primary = -dx;
			secondary = llabs(dy);
			break;
		case DIR_SOUTH:
			eligible = dy > 0;
			primary = dy;
			secondary = llabs(dx);
			break;
		case DIR_NORTH:
			eligible = dy < 0;
			primary = -dy;
			secondary = llabs(dx);
			break;
		case DIR_EAST:
			eligible = dx > 0;
			primary = dx;
			secondary = llabs(dy);
			break;
		}

		if (!eligible)
			continue;

		if (primary < best_primary || (primary == best_primary && secondary < best_secondary)) {
			target = i;
			best_primary = primary;
			best_secondary = secondary;
		}

		if (!aligned_with(&source, &candidate, direction))
			continue;
		if (primary < aligned_primary || (primary == aligned_primary && secondary < aligned_secondary)) {
			aligned_target = i;
			aligned_primary = primary;
			aligned_secondary = secondary;
		}
	}

	if (aligned_target >= 0)
		target = aligned_target;

	if (target >= 0 || !wrap)
		return target;

	best_primary = INT64_MAX;
	best_secondary = INT64_MAX;
	for (int i = 0; i < count; i++) {
		if (i == source_index)
			continue;
		const struct wlr_box candidate = node_layout_box(nodes[i]);
		const int64_t x = (int64_t)candidate.x * 2 + candidate.width;
		const int64_t y = (int64_t)candidate.y * 2 + candidate.height;
		int64_t primary, secondary;

		if (direction == DIR_WEST || direction == DIR_EAST) {
			primary = direction == DIR_WEST ? -x : x;
			secondary = llabs(y - source_y);
		} else {
			primary = direction == DIR_NORTH ? -y : y;
			secondary = llabs(x - source_x);
		}

		if (primary < best_primary || (primary == best_primary && secondary < best_secondary)) {
			target = i;
			best_primary = primary;
			best_secondary = secondary;
		}
	}
	return target;
}

bool master_stack_focus(desktop_t *d, const direction_t direction) {
	node_t **nodes = NULL;
	int count = 0;
	int index = -1;
	nodes = gather_focused(d, &count, &index);
	if (!nodes)
		return false;

	const int target = directional_target(nodes, count, index, direction, settings.focus_wrapping);
	if (target >= 0) {
		d->focus = nodes[target];
		if (d->output)
			focus_node(d->output, d, d->focus);
	}
	free(nodes);
	return target >= 0;
}

bool master_stack_swap(output_t *m, desktop_t *d, const direction_t direction) {
	node_t **nodes = NULL;
	int count = 0;
	int index = -1;
	nodes = gather_focused(d, &count, &index);
	if (!nodes)
		return false;

	const int target = directional_target(nodes, count, index, direction, settings.focus_wrapping);
	if (target < 0) {
		free(nodes);
		return false;
	}

	client_t *focused = nodes[index]->client;
	client_t *other = nodes[target]->client;
	const uint64_t order = focused->master_stack_order;
	const bool is_master = focused->flags.master_stack_master;
	focused->master_stack_order = other->master_stack_order;
	focused->flags.master_stack_master = other->flags.master_stack_master;
	other->master_stack_order = order;
	other->flags.master_stack_master = is_master;

	free(nodes);
	arrange(m, d, true);
	return true;
}

// a drag along the master/stack split resizes the master area instead of a tree split
static bool master_stack_resize(output_t *m, desktop_t *d, const direction_t dir,
		const double delta) {
	if (d == NULL)
		return false;

	const bool horizontal = dir == DIR_WEST || dir == DIR_EAST;
	const master_area_orientation_t orientation = ms_orientation(d);
	const bool horizontal_split = orientation == MASTER_LEFT || orientation == MASTER_RIGHT ||
		orientation == MASTER_CENTER;

	// the other axis still resizes the split tree
	if (horizontal != horizontal_split)
		return true;

	const double amount = orientation == MASTER_RIGHT || orientation == MASTER_BOTTOM ? -delta : delta;
	if (master_stack_adjust_ratio(d, (float)amount))
		arrange(m, d, true);

	return true;
}

static const str_pair_t orientation_values[] = {
	{"left", MASTER_LEFT},
	{"right", MASTER_RIGHT},
	{"top", MASTER_TOP},
	{"bottom", MASTER_BOTTOM},
	{"down", MASTER_BOTTOM},
	{"center", MASTER_CENTER},
	IPC_ENUM_END,
};

static bool ms_target(ipc_args_t *a, output_t **mon, desktop_t **desk) {
	return layout_command_target(a, false, mon, desk);
}

static void ms_cycle_orientation(ipc_args_t *a) {
	output_t *m;
	desktop_t *d;
	if (!ms_target(a, &m, &d))
		return;

	master_stack_cycle_orientation(d);
	layout_command_commit(a, m, d, "Orientation cycled\n");
}

static void ms_cmd_orientation(ipc_args_t *a) {
	long orientation;
	if (!ipc_enum(a, "orientation", orientation_values, &orientation))
		return;

	output_t *m;
	desktop_t *d;
	if (!ms_target(a, &m, &d))
		return;

	master_stack_set_orientation(d, (master_area_orientation_t)orientation);
	layout_command_commit(a, m, d, "Orientation set\n");
}

static void ms_cycle_stack_layout(ipc_args_t *a) {
	output_t *m;
	desktop_t *d;
	if (!ms_target(a, &m, &d))
		return;

	master_stack_cycle_stack_layout(d);
	layout_command_commit(a, m, d, "Stack layout cycled\n");
}

static void ms_inc(ipc_args_t *a) {
	output_t *m;
	desktop_t *d;
	if (!ms_target(a, &m, &d))
		return;

	master_stack_increment(d);
	layout_command_commit(a, m, d, "Master added\n");
}

static void ms_dec(ipc_args_t *a) {
	output_t *m;
	desktop_t *d;
	if (!ms_target(a, &m, &d))
		return;

	master_stack_decrement(d);
	layout_command_commit(a, m, d, "master removed\n");
}

static void ms_flip(ipc_args_t *a) {
	output_t *m;
	desktop_t *d;
	if (!ms_target(a, &m, &d))
		return;

	master_stack_flip_orientation(d);
	layout_command_commit(a, m, d, "orientation flipped\n");
}

static void ms_promote(ipc_args_t *a) {
	output_t *m;
	desktop_t *d;
	if (!ms_target(a, &m, &d))
		return;

	if (!master_stack_promote(d)) {
		ipc_fail(a, "Focus a secondary tiled window\n");
		return;
	}

	layout_command_commit(a, m, d, "focused window promoted\n");
}

static void ms_demote(ipc_args_t *a) {
	output_t *m;
	desktop_t *d;
	if (!ms_target(a, &m, &d))
		return;

	if (!master_stack_demote(d)) {
		ipc_fail(a, "Focus a master tiled window\n");
		return;
	}

	layout_command_commit(a, m, d, "Focused window demoted\n");
}

static void ms_set_count(ipc_args_t *a) {
	int count;
	if (!ipc_int(a, "count", 0, INT_MAX, &count))
		return;

	output_t *m;
	desktop_t *d;
	if (!ms_target(a, &m, &d))
		return;

	master_stack_set_count(d, count);
	layout_command_commit(a, m, d, "Master count set\n");
}

static void ms_set_ratio(ipc_args_t *a) {
	float ratio;
	if (!ipc_float(a, "ratio", 0.1f, 0.9f, &ratio))
		return;

	output_t *m;
	desktop_t *d;
	if (!ms_target(a, &m, &d))
		return;

	master_stack_set_ratio(d, ratio);
	layout_command_commit(a, m, d, "Ratio set\n");
}

static const command_t master_stack_commands[] = {
	{"cycle_orientation", "layout master_stack cycle_orientation", ms_cycle_orientation},
	{"orientation", "layout master_stack orientation <left|right|top|bottom|center>",
		ms_cmd_orientation},
	{"cycle_stack_layout", "layout master_stack cycle_stack_layout", ms_cycle_stack_layout},
	{"inc", "layout master_stack inc", ms_inc},
	{"dec", "layout master_stack dec", ms_dec},
	{"flip", "layout master_stack flip", ms_flip},
	{"promote", "layout master_stack promote", ms_promote},
	{"demote", "layout master_stack demote", ms_demote},
	{"set_count", "layout master_stack set_count <count>", ms_set_count},
	{"set_ratio", "layout master_stack set_ratio <0.1-0.9>", ms_set_ratio},
	{NULL, NULL, NULL},
};

const layout_impl_t master_stack_impl = {
	.name = "master_stack",
	.arrange = master_stack_arrange,
	.on_focus = NULL,
	.focus = master_stack_focus,
	.swap = master_stack_swap,
	.enter = NULL,
	.leave = NULL,
	.init_client = NULL,
	.client_mapped = NULL,
	.client_unmapped = NULL,
	.focus_after_unmap = NULL,
	.on_client_state = NULL,
	.resize = master_stack_resize,
	.places_floating = NULL,
	.wants_visible = NULL,
	.borderless = NULL,
	.reports_maximized = NULL,
	.window_gap = NULL,
	.resize_edges = NULL,
	.pointer_resize_begin = NULL,
	.pointer_resize_update = NULL,
	.focus_scroll_distance = NULL,
	.pan_fingers = NULL,
	.pan_update = NULL,
	.pan_end = NULL,
	.commands = master_stack_commands,
	.single_visible = false,
};
