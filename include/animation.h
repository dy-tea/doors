#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <wlr/util/box.h>

struct bwm_output;
struct node_t;
struct bwm_toplevel;
struct wlr_scene_tree;

void animation_init(void);
void animation_fini(void);

bool animation_apply_geometry(struct node_t *node, struct wlr_scene_tree *scene_tree,
  struct wlr_box target, bool animate);

bool animation_apply_geometry_from(struct node_t *node, struct wlr_scene_tree *scene_tree,
  struct wlr_box from, struct wlr_box target, bool animate);

bool animation_start_snapshot_resize(struct bwm_toplevel *toplevel, struct wlr_box from,
	struct wlr_box to);

void animation_cancel_node(struct node_t *node);

bool animation_update_output(struct bwm_output *output, struct timespec now);
