#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>

#define MAXLEN 256
#define SMALEN 64

// forward declarations
struct node_t;
struct desktop_t;
struct client_t;
struct output_t;
struct tab_bar_t;
struct scroller_state_t;

typedef struct view_t view_t;
typedef struct xdg_toplevel_t xdg_toplevel_t;
typedef struct xwayland_toplevel_t xwayland_toplevel_t;

// enums
typedef enum {
	TYPE_HORIZONTAL,
	TYPE_VERTICAL,
	TYPE_TABBED
} split_type_t;

typedef enum {
	SCHEME_LONGEST_SIDE,
	SCHEME_ALTERNATE,
	SCHEME_SPIRAL
} automatic_scheme_t;

typedef enum {
	STATE_TILED,
	STATE_PSEUDO_TILED,
	STATE_FLOATING,
	STATE_FULLSCREEN
} client_state_t;

typedef enum {
	DECORATION_NONE,
	DECORATION_TABS,
	DECORATION_ALWAYS,
	DECORATION_CSD
} decoration_mode_t;

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

typedef enum {
	LAYOUT_TILED,
	LAYOUT_MONOCLE,
	LAYOUT_SCROLLER,
	LAYOUT_MASTER_STACK,
	LAYOUT_FLOATING
} layout_t;

static inline char layout_to_char(layout_t l) {
	const char layout_chars[] = {
		[LAYOUT_TILED] = 'T',
		[LAYOUT_MONOCLE] = 'M',
		[LAYOUT_SCROLLER] = 'S',
		[LAYOUT_MASTER_STACK] = 'K',
		[LAYOUT_FLOATING] = 'F'
	};
	return layout_chars[l];
}

typedef enum {
	DIR_NORTH,
	DIR_WEST,
	DIR_SOUTH,
	DIR_EAST
} direction_t;

typedef enum {
	FIRST_CHILD,
	SECOND_CHILD
} child_polarity_t;

typedef enum {
	FLIP_HORIZONTAL,
	FLIP_VERTICAL
} flip_t;

typedef enum {
	MASTER_LEFT,
	MASTER_TOP,
	MASTER_RIGHT,
	MASTER_BOTTOM,
	MASTER_CENTER,
} master_area_orientation_t;

typedef enum {
	STACK_VERTICAL,
	STACK_HORIZONTAL,
} stack_layout_t;

typedef enum {
	WORKSPACE_ANIM_VERTICAL,
	WORKSPACE_ANIM_HORIZONTAL
} workspace_anim_direction_t;

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

// structures
typedef struct {
	int top, right, bottom, left;
} padding_t;

typedef struct {
	uint16_t min_width, min_height;
} constraints_t;

typedef struct {
	double split_ratio;
	direction_t split_dir;
	bool freed;
} presel_t;

typedef struct {
	uint32_t urgent : 1;
	uint32_t shown : 1;
	uint32_t freed : 1;
	uint32_t maximized : 1;
	uint32_t minimized : 1;
	uint32_t master_stack_master : 1;
	uint32_t blur : 1;
	uint32_t blur_from_rule : 1;
	uint32_t mica : 1;
	uint32_t acrylic : 1;
	uint32_t shadow : 1;
	uint32_t block_out_from_screenshare : 1;
	uint32_t allow_tearing : 1;
	uint32_t allow_tearing_from_rule : 1;
	uint32_t anim_disabled : 1;
	uint32_t no_dim : 1;
} client_flags_t;

typedef enum {
	VIEW_XDG,
	VIEW_XWAYLAND,
} view_type_t;

typedef enum {
	CLIENT_LAYER_NORMAL,
	CLIENT_LAYER_BELOW,
	CLIENT_LAYER_ABOVE,
} client_layer_t;

typedef struct client_t {
	char app_id[MAXLEN];
	char title[MAXLEN];
	client_state_t state, last_state;
	struct wlr_box floating_rectangle, tiled_rectangle, committed_tiled_rectangle, arranged_rectangle;
	struct wlr_box pre_maximize_rectangle;
	struct view_t *view;

	struct {
		struct wl_listener activate;
		struct wl_listener fullscreen;
		struct wl_listener close;
		struct wl_listener destroy;
	} foreign;

	// Master-stack layout properties
	uint64_t master_stack_order;

	// Visual effects
	float border_radius;
	float opacity;
	int render_unfocused_fps;
	float shadow_size, shadow_offset_x, shadow_offset_y;
	float shadow_color[4];

	client_flags_t flags;
	client_layer_t layer;
} client_t;

typedef struct node_t {
	uint32_t id;
	split_type_t split_type;
	double split_ratio;
	presel_t *presel;
	struct wlr_box rectangle;
	constraints_t constraints;
	bool vacant, hidden, sticky, scratchpad, private_node, locked, marked;
	struct node_t *first_child, *second_child, *parent;
	client_t *client;
	struct output_t *output;
	struct desktop_t *desktop;

	struct wl_list minimize_link;

	// transaction support
	struct transaction_inst_t *instruction, *pending_inst;
	size_t ntxnrefs;
	bool dirty;
	bool destroying;
	bool freed;

	// current and pending state
	struct {
		struct wlr_box rectangle;
		double split_ratio;
		split_type_t split_type;
		bool hidden;
	} current, pending;

	struct tab_bar_t *tab_bar;
} node_t;

typedef struct desktop_t {
	char name[SMALEN];
	uint32_t id;
	layout_t layout, user_layout;
	node_t *root, *focus;
	struct scroller_state_t *scroller_state;
	struct wl_list link;
	int window_gap;
	struct output_t *output;
	uint32_t fullscreen_recreate_pending_window_id;
	pid_t fullscreen_recreate_pending_pid;
	struct {
		float ratio;
		master_area_orientation_t orientation;
		stack_layout_t stack_layout;
		int count;
	} master_stack;

	// minimize history
	struct wl_list minimized;
} desktop_t;

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

typedef enum {
	KEYBOARD_GROUP_DEFAULT,
	KEYBOARD_GROUP_NONE,
	KEYBOARD_GROUP_SMART,
} keyboard_grouping_t;

enum blur_algorithm {
	BLUR_ALGORITHM_NONE,
	BLUR_ALGORITHM_KAWASE,
	BLUR_ALGORITHM_GAUSSIAN,
	BLUR_ALGORITHM_BOX,
	BLUR_ALGORITHM_REFRACTION,
	BLUR_ALGORITHM_LENS_REFRACTION,
};

// global settings
typedef struct {
	// layout behaviour
	automatic_scheme_t automatic_scheme;
	child_polarity_t initial_polarity;
	bool removal_adjustment;
	bool focus_wrapping;
	focus_on_activate_mode_t focus_on_activate;
	int focus_follows_mouse;
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
