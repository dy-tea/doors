#pragma once

#include "server.h"

#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_touch.h>

struct seat_t;

void view_begin_interactive(view_t *view, enum cursor_mode mode, uint32_t edges);
void cursor_rebase(void);
void *desktop_type_at(double lx, double ly, struct wlr_surface **surface, double *sx, double *sy);
bool cursor_set_size(uint32_t size);

void cursor_init(void);
void cursor_fini(void);
