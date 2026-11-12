#pragma once

#include "fwd.h"
#include "macros.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>

// enums
typedef enum {
	TYPE_HORIZONTAL,
	TYPE_VERTICAL,
	TYPE_TABBED
} split_type_t;

typedef enum {
	STATE_TILED,
	STATE_PSEUDO_TILED,
	STATE_FLOATING,
	STATE_FULLSCREEN
} client_state_t;

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

#include "layout/master_stack.h"

typedef enum {
	CLIENT_LAYER_NORMAL,
	CLIENT_LAYER_BELOW,
	CLIENT_LAYER_ABOVE,
} client_layer_t;

// structures
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

struct client_t {
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
};

struct node_t {
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
};

struct desktop_t {
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
};
