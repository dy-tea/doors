#pragma once

#include "types.h"

#include <wlr/types/wlr_scene.h>

typedef struct surface_blur_t surface_blur_t;
typedef struct surface_rounded_t surface_rounded_t;
typedef struct surface_shadow_t surface_shadow_t;
typedef struct output_t output_t;

struct wlr_scene_tree *client_get_scene_tree(client_t *client);
struct wlr_scene_tree *client_get_content_tree(client_t *client);
struct wlr_scene_tree *client_border_tree(client_t *client);
struct wlr_scene_rect **client_border_rects(client_t *client);
surface_rounded_t *client_get_rounded(client_t *client);

surface_blur_t **client_blur_slot(client_t *client);
surface_rounded_t **client_rounded_slot(client_t *client);
surface_shadow_t **client_shadow_slot(client_t *client);

// flips a client between visible and hidden in the scene graph
void client_set_visible(client_t *client, bool show);
node_t *client_get_node(client_t *client);
output_t *client_get_output(client_t *client);

// scene tree a client's state puts it in, ignoring its layer
struct wlr_scene_tree *client_state_tree(const client_t *client);

// scene tree a client's state and layer put it in
struct wlr_scene_tree *client_layer_tree(const client_t *client);

// parents a client's scene tree into the tree its layer calls for
void client_apply_layer(client_t *client);

bool client_is_mapped(const client_t *client);
struct wlr_surface *client_wlr_surface(client_t *client);
struct wlr_ext_foreign_toplevel_handle_v1 *client_get_ext_foreign_toplevel(const client_t *client);
struct wlr_foreign_toplevel_handle_v1 *client_get_foreign_toplevel(const client_t *client);
char client_state_to_char(const client_t *c);

void client_set_title(client_t *client, const char *title);
void client_set_app_id(client_t *client, const char *app_id);

void client_update_foreign_toplevel_state(client_t *client);
void client_update_ext_foreign_toplevel(client_t *client);
void client_connect_foreign_toplevel(client_t *client,
	struct wlr_foreign_toplevel_handle_v1 *handle);
void client_disconnect_foreign_toplevel(client_t *client);
void client_send_activated(client_t *client, bool activated);
