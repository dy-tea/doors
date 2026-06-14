#pragma once

#include <GLES2/gl2.h>
#include <pixman.h>
#include <wayland-server.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_scene.h>

typedef struct output_t output_t;

typedef struct layer_surface_t {
  struct wl_list link;
  struct wlr_layer_surface_v1 *layer_surface;
  struct wlr_scene_tree *scene_tree;
  struct wlr_scene_layer_surface_v1 *scene_layer;
  struct wlr_scene_tree *saved_tree;
  output_t *output;
  bool mapped;

  struct wlr_scene_buffer *blur_node;
  struct wlr_buffer *blur_buf;
  GLuint blur_buf_fbo;
  bool blur_scene_hidden;
  pixman_region32_t blur_region;  // blur region in surface-local coordinates
  int blur_region_offset_x, blur_region_offset_y;
  int blur_region_width, blur_region_height;

  struct wl_listener new_popup;
  struct wl_listener destroy;
  struct wl_listener map;
  struct wl_listener unmap;
  struct wl_listener surface_commit;
} layer_surface_t;

void handle_new_layer_surface(struct wl_listener *listener, void *data);
void arrange_layers(output_t *output);
void focus_layer_surface(layer_surface_t *layer_surface);
void layer_surface_set_blur(layer_surface_t *ls, bool enabled);
