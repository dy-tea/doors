#include "input/pointer.h"
#include "input/seat.h"
#include "server.h"
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/util/log.h>

static void pointer_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	pointer_t *ptr = wl_container_of(listener, ptr, destroy);

	ptr->wlr_pointer->base.data = NULL;
	wl_list_remove(&ptr->destroy.link);
	wl_list_remove(&ptr->link);
	free(ptr);
}

void pointer_create(struct wlr_input_device *device) {
	wlr_cursor_attach_input_device(server.cursor, device);
	struct wlr_pointer *pointer = wlr_pointer_from_input_device(device);
	pointer_t *ptr = calloc(1, sizeof(*ptr));
	if (!ptr) {
		wlr_log(WLR_ERROR, "Allocation failed");
		return;
	}
	ptr->wlr_pointer = pointer;
	ptr->seat = seat_default();
	ptr->scroll_factor = 1.0;
	device->data = ptr;

	ptr->destroy.notify = pointer_destroy;
	wl_signal_add(&device->events.destroy, &ptr->destroy);
	wl_list_insert(&server.pointers, &ptr->link);
}
