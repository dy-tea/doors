#pragma once

#include <wayland-server.h>
#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_text_input_v3.h>

struct ime_text_t;

typedef struct ime_relay_t {
	struct wlr_seat *wlr_seat;
	struct wl_list text_inputs;
	struct wlr_input_method_v2 *input_method;
	struct wlr_surface *focused_surface;

	struct wlr_keyboard_modifiers forwarded_modifiers;

	struct ime_text_t *active_text_input;

	struct wl_list popups;
	struct wlr_scene_tree *popup_tree;

	struct wl_listener new_text_input;
	struct wl_listener new_input_method;

	struct wl_listener input_method_commit;
	struct wl_listener input_method_grab_keyboard;
	struct wl_listener input_method_destroy;
	struct wl_listener input_method_new_popup_surface;

	struct wl_listener keyboard_grab_destroy;
	struct wl_listener focused_surface_destroy;
} ime_relay_t;

typedef struct ime_popup_t {
	struct wlr_input_popup_surface_v2 *popup_surface;
	struct wlr_scene_tree *tree;
	struct wlr_scene_tree *scene_surface;
	ime_relay_t *relay;
	struct wl_list link;

	struct wl_listener destroy;
	struct wl_listener commit;
} ime_popup_t;

typedef struct ime_text_t {
	ime_relay_t *relay;
	struct wlr_text_input_v3 *input;
	struct wl_list link;

	struct wl_listener enable;
	struct wl_listener commit;
	struct wl_listener disable;
	struct wl_listener destroy;
} ime_text_t;

struct ime_relay_t *input_method_relay_create(struct wlr_seat *wlr_seat);
void input_method_relay_finish(ime_relay_t *relay);
void input_method_relay_set_focus(ime_relay_t *relay, struct wlr_surface *surface);

bool input_method_keyboard_grab_forward_key(struct wlr_keyboard *keyboard,
	struct wlr_keyboard_key_event *event, struct ime_relay_t *relay);
bool input_method_keyboard_grab_forward_modifiers(struct wlr_keyboard *keyboard,
	ime_relay_t *relay);
