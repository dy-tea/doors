#pragma once

#include <wayland-server.h>
#include <wlr/types/wlr_compositor.h>

typedef struct {
	struct wlr_idle_inhibitor_v1 *idle_inhibitor;
	struct wl_listener destroy;
} idle_inhibitor_t;

void handle_new_idle_inhibitor(struct wl_listener *listener, void *data);
void update_idle_inhibitors(struct wlr_surface *sans);
