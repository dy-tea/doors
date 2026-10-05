#include "client.h"
#include "server.h"
#include "tree.h"
#include "types.h"
#include "view.h"
#include <string.h>

struct wlr_scene_tree *client_get_scene_tree(client_t *client) {
	return client && client->view ? client->view->scene_tree : NULL;
}

struct wlr_scene_tree *client_get_content_tree(client_t *client) {
	return client && client->view ? client->view->content_tree : NULL;
}

struct wlr_scene_tree *client_border_tree(client_t *client) {
	return client && client->view ? client->view->border_tree : NULL;
}

struct wlr_scene_rect **client_border_rects(client_t *client) {
	return client && client->view ? client->view->border_rects : NULL;
}

surface_rounded_t *client_get_rounded(client_t *client) {
	return client && client->view ? client->view->rounded : NULL;
}

surface_blur_t **client_blur_slot(client_t *client) {
	return client && client->view ? &client->view->blur : NULL;
}

surface_rounded_t **client_rounded_slot(client_t *client) {
	return client && client->view ? &client->view->rounded : NULL;
}

surface_shadow_t **client_shadow_slot(client_t *client) {
	return client && client->view ? &client->view->shadow : NULL;
}

// Whether the client's view is currently mapped.
bool client_is_mapped(const client_t *client) {
	return client && client->view && client->view->mapped;
}

struct wlr_surface *client_wlr_surface(client_t *client) {
	return client ? view_wlr_surface(client->view) : NULL;
}

struct wlr_ext_foreign_toplevel_handle_v1 *client_get_ext_foreign_toplevel(const client_t *client) {
	return client && client->view ? client->view->ext_foreign_toplevel : NULL;
}

struct wlr_foreign_toplevel_handle_v1 *client_get_foreign_toplevel(const client_t *client) {
	return client && client->view ? client->view->foreign_toplevel : NULL;
}

// flips a client between visible and hidden in the scene graph
void client_set_visible(client_t *client, bool show) {
	if (client == NULL)
		return;

	if (show && client->flags.minimized)
		return;

	client->flags.shown = show;

	struct wlr_scene_tree *st = client_get_scene_tree(client);
	if (st)
		wlr_scene_node_set_enabled(&st->node, show);
}

node_t *client_get_node(client_t *client) {
	return client && client->view ? client->view->node : NULL;
}

void client_set_title(client_t *client, const char *title) {
	if (client == NULL || title == NULL)
		return;

	strncpy(client->title, title, MAXLEN - 1);
	client->title[MAXLEN - 1] = '\0';
}

void client_set_app_id(client_t *client, const char *app_id) {
	if (client == NULL || app_id == NULL)
		return;

	strncpy(client->app_id, app_id, MAXLEN - 1);
	client->app_id[MAXLEN - 1] = '\0';
}

output_t *client_get_output(client_t *client) {
	node_t *n = client_get_node(client);
	return n ? n->output : NULL;
}

struct wlr_scene_tree *client_state_tree(const client_t *client) {
	if (client == NULL)
		return NULL;

	if (client->state == STATE_FULLSCREEN)
		return scene_stack_tree(STACK_FULLSCREEN);

	// minimized and scratchpad toplevels are parked in the floating tree
	return IS_FLOATING(client) ||
		client->flags.minimized ? scene_stack_tree(STACK_FLOATING) : scene_stack_tree(STACK_TILED);
}

struct wlr_scene_tree *client_layer_tree(const client_t *client) {
	struct wlr_scene_tree *tree = client_state_tree(client);
	if (client == NULL || client->state == STATE_FULLSCREEN)
		return tree;

	switch (client->layer) {
	case CLIENT_LAYER_BELOW:
		return scene_stack_tree(STACK_BOTTOM);
	case CLIENT_LAYER_ABOVE:
		return scene_stack_tree(STACK_TOP);
	case CLIENT_LAYER_NORMAL:
		break;
	}

	return tree;
}

void client_apply_layer(client_t *client) {
	struct wlr_scene_tree *scene_tree = client_get_scene_tree(client);
	struct wlr_scene_tree *parent = client_layer_tree(client);
	if (scene_tree == NULL || parent == NULL)
		return;
	if (scene_tree->node.parent == parent)
		return;

	wlr_scene_node_reparent(&scene_tree->node, parent);
}
