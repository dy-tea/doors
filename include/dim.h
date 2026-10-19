#pragma once

#include <stdbool.h>

struct wlr_scene_buffer;

struct output_t;
struct view_t;

struct dim_scrim_t {
	struct wlr_scene_buffer *node;
	float color[4];
	int buf_width;
	int buf_height;
	float radius;
	bool has_color;
};

// recompute scrims of output
void dim_update_output(struct output_t *output);

// recompute scrims for every output
void dim_update(void);

void dim_update_view(struct view_t *view);

// true when the output's scrim is drawn directly below this view so whatever is
// cut away from its rounded corners darkens with it
bool dim_scrim_below_view(const struct view_t *view, const struct output_t *output);

// release scrims owned by view or output
void dim_destroy_view(struct view_t *view);
void dim_destroy_output(struct output_t *output);

bool dim_output_visible(const struct output_t *output);
