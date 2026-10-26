#include "once.h"
#include "permission.h"
#include "server.h"
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>

static void handle_new_virtual_pointer(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
	struct wlr_virtual_pointer_v1 *pointer = event->new_pointer;
	struct wlr_input_device *device = &pointer->pointer.base;

	struct wl_client *client = wl_resource_get_client(pointer->resource);
	perm_result_t permission = permission_check(client, PERM_INJECT);
	if (permission.decision != PERM_ALLOW) {
		if (permission.decision == PERM_ASK)
			permission_prompt(client, PERM_INJECT);
		else
			permission_log_denial(client, PERM_INJECT, &permission);
		wl_resource_destroy(pointer->resource);
		return;
	}

	wlr_cursor_attach_input_device(server.cursor, device);
	if (event->suggested_output)
		wlr_cursor_map_input_to_output(server.cursor, device, event->suggested_output);
}

void virtual_pointer_init(void) {
	ONCE();
	server.virtual_pointer_manager = wlr_virtual_pointer_manager_v1_create(server.wl_display);
	if (!server.virtual_pointer_manager) {
		wlr_log(WLR_ERROR, "Failed to create virtual pointer manager");
		exit(EXIT_FAILURE);
	}
	server.new_virtual_pointer.notify = handle_new_virtual_pointer;
	wl_signal_add(&server.virtual_pointer_manager->events.new_virtual_pointer,
		&server.new_virtual_pointer);
}

void virtual_pointer_fini(void) {
	ONCE();
	wl_list_remove(&server.new_virtual_pointer.link);
}
