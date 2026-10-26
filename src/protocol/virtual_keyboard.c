#include "input/keyboard.h"
#include "once.h"
#include "permission.h"
#include "server.h"
#include <wlr/types/wlr_virtual_keyboard_v1.h>

static void handle_new_virtual_keyboard(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_virtual_keyboard_v1 *virtual_keyboard = data;

	struct wl_client *client = wl_resource_get_client(virtual_keyboard->resource);
	perm_result_t permission = permission_check(client, PERM_INJECT);
	if (permission.decision != PERM_ALLOW) {
		if (permission.decision == PERM_ASK)
			permission_prompt(client, PERM_INJECT);
		else
			permission_log_denial(client, PERM_INJECT, &permission);
		wl_resource_destroy(virtual_keyboard->resource);
		return;
	}

	keyboard_create(&virtual_keyboard->keyboard.base);
}

void virtual_keyboard_init(void) {
	ONCE();
	server.virtual_keyboard_manager = wlr_virtual_keyboard_manager_v1_create(server.wl_display);
	if (!server.virtual_keyboard_manager) {
		wlr_log(WLR_ERROR, "Failed to create virtual keyboard manager");
		exit(EXIT_FAILURE);
	}
	server.new_virtual_keyboard.notify = handle_new_virtual_keyboard;
	wl_signal_add(&server.virtual_keyboard_manager->events.new_virtual_keyboard,
		&server.new_virtual_keyboard);
}

void virtual_keyboard_fini(void) {
	ONCE();
	wl_list_remove(&server.new_virtual_keyboard.link);
}
