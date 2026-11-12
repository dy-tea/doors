#pragma once

#include "fwd.h"

#include <wlr/types/wlr_scene.h>

// border gradient theme
#define BORDER_GRADIENT_MAX_STOPS 10
typedef struct {
	float gradient[BORDER_GRADIENT_MAX_STOPS * 4];
	int gradient_count;
	float gradient_angle;
	float gradient2[BORDER_GRADIENT_MAX_STOPS * 4];
	int gradient2_count;
	float gradient2_angle;
	float gradient_lerp;
} border_theme_t;

// which of the three border colour/theme buckets a client currently falls into
typedef struct {
	output_t *output;
	desktop_t *desk;
	bool is_focused;
	bool is_active;
} border_state_t;

border_state_t get_border_state(client_t *client);

// the four-rect border drawn around a client, owned by its scene tree
void create_borders(struct wlr_scene_tree *parent, struct wlr_scene_tree **border_tree,
	struct wlr_scene_rect *rects[4]);
void destroy_borders(struct wlr_scene_tree **border_tree, struct wlr_scene_rect *rects[4]);
void update_borders(struct wlr_scene_tree *border_tree, struct wlr_scene_rect *rects[4],
	struct wlr_box geo, unsigned int bw, int off_x, int off_y);
void update_border_colors(client_t *client);

// reparse the configured colours and repaint every border on every desktop
void refresh_border_colors(void);
