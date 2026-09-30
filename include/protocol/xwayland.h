#pragma once

#include "view.h"

#include <wayland-server-core.h>
#include <wlr/util/box.h>
#include <wlr/xwayland.h>
#include <xcb/xproto.h>

struct server_t;

enum atom_name {
	NET_WM_WINDOW_TYPE_NORMAL,
	NET_WM_WINDOW_TYPE_DIALOG,
	NET_WM_WINDOW_TYPE_UTILITY,
	NET_WM_WINDOW_TYPE_TOOLBAR,
	NET_WM_WINDOW_TYPE_SPLASH,
	NET_WM_WINDOW_TYPE_MENU,
	NET_WM_WINDOW_TYPE_DROPDOWN_MENU,
	NET_WM_WINDOW_TYPE_POPUP_MENU,
	NET_WM_WINDOW_TYPE_TOOLTIP,
	NET_WM_WINDOW_TYPE_NOTIFICATION,
	NET_WM_STATE_MODAL,
	ATOM_LAST,
};

typedef struct xwayland_t {
	struct wlr_xwayland *wlr_xwayland;
	struct wlr_xcursor_manager *xcursor_manager;
	xcb_atom_t atoms[ATOM_LAST];
} xwayland_t;

typedef struct xwayland_toplevel_t {
	view_t view;

	struct wlr_xwayland_surface *xwayland_surface;

	bool fullscreen_at_map;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener commit;
	struct wl_listener request_configure;
	struct wl_listener request_fullscreen;
	struct wl_listener request_minimize;
	struct wl_listener request_maximize;
	struct wl_listener request_activate;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener set_title;
	struct wl_listener set_class;
	struct wl_listener set_hints;
	struct wl_listener set_window_type;
	struct wl_listener set_startup_id;
	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener override_redirect;
} xwayland_toplevel_t;

typedef struct xwayland_unmanaged_t {
	struct wlr_xwayland_surface *xwayland_surface;
	struct wlr_scene_surface *surface_scene;

	struct wl_listener request_configure;
	struct wl_listener request_activate;
	struct wl_listener set_geometry;
	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener override_redirect;
} xwayland_unmanaged_t;

void xwayland_init(void);
void xwayland_fini(void);
