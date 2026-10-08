#include "animation.h"
#include "effects/backend.h"
#include "input/cursor.h"
#include "input/input_method.h"
#include "input/seat.h"
#include "ipc/ipc.h"
#include "launcher.h"
#include "layout/layout.h"
#include "once.h"
#include "output/output.h"
#include "protocol/workspace.h"
#include "protocol/xwayland.h"
#include "render_unfocused.h"
#include "rule.h"
#include "scratchpad.h"
#include "server.h"
#include "surface.h"
#include "tree.h"
#include "types.h"
#include <pixman.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/xwayland.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>

static const char *atom_map[ATOM_LAST] = {
	[NET_WM_WINDOW_TYPE_NORMAL] = "_NET_WM_WINDOW_TYPE_NORMAL",
	[NET_WM_WINDOW_TYPE_DIALOG] = "_NET_WM_WINDOW_TYPE_DIALOG",
	[NET_WM_WINDOW_TYPE_UTILITY] = "_NET_WM_WINDOW_TYPE_UTILITY",
	[NET_WM_WINDOW_TYPE_TOOLBAR] = "_NET_WM_WINDOW_TYPE_TOOLBAR",
	[NET_WM_WINDOW_TYPE_SPLASH] = "_NET_WM_WINDOW_TYPE_SPLASH",
	[NET_WM_WINDOW_TYPE_MENU] = "_NET_WM_WINDOW_TYPE_MENU",
	[NET_WM_WINDOW_TYPE_DROPDOWN_MENU] = "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU",
	[NET_WM_WINDOW_TYPE_POPUP_MENU] = "_NET_WM_WINDOW_TYPE_POPUP_MENU",
	[NET_WM_WINDOW_TYPE_TOOLTIP] = "_NET_WM_WINDOW_TYPE_TOOLTIP",
	[NET_WM_WINDOW_TYPE_NOTIFICATION] = "_NET_WM_WINDOW_TYPE_NOTIFICATION",
	[NET_WM_STATE_MODAL] = "_NET_WM_STATE_MODAL",
};

static void xwayland_toplevel_destroy(xwayland_toplevel_t *xwayland_toplevel);
static xwayland_toplevel_t *xwayland_toplevel_create(struct wlr_xwayland_surface *xsurface);
static xwayland_unmanaged_t *xwayland_unmanaged_create(struct wlr_xwayland_surface *xsurface);
static void xwayland_relative_to_absolute(struct wlr_xwayland_surface *parent, int child_x,
		int child_y, int *abs_x, int *abs_y) {
	if (parent && !parent->override_redirect && parent->data) {
		xwayland_toplevel_t *parent_toplevel = parent->data;
		if (parent_toplevel->xwayland_surface && parent_toplevel->view.node) {
			int x_offset = child_x - parent_toplevel->xwayland_surface->x;
			int y_offset = child_y - parent_toplevel->xwayland_surface->y;
			node_t *parent_node = parent_toplevel->view.node;
			if (parent_node->client && parent_node->client->state == STATE_FLOATING) {
				*abs_x = parent_node->client->floating_rectangle.x + x_offset;
				*abs_y = parent_node->client->floating_rectangle.y + y_offset;
			} else {
				*abs_x = parent_node->rectangle.x + x_offset;
				*abs_y = parent_node->rectangle.y + y_offset;
			}
			return;
		}
	}
	*abs_x += parent ? parent->x : 0;
	*abs_y += parent ? parent->y : 0;
}

static void unmanaged_handle_request_configure(struct wl_listener *listener, void *data) {
	xwayland_unmanaged_t *surface = wl_container_of(listener, surface, request_configure);
	struct wlr_xwayland_surface *xsurface = surface->xwayland_surface;
	struct wlr_xwayland_surface_configure_event *ev = data;

	wlr_xwayland_surface_configure(xsurface, ev->x, ev->y, ev->width, ev->height);

	if (surface->surface_scene) {
		int abs_x = ev->x;
		int abs_y = ev->y;
		if (xsurface->parent)
			xwayland_relative_to_absolute(xsurface->parent, ev->x, ev->y, &abs_x, &abs_y);
		wlr_scene_node_set_position(&surface->surface_scene->buffer->node, abs_x, abs_y);
	}
}

static void unmanaged_handle_set_geometry(struct wl_listener *listener, void *data) {
	(void)data;
	xwayland_unmanaged_t *surface = wl_container_of(listener, surface, set_geometry);
	struct wlr_xwayland_surface *xsurface = surface->xwayland_surface;

	int abs_x = xsurface->x;
	int abs_y = xsurface->y;
	if (xsurface->parent)
		xwayland_relative_to_absolute(xsurface->parent, xsurface->x, xsurface->y, &abs_x, &abs_y);
	wlr_scene_node_set_position(&surface->surface_scene->buffer->node, abs_x, abs_y);
}

static void unmanaged_handle_map(struct wl_listener *listener, void *data) {
	(void)data;
	xwayland_unmanaged_t *surface = wl_container_of(listener, surface, map);
	struct wlr_xwayland_surface *xsurface = surface->xwayland_surface;

	surface->surface_scene = wlr_scene_surface_create(server.over_tree, xsurface->surface);

	if (surface->surface_scene) {
		int abs_x = xsurface->x;
		int abs_y = xsurface->y;

		wlr_log(WLR_INFO, "Unmanaged window %u map: x=%d y=%d size=%dx%d parent=%p", xsurface->window_id,
			xsurface->x, xsurface->y, xsurface->width, xsurface->height, (void *)xsurface->parent);

		if (xsurface->parent) {
			wlr_log(WLR_INFO, "Parent window %u at X11 coords (%d,%d) override_redirect=%d data=%p",
				xsurface->parent->window_id, xsurface->parent->x, xsurface->parent->y,
				xsurface->parent->override_redirect, xsurface->parent->data);

			// if parent is a managed window, use its X11 coordinates
			xwayland_relative_to_absolute(xsurface->parent, xsurface->x, xsurface->y, &abs_x, &abs_y);
		} else {
			view_t *focused_view = server.last_focused_xwayland_view;
			bool resolved_from_node = false;

			if (!focused_view) {
				wlr_log(WLR_INFO, "Last focused is NULL, searching for any mapped xwayland window");
				output_t *mon = server.focused_output ? server.focused_output : (wl_list_empty(&mon_list) ? NULL
					: wl_container_of(mon_list.next, mon, link));

				if (mon && mon->desk) {
					desktop_t *d = mon->desk;
					node_t *n = d->root;

					while (n && !resolved_from_node) {
						if (n->client && n->client->view && n->client->view->type == VIEW_XWAYLAND) {
							if (n->rectangle.x > 0 || n->rectangle.y > 0 || n->client->floating_rectangle.x > 0 ||
									n->client->floating_rectangle.y > 0) {
								if (n->client->state == STATE_FLOATING) {
									abs_x = n->client->floating_rectangle.x + xsurface->x;
									abs_y = n->client->floating_rectangle.y + xsurface->y;
								} else {
									abs_x = n->rectangle.x + xsurface->x;
									abs_y = n->rectangle.y + xsurface->y;
								}

								wlr_log(WLR_INFO, "Using found node position, final: (%d,%d)", abs_x, abs_y);
								resolved_from_node = true;
								break;
							}
						}

						if (n->first_child) {
							n = n->first_child;
						} else if (n->second_child) {
							n = n->second_child;
						} else {
							while (n->parent) {
								if (n == n->parent->first_child && n->parent->second_child) {
									n = n->parent->second_child;
									break;
								}
								n = n->parent;
							}

							if (!n->parent)
								break;
						}
					}
				}
			}

			if (!resolved_from_node && focused_view && focused_view->mapped && focused_view->client) {
				xwayland_relative_to_absolute(view_to_xwayland(focused_view)->xwayland_surface, xsurface->x,
					xsurface->y, &abs_x, &abs_y);
			}
		}

		wlr_scene_node_set_position(&surface->surface_scene->buffer->node, abs_x, abs_y);

		wl_signal_add(&xsurface->events.set_geometry, &surface->set_geometry);
		surface->set_geometry.notify = unmanaged_handle_set_geometry;
	}

	// focus override-redirect windows that want focus
	if (wlr_xwayland_surface_override_redirect_wants_focus(xsurface)) {
		struct wlr_xwayland *xwayland = server.xwayland.wlr_xwayland;
		seat_t *s = seat_default();
		if (!s)
			return;
		wlr_xwayland_set_seat(xwayland, s->wlr_seat);
		wlr_seat_keyboard_notify_enter(s->wlr_seat, xsurface->surface, NULL, 0, NULL);
		if (s->input_method_relay)
			input_method_relay_set_focus(s->input_method_relay, xsurface->surface);
	}
}

static void unmanaged_handle_unmap(struct wl_listener *listener, void *data) {
	(void)data;
	xwayland_unmanaged_t *surface = wl_container_of(listener, surface, unmap);

	if (surface->surface_scene) {
		wl_list_remove(&surface->set_geometry.link);
		wlr_scene_node_destroy(&surface->surface_scene->buffer->node);
		surface->surface_scene = NULL;
	}
}

static void unmanaged_handle_request_activate(struct wl_listener *listener, void *data) {
	(void)data;
	xwayland_unmanaged_t *surface = wl_container_of(listener, surface, request_activate);
	struct wlr_xwayland_surface *xsurface = surface->xwayland_surface;

	if (xsurface->surface == NULL || !xsurface->surface->mapped)
		return;

	if (wlr_xwayland_surface_override_redirect_wants_focus(xsurface)) {
		seat_t *s = seat_default();
		if (!s)
			return;
		wlr_seat_keyboard_notify_enter(s->wlr_seat, xsurface->surface, NULL, 0, NULL);
		if (s->input_method_relay)
			input_method_relay_set_focus(s->input_method_relay, xsurface->surface);
	}
}

static void unmanaged_handle_associate(struct wl_listener *listener, void *data) {
	(void)data;
	xwayland_unmanaged_t *surface = wl_container_of(listener, surface, associate);
	struct wlr_xwayland_surface *xsurface = surface->xwayland_surface;

	wl_signal_add(&xsurface->surface->events.map, &surface->map);
	surface->map.notify = unmanaged_handle_map;
	wl_signal_add(&xsurface->surface->events.unmap, &surface->unmap);
	surface->unmap.notify = unmanaged_handle_unmap;
}

static void unmanaged_handle_dissociate(struct wl_listener *listener, void *data) {
	(void)data;
	xwayland_unmanaged_t *surface = wl_container_of(listener, surface, dissociate);

	wl_list_remove(&surface->map.link);
	wl_list_remove(&surface->unmap.link);
}

static void unmanaged_handle_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	xwayland_unmanaged_t *surface = wl_container_of(listener, surface, destroy);

	wl_list_remove(&surface->request_configure.link);
	wl_list_remove(&surface->request_activate.link);
	wl_list_remove(&surface->associate.link);
	wl_list_remove(&surface->dissociate.link);
	wl_list_remove(&surface->destroy.link);
	wl_list_remove(&surface->override_redirect.link);
	free(surface);
}

static void unmanaged_handle_override_redirect(struct wl_listener *listener, void *data) {
	(void)data;
	xwayland_unmanaged_t *surface = wl_container_of(listener, surface, override_redirect);
	struct wlr_xwayland_surface *xsurface = surface->xwayland_surface;

	bool associated = xsurface->surface != NULL;
	bool mapped = associated && xsurface->surface->mapped;

	if (mapped)
		unmanaged_handle_unmap(&surface->unmap, NULL);
	if (associated)
		unmanaged_handle_dissociate(&surface->dissociate, NULL);

	unmanaged_handle_destroy(&surface->destroy, NULL);
	xsurface->data = NULL;

	xwayland_toplevel_create(xsurface);
}

static struct xwayland_unmanaged_t *xwayland_unmanaged_create(struct wlr_xwayland_surface *xsurface)
		{
	xwayland_unmanaged_t *surface = calloc(1, sizeof(*surface));
	if (surface == NULL)
		return NULL;

	surface->xwayland_surface = xsurface;

	wl_signal_add(&xsurface->events.request_configure, &surface->request_configure);
	surface->request_configure.notify = unmanaged_handle_request_configure;
	wl_signal_add(&xsurface->events.request_activate, &surface->request_activate);
	surface->request_activate.notify = unmanaged_handle_request_activate;
	wl_signal_add(&xsurface->events.associate, &surface->associate);
	surface->associate.notify = unmanaged_handle_associate;
	wl_signal_add(&xsurface->events.dissociate, &surface->dissociate);
	surface->dissociate.notify = unmanaged_handle_dissociate;
	wl_signal_add(&xsurface->events.destroy, &surface->destroy);
	surface->destroy.notify = unmanaged_handle_destroy;
	wl_signal_add(&xsurface->events.set_override_redirect, &surface->override_redirect);
	surface->override_redirect.notify = unmanaged_handle_override_redirect;

	return surface;
}

static bool xwayland_toplevel_wants_floating(struct xwayland_toplevel_t *xwayland_toplevel) {
	struct wlr_xwayland_surface *surface = xwayland_toplevel->xwayland_surface;

	if (surface->modal)
		return true;

	xwayland_t *xwayland = &server.xwayland;
	for (size_t i = 0; i < surface->window_type_len; i++) {
		xcb_atom_t type = surface->window_type[i];
		if (type == xwayland->atoms[NET_WM_WINDOW_TYPE_DIALOG] ||
				type == xwayland->atoms[NET_WM_WINDOW_TYPE_UTILITY] ||
				type == xwayland->atoms[NET_WM_WINDOW_TYPE_TOOLBAR] ||
				type == xwayland->atoms[NET_WM_WINDOW_TYPE_SPLASH]) {
			return true;
		}
	}

	xcb_size_hints_t *size_hints = surface->size_hints;
	if (size_hints != NULL && size_hints->min_width > 0 && size_hints->min_height > 0 &&
			(size_hints->max_width == size_hints->min_width ||
			size_hints->max_height == size_hints->min_height)) {
		return true;
	}

	return false;
}

static void xwayland_toplevel_configure(xwayland_toplevel_t *xwayland_toplevel, int x, int y,
		int width, int height) {
	view_configure(&xwayland_toplevel->view, (struct wlr_box){
		.x = x,
		.y = y,
		.width = width,
		.height = height,
	});

	if (xwayland_toplevel->view.scene_tree && xwayland_toplevel->view.client &&
			xwayland_toplevel->view.client->state == STATE_FLOATING) {
		wlr_scene_node_set_position(&xwayland_toplevel->view.scene_tree->node, x, y);
	}
}

// push the geometry the layout chosen by layout to an X11 client
static void xwayland_sync_configure(xwayland_toplevel_t *xwayland_toplevel) {
	node_t *node = xwayland_toplevel->view.node;
	struct wlr_xwayland_surface *xsurface = xwayland_toplevel->xwayland_surface;

	if (node == NULL || node->client == NULL || xsurface == NULL)
		return;

	client_t *c = node->client;
	struct wlr_box rect = c->arranged_rectangle;

	if (c->state == STATE_FULLSCREEN && node->output != NULL) {
		rect = node->output->rectangle;
	} else if (rect.width < 1 || rect.height < 1) {
		rect = c->tiled_rectangle;
	}

	if (rect.width < 1 || rect.height < 1)
		return;

	view_configure(&xwayland_toplevel->view, rect);

	xwayland_toplevel->view.geometry.width = rect.width;
	xwayland_toplevel->view.geometry.height = rect.height;
}

static void xwayland_view_impl_set_activated(view_t *view, bool activated) {
	xwayland_toplevel_t *xwayland_toplevel = view_to_xwayland(view);
	if (xwayland_toplevel == NULL)
		return;

	struct wlr_xwayland_surface *surface = xwayland_toplevel->xwayland_surface;
	if (!surface || !surface->surface)
		return;

	if (activated && surface->minimized)
		wlr_xwayland_surface_set_minimized(surface, false);

	wlr_xwayland_surface_activate(surface, activated);

	if (activated)
		wlr_xwayland_surface_set_fullscreen(surface, surface->fullscreen);
}

static void xwayland_toplevel_close(xwayland_toplevel_t *xwayland_toplevel) {
	if (!xwayland_toplevel || !xwayland_toplevel->xwayland_surface)
		return;

	struct wlr_xwayland_surface *surface = xwayland_toplevel->xwayland_surface;
	wlr_xwayland_surface_close(surface);
}

static void xwayland_view_impl_set_decorations(view_t *view) {
	(void)view;
}

static void xwayland_view_impl_close(view_t *view) {
	xwayland_toplevel_close(view_to_xwayland(view));
}

static void xwayland_view_impl_configure(view_t *view, struct wlr_box rect) {
	xwayland_toplevel_t *xwayland_toplevel = view_to_xwayland(view);
	if (xwayland_toplevel == NULL || xwayland_toplevel->xwayland_surface == NULL)
		return;

	// x11 has no configure serial to correlate against and no dedup of its own,
	// callers decide whether a repeat is worth sending
	wlr_xwayland_surface_configure(xwayland_toplevel->xwayland_surface, rect.x, rect.y, rect.width,
		rect.height);
}

static const view_impl_t xwayland_view_impl = {
	.set_activated = xwayland_view_impl_set_activated,
	.close = xwayland_view_impl_close,
	.set_decorations = xwayland_view_impl_set_decorations,
	.configure = xwayland_view_impl_configure,
};

static void handle_commit(struct wl_listener *listener, void *data) {
	(void)data;
	xwayland_toplevel_t *xwayland_toplevel = wl_container_of(listener, xwayland_toplevel, commit);
	struct wlr_xwayland_surface *xsurface = xwayland_toplevel->xwayland_surface;
	struct wlr_surface_state *state = &xsurface->surface->current;

	struct wlr_box new_geo = {0};
	new_geo.width = state->width;
	new_geo.height = state->height;

	if (xwayland_toplevel->view.geometry.width != new_geo.width ||
			xwayland_toplevel->view.geometry.height != new_geo.height) {
		xwayland_toplevel->view.geometry = new_geo;
		if (xwayland_toplevel->view.client && xwayland_toplevel->view.client->state == STATE_FLOATING) {
			xwayland_toplevel->view.client->floating_rectangle.width = new_geo.width;
			xwayland_toplevel->view.client->floating_rectangle.height = new_geo.height;
		}

		if (xwayland_toplevel->view.node && !animation_is_resizing(xwayland_toplevel->view.node))
			view_center_and_clip_surface(&xwayland_toplevel->view);
	}

	// update opacity
	if (xwayland_toplevel->view.client && xwayland_toplevel->view.scene_tree)
		surface_set_opacity(&xwayland_toplevel->view.scene_tree->node,
			xwayland_toplevel->view.client->opacity);

	if (xwayland_toplevel->view.node && xwayland_toplevel->view.node->output)
		output_schedule_frame(xwayland_toplevel->view.node->output);
}

static void handle_map(struct wl_listener *listener, void *data) {
	(void)data;
	xwayland_toplevel_t *xwayland_toplevel = wl_container_of(listener, xwayland_toplevel, map);
	struct wlr_xwayland_surface *xsurface = xwayland_toplevel->xwayland_surface;

	xwayland_toplevel->view.mapped = true;
	xwayland_toplevel->view.configured = false;
	xwayland_toplevel->fullscreen_at_map = false;

	if (!xsurface->surface) {
		wlr_log(WLR_ERROR, "XWayland surface %u has no wlr_surface at map time", xsurface->window_id);
		return;
	}

	wlr_log(WLR_DEBUG, "XWayland map: window_id=%u class='%s' title='%s' "
		"override_redirect=%d mapped=%d", xsurface->window_id,
			xsurface->class ? xsurface->class : "(null)", xsurface->title ? xsurface->title : "(null)",
			xsurface->override_redirect, xsurface->surface->mapped);

	wlr_scene_subsurface_tree_create(xwayland_toplevel->view.content_tree, xsurface->surface);

	wl_signal_add(&xsurface->surface->events.commit, &xwayland_toplevel->commit);
	xwayland_toplevel->commit.notify = handle_commit;

	bool wants_float = xwayland_toplevel_wants_floating(xwayland_toplevel);

	xwayland_toplevel->view.geometry.width = xsurface->width;
	xwayland_toplevel->view.geometry.height = xsurface->height;

	output_t *mon = server.focused_output ? server.focused_output : (wl_list_empty(&mon_list) ? NULL :
		wl_container_of(mon_list.next, mon, link));
	if (!mon) {
		wlr_log(WLR_ERROR, "No monitor available for xwayland view %p", (void *)xsurface);
		return;
	}

	desktop_t *d = mon->desk;
	if (!d) {
		wlr_log(WLR_ERROR, "No desktop available for xwayland view %p", (void *)xsurface);
		return;
	}

	node_t *node = make_node(next_node_id++);
	if (!node) {
		wlr_log(WLR_ERROR, "Failed to create node for xwayland view %p", (void *)xsurface);
		return;
	}

	client_t *client = make_client();
	if (!client) {
		wlr_log(WLR_ERROR, "Failed to create client for xwayland view %p", (void *)xsurface);
		free_node(node);
		return;
	}

	node->client = client;

	client->view = &xwayland_toplevel->view;
	xwayland_toplevel->view.client = client;
	xwayland_toplevel->view.node = node;
	node->output = mon;

	// populate constraints from xwayland size hints
	xcb_size_hints_t *size_hints = xsurface->size_hints;
	if (size_hints != NULL) {
		if (size_hints->min_width > 0)
			node->constraints.min_width = size_hints->min_width;
		if (size_hints->min_height > 0)
			node->constraints.min_height = size_hints->min_height;
	}

	const char *app_id = xsurface->class;
	const char *title = xsurface->title;

	client_set_app_id(client, app_id);
	client_set_title(client, title);

	rule_consequence_t *rule = find_matching_rule(app_id, title, NULL);

	if (rule && rule->has & RULE_TYPE_MANAGE && !(rule->flags & RULE_TYPE_MANAGE)) {
		wlr_log(WLR_INFO, "XWayland window %s ignored by rule (manage=off)", app_id ? app_id : "?");
		xwayland_toplevel->view.node = NULL;
		xwayland_toplevel->view.client = NULL;
		free_node(node);
		return;
	}

	bool should_focus = true;
	if (rule && rule->has & RULE_TYPE_FOCUS && !(rule->flags & RULE_TYPE_FOCUS))
		should_focus = false;

	desktop_t *target_desktop = d;
	if (rule && rule->has & RULE_TYPE_DESKTOP) {
		desktop_t *new_desk = find_desktop_by_name(rule->desktop);
		if (new_desk)
			target_desktop = new_desk;
		else
			wlr_log(WLR_ERROR, "XWayland rule: desktop '%s' not found", rule->desktop);
	}

	output_t *target_monitor = target_desktop->output ? target_desktop->output : mon;
	node->output = target_monitor;

	rule_apply_consequence(node, client, rule);
	rule_apply_view_consequence(&xwayland_toplevel->view, rule);

	render_unfocused_client_update(client);

	wlr_log(WLR_INFO, "XWayland window mapped: %s (%s) wants_float=%d size=%dx%d",
		title ? title : "untitled", app_id ? app_id : "unknown", wants_float, xsurface->width,
		xsurface->height);

	view_create_foreign_toplevels(&xwayland_toplevel->view, app_id, title);

	bool rule_forces_float = rule && rule->has & RULE_TYPE_STATE && rule->state == STATE_FLOATING;
	if (wants_float || rule_forces_float) {
		wlr_scene_node_reparent(&xwayland_toplevel->view.scene_tree->node, server.float_tree);
		client->floating_rectangle.x = xsurface->x;
		client->floating_rectangle.y = xsurface->y;
		client->floating_rectangle.width = xsurface->width;
		client->floating_rectangle.height = xsurface->height;
		client->state = STATE_FLOATING;
		client->last_state = STATE_TILED;

		wlr_scene_node_set_position(&xwayland_toplevel->view.scene_tree->node,
			client->floating_rectangle.x, client->floating_rectangle.y);

		node_set_hidden(node, false);
		// a rule can ask for the toplevel to start out minimized
		if (!client->flags.minimized) {
			client->flags.shown = true;
			wlr_scene_node_set_enabled(&xwayland_toplevel->view.scene_tree->node, true);
		}
	} else if (!(rule && rule->has & RULE_TYPE_STATE) && layout_init_client(target_monitor,
			target_desktop, client)) {
		// tell the client about the size and position the layout picked
		struct wlr_box *rect = &client->floating_rectangle;
		xwayland_toplevel_configure(xwayland_toplevel, rect->x, rect->y, rect->width, rect->height);
	} else {
		if (client->state != STATE_PSEUDO_TILED)
			client->state = STATE_TILED;

		node->rectangle.width = xsurface->width;
		node->rectangle.height = xsurface->height;
		// a rule can ask for the toplevel to start out minimized
		if (!client->flags.minimized) {
			client->flags.shown = true;
			wlr_scene_node_set_enabled(&xwayland_toplevel->view.scene_tree->node, true);
			wlr_scene_node_set_enabled(&xwayland_toplevel->view.content_tree->node, true);
		}

		wlr_log(WLR_DEBUG, "XWayland window will be tiled, scene_tree=%p enabled=%d",
			(void *)xwayland_toplevel->view.scene_tree, xwayland_toplevel->view.scene_tree->node.enabled);
	}

	bool target_desktop_is_focused = (target_desktop == target_monitor->desk);

	// floating toplevels are kept out of the tree
	if (IS_FLOATING(client)) {
		node->desktop = target_desktop;
	} else {
		insert_node(target_desktop, node, target_desktop->focus);
	}

	if (target_desktop != d && !target_desktop_is_focused) {
		client->flags.shown = false;
		wlr_scene_node_set_enabled(&xwayland_toplevel->view.scene_tree->node, false);
	}

	if (should_focus)
		activate_node(target_monitor, target_desktop, node);

	if (xsurface->fullscreen &&
			target_desktop->fullscreen_recreate_pending_window_id == xsurface->window_id) {
		target_desktop->fullscreen_recreate_pending_window_id = 0;
	} else if (rule && rule->has & RULE_TYPE_STATE && rule->state == STATE_FULLSCREEN) {
		target_desktop->fullscreen_recreate_pending_window_id = 0;
		client_set_fullscreen(target_monitor, target_desktop, node, true);
	} else if (xsurface->fullscreen && settings.ignore_ewmh_fullscreen != 1) {
		target_desktop->fullscreen_recreate_pending_window_id = 0;
		client_set_fullscreen(target_monitor, target_desktop, node, true);
		xwayland_toplevel->fullscreen_at_map = true;
	} else if (xsurface->maximized_vert || xsurface->maximized_horz) {
		client_set_maximized(target_monitor, target_desktop, node, true);
		wlr_xwayland_surface_set_maximized(xsurface, true, true);
	}

	// notify client of scale
	if (target_monitor && target_monitor->wlr_output && xsurface->surface) {
		float scale = target_monitor->wlr_output->scale;
		wlr_fractional_scale_v1_notify_scale(xsurface->surface, scale);
		wlr_surface_set_preferred_buffer_scale(xsurface->surface, ceil(scale));
	}

	if (client->flags.minimized)
		desktop_minimized_push(target_desktop, node);

	arrange(target_monitor, target_desktop, true);

	ipc_put_status(SUB_MASK_NODE_ADD, "node_add[%s,%s,%u]\n",
		client && client->app_id[0] ? client->app_id : "?",
		client && client->title[0] ? client->title : "?", node->id);

	view_set_activated(&xwayland_toplevel->view, true);
	server.last_focused_xwayland_view = &xwayland_toplevel->view;

	if (!client->flags.block_out_from_screenshare) {
		xwayland_toplevel->view.image_capture_surface =
			wlr_scene_surface_create(&xwayland_toplevel->view.image_capture->tree, xsurface->surface);
	}
	view_update_image_capture_block_out(&xwayland_toplevel->view);

	client_update_foreign_toplevel_state(client);

	wlr_log(WLR_DEBUG, "XWayland window map complete: scene_tree enabled=%d shown=%d",
		xwayland_toplevel->view.scene_tree->node.enabled, client->flags.shown);
}

static void handle_unmap(struct wl_listener *listener, void *data) {
	(void)data;
	xwayland_toplevel_t *xwayland_toplevel = wl_container_of(listener, xwayland_toplevel, unmap);

	xwayland_toplevel->view.mapped = false;
	xwayland_toplevel->view.configured = false;
	if (xwayland_toplevel->view.node)
		animation_cancel_node(xwayland_toplevel->view.node);

	wl_list_remove(&xwayland_toplevel->commit.link);

	xwayland_toplevel->view.image_capture_surface = NULL;

	view_destroy_foreign_toplevels(&xwayland_toplevel->view);

	if (xwayland_toplevel->view.scene_tree) {
		wlr_scene_node_set_enabled(&xwayland_toplevel->view.scene_tree->node, false);
		wlr_scene_node_set_enabled(&xwayland_toplevel->view.content_tree->node, false);
	}
	if (xwayland_toplevel->view.node) {
		output_t *mon = xwayland_toplevel->view.node->output;
		desktop_t *desk = desktop_for_node(xwayland_toplevel->view.node);

		if (xwayland_toplevel->view.client)
			xwayland_toplevel->view.client->view = NULL;

		if (desk) {
			if (xwayland_toplevel->view.client &&
					xwayland_toplevel->view.client->state == STATE_FULLSCREEN) {
				desk->fullscreen_recreate_pending_window_id = xwayland_toplevel->xwayland_surface->window_id;
			}
			ipc_put_status(SUB_MASK_NODE_REMOVE, "node_remove[%s,%s,%u]\n",
				xwayland_toplevel->view.client &&
				xwayland_toplevel->view.client->app_id[0] ? xwayland_toplevel->view.client->app_id : "?",
				xwayland_toplevel->view.client &&
				xwayland_toplevel->view.client->title[0] ? xwayland_toplevel->view.client->title : "?",
				xwayland_toplevel->view.node->id);
			remove_node(desk, xwayland_toplevel->view.node);
			if (mon && desk) {
				arrange(mon, desk, true);

				// the closed toplevel was the last one standing
				if (desk->focus == xwayland_toplevel->view.node || !node_focusable(desk->focus))
					desk->focus = desktop_fallback_focus(desk, xwayland_toplevel->view.node);

				if (desk->focus != NULL)
					focus_node(mon, desk, desk->focus);
			}
		}

		xwayland_toplevel->view.node->destroying = true;
		xwayland_toplevel->view.node = NULL;
		xwayland_toplevel->view.client = NULL;
	}
}

static void handle_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	xwayland_toplevel_t *xwayland_toplevel = wl_container_of(listener, xwayland_toplevel, destroy);
	xwayland_toplevel_destroy(xwayland_toplevel);
}

static void handle_request_configure(struct wl_listener *listener, void *data) {
	xwayland_toplevel_t *xwayland_toplevel = wl_container_of(listener, xwayland_toplevel,
		request_configure);
	struct wlr_xwayland_surface_configure_event *ev = data;
	struct wlr_xwayland_surface *xsurface = xwayland_toplevel->xwayland_surface;

	if (!xsurface->surface || !xsurface->surface->mapped) {
		wlr_xwayland_surface_configure(xsurface, ev->x, ev->y, ev->width, ev->height);
		return;
	}

	// honor floating window request
	if (xwayland_toplevel->view.client && xwayland_toplevel->view.client->state == STATE_FLOATING &&
			!client_is_maximized(xwayland_toplevel->view.client)) {
		xwayland_toplevel_configure(xwayland_toplevel, ev->x, ev->y, ev->width, ev->height);
		if (xwayland_toplevel->view.node) {
			xwayland_toplevel->view.client->floating_rectangle.x = ev->x;
			xwayland_toplevel->view.client->floating_rectangle.y = ev->y;
			xwayland_toplevel->view.client->floating_rectangle.width = ev->width;
			xwayland_toplevel->view.client->floating_rectangle.height = ev->height;
		}
	} else {
		xwayland_sync_configure(xwayland_toplevel);
	}
}

static void handle_request_fullscreen(struct wl_listener *listener, void *data) {
	(void)data;
	xwayland_toplevel_t *xwayland_toplevel = wl_container_of(listener, xwayland_toplevel,
		request_fullscreen);
	struct wlr_xwayland_surface *xsurface = xwayland_toplevel->xwayland_surface;

	node_t *node = xwayland_toplevel->view.node;
	if (!node || !node->client || !node->output || !node->desktop)
		return;

	client_t *client = node->client;
	output_t *m = node->output;
	desktop_t *d = node->desktop;

	struct wlr_scene_tree *scene_tree = client_get_scene_tree(client);
	if (!scene_tree) {
		wlr_log(WLR_ERROR, "No scene tree for node %u", node->id);
		return;
	}

	if (settings.ignore_ewmh_fullscreen >= 1)
		return;

	bool requested_fullscreen = xsurface->fullscreen;
	if (requested_fullscreen == (client->state == STATE_FULLSCREEN))
		return;

	d->fullscreen_recreate_pending_window_id = 0;

	client_set_fullscreen(m, d, node, requested_fullscreen);
}

static void handle_request_maximize(struct wl_listener *listener, void *data) {
	(void)data;
	xwayland_toplevel_t *xwayland_toplevel = wl_container_of(listener, xwayland_toplevel,
		request_maximize);
	struct wlr_xwayland_surface *xsurface = xwayland_toplevel->xwayland_surface;

	node_t *node = xwayland_toplevel->view.node;
	if (node == NULL || node->client == NULL)
		return;

	if (node->client->state == STATE_FULLSCREEN)
		return;

	bool requested = xsurface->maximized_vert || xsurface->maximized_horz;
	if (requested == client_is_maximized(node->client))
		return;

	// when maximize is disabled, don't report the window as maximized either
	if (requested && !settings.enable_maximize)
		return;

	output_t *m = node->output;
	desktop_t *d = node->desktop;

	client_set_maximized(m, d, node, requested);

	wlr_xwayland_surface_set_maximized(xsurface, requested, requested);
	xwayland_sync_configure(xwayland_toplevel);
}

static void handle_request_minimize(struct wl_listener *listener, void *data) {
	xwayland_toplevel_t *xwayland_toplevel = wl_container_of(listener, xwayland_toplevel,
		request_minimize);
	struct wlr_xwayland_surface *xsurface = xwayland_toplevel->xwayland_surface;
	struct wlr_xwayland_minimize_event *ev = data;

	if (view_is_ready(&xwayland_toplevel->view) && xwayland_toplevel->view.node) {
		output_t *m = xwayland_toplevel->view.node->output;
		desktop_t *d = m != NULL ? m->desk : NULL;

		if (ev->minimize && xwayland_toplevel->fullscreen_at_map && xsurface->fullscreen) {
			wlr_xwayland_surface_set_minimized(xsurface, false);
			return;
		}

		// when minimize is disabled, don't report the window as hidden either
		if (ev->minimize && !settings.enable_minimize)
			return;

		client_set_minimized(m, d, xwayland_toplevel->view.node, ev->minimize);
	}

	wlr_xwayland_surface_set_minimized(xsurface, ev->minimize);
}

static void handle_request_activate(struct wl_listener *listener, void *data) {
	(void)data;
	xwayland_toplevel_t *xwayland_toplevel = wl_container_of(listener, xwayland_toplevel,
		request_activate);

	if (!xwayland_toplevel->xwayland_surface->surface ||
		!xwayland_toplevel->xwayland_surface->surface->mapped)
		return;

	if (xwayland_toplevel->view.node) {
		output_t *mon = xwayland_toplevel->view.node->output;
		if (mon) {
			desktop_t *d;
			wl_list_for_each(d, &mon->desk_list, link) {
				node_t *n = d->root;
				if (n == xwayland_toplevel->view.node || (n && (n->first_child == xwayland_toplevel->view.node ||
						n->second_child == xwayland_toplevel->view.node))) {
					activate_node(mon, d, xwayland_toplevel->view.node);
					break;
				}
			}
		}
	}
}

static void handle_request_move(struct wl_listener *listener, void *data) {
	(void)data;
	xwayland_toplevel_t *xwayland_toplevel = wl_container_of(listener, xwayland_toplevel,
		request_move);

	if (!xwayland_toplevel->xwayland_surface->surface ||
		!xwayland_toplevel->xwayland_surface->surface->mapped)
		return;

	if (xwayland_toplevel->view.client && xwayland_toplevel->view.client->state == STATE_FLOATING)
		view_begin_interactive(&xwayland_toplevel->view, CURSOR_MOVE, 0);
}

static void handle_request_resize(struct wl_listener *listener, void *data) {
	xwayland_toplevel_t *xwayland_toplevel = wl_container_of(listener, xwayland_toplevel,
		request_resize);
	struct wlr_xwayland_resize_event *ev = data;

	if (!xwayland_toplevel->xwayland_surface->surface ||
		!xwayland_toplevel->xwayland_surface->surface->mapped)
		return;

	if (xwayland_toplevel->view.client && xwayland_toplevel->view.client->state == STATE_FLOATING)
		view_begin_interactive(&xwayland_toplevel->view, CURSOR_RESIZE, ev->edges);
}

static void handle_set_title(struct wl_listener *listener, void *data) {
	(void)data;
	xwayland_toplevel_t *xwayland_toplevel = wl_container_of(listener, xwayland_toplevel, set_title);
	struct wlr_xwayland_surface *xsurface = xwayland_toplevel->xwayland_surface;

	view_set_title(&xwayland_toplevel->view, xsurface->title);
}

static void handle_set_class(struct wl_listener *listener, void *data) {
	(void)data;
	xwayland_toplevel_t *xwayland_toplevel = wl_container_of(listener, xwayland_toplevel, set_class);
	struct wlr_xwayland_surface *xsurface = xwayland_toplevel->xwayland_surface;

	view_set_app_id(&xwayland_toplevel->view, xsurface->class);
}

static void handle_set_hints(struct wl_listener *listener, void *data) {
	(void)data;
	xwayland_toplevel_t *xwayland_toplevel = wl_container_of(listener, xwayland_toplevel, set_hints);
	struct wlr_xwayland_surface *xsurface = xwayland_toplevel->xwayland_surface;

	if (xwayland_toplevel->view.client) {
		xwayland_toplevel->view.client->flags.urgent = xsurface->hints &&
			(xsurface->hints->flags & XCB_ICCCM_WM_HINT_X_URGENCY);
		ipc_put_status(SUB_MASK_REPORT, NULL);
		ipc_put_status(SUB_MASK_NODE_FLAG, "node_flag[%s,%s,%u,%c]\n",
			xwayland_toplevel->view.client->app_id[0] ? xwayland_toplevel->view.client->app_id : "?",
			xwayland_toplevel->view.client->title[0] ? xwayland_toplevel->view.client->title : "?",
			xwayland_toplevel->view.node->id, xwayland_toplevel->view.client->flags.urgent ? 'U' : 'u');
	}
}

static void handle_set_startup_id(struct wl_listener *listener, void *data) {
	(void)data;
	xwayland_toplevel_t *xwayland_toplevel = wl_container_of(listener, xwayland_toplevel,
		set_startup_id);
	struct wlr_xwayland_surface *xsurface = xwayland_toplevel->xwayland_surface;

	if (xsurface->startup_id == NULL)
		return;

	struct wlr_xdg_activation_token_v1 *token =
		wlr_xdg_activation_v1_find_token(server.xdg_activation_v1, xsurface->startup_id);
	if (token) {
		launcher_ctx_t *ctx = token->data;
		if (ctx) {
			wlr_log(WLR_DEBUG, "Startup_id '%s' matches launcher ctx", xsurface->startup_id);
			launcher_ctx_consume(ctx);
		}
	}
}

static void handle_set_window_type(struct wl_listener *listener, void *data) {
	(void)data;
	xwayland_toplevel_t *xwayland_toplevel = wl_container_of(listener, xwayland_toplevel,
		set_window_type);

	bool should_float = xwayland_toplevel_wants_floating(xwayland_toplevel);

	if (xwayland_toplevel->view.client) {
		client_state_t current_state = xwayland_toplevel->view.client->state;
		bool is_floating = (current_state == STATE_FLOATING);

		if (should_float && !is_floating)
			xwayland_toplevel->view.client->state = STATE_FLOATING;
		else if (!should_float && is_floating)
			xwayland_toplevel->view.client->state = STATE_TILED;
	}
}

static void handle_associate(struct wl_listener *listener, void *data) {
	(void)data;
	xwayland_toplevel_t *xwayland_toplevel = wl_container_of(listener, xwayland_toplevel, associate);
	struct wlr_xwayland_surface *xsurface = xwayland_toplevel->xwayland_surface;

	wl_signal_add(&xsurface->surface->events.map, &xwayland_toplevel->map);
	xwayland_toplevel->map.notify = handle_map;
	wl_signal_add(&xsurface->surface->events.unmap, &xwayland_toplevel->unmap);
	xwayland_toplevel->unmap.notify = handle_unmap;
}

static void handle_dissociate(struct wl_listener *listener, void *data) {
	(void)data;
	xwayland_toplevel_t *xwayland_toplevel = wl_container_of(listener, xwayland_toplevel, dissociate);
	wl_list_remove(&xwayland_toplevel->map.link);
	wl_list_remove(&xwayland_toplevel->unmap.link);
}

static void handle_override_redirect(struct wl_listener *listener, void *data) {
	(void)data;
	xwayland_toplevel_t *xwayland_toplevel = wl_container_of(listener, xwayland_toplevel,
		override_redirect);
	struct wlr_xwayland_surface *xsurface = xwayland_toplevel->xwayland_surface;

	bool associated = xsurface->surface != NULL;
	bool mapped = associated && xsurface->surface->mapped;

	if (mapped)
		handle_unmap(&xwayland_toplevel->unmap, NULL);
	if (associated)
		handle_dissociate(&xwayland_toplevel->dissociate, NULL);

	xwayland_toplevel_destroy(xwayland_toplevel);
	xsurface->data = NULL;

	xwayland_unmanaged_t *unmanaged = xwayland_unmanaged_create(xsurface);
	if (unmanaged && associated) {
		unmanaged_handle_associate(&unmanaged->associate, NULL);
		if (mapped)
			unmanaged_handle_map(&unmanaged->map, NULL);
	}
}

static xwayland_toplevel_t *xwayland_toplevel_create(struct wlr_xwayland_surface *xsurface) {
	xwayland_toplevel_t *xwayland_toplevel = calloc(1, sizeof(*xwayland_toplevel));
	if (!xwayland_toplevel)
		return NULL;

	xwayland_toplevel->xwayland_surface = xsurface;
	xwayland_toplevel->view.impl = &xwayland_view_impl;

	if (!view_init(&xwayland_toplevel->view, VIEW_XWAYLAND)) {
		wlr_log(WLR_ERROR, "Failed to initialise view for xwayland surface %p", (void *)xsurface);
		free(xwayland_toplevel);
		return NULL;
	}

	xwayland_toplevel->view.scene_tree->node.data = &xwayland_toplevel->view;
	wlr_scene_node_set_enabled(&xwayland_toplevel->view.scene_tree->node, false);

	create_borders(xwayland_toplevel->view.scene_tree, &xwayland_toplevel->view.border_tree,
		xwayland_toplevel->view.border_rects);

	wl_signal_add(&xsurface->events.destroy, &xwayland_toplevel->destroy);
	xwayland_toplevel->destroy.notify = handle_destroy;

	wl_signal_add(&xsurface->events.request_configure, &xwayland_toplevel->request_configure);
	xwayland_toplevel->request_configure.notify = handle_request_configure;

	wl_signal_add(&xsurface->events.request_fullscreen, &xwayland_toplevel->request_fullscreen);
	xwayland_toplevel->request_fullscreen.notify = handle_request_fullscreen;

	wl_signal_add(&xsurface->events.request_minimize, &xwayland_toplevel->request_minimize);
	xwayland_toplevel->request_minimize.notify = handle_request_minimize;

	wl_signal_add(&xsurface->events.request_maximize, &xwayland_toplevel->request_maximize);
	xwayland_toplevel->request_maximize.notify = handle_request_maximize;

	wl_signal_add(&xsurface->events.request_activate, &xwayland_toplevel->request_activate);
	xwayland_toplevel->request_activate.notify = handle_request_activate;

	wl_signal_add(&xsurface->events.request_move, &xwayland_toplevel->request_move);
	xwayland_toplevel->request_move.notify = handle_request_move;

	wl_signal_add(&xsurface->events.request_resize, &xwayland_toplevel->request_resize);
	xwayland_toplevel->request_resize.notify = handle_request_resize;

	wl_signal_add(&xsurface->events.set_title, &xwayland_toplevel->set_title);
	xwayland_toplevel->set_title.notify = handle_set_title;

	wl_signal_add(&xsurface->events.set_class, &xwayland_toplevel->set_class);
	xwayland_toplevel->set_class.notify = handle_set_class;

	wl_signal_add(&xsurface->events.set_hints, &xwayland_toplevel->set_hints);
	xwayland_toplevel->set_hints.notify = handle_set_hints;

	wl_signal_add(&xsurface->events.set_window_type, &xwayland_toplevel->set_window_type);
	xwayland_toplevel->set_window_type.notify = handle_set_window_type;

	wl_signal_add(&xsurface->events.set_startup_id, &xwayland_toplevel->set_startup_id);
	xwayland_toplevel->set_startup_id.notify = handle_set_startup_id;

	wl_signal_add(&xsurface->events.associate, &xwayland_toplevel->associate);
	xwayland_toplevel->associate.notify = handle_associate;

	wl_signal_add(&xsurface->events.dissociate, &xwayland_toplevel->dissociate);
	xwayland_toplevel->dissociate.notify = handle_dissociate;

	wl_signal_add(&xsurface->events.set_override_redirect, &xwayland_toplevel->override_redirect);
	xwayland_toplevel->override_redirect.notify = handle_override_redirect;

	xsurface->data = xwayland_toplevel;

	wl_list_insert(&server.views, &xwayland_toplevel->view.link);

	return xwayland_toplevel;
}

static void xwayland_toplevel_destroy(xwayland_toplevel_t *xwayland_toplevel) {
	if (xwayland_toplevel->view.mapped)
		handle_unmap(&xwayland_toplevel->unmap, NULL);

	if (server.last_focused_xwayland_view == &xwayland_toplevel->view) {
		uint32_t window_id = xwayland_toplevel->xwayland_surface ?
			xwayland_toplevel->xwayland_surface->window_id : 0;
		wlr_log(WLR_INFO, "Clearing last_focused_xwayland_view (window %u being destroyed)", window_id);
		server.last_focused_xwayland_view = NULL;
	}

	client_t *client = xwayland_toplevel->view.client;
	if (client)
		render_unfocused_client_remove(client);

	wl_list_remove(&xwayland_toplevel->destroy.link);
	wl_list_remove(&xwayland_toplevel->request_configure.link);
	wl_list_remove(&xwayland_toplevel->request_fullscreen.link);
	wl_list_remove(&xwayland_toplevel->request_minimize.link);
	wl_list_remove(&xwayland_toplevel->request_maximize.link);
	wl_list_remove(&xwayland_toplevel->request_activate.link);
	wl_list_remove(&xwayland_toplevel->request_move.link);
	wl_list_remove(&xwayland_toplevel->request_resize.link);
	wl_list_remove(&xwayland_toplevel->set_title.link);
	wl_list_remove(&xwayland_toplevel->set_class.link);
	wl_list_remove(&xwayland_toplevel->set_hints.link);
	wl_list_remove(&xwayland_toplevel->set_window_type.link);
	wl_list_remove(&xwayland_toplevel->set_startup_id.link);
	wl_list_remove(&xwayland_toplevel->associate.link);
	wl_list_remove(&xwayland_toplevel->dissociate.link);
	wl_list_remove(&xwayland_toplevel->override_redirect.link);

	view_destroy(&xwayland_toplevel->view);

	xwayland_toplevel->xwayland_surface = NULL;
	free(xwayland_toplevel);
}

static void handle_xwayland_surface(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_xwayland_surface *xsurface = data;

	if (xsurface->override_redirect) {
		xwayland_unmanaged_create(xsurface);
		return;
	}

	xwayland_toplevel_create(xsurface);
}

static void handle_xwayland_ready(struct wl_listener *listener, void *data) {
	(void)listener;
	(void)data;
	xwayland_t *xwayland = &server.xwayland;

	xcb_connection_t *xcb_conn = xcb_connect(NULL, NULL);
	int err = xcb_connection_has_error(xcb_conn);
	if (err)
		return;

	// intern atoms (microwave reference???)
	xcb_intern_atom_cookie_t cookies[ATOM_LAST];
	for (size_t i = 0; i < ATOM_LAST; i++)
		cookies[i] = xcb_intern_atom(xcb_conn, 0, strlen(atom_map[i]), atom_map[i]);

	for (size_t i = 0; i < ATOM_LAST; i++) {
		xcb_generic_error_t *error = NULL;
		xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(xcb_conn, cookies[i], &error);

		if (reply != NULL && error == NULL)
			xwayland->atoms[i] = reply->atom;

		free(reply);
		free(error);
	}

	xcb_disconnect(xcb_conn);

	seat_t *s = seat_default();
	wlr_xwayland_set_seat(xwayland->wlr_xwayland, s ? s->wlr_seat : NULL);
}

void xwayland_init(void) {
	ONCE();
	server.xwayland.wlr_xwayland = wlr_xwayland_create(server.wl_display, server.compositor, true);
	if (server.xwayland.wlr_xwayland) {
		server.xwayland.xcursor_manager = server.cursor_mgr;

		server.xwayland_surface.notify = handle_xwayland_surface;
		wl_signal_add(&server.xwayland.wlr_xwayland->events.new_surface, &server.xwayland_surface);

		server.xwayland_ready.notify = handle_xwayland_ready;
		wl_signal_add(&server.xwayland.wlr_xwayland->events.ready, &server.xwayland_ready);

		setenv("DISPLAY", server.xwayland.wlr_xwayland->display_name, true);
	}
}

void xwayland_fini(void) {
	ONCE();
	if (server.xwayland.wlr_xwayland) {
		wl_list_remove(&server.xwayland_ready.link);
		wl_list_remove(&server.xwayland_surface.link);
		wlr_xwayland_destroy(server.xwayland.wlr_xwayland);
		server.xwayland.wlr_xwayland = NULL;
	}
}
