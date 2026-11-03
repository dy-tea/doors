#include "borders.h"
#include "settings.h"
#include <stdint.h>
#include <stdlib.h>

static const scroller_size_t scroller_default_preset_widths[] = {
	{SCROLLER_SIZE_PROPORTION, 1.0 / 3.0},
	{SCROLLER_SIZE_PROPORTION, 0.5},
	{SCROLLER_SIZE_PROPORTION, 2.0 / 3.0},
};

static const scroller_size_t scroller_default_preset_heights[] = {
	{SCROLLER_SIZE_PROPORTION, 1.0 / 3.0},
	{SCROLLER_SIZE_PROPORTION, 0.5},
	{SCROLLER_SIZE_PROPORTION, 2.0 / 3.0},
};

static void scroller_preset_release(scroller_size_t **list, int *count) {
	if (*list != NULL && *list != scroller_default_preset_widths &&
		*list != scroller_default_preset_heights)
		free(*list);

	*list = NULL;
	*count = 0;
}

void settings_set_scroller_preset_column_widths(scroller_size_t *list, const int count) {
	scroller_preset_release(&settings.scroller_preset_column_widths,
		&settings.scroller_preset_column_widths_count);
	settings.scroller_preset_column_widths = list;
	settings.scroller_preset_column_widths_count = count;
}

void settings_set_scroller_preset_window_heights(scroller_size_t *list, const int count) {
	scroller_preset_release(&settings.scroller_preset_window_heights,
		&settings.scroller_preset_window_heights_count);
	settings.scroller_preset_window_heights = list;
	settings.scroller_preset_window_heights_count = count;
}

// global settings
doors_settings_t settings = {
	// layout behaviour
	.automatic_scheme = SCHEME_SPIRAL,
	.initial_polarity = FIRST_CHILD,
	.removal_adjustment = true,
	.focus_wrapping = true,
	.focus_on_activate = FOCUS_ON_ACTIVATE_FOCUS,
	.focus_follows_mouse = FOLLOWS_NO,
	.pointer_follows_focus = false,
	.split_ratio = 0.5,

	// gaps, borders and decoration
	.border_width = 2,
	.window_gap = 10,
	.monocle_padding = {0},
	.smart_gaps = false,
	.smart_borders = false,
	.borderless_monocle = false,
	.borderless_singleton = false,
	.gapless_monocle = false,
	.respect_tiled_min_size = false,
	.hide_lone_tab = false,
	.decoration_mode = DECORATION_ALWAYS,

	// border colours
	.normal_border_color = "444444ff",
	.active_border_color = "555555ff",
	.focused_border_color = "1793dfff",
	.presel_feedback_color = "ff5555ff",
	.tiling_drag_indicator_color = "4d9eff4d",
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

	// shadows
	.shadow_size = 8.0f,
	.shadow_offset_x = 0.0f,
	.shadow_offset_y = 4.0f,
	.shadow_color = {0.0f, 0.0f, 0.0f, 0.5f},

	// dimming
	.dim_inactive = false,
	.dim_below = false,
	.dim_strength = 0.1f,
	.dim_color = {0.0f, 0.0f, 0.0f, 1.0f},

	// animations
	.enable_animations = false,
	.workspace_anim_direction = WORKSPACE_ANIM_VERTICAL,
	.workspace_anim_slide_up = false,

	// scroller layout
	.scroller_default_column_width = {SCROLLER_SIZE_PROPORTION, 0.5},
	.scroller_preset_column_widths = (scroller_size_t *)scroller_default_preset_widths,
	.scroller_preset_column_widths_count = (int)(sizeof(scroller_default_preset_widths) /
		sizeof(scroller_default_preset_widths[0])),
	.scroller_preset_window_heights = (scroller_size_t *)scroller_default_preset_heights,
	.scroller_preset_window_heights_count = (int)(sizeof(scroller_default_preset_heights) /
		sizeof(scroller_default_preset_heights[0])),
	.scroller_center_focused_column = SCROLLER_CENTER_NEVER,
	.scroller_always_center_single_column = false,
	.scroller_struts = {0},
	.scroller_pan_fingers = 3,
	.scroller_snap_percent = 30,

	// blur
	.blur_algorithm = BLUR_ALGORITHM_KAWASE,
	.blur_enabled = true,
	.blur_passes = 1,
	.blur_radius = 5.0f,
	.blur_downsample = 4,
	.blur_full_res = true,
	.blur_offset = 3.0f,
	.blur_saturation = 1.5f,
	.blur_vibrancy = 0.0f,
	.blur_vibrancy_darkness = 0.5f,
	.blur_noise_strength = 0.0f,
	.blur_brightness = 1.0f,
	.blur_contrast = 1.0f,

	// refraction
	.refraction_strength = 30.0f,
	.refraction_edge_size_px = 18.0f,
	.refraction_corner_radius_px = 8.0f,
	.refraction_normal_pow = 6.0f,
	.refraction_rgb_fringing = 22.0f / 30.0f,
	.refraction_texture_repeat_mode = 1,
	.refraction_offset = 1.0f,

	// mica and acrylic
	.mica_enabled = false,
	.mica_tint = {0.12f, 0.12f, 0.14f, 1.0f},
	.mica_tint_strength = 0.35f,
	.acrylic_tint = {1.0f, 1.0f, 1.0f, 1.0f},
	.acrylic_tint_strength = 0.3f,
	.acrylic_noise_strength = 0.02f,
	.acrylic_light_anchor = {0.5f, 0.5f},
	.acrylic_blur_passes = 4,

	// screen shader
	.screen_shader_enabled = false,
	.screen_shader = "none",

	// text and tab bar
	.text_font = "Sans 10",
	.text_height = 18,
	.color_bar_bg = {0.10f, 0.10f, 0.12f, 1.0f},
	.color_tab_bg = {0.18f, 0.18f, 0.20f, 1.0f},
	.color_tab_bg_active = {0.09f, 0.58f, 0.87f, 1.0f},
	.color_tab_text = {0.85f, 0.85f, 0.88f, 1.0f},
	.color_tab_text_active = {1.0f, 1.0f, 1.0f, 1.0f},
	.color_tab_sep = {0.0f, 0.0f, 0.0f, 1.0f},

	// window state
	.enable_minimize = true,
	.enable_maximize = true,
	.minimize_to_scratchpad = false,
	.scratchpad_restore_to_origin = true,

	// input
	.keyboard_grouping = KEYBOARD_GROUP_DEFAULT,

	// idle and power
	.idle_timeout = 0,
	.idle_dpms = true,
	.realtime_scheduling = false,

	// transactions and debugging
	.txn_timeout_ms = 200,
	.debug_txn_timings = false,
	.debug_noatomic = false,
	.debug_txn_wait = false,

	// misc
	.record_history = true,
	.allow_tearing = false,
	.direct_scanout = false,
	.auto_float_dialogs = false,
	.ignore_ewmh_fullscreen = 0,
};

void refresh_border_color_cache(void) {
	parse_color(settings.normal_border_color, settings.normal_border_color_rgba);
	parse_color(settings.active_border_color, settings.active_border_color_rgba);
	parse_color(settings.focused_border_color, settings.focused_border_color_rgba);
	parse_color(settings.presel_feedback_color, settings.presel_feedback_color_rgba);
	parse_color(settings.tiling_drag_indicator_color, settings.tiling_drag_indicator_color_rgba);
}

void settings_fini(void) {
	settings_set_scroller_preset_column_widths(NULL, 0);
	settings_set_scroller_preset_window_heights(NULL, 0);
}

// global state
struct wl_list mon_list;
uint32_t next_node_id = 1;
uint32_t next_desktop_id = 1;
uint32_t next_monitor_id = 1;
struct wl_list orphan_desk_list;
