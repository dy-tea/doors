#include "borders.h"
#include "client.h"
#include "macros.h"
#include "protocol/xwayland.h"
#include "settings.h"
#include "str.h"
#include "surface.h"
#include "tree.h"
#include "types.h"
#include <string.h>
#include <wlr/types/wlr_scene.h>

border_state_t get_border_state(client_t *client) {
	border_state_t state = {0};
	state.output = client_get_output(client);
	state.desk = state.output ? state.output->desk : NULL;
	state.is_focused = (state.desk && state.desk->focus && state.desk->focus->client == client);
	state.is_active = (state.desk && state.desk->focus && state.desk->focus->client != NULL);
	return state;
}

void parse_color(const char *hex, float *color) {
	if (!str_parse_color(hex, color)) {
		color[0] = color[1] = color[2] = 0.5f;
		color[3] = 1.0f;
	}
}

static void get_border_color(client_t *client, float *color) {
	if (!client || settings.border_width == 0) {
		color[0] = color[1] = color[2] = 0.0f;
		color[3] = 1.0f;
		return;
	}

	const border_state_t bs = get_border_state(client);

	const bool the_only_window = (wl_list_length(&mon_list) == 1) && bs.desk && bs.desk->root &&
		bs.desk->root->client;
	const bool no_border = (settings.borderless_monocle && bs.desk &&
		bs.desk->layout == LAYOUT_MONOCLE && IS_TILED(client)) || (settings.borderless_singleton &&
		the_only_window) || client->state == STATE_FULLSCREEN;

	if (no_border) {
		color[0] = color[1] = color[2] = 0.0f;
		color[3] = 0.0f;
		return;
	}

	// check if this client's node has an active preselection
	const node_t *n = client_get_node(client);
	if (n && n->presel) {
		memcpy(color, settings.presel_feedback_color_rgba, sizeof(float) * 4);
		return;
	}

	if (bs.is_focused)
		memcpy(color, settings.focused_border_color_rgba, sizeof(float) * 4);
	else if (bs.is_active)
		memcpy(color, settings.active_border_color_rgba, sizeof(float) * 4);
	else
		memcpy(color, settings.normal_border_color_rgba, sizeof(float) * 4);
}

void create_borders(struct wlr_scene_tree *parent, struct wlr_scene_tree **border_tree,
		struct wlr_scene_rect *rects[4]) {
	if (settings.border_width == 0) {
		if (*border_tree) {
			wlr_scene_node_destroy(&(*border_tree)->node);
			*border_tree = NULL;
			for (int i = 0; i < 4; i++)
				rects[i] = NULL;
		}
		return;
	}

	// create border tree container
	*border_tree = wlr_scene_tree_create(parent);
	if (!*border_tree)
		return;

	rects[0] = wlr_scene_rect_create(*border_tree, 0, settings.border_width, COLOR_TRANSPARENT);
	rects[1] = wlr_scene_rect_create(*border_tree, 0, settings.border_width, COLOR_TRANSPARENT);
	rects[2] = wlr_scene_rect_create(*border_tree, settings.border_width, 0, COLOR_TRANSPARENT);
	rects[3] = wlr_scene_rect_create(*border_tree, settings.border_width, 0, COLOR_TRANSPARENT);
}

void destroy_borders(struct wlr_scene_tree **border_tree, struct wlr_scene_rect *rects[4]) {
	if (*border_tree) {
		wlr_scene_node_destroy(&(*border_tree)->node);
		*border_tree = NULL;
		for (int i = 0; i < 4; i++)
			rects[i] = NULL;
	}
}

void update_borders(struct wlr_scene_tree *border_tree, struct wlr_scene_rect *rects[4],
		const struct wlr_box geo, const unsigned int bw, const int off_x, const int off_y) {
	if (!border_tree)
		return;

	if (bw == 0 || geo.width < 1 || geo.height < 1) {
		wlr_scene_node_set_enabled(&border_tree->node, false);
		return;
	}

	wlr_scene_node_set_enabled(&border_tree->node, true);

	const int w = geo.width;
	const int h = geo.height;
	const int bwi = (int)bw;

	wlr_scene_node_set_position(&border_tree->node, off_x - bwi, off_y - bwi);

	// top
	if (rects[0]) {
		wlr_scene_node_set_position(&rects[0]->node, 0, 0);
		wlr_scene_rect_set_size(rects[0], w + 2 * bwi, bwi);
	}

	// bottom
	if (rects[1]) {
		wlr_scene_node_set_position(&rects[1]->node, 0, h + bwi);
		wlr_scene_rect_set_size(rects[1], w + 2 * bwi, bwi);
	}

	// left
	if (rects[2]) {
		wlr_scene_node_set_position(&rects[2]->node, 0, bwi);
		wlr_scene_rect_set_size(rects[2], bwi, h);
	}

	// right
	if (rects[3]) {
		wlr_scene_node_set_position(&rects[3]->node, w + bwi, bwi);
		wlr_scene_rect_set_size(rects[3], bwi, h);
	}
}

void update_border_colors(client_t *client) {
	struct wlr_scene_tree *border_tree = client_border_tree(client);

	if (client && client->state == STATE_FULLSCREEN) {
		surface_rounded_t *rounded = client_get_rounded(client);
		if (rounded) {
			if (rounded->border_shader_node)
				wlr_scene_node_set_enabled(&rounded->border_shader_node->node, false);
			if (rounded->corner_mask_node)
				wlr_scene_node_set_enabled(&rounded->corner_mask_node->node, false);
			// mark dirty so the border re-renders when leaving fullscreen
			if (memcmp(rounded->border_color, COLOR_TRANSPARENT, sizeof(rounded->border_color)) != 0) {
				memcpy(rounded->border_color, COLOR_TRANSPARENT, sizeof(rounded->border_color));
				rounded->border_dirty = true;
				rounded->corner_mask_dirty = true;
			}
		}
		if (border_tree) {
			struct wlr_scene_rect **rects = client_border_rects(client);
			for (int i = 0; i < 4; i++)
				if (rects[i])
					wlr_scene_rect_set_color(rects[i], COLOR_TRANSPARENT);
		}
		return;
	}

	if (settings.border_width == 0 || !border_tree)
		return;

	float color[4];
	get_border_color(client, color);

	struct wlr_scene_rect **rects = client_border_rects(client);

	// determine which gradient set applies to this client
	border_state_t bs = get_border_state(client);

	border_theme_t *bt;
	if (bs.is_focused)
		bt = &settings.focused_border_theme;
	else if (bs.is_active)
		bt = &settings.active_border_theme;
	else
		bt = &settings.normal_border_theme;

	bool has_gradient = (bt->gradient_count >= 2);
	bool use_shader = (has_gradient || (client->border_radius > 0.0f));

	if (use_shader) {
		surface_rounded_t **rounded_ptr = client_rounded_slot(client);

		// the rounded surface owns the border when it exists, otherwise fall
		// back to the four plain rects below
		if (rounded_ptr) {
			surface_update_rounded(rounded_ptr, color, bt);
			if (*rounded_ptr) {
				for (int i = 0; i < 4; i++)
					if (rects[i])
						wlr_scene_rect_set_color(rects[i], COLOR_TRANSPARENT);

				return;
			}
		}
	} else {
		const surface_rounded_t *rounded = client_get_rounded(client);
		if (rounded && rounded->border_shader_node)
			wlr_scene_node_set_enabled(&rounded->border_shader_node->node, false);
	}

	for (int i = 0; i < 4; i++)
		if (rects[i])
			wlr_scene_rect_set_color(rects[i], color);
}

void refresh_border_colors(void) {
	refresh_border_color_cache();
	output_t *m;
	wl_list_for_each(m, &mon_list, link) {
		desktop_t *d;
		wl_list_for_each(d, &m->desk_list, link) {
			if (d->root == NULL)
				continue;

			FOR_EACH_LEAF(n, d->root) {
				if (n->client == NULL)
					continue;

				update_border_colors(n->client);
			}
		}
	}
}
