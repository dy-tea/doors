#pragma once

#include "borders.h"
#include "effects/backend.h"
#include "layout/scroller.h"

#include <stdbool.h>
#include <stddef.h>

// layout behaviour
typedef enum {
	SCHEME_LONGEST_SIDE,
	SCHEME_ALTERNATE,
	SCHEME_SPIRAL
} automatic_scheme_t;

typedef enum {
	FIRST_CHILD,
	SECOND_CHILD
} child_polarity_t;

typedef enum {
	FOCUS_ON_ACTIVATE_FOCUS,
	FOCUS_ON_ACTIVATE_NONE,
	FOCUS_ON_ACTIVATE_SMART,
	FOCUS_ON_ACTIVATE_URGENT
} focus_on_activate_mode_t;

typedef enum {
	FOLLOWS_NO,
	FOLLOWS_YES,
	FOLLOWS_ALWAYS
} focus_follows_mouse_mode_t;

// decoration
typedef enum {
	DECORATION_NONE,
	DECORATION_TABS,
	DECORATION_ALWAYS,
	DECORATION_CSD
} decoration_mode_t;

// animations
typedef enum {
	WORKSPACE_ANIM_VERTICAL,
	WORKSPACE_ANIM_HORIZONTAL
} workspace_anim_direction_t;

// input
typedef enum {
	KEYBOARD_GROUP_DEFAULT,
	KEYBOARD_GROUP_NONE,
	KEYBOARD_GROUP_SMART,
} keyboard_grouping_t;

// decorations
typedef struct {
	int top, right, bottom, left;
} padding_t;

// global settings
typedef struct {
	// layout behaviour
	automatic_scheme_t automatic_scheme;
	child_polarity_t initial_polarity;
	bool removal_adjustment;
	bool focus_wrapping;
	focus_on_activate_mode_t focus_on_activate;
	focus_follows_mouse_mode_t focus_follows_mouse;
	int focus_follows_mouse_max_scroll;
	bool pointer_follows_focus;
	double split_ratio;

	// gaps, borders and decoration
	int border_width;
	int window_gap;
	padding_t monocle_padding;
	bool smart_gaps;
	bool smart_borders;
	bool borderless_monocle;
	bool borderless_singleton;
	bool gapless_monocle;
	bool respect_tiled_min_size;
	bool hide_lone_tab;
	decoration_mode_t decoration_mode;

	// border colours
	char normal_border_color[16];
	char active_border_color[16];
	char focused_border_color[16];
	char presel_feedback_color[16];
	char tiling_drag_indicator_color[16];

	float normal_border_color_rgba[4];
	float active_border_color_rgba[4];
	float focused_border_color_rgba[4];
	float presel_feedback_color_rgba[4];
	float tiling_drag_indicator_color_rgba[4];

	border_theme_t normal_border_theme;
	border_theme_t active_border_theme;
	border_theme_t focused_border_theme;

	// shadows
	float shadow_size;
	float shadow_offset_x;
	float shadow_offset_y;
	float shadow_color[4];

	// dimming
	bool dim_inactive; // darken everything but the focused window
	bool dim_below; // darken the area behind each window
	float dim_strength;
	float dim_color[4];

	// animations
	bool enable_animations;
	workspace_anim_direction_t workspace_anim_direction;
	bool workspace_anim_slide_up;

	// scroller layout
	scroller_size_t scroller_default_column_width;
	scroller_size_t *scroller_preset_column_widths;
	int scroller_preset_column_widths_count;
	scroller_size_t *scroller_preset_window_heights;
	int scroller_preset_window_heights_count;
	scroller_center_mode_t scroller_center_focused_column;
	bool scroller_always_center_single_column;
	padding_t scroller_struts;
	uint32_t scroller_pan_fingers;
	int scroller_snap_percent;

	// blur
	enum blur_algorithm blur_algorithm;
	bool blur_enabled;
	int blur_passes;
	float blur_radius;
	int blur_downsample;
	bool blur_full_res;
	float blur_offset;
	float blur_saturation;
	float blur_vibrancy;
	float blur_vibrancy_darkness;
	float blur_noise_strength;
	float blur_brightness;
	float blur_contrast;

	// refraction
	float refraction_strength;
	float refraction_edge_size_px;
	float refraction_corner_radius_px;
	float refraction_normal_pow;
	float refraction_rgb_fringing;
	int refraction_texture_repeat_mode;
	float refraction_offset;

	// mica and acrylic
	bool mica_enabled;
	float mica_tint[4];
	float mica_tint_strength;
	float acrylic_tint[4];
	float acrylic_tint_strength;
	float acrylic_noise_strength;
	float acrylic_light_anchor[2];
	int acrylic_blur_passes;

	// screen shader
	bool screen_shader_enabled;
	char screen_shader[256];

	// text and tab bar
	char text_font[128];
	int text_height;
	float color_bar_bg[4];
	float color_tab_bg[4];
	float color_tab_bg_active[4];
	float color_tab_text[4];
	float color_tab_text_active[4];
	float color_tab_sep[4];

	// window state
	bool enable_minimize; // when false, disables minimizing completely, still allows you to unminimize
	bool enable_maximize; // when false, disables maximizing completely, still allows you to unmaximize
	bool minimize_to_scratchpad;
	// when a minimized window is restored, put it back on the desktop it came from
	// instead of the currently focused one
	bool scratchpad_restore_to_origin;

	// input
	keyboard_grouping_t keyboard_grouping;

	// idle and power
	int idle_timeout;
	bool idle_dpms;
	bool realtime_scheduling;

	// transactions and debugging
	int txn_timeout_ms;
	bool debug_txn_timings;
	bool debug_noatomic;
	bool debug_txn_wait;

	// misc
	bool record_history;
	bool allow_tearing;
	bool direct_scanout;
	bool auto_float_dialogs;
	int ignore_ewmh_fullscreen;
} doors_settings_t;

extern doors_settings_t settings;

void settings_init(void);
void refresh_border_color_cache(void);
void settings_fini(void);

void settings_set_scroller_preset_column_widths(scroller_size_t *list, int count);
void settings_set_scroller_preset_window_heights(scroller_size_t *list, int count);

bool config_apply_value(const char *name, const char *value, char *err, size_t errsz);

bool config_validate_value(const char *name, const char *value, char *err, size_t errsz);

void config_snapshot_defaults(void);

extern struct wl_list mon_list;
extern struct wl_list orphan_desk_list;
extern uint32_t next_node_id;
extern uint32_t next_desktop_id;
extern uint32_t next_monitor_id;
