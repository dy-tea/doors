#include "client.h"
#include "dim.h"
#include "layout/layout.h"
#include "output/output.h"
#include "server.h"
#include "settings.h"
#include "tree.h"
#include "types.h"
#include <wayland-util.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

static void dim_get_color(float out[4]) {
	float alpha = settings.dim_color[3] * settings.dim_strength;
	for (int i = 0; i < 3; i++)
		out[i] = settings.dim_color[i] * alpha;
	out[3] = alpha;
}

static struct wlr_scene_rect *dim_ensure(struct wlr_scene_rect **slot,
		struct wlr_scene_tree *parent) {
	if (*slot != NULL)
		return *slot;

	float color[4];
	dim_get_color(color);

	*slot = wlr_scene_rect_create(parent, 0, 0, color);
	if (*slot == NULL)
		wlr_log(WLR_ERROR, "Failed to create dim scrim");

	return *slot;
}

static void dim_place_before(struct wlr_scene_node *node, struct wlr_scene_node *sibling) {
	if (node->parent != sibling->parent)
		wlr_scene_node_reparent(node, sibling->parent);

	struct wl_list *elm = &node->link;
	struct wl_list *pos = &sibling->link;

	wl_list_remove(elm);
	elm->prev = pos->prev;
	elm->next = pos;
	pos->prev->next = elm;
	pos->prev = elm;
}

static bool dim_can_dim(view_t *view) {
	if (view == NULL || view->client == NULL || view->scene_tree == NULL)
		return false;
	if (!view->mapped || view->client->flags.freed)
		return false;

	return !view->client->flags.no_dim;
}

static bool view_occupied_box(view_t *view, struct wlr_box *out) {
	client_t *client = view->client;

	if (client->state == STATE_FULLSCREEN) {
		output_t *m = view->node ? view->node->output : NULL;
		if (m == NULL)
			return false;
		*out = m->rectangle;
		return out->width > 0 && out->height > 0;
	}

	// floating and minimized toplevels keep their own rectangle
	*out = IS_FLOATING(client) ||
		client->flags.minimized ? client->floating_rectangle : client->tiled_rectangle;

	return out->width > 0 && out->height > 0;
}

void dim_update_view(view_t *view) {
	if (view == NULL || view->scene_tree == NULL)
		return;

	struct wlr_scene_rect *scrim = view->dim_node;

	if (!settings.dim_below || !dim_can_dim(view)) {
		if (scrim != NULL)
			wlr_scene_node_set_enabled(&scrim->node, false);
		return;
	}

	struct wlr_box box;
	if (!view_occupied_box(view, &box)) {
		if (scrim != NULL)
			wlr_scene_node_set_enabled(&scrim->node, false);
		return;
	}

	scrim = dim_ensure(&view->dim_node, view->scene_tree);
	if (scrim == NULL)
		return;

	float color[4];
	dim_get_color(color);
	wlr_scene_rect_set_color(scrim, color);
	wlr_scene_rect_set_size(scrim, box.width, box.height);

	wlr_scene_node_set_position(&scrim->node, 0, 0);
	dim_place_before(&scrim->node, &view->content_tree->node);
	wlr_scene_node_set_enabled(&scrim->node, true);
}

void dim_destroy_view(view_t *view) {
	if (view == NULL || view->dim_node == NULL)
		return;

	wlr_scene_node_destroy(&view->dim_node->node);
	view->dim_node = NULL;
}

void dim_update_output(output_t *output) {
	if (output == NULL)
		return;

	struct wlr_scene_rect *scrim = output->dim_node;

	view_t *focused = NULL;
	if (output->desk != NULL && output->desk->focus != NULL && output->desk->focus->client != NULL)
		focused = output->desk->focus->client->view;

	if (!settings.dim_inactive || !dim_can_dim(focused)) {
		if (scrim != NULL)
			wlr_scene_node_set_enabled(&scrim->node, false);
		return;
	}

	if (!focused->scene_tree || focused->scene_tree->node.parent == NULL) {
		if (scrim != NULL)
			wlr_scene_node_set_enabled(&scrim->node, false);
		return;
	}

	scrim = dim_ensure(&output->dim_node, focused->scene_tree->node.parent);
	if (scrim == NULL)
		return;

	const struct wlr_box *rect = &output->rectangle;
	if (rect->width <= 0 || rect->height <= 0) {
		wlr_scene_node_set_enabled(&scrim->node, false);
		return;
	}

	float color[4];
	dim_get_color(color);
	wlr_scene_rect_set_color(scrim, color);
	wlr_scene_rect_set_size(scrim, rect->width, rect->height);
	wlr_scene_node_set_position(&scrim->node, rect->x, rect->y);

	dim_place_before(&scrim->node, &focused->scene_tree->node);
	wlr_scene_node_set_enabled(&scrim->node, true);
}

void dim_destroy_output(output_t *output) {
	if (output == NULL || output->dim_node == NULL)
		return;

	wlr_scene_node_destroy(&output->dim_node->node);
	output->dim_node = NULL;
}

void dim_update(void) {
	output_t *m;
	wl_list_for_each(m, &mon_list, link)
		dim_update_output(m);

	view_t *view;
	wl_list_for_each(view, &server.views, link)
		dim_update_view(view);
}

bool dim_output_visible(const output_t *output) {
	return output != NULL && output->dim_node != NULL && output->dim_node->node.enabled;
}
