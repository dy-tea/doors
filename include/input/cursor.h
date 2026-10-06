#pragma once

#include "gesture.h"
#include "server.h"

#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_touch.h>

struct seat_t;

typedef struct cursor_t {
	gesture_tracker_t gesture_tracker;
	struct wl_listener hold_begin;
	struct wl_listener hold_end;
	struct wl_listener pinch_begin;
	struct wl_listener pinch_update;
	struct wl_listener pinch_end;
	struct wl_listener swipe_begin;
	struct wl_listener swipe_update;
	struct wl_listener swipe_end;
} cursor_t;

void view_begin_interactive(view_t *view, enum cursor_mode mode, uint32_t edges);
void cursor_rebase(void);
void *desktop_type_at(double lx, double ly, struct wlr_surface **surface, double *sx, double *sy);
bool cursor_set_size(uint32_t size);

void cursor_init(void);
void cursor_fini(void);
