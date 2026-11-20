#include "client.h"
#include "ipc/args.h"
#include "layout/layout.h"
#include "layout/tiled.h"
#include "layout/tree.h"
#include "output/output.h"
#include "settings.h"
#include "transaction.h"
#include "tree.h"
#include "types.h"
#include "view.h"
#include <limits.h>
#include <wlr/util/log.h>

static struct {
	bool borderless;
	bool gapless;
	int padding_top, padding_right, padding_bottom, padding_left;
} monocle_config;

static int monocle_window_gap(const desktop_t *d) {
	return monocle_config.gapless ? 0 : compute_window_gap(d);
}

static void monocle_arrange(output_t *m, desktop_t *d, struct wlr_box available) {
	available.x += monocle_config.padding_left;
	available.y += monocle_config.padding_top;
	available.width -= monocle_config.padding_left + monocle_config.padding_right;
	available.height -= monocle_config.padding_top + monocle_config.padding_bottom;

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
	return monocle_config.borderless && IS_TILED(c);
}

// the focused toplevel fills the screen, so it is reported as maximized
static bool monocle_reports_maximized(const desktop_t *d, const client_t *c) {
	return d->focus != NULL && d->focus->client == c;
}

// `layout monocle padding [<px>]` reads or sets every side at once
static void monocle_padding_command(ipc_args_t *a) {
	if (ipc_peek(a) == NULL) {
		ipc_okf(a, "%d %d %d %d\n", monocle_config.padding_top, monocle_config.padding_right,
			monocle_config.padding_bottom, monocle_config.padding_left);
		return;
	}

	int padding;
	if (!ipc_int(a, "padding", 0, INT_MAX, &padding))
		return;

	if (!a->dry_run) {
		monocle_config.padding_top = padding;
		monocle_config.padding_right = padding;
		monocle_config.padding_bottom = padding;
		monocle_config.padding_left = padding;
		transaction_commit_dirty();
	}

	ipc_okf(a, "padding %s\n", a->dry_run ? "would be set" : "set");
}

static const setting_t monocle_settings[] = {
	{
		.name = "borderless",
		.type = SETTING_BOOL,
		.ptr = &monocle_config.borderless,
		.flags = SETTING_COMMIT,
	},
	{
		.name = "gapless",
		.type = SETTING_BOOL,
		.ptr = &monocle_config.gapless,
		.flags = SETTING_COMMIT,
	},
	{
		.name = "top_padding",
		.type = SETTING_INT,
		.ptr = &monocle_config.padding_top,
		.min = 0,
		.max = INT_MAX,
		.fmt = "%d\n",
		.flags = SETTING_COMMIT,
	},
	{
		.name = "right_padding",
		.type = SETTING_INT,
		.ptr = &monocle_config.padding_right,
		.min = 0,
		.max = INT_MAX,
		.fmt = "%d\n",
		.flags = SETTING_COMMIT,
	},
	{
		.name = "bottom_padding",
		.type = SETTING_INT,
		.ptr = &monocle_config.padding_bottom,
		.min = 0,
		.max = INT_MAX,
		.fmt = "%d\n",
		.flags = SETTING_COMMIT,
	},
	{
		.name = "left_padding",
		.type = SETTING_INT,
		.ptr = &monocle_config.padding_left,
		.min = 0,
		.max = INT_MAX,
		.fmt = "%d\n",
		.flags = SETTING_COMMIT,
	},
};

static const command_t monocle_commands[] = {
	{"padding", "layout monocle padding [<px>]", monocle_padding_command},
	{NULL, NULL, NULL},
};

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
	.settings = monocle_settings,
	.settings_count = IPC_ARRAY_LEN(monocle_settings),
	.commands = monocle_commands,
	.single_visible = true,
	.has_directional_nav = false,
};
