#include "animation.h"
#include "borders.h"
#include "client.h"
#include "effects/effects.h"
#include "input/input_method.h"
#include "input/seat.h"
#include "input/tablet.h"
#include "ipc/ipc.h"
#include "layout/tree.h"
#include "output/output.h"
#include "protocol/copy_capture.h"
#include "protocol/workspace.h"
#include "protocol/xdg_toplevel.h"
#include "protocol/xwayland.h"
#include "server.h"
#include "surface.h"
#include "tabs.h"
#include "tree.h"
#include "view.h"
#include <pixman.h>
#include <stdlib.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

xdg_toplevel_t *view_to_xdg(view_t *view) {
	if (view == NULL || view->type != VIEW_XDG)
		return NULL;
	xdg_toplevel_t *xdg;
	return wl_container_of(view, xdg, view);
}

xwayland_toplevel_t *view_to_xwayland(view_t *view) {
	if (view == NULL || view->type != VIEW_XWAYLAND)
		return NULL;
	xwayland_toplevel_t *xwayland;
	return wl_container_of(view, xwayland, view);
}

struct wlr_surface *view_wlr_surface(view_t *view) {
	if (!view)
		return NULL;

	xdg_toplevel_t *xdg = view_to_xdg(view);
	if (xdg)
		return xdg->xdg_toplevel ? xdg->xdg_toplevel->base->surface : NULL;

	xwayland_toplevel_t *xwayland = view_to_xwayland(view);
	if (xwayland)
		return xwayland->xwayland_surface ? xwayland->xwayland_surface->surface : NULL;

	return NULL;
}

bool view_is_ready(view_t *view) {
	if (!view || !view->mapped)
		return false;
	struct wlr_surface *surface = view_wlr_surface(view);
	return surface && surface->mapped;
}

view_t *view_from_wlr_surface(struct wlr_surface *surface) {
	if (!surface)
		return NULL;

	struct wlr_xdg_surface *xdg_surface = wlr_xdg_surface_try_from_wlr_surface(surface);
	if (xdg_surface) {
		if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL)
			return NULL;
		xdg_toplevel_t *toplevel = xdg_surface->data;
		return toplevel ? &toplevel->view : NULL;
	}

	struct wlr_xwayland_surface *xsurface = wlr_xwayland_surface_try_from_wlr_surface(surface);
	if (xsurface) {
		xwayland_toplevel_t *xwayland = xsurface->data;
		return xwayland ? &xwayland->view : NULL;
	}

	// a client may render its window into a subsurface, in which case the view
	// that owns it is the one owning the subsurface's parent surface
	struct wlr_subsurface *subsurface = wlr_subsurface_try_from_wlr_surface(surface);
	if (subsurface)
		return view_from_wlr_surface(subsurface->parent);

	return NULL;
}

static void handle_foreign_activate_request(struct wl_listener *listener, void *data);
static void handle_foreign_fullscreen_request(struct wl_listener *listener, void *data);
static void handle_foreign_close_request(struct wl_listener *listener, void *data);
static void handle_foreign_destroy(struct wl_listener *listener, void *data);

void client_update_ext_foreign_toplevel(client_t *c) {
	if (!c)
		return;
	view_t *view = c->view;
	if (!view || !view->ext_foreign_toplevel)
		return;

	struct wlr_ext_foreign_toplevel_handle_v1_state state = {0};

	if (c->title[0] != '\0')
		state.title = c->title;
	if (c->app_id[0] != '\0')
		state.app_id = c->app_id;

	wlr_ext_foreign_toplevel_handle_v1_update_state(view->ext_foreign_toplevel, &state);
}

void client_update_foreign_toplevel_state(client_t *c) {
	if (!c)
		return;
	view_t *view = c->view;
	if (!view || !view->foreign_toplevel)
		return;

	node_t *n = view->node;
	bool maximized = client_reports_maximized(c, n ? n->desktop : NULL);
	bool fullscreen = (c->state == STATE_FULLSCREEN);
	bool minimized = c->flags.minimized;

	wlr_foreign_toplevel_handle_v1_set_fullscreen(view->foreign_toplevel, fullscreen);
	wlr_foreign_toplevel_handle_v1_set_maximized(view->foreign_toplevel, maximized);
	wlr_foreign_toplevel_handle_v1_set_minimized(view->foreign_toplevel, minimized);
}

void client_connect_foreign_toplevel(client_t *c, struct wlr_foreign_toplevel_handle_v1 *ft) {
	if (c == NULL || ft == NULL)
		return;

	c->foreign.activate.notify = handle_foreign_activate_request;
	wl_signal_add(&ft->events.request_activate, &c->foreign.activate);
	c->foreign.fullscreen.notify = handle_foreign_fullscreen_request;
	wl_signal_add(&ft->events.request_fullscreen, &c->foreign.fullscreen);
	c->foreign.close.notify = handle_foreign_close_request;
	wl_signal_add(&ft->events.request_close, &c->foreign.close);
	c->foreign.destroy.notify = handle_foreign_destroy;
	wl_signal_add(&ft->events.destroy, &c->foreign.destroy);
}

void client_disconnect_foreign_toplevel(client_t *c) {
	if (c == NULL)
		return;
	wl_list_remove(&c->foreign.activate.link);
	wl_list_remove(&c->foreign.fullscreen.link);
	wl_list_remove(&c->foreign.close.link);
	wl_list_remove(&c->foreign.destroy.link);
}

void client_send_activated(client_t *c, bool activated) {
	if (c == NULL)
		return;

	view_t *view = c->view;
	if (!view)
		return;

	if (view->foreign_toplevel)
		wlr_foreign_toplevel_handle_v1_set_activated(view->foreign_toplevel, activated);

	view->impl->set_activated(view, activated);
}

void view_set_activated(view_t *view, bool activated) {
	if (view == NULL)
		return;

	// never steal keyboard focus to a window on a non-visible desktop
	if (activated && view->node && view->node->desktop && view->node->output &&
			view->node->desktop != view->node->output->desk) {
		view->node->desktop->focus = view->node;
		return;
	}

	struct wlr_seat *seat = server.seat;
	struct wlr_surface *surface = view_wlr_surface(view);
	if (surface == NULL)
		return;

	struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
	if (prev_surface == surface)
		return;

	if (view->foreign_toplevel)
		wlr_foreign_toplevel_handle_v1_set_activated(view->foreign_toplevel, activated);

	view->impl->set_activated(view, activated);

	if (activated && view->scene_tree)
		wlr_scene_node_raise_to_top(&view->scene_tree->node);

	if (!activated)
		return;

	if (seat->keyboard_state.keyboard != NULL)
		wlr_seat_keyboard_notify_enter(seat, surface, seat->keyboard_state.keyboard->keycodes,
			seat->keyboard_state.keyboard->num_keycodes, &seat->keyboard_state.keyboard->modifiers);

	// update input method focus
	seat_t *s = seat_default();
	if (s && s->input_method_relay)
		input_method_relay_set_focus(s->input_method_relay, surface);

	// update tablet pad focus
	if (s)
		tablet_pads_set_focus(s, surface);

	if (view->node && view->node->output)
		server.focused_output = view->node->output;

	// update borders
	if (view->node) {
		output_t *m = view->node->output;
		desktop_t *d = m ? m->desk : NULL;
		if (d && d->root != NULL) {
			FOR_EACH_LEAF(node, d->root) {
				if (node->client == NULL)
					continue;

				update_border_colors(node->client);
			}
		}
	}
}

void view_close(view_t *view) {
	if (view)
		view->impl->close(view);
}

void view_configure(view_t *view, struct wlr_box rect) {
	if (view == NULL || view->impl == NULL || view->impl->configure == NULL)
		return;

	view->impl->configure(view, rect);
}

void view_set_title(view_t *view, const char *title) {
	if (view == NULL || view->client == NULL)
		return;

	client_t *c = view->client;

	if (title) {
		client_set_title(c, title);
		wlr_log(WLR_DEBUG, "View title changed: %s", title);

		if (view->foreign_toplevel)
			wlr_foreign_toplevel_handle_v1_set_title(view->foreign_toplevel, title);
	}

	client_update_ext_foreign_toplevel(c);
	tabs_update_label_for_leaf(view->node);

	ipc_put_status(SUB_MASK_NODE_CHANGE, "node_change[%s,%s,%u,title]\n",
		c->app_id[0] ? c->app_id : "?", title ? title : "?", view->node ? view->node->id : 0);
}

void view_set_app_id(view_t *view, const char *app_id) {
	if (view == NULL || view->client == NULL)
		return;

	client_t *c = view->client;

	if (app_id) {
		client_set_app_id(c, app_id);
		wlr_log(WLR_DEBUG, "View app_id changed: %s", app_id);

		if (view->foreign_toplevel)
			wlr_foreign_toplevel_handle_v1_set_app_id(view->foreign_toplevel, app_id);
	}

	client_update_ext_foreign_toplevel(c);
	tabs_update_label_for_leaf(view->node);

	ipc_put_status(SUB_MASK_NODE_CHANGE, "node_change[%s,%s,%u,app_id]\n", app_id ? app_id : "?",
		c->title[0] ? c->title : "?", view->node ? view->node->id : 0);
}

void view_create_foreign_toplevels(view_t *view, const char *app_id, const char *title) {
	if (view == NULL)
		return;

	struct wlr_ext_foreign_toplevel_handle_v1_state ext_state = {
		.app_id = app_id,
		.title = title,
	};

	view->ext_foreign_toplevel = wlr_ext_foreign_toplevel_handle_v1_create(server.foreign_toplevel_list,
		&ext_state);
	if (view->ext_foreign_toplevel) {
		view->ext_foreign_toplevel->data = view;
		view->foreign_identifier = view->ext_foreign_toplevel->identifier;
	}

	view->foreign_toplevel = wlr_foreign_toplevel_handle_v1_create(server.foreign_toplevel_manager);

	if (view->foreign_toplevel && view->client) {
		client_connect_foreign_toplevel(view->client, view->foreign_toplevel);

		if (app_id)
			wlr_foreign_toplevel_handle_v1_set_app_id(view->foreign_toplevel, app_id);
	}
}

void view_destroy_foreign_toplevels(view_t *view) {
	if (view == NULL)
		return;

	if (view->ext_foreign_toplevel) {
		wlr_ext_foreign_toplevel_handle_v1_destroy(view->ext_foreign_toplevel);
		view->ext_foreign_toplevel = NULL;
	}

	view->foreign_identifier = NULL;

	if (view->foreign_toplevel) {
		wlr_foreign_toplevel_handle_v1_destroy(view->foreign_toplevel);
		view->foreign_toplevel = NULL;
	}
}

static void handle_foreign_activate_request(struct wl_listener *listener, void *data) {
	(void)data;
	client_t *c = wl_container_of(listener, c, foreign.activate);

	node_t *n = c->view ? c->view->node : NULL;
	if (n == NULL)
		return;

	output_t *m = n->output;
	if (!m)
		return;

	desktop_t *toplevel_desk = n->desktop;
	if (!toplevel_desk)
		return;

	if (m->desk != toplevel_desk)
		workspace_switch_to_desktop(toplevel_desk->name);

	// activating a minimized window brings it back
	if (c->flags.minimized)
		client_set_minimized(m, toplevel_desk, n, false);

	if (toplevel_desk->focus == n)
		return;

	node_t *prev = toplevel_desk->focus;
	if (prev != NULL && prev != n)
		client_send_activated(prev->client, false);

	activate_node(m, toplevel_desk, n);
}

static void handle_foreign_fullscreen_request(struct wl_listener *listener, void *data) {
	struct wlr_foreign_toplevel_handle_v1_fullscreen_event *event = data;
	client_t *c = wl_container_of(listener, c, foreign.fullscreen);

	node_t *n = c->view ? c->view->node : NULL;
	if (n == NULL)
		return;

	output_t *m = n->output;
	desktop_t *d = m ? m->desk : NULL;

	client_set_fullscreen(m, d, n, event->fullscreen);
}

static void handle_foreign_close_request(struct wl_listener *listener, void *data) {
	(void)data;
	client_t *c = wl_container_of(listener, c, foreign.close);
	view_close(c->view);
}

static void handle_foreign_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	client_t *c = wl_container_of(listener, c, foreign.destroy);
	client_disconnect_foreign_toplevel(c);
}

bool view_output_handler_point_accepts_input(struct wlr_scene_buffer *buffer, double *x,
		double *y) {
	(void)buffer;
	(void)x;
	(void)y;
	return false;
}

static void view_disconnect_outputs_update(view_t *view) {
	if (view == NULL)
		return;
	wl_list_remove(&view->outputs_update.link);
	wl_list_init(&view->outputs_update.link);
}

void view_handle_outputs_update(struct wl_listener *listener, void *data) {
	view_t *view = wl_container_of(listener, view, outputs_update);
	struct wlr_scene_outputs_update_event *event = data;

	struct wlr_foreign_toplevel_handle_v1 *ft = view->foreign_toplevel;
	if (ft) {
		struct wlr_foreign_toplevel_handle_v1_output *toplevel_output, *tmp;
		wl_list_for_each_safe(toplevel_output, tmp, &ft->outputs, link) {
			bool active = false;
			for (size_t i = 0; i < event->size; i++) {
				struct wlr_scene_output *scene_output = event->active[i];
				if (scene_output->output == toplevel_output->output) {
					active = true;
					break;
				}
			}

			if (!active) {
				wlr_log(WLR_DEBUG, "Toplevel output leave: %s", toplevel_output->output->name);
				wlr_foreign_toplevel_handle_v1_output_leave(ft, toplevel_output->output);
			}
		}

		for (size_t i = 0; i < event->size; i++) {
			struct wlr_scene_output *scene_output = event->active[i];
			wlr_log(WLR_DEBUG, "Toplevel output enter: %s", scene_output->output->name);
			wlr_foreign_toplevel_handle_v1_output_enter(ft, scene_output->output);
		}
	}
}

void view_resolve_content_layout(view_t *view, struct wlr_box container,
		struct wlr_box *content_offset, struct wlr_box *border_size) {
	int container_w = container.width > 0 ? container.width : 0;
	int container_h = container.height > 0 ? container.height : 0;
	int off_x = 0, off_y = 0;
	int border_w = container_w, border_h = container_h;

	if (view && view->geometry.width > 0 && view->geometry.height > 0) {
		int geo_w = (int)view->geometry.width;
		int geo_h = (int)view->geometry.height;

		if (geo_w < container_w) {
			off_x = (container_w - geo_w) / 2;
			border_w = geo_w;
		}
		if (geo_h < container_h) {
			off_y = (container_h - geo_h) / 2;
			border_h = geo_h;
		}
	}

	if (content_offset)
		*content_offset = (struct wlr_box){
			.x = off_x,
			.y = off_y,
			.width = 0,
			.height = 0
		};
	if (border_size)
		*border_size = (struct wlr_box){
			.x = 0,
			.y = 0,
			.width = border_w,
			.height = border_h
		};
}

void view_center_and_clip_surface(view_t *view) {
	if (!view || !view->content_tree || !view->client)
		return;

	client_t *c = view->client;
	bool floating = (c->state == STATE_FLOATING);
	bool fullscreen = (c->state == STATE_FULLSCREEN);
	bool tiled = IS_TILED(c);
	struct wlr_box *container_rect = NULL;
	bool clip_to_geometry = true;

	if (floating)
		container_rect = &c->floating_rectangle;
	else if (fullscreen) {
		output_t *m = view->node->output;
		container_rect = m ? &m->rectangle : &c->tiled_rectangle;
	} else if (tiled)
		container_rect = &c->tiled_rectangle;

	int bw = effective_border_width(view->node->desktop);
	struct wlr_box content_offset = {0};
	struct wlr_box border_size = {0};
	if (container_rect)
		view_resolve_content_layout(view, *container_rect, &content_offset, &border_size);

	int x = content_offset.x;
	int y = content_offset.y;

	if (container_rect && (floating || fullscreen) && (x != 0 || y != 0)) {
		wlr_log(WLR_DEBUG, "Centering surface: %dx%d at offset (%d,%d) in container %dx%d",
			view->geometry.width, view->geometry.height, x, y, container_rect->width,
			container_rect->height);
		clip_to_geometry = false;
	}

	wlr_scene_node_set_position(&view->content_tree->node, x, y);

	// when a tiled or floating surface is smaller than its container, the
	// border wraps the actual surface instead of the full allocated space
	if ((tiled || floating) && view->border_tree) {
		if (bw > 0) {
			update_borders(view->border_tree, view->border_rects, border_size, (unsigned int)bw, x, y);
			update_border_colors(c);
			if (view->rounded && view->rounded->border_shader_node && (c->border_radius > 0.0f ||
					view->rounded->gradient_count >= 2)) {
				rounded_mark_border_size(view->rounded, border_size.width, border_size.height, bw,
					view->node && view->node->output ? view->node->output->wlr_output->scale : 1.0f);
				if (border_size.width + 2 * bw > 0)
					wlr_scene_buffer_set_dest_size(view->rounded->border_shader_node, border_size.width + 2 * bw,
						border_size.height + 2 * bw);
			}
		} else if (view->border_tree->node.enabled) {
			wlr_scene_node_set_enabled(&view->border_tree->node, false);
		}
	}

	if (!wl_list_empty(&view->content_tree->children) && container_rect) {
		int clip_w = container_rect->width;
		int clip_h = container_rect->height;
		if (tiled && view->geometry.width > 0 && view->geometry.height > 0) {
			if ((int)view->geometry.width < container_rect->width)
				clip_w = view->geometry.width;
			else if ((int)view->geometry.width > container_rect->width)
				clip_w = container_rect->width;
			if ((int)view->geometry.height < container_rect->height)
				clip_h = view->geometry.height;
			else if ((int)view->geometry.height > container_rect->height)
				clip_h = container_rect->height;
		}

		struct wlr_box clip = {
			.x = view->geometry.x,
			.y = view->geometry.y,
			.width = clip_w,
			.height = clip_h
		};

		if (clip_to_geometry) {
			wlr_scene_subsurface_tree_set_clip(&view->content_tree->node, &clip);
		} else {
			wlr_scene_subsurface_tree_set_clip(&view->content_tree->node, NULL);
		}
	}

	if (view->shadow)
		view->shadow->shadow_geometry_dirty = true;
}

bool view_get_surface_offset(view_t *view, int *ox, int *oy) {
	if (!view || !view->scene_tree || !view->content_tree)
		return false;

	int x = view->content_tree->node.x - view->geometry.x;
	int y = view->content_tree->node.y - view->geometry.y;
	if (ox)
		*ox = x;
	if (oy)
		*oy = y;
	return true;
}

static struct wlr_fbox clamp_scene_buffer_source_box(struct wlr_scene_buffer *buffer,
		struct wlr_fbox box) {
	if (!buffer || !buffer->buffer)
		return (struct wlr_fbox){0};

	if (box.x < 0.0f) {
		box.width += box.x;
		box.x = 0.0f;
	}
	if (box.y < 0.0f) {
		box.height += box.y;
		box.y = 0.0f;
	}

	float max_w = (float)buffer->buffer->width;
	float max_h = (float)buffer->buffer->height;

	if (max_w < 1.0f || max_h < 1.0f)
		return (struct wlr_fbox){
			0,
			0,
			1,
			1
		};

	if (box.x > max_w)
		box.x = max_w;
	if (box.y > max_h)
		box.y = max_h;
	if (box.x + box.width > max_w)
		box.width = max_w - box.x;
	if (box.y + box.height > max_h)
		box.height = max_h - box.y;

	if (box.width < 1.0f)
		box.width = 1.0f;
	if (box.height < 1.0f)
		box.height = 1.0f;
	if (box.x + box.width > max_w)
		box.x = max_w - box.width;
	if (box.y + box.height > max_h)
		box.y = max_h - box.height;
	if (box.x < 0.0f)
		box.x = 0.0f;
	if (box.y < 0.0f)
		box.y = 0.0f;

	return box;
}

static int buffer_copy_count = 0;

static void save_buffer_iterator(struct wlr_scene_buffer *buffer, int sx, int sy, void *data) {
	struct wlr_scene_tree *tree = data;

	buffer_copy_count++;
	wlr_log(WLR_DEBUG, "Buffer save: buffer=%p, sx=%d, sy=%d", (void *)buffer, sx, sy);

	// ignore buffers with no content
	if (!buffer->buffer) {
		wlr_log(WLR_DEBUG, "Skipping buffer with no content");
		return;
	}

	struct wlr_scene_buffer *sbuf = wlr_scene_buffer_create(tree, NULL);
	if (!sbuf) {
		wlr_log(WLR_ERROR, "Could not allocate a scene buffer when saving surface %p", (void *)tree);
		return;
	}

	wlr_scene_buffer_set_dest_size(sbuf, buffer->dst_width, buffer->dst_height);
	wlr_scene_buffer_set_opaque_region(sbuf, &buffer->opaque_region);
	struct wlr_fbox src_box = clamp_scene_buffer_source_box(buffer, buffer->src_box);
	wlr_scene_buffer_set_source_box(sbuf, &src_box);
	wlr_scene_node_set_position(&sbuf->node, sx, sy);
	wlr_scene_buffer_set_transform(sbuf, buffer->transform);
	wlr_scene_buffer_set_buffer(sbuf, buffer->buffer);

	wlr_log(WLR_DEBUG, "Successfully copied buffer %dx%d at (%d,%d)", buffer->dst_width,
		buffer->dst_height, sx, sy);
}

void view_save_buffer(view_t *view) {
	if (!view || !view->scene_tree || !view->content_tree)
		return;

	// removed saved buffer
	if (view->saved_surface_tree) {
		wlr_log(WLR_DEBUG, "Removing existing saved buffer before saving new one");
		view_remove_saved_buffer(view);
	}

	view->saved_surface_tree = wlr_scene_tree_create(view->scene_tree);
	if (!view->saved_surface_tree) {
		wlr_log(WLR_ERROR, "Could not allocate a scene tree node when saving surface %p", (void *)view);
		return;
	}

	wlr_scene_node_set_enabled(&view->saved_surface_tree->node, false);

	// copy scene buffers
	buffer_copy_count = 0;
	wlr_log(WLR_DEBUG, "Starting buffer iteration for content_tree=%p", (void *)view->content_tree);

	wlr_scene_node_for_each_buffer(&view->content_tree->node, save_buffer_iterator,
		view->saved_surface_tree);

	wlr_log(WLR_DEBUG, "Buffer iteration complete, copied %d buffers", buffer_copy_count);

	bool has_children = !wl_list_empty(&view->saved_surface_tree->children);
	wlr_log(WLR_DEBUG, "After iteration: saved_surface_tree has_children=%d", has_children);

	if (!has_children) {
		// cleanup
		wlr_scene_node_destroy(&view->saved_surface_tree->node);
		view->saved_surface_tree = NULL;
		wlr_log(WLR_DEBUG, "No buffers to save for toplevel - destroyed saved tree");
	} else {
		wlr_scene_node_set_enabled(&view->content_tree->node, false);
		wlr_scene_node_set_enabled(&view->saved_surface_tree->node, true);
		wlr_log(WLR_DEBUG, "Saved buffer for toplevel - swapped content_tree for saved_surface_tree");
	}
}

void view_remove_saved_buffer(view_t *view) {
	if (!view || !view->saved_surface_tree)
		return;
	wlr_log(WLR_DEBUG, "Removing saved buffer for toplevel");

	wlr_scene_node_destroy(&view->saved_surface_tree->node);
	view->saved_surface_tree = NULL;

	if (view->content_tree)
		wlr_scene_node_set_enabled(&view->content_tree->node, true);
}

void view_freeze_sibling_buffers(desktop_t *d, node_t *n) {
	if (!d || !d->root)
		return;
	if (!settings.enable_animations)
		return;
	if (n->client && n->client->flags.anim_disabled)
		return;

	node_t *root = d->root;
	node_t *leaf = first_extrema(root);
	while (leaf) {
		view_t *view = leaf->client ? leaf->client->view : NULL;
		if (leaf != n && leaf->client && leaf->client->flags.shown && view && !view->saved_surface_tree) {
			view_save_buffer(view);
			wlr_log(WLR_DEBUG, "Froze buffer for sibling node %u", leaf->id);
		}
		leaf = next_leaf(leaf, root);
	}
}

void view_send_frame_done_iterator(struct wlr_scene_buffer *scene_buffer, int x, int y,
		void *data) {
	(void)x;
	(void)y;
	struct timespec *when = data;
	struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
	if (scene_surface == NULL)
		return;

	wlr_surface_send_frame_done(scene_surface->surface, when);
}

void view_send_frame_done(view_t *view) {
	if (!view || !view->content_tree)
		return;

	struct timespec when;
	clock_gettime(CLOCK_MONOTONIC, &when);

	struct wlr_scene_node *node;
	wl_list_for_each(node, &view->content_tree->children, link)
		wlr_scene_node_for_each_buffer(node, view_send_frame_done_iterator, &when);
}

bool view_init(view_t *view, view_type_t type) {
	view->type = type;
	wl_list_init(&view->link);

	// create parent scene tree container
	view->scene_tree = wlr_scene_tree_create(server.tile_tree);
	if (!view->scene_tree) {
		wlr_log(WLR_ERROR, "Failed to create scene tree for view %p", (void *)view);
		return false;
	}

	// keep scene invisible until arrange_node_geometry positions and enables it
	wlr_scene_node_set_enabled(&view->scene_tree->node, false);

	// create content tree as child
	view->content_tree = wlr_scene_tree_create(view->scene_tree);
	if (!view->content_tree) {
		wlr_log(WLR_ERROR, "Failed to create content tree for view %p", (void *)view);
		wlr_scene_node_destroy(&view->scene_tree->node);
		view->scene_tree = NULL;
		return false;
	}

	for (int i = 0; i < 4; i++)
		view->border_rects[i] = NULL;

	// create image capture
	view->image_capture = wlr_scene_create();
	view->image_capture_tree = wlr_scene_tree_create(&view->image_capture->tree);

	view->output_handler = wlr_scene_buffer_create(view->scene_tree, NULL);
	if (!view->output_handler) {
		wlr_log(WLR_ERROR, "Failed to create output handler for view %p", (void *)view);
	} else {
		view->output_handler->point_accepts_input = view_output_handler_point_accepts_input;
		view->outputs_update.notify = view_handle_outputs_update;
		wl_signal_add(&view->output_handler->events.outputs_update, &view->outputs_update);
	}

	return true;
}

void view_destroy(view_t *view) {
	if (view == NULL)
		return;

	// stop any playing animation and detach the client
	animation_cancel_view(view);
	if (view->client) {
		if (view->node)
			animation_cancel_node(view->node);
		view->client->view = NULL;
	}
	view->node = NULL;
	view->client = NULL;

	view_destroy_foreign_toplevels(view);

	if (view->capture_renderer) {
		capture_renderer_destroy(view->capture_renderer);
		view->capture_renderer = NULL;
	}

	if (view->image_capture != NULL) {
		wlr_scene_node_destroy(&view->image_capture->tree.node);
		view->image_capture = NULL;
		view->image_capture_source = NULL;
	}

	view_remove_saved_buffer(view);
	view_free_effects(view);
	destroy_borders(&view->border_tree, view->border_rects);
	view_disconnect_outputs_update(view);

	if (view->output_handler) {
		wlr_scene_node_destroy(&view->output_handler->node);
		view->output_handler = NULL;
	}

	if (view->scene_tree) {
		wlr_scene_node_destroy(&view->scene_tree->node);
		view->scene_tree = NULL;
		view->content_tree = NULL;
	}

	wl_list_remove(&view->link);
	wl_list_init(&view->link);
}

void view_free_effects(view_t *view) {
	if (view->blur) {
		blur_destroy_nodes(view->blur);

		if (view->blur->blur_buf)
			effects_destroy_buffer(&view->blur->blur_buf, view->blur->blur_native);

		if (view->blur->mica_node) {
			wlr_scene_node_destroy(&view->blur->mica_node->node);
			view->blur->mica_node = NULL;
		}

		if (view->blur->acrylic_node) {
			wlr_scene_node_destroy(&view->blur->acrylic_node->node);
			view->blur->acrylic_node = NULL;
		}

		if (view->blur->acrylic_buf)
			effects_destroy_buffer(&view->blur->acrylic_buf, view->blur->acrylic_native);

		pixman_region32_fini(&view->blur->blur_region);
		free(view->blur);
		view->blur = NULL;
	}

	if (view->rounded) {
		if (view->rounded->border_shader_buf) {
			effects_destroy_buffer(&view->rounded->border_shader_buf, view->rounded->border_shader_native);
			view->rounded->border_shader_buf_w = 0;
			view->rounded->border_shader_buf_h = 0;
		}

		if (view->rounded->border_shader_node) {
			wlr_scene_node_destroy(&view->rounded->border_shader_node->node);
			view->rounded->border_shader_node = NULL;
		}

		if (view->rounded->corner_mask_node) {
			wlr_scene_node_destroy(&view->rounded->corner_mask_node->node);
			view->rounded->corner_mask_node = NULL;
		}

		if (view->rounded->corner_mask_buf) {
			effects_destroy_buffer(&view->rounded->corner_mask_buf, view->rounded->corner_mask_native);
		}

		free(view->rounded);
		view->rounded = NULL;
	}

	if (view->shadow) {
		if (view->shadow->shadow_node) {
			wlr_scene_node_destroy(&view->shadow->shadow_node->node);
			view->shadow->shadow_node = NULL;
		}
		if (view->shadow->shadow_buf) {
			effects_destroy_buffer(&view->shadow->shadow_buf, view->shadow->shadow_native);
		}
		free(view->shadow);
		view->shadow = NULL;
	}
}
