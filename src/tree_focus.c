#include "borders.h"
#include "client.h"
#include "floating.h"
#include "ipc.h"
#include "layout.h"
#include "output.h"
#include "server.h"
#include "settings.h"
#include "tabs.h"
#include "toplevel.h"
#include "tree.h"
#include "tree_focus.h"
#include "types.h"
#include "xwayland.h"
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

bool node_focusable(node_t *n) {
	if (n == NULL || n->client == NULL || n->destroying || n->freed)
		return false;

	// the toplevel behind the node has to still be alive
	return n->client->toplevel != NULL || n->client->xwayland_view != NULL;
}

node_t *desktop_fallback_focus(desktop_t *d, node_t *skip) {
	if (d == NULL)
		return NULL;

	node_t **toplevels = NULL;
	int count = desktop_toplevels(d, &toplevels);
	if (toplevels == NULL)
		return NULL;

	// get topmost floating toplevel
	node_t *best = NULL;
	struct wl_list *link = server.float_tree->children.prev;
	while (link != &server.float_tree->children && best == NULL) {
		struct wlr_scene_node *sn = wl_container_of(link, sn, link);

		for (int i = 0; i < count; i++) {
			node_t *n = toplevels[i];
			if (n == skip || !IS_FLOATING(n->client) || node_is_invisible(n) || !n->client->flags.shown)
				continue;

			struct wlr_scene_tree *st = client_get_scene_tree(n->client);
			if (st != NULL && &st->node == sn)
				best = n;
		}

		link = link->prev;
	}

	// no toplevel above tree, take the first one of it that the user can see
	if (best == NULL && d->root != NULL) {
		FOR_EACH_LEAF(n, d->root) {
			if (n != skip && !node_is_invisible(n)) {
				best = n;
				break;
			}
		}
	}

	free(toplevels);

	return best != NULL && node_focusable(best) ? best : NULL;
}

static bool focus_node_impl(output_t *m, desktop_t *d, node_t *n, bool give_keyboard_focus) {
	if (m == NULL || d == NULL || n == NULL)
		return false;

	d->focus = n;
	output_set_focused(m);

	bool is_current_desktop = (m->desk == d);

	if (!is_current_desktop) {
		if (give_keyboard_focus) {
			ipc_put_status(SUB_MASK_REPORT, NULL);
			ipc_put_status(SUB_MASK_NODE_FOCUS, "node_focus[%s,%s,%u]\n",
				n->client && n->client->app_id[0] ? n->client->app_id : "?",
				n->client && n->client->title[0] ? n->client->title : "?", n->id);
		}
		return true;
	}

	const layout_impl_t *impl = layout_get_impl(d->layout);
	if (impl && impl->on_focus) {
		impl->on_focus(m, d, n);
	} else if (d->root != NULL) {
		FOR_EACH_LEAF(node, d->root)
			if (node->client != NULL && !node_is_minimized(node))
				node->client->flags.shown = true;
	}

	node_t *t = tabbed_ancestor(n);
	if (t != NULL) {
		tabs_update_focus(t, n);
		tabs_set_active_leaf(t, n);
	}

	if (give_keyboard_focus && n != NULL && n->client != NULL) {
		if (n->client->toplevel)
			focus_toplevel(n->client->toplevel);
		else if (n->client->xwayland_view)
			xwayland_view_set_activated(n->client->xwayland_view, true);
	}

	// update border colors for all visible clients on this desktop
	if (d->root != NULL) {
		FOR_EACH_LEAF(node, d->root) {
			if (node->client == NULL)
				continue;

			update_border_colors(node->client);
		}
	}

	// pointer follows focus (only when giving keyboard focus)
	if (give_keyboard_focus && settings.pointer_follows_focus && n != NULL && n->client != NULL &&
			!server.focus_from_click) {
		int center_x = n->rectangle.x + n->rectangle.width / 2;
		int center_y = n->rectangle.y + n->rectangle.height / 2;
		wlr_cursor_warp(server.cursor, NULL, center_x, center_y);
	}

	server.focus_from_click = false;

	if (give_keyboard_focus) {
		ipc_put_status(SUB_MASK_REPORT, NULL);
		ipc_put_status(SUB_MASK_MONITOR_FOCUS, "monitor_focus[%s]\n", m->name);
		ipc_put_status(SUB_MASK_NODE_FOCUS, "node_focus[%s,%s,%u]\n",
			n->client && n->client->app_id[0] ? n->client->app_id : "?",
			n->client && n->client->title[0] ? n->client->title : "?", n->id);
	}

	return true;
}

bool focus_node(output_t *m, desktop_t *d, node_t *n) {
	return focus_node_impl(m, d, n, true);
}

bool activate_node(output_t *m, desktop_t *d, node_t *n) {
	if (settings.focus_on_activate == FOCUS_ON_ACTIVATE_FOCUS)
		return focus_node_impl(m, d, n, true);

	if (settings.focus_on_activate == FOCUS_ON_ACTIVATE_NONE ||
			(settings.focus_on_activate == FOCUS_ON_ACTIVATE_URGENT)) {
		bool result = focus_node_impl(m, d, n, false);
		if (settings.focus_on_activate == FOCUS_ON_ACTIVATE_URGENT && n && n->client) {
			n->client->flags.urgent = true;
			update_border_colors(n->client);
		}
		return result;
	}

	// focus only if on currently focused desktop
	bool on_current_desktop = (mon && mon->desk == d);
	return focus_node_impl(m, d, n, on_current_desktop);
}

node_t *find_fence_from(node_t *n, direction_t dir) {
	if (n == NULL)
		return NULL;

	node_t *p = n->parent;

	while (p != NULL) {
		node_t *brother = (is_first_child(n)) ? p->second_child : p->first_child;

		if (brother != NULL) {
			bool vertical = (dir == DIR_WEST || dir == DIR_EAST);
			bool horizontal = (dir == DIR_NORTH || dir == DIR_SOUTH);

			if ((vertical && p->split_type == TYPE_VERTICAL) || (horizontal &&
					p->split_type == TYPE_HORIZONTAL)) {
				bool first = (dir == DIR_WEST || dir == DIR_NORTH);

				if (first != is_first_child(n))
					return brother;
			}
		}

		n = p;
		p = n->parent;
	}

	return NULL;
}

node_t *find_fence(node_t *n, direction_t dir) {
	return find_fence_from(n, dir);
}
