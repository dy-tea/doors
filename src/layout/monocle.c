#include "client.h"
#include "layout/layout.h"
#include "layout/tiled.h"
#include "layout/tree.h"
#include "output/output.h"
#include "settings.h"
#include "tree.h"
#include "types.h"
#include "view.h"
#include <wlr/util/log.h>

static int monocle_window_gap(const desktop_t *d) {
	return settings.gapless_monocle ? 0 : compute_window_gap(d);
}

static void monocle_arrange(output_t *m, desktop_t *d, struct wlr_box available) {
	available.x += settings.monocle_padding.left;
	available.y += settings.monocle_padding.top;
	available.width -= settings.monocle_padding.left + settings.monocle_padding.right;
	available.height -= settings.monocle_padding.top + settings.monocle_padding.bottom;

	const int wg = monocle_window_gap(d);
	if (wg > 0) {
		available.x += wg;
		available.y += wg;
		available.width -= wg;
		available.height -= wg;
	}

	apply_layout(m, d, d->root, available, available);
}

// monocle keeps exactly one window on screen, so focusing reveals n and hides
// every other leaf on the desktop
static void monocle_on_focus(output_t *m, desktop_t *d, node_t *n) {
	(void)m;
	if (!d->root)
		return;

	FOR_EACH_LEAF(node, d->root)
		client_set_visible(node->client, node == n);
}

static void monocle_on_client_state(output_t *m, desktop_t *d, node_t *n, client_state_t old) {
	(void)n;
	(void)old;

	if (d->root == NULL)
		return;

	const node_t *reveal = d->focus;
	FOR_EACH_LEAF(node, d->root) {
		if (node->client != NULL && (client_is_maximized(node->client) ||
				node->client->state == STATE_FULLSCREEN)) {
			reveal = node;
			break;
		}
	}

	FOR_EACH_LEAF(node, d->root)
		client_set_visible(node->client, node == reveal);

	if (m != NULL && reveal != NULL && reveal->client != NULL && reveal->client->view != NULL &&
		reveal->client->view->configured)
		arrange(m, d, true);
}

// only the focused toplevel is shown, so nothing else may be revealed
static bool monocle_wants_visible(const desktop_t *d, const node_t *n) {
	return d->focus == n;
}

static bool monocle_borderless(const desktop_t *d, const client_t *c) {
	(void)d;
	return settings.borderless_monocle && IS_TILED(c);
}

// the focused toplevel fills the screen, so it is reported as maximized
static bool monocle_reports_maximized(const desktop_t *d, const client_t *c) {
	return d->focus != NULL && d->focus->client == c;
}

const layout_impl_t monocle_impl = {
	.name = "monocle",
	.arrange = monocle_arrange,
	.on_focus = monocle_on_focus,
	.focus = tiled_focus,
	.swap = NULL,
	.enter = NULL,
	.leave = NULL,
	.init_client = NULL,
	.client_mapped = NULL,
	.client_unmapped = NULL,
	.focus_after_unmap = NULL,
	.on_client_state = monocle_on_client_state,
	.resize = NULL,
	.places_floating = NULL,
	.wants_visible = monocle_wants_visible,
	.borderless = monocle_borderless,
	.reports_maximized = monocle_reports_maximized,
	.window_gap = monocle_window_gap,
	.resize_edges = NULL,
	.pointer_resize_begin = NULL,
	.pointer_resize_update = NULL,
	.focus_scroll_distance = NULL,
	.pan_fingers = NULL,
	.pan_update = NULL,
	.pan_end = NULL,
	.collect = tiled_collect,
	.single_visible = true,
	.has_directional_nav = false,
};
