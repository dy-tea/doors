#include "client.h"
#include "dim.h"
#include "layout/floating.h"
#include "layout/layout.h"
#include "layout/master_stack.h"
#include "layout/scroller.h"
#include "layout/tree.h"
#include "output/output.h"
#include "transaction.h"
#include "tree.h"
#include "types.h"
#include "view.h"
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#define REGISTRY_LEN(arr) (sizeof(arr) / sizeof((arr)[0]))

void arrange(output_t *m, desktop_t *d, const bool use_transaction) {
	if (d == NULL)
		return;

	if (m != NULL) {
		const struct wlr_box rect = desktop_usable_area(m, d);

		const layout_impl_t *impl = layout_get_impl(d->layout);
		if (impl && impl->arrange)
			impl->arrange(m, d, rect);
	}

	if (use_transaction)
		transaction_commit_dirty();

	dim_update_output(m);
}

static void tiled_arrange(output_t *m, desktop_t *d, struct wlr_box available) {
	const int wg = compute_window_gap(d);
	available.x += wg;
	available.y += wg;
	available.width -= wg;
	available.height -= wg;
	apply_layout(m, d, d->root, available, available);
}

static void monocle_arrange(output_t *m, desktop_t *d, struct wlr_box available) {
	available.x += settings.monocle_padding.left;
	available.y += settings.monocle_padding.top;
	available.width -= settings.monocle_padding.left + settings.monocle_padding.right;
	available.height -= settings.monocle_padding.top + settings.monocle_padding.bottom;
	if (!settings.gapless_monocle) {
		int wg = compute_window_gap(d);
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

static void scroller_on_focus(output_t *m, desktop_t *d, node_t *n) {
	if (!d)
		return;

	if (!d->scroller_state)
		d->scroller_state = scroller_create();

	scroller_state_t *s = d->scroller_state;
	if (!s)
		return;

	if (n != NULL && n->client != NULL)
		scroller_sync_focus(s, n->client);

	if (s->column_count == 0 && d->root) {
		FOR_EACH_LEAF(leaf, d->root)
			if (leaf->client && !node_is_minimized(leaf))
				leaf->client->flags.shown = true;
	} else {
		for (int i = 0; i < s->column_count; i++) {
			for (int j = 0; j < s->columns[i].tile_count; j++) {
				client_t *c = s->columns[i].tiles[j].client;
				if (c && !c->flags.minimized)
					c->flags.shown = true;
			}
		}
	}

	if (n != NULL && n->client && n->client->view && n->client->view->configured) {
		wlr_log(WLR_DEBUG, "Triggering arrange");
		arrange(m, d, true);
	} else {
		wlr_log(WLR_DEBUG, "Skipping arrange (initial map)");
	}
}

static void scroller_on_client_state(output_t *m, desktop_t *d, node_t *n, client_state_t old) {
	(void)m;
	(void)old;

	if (n == NULL || n->client == NULL)
		return;

	scroller_set_maximized(d, n, client_is_maximized(n->client));
}

static int tiled_collect(desktop_t *d, node_t ***out_nodes) {
	return collect_tiled_leaves(d, out_nodes);
}

static int scroller_collect_fn(desktop_t *d, node_t ***out_nodes) {
	return scroller_collect(d, out_nodes);
}

static bool scroller_focus(desktop_t *d, direction_t dir) {
	switch (dir) {
	case DIR_WEST:
		return scroller_focus_prev(d);
	case DIR_EAST:
		return scroller_focus_next(d);
	case DIR_NORTH:
		return scroller_focus_up(d);
	case DIR_SOUTH:
		return scroller_focus_down(d);
	}
	return false;
}

// pick the leaf of `sub` that is adjacent to `from` in direction `dir`
static node_t *closest_leaf(node_t *sub, node_t *from, direction_t dir) {
	const bool horiz = dir == DIR_WEST || dir == DIR_EAST;
	struct wlr_box f = from->rectangle;
	node_t *best = NULL;
	long best_overlap = -1, best_dist = 0;

	FOR_EACH_LEAF(leaf, sub) {
		if (node_is_invisible(leaf))
			continue;

		const struct wlr_box r = leaf->rectangle;
		long lo, hi, dist;
		if (horiz) {
			lo = r.y > f.y ? r.y : f.y;
			hi = r.y + r.height < f.y + f.height ? r.y + r.height : f.y + f.height;
			dist = dir == DIR_EAST ? r.x : -(r.x + r.width);
		} else {
			lo = r.x > f.x ? r.x : f.x;
			hi = r.x + r.width < f.x + f.width ? r.x + r.width : f.x + f.width;
			dist = dir == DIR_SOUTH ? r.y : -(r.y + r.height);
		}
		const long overlap = hi - lo;
		if (best == NULL || overlap > best_overlap || (overlap == best_overlap && dist < best_dist)) {
			best = leaf;
			best_overlap = overlap;
			best_dist = dist;
		}
	}
	return best;
}

static node_t *focus_fence_leaf(node_t *from, const direction_t dir) {
	node_t *cursor = from;

	while (cursor != NULL) {
		node_t *sub = find_fence_from(cursor, dir);
		if (sub == NULL)
			return NULL;

		node_t *leaf = closest_leaf(sub, from, dir);
		if (leaf != NULL)
			return leaf;

		cursor = sub;
	}

	return NULL;
}

static node_t *wrapped_focus_leaf(const desktop_t *d, const direction_t dir) {
	if (!settings.focus_wrapping || d->root == NULL)
		return NULL;

	node_t *first = NULL, *last = NULL;
	FOR_EACH_LEAF(leaf, d->root) {
		if (node_is_invisible(leaf))
			continue;

		if (first == NULL)
			first = leaf;
		last = leaf;
	}

	node_t *w = dir == DIR_EAST || dir == DIR_SOUTH ? first : last;
	return w != NULL && w != d->focus ? w : NULL;
}

static bool tiled_focus(desktop_t *d, const direction_t dir) {
	if (d->focus == NULL)
		return false;

	node_t *n = focus_fence_leaf(d->focus, dir);
	if (n != NULL)
		return focus_node(output_focused(), d, n);

	n = wrapped_focus_leaf(d, dir);
	if (n != NULL)
		return focus_node(output_focused(), d, n);

	return false;
}

static bool tiled_swap(output_t *m, desktop_t *d, direction_t dir) {
	if (d->focus == NULL)
		return false;

	node_t *n = focus_fence_leaf(d->focus, dir);
	if (n != NULL) {
		swap_nodes(m, d, d->focus, m, d, n);
		return true;
	}
	return false;
}

static const layout_impl_t tiled_impl = {
	.name = "tiled",
	.arrange = tiled_arrange,
	.on_focus = NULL,
	.focus = tiled_focus,
	.swap = tiled_swap,
	.enter = NULL,
	.leave = NULL,
	.init_client = NULL,
	.on_client_state = NULL,
	.places_floating = NULL,
	.collect = tiled_collect,
	.single_visible = false,
	.has_directional_nav = false,
};

static const layout_impl_t monocle_impl = {
	.name = "monocle",
	.arrange = monocle_arrange,
	.on_focus = monocle_on_focus,
	.focus = tiled_focus,
	.swap = NULL,
	.enter = NULL,
	.leave = NULL,
	.init_client = NULL,
	.on_client_state = monocle_on_client_state,
	.places_floating = NULL,
	.collect = tiled_collect,
	.single_visible = true,
	.has_directional_nav = false,
};

static const layout_impl_t scroller_impl = {
	.name = "scroller",
	.arrange = scroller_arrange,
	.on_focus = scroller_on_focus,
	.focus = scroller_focus,
	.swap = scroller_swap,
	.enter = NULL,
	.leave = scroller_leave,
	.init_client = NULL,
	.on_client_state = scroller_on_client_state,
	.places_floating = NULL,
	.collect = scroller_collect_fn,
	.single_visible = false,
	.has_directional_nav = true,
};

static const layout_impl_t master_stack_impl = {
	.name = "master_stack",
	.arrange = master_stack_arrange,
	.on_focus = NULL,
	.focus = master_stack_focus,
	.swap = master_stack_swap,
	.enter = NULL,
	.leave = NULL,
	.init_client = NULL,
	.on_client_state = NULL,
	.places_floating = NULL,
	.collect = master_stack_collect,
	.single_visible = false,
	.has_directional_nav = true,
};

static const layout_impl_t floating_impl = {
	.name = "floating",
	.arrange = floating_arrange,
	.on_focus = floating_on_focus,
	.focus = floating_focus,
	.swap = NULL,
	.enter = floating_enter,
	.leave = NULL,
	.init_client = floating_init_client,
	.on_client_state = NULL,
	.places_floating = floating_places_floating,
	.collect = floating_collect,
	.single_visible = false,
	.has_directional_nav = true,
};

static const layout_impl_t *registry[] = {
	[LAYOUT_TILED] = &tiled_impl,
	[LAYOUT_MONOCLE] = &monocle_impl,
	[LAYOUT_SCROLLER] = &scroller_impl,
	[LAYOUT_MASTER_STACK] = &master_stack_impl,
	[LAYOUT_FLOATING] = &floating_impl,
};

typedef struct dynamic_layout_t {
	const layout_impl_t *impl;
	char status_char;
	layout_t id;
	struct wl_list link;
} dynamic_layout_t;

static struct wl_list dynamic_layouts;
static size_t dynamic_layout_count;
static str_pair_t *dynamic_ipc_values;

static bool build_ipc_values(void);

void layout_init(void) {
	wl_list_init(&dynamic_layouts);
}

static const dynamic_layout_t *dynamic_layout_for(layout_t layout) {
	const size_t i = (size_t)layout;
	if (i < LAYOUT_BUILTIN_COUNT)
		return NULL;

	const dynamic_layout_t *entry;
	wl_list_for_each(entry, &dynamic_layouts, link) {
		if (entry->id == layout)
			return entry;
	}
	return NULL;
}

char layout_plugin_char(layout_t l) {
	const dynamic_layout_t *entry = dynamic_layout_for(l);
	return entry != NULL ? entry->status_char : '?';
}

layout_t layout_register(const layout_impl_t *impl) {
	if (impl == NULL || impl->name == NULL || impl->name[0] == '\0')
		return (layout_t) - 1;

	if (layout_find_by_name(impl->name, NULL))
		return (layout_t) - 1;

	const char builtin_chars[] = "TMSKF";
	char status_char = '?';
	for (char c = 'A'; c <= 'Z'; c++) {
		if (strchr(builtin_chars, c) != NULL)
			continue;

		bool taken = false;
		const dynamic_layout_t *entry;
		wl_list_for_each(entry, &dynamic_layouts, link) {
			if (entry->status_char == c) {
				taken = true;
				break;
			}
		}
		if (!taken) {
			status_char = c;
			break;
		}
	}

	dynamic_layout_t *entry = malloc(sizeof(dynamic_layout_t));
	if (entry == NULL) {
		wlr_log(WLR_ERROR, "failed to allocate registry entry for layout \"%s\"", impl->name);
		return (layout_t) - 1;
	}

	entry->impl = impl;
	entry->status_char = status_char;
	entry->id = (layout_t)(LAYOUT_BUILTIN_COUNT + dynamic_layout_count);
	wl_list_insert(dynamic_layouts.prev, &entry->link);
	dynamic_layout_count++;

	if (!build_ipc_values())
		wlr_log(WLR_ERROR, "failed to rebuild layout IPC table");

	wlr_log(WLR_INFO, "registered layout \"%s\" (id %d)", impl->name, entry->id);
	return entry->id;
}

size_t layout_count(void) {
	return LAYOUT_BUILTIN_COUNT + dynamic_layout_count;
}

const char *layout_name(layout_t layout) {
	const size_t i = (size_t)layout;
	if (i < REGISTRY_LEN(registry))
		return registry[i] != NULL ? registry[i]->name : NULL;

	const dynamic_layout_t *entry = dynamic_layout_for(layout);
	return entry != NULL ? entry->impl->name : NULL;
}

bool layout_find_by_name(const char *name, layout_t *out) {
	if (name == NULL)
		return false;

	for (size_t i = 0; i < REGISTRY_LEN(registry); i++) {
		if (registry[i] != NULL && streq(registry[i]->name, name)) {
			if (out != NULL)
				*out = (layout_t)i;
			return true;
		}
	}

	const dynamic_layout_t *entry;
	wl_list_for_each(entry, &dynamic_layouts, link) {
		if (streq(entry->impl->name, name)) {
			if (out != NULL)
				*out = entry->id;
			return true;
		}
	}

	return false;
}

const str_pair_t *layout_ipc_values(void) {
	if (dynamic_ipc_values == NULL)
		build_ipc_values();
	return dynamic_ipc_values;
}

const layout_impl_t *layout_get_impl(layout_t layout) {
	const size_t i = (size_t)layout;
	if (i < REGISTRY_LEN(registry))
		return registry[i];

	const dynamic_layout_t *entry = dynamic_layout_for(layout);
	return entry != NULL ? entry->impl : NULL;
}

static bool build_ipc_values(void) {
	str_pair_t *values = malloc((dynamic_layout_count + 6) * sizeof(str_pair_t));
	if (values == NULL)
		return false;

	static const str_pair_t builtins[] = {
		{"tiled", LAYOUT_TILED},
		{"monocle", LAYOUT_MONOCLE},
		{"scroller", LAYOUT_SCROLLER},
		{"master_stack", LAYOUT_MASTER_STACK},
		{"floating", LAYOUT_FLOATING},
	};
	size_t n = 0;
	for (size_t i = 0; i < REGISTRY_LEN(builtins); i++)
		values[n++] = builtins[i];
	const dynamic_layout_t *entry;
	wl_list_for_each(entry, &dynamic_layouts, link)
		values[n++] = (str_pair_t){
			entry->impl->name,
			(long)entry->id
		};
	values[n] = (str_pair_t){
		NULL,
		0
	};

	free(dynamic_ipc_values);
	dynamic_ipc_values = values;
	return true;
}

bool layout_init_client(output_t *m, desktop_t *d, client_t *c) {
	if (!d || !c)
		return false;

	const layout_impl_t *impl = layout_get_impl(d->layout);
	if (impl && impl->init_client)
		return impl->init_client(m, d, c);

	return false;
}

void layout_client_state_changed(output_t *m, desktop_t *d, node_t *n, client_state_t old_state) {
	if (d == NULL)
		return;

	const layout_impl_t *impl = layout_get_impl(d->layout);
	if (impl != NULL && impl->on_client_state != NULL)
		impl->on_client_state(m, d, n, old_state);
}

void layout_desktop_changed(output_t *m, desktop_t *d) {
	if (d == NULL)
		return;

	layout_client_state_changed(m, d, NULL, STATE_TILED);
}

void layout_set(desktop_t *d, const layout_t new_layout) {
	if (d == NULL || (size_t)new_layout >= layout_count() || layout_get_impl(new_layout) == NULL)
		return;

	const layout_t old_layout = d->layout;

	if (old_layout == new_layout) {
		layout_desktop_changed(d->output ? d->output : output_focused(), d);
		return;
	}

	d->user_layout = old_layout;
	output_t *m = d->output != NULL ? d->output : output_focused();

	// let the outgoing layout drop the per-desktop state it owns
	const layout_impl_t *old_impl = layout_get_impl(old_layout);
	if (m != NULL && old_impl != NULL && old_impl->leave != NULL)
		old_impl->leave(m, d);

	d->layout = new_layout;

	// let the incoming layout adopt the toplevels already on the desktop
	const layout_impl_t *impl = layout_get_impl(new_layout);
	if (m != NULL && impl != NULL && impl->enter != NULL)
		impl->enter(m, d);

	layout_desktop_changed(m, d);
}

void layout_toggle(desktop_t *d, const layout_t target) {
	if (d == NULL)
		return;

	layout_set(d, d->layout == target ? d->user_layout : target);
}
