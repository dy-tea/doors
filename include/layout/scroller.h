#pragma once

#include "fwd.h"
#include "types.h"

#include <stdbool.h>
#include <wlr/util/box.h>

typedef enum {
	SCROLLER_SIZE_PROPORTION,
	SCROLLER_SIZE_FIXED,
} scroller_size_type_t;

typedef struct {
	scroller_size_type_t type;
	double value;
} scroller_size_t;

typedef enum {
	SCROLLER_HEIGHT_AUTO,
	SCROLLER_HEIGHT_PROPORTION,
	SCROLLER_HEIGHT_FIXED,
} scroller_height_type_t;

typedef struct {
	scroller_height_type_t type;
	double value;
} scroller_window_height_t;

typedef enum {
	SCROLLER_CENTER_NEVER,
	SCROLLER_CENTER_ALWAYS,
	SCROLLER_CENTER_ON_OVERFLOW,
} scroller_center_mode_t;

#define SCROLLER_MIN_WIDTH 1
#define SCROLLER_MIN_HEIGHT 1

// layout impl
void scroller_leave(struct output_t *m, desktop_t *d);
void scroller_arrange(struct output_t *m, desktop_t *d, struct wlr_box available);
void scroller_set_maximized(const desktop_t *d, const node_t *n, bool value);

// true when desktop uses the scroller layout and state exists
bool scroller_is_active(const desktop_t *d);

// true when `client` is tracked as a tile of the desktop's scroller state
bool scroller_has_tile(const desktop_t *d, const client_t *c);

// the configured width of the column holding `client`, 0 when it has none
double scroller_column_width(const desktop_t *d, const client_t *c);

// adjusts the width of the column holding `client` by drag delta. returns true when updated
bool scroller_resize_column_drag(desktop_t *d, const client_t *c, double delta_x,
	double initial_width_hint);

// tile membership
bool scroller_add_tile(desktop_t *d, client_t *client, bool activate);
bool scroller_add_tile_to_column(desktop_t *d, client_t *client, int col_idx, bool activate);
void scroller_remove_tile(desktop_t *d, client_t *client, const struct output_t *m);

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
double scroller_scroll_amount_to_activate(const desktop_t *d, const client_t *c);
void scroller_center_visible_columns(const desktop_t *d);

bool scroller_move_column_left(desktop_t *d);
bool scroller_move_column_right(desktop_t *d);
bool scroller_move_column_to_first(desktop_t *d);
bool scroller_move_column_to_last(desktop_t *d);
bool scroller_move_column_up(desktop_t *d);
bool scroller_move_column_down(desktop_t *d);

bool scroller_consume_into_column(desktop_t *d);
bool scroller_expel_from_column(desktop_t *d);

bool scroller_is_tiled(const client_t *c);

bool scroller_set_column_width(const desktop_t *d, scroller_size_t size);
bool scroller_resize_width(const desktop_t *d, float delta);
bool scroller_cycle_width_preset(const desktop_t *d, int direction);
bool scroller_toggle_column_full_width(const desktop_t *d);
bool scroller_expand_column_to_available_width(const desktop_t *d);
bool scroller_set_window_height(const desktop_t *d, scroller_size_t size);
bool scroller_reset_window_height(const desktop_t *d);
bool scroller_cycle_height_preset(const desktop_t *d, int direction);

// delta is a fraction of the scroller working area height
bool scroller_resize_stack(desktop_t *d, float delta);

void scroller_apply_active_focus(desktop_t *d, struct output_t *m);

void scroller_sync_focus(desktop_t *d, client_t *c);

// applies the default column width to the desktop's scroller state, if it has one
void scroller_set_default_column_width(const desktop_t *d, scroller_size_t size);

void scroller_view_offset_gesture_begin(desktop_t *d, bool is_touchpad);
void scroller_view_offset_gesture_update(desktop_t *d, double delta_x);
bool scroller_view_offset_gesture_end(desktop_t *d);
