#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <wlr/util/box.h>

#define MAXLEN 256
#define SMALEN 64

// forward declarations
struct node_t;
struct desktop_t;
struct client_t;
struct output_t;
struct toplevel_t;
struct xwayland_toplevel_t;
struct tab_bar_t;

struct output_t *output_at(double x, double y);

// enums
typedef enum { TYPE_HORIZONTAL, TYPE_VERTICAL, TYPE_TABBED } split_type_t;

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

typedef enum { DECORATION_NONE, DECORATION_TABS, DECORATION_ALWAYS, DECORATION_CSD } decoration_mode_t;

typedef enum { LAYER_BELOW, LAYER_NORMAL, LAYER_ABOVE } stack_layer_t;

typedef enum { LAYOUT_TILED, LAYOUT_MONOCLE, LAYOUT_SCROLLER } layout_t;

typedef enum { DIR_NORTH, DIR_WEST, DIR_SOUTH, DIR_EAST } direction_t;

typedef enum { FIRST_CHILD, SECOND_CHILD } child_polarity_t;

typedef enum { FLIP_HORIZONTAL, FLIP_VERTICAL } flip_t;

typedef enum { WORKSPACE_ANIM_VERTICAL, WORKSPACE_ANIM_HORIZONTAL } workspace_anim_direction_t;

// structures
typedef struct {
  int top;
  int right;
  int bottom;
  int left;
} padding_t;

typedef struct {
  uint16_t min_width;
  uint16_t min_height;
} constraints_t;

typedef struct {
  double split_ratio;
  direction_t split_dir;
} presel_t;

typedef struct client_t {
  char app_id[MAXLEN];
  char title[MAXLEN];
  unsigned int border_width;
  bool urgent;
  bool shown;
  client_state_t state;
  client_state_t last_state;
  stack_layer_t layer;
  stack_layer_t last_layer;
  struct wlr_box floating_rectangle;
  struct wlr_box tiled_rectangle;
  struct wlr_box committed_tiled_rectangle;
  struct toplevel_t *toplevel;
  struct xwayland_toplevel_t *xwayland_view;

  // Scroller layout properties
  float scroller_proportion;
  float scroller_proportion_single;
  float stack_proportion;
  struct client_t *next_in_stack;
  struct client_t *prev_in_stack;

  // Resize state for scroller
  float old_scroller_proportion;
  float old_stack_proportion;
  bool cursor_in_left_half;
  bool cursor_in_upper_half;

  // Visual effects
  bool blur;
  bool blur_from_rule;
  bool mica;
  bool acrylic;
  float border_radius;

  // Screenshare privacy
  bool block_out_from_screenshare;

  // Tearing
  bool allow_tearing;
  bool allow_tearing_from_rule;
} client_t;

typedef struct node_t {
  uint32_t id;
  split_type_t split_type;
  double split_ratio;
  presel_t *presel;
  struct wlr_box rectangle;
  constraints_t constraints;
  bool vacant;
  bool hidden;
  bool sticky;
  bool scratchpad;
  bool private_node;
  bool locked;
  bool marked;
  struct node_t *first_child;
  struct node_t *second_child;
  struct node_t *parent;
  client_t *client;
  struct output_t *output;
  struct desktop_t *desktop;

  // transaction support
  struct transaction_inst_t *instruction;
  size_t ntxnrefs;
  bool dirty;
  bool destroying;

  // current state
  struct {
    struct wlr_box rectangle;
    double split_ratio;
    split_type_t split_type;
    bool hidden;
  } current;

  // pending state
  struct {
    struct wlr_box rectangle;
    double split_ratio;
    split_type_t split_type;
    bool hidden;
  } pending;

  struct tab_bar_t *tab_bar;
} node_t;

typedef struct desktop_t {
  char name[SMALEN];
  uint32_t id;
  layout_t layout;
  layout_t user_layout;
  node_t *root;
  node_t *focus;
  struct desktop_t *prev;
  struct desktop_t *next;
  padding_t padding;
  int window_gap;
  unsigned int border_width;
  struct output_t *output;
} desktop_t;

typedef struct {
  struct output_t *output;
  desktop_t *desktop;
  node_t *node;
} coordinates_t;

// global settings
extern automatic_scheme_t automatic_scheme;
extern child_polarity_t initial_polarity;
extern bool single_monocle;
extern bool borderless_monocle;
extern bool borderless_singleton;
extern bool gapless_monocle;
extern bool removal_adjustment;
extern bool focus_follows_pointer;
extern bool pointer_follows_focus;
extern bool record_history;
extern bool click_to_focus;
extern bool allow_tearing;
extern decoration_mode_t decoration_mode;
extern bool enable_animations;
extern workspace_anim_direction_t workspace_anim_direction;
extern bool workspace_anim_slide_up;
extern int mapping_events_count;

// Transaction system settings
extern int txn_timeout_ms;
extern bool debug_txn_timings;
extern bool debug_noatomic;
extern bool debug_txn_wait;
extern int directional_focus_tightness;
extern int ignore_ewmh_fullscreen;
extern padding_t monocle_padding;
extern padding_t padding;
extern int border_width;
extern int window_gap;
extern double split_ratio;
extern char normal_border_color[16];
extern char active_border_color[16];
extern char focused_border_color[16];
extern char presel_feedback_color[16];
extern char tiling_drag_indicator_color[16];

// broder gradients
#define BORDER_GRADIENT_MAX_STOPS 10
extern float normal_border_gradient[BORDER_GRADIENT_MAX_STOPS * 4];
extern int normal_border_gradient_count;
extern float normal_border_gradient_angle;
extern float normal_border_gradient2[BORDER_GRADIENT_MAX_STOPS * 4];
extern int normal_border_gradient2_count;
extern float normal_border_gradient2_angle;
extern float normal_border_gradient_lerp;

extern float active_border_gradient[BORDER_GRADIENT_MAX_STOPS * 4];
extern int active_border_gradient_count;
extern float active_border_gradient_angle;
extern float active_border_gradient2[BORDER_GRADIENT_MAX_STOPS * 4];
extern int active_border_gradient2_count;
extern float active_border_gradient2_angle;
extern float active_border_gradient_lerp;

extern float focused_border_gradient[BORDER_GRADIENT_MAX_STOPS * 4];
extern int focused_border_gradient_count;
extern float focused_border_gradient_angle;
extern float focused_border_gradient2[BORDER_GRADIENT_MAX_STOPS * 4];
extern int focused_border_gradient2_count;
extern float focused_border_gradient2_angle;
extern float focused_border_gradient_lerp;

// global state
extern struct output_t *mon;
extern struct output_t *mon_head;
extern struct output_t *mon_tail;
extern uint32_t next_node_id;
extern uint32_t next_desktop_id;
extern uint32_t next_monitor_id;
