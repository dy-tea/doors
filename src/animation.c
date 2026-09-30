#include "animation.h"
#include "bezier.h"
#include "once.h"
#include "output/output.h"
#include "protocol/layer.h"
#include "spring.h"
#include "surface.h"
#include "tree.h"
#include "types.h"
#include "view.h"
#include <pixman.h>
#include <stdio.h>
#include <stdlib.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>

#define SPRING_FIXED_DT 0.001
#define SPRING_MAX_STEPS 10

typedef enum {
	ANIM_KIND_NONE = 0,
	ANIM_KIND_RESIZE,
	ANIM_KIND_GEOMETRY,
	ANIM_KIND_WORKSPACE_SLIDE,
	ANIM_KIND_FADE_IN,
	ANIM_KIND_FADE_OUT,
} animation_kind_t;

typedef struct {
	struct wl_list link;
	animation_kind_t kind;
	bool slide_out;
	node_t *node;
	view_t *view;
	struct wlr_scene_tree *scene_tree;
	struct wlr_scene_tree *saved_tree;
	struct wlr_box from;
	struct wlr_box to;
	struct timespec start;
	uint32_t duration_ms;
	output_t *output;
	float from_opacity, to_opacity;
	bool use_spring;
	double spring_position;
	double spring_velocity;
	double spring_accumulator;
	struct timespec spring_last_tick;
	bool spring_done;
	char curve_name[64];
	double eased;
	double progress;
} animation_entry_t;

static struct wl_list animations;
static uint32_t ANIMATION_DURATION_MS = 180;
static char default_bezier_name[64] = "default";

#define ANIM_TYPE_COUNT 7
static const char *anim_type_names[ANIM_TYPE_COUNT] = {
	"geometry",
	"resize",
	"fade_in",
	"fade_out",
	"fade_in_layer",
	"fade_out_layer",
	"workspace_slide"
};

typedef struct {
	char bezier_name[64];
	uint32_t duration_ms;
	char spring_name[64];
	bool enabled;
} animation_type_config_t;

static animation_type_config_t anim_type_configs[ANIM_TYPE_COUNT];

static void apply_config_to_entry(animation_entry_t *entry, int type_index) {
	if (type_index >= 0 && type_index < ANIM_TYPE_COUNT) {
		animation_type_config_t *cfg = &anim_type_configs[type_index];

		// spring takes priority over bezier
		if (cfg->spring_name[0] != '\0' && spring_exists(cfg->spring_name)) {
			entry->use_spring = true;
			entry->spring_position = 0.0;
			entry->spring_velocity = 0.0;
			entry->spring_done = false;
			entry->spring_accumulator = 0.0;
			clock_gettime(CLOCK_MONOTONIC, &entry->spring_last_tick);
			snprintf(entry->curve_name, sizeof(entry->curve_name), "%s", cfg->spring_name);
			return;
		}

		if (cfg->bezier_name[0] != '\0' && bezier_exists(cfg->bezier_name))
			snprintf(entry->curve_name, sizeof(entry->curve_name), "%s", cfg->bezier_name);

		if (cfg->duration_ms > 0)
			entry->duration_ms = cfg->duration_ms;
	}
}

int animation_type_from_name(const char *name) {
	for (int i = 0; i < ANIM_TYPE_COUNT; i++)
		if (strcmp(name, anim_type_names[i]) == 0)
			return i;

	return -1;
}

bool animation_set_type_config(const char *type_name, const char *bezier_name,
		uint32_t duration_ms) {
	int idx = animation_type_from_name(type_name);
	if (idx < 0)
		return false;

	if (bezier_name) {
		if (bezier_name[0] == '\0' || bezier_exists(bezier_name))
			snprintf(anim_type_configs[idx].bezier_name, sizeof(anim_type_configs[idx].bezier_name), "%s",
				bezier_name);
		else
			return false;
	}
	if (duration_ms > 0)
		anim_type_configs[idx].duration_ms = duration_ms;

	return true;
}

const char *animation_type_get_bezier(const char *type_name) {
	int idx = animation_type_from_name(type_name);
	if (idx < 0)
		return NULL;
	return anim_type_configs[idx].bezier_name[0] ? anim_type_configs[idx].bezier_name : NULL;
}

uint32_t animation_type_get_duration(const char *type_name) {
	int idx = animation_type_from_name(type_name);
	if (idx < 0)
		return 0;
	return anim_type_configs[idx].duration_ms;
}

static animation_entry_t *create_animation_entry(void) {
	animation_entry_t *entry = calloc(1, sizeof(*entry));
	if (!entry)
		return NULL;

	entry->from_opacity = 1.0f;
	entry->to_opacity = 1.0f;
	wl_list_insert(&animations, &entry->link);
	wlr_log(WLR_DEBUG, "Created entry %p", (void *)entry);
	return entry;
}

static animation_entry_t *find_animation(node_t *node) {
	animation_entry_t *entry;
	wl_list_for_each(entry, &animations, link)
		if (entry->node == node)
			return entry;

	return NULL;
}

bool animation_is_opacity_fading(view_t *view) {
	if (!view)
		return false;

	animation_entry_t *entry;
	wl_list_for_each(entry, &animations, link) {
		if (entry->view == view && entry->from_opacity != entry->to_opacity)
			return true;
	}

	return false;
}

static double elapsed_ms(struct timespec start, struct timespec now) {
	return (now.tv_sec - start.tv_sec) * 1000.0 + (now.tv_nsec - start.tv_nsec) / 1000000.0;
}

void animation_set_bezier(const char *name) {
	if (name && name[0] != '\0') {
		if (bezier_exists(name)) {
			snprintf(default_bezier_name, sizeof(default_bezier_name), "%s", name);
			wlr_log(WLR_DEBUG, "Default bezier set to '%s'", name);
		} else {
			wlr_log(WLR_ERROR, "No such bezier curve '%s'", name);
		}
	}
}

const char *animation_get_bezier(void) {
	return default_bezier_name;
}

void animation_set_duration(uint32_t ms) {
	if (ms > 0) {
		ANIMATION_DURATION_MS = ms;
		wlr_log(WLR_DEBUG, "Default duration set to %u ms", ms);
	}
}

uint32_t animation_get_duration(void) {
	return ANIMATION_DURATION_MS;
}

static void schedule_output(output_t *output) {
	if (!output || !output->wlr_output || !output->enabled)
		return;

	output_schedule_frame(output);
}

static void tick_entry(animation_entry_t *entry, struct timespec now) {
	if (entry->use_spring) {
		double elapsed = elapsed_ms(entry->spring_last_tick, now) / 1000.0;
		entry->spring_last_tick = now;
		entry->spring_accumulator += elapsed;

		spring_curve_t *curve = spring_find(entry->curve_name);

		if (curve) {
			int remaining = SPRING_MAX_STEPS;
			while (entry->spring_accumulator >= SPRING_FIXED_DT && remaining > 0 && !entry->spring_done) {
				entry->eased = spring_evaluate(curve, SPRING_FIXED_DT, &entry->spring_position,
					&entry->spring_velocity, &entry->spring_done);
				entry->spring_accumulator -= SPRING_FIXED_DT;
				remaining--;
			}
		} else {
			entry->eased = 1.0;
			entry->spring_done = true;
		}
	} else {
		entry->progress = elapsed_ms(entry->start, now) / (double)entry->duration_ms;
		if (entry->progress < 0.0)
			entry->progress = 0.0;
		if (entry->progress > 1.0)
			entry->progress = 1.0;

		const char *bname = entry->curve_name[0] ? entry->curve_name : default_bezier_name;
		entry->eased = bezier_evaluate(bname, entry->progress);
	}
}

static bool is_entry_done(animation_entry_t *entry) {
	if (entry->use_spring)
		return entry->spring_done;

	return entry->progress >= 1.0;
}

bool animation_set_type_spring(const char *type_name, const char *spring_name) {
	int idx = animation_type_from_name(type_name);

	if (idx < 0)
		return false;
	if (!spring_name)
		return false;

	if (spring_name[0] == '\0' || spring_exists(spring_name)) {
		snprintf(anim_type_configs[idx].spring_name, sizeof(anim_type_configs[idx].spring_name), "%s",
			spring_name);
		return true;
	}

	return false;
}

const char *animation_type_get_spring(const char *type_name) {
	int idx = animation_type_from_name(type_name);
	if (idx < 0)
		return NULL;

	return anim_type_configs[idx].spring_name[0] ? anim_type_configs[idx].spring_name : NULL;
}

bool animation_type_set_enabled(const char *type_name, bool enabled) {
	int idx = animation_type_from_name(type_name);
	if (idx < 0)
		return false;
	anim_type_configs[idx].enabled = enabled;
	return true;
}

bool animation_type_get_enabled(const char *type_name) {
	int idx = animation_type_from_name(type_name);
	if (idx < 0)
		return true;
	return anim_type_configs[idx].enabled;
}

void animation_init(void) {
	ONCE();
	wl_list_init(&animations);

	for (int i = 0; i < ANIM_TYPE_COUNT; i++)
		anim_type_configs[i].enabled = true;
}

void animation_fini(void) {
	ONCE();
	animation_entry_t *entry, *tmp;
	wl_list_for_each_safe(entry, tmp, &animations, link) {
		if (entry->from_opacity != entry->to_opacity && entry->scene_tree) {
			surface_set_opacity(&entry->scene_tree->node, entry->to_opacity);
		}
		wl_list_remove(&entry->link);
		free(entry);
	}
}

void animation_cancel_node(node_t *node) {
	animation_entry_t *entry = find_animation(node);
	if (!entry)
		return;

	wlr_log(WLR_DEBUG, "Cancel node %u entry=%p", node ? node->id : 0, (void *)entry);

	if (entry->kind == ANIM_KIND_RESIZE && entry->view && entry->view->content_tree)
		wlr_scene_subsurface_tree_set_clip(&entry->view->content_tree->node, NULL);

	if (entry->from_opacity != entry->to_opacity && entry->scene_tree)
		surface_set_opacity(&entry->scene_tree->node, entry->to_opacity);

	wl_list_remove(&entry->link);
	free(entry);
}

void animation_cancel_view(view_t *view) {
	if (!view)
		return;

	animation_entry_t *entry, *tmp;
	wl_list_for_each_safe(entry, tmp, &animations, link) {
		if (entry->view != view && entry->scene_tree != view->scene_tree &&
			entry->scene_tree != view->content_tree)
			continue;

		wlr_log(WLR_DEBUG, "Cancel toplevel entry=%p node=%u", (void *)entry,
			entry->node ? entry->node->id : 0);

		if (entry->from_opacity != entry->to_opacity && entry->scene_tree) {
			surface_set_opacity(&entry->scene_tree->node, entry->to_opacity);
		}

		wl_list_remove(&entry->link);
		free(entry);
	}
}

void animation_cancel_scene_tree(struct wlr_scene_tree *scene_tree) {
	animation_entry_t *entry, *tmp;
	wl_list_for_each_safe(entry, tmp, &animations, link) {
		if (entry->scene_tree != scene_tree)
			continue;

		if (entry->from_opacity != entry->to_opacity) {
			surface_set_opacity(&entry->scene_tree->node, entry->to_opacity);
		}

		if (entry->saved_tree)
			wlr_scene_node_destroy(&entry->saved_tree->node);

		wl_list_remove(&entry->link);
		free(entry);
	}
}

bool animation_fade_in(view_t *view) {
	if (!view || !view->node || !view->scene_tree || !settings.enable_animations)
		return false;

	if (!anim_type_configs[2].enabled)
		return false;
	if (view->node->client && view->node->client->flags.anim_disabled)
		return false;

	animation_entry_t *entry = find_animation(view->node);
	if (entry) {
		entry->from_opacity = 0.0f;
		entry->to_opacity = view->node->client->opacity;
		entry->view = view;
		entry->node = view->node;
	} else {
		entry = create_animation_entry();
		if (!entry)
			return false;

		entry->node = view->node;
		entry->scene_tree = view->scene_tree;
		entry->output = view->node->output;
		entry->from.x = view->scene_tree->node.x;
		entry->from.y = view->scene_tree->node.y;
		entry->to = entry->from;
		entry->from_opacity = 0.0f;
		entry->to_opacity = view->node->client->opacity;
		clock_gettime(CLOCK_MONOTONIC, &entry->start);
		entry->duration_ms = ANIMATION_DURATION_MS;
		apply_config_to_entry(entry, 2);
	}

	surface_set_opacity(&view->scene_tree->node, 0.0f);

	wlr_log(WLR_DEBUG, "Fade_in entry=%p", (void *)entry);
	schedule_output(view->node->output);
	return true;
}

bool animation_fade_in_layer(layer_surface_t *layer) {
	if (!layer || !layer->scene_tree || !layer->output || !settings.enable_animations)
		return false;

	if (!anim_type_configs[4].enabled)
		return false;

	animation_entry_t *entry = create_animation_entry();
	if (!entry)
		return false;

	entry->node = NULL;
	entry->view = NULL;
	entry->scene_tree = layer->scene_tree;
	entry->output = layer->output;
	entry->from_opacity = 0.0f;
	entry->to_opacity = 1.0f; // possibly have opacity field in future
	clock_gettime(CLOCK_MONOTONIC, &entry->start);
	entry->duration_ms = ANIMATION_DURATION_MS;
	apply_config_to_entry(entry, 4);

	surface_set_opacity(&layer->scene_tree->node, 0.0f);

	wlr_log(WLR_DEBUG, "Fade_in_layer entry=%p", (void *)entry);
	schedule_output(entry->output);
	return true;
}

bool animation_fade_out(view_t *view) {
	if (!view || !view->scene_tree || !view->node || !view->node->output ||
		!settings.enable_animations)
		return false;

	if (!anim_type_configs[3].enabled)
		return false;
	if (view->node->client && view->node->client->flags.anim_disabled)
		return false;

	animation_entry_t *entry = create_animation_entry();
	if (!entry)
		return false;

	entry->node = NULL;
	entry->view = view;
	entry->scene_tree = view->scene_tree;
	entry->output = view->node->output;
	entry->from_opacity = view->node->client->opacity;
	entry->to_opacity = 0.0f;
	clock_gettime(CLOCK_MONOTONIC, &entry->start);
	entry->duration_ms = ANIMATION_DURATION_MS;
	apply_config_to_entry(entry, 3);

	wlr_log(WLR_DEBUG, "Fade_out entry=%p", (void *)entry);
	schedule_output(entry->output);
	return true;
}

bool animation_fade_out_layer(layer_surface_t *layer) {
	if (!layer || !layer->saved_tree || !layer->output || !settings.enable_animations)
		return false;

	if (!anim_type_configs[5].enabled)
		return false;

	animation_entry_t *entry = create_animation_entry();
	if (!entry)
		return false;

	entry->node = NULL;
	entry->view = NULL;
	entry->scene_tree = layer->saved_tree;
	entry->output = layer->output;
	entry->from_opacity = 1.0f; // possibly have opacity field in future
	entry->to_opacity = 0.0f;
	entry->saved_tree = layer->saved_tree;
	clock_gettime(CLOCK_MONOTONIC, &entry->start);
	entry->duration_ms = ANIMATION_DURATION_MS;
	apply_config_to_entry(entry, 5);

	wlr_log(WLR_DEBUG, "Fade_out_layer entry=%p saved_tree=%p", (void *)entry,
		(void *)layer->saved_tree);
	schedule_output(entry->output);
	return true;
}

bool animation_workspace_switch_active(output_t *output) {
	animation_entry_t *entry;
	wl_list_for_each(entry, &animations, link)
		if (entry->kind == ANIM_KIND_WORKSPACE_SLIDE && entry->output == output)
			return true;

	return false;
}

bool animation_node_workspace_slide_out(node_t *node) {
	animation_entry_t *entry = find_animation(node);
	return entry && entry->kind == ANIM_KIND_WORKSPACE_SLIDE && entry->slide_out;
}

bool animation_start_workspace_slide(output_t *output, node_t *node,
		struct wlr_scene_tree *scene_tree, struct wlr_box from, struct wlr_box to, bool slide_out) {
	if (!node || !scene_tree || !output || !settings.enable_animations)
		return false;

	if (!anim_type_configs[6].enabled)
		return false;
	if (node->client && node->client->flags.anim_disabled)
		return false;

	animation_entry_t *entry = find_animation(node);
	if (entry) {
		entry->output = output;
		entry->from.x = scene_tree->node.x;
		entry->from.y = scene_tree->node.y;
		entry->from.width = to.width;
		entry->from.height = to.height;
		entry->to = to;
		entry->kind = ANIM_KIND_WORKSPACE_SLIDE;
		entry->slide_out = slide_out;
		entry->eased = 0.0;
		entry->progress = 0.0;
		clock_gettime(CLOCK_MONOTONIC, &entry->start);
		entry->duration_ms = ANIMATION_DURATION_MS;
		apply_config_to_entry(entry, 6);
		entry->from_opacity = node->client->opacity;
		entry->to_opacity = node->client->opacity;
		schedule_output(output);
		wlr_log(WLR_DEBUG, "Workspace_slide update entry=%p node=%u from=(%d,%d) to=(%d,%d)",
			(void *)entry, node->id, entry->from.x, entry->from.y, to.x, to.y);
		return true;
	}

	entry = create_animation_entry();
	if (!entry)
		return false;

	entry->node = node;
	entry->scene_tree = scene_tree;
	entry->output = output;
	entry->from = from;
	entry->to = to;
	entry->kind = ANIM_KIND_WORKSPACE_SLIDE;
	entry->slide_out = slide_out;
	clock_gettime(CLOCK_MONOTONIC, &entry->start);
	entry->duration_ms = ANIMATION_DURATION_MS;
	apply_config_to_entry(entry, 6);
	entry->from_opacity = node->client->opacity;
	entry->to_opacity = node->client->opacity;

	wlr_scene_node_set_position(&scene_tree->node, entry->from.x, entry->from.y);
	schedule_output(output);
	wlr_log(WLR_DEBUG, "Workspace_slide entry=%p node=%u from=(%d,%d) to=(%d,%d)", (void *)entry,
		node->id, from.x, from.y, to.x, to.y);
	return true;
}

static void update_resize_entry(animation_entry_t *entry);

static bool anim_entry_box(const animation_entry_t *entry, struct wlr_box *out) {
	if (!entry)
		return false;

	double e = entry->eased;
	struct wlr_box r = {
		.x = (int)(entry->from.x + (entry->to.x - entry->from.x) * e),
		.y = (int)(entry->from.y + (entry->to.y - entry->from.y) * e),
		.width = (int)(entry->from.width + (entry->to.width - entry->from.width) * e),
		.height = (int)(entry->from.height + (entry->to.height - entry->from.height) * e),
	};
	if (r.width < 1)
		r.width = 1;
	if (r.height < 1)
		r.height = 1;

	int from_right = entry->from.x + entry->from.width;
	int to_right = entry->to.x + entry->to.width;
	if (entry->from.x == entry->to.x)
		r.x = entry->from.x;
	else if (from_right == to_right)
		r.x = from_right - r.width;

	int from_bottom = entry->from.y + entry->from.height;
	int to_bottom = entry->to.y + entry->to.height;
	if (entry->from.y == entry->to.y)
		r.y = entry->from.y;
	else if (from_bottom == to_bottom)
		r.y = from_bottom - r.height;

	*out = r;
	return true;
}

bool animation_start_resize(view_t *view, struct wlr_box from, struct wlr_box to) {
	if (!view || !view->scene_tree || !view->content_tree || !view->node ||
		!settings.enable_animations)
		return false;

	if (!anim_type_configs[1].enabled)
		return false;
	if (view->node->client && view->node->client->flags.anim_disabled)
		return false;

	// skip if the size didn't actually change
	if (from.width == to.width && from.height == to.height)
		return false;

	// skip trivially small size changes
	int max_change = from.width > to.width ? from.width - to.width : to.width - from.width;
	int h_change = from.height > to.height ? from.height - to.height : to.height - from.height;
	if (max_change < h_change)
		max_change = h_change;
	if (max_change < 10)
		return false;

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	animation_entry_t *entry = find_animation(view->node);
	if (entry) {
		anim_entry_box(entry, &from);
	} else {
		entry = create_animation_entry();
		if (!entry)
			return false;
	}

	entry->kind = ANIM_KIND_RESIZE;
	entry->node = view->node;
	entry->view = view;
	entry->scene_tree = view->scene_tree;
	entry->output = view->node->output;
	entry->from = from;
	entry->to = to;
	entry->start = now;
	entry->duration_ms = ANIMATION_DURATION_MS;
	entry->from_opacity = view->node->client->opacity;
	entry->to_opacity = view->node->client->opacity;
	entry->eased = 0.0;
	entry->progress = 0.0;
	apply_config_to_entry(entry, 1);

	// position the scene tree at the start of the animation
	wlr_scene_node_set_position(&view->scene_tree->node, from.x, from.y);

	update_resize_entry(entry);

	wlr_log(WLR_DEBUG, "Start resize entry=%p node=%u from=(%d,%d %dx%d) to=(%d,%d %dx%d)",
		(void *)entry, entry->node ? entry->node->id : 0, from.x, from.y, from.width, from.height, to.x,
		to.y, to.width, to.height);

	schedule_output(view->node->output);
	return true;
}

static void update_resize_entry(animation_entry_t *entry) {
	if (!entry->view || !entry->view->content_tree)
		return;

	if (entry->from.width <= 0 || entry->from.height <= 0)
		return;

	struct wlr_box box;
	if (!anim_entry_box(entry, &box))
		return;

	int x = box.x;
	int y = box.y;
	int width = box.width;
	int height = box.height;

	// update scene tree position
	wlr_scene_node_set_position(&entry->view->scene_tree->node, x, y);

	// clip content tree to the animated size
	struct wlr_box clip = {
		.x = entry->view->geometry.x,
		.y = entry->view->geometry.y,
		.width = width,
		.height = height,
	};
	wlr_scene_subsurface_tree_set_clip(&entry->view->content_tree->node, &clip);

	struct wlr_box container = {
		.x = entry->view->geometry.x,
		.y = entry->view->geometry.y,
		.width = width,
		.height = height,
	};
	struct wlr_box content_offset = {0};
	struct wlr_box border_size = {0};
	view_resolve_content_layout(entry->view, container, &content_offset, &border_size);

	if (entry->node && entry->node->client) {
		client_t *c = entry->node->client;
		if (IS_TILED(c) || c->state == STATE_FLOATING || c->state == STATE_FULLSCREEN)
			wlr_scene_node_set_position(&entry->view->content_tree->node, content_offset.x,
				content_offset.y);
	}

	// update borders to follow the animated size
	if (entry->view->border_tree) {
		unsigned int bw = 0;
		if (entry->node && entry->node->client)
			bw = effective_border_width(entry->node->desktop);

		update_borders(entry->view->border_tree, entry->view->border_rects, border_size, bw,
			content_offset.x, content_offset.y);
		update_border_colors(entry->node->client);

		// update rounded corner shader buffer to match animated size
		if (entry->view->rounded) {
			if (entry->view->rounded->border_shader_node && bw > 0) {
				int new_fw = border_size.width + 2 * (int)bw;
				int new_fh = border_size.height + 2 * (int)bw;
				if (new_fw > 0 && new_fh > 0)
					wlr_scene_buffer_set_dest_size(entry->view->rounded->border_shader_node, new_fw, new_fh);
			}
			entry->view->rounded->border_dirty = true;
			entry->view->rounded->corner_mask_dirty = true;
		}
	}
}

bool animation_is_resizing(node_t *node) {
	if (!node)
		return false;
	animation_entry_t *entry = find_animation(node);
	return entry && entry->kind == ANIM_KIND_RESIZE;
}

bool animation_get_toplevel_resize_progress(view_t *view, double *progress,
		struct wlr_box *anim_from, struct wlr_box *anim_to) {
	if (!view || !view->node)
		return false;

	animation_entry_t *entry = find_animation(view->node);
	if (!entry || entry->kind != ANIM_KIND_RESIZE)
		return false;

	if (progress)
		*progress = entry->eased;
	if (anim_from)
		*anim_from = entry->from;
	if (anim_to)
		*anim_to = entry->to;

	return true;
}

bool animation_get_geometry_progress(view_t *view, struct wlr_box *out) {
	if (!view || !view->node)
		return false;

	animation_entry_t *entry = find_animation(view->node);
	if (!entry)
		return false;

	if (entry->kind == ANIM_KIND_RESIZE || entry->kind == ANIM_KIND_GEOMETRY ||
			entry->kind == ANIM_KIND_WORKSPACE_SLIDE) {
		if (out)
			anim_entry_box(entry, out);
		return true;
	}

	return false;
}

bool animation_has_fade_out(struct wlr_scene_tree *scene_tree) {
	if (!scene_tree)
		return false;

	animation_entry_t *entry;
	wl_list_for_each(entry, &animations, link)
		if (entry->scene_tree == scene_tree && entry->to_opacity < entry->from_opacity)
			return true;

	return false;
}

bool animation_apply_geometry(node_t *node, struct wlr_scene_tree *scene_tree, struct wlr_box target,
		bool animate) {
	struct wlr_box from;
	from.x = scene_tree->node.x;
	from.y = scene_tree->node.y;
	from.width = target.width;
	from.height = target.height;
	return animation_apply_geometry_from(node, scene_tree, from, target, animate);
}

bool animation_apply_geometry_from(node_t *node, struct wlr_scene_tree *scene_tree,
		struct wlr_box from, struct wlr_box target, bool animate) {
	if (!node || !scene_tree)
		return false;

	// if the size changes, delegate to the resize animation system
	if ((from.width != target.width || from.height != target.height) && node->client &&
			node->client->view && from.width > 0 && from.height > 0) {
		if (animation_start_resize(node->client->view, from, target))
			return true;
	}

	if (animation_is_resizing(node)) {
		animation_entry_t *entry = find_animation(node);
		if (entry)
			entry->to = target;
		return true;
	}

	output_t *output = node->output;
	if (!animate || !settings.enable_animations || !output || !output->enabled || !node->client ||
			!node->client->flags.shown) {
		animation_cancel_node(node);
		wlr_scene_node_set_position(&scene_tree->node, target.x, target.y);
		return false;
	}

	if (!anim_type_configs[0].enabled) {
		animation_cancel_node(node);
		wlr_scene_node_set_position(&scene_tree->node, target.x, target.y);
		return false;
	}
	if (node->client && node->client->flags.anim_disabled) {
		animation_cancel_node(node);
		wlr_scene_node_set_position(&scene_tree->node, target.x, target.y);
		return false;
	}

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	animation_entry_t *entry = find_animation(node);
	if (entry) {
		double e = entry->eased;
		from.x = (int)(entry->from.x + (entry->to.x - entry->from.x) * e);
		from.y = (int)(entry->from.y + (entry->to.y - entry->from.y) * e);
	} else {
		entry = create_animation_entry();
		if (!entry) {
			wlr_scene_node_set_position(&scene_tree->node, target.x, target.y);
			return false;
		}
	}

	entry->kind = ANIM_KIND_GEOMETRY;
	entry->node = node;
	entry->scene_tree = scene_tree;
	entry->from = from;
	entry->to = target;
	entry->start = now;
	entry->duration_ms = ANIMATION_DURATION_MS;
	entry->from_opacity = node->client->opacity;
	entry->to_opacity = node->client->opacity;
	entry->eased = 0.0;
	entry->progress = 0.0;
	apply_config_to_entry(entry, 0);

	if (entry->from.x == entry->to.x && entry->from.y == entry->to.y) {
		animation_cancel_node(node);
		wlr_scene_node_set_position(&scene_tree->node, target.x, target.y);
		return false;
	}

	wlr_scene_node_set_position(&scene_tree->node, entry->from.x, entry->from.y);
	schedule_output(output);
	return true;
}

static void update_blur_for_slide_animation(output_t *output, animation_entry_t *entry) {
	if (!output || !entry->node || !entry->node->client)
		return;

	view_t *tl = entry->node->client->view;
	if (!tl)
		return;
	if (!tl->blur || blur_count(tl->blur) == 0)
		return;
	if (!tl->blur->blur_buf)
		return;

	client_t *c = entry->node->client;

	double e = entry->eased;
	int x = (int)(entry->from.x + (entry->to.x - entry->from.x) * e);
	int y = (int)(entry->from.y + (entry->to.y - entry->from.y) * e);

	struct wlr_box r;
	if (c->state == STATE_FULLSCREEN)
		r = output->rectangle;
	else if (c->state == STATE_FLOATING)
		r = c->floating_rectangle;
	else
		r = c->tiled_rectangle;

	r.x = x;
	r.y = y;

	if (!pixman_region32_empty(&tl->blur->blur_region)) {
		// blur region lives in the window's own coordinate space
		int sox, soy;
		if (!view_get_surface_offset(tl, &sox, &soy))
			return;
		int n;
		const pixman_box32_t *boxes = pixman_region32_rectangles(&tl->blur->blur_region, &n);
		blur_set_node_count(tl->blur, tl->scene_tree, n);
		blur_set_buffer(tl->blur, tl->blur->blur_buf);
		for (int i = 0; i < n; i++) {
			int br_x = boxes[i].x1;
			int br_y = boxes[i].y1;
			int br_w = boxes[i].x2 - br_x;
			int br_h = boxes[i].y2 - br_y;
			struct wlr_fbox src = {
				.x = sox + br_x,
				.y = soy + br_y,
				.width = br_w,
				.height = br_h
			};
			struct wlr_scene_buffer *node = blur_get(tl->blur, i);
			if (!node)
				break;
			wlr_scene_node_set_position(&node->node, sox + br_x, soy + br_y);
			wlr_scene_buffer_set_source_box(node, &src);
			wlr_scene_buffer_set_dest_size(node, br_w, br_h);
		}
		return;
	}

	blur_set_node_count(tl->blur, tl->scene_tree, 1);
	struct wlr_scene_buffer *node = blur_get(tl->blur, 0);
	if (!node) {
		blur_set_buffer_null(tl->blur);
		return;
	}
	blur_set_buffer(tl->blur, tl->blur->blur_buf);

	// clip the on-screen portion against the output
	int rx = r.x > output->lx ? r.x : output->lx;
	int ry = r.y > output->ly ? r.y : output->ly;
	int rxe = r.x + r.width < output->lx + output->width ? r.x + r.width : output->lx + output->width;
	int rye = r.y + r.height < output->ly + output->height ? r.y + r.height : output->ly +
		output->height;
	if (rxe <= rx || rye <= ry) {
		blur_set_buffer_null(tl->blur);
		return;
	}
	int off_x = rx - r.x;
	int off_y = ry - r.y;
	struct wlr_fbox src = {
		.x = off_x,
		.y = off_y,
		.width = rxe - rx,
		.height = rye - ry
	};
	wlr_scene_node_set_position(&node->node, off_x, off_y);
	wlr_scene_buffer_set_source_box(node, &src);
	wlr_scene_buffer_set_dest_size(node, rxe - rx, rye - ry);
}

void animation_update_slide_blur(output_t *output) {
	if (!output)
		return;

	animation_entry_t *entry;
	wl_list_for_each(entry, &animations, link) {
		if (entry->kind != ANIM_KIND_WORKSPACE_SLIDE)
			continue;
		if (entry->node && entry->node->output != output)
			continue;
		if (!entry->node || !entry->output || entry->output != output)
			continue;

		update_blur_for_slide_animation(output, entry);
	}
}

bool animation_update_output(output_t *output, struct timespec now) {
	bool active = false;
	animation_entry_t *entry, *tmp;

	wl_list_for_each_safe(entry, tmp, &animations, link) {
		if (!entry->node) {
			if (entry->output != output) {
				wlr_log(WLR_DEBUG, "Skip entry=%p output mismatch", (void *)entry);
				continue;
			}

			if (!entry->scene_tree) {
				wl_list_remove(&entry->link);
				free(entry);
				continue;
			}

			tick_entry(entry, now);

			if (is_entry_done(entry)) {
				surface_set_opacity(&entry->scene_tree->node, entry->to_opacity);

				if (entry->to_opacity < entry->from_opacity)
					wlr_scene_node_set_enabled(&entry->scene_tree->node, false);

				if (entry->saved_tree)
					wlr_scene_node_destroy(&entry->saved_tree->node);

				wl_list_remove(&entry->link);
				free(entry);
			} else {
				float opacity = (float)(entry->from_opacity + (entry->to_opacity - entry->from_opacity) *
					entry->eased);
				surface_set_opacity(&entry->scene_tree->node, opacity);
				active = true;
			}
			continue;
		}

		if (!entry->scene_tree || !entry->node->client) {
			wl_list_remove(&entry->link);
			free(entry);
			continue;
		}

		if (entry->node->output != output)
			continue;

		if (!entry->node->client->flags.shown || !entry->scene_tree->node.enabled) {
			// if this is a resize animation finishing early, clean up the clip
			if (entry->kind == ANIM_KIND_RESIZE && entry->view && entry->view->content_tree)
				wlr_scene_subsurface_tree_set_clip(&entry->view->content_tree->node, NULL);
			wl_list_remove(&entry->link);
			free(entry);
			continue;
		}

		tick_entry(entry, now);

		if (entry->kind == ANIM_KIND_RESIZE) {
			// interpolate position and size, update clip and borders
			update_resize_entry(entry);

			if (is_entry_done(entry)) {
				wlr_scene_node_set_position(&entry->scene_tree->node, entry->to.x, entry->to.y);
				// re-apply centering, borders, and clip immediately so the
				// surface doesn't lose its clip until the next client commit
				if (entry->view)
					view_center_and_clip_surface(entry->view);
				wlr_log(WLR_DEBUG, "Resize complete entry=%p node=%u", (void *)entry,
					entry->node ? entry->node->id : 0);
				wl_list_remove(&entry->link);
				free(entry);
			} else {
				active = true;
			}
		} else {
			// interpolate position only
			int x = (int)(entry->from.x + (entry->to.x - entry->from.x) * entry->eased);
			int y = (int)(entry->from.y + (entry->to.y - entry->from.y) * entry->eased);
			wlr_scene_node_set_position(&entry->scene_tree->node, x, y);

			if (entry->kind == ANIM_KIND_WORKSPACE_SLIDE) {
				update_blur_for_slide_animation(output, entry);
				if (entry->node && entry->node->client && entry->node->client->view) {
					view_t *tl = entry->node->client->view;
					if (tl->rounded && tl->rounded->corner_mask_node) {
						tl->rounded->corner_mask_dirty = true;
						tl->rounded->border_dirty = true;
					}
				}
			}

			if (is_entry_done(entry)) {
				if (entry->from_opacity != entry->to_opacity)
					surface_set_opacity(&entry->scene_tree->node, entry->to_opacity);

				if (entry->kind == ANIM_KIND_WORKSPACE_SLIDE && entry->slide_out && entry->node &&
						entry->node->client) {
					entry->node->client->flags.shown = false;
					wlr_scene_node_set_enabled(&entry->scene_tree->node, false);
					wlr_log(WLR_DEBUG, "Workspace slide-out complete, disabled node=%u", entry->node->id);
				}

				wl_list_remove(&entry->link);
				free(entry);
			} else {
				if (entry->from_opacity != entry->to_opacity) {
					float cur_opacity = (float)(entry->from_opacity + (entry->to_opacity - entry->from_opacity) *
						entry->eased);
					surface_set_opacity(&entry->scene_tree->node, cur_opacity);
				}
				active = true;
			}
		}
	}

	return active;
}
