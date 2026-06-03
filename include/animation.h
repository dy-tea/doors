#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <wlr/util/box.h>

typedef struct output_t output_t;
typedef struct node_t node_t;
typedef struct toplevel_t toplevel_t;
typedef struct layer_surface_t layer_surface_t;
struct wlr_scene_tree;

void animation_init(void);
void animation_fini(void);

bool animation_apply_geometry(node_t *node, struct wlr_scene_tree *scene_tree,
  struct wlr_box target, bool animate);

bool animation_apply_geometry_from(node_t *node, struct wlr_scene_tree *scene_tree,
  struct wlr_box from, struct wlr_box target, bool animate);

bool animation_start_workspace_slide(output_t *output,
  node_t *node, struct wlr_scene_tree *scene_tree,
  struct wlr_box from, struct wlr_box to, bool slide_out);

bool animation_start_snapshot_resize(toplevel_t *toplevel, struct wlr_box from,
	struct wlr_box to);

bool animation_fade_in(toplevel_t *toplevel);
bool animation_fade_in_layer(layer_surface_t *layer);
bool animation_fade_out(toplevel_t *toplevel);
bool animation_fade_out_layer(layer_surface_t *layer);

void animation_cancel_node(node_t *node);
bool animation_has_fade_out(struct wlr_scene_tree *scene_tree);
void animation_cancel_scene_tree(struct wlr_scene_tree *scene_tree);

bool animation_update_output(output_t *output, struct timespec now);
