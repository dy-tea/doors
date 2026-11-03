#include "macros.h"
#include "protocol/xdg_toplevel.h"
#include "server.h"
#include <wlr/types/wlr_xdg_decoration_v1.h>

static xdg_toplevel_t *toplevel_for_xdg_surface(struct wlr_xdg_surface *surface) {
	view_t *view;
	wl_list_for_each(view, &server.views, link) {
		xdg_toplevel_t *tl = view_to_xdg(view);
		if (tl && tl->xdg_toplevel && tl->xdg_toplevel->base == surface)
			return tl;
	}

	return NULL;
}

static void handle_decoration_request_mode(struct wl_listener *listener, void *data) {
	(void)data;
	xdg_toplevel_t *tl = wl_container_of(listener, tl, decoration_request_mode);
	tl->view.impl->set_decorations(&tl->view);
}

static void handle_decoration_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	xdg_toplevel_t *tl = wl_container_of(listener, tl, decoration_destroy);

	wl_list_remove(&tl->decoration_destroy.link);
	wl_list_remove(&tl->decoration_request_mode.link);
	tl->xdg_decoration = NULL;
}

static void handle_new_xdg_decoration(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_xdg_toplevel_decoration_v1 *deco = data;
	struct wlr_xdg_surface *xdg_surface = deco->toplevel->base;
	xdg_toplevel_t *tl = toplevel_for_xdg_surface(xdg_surface);

	if (tl == NULL)
		return;

	tl->xdg_decoration = deco;

	tl->decoration_destroy.notify = handle_decoration_destroy;
	wl_signal_add(&deco->events.destroy, &tl->decoration_destroy);

	tl->decoration_request_mode.notify = handle_decoration_request_mode;
	wl_signal_add(&deco->events.request_mode, &tl->decoration_request_mode);

	if (xdg_surface->initialized && tl->view.node)
		tl->view.impl->set_decorations(&tl->view);
}

void xdg_decoration_init(void) {
	ONCE();
	server.xdg_decoration_manager = wlr_xdg_decoration_manager_v1_create(server.wl_display, 2);
	if (!server.xdg_decoration_manager) {
		wlr_log(WLR_ERROR, "Failed to create xdg decoration manager");
		exit(EXIT_FAILURE);
	}
	server.new_xdg_decoration.notify = handle_new_xdg_decoration;
	wl_signal_add(&server.xdg_decoration_manager->events.new_toplevel_decoration,
		&server.new_xdg_decoration);
}

void xdg_decoration_fini(void) {
	ONCE();
	wl_list_remove(&server.new_xdg_decoration.link);
}
