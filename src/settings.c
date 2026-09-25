#include "settings.h"
#include "tree.h"
#include <stdint.h>

// global settings
doors_settings_t settings = {
	.automatic_scheme = SCHEME_SPIRAL,
	.initial_polarity = FIRST_CHILD,
	.borderless_monocle = false,
	.borderless_singleton = false,
	.gapless_monocle = false,
	.removal_adjustment = true,
	.focus_follows_mouse = FOLLOWS_NO,
	.pointer_follows_focus = false,
	.record_history = true,
	.click_to_focus = false,
	.allow_tearing = false,
	.auto_float_dialogs = false,
	.decoration_mode = DECORATION_ALWAYS,
	.enable_animations = false,
	.hide_lone_tab = false,
	.workspace_anim_direction = WORKSPACE_ANIM_VERTICAL,
	.workspace_anim_slide_up = false,
	.mapping_events_count = 0,
	.directional_focus_tightness = 20,
	.ignore_ewmh_fullscreen = 0,
	.idle_timeout = 0,
	.idle_dpms = true,
	.realtime_scheduling = false,
	.monocle_padding = {0},
	.padding = {0},
	.border_width = 2,
	.window_gap = 10,
	.smart_gaps = false,
	.smart_borders = false,
	.respect_tiled_min_size = false,
	.focus_wrapping = true,
	.focus_on_activate = FOCUS_ON_ACTIVATE_FOCUS,
	.split_ratio = 0.5,
	.minimize_to_scratchpad = false,
	.scratchpad_restore_to_origin = true,

	// transaction settings
	.txn_timeout_ms = 200,
	.debug_txn_timings = false,
	.debug_noatomic = false,
	.debug_txn_wait = false,

	// border colors
	.normal_border_color = "444444ff",
	.active_border_color = "555555ff",
	.focused_border_color = "1793dfff",
	.normal_border_theme = {
		{0},
		0,
		0,
		{0},
		0,
		0,
		0
	},
	.active_border_theme = {
		{0},
		0,
		0,
		{0},
		0,
		0,
		0
	},
	.focused_border_theme = {
		{0},
		0,
		0,
		{0},
		0,
		0,
		0
	},
	.presel_feedback_color = "ff5555ff",
	.tiling_drag_indicator_color = "4d9eff4d",

	// shadow settings
	.shadow_size = 8.0f,
	.shadow_offset_x = 0.0f,
	.shadow_offset_y = 4.0f,
	.shadow_color = {0.0f, 0.0f, 0.0f, 0.5f},
};

void refresh_border_color_cache(void) {
	parse_color(settings.normal_border_color, settings.normal_border_color_rgba);
	parse_color(settings.active_border_color, settings.active_border_color_rgba);
	parse_color(settings.focused_border_color, settings.focused_border_color_rgba);
	parse_color(settings.presel_feedback_color, settings.presel_feedback_color_rgba);
	parse_color(settings.tiling_drag_indicator_color, settings.tiling_drag_indicator_color_rgba);
}

// global state
output_t *mon = NULL;
struct wl_list mon_list;
uint32_t next_node_id = 1;
uint32_t next_desktop_id = 1;
uint32_t next_monitor_id = 1;
struct wl_list orphan_desk_list;
