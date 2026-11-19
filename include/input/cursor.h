#pragma once

#include "server.h"

#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_touch.h>

struct seat_t;

void view_begin_interactive(view_t *view, enum cursor_mode mode, uint32_t edges);

// begin an interactive move, resize or tiling drag on the window under the cursor
bool cursor_interactive_move(void);
bool cursor_interactive_resize(void);
bool cursor_tiling_drag(void);
void cursor_rebase(void);
void *desktop_type_at(double lx, double ly, struct wlr_surface **surface, double *sx, double *sy);
bool cursor_set_size(uint32_t size);

void cursor_init(void);
void cursor_fini(void);
