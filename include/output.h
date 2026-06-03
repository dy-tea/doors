#pragma once

#include <wayland-server.h>
#include <wlr/util/box.h>
#include <wlr/render/color.h>
#include <time.h>
#include <stdbool.h>
#include <stdint.h>
#include "types.h"

typedef struct blur_output_ctx_t blur_output_ctx_t;
typedef struct desktop_t desktop_t;

enum scale_filter_mode {
	SCALE_FILTER_AUTO,
	SCALE_FILTER_LINEAR,
	SCALE_FILTER_NEAREST,
};

typedef struct output_t {
  struct wlr_output *wlr_output;
  struct wlr_box usable_area;
  struct wlr_scene_tree *layer_bg;
  struct wlr_scene_tree *layer_bottom;
  struct wlr_scene_tree *layer_top;
  struct wlr_scene_tree *layer_overlay;
  struct wl_list layers[4];
  struct wl_listener frame;
  struct wl_listener request_state;
  struct wl_listener destroy;
  struct wl_listener present;

  struct wlr_session_lock_surface_v1 *lock_surface;
  struct wl_listener destroy_lock_surface;
  struct wl_listener map_lock_surface;

  bool enabled;
  bool allow_tearing;
  int lx, ly;
  int width, height;

  struct timespec last_presentation;
  uint64_t refresh_nsec;
  int max_render_time;

  enum scale_filter_mode scale_filter_mode;
  enum wl_output_subpixel detected_subpixel;

  struct wlr_color_transform *color_transform;
  blur_output_ctx_t *blur_ctx;

  char name[64]; // SMALEN
  uint32_t id;
  bool wired;
  padding_t padding;
  unsigned int sticky_count;
  int window_gap;
  unsigned int border_width;
  struct wlr_box rectangle;
  desktop_t *desk;
  desktop_t *desk_head;
  desktop_t *desk_tail;
  struct output_t *prev;
  struct output_t *next;
} output_t;

void handle_new_output(struct wl_listener *listener, void *data);
void output_enable(output_t *output);
void output_disable(output_t *output);
void output_destroy(output_t *output);
output_t *output_from_wlr_output(struct wlr_output *wlr_output);
output_t *output_get_in_direction(output_t *reference, uint32_t direction);
void output_update_usable_area(output_t *output);
void output_set_scale_filter(output_t *output, enum scale_filter_mode mode);
void output_get_identifier(char *identifier, size_t len, output_t *output);
void output_update_scale(output_t *output, float scale);
output_t *output_get_valid(void);
