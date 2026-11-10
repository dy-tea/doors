#include "animation.h"
#include "effects/effects.h"
#include "input/input_method.h"
#include "input/seat.h"
#include "input/tablet.h"
#include "layout/layout.h"
#include "macros.h"
#include "output/output.h"
#include "protocol/layer.h"
#include "protocol/popup.h"
#include "server.h"
#include "tree.h"
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_damage_ring.h>
#include <wlr/types/wlr_ext_background_effect_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>

extern struct server_t server;

static void layer_surface_destroy(struct wl_listener *listener, void *data);
static void layer_surface_map(struct wl_listener *listener, void *data);
static void layer_surface_unmap(struct wl_listener *listener, void *data);
static void layer_surface_commit(struct wl_listener *listener, void *data);

struct wlr_scene_tree *output_shell_layer(output_t *output, enum zwlr_layer_shell_v1_layer layer);

static bool layer_surface_rule_blur(const layer_surface_t *layer) {
	return (layer->rule.has & LAYER_RULE_BLUR) && layer_rule_flag(&layer->rule, LAYER_RULE_BLUR);
}

static enum zwlr_layer_surface_v1_keyboard_interactivity layer_keyboard_interactivity(const
		layer_surface_t *layer) {
	if (layer->rule.has & LAYER_RULE_KEYBOARD_INTERACTIVITY)
		return layer->rule.keyboard_interactivity;
	return (enum
		zwlr_layer_surface_v1_keyboard_interactivity)layer->layer_surface->current.keyboard_interactive;
}

static enum zwlr_layer_shell_v1_layer layer_effective_layer(const layer_surface_t *layer) {
	if (layer->rule.has & LAYER_RULE_LAYER)
		return layer->rule.layer;
	return layer->layer_surface->current.layer;
}

static void layer_surface_damage_output(layer_surface_t *layer) {
	if (!layer || !layer->output)
		return;

	struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(server.scene,
		layer->output->wlr_output);
	if (!scene_output)
		return;

	pixman_region32_union_rect(&scene_output->damage_ring.current, &scene_output->damage_ring.current,
		0, 0, (unsigned int)layer->output->width, (unsigned int)layer->output->height);
	output_schedule_frame(layer->output);
}

void layer_surface_set_blur(layer_surface_t *ls, bool enabled) {
	if (!ls || !ls->scene_tree)
		return;

	if (enabled && blur_pool_count(&ls->blur_pool) == 0) {
		blur_pool_set_count(&ls->blur_pool, ls->scene_tree, 1);
		if (blur_pool_get(&ls->blur_pool, 0)) {
			ls->blur_region_dirty = true;
			// ensure next frame recaptures the background for this new blur node
			struct wlr_scene_output *so = wlr_scene_get_scene_output(server.scene, ls->output->wlr_output);
			if (so) {
				pixman_region32_union_rect(&so->damage_ring.current, &so->damage_ring.current, 0, 0,
					(unsigned int)ls->output->width, (unsigned int)ls->output->height);
				output_schedule_frame(ls->output);
			}
		}
	} else if (!enabled && blur_pool_count(&ls->blur_pool) > 0) {
		blur_pool_destroy(&ls->blur_pool);
		pixman_region32_clear(&ls->blur_region);
		ls->blur_region_dirty = true;
	}
}

static void layer_surface_save_buffer_iterator(struct wlr_scene_buffer *buffer, int sx, int sy,
		void *data) {
	struct wlr_scene_tree *tree = data;
	if (!buffer || !buffer->buffer)
		return;

	struct wlr_scene_buffer *sbuf = wlr_scene_buffer_create(tree, NULL);
	if (!sbuf)
		return;

	wlr_scene_buffer_set_dest_size(sbuf, buffer->dst_width, buffer->dst_height);
	wlr_scene_buffer_set_opaque_region(sbuf, &buffer->opaque_region);
	wlr_scene_buffer_set_source_box(sbuf, &buffer->src_box);
	wlr_scene_node_set_position(&sbuf->node, sx, sy);
	wlr_scene_buffer_set_transform(sbuf, buffer->transform);
	wlr_scene_buffer_set_buffer(sbuf, buffer->buffer);
}

static void layer_surface_save_buffer(layer_surface_t *layer) {
	if (!layer || !layer->saved_tree)
		return;

	wlr_scene_node_for_each_buffer(&layer->scene_tree->node, layer_surface_save_buffer_iterator,
		layer->saved_tree);
	wlr_log(WLR_DEBUG, "Saved tree children=%d", wl_list_length(&layer->saved_tree->children));
}

static void layer_surface_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	layer_surface_t *layer = wl_container_of(listener, layer, destroy);

	animation_cancel_scene_tree(layer->scene_tree);

	wl_list_remove(&layer->destroy.link);
	wl_list_remove(&layer->new_popup.link);
	wl_list_remove(&layer->map.link);
	wl_list_remove(&layer->unmap.link);
	wl_list_remove(&layer->surface_commit.link);
	wl_list_remove(&layer->link);

	pixman_region32_fini(&layer->blur_region);
	free(layer->blur_pool.nodes);
	layer->blur_pool.nodes = NULL;

	arrange_layers(layer->output);
	free(layer);
}

static void layer_surface_map(struct wl_listener *listener, void *data) {
	(void)data;
	layer_surface_t *layer = wl_container_of(listener, layer, map);

	layer->mapped = true;
	arrange_layers(layer->output);

	if (layer_effective_layer(layer) >= ZWLR_LAYER_SHELL_V1_LAYER_TOP)
		focus_layer_surface(layer);

	effects_invalidate_mica(layer->output->effects);
	if (!animation_fade_in_layer(layer))
		surface_set_opacity(&layer->scene_tree->node, layer->opacity);
}

static void layer_surface_unmap(struct wl_listener *listener, void *data) {
	(void)data;
	layer_surface_t *layer = wl_container_of(listener, layer, unmap);
	layer->mapped = false;

	if (!settings.enable_animations || layer->anim_disabled) {
		animation_cancel_scene_tree(layer->scene_tree);
		wlr_scene_node_set_enabled(&layer->scene_tree->node, false);
	} else {
		struct wlr_scene_node *child;
		wl_list_for_each(child, &layer->scene_tree->children, link)
			wlr_scene_node_set_enabled(child, true);

		layer->saved_tree = wlr_scene_tree_create(&server.scene->tree);
		if (!layer->saved_tree || !animation_fade_out_layer(layer)) {
			if (layer->saved_tree)
				wlr_scene_node_destroy(&layer->saved_tree->node);
			layer->saved_tree = NULL;
			animation_cancel_scene_tree(layer->scene_tree);
			wlr_scene_node_set_enabled(&layer->scene_tree->node, false);
		} else {
			layer_surface_save_buffer(layer);
		}
	}

	arrange_layers(layer->output);
}

static void layer_surface_commit(struct wl_listener *listener, void *data) {
	(void)data;
	layer_surface_t *layer = wl_container_of(listener, layer, surface_commit);
	struct wlr_layer_surface_v1 *layer_surface = layer->layer_surface;

	if (!layer_surface->initialized)
		return;

	bool needs_arrange = false;
	const enum zwlr_layer_shell_v1_layer effective_layer = layer_effective_layer(layer);
	if (layer->layer != effective_layer) {
		wl_list_remove(&layer->link);
		wl_list_insert(&layer->output->layers[effective_layer], &layer->link);
		wlr_scene_node_reparent(&layer->scene_layer->tree->node, output_shell_layer(layer->output,
			effective_layer));
		layer->layer = effective_layer;
		needs_arrange = true;
	}

	// rearrange if needed
	if (needs_arrange || layer_surface->initial_commit) {
		layer->mapped = layer_surface->surface->mapped;
		arrange_layers(layer->output);
	}

	if (layer->blur_from_rule) {
		const bool wants_blur = layer_surface_rule_blur(layer);
		const bool had_blur = blur_pool_count(&layer->blur_pool) > 0;
		if (wants_blur != had_blur)
			layer_surface_set_blur(layer, wants_blur);

		if (wants_blur) {
			const int width = (int)layer_surface->current.actual_width;
			const int height = (int)layer_surface->current.actual_height;
			if (width > 0 && height > 0) {
				pixman_region32_t region;
				pixman_region32_init_rect(&region, 0, 0, width, height);
				if (!pixman_region32_equal(&layer->blur_region, &region)) {
					pixman_region32_copy(&layer->blur_region, &region);
					layer->blur_region_dirty = true;
					layer_surface_damage_output(layer);
				}
				pixman_region32_fini(&region);
			}
		}
	} else {
		// check ext_background_effect_v1 state, the blur region is clipped to the surface size
		const struct wlr_ext_background_effect_surface_v1_state *fx =
			wlr_ext_background_effect_v1_get_surface_state(layer_surface->surface);
		pixman_region32_t blur_region;
		pixman_region32_init(&blur_region);
		if (fx)
			surface_blur_region_clip(&blur_region, &fx->blur_region, (int)layer_surface->current.actual_width,
				(int)layer_surface->current.actual_height);

		bool wants_blur = fx && !pixman_region32_empty(&blur_region);
		bool had_blur = blur_pool_count(&layer->blur_pool) > 0;
		if (wants_blur != had_blur)
			layer_surface_set_blur(layer, wants_blur);

		// update blur region if blur is enabled
		if (blur_pool_count(&layer->blur_pool) > 0 && !pixman_region32_equal(&layer->blur_region,
				&blur_region)) {
			pixman_region32_copy(&layer->blur_region, &blur_region);
			layer->blur_region_dirty = true;
		}
		pixman_region32_fini(&blur_region);
	}

	if (!animation_is_layer_opacity_fading(layer))
		surface_set_opacity(&layer->scene_tree->node, layer->opacity);

	output_schedule_frame(layer->output);
}

void handle_new_layer_surface(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_layer_surface_v1 *layer_surface = data;
	layer_surface_t *layer;
	output_t *output;

	wlr_log(WLR_INFO, "New layer surface on layer %d", layer_surface->pending.layer);

	if (!layer_surface->output) {
		wlr_log(WLR_INFO, "Layer surface has no output, using focused_monitor");
		if (!server.focused_output) {
			wlr_log(WLR_ERROR, "No focused monitor, destroying layer surface on layer %d",
				layer_surface->pending.layer);
			wlr_layer_surface_v1_destroy(layer_surface);
			return;
		}
		layer_surface->output = server.focused_output->wlr_output;
	}

	output = layer_surface->output->data;
	if (!output) {
		wlr_log(WLR_ERROR, "No output data, destroying layer surface on layer %d",
			layer_surface->pending.layer);
		wlr_layer_surface_v1_destroy(layer_surface);
		return;
	}

	layer = calloc(1, sizeof(*layer));
	if (!layer) {
		wlr_layer_surface_v1_destroy(layer_surface);
		return;
	}

	layer->layer_surface = layer_surface;
	layer->output = output;
	layer_surface->data = layer;
	pixman_region32_init(&layer->blur_region);

	layer->opacity = 1.0f;
	layer->layer = layer_surface->pending.layer;

	layer_rule_consequence_t *rule = find_matching_layer_rule(layer_surface->namespace, output->name);
	if (rule) {
		layer->rule = *rule;
		if (rule->has & LAYER_RULE_LAYER)
			layer->layer = rule->layer;
		if (rule->has & LAYER_RULE_OPACITY)
			layer->opacity = rule->opacity;
		layer->anim_disabled = (rule->has & LAYER_RULE_ANIMATIONS_DISABLE) && layer_rule_flag(rule,
			LAYER_RULE_ANIMATIONS_DISABLE);
		layer->blur_from_rule = (rule->has & LAYER_RULE_BLUR) != 0;
	}

	struct wlr_scene_tree *layer_tree = output_shell_layer(output, layer->layer);

	layer->scene_layer = wlr_scene_layer_surface_v1_create(layer_tree, layer_surface);
	layer->scene_tree = layer->scene_layer->tree;

	layer->scene_tree->node.data = layer;

	layer->destroy.notify = layer_surface_destroy;
	wl_signal_add(&layer_surface->events.destroy, &layer->destroy);

	layer->new_popup.notify = handle_new_layer_popup;
	wl_signal_add(&layer_surface->events.new_popup, &layer->new_popup);

	layer->map.notify = layer_surface_map;
	wl_signal_add(&layer_surface->surface->events.map, &layer->map);

	layer->unmap.notify = layer_surface_unmap;
	wl_signal_add(&layer_surface->surface->events.unmap, &layer->unmap);

	layer->surface_commit.notify = layer_surface_commit;
	wl_signal_add(&layer_surface->surface->events.commit, &layer->surface_commit);

	wl_list_insert(&output->layers[layer->layer], &layer->link);

	// fractional scale
	wlr_fractional_scale_v1_notify_scale(layer_surface->surface, layer->output->wlr_output->scale);
	wlr_surface_set_preferred_buffer_scale(layer_surface->surface,
		ceil(layer->output->wlr_output->scale));

	wlr_surface_send_enter(layer_surface->surface, layer_surface->output);

	arrange_layers(output);
}

void arrange_layers(output_t *output) {
	struct wlr_box usable_area = output->rectangle;
	struct wlr_box full_area = output->rectangle;
	layer_surface_t *layer;
	int i;

	if (!output->wlr_output->enabled)
		return;

	for (i = 3; i >= 0; i--) {
		wl_list_for_each_reverse(layer, &output->layers[i], link) {
			if (!layer->layer_surface->initialized)
				continue;
			if (layer->layer_surface->current.exclusive_zone > 0)
				wlr_scene_layer_surface_v1_configure(layer->scene_layer, &full_area, &usable_area);
		}
	}

	if (!wlr_box_equal(&usable_area, &output->usable_area)) {
		output->usable_area = usable_area;
		output_t *m = output;
		if (m && m->desk)
			arrange(m, m->desk, true);
	}

	for (i = 3; i >= 0; i--) {
		wl_list_for_each_reverse(layer, &output->layers[i], link) {
			if (!layer->layer_surface->initialized)
				continue;
			if (layer->layer_surface->current.exclusive_zone <= 0)
				wlr_scene_layer_surface_v1_configure(layer->scene_layer, &full_area, &usable_area);
		}
	}
}

void focus_layer_surface(layer_surface_t *layer_surface) {
	if (!layer_surface->layer_surface || !layer_surface->layer_surface->surface ||
		!layer_surface->layer_surface->surface->mapped)
		return;

	struct wlr_surface *surface = layer_surface->layer_surface->surface;

	if (layer_keyboard_interactivity(layer_surface) ==
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE)
		return;
	if (layer_surface->scene_layer == NULL)
		return;

	// notify keyboard enter
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server.seat);
	if (keyboard)
		wlr_seat_keyboard_notify_enter(server.seat, surface, keyboard->keycodes, keyboard->num_keycodes,
			&keyboard->modifiers);

	seat_t *s = seat_default();
	if (s && s->input_method_relay)
		input_method_relay_set_focus(s->input_method_relay, surface);

	if (s)
		tablet_pads_set_focus(s, surface);
}

struct wlr_scene_tree *output_shell_layer(output_t *output, enum zwlr_layer_shell_v1_layer layer) {
	switch (layer) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
		return output->layer_bg;
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
		return output->layer_bottom;
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
		return output->layer_top;
	case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
		return output->layer_overlay;
	default:
		return output->layer_top;
	}
}

void layer_init(void) {
	ONCE();
	server.layer_shell = wlr_layer_shell_v1_create(server.wl_display, 5);
	if (!server.layer_shell) {
		wlr_log(WLR_ERROR, "Failed to create layer shell");
		exit(EXIT_FAILURE);
	}

	server.new_layer_surface.notify = handle_new_layer_surface;
	wl_signal_add(&server.layer_shell->events.new_surface, &server.new_layer_surface);
}

void layer_fini(void) {
	ONCE();
	wl_list_remove(&server.new_layer_surface.link);
}
