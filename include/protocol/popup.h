#pragma once

#include <wayland-server.h>

typedef struct popup_t {
	struct wlr_xdg_popup *xdg_popup;
	struct wlr_scene_tree *parent_tree;

	struct wlr_scene_tree *image_capture_tree;

	struct wl_listener commit;
	struct wl_listener reposition;
	struct wl_listener new_popup;
	struct wl_listener destroy;
} popup_t;

void handle_new_xdg_popup(struct wl_listener *listener, void *data);
void handle_new_layer_popup(struct wl_listener *listener, void *data);
