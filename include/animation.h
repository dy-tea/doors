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

void animation_set_bezier(const char *name);
const char *animation_get_bezier(void);
void animation_set_duration(uint32_t ms);
uint32_t animation_get_duration(void);

int animation_type_from_name(const char *name);
bool animation_set_type_config(const char *type_name, const char *bezier_name, uint32_t duration_ms);
const char *animation_type_get_bezier(const char *type_name);
uint32_t animation_type_get_duration(const char *type_name);
bool animation_set_type_spring(const char *type_name, const char *spring_name);
const char *animation_type_get_spring(const char *type_name);

bool animation_type_set_enabled(const char *type_name, bool enabled);
bool animation_type_get_enabled(const char *type_name);

bool animation_apply_geometry(node_t *node, struct wlr_scene_tree *scene_tree, struct wlr_box target, bool animate);

bool animation_apply_geometry_from(
    node_t *node, struct wlr_scene_tree *scene_tree, struct wlr_box from, struct wlr_box target, bool animate);

bool animation_start_workspace_slide(output_t *output, node_t *node, struct wlr_scene_tree *scene_tree,
    struct wlr_box from, struct wlr_box to, bool slide_out);

bool animation_fade_in(toplevel_t *toplevel);
bool animation_fade_in_layer(layer_surface_t *layer);
bool animation_fade_out(toplevel_t *toplevel);
bool animation_fade_out_layer(layer_surface_t *layer);
bool animation_is_opacity_fading(toplevel_t *toplevel);

bool animation_workspace_switch_active(output_t *output);
bool animation_node_workspace_slide_out(node_t *node);
void animation_cancel_node(node_t *node);
void animation_cancel_toplevel(toplevel_t *toplevel);
bool animation_has_fade_out(struct wlr_scene_tree *scene_tree);
void animation_cancel_scene_tree(struct wlr_scene_tree *scene_tree);

bool animation_update_output(output_t *output, struct timespec now);
