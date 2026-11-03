#include "input/seat.h"
#include "macros.h"
#include "protocol/global_shortcuts.h"
#include "server.h"
#include "xx-hotkey-v1-protocol.h"
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

#define GS_HOTKEY_VERSION 1
#define GS_MAX_SYMS 8

typedef enum {
	TRIG_NONE = 0,
	TRIG_KEY,
	TRIG_BUTTON,
} trigger_kind_t;

typedef struct gs_hotkey {
	struct wl_resource *resource;
	struct wl_list link;
	char *app_id;

	trigger_kind_t kind;
	uint32_t modifiers;
	union {
		xkb_keysym_t keysym;
		uint32_t button;
	} trigger;
	char *desc;
	struct wl_resource *seat;

	bool bound;
	trigger_kind_t active_kind;
	uint32_t active_modifiers;
	union {
		xkb_keysym_t keysym;
		uint32_t button;
	} active_trigger;
	char *active_desc;
	struct wl_resource *active_seat;

	struct wl_listener seat_destroy;
	bool seat_listener_installed;

	uint32_t trigger_keycode;
	bool down, armed, contaminated;
} gs_hotkey_t;

typedef struct gs_client {
	struct wl_resource *manager_resource;
	char *app_id;
	bool app_id_set;
	struct wl_list link; // gs.clients
} gs_client_t;

static struct {
	struct wl_global *global;
	struct wl_list clients;
	struct wl_list hotkeys;
	struct wl_listener display_destroy;
	bool display_listener_installed;
	bool initialized;
	uint32_t serial;
} gs;

static const uint32_t gs_semantic_mods = WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT |
	WLR_MODIFIER_LOGO;
static const uint32_t gs_xx_mod_mask = XX_HOTKEY_MANAGER_V1_MODIFIERS_SHIFT |
	XX_HOTKEY_MANAGER_V1_MODIFIERS_CTRL | XX_HOTKEY_MANAGER_V1_MODIFIERS_ALT |
	XX_HOTKEY_MANAGER_V1_MODIFIERS_SUPER;

static uint32_t gs_next_serial(void) {
	if (gs.serial == UINT32_MAX)
		gs.serial = 0;
	return ++gs.serial;
}

static uint32_t gs_xx_to_wlr_mods(uint32_t xx) {
	uint32_t m = 0;
	if (xx & XX_HOTKEY_MANAGER_V1_MODIFIERS_SHIFT)
		m |= WLR_MODIFIER_SHIFT;
	if (xx & XX_HOTKEY_MANAGER_V1_MODIFIERS_CTRL)
		m |= WLR_MODIFIER_CTRL;
	if (xx & XX_HOTKEY_MANAGER_V1_MODIFIERS_ALT)
		m |= WLR_MODIFIER_ALT;
	if (xx & XX_HOTKEY_MANAGER_V1_MODIFIERS_SUPER)
		m |= WLR_MODIFIER_LOGO;
	return m;
}

static bool gs_keysym_is_modifier(xkb_keysym_t s) {
	switch (s) {
	case XKB_KEY_Shift_L:
	case XKB_KEY_Shift_R:
	case XKB_KEY_Control_L:
	case XKB_KEY_Control_R:
	case XKB_KEY_Alt_L:
	case XKB_KEY_Alt_R:
	case XKB_KEY_Super_L:
	case XKB_KEY_Super_R:
	case XKB_KEY_Hyper_L:
	case XKB_KEY_Hyper_R:
	case XKB_KEY_Meta_L:
	case XKB_KEY_Meta_R:
	case XKB_KEY_ISO_Level3_Shift:
	case XKB_KEY_ISO_Level3_Latch:
	case XKB_KEY_ISO_Level3_Lock:
	case XKB_KEY_ISO_Next_Group:
	case XKB_KEY_ISO_Prev_Group:
	case XKB_KEY_Mode_switch:
	case XKB_KEY_Caps_Lock:
	case XKB_KEY_Num_Lock:
	case XKB_KEY_Scroll_Lock:
		return true;
	}
	return false;
}

static bool gs_keysym_name_prefix(xkb_keysym_t s, const char *prefix, size_t plen) {
	char name[64];
	int n = xkb_keysym_get_name(s, name, sizeof(name));
	return n >= 0 && (size_t)n >= plen && strncmp(name, prefix, plen) == 0;
}

static bool gs_keysym_is_keypad(xkb_keysym_t s) {
	return gs_keysym_name_prefix(s, "KP", 2);
}

static bool gs_keysym_is_dead(xkb_keysym_t s) {
	return gs_keysym_name_prefix(s, "dead", 4);
}

static bool gs_keysym_has_char(xkb_keysym_t s) {
	char buf[8];
	return xkb_keysym_to_utf8(s, buf, sizeof(buf)) > 0;
}

static int gs_resolve_base_keysyms(struct xkb_keymap *keymap, xkb_keycode_t keycode,
		xkb_keysym_t *out, int maxout) {
	int nlayouts = xkb_keymap_num_layouts(keymap);
	if (nlayouts < 1)
		nlayouts = 1;
	int n = 0;
	for (int l = 0; l < nlayouts && n < maxout; l++) {
		const xkb_keysym_t *syms = NULL;
		int cnt = xkb_keymap_key_get_syms_by_level(keymap, keycode, l, 0, &syms);
		for (int i = 0; i < cnt && n < maxout; i++) {
			bool dup = false;
			for (int j = 0; j < n; j++)
				if (out[j] == syms[i]) {
					dup = true;
				break;
			}
			if (!dup)
				out[n++] = syms[i];
		}
	}
	return n;
}

static bool gs_hotkey_is_tap(const gs_hotkey_t *h) {
	return h->active_kind == TRIG_KEY && gs_keysym_is_modifier(h->active_trigger.keysym);
}

static bool gs_trigger_permitted(const gs_hotkey_t *h) {
	uint32_t m = h->modifiers;
	// ctrl / alt / super always permit the trigger
	if (m & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO))
		return true;

	switch (h->kind) {
	case TRIG_KEY: {
		xkb_keysym_t ks = h->trigger.keysym;
		if (gs_keysym_is_modifier(ks))
			return true; // modifier keysyms (taps) are permitted
		if (gs_keysym_is_keypad(ks))
			return true;
		if (gs_keysym_is_dead(ks))
			return false;
		if (gs_keysym_has_char(ks))
			return false;
		return true; // function / navigation / media keys
	}
	case TRIG_BUTTON:
		return h->trigger.button > BTN_MIDDLE;
	default:
		return false;
	}
}

static bool gs_seat_matches(struct wl_resource *seat_resource, struct wlr_seat *event_seat) {
	if (!seat_resource)
		return true;
	if (!event_seat)
		return false;

	seat_t *s;
	wl_list_for_each(s, &server.seats, link) {
		struct wlr_seat *ws = s->wlr_seat;
		if (!ws)
			continue;
		struct wlr_seat_client *sc;
		wl_list_for_each(sc, &ws->clients, link) {
			struct wl_resource *res;
			wl_list_for_each(res, &sc->resources, link) {
				if (res == seat_resource)
					return ws == event_seat;
			}
		}
	}
	return false;
}

static gs_client_t *gs_client_for_wl_client(struct wl_client *client) {
	if (!client)
		return NULL;
	gs_client_t *c;
	wl_list_for_each(c, &gs.clients, link) {
		if (c->manager_resource && wl_resource_get_client(c->manager_resource) == client)
			return c;
	}
	return NULL;
}

static void gs_hotkey_refresh_seat_listener(gs_hotkey_t *h);
static void gs_hotkey_revoke(gs_hotkey_t *h, const char *msg);

static void gs_seat_destroy_notify(struct wl_listener *listener, void *data) {
	(void)data;
	gs_hotkey_t *h = wl_container_of(listener, h, seat_destroy);
	h->seat_listener_installed = false;
	gs_hotkey_revoke(h, "seat was removed");
}

static void gs_hotkey_refresh_seat_listener(gs_hotkey_t *h) {
	if (h->seat_listener_installed) {
		wl_list_remove(&h->seat_destroy.link);
		h->seat_listener_installed = false;
	}
	if (h->active_seat) {
		h->seat_destroy.notify = gs_seat_destroy_notify;
		wl_signal_add(&h->active_seat->destroy_signal, &h->seat_destroy);
		h->seat_listener_installed = true;
	}
}

static void gs_hotkey_revoke(gs_hotkey_t *h, const char *msg) {
	if (!h->bound)
		return;
	h->bound = false;
	free(h->active_desc);
	h->active_desc = NULL;
	h->active_seat = NULL;
	h->active_kind = TRIG_NONE;
	h->active_modifiers = 0;
	h->active_trigger.keysym = 0;
	h->trigger_keycode = 0;
	h->down = false;
	h->armed = false;
	h->contaminated = false;
	gs_hotkey_refresh_seat_listener(h);
	if (h->resource && wl_resource_get_user_data(h->resource) == (void *)h)
		xx_hotkey_v1_send_revoked(h->resource, msg);
}

static void gs_hotkey_apply(gs_hotkey_t *h) {
	gs_client_t *c = gs_client_for_wl_client(h->resource ? wl_resource_get_client(h->resource) : NULL);

	free(h->app_id);
	h->app_id = (c && c->app_id) ? strdup(c->app_id) : strdup("");

	free(h->active_desc);
	h->active_desc = h->desc ? strdup(h->desc) : strdup("");

	h->active_seat = h->seat;
	h->active_kind = h->kind;
	h->active_modifiers = h->modifiers;
	switch (h->kind) {
	case TRIG_KEY:
		h->active_trigger.keysym = h->trigger.keysym;
		break;
	case TRIG_BUTTON:
		h->active_trigger.button = h->trigger.button;
		break;
	default:
		h->active_kind = TRIG_NONE;
		break;
	}
	h->bound = true;
	h->trigger_keycode = 0;
	h->down = false;
	h->armed = false;
	h->contaminated = false;
	gs_hotkey_refresh_seat_listener(h);
}

static void gs_hotkey_free(gs_hotkey_t *h) {
	if (h->seat_listener_installed) {
		wl_list_remove(&h->seat_destroy.link);
		h->seat_listener_installed = false;
	}
	wl_list_remove(&h->link);
	free(h->desc);
	free(h->active_desc);
	free(h->app_id);
	free(h);
}

static void gs_hotkey_resource_destroy(struct wl_resource *resource) {
	gs_hotkey_t *h = wl_resource_get_user_data(resource);
	if (!h)
		return;
	wl_resource_set_user_data(resource, NULL);
	gs_hotkey_free(h);
}

static void gs_hotkey_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
	(void)client;
	wl_resource_destroy(resource);
}

static void gs_hotkey_handle_set_description(struct wl_client *client, struct wl_resource *resource,
		const char *description) {
	(void)client;
	gs_hotkey_t *h = wl_resource_get_user_data(resource);
	if (!h)
		return;
	free(h->desc);
	h->desc = description ? strdup(description) : strdup("");
}

static void gs_hotkey_handle_set_seat(struct wl_client *client, struct wl_resource *resource,
		struct wl_resource *seat) {
	(void)client;
	gs_hotkey_t *h = wl_resource_get_user_data(resource);
	if (!h)
		return;
	h->seat = seat;
}

static void gs_hotkey_handle_set_key_trigger(struct wl_client *client, struct wl_resource *resource,
		uint32_t keysym, uint32_t modifiers) {
	(void)client;
	gs_hotkey_t *h = wl_resource_get_user_data(resource);
	if (!h)
		return;
	if (modifiers & ~gs_xx_mod_mask) {
		wl_resource_post_error(resource, XX_HOTKEY_V1_ERROR_INVALID_TRIGGER,
			"modifier mask has bits outside the defined set");
		return;
	}
	h->kind = TRIG_KEY;
	h->trigger.keysym = keysym;
	h->modifiers = gs_xx_to_wlr_mods(modifiers);
}

static void gs_hotkey_handle_set_button_trigger(struct wl_client *client,
		struct wl_resource *resource, uint32_t button, uint32_t modifiers) {
	(void)client;
	gs_hotkey_t *h = wl_resource_get_user_data(resource);
	if (!h)
		return;
	if (modifiers & ~gs_xx_mod_mask) {
		wl_resource_post_error(resource, XX_HOTKEY_V1_ERROR_INVALID_TRIGGER,
			"modifier mask has bits outside the defined set");
		return;
	}
	h->kind = TRIG_BUTTON;
	h->trigger.button = button;
	h->modifiers = gs_xx_to_wlr_mods(modifiers);
}

static bool gs_hotkey_is_taken(const gs_hotkey_t *self) {
	const gs_hotkey_t *h;
	wl_list_for_each(h, &gs.hotkeys, link) {
		if (h == self)
			continue;
		if (!h->bound)
			continue;
		if (h->active_kind != self->kind)
			continue;
		if ((h->active_modifiers & gs_semantic_mods) != (self->modifiers & gs_semantic_mods))
			continue;
		if (h->active_seat != self->seat)
			continue;
		if (h->active_kind == TRIG_KEY && h->active_trigger.keysym != self->trigger.keysym)
			continue;
		if (h->active_kind == TRIG_BUTTON && h->active_trigger.button != self->trigger.button)
			continue;
		return true;
	}
	return false;
}

static void gs_hotkey_handle_commit(struct wl_client *client, struct wl_resource *resource) {
	(void)client;
	gs_hotkey_t *h = wl_resource_get_user_data(resource);
	if (!h)
		return;

	if (h->kind == TRIG_NONE) {
		wl_resource_post_error(resource, XX_HOTKEY_V1_ERROR_NO_TRIGGER,
			"commit was sent before a trigger was described");
		return;
	}

	gs_client_t *c = gs_client_for_wl_client(wl_resource_get_client(resource));

	if (!gs_trigger_permitted(h)) {
		wlr_log(WLR_DEBUG, "Denying %s shortcut for %s (not permitted)",
			h->kind == TRIG_KEY ? "key" : "button", c && c->app_id ? c->app_id : "(unidentified)");
		xx_hotkey_v1_send_denied(h->resource, XX_HOTKEY_V1_DENY_REASON_NOT_PERMITTED,
			"trigger is not permitted (bind with ctrl/alt/super, or use a function/"
			"keypad/modifier key)");
		return;
	}

	if (gs_hotkey_is_taken(h)) {
		xx_hotkey_v1_send_denied(h->resource, XX_HOTKEY_V1_DENY_REASON_ALREADY_BOUND,
			"the requested shortcut is already taken");
		return;
	}

	gs_hotkey_apply(h);
	xx_hotkey_v1_send_bound(h->resource);
}

static const struct xx_hotkey_v1_interface hotkey_impl = {
	.destroy = gs_hotkey_handle_destroy,
	.set_description = gs_hotkey_handle_set_description,
	.set_seat = gs_hotkey_handle_set_seat,
	.set_key_trigger = gs_hotkey_handle_set_key_trigger,
	.set_button_trigger = gs_hotkey_handle_set_button_trigger,
	.commit = gs_hotkey_handle_commit,
};

static void gs_manager_handle_destroy(struct wl_client *client_unused,
		struct wl_resource *resource) {
	(void)client_unused;
	wl_resource_destroy(resource);
}

static void gs_manager_handle_set_app_id(struct wl_client *client_unused,
		struct wl_resource *resource, const char *app_id) {
	(void)client_unused;
	gs_client_t *c = wl_resource_get_user_data(resource);
	if (!c)
		return;
	if (!app_id || app_id[0] == '\0') {
		wl_resource_post_error(resource, XX_HOTKEY_MANAGER_V1_ERROR_INVALID_APP_ID,
			"app_id must not be empty");
		return;
	}
	if (c->app_id_set) {
		wl_resource_post_error(resource, XX_HOTKEY_MANAGER_V1_ERROR_INVALID_APP_ID,
			"app_id may only be set once");
		return;
	}
	c->app_id = strdup(app_id);
	c->app_id_set = true;
}

static void gs_manager_handle_create_hotkey(struct wl_client *client_unused,
		struct wl_resource *resource, uint32_t id) {
	(void)client_unused;
	gs_client_t *c = wl_resource_get_user_data(resource);
	if (!c)
		return;

	uint32_t version = wl_resource_get_version(resource);
	if (version > GS_HOTKEY_VERSION)
		version = GS_HOTKEY_VERSION;

	struct wl_resource *res = wl_resource_create(client_unused, &xx_hotkey_v1_interface, version, id);
	if (!res) {
		wl_client_post_no_memory(client_unused);
		return;
	}

	gs_hotkey_t *h = calloc(1, sizeof(*h));
	if (!h) {
		wl_resource_post_no_memory(res);
		return;
	}
	h->resource = res;
	h->kind = TRIG_NONE;
	h->active_kind = TRIG_NONE;
	wl_list_init(&h->link);
	wl_resource_set_implementation(res, &hotkey_impl, h, gs_hotkey_resource_destroy);
	wl_list_insert(&gs.hotkeys, &h->link);
}

static const struct xx_hotkey_manager_v1_interface manager_impl = {
	.destroy = gs_manager_handle_destroy,
	.set_app_id = gs_manager_handle_set_app_id,
	.create_hotkey = gs_manager_handle_create_hotkey,
};

static void gs_client_resource_destroy(struct wl_resource *resource) {
	gs_client_t *c = wl_resource_get_user_data(resource);
	if (!c)
		return;
	wl_resource_set_user_data(resource, NULL);
	wl_list_remove(&c->link);
	free(c->app_id);
	free(c);
}

static void gs_bind_manager(struct wl_client *client_unused, void *data, uint32_t version,
		uint32_t id) {
	(void)data;
	struct wl_resource *res = wl_resource_create(client_unused, &xx_hotkey_manager_v1_interface,
		version, id);
	if (!res) {
		wl_client_post_no_memory(client_unused);
		return;
	}
	gs_client_t *c = calloc(1, sizeof(*c));
	if (!c) {
		wl_resource_post_no_memory(res);
		return;
	}
	c->manager_resource = res;
	wl_list_insert(&gs.clients, &c->link);
	wl_resource_set_implementation(res, &manager_impl, c, gs_client_resource_destroy);
}

static void gs_display_destroy_notify(struct wl_listener *listener, void *data) {
	(void)listener;
	(void)data;
	wl_list_remove(&gs.display_destroy.link);
	gs.display_listener_installed = false;
	gs.initialized = false;
	if (gs.global) {
		wl_global_destroy(gs.global);
		gs.global = NULL;
	}
}

static void gs_destroy(void) {
	if (!gs.initialized)
		return;
	gs_hotkey_t *h, *htmp;
	wl_list_for_each_safe(h, htmp, &gs.hotkeys, link)
		wl_resource_destroy(h->resource);
	gs_client_t *c, *ctmp;
	wl_list_for_each_safe(c, ctmp, &gs.clients, link)
		wl_resource_destroy(c->manager_resource);
	if (gs.display_listener_installed) {
		wl_list_remove(&gs.display_destroy.link);
		gs.display_listener_installed = false;
	}
	if (gs.global)
		wl_global_destroy(gs.global);
	gs.global = NULL;
	gs.initialized = false;
}

void global_shortcuts_init(void) {
	ONCE();
	wl_list_init(&gs.clients);
	wl_list_init(&gs.hotkeys);
	gs.global = wl_global_create(server.wl_display, &xx_hotkey_manager_v1_interface, GS_HOTKEY_VERSION,
		NULL, gs_bind_manager);
	if (!gs.global) {
		wlr_log(WLR_ERROR, "Failed to create global");
		return;
	}
	wl_list_init(&gs.display_destroy.link);
	gs.display_destroy.notify = gs_display_destroy_notify;
	wl_display_add_destroy_listener(server.wl_display, &gs.display_destroy);
	gs.display_listener_installed = true;
	gs.initialized = true;
}

void global_shortcuts_fini(void) {
	ONCE();
	gs_destroy();
}

static void gs_contaminate_armed(void) {
	gs_hotkey_t *h;
	wl_list_for_each(h, &gs.hotkeys, link) {
		if (h->bound && h->armed)
			h->contaminated = true;
	}
}

bool global_shortcuts_handle_key(struct wlr_keyboard *wkb, struct wlr_seat *wlr_seat,
		uint32_t keycode, uint32_t state, uint32_t time_msec) {
	if (!gs.initialized || !wkb || !wkb->keymap)
		return false;

	uint32_t modifiers = wlr_keyboard_get_modifiers(wkb);
	xkb_keysym_t syms[GS_MAX_SYMS];
	int nsyms = gs_resolve_base_keysyms(wkb->keymap, keycode, syms, GS_MAX_SYMS);

	bool pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;
	if (pressed)
		gs_contaminate_armed();

	bool consumed = false;
	gs_hotkey_t *h;
	wl_list_for_each(h, &gs.hotkeys, link) {
		if (!h->bound || h->active_kind != TRIG_KEY)
			continue;
		if (!gs_seat_matches(h->active_seat, wlr_seat))
			continue;
		if ((modifiers & gs_semantic_mods) != (h->active_modifiers & gs_semantic_mods))
			continue;

		bool matched = false;
		for (int i = 0; i < nsyms; i++)
			if (syms[i] == h->active_trigger.keysym) {
				matched = true;
			break;
		}
		if (!matched)
			continue;

		if (gs_hotkey_is_tap(h)) {
			if (pressed) {
				h->armed = true;
				h->contaminated = false;
			} else if (h->armed && !h->contaminated) {
				uint32_t serial = gs_next_serial();
				xx_hotkey_v1_send_triggered(h->resource, serial, time_msec);
				xx_hotkey_v1_send_released(h->resource, serial, time_msec);
			}
			h->armed = false;
		} else {
			if (pressed) {
				if (!h->down) {
					uint32_t serial = gs_next_serial();
					xx_hotkey_v1_send_triggered(h->resource, serial, time_msec);
					h->trigger_keycode = keycode;
					h->down = true;
				}
				consumed = true;
			} else if (h->down && h->trigger_keycode == keycode) {
				uint32_t serial = gs_next_serial();
				xx_hotkey_v1_send_released(h->resource, serial, time_msec);
				h->down = false;
				consumed = true;
			}
		}
	}
	return consumed;
}

bool global_shortcuts_handle_button(struct wlr_seat *wlr_seat, uint32_t modifiers, uint32_t button,
		uint32_t state, uint32_t time_msec) {
	if (!gs.initialized)
		return false;

	bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
	if (pressed)
		gs_contaminate_armed();

	bool consumed = false;
	gs_hotkey_t *h;
	wl_list_for_each(h, &gs.hotkeys, link) {
		if (!h->bound || h->active_kind != TRIG_BUTTON)
			continue;
		if (!gs_seat_matches(h->active_seat, wlr_seat))
			continue;
		if ((modifiers & gs_semantic_mods) != (h->active_modifiers & gs_semantic_mods))
			continue;
		if (h->active_trigger.button != button)
			continue;

		if (pressed) {
			if (!h->down) {
				uint32_t serial = gs_next_serial();
				xx_hotkey_v1_send_triggered(h->resource, serial, time_msec);
				h->down = true;
			}
			consumed = true;
		} else if (h->down) {
			uint32_t serial = gs_next_serial();
			xx_hotkey_v1_send_released(h->resource, serial, time_msec);
			h->down = false;
			consumed = true;
		}
	}
	return consumed;
}

static void gs_format_trigger(const gs_hotkey_t *h, char *out, size_t size) {
	size_t n = 0;
	if (h->active_kind == TRIG_KEY) {
		uint32_t m = h->active_modifiers;
		if (m & WLR_MODIFIER_SHIFT)
			n += snprintf(out + n, size - n, "Shift+");
		if (m & WLR_MODIFIER_CTRL)
			n += snprintf(out + n, size - n, "Ctrl+");
		if (m & WLR_MODIFIER_ALT)
			n += snprintf(out + n, size - n, "Alt+");
		if (m & WLR_MODIFIER_LOGO)
			n += snprintf(out + n, size - n, "Super+");
		char kn[32];
		xkb_keysym_get_name(h->active_trigger.keysym, kn, sizeof(kn));
		n += snprintf(out + n, size - n, "%s", kn);
	} else if (h->active_kind == TRIG_BUTTON) {
		n += snprintf(out + n, size - n, "Button(0x%x)", h->active_trigger.button);
	}
	if (gs_hotkey_is_tap(h))
		n += snprintf(out + n, size - n, " [tap]");
	if (n >= size)
		out[size - 1] = '\0';
}

void global_shortcuts_list(char *buf, size_t buf_size) {
	size_t off = 0;
	off += snprintf(buf + off, buf_size - off, "global shortcuts (xx-hotkey-v1):\n");
	if (off >= buf_size)
		return;

	gs_hotkey_t *h;
	wl_list_for_each(h, &gs.hotkeys, link) {
		if (!h->bound)
			continue;
		char trig[128];
		gs_format_trigger(h, trig, sizeof(trig));
		const char *app = (h->app_id && h->app_id[0]) ? h->app_id : "(unidentified)";
		const char *desc = (h->active_desc && h->active_desc[0]) ? h->active_desc : "(none)";
		off += snprintf(buf + off, buf_size - off, "\t%s\t%s\tdesc: %s\n", app, trig, desc);
		if (off >= buf_size)
			return;
	}
}
