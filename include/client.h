#pragma once

#include "types.h"

#include <wlr/types/wlr_scene.h>

typedef struct surface_rounded_t surface_rounded_t;
typedef struct output_t output_t;

struct wlr_scene_tree *client_get_scene_tree(client_t *client);
struct wlr_scene_tree *client_get_content_tree(client_t *client);
struct wlr_scene_tree *client_border_tree(client_t *client);
struct wlr_scene_rect **client_border_rects(client_t *client);
surface_rounded_t *client_get_rounded(client_t *client);

// flips a client between visible and hidden in the scene graph
void client_set_visible(client_t *client, bool show);
node_t *client_get_node(client_t *client);
output_t *client_get_output(client_t *client);
