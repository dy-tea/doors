#pragma once

#include <wayland-server.h>

void screencopy_init(void);
void screencopy_fini(void);
struct wl_global *screencopy_get_global(void);
