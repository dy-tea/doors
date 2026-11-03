#include "ipc/ipc.h"
#include "macros.h"
#include "protocol/xdg_toplevel.h"
#include "rule.h"
#include "server.h"
#include "transaction.h"
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_toplevel_tag_v1.h>

static void xdg_toplevel_tag_manager_v1_handle_set_tag(struct wl_listener *listener, void *data) {
	(void)listener;
	const struct wlr_xdg_toplevel_tag_manager_v1_set_tag_event *event = data;
	xdg_toplevel_t *toplevel = event->toplevel->base->data;
	if (!toplevel)
		return;

	free(toplevel->tag);
	toplevel->tag = strdup(event->tag);
	if (!toplevel->tag) {
		wlr_log(WLR_ERROR, "Allocation failed");
		return;
	}

	node_t *n = toplevel->view.node;
	if (n && n->client) {
		const char *app_id = n->client->app_id;
		const char *title = n->client->title;
		find_matching_rule(app_id, title, toplevel->tag);
		ipc_put_status(SUB_MASK_NODE_CHANGE, "node_change[%s,%s,%u,tag]\n",
			app_id && app_id[0] ? app_id : "?", title && title[0] ? title : "?", n->id);
	}

	transaction_commit_dirty();
}

void toplevel_tag_init(void) {
	ONCE();
	struct wlr_xdg_toplevel_tag_manager_v1 *xdg_toplevel_tag_manager_v1 =
		wlr_xdg_toplevel_tag_manager_v1_create(server.wl_display, 1);
	if (!xdg_toplevel_tag_manager_v1) {
		wlr_log(WLR_ERROR, "Failed to create xdg toplevel tag manager");
		exit(EXIT_FAILURE);
	}
	server.xdg_toplevel_tag_manager_v1_set_tag.notify = xdg_toplevel_tag_manager_v1_handle_set_tag;
	wl_signal_add(&xdg_toplevel_tag_manager_v1->events.set_tag,
		&server.xdg_toplevel_tag_manager_v1_set_tag);
}

void toplevel_tag_fini(void) {
	ONCE();
	wl_list_remove(&server.xdg_toplevel_tag_manager_v1_set_tag.link);
}
