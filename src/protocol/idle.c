#include "once.h"
#include "protocol/idle.h"
#include "server.h"
#include "view.h"
#include <stdbool.h>
#include <stdlib.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_session_lock_v1.h>

static bool inhibitor_surface_is_presented(struct wlr_surface *surface) {
	struct wlr_surface *root = wlr_surface_get_root_surface(surface);

	view_t *view = view_from_wlr_surface(root);
	if (view)
		return view_is_ready(view);

	struct wlr_session_lock_surface_v1 *lock_surface =
		wlr_session_lock_surface_v1_try_from_wlr_surface(root);
	if (lock_surface) {
		struct wlr_scene_tree *tree = lock_surface->surface->data;
		int x, y;
		return tree != NULL && wlr_scene_node_coords(&tree->node, &x, &y);
	}

	return false;
}

bool idle_inhibitors_active(struct wlr_surface *sans) {
	struct wlr_idle_inhibitor_v1 *idle;
	wl_list_for_each(idle, &server.idle_inhibit_manager->inhibitors, link) {
		struct wlr_surface *surface = wlr_surface_get_root_surface(idle->surface);
		if (surface != sans && inhibitor_surface_is_presented(surface))
			return true;
	}
	return false;
}

void update_idle_inhibitors(struct wlr_surface *sans) {
	wlr_idle_notifier_v1_set_inhibited(server.idle_notifier, idle_inhibitors_active(sans));
}

void handle_idle_inhibitor_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	idle_inhibitor_t *idle = wl_container_of(listener, idle, destroy);
	wl_list_remove(&idle->destroy.link);
	update_idle_inhibitors(wlr_surface_get_root_surface(idle->idle_inhibitor->surface));
	free(idle);
}

static void handle_new_idle_inhibitor(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_idle_inhibitor_v1 *idle_inhibitor = data;

	idle_inhibitor_t *idle = calloc(1, sizeof(*idle));
	if (!idle) {
		wlr_log(WLR_ERROR, "Allocation failed");
		return;
	}
	idle->idle_inhibitor = idle_inhibitor;

	idle->destroy.notify = handle_idle_inhibitor_destroy;
	wl_signal_add(&idle_inhibitor->events.destroy, &idle->destroy);

	update_idle_inhibitors(NULL);
}

void idle_init(void) {
	ONCE();
	server.idle_inhibit_manager = wlr_idle_inhibit_v1_create(server.wl_display);
	if (!server.idle_inhibit_manager) {
		wlr_log(WLR_ERROR, "Failed to create idle inhibit manager");
		exit(EXIT_FAILURE);
	}
	server.new_idle_inhibitor.notify = handle_new_idle_inhibitor;
	wl_signal_add(&server.idle_inhibit_manager->events.new_inhibitor, &server.new_idle_inhibitor);
}

void idle_fini(void) {
	ONCE();
	wl_list_remove(&server.new_idle_inhibitor.link);
}
