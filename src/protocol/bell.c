#include "config.h"
#include "once.h"
#include "server.h"
#include <wayland-server-core.h>
#include <wlr/types/wlr_xdg_system_bell_v1.h>

static bool bell_debouncing = false;

static int handle_system_bell_timer(void *data) {
	(void)data;
	bell_debouncing = false;
	return 0;
}

static void handle_ring_system_bell(struct wl_listener *listener, void *data) {
	(void)listener;
	(void)data;

	if (bell_debouncing || !server.system_bell_timer)
		return;

	bell_debouncing = true;
	wl_event_source_timer_update(server.system_bell_timer, 100);

	execute_bell_bind();
}

void bell_init(void) {
	ONCE();
	server.xdg_system_bell = wlr_xdg_system_bell_v1_create(server.wl_display, 1);
	if (!server.xdg_system_bell) {
		wlr_log(WLR_ERROR, "Failed to create xdg system bell");
		exit(EXIT_FAILURE);
	}

	server.system_bell_timer = wl_event_loop_add_timer(wl_display_get_event_loop(server.wl_display),
		handle_system_bell_timer, NULL);

	server.ring_system_bell.notify = handle_ring_system_bell;
	wl_signal_add(&server.xdg_system_bell->events.ring, &server.ring_system_bell);
}

void bell_fini(void) {
	ONCE();
	if (server.system_bell_timer) {
		wl_event_source_remove(server.system_bell_timer);
		server.system_bell_timer = NULL;
	}
	bell_debouncing = false;
	wl_list_remove(&server.ring_system_bell.link);
}
