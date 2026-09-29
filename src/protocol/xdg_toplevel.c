#include "animation.h"
#include "effects/effects.h"
#include "input/cursor.h"
#include "input/input_method.h"
#include "ipc/ipc.h"
#include "layout/floating.h"
#include "layout/layout.h"
#include "layout/scroller.h"
#include "output/output.h"
#include "protocol/popup.h"
#include "protocol/workspace.h"
#include "protocol/xdg_toplevel.h"
#include "protocol/xwayland.h"
#include "render_unfocused.h"
#include "rule.h"
#include "scratchpad.h"
#include "server.h"
#include "surface.h"
#include "tabs.h"
#include "transaction.h"
#include "tree.h"
#include "types.h"
#include <math.h>
#include <pixman.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_content_type_v1.h>
#include <wlr/types/wlr_damage_ring.h>
#include <wlr/types/wlr_ext_background_effect_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_toplevel_tag_v1.h>
#include <wlr/util/log.h>

static void xdg_toplevel_handle_maximize(xdg_toplevel_t *toplevel, bool requested_maximized);
static void xdg_view_impl_set_activated(view_t *view, bool activated);
static void xdg_view_impl_close(view_t *view);
static void xdg_view_impl_set_decorations(view_t *view);
static void xdg_view_impl_configure(view_t *view, struct wlr_box rect);

static const view_impl_t xdg_view_impl = {
	.set_activated = xdg_view_impl_set_activated,
	.close = xdg_view_impl_close,
	.set_decorations = xdg_view_impl_set_decorations,
	.configure = xdg_view_impl_configure,
};

static bool xdg_toplevel_should_use_server_decorations(xdg_toplevel_t *tl) {
	if (!tl || !tl->view.node)
		return false;

	switch (settings.decoration_mode) {
	case DECORATION_NONE:
	case DECORATION_TABS:
		return true;
	case DECORATION_ALWAYS:
		return tabbed_ancestor(tl->view.node) != NULL;
	case DECORATION_CSD:
		return false;
	}
	return false;
}

void xdg_toplevel_apply_decoration_mode(xdg_toplevel_t *tl) {
	if (!tl || !tl->xdg_decoration || !tl->xdg_toplevel || !tl->xdg_toplevel->base)
		return;

	if (!tl->xdg_toplevel->base->initialized)
		return;

	enum wlr_xdg_toplevel_decoration_v1_mode mode = xdg_toplevel_should_use_server_decorations(tl) ?
		WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE : WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;

	wlr_xdg_toplevel_decoration_v1_set_mode(tl->xdg_decoration, mode);
}

static void xdg_view_impl_set_activated(view_t *view, bool activated) {
	xdg_toplevel_t *toplevel = view_to_xdg(view);
	if (toplevel == NULL || toplevel->xdg_toplevel == NULL)
		return;

	if (activated) {
		struct wlr_surface *prev = server.seat->keyboard_state.focused_surface;
		struct wlr_xdg_toplevel *prev_toplevel = prev ? wlr_xdg_toplevel_try_from_wlr_surface(prev) :
			NULL;
		if (prev_toplevel != NULL)
			wlr_xdg_toplevel_set_activated(prev_toplevel, false);
	}

	wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, activated);
}

static void xdg_view_impl_set_decorations(view_t *view) {
	xdg_toplevel_t *toplevel = view_to_xdg(view);
	if (toplevel)
		xdg_toplevel_apply_decoration_mode(toplevel);
}

static void xdg_view_impl_configure(view_t *view, struct wlr_box rect) {
	xdg_toplevel_t *toplevel = view_to_xdg(view);
	if (toplevel == NULL || toplevel->xdg_toplevel == NULL)
		return;

	if (rect.width == (int)view->last_requested.width &&
		rect.height == (int)view->last_requested.height)
		return;

	wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, rect.width, rect.height);
	view->last_requested.width = rect.width;
	view->last_requested.height = rect.height;
}

static void xdg_view_impl_close(view_t *view) {
	xdg_toplevel_t *toplevel = view_to_xdg(view);
	if (toplevel && toplevel->xdg_toplevel)
		wlr_xdg_toplevel_send_close(toplevel->xdg_toplevel);
}

static void xdg_toplevel_set_floating(xdg_toplevel_t *toplevel, node_t *n, output_t *output) {
	wlr_scene_node_reparent(&toplevel->view.scene_tree->node, server.float_tree);
	struct wlr_box mon_rect = output->rectangle;
	struct wlr_box base_rect = view_to_xdg(n->client->view)->xdg_toplevel->base->geometry;
	n->client->floating_rectangle = (struct wlr_box){
		.x = mon_rect.x + (mon_rect.width - base_rect.width) / 2,
		.y = mon_rect.y + (mon_rect.height - base_rect.height) / 2,
		.width = base_rect.width,
		.height = base_rect.height
	};
	wlr_scene_node_set_position(&toplevel->view.scene_tree->node, n->client->floating_rectangle.x,
		n->client->floating_rectangle.y);
	effects_dirty_corner_masks(output);
	n->client->last_state = STATE_TILED;
	n->client->state = STATE_FLOATING;
	node_set_hidden(n, false);
	// a rule can ask for the toplevel to start out minimized
	if (!n->client->flags.minimized) {
		n->client->flags.shown = true;
		wlr_scene_node_set_enabled(&toplevel->view.scene_tree->node, true);
	}
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
	(void)data;
	xdg_toplevel_t *toplevel = wl_container_of(listener, toplevel, map);
	xdg_toplevel_adopt(toplevel);
}

void xdg_toplevel_adopt(xdg_toplevel_t *toplevel) {
	wlr_log(WLR_INFO, "Toplevel mapped");

	toplevel->view.mapped = true;
	toplevel->view.configured = false;

	output_t *m = server.focused_output;
	if (m == NULL) {
		wlr_log(WLR_ERROR, "No monitor available for toplevel %p", (void *)toplevel);
		return;
	}

	desktop_t *d = m->desk;
	if (d == NULL) {
		// try to find a valid output with a desk
		output_t *valid_output = output_get_valid();
		if (valid_output) {
			m = valid_output;
			d = m->desk;
			server.focused_output = m;
		} else {
			wlr_log(WLR_ERROR, "No desktop available for toplevel %p", (void *)toplevel);
			return;
		}
	}

	node_t *n = make_node(0);
	if (n == NULL) {
		wlr_log(WLR_ERROR, "Failed to create node for toplevel %p", (void *)toplevel);
		return;
	}

	n->client = make_client();
	if (n->client == NULL) {
		wlr_log(WLR_ERROR, "Failed to create client for toplevel %p", (void *)toplevel);
		free_node(n);
		return;
	}

	// link client and view
	n->client->view = &toplevel->view;
	toplevel->view.client = n->client;
	toplevel->view.node = n;

	// populate constraints from xdg_toplevel protocol state
	struct wlr_xdg_toplevel_state *toplevel_state = &toplevel->xdg_toplevel->current;
	if (toplevel_state->min_width > 0)
		n->constraints.min_width = toplevel_state->min_width;
	if (toplevel_state->min_height > 0)
		n->constraints.min_height = toplevel_state->min_height;

	// set initial app_id and title
	const char *app_id = toplevel->xdg_toplevel->app_id;
	const char *title = toplevel->xdg_toplevel->title;

	client_set_app_id(n->client, app_id);
	client_set_title(n->client, title);

	wlr_log(WLR_INFO, "New window: %s (%s)", title ? title : "untitled", app_id ? app_id : "unknown");

	rule_consequence_t *rule = find_matching_rule(app_id, title, toplevel->tag);

	if (rule && rule->has & RULE_TYPE_MANAGE && !(rule->flags & RULE_TYPE_MANAGE)) {
		wlr_log(WLR_INFO, "Window %s ignored by rule (manage=off)", app_id ? app_id : "?");
		n->client->view = NULL;
		toplevel->view.client = NULL;
		toplevel->view.node = NULL;
		free_node(n);
		return;
	}

	bool should_focus = true;
	if (rule) {
		if (rule->has & RULE_TYPE_FOCUS && !(rule->flags & RULE_TYPE_FOCUS))
			should_focus = false;
	}

	// find target monitor
	output_t *target_output = m;
	if (rule && rule->has & RULE_TYPE_MONITOR) {
		wlr_log(WLR_DEBUG, "  Rule specifies monitor: %s", rule->monitor);
		struct output_t *new_monitor = find_output_by_name(rule->monitor);
		if (new_monitor) {
			target_output = new_monitor;
			wlr_log(WLR_DEBUG, "  Target desktop changed to: %s", target_output->name);
		} else {
			wlr_log(WLR_ERROR, "  Monitor %s not found", rule->monitor);
		}
	}

	if (!target_output)
		target_output = m;

	// If target_output is disabled, fall back to the focused output
	if (!target_output || !target_output->wlr_output) {
		wlr_log(WLR_INFO, "Target output is not available, using focused output");
		target_output = output_get_valid();
		if (!target_output) {
			wlr_log(WLR_ERROR, "No valid output available for toplevel %p", (void *)toplevel);
			free_node(n);
			return;
		}
	}

	n->output = target_output;

	// find target desktop
	desktop_t *target_desktop = d;
	wlr_log(WLR_DEBUG, "Window %s: current desktop=%s, has_rule=%d", app_id ? app_id : "?", d->name,
		rule != NULL);
	if (rule && rule->has & RULE_TYPE_DESKTOP) {
		wlr_log(WLR_DEBUG, "  Rule specifies desktop: %s", rule->desktop);
		desktop_t *new_desk = find_desktop_by_name_in_monitor(target_output, rule->desktop);
		if (new_desk) {
			target_desktop = new_desk;
			wlr_log(WLR_DEBUG, "  Target desktop changed to: %s", target_desktop->name);
		} else {
			wlr_log(WLR_ERROR, "  Desktop %s not found", rule->desktop);
		}
	}

	bool desktop_changed = (target_desktop != d);
	wlr_log(WLR_DEBUG, "  Final target desktop: %s (changed=%d)", target_desktop->name,
		desktop_changed);

	rule_apply_consequence(n, n->client, rule);
	rule_apply_view_consequence(&toplevel->view, rule);

	view_create_foreign_toplevels(&toplevel->view, app_id, title);

	// center to output if floating, also ensure it does not tile
	if (rule && rule->state == STATE_FLOATING) {
		xdg_toplevel_set_floating(toplevel, n, target_output);
	} else if (rule && rule->state == STATE_PSEUDO_TILED) {
		struct wlr_box base_rect = view_to_xdg(n->client->view)->xdg_toplevel->base->geometry;
		n->client->floating_rectangle = (struct wlr_box){
			.x = 0,
			.y = 0,
			.width = base_rect.width,
			.height = base_rect.height
		};
		n->client->state = STATE_PSEUDO_TILED;
	} else if (settings.auto_float_dialogs && toplevel->is_dialog) {
		xdg_toplevel_set_floating(toplevel, n, target_output);
	} else if (!(rule && rule->has & RULE_TYPE_STATE)) {
		// let the layout of the target desktop decide how new toplevels spawn
		layout_init_client(target_output, target_desktop, n->client);
	}

	// notify wlr_foreign_toplevel clients about the output association
	if (toplevel->view.foreign_toplevel && target_output && target_output->wlr_output)
		wlr_foreign_toplevel_handle_v1_output_enter(toplevel->view.foreign_toplevel,
			target_output->wlr_output);

	// create borders if applicable
	create_borders(toplevel->view.scene_tree, &toplevel->view.border_tree,
		toplevel->view.border_rects);

	// mark dirty to update borders
	if (n->client->state == STATE_FLOATING)
		node_set_dirty(n);

	// insert node into tree, floating toplevels are kept out of it
	if (IS_FLOATING(n->client)) {
		n->desktop = target_desktop;
	} else {
		node_t *focus = target_desktop->focus;
		insert_node(target_desktop, n, focus);
	}

	// in scroller layout, also track the toplevel in the scroller state.
	if (target_desktop->layout == LAYOUT_SCROLLER && target_desktop->scroller_state &&
		IS_TILED(n->client))
		scroller_add_tile(target_desktop->scroller_state, n->client, should_focus);

	// notify client of scale
	if (target_output && target_output->wlr_output) {
		float scale = target_output->wlr_output->scale;
		wlr_fractional_scale_v1_notify_scale(toplevel->xdg_toplevel->base->surface, scale);
		wlr_surface_set_preferred_buffer_scale(toplevel->xdg_toplevel->base->surface, ceil(scale));
	}

	// hide window if target desktop is not the focused desktop
	bool target_desktop_is_focused = (target_desktop == (target_output ? target_output->desk : NULL));
	if (desktop_changed && !target_desktop_is_focused) {
		n->client->flags.shown = false;
		wlr_scene_node_set_enabled(&toplevel->view.scene_tree->node, false);
	}

	if (should_focus && target_output)
		activate_node(target_output, target_desktop, n);

	bool rule_has_state = rule && (rule->has & RULE_TYPE_STATE);
	if (rule && rule->state == STATE_FULLSCREEN) {
		client_set_fullscreen(target_output, target_desktop, n, true);
	} else if (!rule_has_state && toplevel->xdg_toplevel->requested.fullscreen) {
		// client requested fullscreen before map
		client_set_fullscreen(target_output, target_desktop, n, true);
	}

	// a client can ask to be maximized before the window is mapped
	if (!rule_has_state && toplevel->xdg_toplevel->requested.maximized)
		xdg_toplevel_handle_maximize(toplevel, true);

	if (n->client->flags.minimized)
		desktop_minimized_push(target_desktop, n);

	toplevel->view.wants_fade = true;
	arrange(target_output, target_desktop, true);

	ipc_put_status(SUB_MASK_NODE_ADD, "node_add[%s,%s,%u]\n",
		n->client && n->client->app_id[0] ? n->client->app_id : "?",
		n->client && n->client->title[0] ? n->client->title : "?", n->id);

	// apply the configured xdg-decoration policy
	xdg_toplevel_apply_decoration_mode(toplevel);

	if (!n->client->flags.block_out_from_screenshare) {
		toplevel->view.image_capture_surface =
			wlr_scene_surface_create(&toplevel->view.image_capture->tree,
			toplevel->xdg_toplevel->base->surface);
	}

	client_update_foreign_toplevel_state(toplevel->view.client);

	render_unfocused_client_update(n->client);

	wlr_log(WLR_INFO, "Window mapped and tiled: %s",
		n->client->title[0] ? n->client->title : "untitled");
}

void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
	(void)data;
	xdg_toplevel_t *toplevel = wl_container_of(listener, toplevel, unmap);

	wlr_log(WLR_INFO, "Toplevel unmapped");

	toplevel->view.mapped = false;
	toplevel->view.configured = false;

	toplevel->view.image_capture_surface = NULL;
	animation_cancel_view(&toplevel->view);

	view_destroy_foreign_toplevels(&toplevel->view);

	if (toplevel->view.node == NULL)
		return;

	if (!animation_fade_out(&toplevel->view))
		animation_cancel_node(toplevel->view.node);

	if (toplevel->view.scene_tree && !animation_has_fade_out(toplevel->view.scene_tree))
		wlr_scene_node_set_enabled(&toplevel->view.scene_tree->node, false);

	if (settings.enable_animations && toplevel->view.client && toplevel->view.client->flags.shown)
		view_save_buffer(&toplevel->view);

	node_t *n = toplevel->view.node;
	output_t *m = n->output ? n->output : mon;
	desktop_t *d = desktop_for_node(n);
	bool node_held = false;

	if (d == NULL) {
		wlr_log(WLR_ERROR, "Could not find desktop for node %u, using current desktop", n->id);
		d = m ? m->desk : NULL;
	} else if (d->output) {
		m = d->output;
	}

	// freeze sibling buffers before modifying layout tree so current visual
	// state is captured
	view_freeze_sibling_buffers(d, n);

	if (m && d) {
		if (n) {
			node_set_dirty(n);
			ipc_put_status(SUB_MASK_NODE_REMOVE, "node_remove[%s,%s,%u]\n",
				n->client && n->client->app_id[0] ? n->client->app_id : "?",
				n->client && n->client->title[0] ? n->client->title : "?", n->id);
		}
		// in scroller layout, remove from scroller state first
		if (d->layout == LAYOUT_SCROLLER && d->scroller_state && n->client) {
			scroller_remove_tile(d->scroller_state, n->client, m);
			scroller_apply_active_focus(d, m);
		}

		remove_node(d, n);

		if (n)
			n->destroying = true;
		if (n && n->client) {
			n->client->view = NULL;
		}

		// arrange() can commit the transaction right away, which frees a destroying node that
		// nothing waits for any more, hold a reference until the view is reported as unmapped
		if (n) {
			n->ntxnrefs++;
			node_held = true;
		}
		arrange(m, d, true);

		toplevel->view.node = NULL;
		toplevel->view.client = NULL;

		// focus handling after removing node
		if (d->layout == LAYOUT_SCROLLER) {
			// fall back to tree focus only if croller is empty
			if (d->focus == NULL && d->root != NULL) {
				d->focus = first_extrema(d->root);
				if (d->focus != NULL)
					focus_node(d->output ? d->output : m, d, d->focus);
			}
		} else {
			// the closed toplevel was the last one standing (floating layouts never put
			// anything in the tree), hand the focus to the topmost toplevel left
			if (d->focus == n || !node_focusable(d->focus)) {
				node_t *next = desktop_fallback_focus(d, n);
				d->focus = next;
			}

			if (d->focus != NULL)
				focus_node(d->output ? d->output : m, d, d->focus);
		}
	}

	transaction_notify_view_unmapped(n);

	if (node_held) {
		n->ntxnrefs--;
		if (n->destroying && n->ntxnrefs == 0)
			free_node(n);
	}
}

void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
	(void)data;
	xdg_toplevel_t *toplevel = wl_container_of(listener, toplevel, commit);
	struct wlr_xdg_surface *xdg_surface = toplevel->xdg_toplevel->base;

	if (xdg_surface->initial_commit) {
		// initial commit can happen before the xdg_surface is marked initialized
		if (xdg_surface->initialized)
			wlr_xdg_surface_schedule_configure(xdg_surface);

		wlr_xdg_toplevel_set_wm_capabilities(toplevel->xdg_toplevel,
			WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN | WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE |
			WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE);
		return;
	}

	if (toplevel->view.mapped && toplevel->xdg_toplevel->base->surface->mapped) {
		// update constraints
		if (toplevel->view.node) {
			struct wlr_xdg_toplevel_state *state = &toplevel->xdg_toplevel->current;
			bool constraints_changed = false;
			if (state->min_width > 0 &&
					(uint16_t)state->min_width != toplevel->view.node->constraints.min_width) {
				toplevel->view.node->constraints.min_width = state->min_width;
				constraints_changed = true;
			}
			if (state->min_height > 0 &&
					(uint16_t)state->min_height != toplevel->view.node->constraints.min_height) {
				toplevel->view.node->constraints.min_height = state->min_height;
				constraints_changed = true;
			}
			if (constraints_changed && toplevel->view.node->output && toplevel->view.node->desktop) {
				wlr_log(WLR_DEBUG, "Node %u constraints updated to %dx%d", toplevel->view.node->id,
					toplevel->view.node->constraints.min_width, toplevel->view.node->constraints.min_height);
				arrange(toplevel->view.node->output, toplevel->view.node->desktop, true);
			}
		}

		struct wlr_box *new_geo = &xdg_surface->geometry;
		bool new_size = new_geo->width != toplevel->view.geometry.width ||
			new_geo->height != toplevel->view.geometry.height || new_geo->x != toplevel->view.geometry.x ||
			new_geo->y != toplevel->view.geometry.y;

		if (new_size) {
			// update stored geometry
			memcpy(&toplevel->view.geometry, new_geo, sizeof(struct wlr_box));

			if (toplevel->view.client) {
				client_t *c = toplevel->view.client;

				if (c->state == STATE_FLOATING) {
					if (c->floating_rectangle.width > 0) {
						int old_w = c->floating_rectangle.width;
						int old_h = c->floating_rectangle.height;
						c->floating_rectangle.width = toplevel->view.geometry.width;
						c->floating_rectangle.height = toplevel->view.geometry.height;
						c->floating_rectangle.x += (old_w - toplevel->view.geometry.width) / 2;
						c->floating_rectangle.y += (old_h - toplevel->view.geometry.height) / 2;
						wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, toplevel->view.geometry.width,
							toplevel->view.geometry.height);
						toplevel->view.last_requested.width = toplevel->view.geometry.width;
						toplevel->view.last_requested.height = toplevel->view.geometry.height;
						transaction_commit_dirty_client();
					}
				} else if (IS_TILED(c)) {
					if (c->tiled_rectangle.width > 0 && c->tiled_rectangle.height > 0 &&
							(toplevel->view.geometry.width != c->tiled_rectangle.width ||
							toplevel->view.geometry.height != c->tiled_rectangle.height)) {
						if (!(toplevel->view.last_requested.width > 0 &&
								toplevel->view.geometry.width == toplevel->view.last_requested.width &&
								toplevel->view.geometry.height == toplevel->view.last_requested.height)) {
							wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, c->tiled_rectangle.width,
								c->tiled_rectangle.height);
							node_set_dirty(toplevel->view.node);
							transaction_commit_dirty_client();
						}
					}
				}
			}
		}

		uint32_t serial = toplevel->xdg_toplevel->base->current.configure_serial;
		bool successful = transaction_notify_view_ready_by_serial(&toplevel->view, serial);

		if (successful)
			wlr_log(WLR_DEBUG, "Transaction completed for serial=%u", serial);

		// ack as configured if serial is non-zero
		if (!toplevel->view.configured && serial != 0) {
			toplevel->view.configured = true;
			wlr_log(WLR_DEBUG, "Toplevel marked configured via serial=%u (transaction match=%d)", serial,
				successful);
		}

		if (toplevel->view.saved_surface_tree && !successful)
			view_send_frame_done(&toplevel->view);

		if (!animation_is_resizing(toplevel->view.node))
			view_center_and_clip_surface(&toplevel->view);
	}

	// check ext_background_effect_v1 state
	const struct wlr_ext_background_effect_surface_v1_state *fx =
		wlr_ext_background_effect_v1_get_surface_state(xdg_surface->surface);
	bool wants_blur = fx && !pixman_region32_empty(&fx->blur_region);
	bool has_blur = blur_count(toplevel->view.blur) > 0;

	// only update blur from protocol if it wasn't set by a rule
	if (toplevel->view.client && !toplevel->view.client->flags.blur_from_rule) {
		if (wants_blur != has_blur)
			surface_client_set_effect(toplevel->view.client, EFFECT_BLUR, wants_blur);
		if (toplevel->view.blur && fx) {
			if (!pixman_region32_equal(&toplevel->view.blur->blur_region, &fx->blur_region)) {
				pixman_region32_copy(&toplevel->view.blur->blur_region, &fx->blur_region);
				toplevel->view.blur->blur_region_dirty = true;
			}
		}
	}

	// update opacity
	if (toplevel->view.client && !animation_is_opacity_fading(&toplevel->view))
		surface_set_opacity(&toplevel->view.scene_tree->node, toplevel->view.client->opacity);

	if (toplevel->view.node && toplevel->view.node->output)
		output_schedule_frame(toplevel->view.node->output);
}

void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	xdg_toplevel_t *toplevel = wl_container_of(listener, toplevel, destroy);

	wlr_log(WLR_INFO, "Toplevel destroyed");

	client_t *client = toplevel->view.client;

	view_destroy(&toplevel->view);

	if (client)
		render_unfocused_client_remove(client);

	wl_list_remove(&toplevel->map.link);
	wl_list_remove(&toplevel->unmap.link);
	wl_list_remove(&toplevel->commit.link);
	wl_list_remove(&toplevel->new_xdg_popup.link);
	wl_list_remove(&toplevel->destroy.link);
	wl_list_remove(&toplevel->request_move.link);
	wl_list_remove(&toplevel->request_resize.link);
	wl_list_remove(&toplevel->request_maximize.link);
	wl_list_remove(&toplevel->request_fullscreen.link);
	wl_list_remove(&toplevel->request_minimize.link);
	wl_list_remove(&toplevel->set_title.link);
	wl_list_remove(&toplevel->set_app_id.link);

	if (toplevel->xdg_decoration) {
		wl_list_remove(&toplevel->decoration_destroy.link);
		wl_list_remove(&toplevel->decoration_request_mode.link);
		toplevel->xdg_decoration = NULL;
	}

	free(toplevel->tag);
	free(toplevel);
}

void xdg_toplevel_request_move(struct wl_listener *listener, void *data) {
	(void)data;
	xdg_toplevel_t *toplevel = wl_container_of(listener, toplevel, request_move);
	wlr_log(WLR_DEBUG, "Toplevel requested move");

	if (toplevel->view.client && toplevel->view.client->state == STATE_FLOATING)
		view_begin_interactive(&toplevel->view, CURSOR_MOVE, 0);
	else if (toplevel->xdg_toplevel->base->initialized)
		wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
}

void xdg_toplevel_request_resize(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_resize_event *event = data;
	xdg_toplevel_t *toplevel = wl_container_of(listener, toplevel, request_resize);
	wlr_log(WLR_DEBUG, "Toplevel requested resize");

	if (!event || !toplevel->view.client)
		return;

	if (toplevel->view.client->state == STATE_FLOATING)
		view_begin_interactive(&toplevel->view, CURSOR_RESIZE, event->edges);
	else if (toplevel->xdg_toplevel->base->initialized)
		wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
}

static void xdg_toplevel_handle_maximize(xdg_toplevel_t *toplevel, bool requested_maximized) {
	node_t *n = toplevel->view.node;
	if (n == NULL || n->client == NULL)
		return;

	output_t *m = n->output;
	desktop_t *d = m != NULL ? m->desk : (mon != NULL ? mon->desk : NULL);
	if (d == NULL)
		return;

	client_set_maximized(m, d, n, requested_maximized);
}

void xdg_toplevel_request_maximize(struct wl_listener *listener, void *data) {
	(void)data;
	xdg_toplevel_t *toplevel = wl_container_of(listener, toplevel, request_maximize);

	if (!toplevel->xdg_toplevel->base->initialized)
		return;
	if (toplevel->view.client == NULL)
		return;
	if (toplevel->view.client->state == STATE_FULLSCREEN)
		return;

	bool requested_maximized = toplevel->xdg_toplevel->requested.maximized;
	if (requested_maximized == toplevel->view.client->flags.maximized)
		return;

	xdg_toplevel_handle_maximize(toplevel, requested_maximized);
}

void xdg_toplevel_request_fullscreen(struct wl_listener *listener, void *data) {
	(void)data;
	xdg_toplevel_t *toplevel = wl_container_of(listener, toplevel, request_fullscreen);

	if (!toplevel->xdg_toplevel->base->initialized)
		return;
	if (toplevel->view.client == NULL)
		return;

	bool requested_fullscreen = toplevel->xdg_toplevel->requested.fullscreen;
	if (requested_fullscreen == (toplevel->view.client->state == STATE_FULLSCREEN))
		return;

	output_t *m = toplevel->view.node->output;
	desktop_t *d = m ? m->desk : NULL;

	client_set_fullscreen(m, d, toplevel->view.node, requested_fullscreen);
}

void xdg_toplevel_request_minimize(struct wl_listener *listener, void *data) {
	(void)data;
	xdg_toplevel_t *toplevel = wl_container_of(listener, toplevel, request_minimize);

	if (!view_is_ready(&toplevel->view))
		return;

	output_t *m = toplevel->view.node != NULL ? toplevel->view.node->output : NULL;
	desktop_t *d = m != NULL ? m->desk : NULL;

	client_set_minimized(m, d, toplevel->view.node, true);
}

void xdg_toplevel_set_title(struct wl_listener *listener, void *data) {
	(void)data;
	xdg_toplevel_t *toplevel = wl_container_of(listener, toplevel, set_title);

	view_set_title(&toplevel->view, toplevel->xdg_toplevel->title);
}

void xdg_toplevel_set_app_id(struct wl_listener *listener, void *data) {
	(void)data;
	xdg_toplevel_t *toplevel = wl_container_of(listener, toplevel, set_app_id);

	view_set_app_id(&toplevel->view, toplevel->xdg_toplevel->app_id);
}

xdg_toplevel_t *xdg_toplevel_create(struct wlr_xdg_toplevel *xdg_toplevel) {
	xdg_toplevel_t *toplevel = calloc(1, sizeof(*toplevel));
	if (!toplevel) {
		wlr_log(WLR_ERROR, "Allocation failed");
		return NULL;
	}

	toplevel->xdg_toplevel = xdg_toplevel;
	toplevel->view.impl = &xdg_view_impl;

	if (!view_init(&toplevel->view, VIEW_XDG)) {
		free(toplevel);
		return NULL;
	}

	toplevel->unmap.notify = xdg_toplevel_unmap;
	wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);

	// create surface scene within the content tree
	struct wlr_scene_tree *xdg_tree = wlr_scene_xdg_surface_create(toplevel->view.content_tree,
		xdg_toplevel->base);
	if (!xdg_tree) {
		wlr_log(WLR_ERROR, "Failed to create XDG surface scene for toplevel");
		wlr_scene_node_destroy(&toplevel->view.scene_tree->node);
		free(toplevel);
		return NULL;
	}

	toplevel->view.scene_tree->node.data = &toplevel->view;
	xdg_toplevel->base->data = toplevel;

	wlr_scene_node_set_enabled(&toplevel->view.scene_tree->node, false);

	// register event listeners
	toplevel->map.notify = xdg_toplevel_map;
	wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);

	toplevel->commit.notify = xdg_toplevel_commit;
	wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);

	toplevel->new_xdg_popup.notify = handle_new_xdg_popup;
	wl_signal_add(&xdg_toplevel->base->events.new_popup, &toplevel->new_xdg_popup);

	toplevel->destroy.notify = xdg_toplevel_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);

	toplevel->request_move.notify = xdg_toplevel_request_move;
	wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);

	toplevel->request_resize.notify = xdg_toplevel_request_resize;
	wl_signal_add(&xdg_toplevel->events.request_resize, &toplevel->request_resize);

	toplevel->request_maximize.notify = xdg_toplevel_request_maximize;
	wl_signal_add(&xdg_toplevel->events.request_maximize, &toplevel->request_maximize);

	toplevel->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);

	toplevel->request_minimize.notify = xdg_toplevel_request_minimize;
	wl_signal_add(&xdg_toplevel->events.request_minimize, &toplevel->request_minimize);

	toplevel->set_title.notify = xdg_toplevel_set_title;
	wl_signal_add(&xdg_toplevel->events.set_title, &toplevel->set_title);

	toplevel->set_app_id.notify = xdg_toplevel_set_app_id;
	wl_signal_add(&xdg_toplevel->events.set_app_id, &toplevel->set_app_id);

	return toplevel;
}
