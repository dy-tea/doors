#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct wlr_keyboard;
struct wlr_seat;

void global_shortcuts_init(void);
void global_shortcuts_fini(void);

// dispatch a keyboard key event
// returns true if a (non-tap) hotkey consumed the event
bool global_shortcuts_handle_key(struct wlr_keyboard *wkb, struct wlr_seat *wlr_seat,
	uint32_t keycode, uint32_t state, uint32_t time_msec);

// dispatch a pointer button event
// returns true if a hotkey consumed the event
bool global_shortcuts_handle_button(struct wlr_seat *wlr_seat, uint32_t modifiers, uint32_t button,
	uint32_t state, uint32_t time_msec);

// render a human-readable summary of the bound global shortcuts into buf
bool global_shortcuts_list(char *buf, size_t buf_size);
