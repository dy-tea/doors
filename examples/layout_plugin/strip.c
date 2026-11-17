// strip - an example doors layout plugin
//
// Places every tiled window in a single, evenly divided horizontal strip.
//
// Build (from the doors repository root):
//   cc -shared -fPIC -o strip.so strip.c -DWLR_USE_UNSTABLE -I../../include -I/usr/include/pixman-1
// -I/usr/include/wlroots-0.21 $(pkg-config --cflags wayland-server glib-2.0)
//
// Load and use:
//   doorsctl plugin load /path/to/strip.so
//   doorsctl desktop --layout strip

#include "client.h"
#include "layout/layout.h"
#include "layout/tree.h"
#include "output/output.h"
#include "plugin.h"
#include "settings.h"
#include "tree.h"
#include "tree_focus.h"
#include "types.h"
#include <stdlib.h>
#include <wlr/util/box.h>

// leaves of the desktop tree, ordered left to right along the strip
static int strip_nodes(desktop_t *d, node_t ***out_nodes) {
	const int count = collect_tiled_leaves(d, out_nodes);
	if (count <= 1)
		return count;

	node_t **nodes = *out_nodes;
	for (int i = 1; i < count; i++) {
		node_t *key = nodes[i];
		const struct wlr_box key_box = key->rectangle;
		int j = i - 1;
		while (j >= 0 && (nodes[j]->rectangle.x > key_box.x || (nodes[j]->rectangle.x == key_box.x &&
				nodes[j]->rectangle.y > key_box.y))) {
			nodes[j + 1] = nodes[j];
			j--;
		}
		nodes[j + 1] = key;
	}
	return count;
}

// index of the focused leaf in the ordered node list, -1 if unfocused
static int strip_focused_index(node_t **nodes, int count, const desktop_t *d) {
	if (d == NULL)
		return -1;

	for (int i = 0; i < count; i++) {
		if (nodes[i] == d->focus)
			return i;
	}
	return -1;
}

static void strip_arrange(output_t *m, desktop_t *d, struct wlr_box available) {
	if (d == NULL)
		return;

	node_t **nodes = NULL;
	const int count = strip_nodes(d, &nodes);
	if (count <= 0) {
		free(nodes);
		return;
	}

	const int gap = compute_window_gap(d) > 0 ? compute_window_gap(d) : 0;
	const int bw = (int)effective_border_width(d);

	// split the strip into equal columns
	int actual_gap = gap;
	if (count > 1 && (long long)actual_gap * (count - 1) >= available.width) {
		actual_gap = (available.width - count) / (count - 1);
		if (actual_gap < 0)
			actual_gap = 0;
	}

	const int usable = available.width > actual_gap * (count - 1) ? available.width - actual_gap *
		(count - 1) : 0;
	const int base = usable / count;
	const int remainder = usable % count;

	int cursor = available.x;
	for (int i = 0; i < count; i++) {
		const int width = base + (i < remainder ? 1 : 0);
		const struct wlr_box outer = {
			.x = cursor,
			.y = available.y,
			.width = width > 0 ? width : 1,
			.height = available.height > 0 ? available.height : 1,
		};

		node_t *n = nodes[i];
		if (n->client != NULL) {
			const struct wlr_box inner = apply_bleed(outer, bw, 0);
			n->client->tiled_rectangle = inner;
			n->client->arranged_rectangle = inner;
			n->output = m;
		}
		node_set_pending_rectangle(n, outer);
		node_set_dirty(n);

		cursor += width + actual_gap;
	}

	free(nodes);
}

static bool strip_focus(desktop_t *d, const direction_t dir) {
	if (d == NULL)
		return false;

	node_t **nodes = NULL;
	const int count = strip_nodes(d, &nodes);
	if (count <= 0) {
		free(nodes);
		return false;
	}

	const int index = strip_focused_index(nodes, count, d);
	if (index < 0) {
		free(nodes);
		return false;
	}

	int target = -1;
	switch (dir) {
	case DIR_WEST:
		target = index > 0 ? index - 1 : (settings.focus_wrapping ? count - 1 : -1);
		break;
	case DIR_EAST:
		target = index + 1 < count ? index + 1 : (settings.focus_wrapping ? 0 : -1);
		break;
	default:
		break;
	}

	if (target >= 0) {
		focus_node(output_focused(), d, nodes[target]);
	}

	free(nodes);
	return target >= 0;
}

static bool strip_swap(output_t *m, desktop_t *d, const direction_t dir) {
	if (d == NULL)
		return false;

	node_t **nodes = NULL;
	const int count = strip_nodes(d, &nodes);
	if (count <= 0) {
		free(nodes);
		return false;
	}

	const int index = strip_focused_index(nodes, count, d);
	if (index < 0) {
		free(nodes);
		return false;
	}

	int target = -1;
	switch (dir) {
	case DIR_WEST:
		target = index > 0 ? index - 1 : (settings.focus_wrapping ? count - 1 : -1);
		break;
	case DIR_EAST:
		target = index + 1 < count ? index + 1 : (settings.focus_wrapping ? 0 : -1);
		break;
	default:
		break;
	}

	if (target >= 0) {
		swap_nodes(m, d, nodes[index], m, d, nodes[target]);
		arrange(m, d, true);
	}

	free(nodes);
	return target >= 0;
}

static const layout_impl_t strip_impl = {
	.name = "strip",
	.arrange = strip_arrange,
	.on_focus = NULL,
	.focus = strip_focus,
	.swap = strip_swap,
	.enter = NULL,
	.leave = NULL,
	.init_client = NULL,
	.on_client_state = NULL,
	.places_floating = NULL,
	.collect = NULL,
	.single_visible = false,
	.has_directional_nav = true,
};

const doors_plugin_t doors_plugin = {
	.api_version = DOORS_PLUGIN_API_VERSION,
	.name = "strip",
	.layouts = (const layout_impl_t *const[]){
		&strip_impl,
		NULL
	},
};
