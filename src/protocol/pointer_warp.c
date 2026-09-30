#include "input/cursor.h"
#include "once.h"
#include "server.h"
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_pointer_warp_v1.h>
#include <wlr/util/log.h>

static void handle_pointer_warp(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_pointer_warp_v1_event_warp *event = data;

	struct wl_client *focused_client = NULL;
	struct wlr_surface *focused_surface = server.seat->pointer_state.focused_surface;
	if (focused_surface != NULL)
		focused_client = wl_resource_get_client(focused_surface->resource);

	if (focused_surface == NULL || event->seat_client->client != focused_client) {
		wlr_log(WLR_DEBUG, "Denying request to warp cursor from unfocused client");
		return;
	}

	struct wlr_box surface_box = {
		.width = event->surface->current.width,
		.height = event->surface->current.height
	};

	if (!wlr_box_contains_point(&surface_box, event->x, event->y)) {
		wlr_log(WLR_DEBUG, "Denying request to warp cursor outside of surface");
		return;
	}

	view_t *view = view_from_wlr_surface(event->surface);
	if (view == NULL || view->node == NULL) {
		wlr_log(WLR_DEBUG, "Denying request to warp cursor for surface without a view");
		return;
	}

	node_t *n = view->node;
	double lx = event->x + n->pending.rectangle.x - n->rectangle.x;
	double ly = event->y + n->pending.rectangle.y - n->rectangle.y;
	wlr_cursor_warp(server.cursor, NULL, lx, ly);
	wlr_seat_pointer_warp(event->seat_client->seat, event->x, event->y);
	cursor_rebase();
}

void pointer_warp_init(void) {
	ONCE();
	server.pointer_warp_manager = wlr_pointer_warp_v1_create(server.wl_display, 1);
	if (!server.pointer_warp_manager) {
		wlr_log(WLR_ERROR, "Failed to create pointer warp manager");
		exit(EXIT_FAILURE);
	}
	server.pointer_warp.notify = handle_pointer_warp;
	wl_signal_add(&server.pointer_warp_manager->events.warp, &server.pointer_warp);
}

void pointer_warp_fini(void) {
	ONCE();
	wl_list_remove(&server.pointer_warp.link);
}
