#include "config.h"
#include "layout/layout.h"
#include "layout/scroller.h"
#include "layout/scroller_view.h"
#include "output/output.h"
#include "tree.h"
#include "view.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

static int max_i(int a, int b) {
	return a > b ? a : b;
}

static int min_i(int a, int b) {
	return a < b ? a : b;
}

static double max_d(double a, double b) {
	return a > b ? a : b;
}

static double min_d(double a, double b) {
	return a < b ? a : b;
}

static double clamp_d(double val, double lo, double hi) {
	return min_d(max_d(val, lo), hi);
}

static int clamp_i(int val, int lo, int hi) {
	return min_i(max_i(val, lo), hi);
}

static scroller_tile_t *tile_grow(scroller_column_t *col) {
	if (col->tile_count >= col->capacity) {
		int new_cap = col->capacity ? col->capacity * 2 : 4;
		scroller_tile_t *t = realloc(col->tiles, (size_t)new_cap * sizeof(*t));
		if (!t)
			return NULL;
		col->tiles = t;
		col->capacity = new_cap;
	}
	return &col->tiles[col->tile_count];
}

static scroller_column_t *column_grow(scroller_state_t *s) {
	if (s->column_count >= s->capacity) {
		int new_cap = s->capacity ? s->capacity * 2 : 4;
		scroller_column_t *c = realloc(s->columns, (size_t)new_cap * sizeof(*c));
		if (!c)
			return NULL;
		s->columns = c;
		s->capacity = new_cap;
	}
	return &s->columns[s->column_count];
}

scroller_state_t *scroller_create(void) {
	scroller_state_t *s = calloc(1, sizeof(*s));
	if (!s)
		return NULL;
	s->view_offset = 0.0;
	s->activate_prev_column_on_removal = false;
	s->default_column_width = settings.scroller_default_column_width;
	return s;
}

void scroller_destroy(scroller_state_t *s) {
	if (!s)
		return;
	for (int i = 0; i < s->column_count; i++) {
		free(s->columns[i].tiles);
	}
	free(s->columns);
	free(s);
}

void scroller_leave(struct output_t *m, desktop_t *d) {
	(void)m;
	if (d == NULL)
		return;

	scroller_destroy(d->scroller_state);
	d->scroller_state = NULL;
}

static bool scroller_tile_shown(const scroller_tile_t *t) {
	if (t == NULL || t->client == NULL)
		return false;

	if (t->client->flags.minimized)
		return false;

	node_t *n = client_get_node(t->client);
	if (n != NULL && (n->hidden || node_is_detached(n)))
		return false;

	return true;
}

static bool scroller_column_shown(const scroller_column_t *col) {
	for (int j = 0; j < col->tile_count; j++)
		if (scroller_tile_shown(&col->tiles[j]))
			return true;

	return false;
}

static double resolve_size(scroller_size_t size, double gap, double area) {
	switch (size.type) {
	case SCROLLER_SIZE_FIXED:
		return max_d(1.0, size.value);
	case SCROLLER_SIZE_PROPORTION:
		return max_d(1.0, (area - gap) * size.value - gap);
	}
	return 1.0;
}

static struct wlr_box scroller_working_area(struct wlr_box available) {
	int left = max_i(0, settings.scroller_struts.left);
	int right = max_i(0, settings.scroller_struts.right);
	int top = max_i(0, settings.scroller_struts.top);
	int bottom = max_i(0, settings.scroller_struts.bottom);

	return (struct wlr_box){
		.x = available.x + left,
		.y = available.y + top,
		.width = max_i(1, available.width - left - right),
		.height = max_i(1, available.height - top - bottom),
	};
}

static void scroller_resolve_widths(scroller_state_t *s, double gap, double area_w) {
	for (int i = 0; i < s->column_count; i++) {
		if (!scroller_column_shown(&s->columns[i])) {
			s->columns[i].resolved_width = 0.0;
			continue;
		}

		s->columns[i].resolved_width = resolve_size(s->columns[i].width, gap, area_w);
	}
}

static int scroller_camera_column(scroller_state_t *s) {
	if (s->active_column_idx >= 0 && s->active_column_idx < s->column_count &&
		scroller_column_shown(&s->columns[s->active_column_idx]))
		return s->active_column_idx;

	for (int dist = 1; dist < s->column_count; dist++) {
		int right = s->active_column_idx + dist;
		if (right < s->column_count && scroller_column_shown(&s->columns[right]))
			return right;

		int left = s->active_column_idx - dist;
		if (left >= 0 && scroller_column_shown(&s->columns[left]))
			return left;
	}

	return s->active_column_idx;
}

static bool scroller_view_setup(scroller_state_t *s, scroller_view_t *v) {
	if (s == NULL || s->column_count <= 0)
		return false;

	scroller_resolve_widths(s, s->gap, (double)s->working_area.width);

	v->column_count = s->column_count;
	v->gap = s->gap;
	v->view_width = (double)s->working_area.width;
	if (v->view_width <= 1.0)
		v->view_width = 1.0;

	v->widths = malloc((size_t)s->column_count * sizeof(*v->widths));
	v->xs = malloc((size_t)(s->column_count + 1) * sizeof(*v->xs));
	if (v->widths == NULL || v->xs == NULL) {
		free(v->widths);
		free(v->xs);
		v->widths = NULL;
		v->xs = NULL;
		return false;
	}

	for (int i = 0; i < s->column_count; i++)
		v->widths[i] = s->columns[i].resolved_width;

	scroller_view_layout(v->widths, s->column_count, s->gap, v->xs, v->widths);
	return true;
}

static void scroller_view_release(scroller_view_t *v) {
	free(v->widths);
	free(v->xs);
	v->widths = NULL;
	v->xs = NULL;
}

static bool scroller_is_centering(const scroller_state_t *s) {
	return settings.scroller_center_focused_column == SCROLLER_CENTER_ALWAYS ||
		(settings.scroller_always_center_single_column && s->column_count <= 1);
}

static double scroller_view_offset_for_column(scroller_state_t *s, const scroller_view_t *v,
		double cur_x, int idx, int prev_idx) {
	double col_x = v->xs[idx];
	double col_w = v->widths[idx];

	if (scroller_is_centering(s))
		return scroller_view_offset_centered(v, cur_x, col_x, col_w);

	if (settings.scroller_center_focused_column == SCROLLER_CENTER_ON_OVERFLOW && prev_idx >= 0 &&
			prev_idx != idx) {
		int src_idx = prev_idx > idx ? min_i(idx + 1, s->column_count - 1) : max_i(idx - 1, 0);
		double src_x = v->xs[src_idx];
		double src_w = v->widths[src_idx];

		double span = src_x < col_x ? col_x - src_x + col_w : src_x - col_x + src_w;
		span += v->gap * 2.0;

		if (span > v->view_width)
			return scroller_view_offset_centered(v, cur_x, col_x, col_w);
	}

	return scroller_view_offset_fit(v, cur_x, col_x, col_w);
}

static void scroller_recenter_view(scroller_state_t *s, int prev_idx) {
	scroller_view_t v;
	if (!scroller_view_setup(s, &v))
		return;

	int idx = scroller_camera_column(s);
	if (idx < 0 || idx >= s->column_count)
		return;

	if (prev_idx < 0 || prev_idx >= s->column_count || prev_idx == idx)
		prev_idx = idx;

	double cur_x = v.xs[prev_idx] + s->view_offset;
	s->view_offset = scroller_view_offset_for_column(s, &v, cur_x, idx, prev_idx);

	scroller_view_release(&v);
}

static bool find_tile(scroller_state_t *s, client_t *c, int *out_col_idx, int *out_tile_idx) {
	for (int i = 0; i < s->column_count; i++) {
		for (int j = 0; j < s->columns[i].tile_count; j++) {
			if (s->columns[i].tiles[j].client == c) {
				if (out_col_idx)
					*out_col_idx = i;
				if (out_tile_idx)
					*out_tile_idx = j;
				return true;
			}
		}
	}
	return false;
}

static int create_column(scroller_state_t *s, client_t *client, bool activate) {
	scroller_column_t *col = column_grow(s);
	if (!col)
		return -1;

	memset(col, 0, sizeof(*col));
	col->width = s->default_column_width;
	col->active_tile_idx = 0;

	scroller_tile_t *t = tile_grow(col);
	if (!t)
		return -1;
	memset(t, 0, sizeof(*t));
	t->client = client;
	t->height.type = SCROLLER_HEIGHT_AUTO;
	t->height.value = 1.0;

	col->tile_count = 1;

	int idx = s->column_count;
	s->column_count = idx + 1;

	if (activate) {
		s->active_column_idx = idx;
	}

	return idx;
}

bool scroller_add_tile(scroller_state_t *s, client_t *client, bool activate) {
	if (!s || !client)
		return false;

	// if client is already tracked, do nothing
	if (find_tile(s, client, NULL, NULL))
		return true;

	int num_tiled = 0;
	for (int i = 0; i < s->column_count; i++)
		num_tiled += s->columns[i].tile_count;

	// create first column
	if (num_tiled == 0)
		return create_column(s, client, activate) >= 0;

	int insert_col = s->active_column_idx + 1;
	if (insert_col > s->column_count)
		insert_col = s->column_count;

	// shift columns right to make room at insert_col
	if (!column_grow(s))
		return false;

	memmove(&s->columns[insert_col + 1], &s->columns[insert_col],
		(size_t)(s->column_count - insert_col) * sizeof(scroller_column_t));
	s->column_count++;

	scroller_column_t *new_col = &s->columns[insert_col];
	memset(new_col, 0, sizeof(*new_col));
	new_col->width = s->default_column_width;
	new_col->active_tile_idx = 0;
	new_col->tiles = NULL;
	new_col->tile_count = 0;
	new_col->capacity = 0;

	scroller_tile_t *t = tile_grow(new_col);
	if (!t)
		return false;
	memset(t, 0, sizeof(*t));
	t->client = client;
	t->height.type = SCROLLER_HEIGHT_AUTO;
	t->height.value = 1.0;
	new_col->tile_count = 1;

	// adjust active_column_idx
	if (activate) {
		s->active_column_idx = insert_col;
		s->activate_prev_column_on_removal = true;
	} else if (s->active_column_idx >= insert_col)
		s->active_column_idx++;

	return true;
}

bool scroller_add_tile_to_column(scroller_state_t *s, client_t *client, int col_idx,
		bool activate) {
	if (!s || !client)
		return false;
	if (col_idx < 0 || col_idx >= s->column_count)
		return false;

	if (find_tile(s, client, NULL, NULL))
		return true;

	scroller_column_t *col = &s->columns[col_idx];

	scroller_tile_t *t = tile_grow(col);
	if (!t)
		return false;
	memset(t, 0, sizeof(*t));
	t->client = client;
	t->height.type = SCROLLER_HEIGHT_AUTO;
	t->height.value = 1.0;

	col->tile_count++;
	if (activate) {
		col->active_tile_idx = col->tile_count - 1;
		s->active_column_idx = col_idx;
	}

	return true;
}

void scroller_remove_tile(scroller_state_t *s, client_t *client, struct output_t *m) {
	(void)m;
	if (!s || !client)
		return;

	int col_idx, tile_idx;
	if (!find_tile(s, client, &col_idx, &tile_idx))
		return;

	scroller_column_t *col = &s->columns[col_idx];

	// shift tiles left
	memmove(&col->tiles[tile_idx], &col->tiles[tile_idx + 1],
		(size_t)(col->tile_count - tile_idx - 1) * sizeof(scroller_tile_t));
	col->tile_count--;

	// fix active_tile_idx
	if (col->tile_count == 0) {
		free(col->tiles);
		col->tiles = NULL;
		col->capacity = 0;

		memmove(&s->columns[col_idx], &s->columns[col_idx + 1],
			(size_t)(s->column_count - col_idx - 1) * sizeof(scroller_column_t));
		s->column_count--;

		// fix active_column_idx
		if (s->column_count == 0) {
			s->active_column_idx = 0;
			s->activate_prev_column_on_removal = false;
			return;
		}
		if (col_idx == s->active_column_idx) {
			// active column was removed
			if (s->activate_prev_column_on_removal && s->active_column_idx > 0) {
				// activate previous column
				s->active_column_idx--;
			} else {
				// activate next column
				if (s->active_column_idx >= s->column_count)
					s->active_column_idx = s->column_count - 1;
			}
			s->activate_prev_column_on_removal = false;
		} else if (col_idx < s->active_column_idx) {
			s->active_column_idx--;
		}
	} else {
		if (tile_idx < col->active_tile_idx)
			col->active_tile_idx--;
		else if (tile_idx == col->active_tile_idx && col->active_tile_idx >= col->tile_count)
			col->active_tile_idx = col->tile_count - 1;
	}
}

static void populate_from_tree(scroller_state_t *s, desktop_t *d) {
	if (!d->root)
		return;

	int num_tiled = 0;
	FOR_EACH_LEAF(n, d->root)
		if (n->client && scroller_is_tiled(n->client))
			num_tiled++;

	if (num_tiled == 0)
		return;

	FOR_EACH_LEAF(n, d->root) {
		if (!n->client || !scroller_is_tiled(n->client))
			continue;
		scroller_add_tile(s, n->client, false);
	}

	// activate the currently focused window
	if (d->focus && d->focus->client) {
		int col, tile;
		if (find_tile(s, d->focus->client, &col, &tile)) {
			s->active_column_idx = col;
			s->columns[col].active_tile_idx = tile;
		}
	}
}

void scroller_arrange(struct output_t *m, desktop_t *d, struct wlr_box available) {
	if (!d)
		return;

	// lazily create scroller state
	if (!d->scroller_state)
		d->scroller_state = scroller_create();

	scroller_state_t *s = d->scroller_state;
	if (!s)
		return;

	struct wlr_box area = scroller_working_area(available);
	s->working_area = area;

	// populate from BSP tree if scroller state is empty but toplevels exist
	if (s->column_count == 0 && d->root)
		populate_from_tree(s, d);

	if (s->column_count == 0)
		return;

	int gap = compute_window_gap(d);
	if (gap < 0)
		gap = 0;
	s->gap = (double)gap;

	double area_w = max_d(1.0, (double)area.width);
	double area_h = max_d(1.0, (double)area.height);

	scroller_resolve_widths(s, (double)gap, area_w);

	// compute tile heights and positions (world space, relative to area origin)
	for (int i = 0; i < s->column_count; i++) {
		scroller_column_t *col = &s->columns[i];
		if (col->tile_count == 0)
			continue;

		int shown_count = 0;
		double col_avail_h = area_h;
		double total_weight = 0.0;
		double fixed_h = 0.0;

		for (int j = 0; j < col->tile_count; j++) {
			if (!scroller_tile_shown(&col->tiles[j]))
				continue;

			shown_count++;
			if (col->tiles[j].height.type == SCROLLER_HEIGHT_FIXED) {
				fixed_h += col->tiles[j].height.value;
			} else if (col->tiles[j].height.type == SCROLLER_HEIGHT_PROPORTION) {
				// proportions are absolute, so they are not part of the leftover pool
				fixed_h += resolve_size((scroller_size_t){
					SCROLLER_SIZE_PROPORTION,
					col->tiles[j].height.value
				}, (double)gap, col_avail_h);
			} else {
				total_weight += col->tiles[j].height.value;
			}
		}

		if (shown_count == 0)
			continue;

		double gaps_total = (double)gap * (shown_count + 1);
		double auto_avail = col_avail_h - gaps_total - fixed_h;

		// distribute auto height
		double *tile_heights = calloc((size_t)col->tile_count, sizeof(*tile_heights));
		if (!tile_heights)
			return;

		double total_weight_safe = total_weight > 0.0 ? total_weight : 1.0;
		for (int j = 0; j < col->tile_count; j++) {
			if (!scroller_tile_shown(&col->tiles[j]))
				continue;

			if (col->tiles[j].height.type == SCROLLER_HEIGHT_FIXED) {
				tile_heights[j] = col->tiles[j].height.value;
			} else if (col->tiles[j].height.type == SCROLLER_HEIGHT_PROPORTION) {
				tile_heights[j] = resolve_size((scroller_size_t){
					SCROLLER_SIZE_PROPORTION,
					col->tiles[j].height.value
				}, (double)gap, col_avail_h);
			} else {
				double weight = col->tiles[j].height.value;
				double h = auto_avail * (weight / total_weight_safe);
				tile_heights[j] = max_d(1.0, h);
			}
		}

		// compute y positions
		double y = (double)gap;
		for (int j = 0; j < col->tile_count; j++) {
			if (!scroller_tile_shown(&col->tiles[j])) {
				col->tiles[j].rect = (struct wlr_box){0};
				continue;
			}

			col->tiles[j].rect.x = 0; // local X within column (centering not yet)
			col->tiles[j].rect.y = (int)y;
			col->tiles[j].rect.width = (int)col->resolved_width;
			col->tiles[j].rect.height = max_i(1, (int)tile_heights[j]);
			y += tile_heights[j] + (double)gap;
		}

		free(tile_heights);
	}

	// compute world-space column X positions
	double *col_xs = malloc((size_t)(s->column_count + 1) * sizeof(*col_xs));
	double *layout_widths = malloc((size_t)s->column_count * sizeof(*layout_widths));
	if (!col_xs || !layout_widths) {
		free(col_xs);
		free(layout_widths);
		return;
	}

	for (int i = 0; i < s->column_count; i++)
		layout_widths[i] = s->columns[i].resolved_width;
	scroller_view_layout(layout_widths, s->column_count, (double)gap, col_xs, layout_widths);
	free(layout_widths);

	// view position (camera in world space)
	double active_col_x = col_xs[scroller_camera_column(s)];
	double view_pos = active_col_x + s->view_offset;

	// set pending rectangles on tree nodes.
	unsigned int bw = (unsigned int)effective_border_width(d);

	int maximized_col = -1, maximized_tile = -1;
	for (int i = 0; i < s->column_count && maximized_col < 0; i++) {
		for (int j = 0; j < s->columns[i].tile_count; j++) {
			if (!scroller_tile_shown(&s->columns[i].tiles[j]))
				continue;

			if (client_is_maximized(s->columns[i].tiles[j].client)) {
				maximized_col = i;
				maximized_tile = j;
				break;
			}
		}
	}

	if (maximized_col >= 0)
		view_pos = col_xs[maximized_col];

	for (int i = 0; i < s->column_count; i++) {
		scroller_column_t *col = &s->columns[i];
		double screen_x = (double)area.x + col_xs[i] - view_pos;

		for (int j = 0; j < col->tile_count; j++) {
			client_t *c = col->tiles[j].client;
			if (!c || !c->view || !c->view->node)
				continue;

			node_t *node = c->view->node;

			if (!scroller_tile_shown(&col->tiles[j])) {
				col->tiles[j].rect = (struct wlr_box){0};
				c->tiled_rectangle = (struct wlr_box){0};
				c->arranged_rectangle = (struct wlr_box){0};
				node_set_pending_rectangle(node, (struct wlr_box){0});
				node->output = m;
				node_set_dirty(node);
				continue;
			}

			bool is_maximized = (i == maximized_col && j == maximized_tile);
			if (maximized_col >= 0 && !is_maximized) {
				c->tiled_rectangle = (struct wlr_box){0};
				c->arranged_rectangle = (struct wlr_box){0};
				node_set_pending_rectangle(node, (struct wlr_box){0});
				node->output = m;
				node_set_dirty(node);
				continue;
			}

			struct wlr_box outer;
			if (is_maximized) {
				int mx = available.x + (int)gap;
				int my = available.y + (int)gap;
				outer = (struct wlr_box){
					.x = mx,
					.y = my,
					.width = max_i(1, available.width - (int)gap * 2),
					.height = max_i(1, available.height - (int)gap * 2),
				};
			} else {
				outer = (struct wlr_box){
					.x = (int)(screen_x + 0.5),
					.y = (int)((double)area.y + col->tiles[j].rect.y + 0.5),
					.width = max_i(1, (int)(col->resolved_width + 0.5)),
					.height = max_i(1, col->tiles[j].rect.height),
				};
			}

			struct wlr_box inner = apply_bleed(outer, (int)bw, 0);
			if (inner.width < SCROLLER_MIN_WIDTH)
				inner.width = SCROLLER_MIN_WIDTH;
			if (inner.width > outer.width)
				inner.width = outer.width;
			if (inner.height < SCROLLER_MIN_HEIGHT)
				inner.height = SCROLLER_MIN_HEIGHT;
			if (inner.height > outer.height)
				inner.height = outer.height;

			c->tiled_rectangle = inner;
			c->arranged_rectangle = inner;
			node_set_pending_rectangle(node, outer);
			node->output = m;
			node_set_dirty(node);
		}
	}

	free(col_xs);
}

// promote a tile into the camera and make it its own view while it is maximized
void scroller_set_maximized(desktop_t *d, node_t *n, bool value) {
	scroller_state_t *s = d != NULL ? d->scroller_state : NULL;
	if (s == NULL || n == NULL || n->client == NULL)
		return;

	int col_idx, tile_idx;
	if (!find_tile(s, n->client, &col_idx, &tile_idx))
		return;

	if (value) {
		int prev = s->active_column_idx;
		s->active_column_idx = col_idx;
		s->columns[col_idx].active_tile_idx = tile_idx;
		s->view_offset = 0.0;
		s->columns[col_idx].width.type = SCROLLER_SIZE_PROPORTION;
		s->columns[col_idx].width.value = 1.0;
		scroller_recenter_view(s, prev);
	} else {
		s->columns[col_idx].width = s->default_column_width;
	}
}

void scroller_sync_focus(scroller_state_t *s, client_t *c) {
	if (s == NULL || c == NULL)
		return;

	int col_idx, tile_idx;
	if (!find_tile(s, c, &col_idx, &tile_idx))
		return;

	if (col_idx == s->active_column_idx) {
		s->columns[col_idx].active_tile_idx = tile_idx;
		return;
	}

	int prev = s->active_column_idx;
	s->active_column_idx = col_idx;
	s->columns[col_idx].active_tile_idx = tile_idx;
	scroller_recenter_view(s, prev);
}

void scroller_apply_active_focus(desktop_t *d, struct output_t *m) {
	scroller_state_t *s = d->scroller_state;
	if (!s || s->column_count == 0) {
		d->focus = NULL;
		return;
	}

	scroller_column_t *col = &s->columns[clamp_i(s->active_column_idx, 0, s->column_count - 1)];
	if (col->tile_count == 0) {
		d->focus = NULL;
		return;
	}

	int tile_idx = clamp_i(col->active_tile_idx, 0, col->tile_count - 1);
	client_t *c = col->tiles[tile_idx].client;
	if (c && c->view && c->view->node) {
		node_t *target = c->view->node;
		d->focus = target;
		output_t *out = d->output ? d->output : m;
		if (out)
			focus_node(out, d, target);
	}
}


#define VIEW_GESTURE_WORKING_AREA_MOVEMENT 300.

void scroller_view_offset_gesture_begin(desktop_t *d, bool is_touchpad) {
	scroller_state_t *s = d ? d->scroller_state : NULL;
	if (!s || s->column_count == 0)
		return;
	(void)is_touchpad;
}

void scroller_view_offset_gesture_update(desktop_t *d, double delta_x) {
	scroller_state_t *s = d ? d->scroller_state : NULL;
	if (!s || s->column_count == 0)
		return;

	double area_w = (double)s->working_area.width;
	if (area_w <= 1.0)
		area_w = 1.0;

	double norm_factor = area_w / VIEW_GESTURE_WORKING_AREA_MOVEMENT;
	if (norm_factor < 5.0)
		norm_factor = 5.0;

	scroller_view_t v;
	if (!scroller_view_setup(s, &v))
		return;

	s->view_offset += delta_x * norm_factor;

	int cam = clamp_i(scroller_camera_column(s), 0, s->column_count - 1);
	scroller_view_offset_clamp(&v, v.xs[cam], &s->view_offset);

	scroller_view_release(&v);
}

bool scroller_view_offset_gesture_end(desktop_t *d) {
	scroller_state_t *s = d ? d->scroller_state : NULL;
	if (!s || s->column_count == 0)
		return false;

	double area_w = (double)s->working_area.width;
	if (area_w <= 1.0)
		area_w = 1.0;

	scroller_view_t v;
	if (!scroller_view_setup(s, &v))
		return false;

	int cam = clamp_i(scroller_camera_column(s), 0, s->column_count - 1);
	double view_pos = v.xs[cam] + s->view_offset;

	int nearest = cam;
	double nearest_dist = fabs(view_pos - v.xs[cam]);
	double best_snap = v.xs[cam];

	bool centering = scroller_is_centering(s);

	for (int i = 0; i < s->column_count; i++) {
		if (v.widths[i] <= 0.0)
			continue;

		double snap = centering ? v.xs[i] + scroller_view_offset_centered(&v, view_pos, v.xs[i],
			v.widths[i]) : v.xs[i] + scroller_view_offset_fit(&v, view_pos, v.xs[i], v.widths[i]);

		double dist = fabs(view_pos - snap);
		if (dist < nearest_dist) {
			nearest_dist = dist;
			nearest = i;
			best_snap = snap;
		}
	}

	nearest = clamp_i(nearest, 0, s->column_count - 1);

	if (nearest != s->active_column_idx) {
		s->active_column_idx = nearest;
		s->activate_prev_column_on_removal = false;
	}

	s->view_offset = best_snap - v.xs[nearest];

	scroller_view_release(&v);
	return true;
}

static bool scroller_tile_focusable(const scroller_tile_t *t) {
	return scroller_tile_shown(t) && t->client->view != NULL;
}

static bool scroller_column_focusable(const scroller_column_t *col) {
	for (int j = 0; j < col->tile_count; j++)
		if (scroller_tile_focusable(&col->tiles[j]))
			return true;

	return false;
}

static bool scroller_activate_column(desktop_t *d, int idx) {
	scroller_state_t *s = d->scroller_state;
	if (!s || idx < 0 || idx >= s->column_count)
		return false;

	int prev = s->active_column_idx;
	s->active_column_idx = idx;
	s->activate_prev_column_on_removal = false;
	scroller_recenter_view(s, prev);
	return true;
}

static int scroller_search_column(scroller_state_t *s, int from, int step) {
	for (int i = 0; i < s->column_count; i++) {
		int idx = from + step * (i + 1);
		idx = (idx % s->column_count + s->column_count) % s->column_count;
		if (scroller_column_focusable(&s->columns[idx]))
			return idx == from ? -1 : idx;
	}

	return -1;
}

bool scroller_focus_next(desktop_t *d) {
	scroller_state_t *s = d->scroller_state;
	if (!s || s->column_count == 0)
		return false;

	int idx = scroller_search_column(s, s->active_column_idx, 1);
	if (idx < 0)
		return false;

	if (!scroller_activate_column(d, idx))
		return false;

	scroller_apply_active_focus(d, NULL);
	return true;
}

bool scroller_focus_prev(desktop_t *d) {
	scroller_state_t *s = d->scroller_state;
	if (!s || s->column_count == 0)
		return false;

	int idx = scroller_search_column(s, s->active_column_idx, -1);
	if (idx < 0)
		return false;

	if (!scroller_activate_column(d, idx))
		return false;

	scroller_apply_active_focus(d, NULL);
	return true;
}

static bool scroller_focus_column_edge(desktop_t *d, bool first) {
	scroller_state_t *s = d->scroller_state;
	if (!s || s->column_count == 0)
		return false;

	int idx = -1;
	if (first) {
		for (int i = 0; i < s->column_count; i++) {
			if (scroller_column_focusable(&s->columns[i])) {
				idx = i;
				break;
			}
		}
	} else {
		for (int i = s->column_count - 1; i >= 0; i--) {
			if (scroller_column_focusable(&s->columns[i])) {
				idx = i;
				break;
			}
		}
	}

	if (idx < 0 || idx == s->active_column_idx)
		return false;

	if (!scroller_activate_column(d, idx))
		return false;

	scroller_apply_active_focus(d, NULL);
	return true;
}

bool scroller_focus_column_first(desktop_t *d) {
	return scroller_focus_column_edge(d, true);
}

bool scroller_focus_column_last(desktop_t *d) {
	return scroller_focus_column_edge(d, false);
}

static bool scroller_focus_or_sideways(desktop_t *d, bool try_down, bool side_right) {
	if (try_down ? scroller_focus_down(d) : scroller_focus_up(d))
		return true;

	return side_right ? scroller_focus_next(d) : scroller_focus_prev(d);
}

bool scroller_focus_down_or_left(desktop_t *d) {
	return scroller_focus_or_sideways(d, true, false);
}

bool scroller_focus_down_or_right(desktop_t *d) {
	return scroller_focus_or_sideways(d, true, true);
}

bool scroller_focus_up_or_left(desktop_t *d) {
	return scroller_focus_or_sideways(d, false, false);
}

bool scroller_focus_up_or_right(desktop_t *d) {
	return scroller_focus_or_sideways(d, false, true);
}

bool scroller_focus_down(desktop_t *d) {
	scroller_state_t *s = d->scroller_state;
	if (!s || s->column_count == 0)
		return false;

	scroller_column_t *col = &s->columns[clamp_i(s->active_column_idx, 0, s->column_count - 1)];
	if (col->tile_count == 0)
		return false;
	if (col->active_tile_idx >= col->tile_count - 1)
		return false;

	int idx = col->active_tile_idx;
	for (int step = 0; step < col->tile_count; step++) {
		idx++;
		if (idx >= col->tile_count)
			return false;
		if (scroller_tile_focusable(&col->tiles[idx]))
			break;
	}

	col->active_tile_idx = idx;
	scroller_apply_active_focus(d, NULL);
	return true;
}

bool scroller_swap(struct output_t *m, desktop_t *d, direction_t dir) {
	scroller_state_t *s = d ? d->scroller_state : NULL;
	if (!s || s->column_count == 0)
		return false;
	if (!d->focus || !d->focus->client)
		return false;

	int src_col, src_tile;
	if (!find_tile(s, d->focus->client, &src_col, &src_tile))
		return false;

	int dst_col = src_col;
	int dst_tile = src_tile;

	switch (dir) {
	case DIR_WEST:
		for (dst_col = src_col - 1; dst_col >= 0; dst_col--) {
			if (!scroller_column_focusable(&s->columns[dst_col]))
				continue;
			for (dst_tile = s->columns[dst_col].tile_count - 1; dst_tile >= 0; dst_tile--)
				if (scroller_tile_focusable(&s->columns[dst_col].tiles[dst_tile]))
					break;
			break;
		}
		if (dst_col < 0)
			return false;
		break;
	case DIR_EAST:
		for (dst_col = src_col + 1; dst_col < s->column_count; dst_col++) {
			if (!scroller_column_focusable(&s->columns[dst_col]))
				continue;
			for (dst_tile = 0; dst_tile < s->columns[dst_col].tile_count; dst_tile++)
				if (scroller_tile_focusable(&s->columns[dst_col].tiles[dst_tile]))
					break;
			break;
		}
		if (dst_col >= s->column_count)
			return false;
		break;
	case DIR_NORTH:
		for (dst_tile = src_tile - 1; dst_tile >= 0; dst_tile--)
			if (scroller_tile_focusable(&s->columns[src_col].tiles[dst_tile]))
				break;
		if (dst_tile < 0)
			return false;
		break;
	case DIR_SOUTH:
		for (dst_tile = src_tile + 1; dst_tile < s->columns[src_col].tile_count; dst_tile++)
			if (scroller_tile_focusable(&s->columns[src_col].tiles[dst_tile]))
				break;
		if (dst_tile >= s->columns[src_col].tile_count)
			return false;
		break;
	default:
		return false;
	}

	scroller_column_t *src = &s->columns[src_col];
	scroller_column_t *dst = &s->columns[dst_col];
	if (dst_tile < 0 || dst_tile >= dst->tile_count)
		return false;

	scroller_tile_t tmp = src->tiles[src_tile];
	src->tiles[src_tile] = dst->tiles[dst_tile];
	dst->tiles[dst_tile] = tmp;

	// keep focus on the window that was swapped
	int prev = s->active_column_idx;
	s->active_column_idx = dst_col;
	s->columns[dst_col].active_tile_idx = dst_tile;
	s->activate_prev_column_on_removal = false;
	scroller_recenter_view(s, prev);

	if (m)
		arrange(m, d, true);
	return true;
}

bool scroller_focus_up(desktop_t *d) {
	scroller_state_t *s = d->scroller_state;
	if (!s || s->column_count == 0)
		return false;

	scroller_column_t *col = &s->columns[clamp_i(s->active_column_idx, 0, s->column_count - 1)];
	if (col->tile_count == 0)
		return false;
	if (col->active_tile_idx == 0)
		return false;

	int idx = col->active_tile_idx;
	for (int step = 0; step < col->tile_count; step++) {
		idx--;
		if (idx < 0)
			return false;
		if (scroller_tile_focusable(&col->tiles[idx]))
			break;
	}

	col->active_tile_idx = idx;
	scroller_apply_active_focus(d, NULL);
	return true;
}

void scroller_center_window(desktop_t *d, client_t *client) {
	scroller_state_t *s = d->scroller_state;
	if (!s || !client)
		return;

	int col_idx, tile_idx;
	if (!find_tile(s, client, &col_idx, &tile_idx))
		return;

	int prev = s->active_column_idx;
	s->active_column_idx = col_idx;
	s->columns[col_idx].active_tile_idx = tile_idx;
	s->activate_prev_column_on_removal = false;
	scroller_recenter_view(s, prev);
	scroller_apply_active_focus(d, NULL);
}

void scroller_center_visible_columns(desktop_t *d) {
	scroller_state_t *s = d ? d->scroller_state : NULL;
	if (!s || s->column_count == 0)
		return;

	double area_w = (double)s->working_area.width;
	if (area_w <= 1.0)
		return;

	scroller_view_t v;
	if (!scroller_view_setup(s, &v))
		return;

	int cam = clamp_i(scroller_camera_column(s), 0, s->column_count - 1);
	double view_pos = v.xs[cam] + s->view_offset;

	double left = view_pos;
	double right = view_pos + v.view_width;
	for (int i = 0; i < s->column_count; i++) {
		double w = v.widths[i];
		if (w <= 0.0)
			continue;

		double col_left = v.xs[i];
		double col_right = col_left + w;
		if (col_right < view_pos || col_left > right)
			continue;

		left = min_d(left, col_left);
		right = max_d(right, col_right);
	}

	double span = right - left;
	s->view_offset = left - (v.view_width - span) / 2.0 - v.xs[cam];

	scroller_view_release(&v);
}

bool scroller_consume_into_column(desktop_t *d) {
	scroller_state_t *s = d ? d->scroller_state : NULL;
	if (!s || s->column_count < 2)
		return false;

	int col = s->active_column_idx;
	if (col == 0)
		return false;

	scroller_column_t *src = &s->columns[col];
	if (src->tile_count == 0)
		return false;

	client_t *cl = src->tiles[src->active_tile_idx].client;
	if (!cl)
		return false;

	scroller_remove_tile(s, cl, NULL);

	int target_col = col - 1;
	if (target_col >= s->column_count)
		return false;

	bool result = scroller_add_tile_to_column(s, cl, target_col, true);
	if (result)
		s->activate_prev_column_on_removal = true;
	return result;
}

bool scroller_expel_from_column(desktop_t *d) {
	scroller_state_t *s = d ? d->scroller_state : NULL;
	if (!s || s->column_count == 0)
		return false;

	int col = s->active_column_idx;
	scroller_column_t *src = &s->columns[col];
	if (src->tile_count < 2)
		return false;

	client_t *cl = src->tiles[src->active_tile_idx].client;
	if (!cl)
		return false;

	scroller_remove_tile(s, cl, NULL);
	return scroller_add_tile(s, cl, true);
}

static bool scroller_move_column_to_index(scroller_state_t *s, int index) {
	if (s == NULL || s->column_count == 0)
		return false;

	index = clamp_i(index, 0, s->column_count - 1);
	int from = s->active_column_idx;
	if (from < 0 || from >= s->column_count || from == index)
		return false;

	scroller_column_t col = s->columns[from];
	if (from < index)
		memmove(&s->columns[from], &s->columns[from + 1],
			(size_t)(index - from) * sizeof(scroller_column_t));
	else
		memmove(&s->columns[index + 1], &s->columns[index],
			(size_t)(from - index) * sizeof(scroller_column_t));

	s->columns[index] = col;
	s->active_column_idx = index;
	return true;
}

static bool scroller_move_column_sideways(scroller_state_t *s, int dir) {
	if (s == NULL || s->column_count == 0)
		return false;

	int from = s->active_column_idx;
	int to = from + dir;
	if (to < 0 || to >= s->column_count)
		return false;

	return scroller_move_column_to_index(s, to);
}

bool scroller_move_column_left(desktop_t *d) {
	scroller_state_t *s = d ? d->scroller_state : NULL;
	return scroller_move_column_sideways(s, -1);
}

bool scroller_move_column_right(desktop_t *d) {
	scroller_state_t *s = d ? d->scroller_state : NULL;
	return scroller_move_column_sideways(s, 1);
}

bool scroller_move_column_to_first(desktop_t *d) {
	scroller_state_t *s = d ? d->scroller_state : NULL;
	return s != NULL && scroller_move_column_to_index(s, 0);
}

bool scroller_move_column_to_last(desktop_t *d) {
	scroller_state_t *s = d ? d->scroller_state : NULL;
	return s != NULL && s->column_count > 0 && scroller_move_column_to_index(s, s->column_count - 1);
}

static bool scroller_move_tile_vertically(scroller_state_t *s, int dir) {
	if (s == NULL || s->column_count == 0)
		return false;

	scroller_column_t *col = &s->columns[clamp_i(s->active_column_idx, 0, s->column_count - 1)];
	if (col->tile_count == 0)
		return false;

	int from = clamp_i(col->active_tile_idx, 0, col->tile_count - 1);
	int to = from + dir;
	if (to < 0 || to >= col->tile_count)
		return false;

	scroller_tile_t tile = col->tiles[from];
	memmove(&col->tiles[from], &col->tiles[from + 1], (size_t)(to - from) * sizeof(scroller_tile_t));
	col->tiles[to] = tile;
	col->active_tile_idx = to;
	return true;
}

bool scroller_move_column_up(desktop_t *d) {
	scroller_state_t *s = d ? d->scroller_state : NULL;
	return scroller_move_tile_vertically(s, -1);
}

bool scroller_move_column_down(desktop_t *d) {
	scroller_state_t *s = d ? d->scroller_state : NULL;
	return scroller_move_tile_vertically(s, 1);
}

bool scroller_is_tiled(const client_t *c) {
	if (!c)
		return false;
	return c->state == STATE_TILED || c->state == STATE_PSEUDO_TILED;
}

void scroller_apply_client_rules(client_t *c, float rule_proportion, float rule_proportion_single) {
	if (!c)
		return;
	(void)c;
	(void)rule_proportion;
	(void)rule_proportion_single;
	// TODO: store per-client proportion overrides if needed later.
}

bool scroller_set_column_width(desktop_t *d, scroller_size_t size) {
	scroller_state_t *s = d ? d->scroller_state : NULL;
	if (!s || s->column_count == 0)
		return false;

	int col = clamp_i(s->active_column_idx, 0, s->column_count - 1);
	s->columns[col].width = size;
	return true;
}

bool scroller_resize_width(desktop_t *d, float delta) {
	scroller_state_t *s = d ? d->scroller_state : NULL;
	if (!s || s->column_count == 0)
		return false;

	int col = clamp_i(s->active_column_idx, 0, s->column_count - 1);
	double prop = s->columns[col].width.value + (double)delta;
	prop = clamp_d(prop, 0.1, 1.0);

	s->columns[col].width.type = SCROLLER_SIZE_PROPORTION;
	s->columns[col].width.value = prop;
	return true;
}

static int scroller_next_preset(const scroller_size_t *presets, int count, scroller_size_t current,
		int step) {
	for (int i = 0; i < count; i++) {
		bool match = presets[i].type == current.type && fabs(presets[i].value - current.value) < 0.01;
		if (!match)
			continue;

		int next = i + step;
		if (next < 0 || next >= count)
			return -1;

		return next;
	}

	return step > 0 ? 0 : count - 1;
}

bool scroller_cycle_width_preset(desktop_t *d, int step) {
	scroller_state_t *s = d ? d->scroller_state : NULL;
	if (!s || s->column_count == 0)
		return false;

	const scroller_size_t *presets = settings.scroller_preset_column_widths;
	int count = settings.scroller_preset_column_widths_count;
	if (!presets || count == 0 || step == 0)
		return false;

	int col = clamp_i(s->active_column_idx, 0, s->column_count - 1);
	int next = scroller_next_preset(presets, count, s->columns[col].width, step);
	if (next < 0)
		return false;

	s->columns[col].width = presets[next];
	return true;
}

bool scroller_toggle_column_full_width(desktop_t *d) {
	scroller_state_t *s = d ? d->scroller_state : NULL;
	if (!s || s->column_count == 0)
		return false;

	int col = clamp_i(s->active_column_idx, 0, s->column_count - 1);
	scroller_column_t *c = &s->columns[col];

	if (c->width.type == SCROLLER_SIZE_PROPORTION && c->width.value >= 1.0) {
		c->width = c->saved_width;
		return true;
	}

	if (c->saved_width.type == SCROLLER_SIZE_PROPORTION && c->saved_width.value <= 0.0)
		c->saved_width = c->width;

	c->width.type = SCROLLER_SIZE_PROPORTION;
	c->width.value = 1.0;
	return true;
}

bool scroller_expand_column_to_available_width(desktop_t *d) {
	scroller_state_t *s = d ? d->scroller_state : NULL;
	if (!s || s->column_count == 0)
		return false;

	double area_w = (double)s->working_area.width;
	if (area_w <= 1.0)
		return false;

	scroller_view_t v;
	if (!scroller_view_setup(s, &v))
		return false;

	int col = clamp_i(s->active_column_idx, 0, s->column_count - 1);
	double view_pos = v.xs[col] + s->view_offset;
	double gap = v.gap;

	double right_edge = view_pos + v.view_width;
	bool any = false;
	for (int i = 0; i < s->column_count; i++) {
		double w = v.widths[i];
		if (w <= 0.0)
			continue;

		if (v.xs[i] >= right_edge - gap)
			continue;

		if (i != col && v.xs[i] + w <= view_pos + gap)
			continue;

		any = true;
		if (i != col)
			right_edge = max_d(right_edge, v.xs[i] + w);
	}

	double grown = right_edge - v.xs[col] + gap;
	scroller_view_release(&v);

	if (!any || grown >= area_w)
		return scroller_toggle_column_full_width(d);

	if (s->columns[col].saved_width.type == SCROLLER_SIZE_PROPORTION &&
		s->columns[col].saved_width.value <= 0.0)
		s->columns[col].saved_width = s->columns[col].width;

	s->columns[col].width.type = SCROLLER_SIZE_PROPORTION;
	s->columns[col].width.value = (grown + gap) / (area_w - gap);
	return true;
}

static scroller_column_t *scroller_active_column(scroller_state_t *s) {
	if (!s || s->column_count == 0)
		return NULL;

	return &s->columns[clamp_i(s->active_column_idx, 0, s->column_count - 1)];
}

bool scroller_set_window_height(desktop_t *d, scroller_size_t size) {
	scroller_column_t *col = scroller_active_column(d ? d->scroller_state : NULL);
	if (col == NULL || col->tile_count == 0)
		return false;

	int idx = clamp_i(col->active_tile_idx, 0, col->tile_count - 1);

	switch (size.type) {
	case SCROLLER_SIZE_FIXED:
		col->tiles[idx].height.type = SCROLLER_HEIGHT_FIXED;
		col->tiles[idx].height.value = max_d(1.0, size.value);
		break;
	case SCROLLER_SIZE_PROPORTION:
		col->tiles[idx].height.type = SCROLLER_HEIGHT_PROPORTION;
		col->tiles[idx].height.value = clamp_d(size.value, 0.05, 1.0);
		break;
	}

	return true;
}

bool scroller_reset_window_height(desktop_t *d) {
	scroller_column_t *col = scroller_active_column(d ? d->scroller_state : NULL);
	if (col == NULL || col->tile_count == 0)
		return false;

	int idx = clamp_i(col->active_tile_idx, 0, col->tile_count - 1);
	col->tiles[idx].height.type = SCROLLER_HEIGHT_AUTO;
	col->tiles[idx].height.value = 1.0;
	return true;
}

bool scroller_resize_stack(desktop_t *d, float delta) {
	scroller_column_t *col = scroller_active_column(d ? d->scroller_state : NULL);
	if (col == NULL || col->tile_count == 0)
		return false;

	int tile_idx = clamp_i(col->active_tile_idx, 0, col->tile_count - 1);

	if (col->tiles[tile_idx].height.type != SCROLLER_HEIGHT_FIXED) {
		col->tiles[tile_idx].height.type = SCROLLER_HEIGHT_FIXED;
		col->tiles[tile_idx].height.value = (double)col->tiles[tile_idx].rect.height;
	}

	double h = col->tiles[tile_idx].height.value + (double)delta;
	col->tiles[tile_idx].height.value = max_d(1.0, h);
	return true;
}

bool scroller_cycle_height_preset(desktop_t *d, int step) {
	scroller_column_t *col = scroller_active_column(d ? d->scroller_state : NULL);
	if (col == NULL || col->tile_count == 0)
		return false;

	const scroller_size_t *presets = settings.scroller_preset_window_heights;
	int count = settings.scroller_preset_window_heights_count;
	if (!presets || count == 0 || step == 0)
		return false;

	int idx = clamp_i(col->active_tile_idx, 0, col->tile_count - 1);
	scroller_window_height_t current = col->tiles[idx].height;

	int next = -1;
	for (int i = 0; i < count; i++) {
		scroller_height_type_t want = presets[i].type == SCROLLER_SIZE_FIXED ? SCROLLER_HEIGHT_FIXED :
			SCROLLER_HEIGHT_PROPORTION;

		if (want != current.type || fabs(presets[i].value - current.value) >= 0.01)
			continue;

		next = i + step;
		if (next < 0 || next >= count)
			return false;
		break;
	}

	if (next < 0)
		next = step > 0 ? 0 : count - 1;

	return scroller_set_window_height(d, presets[next]);
}

bool scroller_bind_action(desktop_t *d, int action) {
	scroller_state_t *s = d ? d->scroller_state : NULL;
	if (s == NULL || s->column_count == 0)
		return false;

	switch (action) {
	case BIND_SCROLLER_CYCLE_PRESET:
		return scroller_cycle_width_preset(d, 1);
	case BIND_SCROLLER_CYCLE_PRESET_BACK:
		return scroller_cycle_width_preset(d, -1);
	case BIND_SCROLLER_CYCLE_HEIGHT:
		return scroller_cycle_height_preset(d, 1);
	case BIND_SCROLLER_CYCLE_HEIGHT_BACK:
		return scroller_cycle_height_preset(d, -1);
	case BIND_SCROLLER_TOGGLE_FULL_WIDTH:
		return scroller_toggle_column_full_width(d);
	case BIND_SCROLLER_EXPAND_COLUMN:
		return scroller_expand_column_to_available_width(d);
	case BIND_SCROLLER_CENTER:
		if (!d->focus || !d->focus->client)
			return false;

		scroller_center_window(d, d->focus->client);
		return true;
	case BIND_SCROLLER_CENTER_VISIBLE:
		scroller_center_visible_columns(d);
		return true;
	case BIND_FOCUS_COLUMN_FIRST:
		return scroller_focus_column_first(d);
	case BIND_FOCUS_COLUMN_LAST:
		return scroller_focus_column_last(d);
	case BIND_FOCUS_DOWN_OR_LEFT:
		return scroller_focus_down_or_left(d);
	case BIND_FOCUS_DOWN_OR_RIGHT:
		return scroller_focus_down_or_right(d);
	case BIND_FOCUS_UP_OR_LEFT:
		return scroller_focus_up_or_left(d);
	case BIND_FOCUS_UP_OR_RIGHT:
		return scroller_focus_up_or_right(d);
	case BIND_MOVE_COLUMN_LEFT:
		return scroller_move_column_left(d);
	case BIND_MOVE_COLUMN_RIGHT:
		return scroller_move_column_right(d);
	case BIND_MOVE_COLUMN_UP:
		return scroller_move_column_up(d);
	case BIND_MOVE_COLUMN_DOWN:
		return scroller_move_column_down(d);
	case BIND_MOVE_COLUMN_FIRST:
		return scroller_move_column_to_first(d);
	case BIND_MOVE_COLUMN_LAST:
		return scroller_move_column_to_last(d);
	}

	return false;
}

int scroller_collect(desktop_t *d, node_t ***out_nodes) {
	scroller_state_t *s = d ? d->scroller_state : NULL;
	if (!s || s->column_count == 0) {
		if (out_nodes)
			*out_nodes = NULL;
		return 0;
	}

	// count total tiles
	int total = 0;
	for (int i = 0; i < s->column_count; i++)
		total += s->columns[i].tile_count;

	if (total == 0) {
		if (out_nodes)
			*out_nodes = NULL;
		return 0;
	}

	node_t **nodes = calloc((size_t)total, sizeof(*nodes));
	if (!nodes) {
		if (out_nodes)
			*out_nodes = NULL;
		return 0;
	}

	int idx = 0;
	for (int i = 0; i < s->column_count; i++) {
		for (int j = 0; j < s->columns[i].tile_count; j++) {
			client_t *c = s->columns[i].tiles[j].client;
			if (c && c->view)
				nodes[idx++] = c->view->node;
		}
	}

	*out_nodes = nodes;
	return idx;
}
