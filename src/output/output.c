#include "animation.h"
#include "client.h"
#include "diagnostics.h"
#include "dim.h"
#include "effects/effects.h"
#include "ipc/ipc.h"
#include "layout/floating.h"
#include "layout/layout.h"
#include "output/config.h"
#include "output/output.h"
#include "protocol/idle.h"
#include "protocol/layer.h"
#include "protocol/lock.h"
#include "protocol/workspace.h"
#include "protocol/xwayland.h"
#include "server.h"
#include "str.h"
#include "tree.h"
#include "types.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/backend/headless.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_damage_ring.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

static void handle_output_destroy(struct wl_listener *listener, void *data);
static int output_present_timeout(void *data);

// active desktop of the last removed output, restored when an output comes back
static desktop_t *orphan_active_desk;
static char last_removed_name[SMALEN];

static struct wl_listener layout_change;

static void handle_layout_change(struct wl_listener *listener, void *data) {
	(void)listener;
	(void)data;

	output_t *m;
	wl_list_for_each(m, &mon_list, link) {
		if (!m->enabled || !m->wlr_output || !m->wlr_output->enabled)
			continue;

		struct wlr_box layout_box;
		wlr_output_layout_get_box(server.output_layout, m->wlr_output, &layout_box);
		if (wlr_box_equal(&layout_box, &m->rectangle))
			continue;

		output_update_geometry(m);
	}
}

void output_init(void) {
	layout_change.notify = handle_layout_change;
	wl_signal_add(&server.output_layout->events.change, &layout_change);
}

void output_fini(void) {
	wl_list_remove(&layout_change.link);
}

static void output_orphan_desktops(output_t *output) {
	desktop_t *d, *dtmp;
	wl_list_for_each_safe(d, dtmp, &output->desk_list, link) {
		wl_list_remove(&d->link);
		desktop_clear_output(d, NULL);

		d->output = NULL;
		wl_list_insert(orphan_desk_list.prev, &d->link);
	}
	wl_list_init(&output->desk_list);
	output->desk = NULL;
}

static void output_recover_after_removal(desktop_t *active_desk, bool was_focused) {
	orphan_active_desk = active_desk;

	output_t *survivor = output_get_valid();

	if (!survivor) {
		if (was_focused) {
			output_set_focused(NULL);
			if (server.seat)
				wlr_seat_keyboard_notify_clear_focus(server.seat);
		}
		workspace_sync();
		return;
	}

	if (!survivor->desk && !wl_list_empty(&survivor->desk_list))
		survivor->desk = wl_container_of(survivor->desk_list.next, survivor->desk, link);

	if (was_focused)
		output_set_focused(survivor);

	if (survivor->desk) {
		output_update_geometry(survivor);
		workspace_update_visibility(survivor, survivor->desk);

		node_t *target = survivor->desk->focus != NULL ? survivor->desk->focus :
			desktop_fallback_focus(survivor->desk, NULL);
		if (target != NULL) {
			survivor->desk->focus = target;
			focus_node(survivor, survivor->desk, target);
		} else if (was_focused && server.seat) {
			wlr_seat_keyboard_notify_clear_focus(server.seat);
		}
	}

	workspace_sync();
}

bool output_supports_hdr(const output_t *output, const char **unsupported_reason_ptr) {
	const char *unsupported_reason = NULL;
	if (!(output->wlr_output->supported_primaries & WLR_COLOR_NAMED_PRIMARIES_BT2020)) {
		unsupported_reason = "BT2020 primaries not supported by output";
	} else if (!(output->wlr_output->supported_transfer_functions &
			WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ)) {
		unsupported_reason = "PQ transfer function not supported by output";
	} else if (!server.renderer->features.output_color_transform) {
		unsupported_reason = "renderer doesn't support output color transforms";
	}
	if (unsupported_reason_ptr != NULL) {
		*unsupported_reason_ptr = unsupported_reason;
	}
	return unsupported_reason == NULL;
}

static bool output_is_hdr_active(const output_t *output) {
	return output->wlr_output->image_description != NULL;
}

static enum wlr_scale_filter_mode get_scale_filter(const output_t *output,
		const struct wlr_scene_buffer *buffer) {
	if (buffer->dst_width > 0 && buffer->dst_height > 0 &&
		(buffer->dst_width < buffer->WLR_PRIVATE.buffer_width ||
		buffer->dst_height < buffer->WLR_PRIVATE.buffer_height))
		return WLR_SCALE_FILTER_BILINEAR;

	switch (output->scale_filter_mode) {
	case SCALE_FILTER_LINEAR:
		return WLR_SCALE_FILTER_BILINEAR;
	case SCALE_FILTER_NEAREST:
	case SCALE_FILTER_AUTO:
	default:
		return WLR_SCALE_FILTER_NEAREST;
	}
}

static void output_configure_scene_iterator(struct wlr_scene_buffer *buffer, int sx, int sy,
		void *data) {
	(void)sx;
	(void)sy;
	output_t *output = data;
	if (!output)
		return;

	buffer->filter_mode = get_scale_filter(output, buffer);
}

static void output_configure_scene(output_t *output) {
	if (!output)
		return;
	if (output->scale_filter_mode == output->applied_scale_filter)
		return;

	wlr_scene_node_for_each_buffer(&server.bg_tree->node, output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&server.bot_tree->node, output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&server.tile_tree->node, output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&server.float_tree->node, output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&server.top_tree->node, output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&server.full_tree->node, output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&server.over_tree->node, output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&server.lock_tree->node, output_configure_scene_iterator, output);

	wlr_scene_node_for_each_buffer(&output->layer_bg->node, output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&output->layer_bottom->node, output_configure_scene_iterator,
		output);
	wlr_scene_node_for_each_buffer(&output->layer_top->node, output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&output->layer_overlay->node, output_configure_scene_iterator,
		output);

	output->applied_scale_filter = output->scale_filter_mode;
}

static struct wlr_surface *fullscreen_surface(const output_t *output);

static bool output_can_tear(const output_t *output) {
	return output->allow_tearing;
}

static client_t *output_fullscreen_client(const output_t *output) {
	if (output == NULL || output->desk == NULL)
		return NULL;

	// floating and fullscreen toplevels live outside the split tree
	node_t **toplevels = NULL;
	int count = desktop_toplevels(output->desk, &toplevels);
	if (toplevels != NULL) {
		client_t *found = NULL;
		for (int i = 0; i < count && found == NULL; i++) {
			node_t *n = toplevels[i];
			if (n == NULL || n->client == NULL || n->output != output)
				continue;
			if (n->client->state == STATE_FULLSCREEN)
				found = n->client;
		}
		free(toplevels);
		if (found != NULL)
			return found;
	}

	if (output->desk->root != NULL) {
		FOR_EACH_LEAF(n, output->desk->root) {
			if (n->client != NULL && n->client->state == STATE_FULLSCREEN && n->output == output)
				return n->client;
		}
	}

	return NULL;
}

static bool output_has_fullscreen_cover(const output_t *output) {
	return client_is_mapped(output_fullscreen_client(output));
}

static bool fullscreen_has_effects(const output_t *output) {
	const client_t *client = output_fullscreen_client(output);
	if (client == NULL)
		return false;
	return client->flags.blur || client->flags.mica || client->flags.acrylic;
}

static struct wlr_surface *fullscreen_surface(const output_t *output) {
	const client_t *client = output_fullscreen_client(output);
	if (!client_is_mapped(client))
		return NULL;

	return client_wlr_surface(client);
}

static void output_configure_scene_visible(output_t *output) {
	if (!output)
		return;
	if (output->scale_filter_mode == output->applied_scale_filter)
		return;

	wlr_scene_node_for_each_buffer(&server.full_tree->node, output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&server.over_tree->node, output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&server.shader_tree->node, output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&server.drag_tree->node, output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&server.lock_tree->node, output_configure_scene_iterator, output);

	wlr_scene_node_for_each_buffer(&output->layer_overlay->node, output_configure_scene_iterator,
		output);

	output->applied_scale_filter = output->scale_filter_mode;
}

// direct scanout bypasses composition, so a software cursor would not be drawn
static bool output_needs_software_cursor(const output_t *output) {
	const struct wlr_output *wo = output->wlr_output;
	if (wo->software_cursor_locks > 0)
		return true;

	struct wlr_output_cursor *cursor;
	wl_list_for_each(cursor, &wo->cursors, link) {
		if (cursor->enabled && cursor->visible && cursor != wo->hardware_cursor)
			return true;
	}
	return false;
}

static bool output_try_direct_scanout(output_t *output) {
	if (!output_has_fullscreen_cover(output))
		return false;
	if (output_needs_software_cursor(output))
		return false;
	if (fullscreen_has_effects(output))
		return false;
	if (dim_output_visible(output))
		return false;
	if (server.locked || settings.screen_shader_enabled)
		return false;
	if (!settings.direct_scanout)
		return false;

	struct wlr_surface *surface = fullscreen_surface(output);
	if (!surface || !wlr_surface_has_buffer(surface) || !surface->buffer)
		return false;

	if (surface->current.transform != output->wlr_output->transform)
		return false;

	struct wlr_buffer *buf = &surface->buffer->base;

	struct wlr_output_state state;
	wlr_output_state_init(&state);

	wlr_buffer_lock(buf);
	wlr_output_state_set_buffer(&state, buf);

	state.buffer_dst_box.x = 0;
	state.buffer_dst_box.y = 0;
	state.buffer_dst_box.width = output->wlr_output->width;
	state.buffer_dst_box.height = output->wlr_output->height;

	pixman_region32_t damage;
	pixman_region32_init_rect(&damage, 0, 0, (unsigned int)output->wlr_output->width,
		(unsigned int)output->wlr_output->height);
	wlr_output_state_set_damage(&state, &damage);
	pixman_region32_fini(&damage);

	state.tearing_page_flip = output_can_tear(output);

	bool ok = wlr_output_test_state(output->wlr_output, &state);
	if (!ok) {
		wlr_log(WLR_DEBUG, "Direct scanout test failed for %s", output->name);
		wlr_buffer_unlock(buf);
		wlr_output_state_finish(&state);
		return false;
	}

	ok = wlr_output_commit_state(output->wlr_output, &state);
	if (!ok)
		wlr_log(WLR_ERROR, "Direct scanout commit failed for %s", output->name);

	wlr_buffer_unlock(buf);
	wlr_output_state_finish(&state);
	return ok;
}

const long NSEC_PER_MSEC = 1000000;
const long NSEC_PER_SEC = 1000000000;

static void output_repaint_frame(output_t *output) {
	struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(server.scene,
		output->wlr_output);
	if (!scene_output)
		return;

	const bool has_fs = output_has_fullscreen_cover(output);
	const bool fs_effects = has_fs && fullscreen_has_effects(output);

	if (has_fs)
		output_configure_scene_visible(output);
	else
		output_configure_scene(output);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	bool animating = animation_update_output(output, now);

	// ensure blur nodes track animated window positions during workspace slide
	if (animating)
		animation_update_slide_blur(output);

	if (effects_state.available && (fs_effects || !has_fs))
		effects_output_frame(output, scene_output);

	if (has_fs && !fs_effects && output_try_direct_scanout(output)) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		wlr_scene_output_send_frame_done(scene_output, &now);
		if (animating)
			output_schedule_frame(output);

		return;
	}

	struct wlr_scene_output_state_options opts = {
		.color_transform = output->color_transform,
	};

	struct wlr_output_state pending;
	wlr_output_state_init(&pending);
	if (!wlr_scene_output_build_state(scene_output, &pending, &opts)) {
		wlr_output_state_finish(&pending);
		if (animating)
			output_schedule_frame(output);

		return;
	}

	if (output_can_tear(output)) {
		pending.tearing_page_flip = true;

		if (!wlr_output_test_state(output->wlr_output, &pending)) {
			wlr_log(WLR_DEBUG, "Output test failed, retrying without tearing page-flip");
			pending.tearing_page_flip = false;
		}
	}

	if (!wlr_output_commit_state(output->wlr_output, &pending)) {
		if (!output->frame_commit_failed)
			wlr_log(WLR_ERROR, "Failed to commit output state for %s", output->name);
		output->frame_commit_failed = true;
		output->frame_commit_failures++;
		if (output->frame_commit_failures >= 60) {
			output->frame_commit_failures = 0;
			output->frame_needs_modeset = true;
		}
		wlr_damage_ring_add_whole(&scene_output->damage_ring);
		output->wlr_output->frame_pending = false;
		output->frame_retry_pending = true;
		if (output->repaint_timer)
			wl_event_source_timer_update(output->repaint_timer, 16);
	} else {
		if (output->frame_retry_pending) {
			output->frame_retry_pending = false;
			if (output->repaint_timer)
				wl_event_source_timer_update(output->repaint_timer, 0);
		}
		if (!wlr_output_is_headless(output->wlr_output) && output->present_timer &&
				!output->awaiting_present) {
			output->awaiting_present = true;
			wl_event_source_timer_update(output->present_timer, 100);
		}
	}
	wlr_output_state_finish(&pending);

	output->hdr = output_is_hdr_active(output);

	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);

	if (animating)
		output_schedule_frame(output);
}

static void output_repaint(output_t *output) {
	diag_stage_enter(DIAG_STAGE_RENDER);
	output_repaint_frame(output);
	diag_stage_leave();
}

void output_schedule_frame(output_t *output) {
	if (!output || output->frame_scheduled)
		return;
	output->frame_scheduled = true;
	wlr_output_schedule_frame(output->wlr_output);
}

static int output_present_timeout(void *data) {
	output_t *output = data;
	if (!output->awaiting_present || !output->enabled || !output->wlr_output->enabled)
		return 0;

	output->awaiting_present = false;
	output->present_misses++;
	if (output->present_misses >= 10) {
		output->present_misses = 0;
		output->frame_needs_modeset = true;
		if (output->repaint_timer)
			wl_event_source_timer_update(output->repaint_timer, 1);
	} else {
		output->awaiting_present = true;
		wl_event_source_timer_update(output->present_timer, 100);
	}

	return 0;
}

static void output_force_modeset(output_t *output) {
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);

	struct wlr_output_mode *mode = output->wlr_output->current_mode;
	if (!mode)
		mode = wlr_output_preferred_mode(output->wlr_output);
	if (mode)
		wlr_output_state_set_mode(&state, mode);

	wlr_log(WLR_INFO, "Forcing modeset on %s after repeated commit failures", output->name);
	if (!wlr_output_commit_state(output->wlr_output, &state))
		wlr_log(WLR_ERROR, "Forced modeset failed for %s", output->name);
	wlr_output_state_finish(&state);

	output->frame_commit_failures = 0;
	output->frame_scheduled = false;
	output->frame_retry_pending = false;
	output->awaiting_present = false;
	output->present_misses = 0;
	output->wlr_output->frame_pending = false;
	if (output->present_timer)
		wl_event_source_timer_update(output->present_timer, 0);

	struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(server.scene,
		output->wlr_output);
	if (scene_output)
		wlr_damage_ring_add_whole(&scene_output->damage_ring);
}

static int output_repaint_timer_handler(void *data) {
	output_t *output = data;
	if (!output->wlr_output->enabled)
		return 0;

	output->frame_retry_pending = false;

	if (output->frame_needs_modeset) {
		output->frame_needs_modeset = false;
		output_force_modeset(output);
		if (!output->wlr_output->enabled)
			return 0;
	}

	if (output->frame_commit_failed) {
		struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(server.scene,
			output->wlr_output);
		if (scene_output)
			wlr_damage_ring_add_whole(&scene_output->damage_ring);
	}

	output->wlr_output->frame_pending = false;
	output_repaint(output);
	return 0;
}

static void output_frame(struct wl_listener *listener, void *data) {
	(void)data;
	output_t *output = wl_container_of(listener, output, frame);
	if (!output->wlr_output->enabled)
		return;
	output->frame_scheduled = false;

	if (output->max_render_time >= 0) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);

		int msec_until_refresh = 0;
		if (output->last_presentation.tv_sec > 0) {
			struct timespec predicted_refresh = output->last_presentation;
			predicted_refresh.tv_nsec += (long)(output->refresh_nsec % NSEC_PER_SEC);
			predicted_refresh.tv_sec += (long)(output->refresh_nsec / NSEC_PER_SEC);
			if (predicted_refresh.tv_nsec >= NSEC_PER_SEC) {
				predicted_refresh.tv_sec += 1;
				predicted_refresh.tv_nsec -= NSEC_PER_SEC;
			}

			if (predicted_refresh.tv_sec >= now.tv_sec) {
				long nsec_until_refresh = (predicted_refresh.tv_sec - now.tv_sec) * NSEC_PER_SEC +
					(predicted_refresh.tv_nsec - now.tv_nsec);
				msec_until_refresh = (int)(nsec_until_refresh / NSEC_PER_MSEC);
			}
		}

		const int delay = msec_until_refresh - output->max_render_time;

		if (delay < 1) {
			output_repaint(output);
		} else {
			output->wlr_output->frame_pending = true;
			wl_event_source_timer_update(output->repaint_timer, delay);
		}
	} else {
		output_repaint(output);
	}
}

static void handle_output_present(struct wl_listener *listener, void *data) {
	output_t *output = wl_container_of(listener, output, present);
	struct wlr_output_event_present *event = data;

	if (!output->enabled || !event->presented)
		return;

	output->last_presentation = event->when;
	output->refresh_nsec = event->refresh;
	output->frame_commit_failed = false;
	output->frame_commit_failures = 0;
	output->present_misses = 0;
	output->frame_needs_modeset = false;
	output->awaiting_present = false;
	if (output->present_timer)
		wl_event_source_timer_update(output->present_timer, 0);
	if (output->frame_retry_pending) {
		output->frame_retry_pending = false;
		if (output->repaint_timer)
			wl_event_source_timer_update(output->repaint_timer, 0);
	}
}

void output_request_state(struct wl_listener *listener, void *data) {
	output_t *output = wl_container_of(listener, output, request_state);
	struct wlr_output_event_request_state *event = data;
	const float old_scale = output->wlr_output->scale;
	wlr_output_commit_state(output->wlr_output, event->state);
	const float new_scale = output->wlr_output->scale;
	if (new_scale != old_scale)
		output_update_scale(output, new_scale);
}

void output_teardown(output_t *output) {
	if (output == NULL)
		return;

	output_t *next = output->link.next != &mon_list ? wl_container_of(output->link.next, next,
		link) : NULL;
	output_t *prev = output->link.prev != &mon_list ? wl_container_of(output->link.prev, prev,
		link) : NULL;
	wl_list_remove(&output->link);

	if (server.focused_output == output)
		output_set_focused(next ? next : prev);

	if (output->frame.link.next)
		wl_list_remove(&output->frame.link);
	if (output->request_state.link.next)
		wl_list_remove(&output->request_state.link);
	if (output->present.link.next)
		wl_list_remove(&output->present.link);
	if (output->destroy.link.next)
		wl_list_remove(&output->destroy.link);

	if (output->repaint_timer) {
		wl_event_source_remove(output->repaint_timer);
		output->repaint_timer = NULL;
	}
	if (output->present_timer) {
		wl_event_source_remove(output->present_timer);
		output->present_timer = NULL;
	}

	wlr_color_transform_unref(output->color_transform);
	output->color_transform = NULL;
	effects_output_fini(output->effects);
	output->effects = NULL;
	dim_destroy_output(output);

	animation_cancel_output(output);
	output_orphan_desktops(output);

	if (output->layer_bg)
		wlr_scene_node_destroy(&output->layer_bg->node);
	if (output->layer_bottom)
		wlr_scene_node_destroy(&output->layer_bottom->node);
	if (output->layer_top)
		wlr_scene_node_destroy(&output->layer_top->node);
	if (output->layer_overlay)
		wlr_scene_node_destroy(&output->layer_overlay->node);
	output->layer_bg = NULL;
	output->layer_bottom = NULL;
	output->layer_top = NULL;
	output->layer_overlay = NULL;

	if (output->wlr_output) {
		struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(server.scene,
			output->wlr_output);
		if (scene_output)
			wlr_scene_output_destroy(scene_output);
		if (wlr_output_layout_get(server.output_layout, output->wlr_output))
			wlr_output_layout_remove(server.output_layout, output->wlr_output);
		output->wlr_output->data = NULL;
	}

	free(output);
}

void set_orphan_active_desk(desktop_t *d) {
	orphan_active_desk = d;
}

void output_remove(output_t *output) {
	if (!output)
		return;

	layer_surface_t *layer, *tmp;

	for (size_t i = 0; i < 4; i++)
		wl_list_for_each_safe(layer, tmp, &output->layers[i], link)
			wlr_layer_surface_v1_destroy(layer->layer_surface);

	if (output->lock_surface)
		destroy_lock_surface(&output->destroy_lock_surface, NULL);

	char name[SMALEN];
	snprintf(name, sizeof(name), "%s", output->name);
	snprintf(last_removed_name, sizeof(last_removed_name), "%s", output->name);

	desktop_t *active_desk = output->desk;
	bool was_focused = server.focused_output == output;

	if (output->enabled)
		output_disable(output);

	output_teardown(output);
	output_recover_after_removal(active_desk, was_focused);

	ipc_put_status(SUB_MASK_MONITOR_REMOVE, "monitor_remove[%s]\n", name);
}

static void handle_output_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	output_t *output = wl_container_of(listener, output, destroy);
	output_remove(output);
}

void output_create(struct wlr_output *wlr_output) {
	output_t *output = calloc(1, sizeof(*output));
	if (!output)
		return;

	output->wlr_output = wlr_output;
	wlr_output_init_render(wlr_output, server.allocator, server.renderer);

	// init output info
	output->enabled = false;
	output->allow_tearing = false;
	output->max_render_time = -1;
	strncpy(output->name, wlr_output->name, SMALEN - 1);
	output->name[SMALEN - 1] = 0;
	if (wlr_output_is_headless(wlr_output)) {
		server.headless_output_counter++;
		snprintf(output->name, SMALEN, "HEADLESS-%u", server.headless_output_counter);
		wlr_output_set_name(wlr_output, output->name);
	}
	output->id = next_monitor_id++;
	output->rectangle = (struct wlr_box){
		0,
		0,
		1920,
		1080
	};

	// restore orphaned desktops or create default workspace
	bool restored = false;
	wl_list_init(&output->desk_list);
	if (!wl_list_empty(&orphan_desk_list)) {
		restored = true;
		desktop_t *d, *dtmp;
		wl_list_for_each_safe(d, dtmp, &orphan_desk_list, link) {
			wl_list_remove(&d->link);
			wl_list_insert(output->desk_list.prev, &d->link);
			d->output = output;
			desktop_clear_output(d, output);
		}

		output->desk = wl_list_empty(&output->desk_list) ? NULL : wl_container_of(output->desk_list.next,
			output->desk, link);

		if (orphan_active_desk && orphan_active_desk->output == output)
			output->desk = orphan_active_desk;
		orphan_active_desk = NULL;
	} else {
		desktop_t *d = calloc(1, sizeof(desktop_t));
		if (d)
			desktop_init(d, output, output->name);
	}

	// add to monitor linked list
	wl_list_init(&output->link);
	wl_list_insert(mon_list.prev, &output->link);

	if (!server.focused_output)
		output_set_focused(output);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);

	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode)
		wlr_output_state_set_mode(&state, mode);

	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);

	output->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);

	output->present.notify = handle_output_present;
	wl_signal_add(&wlr_output->events.present, &output->present);

	output->repaint_timer = wl_event_loop_add_timer(wl_display_get_event_loop(server.wl_display),
		output_repaint_timer_handler, output);
	output->present_timer = wl_event_loop_add_timer(wl_display_get_event_loop(server.wl_display),
		output_present_timeout, output);

	output->destroy.notify = handle_output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	for (int i = 0; i < 4; i++)
		wl_list_init(&output->layers[i]);

	output->layer_bg = wlr_scene_tree_create(server.bg_tree);
	output->layer_bottom = wlr_scene_tree_create(server.bot_tree);
	output->layer_top = wlr_scene_tree_create(server.top_tree);
	output->layer_overlay = wlr_scene_tree_create(server.over_tree);

	struct wlr_output_layout_output *l_output = wlr_output_layout_add_auto(server.output_layout,
		wlr_output);
	struct wlr_scene_output *scene_output = wlr_scene_output_create(server.scene, wlr_output);
	wlr_scene_output_layout_add_output(server.scene_layout, l_output, scene_output);

	wlr_output->data = output;

	struct wlr_box layout_box;
	wlr_output_layout_get_box(server.output_layout, wlr_output, &layout_box);
	output->rectangle = layout_box;
	output->usable_area = layout_box;
	output->effects = effects_output_init(output->rectangle.width, output->rectangle.height);

	output_enable(output);

	// apply the stored output config now, so hotplugged outputs come up configured
	struct output_config *oc = output_config_find(output->name);
	if (oc)
		output_config_apply(oc);

	if (oc && oc->enable != OUTPUT_CONFIG_DISABLE && !wlr_output->enabled)
		wlr_log(WLR_ERROR, "Output %s is disabled after applying its config", output->name);

	workspace_sync();

	ipc_put_status(SUB_MASK_MONITOR_ADD, "monitor_add[%s]\n", output->name);

	bool reconnected = streq(output->name, last_removed_name);
	if ((restored || reconnected) && output->enabled && output->desk) {
		output_set_focused(output);
		workspace_switch_to_desktop(output->desk->name);
	}

	output_schedule_frame(output);

	wlr_log(WLR_INFO, "Output %s ready: enabled=%d wlr_enabled=%d rect=%d,%d %dx%d restored=%d "
		"reconnected=%d focused=%s desk=%s", output->name, output->enabled, wlr_output->enabled,
			output->rectangle.x, output->rectangle.y, output->rectangle.width, output->rectangle.height,
			restored, reconnected, server.focused_output ? server.focused_output->name : "none",
			output->desk ? output->desk->name : "none");

	output_update_manager_config();
}

void output_enable(output_t *output) {
	if (output->present_timer)
		wl_event_source_timer_update(output->present_timer, 0);
	output->awaiting_present = false;
	if (output->enabled)
		return;

	output->enabled = true;

	if (output->wlr_output) {
		struct wlr_box layout_box;
		wlr_output_layout_get_box(server.output_layout, output->wlr_output, &layout_box);
		output->rectangle = layout_box;
	}

	output->lx = output->rectangle.x;
	output->ly = output->rectangle.y;
	output->width = output->rectangle.width;
	output->height = output->rectangle.height;

	output->scale_filter_mode = SCALE_FILTER_NEAREST;

	if (!server.focused_output)
		output_set_focused(output);

	output_update_usable_area(output);

	if (server.workspace_manager && !wl_list_empty(&server.workspace_manager->groups)) {
		struct wlr_ext_workspace_group_handle_v1 *group =
			wl_container_of(server.workspace_manager->groups.next, group, link);
		wlr_ext_workspace_group_handle_v1_output_enter(group, output->wlr_output);
	}
}

void output_disable(output_t *output) {
	if (!output || !output->enabled)
		return;

	output->enabled = false;
	output->awaiting_present = false;
	if (output->present_timer)
		wl_event_source_timer_update(output->present_timer, 0);

	if (server.workspace_manager && !wl_list_empty(&server.workspace_manager->groups)) {
		struct wlr_ext_workspace_group_handle_v1 *group =
			wl_container_of(server.workspace_manager->groups.next, group, link);
		wlr_ext_workspace_group_handle_v1_output_leave(group, output->wlr_output);
	}

	if (server.focused_output != output)
		return;

	output_t *survivor = output_get_valid();
	output_set_focused(survivor);

	if (!survivor) {
		if (server.seat)
			wlr_seat_keyboard_notify_clear_focus(server.seat);
		return;
	}

	if (!survivor->desk && !wl_list_empty(&survivor->desk_list))
		survivor->desk = wl_container_of(survivor->desk_list.next, survivor->desk, link);
	if (!survivor->desk)
		return;

	workspace_update_visibility(survivor, survivor->desk);
	arrange(survivor, survivor->desk, true);

	node_t *target = survivor->desk->focus != NULL ? survivor->desk->focus :
		desktop_fallback_focus(survivor->desk, NULL);
	if (target != NULL) {
		survivor->desk->focus = target;
		focus_node(survivor, survivor->desk, target);
	} else if (server.seat) {
		wlr_seat_keyboard_notify_clear_focus(server.seat);
	}
}

output_t *output_from_wlr_output(struct wlr_output *wlr_output) {
	if (!wlr_output)
		return NULL;

	return wlr_output->data;
}

output_t *output_get_in_direction(output_t *reference, const uint32_t direction) {
	if (!reference || !direction)
		return NULL;

	struct wlr_box output_box;
	wlr_output_layout_get_box(server.output_layout, reference->wlr_output, &output_box);

	int lx = output_box.x + output_box.width / 2;
	int ly = output_box.y + output_box.height / 2;

	struct wlr_output *wlr_adjacent = wlr_output_layout_adjacent_output(server.output_layout, direction,
		reference->wlr_output, lx, ly);

	if (!wlr_adjacent)
		return NULL;

	output_t *adjacent = output_from_wlr_output(wlr_adjacent);
	return adjacent && adjacent->enabled ? adjacent : NULL;
}

void output_update_usable_area(output_t *output) {
	if (!output || !output->enabled)
		return;

	output->usable_area.x = 0;
	output->usable_area.y = 0;
	output->usable_area.width = output->width;
	output->usable_area.height = output->height;
}

void output_update_geometry(output_t *output) {
	if (!output || !output->wlr_output)
		return;

	struct wlr_box layout_box;
	wlr_output_layout_get_box(server.output_layout, output->wlr_output, &layout_box);

	output->rectangle = layout_box;
	output->usable_area = layout_box;
	output->lx = layout_box.x;
	output->ly = layout_box.y;
	output->width = layout_box.width;
	output->height = layout_box.height;

	dim_update_output(output);
	arrange_layers(output);

	desktop_t *d;
	wl_list_for_each(d, &output->desk_list, link)
		arrange(output, d, true);

	update_idle_inhibitors(NULL);
	output_schedule_frame(output);
}

void output_update_scale(output_t *output, const float scale) {
	if (!output || !output->wlr_output)
		return;

	const struct wlr_output *wlr_output = output->wlr_output;

	wlr_log(WLR_INFO, "Updating output '%s' scale to %.2f", wlr_output->name, scale);

	view_t *view;
	wl_list_for_each(view, &server.views, link) {
		struct wlr_surface *surface = view_wlr_surface(view);
		if (!surface || !view->node)
			continue;

		if (view->node->output && view->node->output == output) {
			wlr_fractional_scale_v1_notify_scale(surface, scale);
			wlr_surface_set_preferred_buffer_scale(surface, ceil(scale));
		}
	}

	// notify all layer surfaces on this output
	for (int i = 0; i < 4; i++) {
		layer_surface_t *layer;
		wl_list_for_each(layer, &output->layers[i], link) {
			if (!layer->layer_surface || !layer->layer_surface->surface)
				continue;

			struct wlr_surface *surface = layer->layer_surface->surface;
			wlr_log(WLR_DEBUG, "Notifying layer surface of scale %.2f", scale);
			wlr_fractional_scale_v1_notify_scale(surface, scale);
			wlr_surface_set_preferred_buffer_scale(surface, ceil(scale));
		}
	}

	effects_invalidate_mica(output->effects);

	output_update_geometry(output);
}

output_t *output_get_valid(void) {
	output_t *m;
	wl_list_for_each(m, &mon_list, link)
		if (m->enabled && m->wlr_output)
			return m;

	return NULL;
}

void output_set_focused(output_t *m) {
	server.focused_output = m;
}

output_t *output_focused(void) {
	return server.focused_output;
}

output_t *output_at(const double x, const double y) {
	output_t *m;
	wl_list_for_each(m, &mon_list, link)
		if (m->enabled && m->wlr_output && m->wlr_output->enabled && wlr_box_contains_point(&m->rectangle,
			(int)x, (int)y))
			return m;

	return NULL;
}

output_t *find_output_by_name(const char *name) {
	output_t *m;
	wl_list_for_each(m, &mon_list, link)
		if (streq(m->name, name))
			return m;
	return NULL;
}
