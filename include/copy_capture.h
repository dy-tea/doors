#pragma once

#include <wayland-server.h>

void capture_renderer_destroy(void *renderer);

void image_copy_capture_init(void);
void image_copy_capture_fini(void);
struct wl_global *image_copy_capture_get_global(void);
struct wl_global *image_capture_source_get_global(void);
