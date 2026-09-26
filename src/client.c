#include "client.h"
#include "toplevel.h"
#include "types.h"
#include "xwayland.h"

#define CLIENT_DISPATCH(client, field) \
	((client)->toplevel ? (client)->toplevel->field : \
	 (client)->xwayland_view ? (client)->xwayland_view->field : NULL)

struct wlr_scene_tree *client_get_scene_tree(client_t *client) {
	if (!client)
		return NULL;
	return CLIENT_DISPATCH(client, scene_tree);
}

struct wlr_scene_tree *client_get_content_tree(client_t *client) {
	if (!client)
		return NULL;
	return CLIENT_DISPATCH(client, content_tree);
}

struct wlr_scene_tree *client_border_tree(client_t *client) {
	if (!client)
		return NULL;
	return CLIENT_DISPATCH(client, border_tree);
}

struct wlr_scene_rect **client_border_rects(client_t *client) {
	if (!client)
		return NULL;
	return CLIENT_DISPATCH(client, border_rects);
}

surface_rounded_t *client_get_rounded(client_t *client) {
	if (!client)
		return NULL;
	return CLIENT_DISPATCH(client, rounded);
}

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
	if (!client)
		return NULL;
	return CLIENT_DISPATCH(client, node);
}

output_t *client_get_output(client_t *client) {
	node_t *n = client_get_node(client);
	return n ? n->output : NULL;
}
