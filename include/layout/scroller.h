#pragma once

#include "types.h"

#include <stdbool.h>
#include <wlr/util/box.h>

struct output_t;
typedef struct client_t client_t;
typedef struct desktop_t desktop_t;
typedef struct node_t node_t;

#define SCROLLER_MIN_WIDTH 1
#define SCROLLER_MIN_HEIGHT 1

typedef struct scroller_tile_t {
	client_t *client;
	scroller_window_height_t height;
	struct wlr_box rect;
} scroller_tile_t;

typedef struct scroller_column_t {
	scroller_tile_t *tiles;
	int tile_count, capacity, active_tile_idx;
	scroller_size_t width;
	scroller_size_t saved_width;
	double resolved_width;
} scroller_column_t;

typedef struct scroller_state_t {
	scroller_column_t *columns;
	int column_count, capacity, active_column_idx;
	double view_offset;
	struct wlr_box working_area;
	double gap;
	bool activate_prev_column_on_removal;
	scroller_size_t default_column_width;
} scroller_state_t;

scroller_state_t *scroller_create(void);
void scroller_destroy(scroller_state_t *s);

void scroller_leave(struct output_t *m, desktop_t *d);
void scroller_arrange(struct output_t *m, desktop_t *d, struct wlr_box available);
void scroller_set_maximized(desktop_t *d, node_t *n, bool value);

bool scroller_add_tile(scroller_state_t *s, client_t *client, bool activate);
bool scroller_add_tile_to_column(scroller_state_t *s, client_t *client, int col_idx, bool activate);
void scroller_remove_tile(scroller_state_t *s, client_t *client, struct output_t *m);

bool scroller_focus_next(desktop_t *d);
bool scroller_focus_prev(desktop_t *d);
bool scroller_focus_down(desktop_t *d);
bool scroller_focus_up(desktop_t *d);
bool scroller_focus_column_first(desktop_t *d);
bool scroller_focus_column_last(desktop_t *d);
bool scroller_focus_down_or_left(desktop_t *d);
bool scroller_focus_down_or_right(desktop_t *d);
bool scroller_focus_up_or_left(desktop_t *d);
bool scroller_focus_up_or_right(desktop_t *d);
bool scroller_swap(struct output_t *m, desktop_t *d, direction_t dir);
void scroller_center_window(desktop_t *d, client_t *client);
void scroller_center_visible_columns(desktop_t *d);

bool scroller_move_column_left(desktop_t *d);
bool scroller_move_column_right(desktop_t *d);
bool scroller_move_column_to_first(desktop_t *d);
bool scroller_move_column_to_last(desktop_t *d);
bool scroller_move_column_up(desktop_t *d);
bool scroller_move_column_down(desktop_t *d);

bool scroller_consume_into_column(desktop_t *d);
bool scroller_expel_from_column(desktop_t *d);

bool scroller_is_tiled(const client_t *c);
void scroller_apply_client_rules(client_t *c, float rule_proportion, float rule_proportion_single);

bool scroller_set_column_width(desktop_t *d, scroller_size_t size);
bool scroller_resize_width(desktop_t *d, float delta);
bool scroller_cycle_width_preset(desktop_t *d, int direction);
bool scroller_toggle_column_full_width(desktop_t *d);
bool scroller_expand_column_to_available_width(desktop_t *d);
bool scroller_set_window_height(desktop_t *d, scroller_size_t size);
bool scroller_reset_window_height(desktop_t *d);
bool scroller_cycle_height_preset(desktop_t *d, int direction);
bool scroller_resize_stack(desktop_t *d, float delta);

void scroller_apply_active_focus(desktop_t *d, struct output_t *m);

void scroller_sync_focus(scroller_state_t *s, client_t *c);

void scroller_view_offset_gesture_begin(desktop_t *d, bool is_touchpad);
void scroller_view_offset_gesture_update(desktop_t *d, double delta_x);
bool scroller_view_offset_gesture_end(desktop_t *d);

int scroller_collect(desktop_t *d, node_t ***out_nodes);

bool scroller_bind_action(desktop_t *d, int action);
