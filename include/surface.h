#pragma once

#include "types.h"
#include <GLES2/gl2.h>
#include <stdbool.h>

struct wlr_scene_tree;

typedef struct surface_blur_t {
  struct wlr_scene_buffer *blur_node;
  struct wlr_scene_buffer *mica_node;
  struct wlr_scene_buffer *acrylic_node;
  bool blur_scene_hidden;
  struct wlr_buffer *blur_buf;
  GLuint blur_buf_fbo;
  struct wlr_buffer *acrylic_buf;
  GLuint acrylic_buf_fbo;
} surface_blur_t;

typedef struct surface_rounded_t {
  struct wlr_scene_buffer *border_shader_node;
  struct wlr_buffer *border_shader_buf;
  GLuint border_shader_buf_fbo;
  int border_shader_buf_w;
  int border_shader_buf_h;
  bool border_dirty;
  bool corner_mask_dirty;
  float border_color[4];

  float gradient_colors[BORDER_GRADIENT_MAX_STOPS * 4];
  int gradient_count;
  float gradient_angle;
  float gradient2_colors[BORDER_GRADIENT_MAX_STOPS * 4];
  int gradient2_count;
  float gradient2_angle;
  float gradient_lerp;

  struct wlr_scene_buffer *corner_mask_node;
  struct wlr_buffer *corner_mask_buf;
  GLuint corner_mask_buf_fbo;
} surface_rounded_t;

typedef struct surface_shadow_t {
  struct wlr_scene_buffer *shadow_node;
  struct wlr_buffer *shadow_buf;
  GLuint shadow_buf_fbo;
  int shadow_buf_w;
  int shadow_buf_h;
  bool shadow_dirty;
  bool shadow_geometry_dirty;
} surface_shadow_t;

void surface_set_blur(struct wlr_scene_tree *scene_tree, struct node_t *node,
  struct surface_blur_t **blur, bool enabled);
void surface_set_mica(struct wlr_scene_tree *scene_tree, struct node_t *node,
  struct surface_blur_t **blur, bool enabled);
void surface_set_acrylic(struct wlr_scene_tree *scene_tree, struct node_t *node,
  struct surface_blur_t **blur, bool enabled);
void surface_set_border_radius(struct wlr_scene_tree *scene_tree,
  struct wlr_scene_tree *content_tree, struct wlr_scene_tree *border_tree,
  struct node_t *node, struct surface_rounded_t **rounded,
  struct surface_shadow_t **shadow, float radius);
void surface_set_shadow(struct wlr_scene_tree *scene_tree, struct node_t *node,
  struct surface_shadow_t **shadow, bool enabled);
