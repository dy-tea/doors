#include "client.h"
#include "floating.h"
#include "ipc.h"
#include "layout.h"
#include "once.h"
#include "output.h"
#include "scratchpad.h"
#include "server.h"
#include "transaction.h"
#include "tree.h"
#include "tree_focus.h"
#include "types.h"
#include "xwayland.h"
#include <stdlib.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

typedef struct scratchpad_entry_t {
	struct wl_list link;
	node_t *node;
	client_state_t saved_state;
	bool saved_maximized;
	struct wlr_box saved_tiled_rect;
	struct wlr_box saved_floating_rect;
	struct output_t *saved_output;
	struct desktop_t *saved_desktop;
	node_t *saved_anchor;
	bool saved_first_child;
	split_type_t saved_split_type;
} scratchpad_entry_t;

static struct wl_list scratchpad_list;

static scratchpad_entry_t *scratchpad_find_entry(node_t *n) {
	if (!n)
		return NULL;

	scratchpad_entry_t *entry;
	wl_list_for_each(entry, &scratchpad_list, link)
		if (entry->node == n)
			return entry;

	return NULL;
}

bool scratchpad_has(node_t *n) {
	return n != NULL && n->scratchpad;
}

// remembers the slot the node currently occupies so it can be restored to it
static void save_tree_position(scratchpad_entry_t *entry, node_t *n) {
	entry->saved_anchor = NULL;
	entry->saved_first_child = false;
	entry->saved_split_type = TYPE_VERTICAL;

	if (n->parent == NULL)
		return;

	entry->saved_anchor = is_first_child(n) ? n->parent->second_child : n->parent->first_child;
	entry->saved_first_child = is_first_child(n);
	entry->saved_split_type = n->parent->split_type;
}

static void restore_tree_position(bool was_first_child, split_type_t split_type, node_t *n) {
	node_t *p = n->parent;
	if (p == NULL)
		return;

	if (is_first_child(n) != was_first_child) {
		node_t *tmp = p->first_child;
		p->first_child = p->second_child;
		p->second_child = tmp;
		p->split_ratio = 1.0 - p->split_ratio;
	}

	p->split_type = split_type;
	p->pending.split_type = p->split_type;
	p->current.split_type = p->split_type;
	p->pending.split_ratio = p->split_ratio;
	p->current.split_ratio = p->split_ratio;
}

// pick the desktop a parked window should be restored onto
static void scratchpad_target(desktop_t **out_d, output_t **out_m,
		const scratchpad_entry_t *entry) {
	desktop_t *d = NULL;
	output_t *m = NULL;

	if (entry != NULL && settings.scratchpad_restore_to_origin && entry->saved_desktop != NULL) {
		d = entry->saved_desktop;
		m = d->output != NULL ? d->output : entry->saved_output;
	}

	if (d == NULL && server.focused_output != NULL) {
		d = server.focused_output->desk;
		m = server.focused_output;
	}

	if (m == NULL && d != NULL)
		m = d->output;

	*out_d = d;
	*out_m = m;
}

static void focus_after_removal(output_t *m, desktop_t *d) {
	if (d == NULL)
		return;

	node_t *next = desktop_fallback_focus(d, NULL);
	d->focus = next;
	if (next != NULL && m != NULL)
		focus_node(m, d, next);
}

static void scratchpad_park(node_t *n, scratchpad_entry_t *entry) {
	desktop_t *d = n->desktop;
	output_t *out = n->output;

	entry->saved_state = n->client->state;
	entry->saved_maximized = n->client->flags.maximized;
	entry->saved_tiled_rect = n->client->tiled_rectangle;
	entry->saved_floating_rect = n->client->floating_rectangle;
	entry->saved_output = out;
	entry->saved_desktop = d;
	save_tree_position(entry, n);

	// detach from the split tree
	if (d != NULL && (n->parent != NULL || d->root == n)) {
		remove_node(d, n);
		n->parent = NULL;
		n->first_child = NULL;
		n->second_child = NULL;

		if (d->focus == n || d->focus == NULL)
			focus_after_removal(out, d);

		if (out != NULL && d->root != NULL)
			arrange(out, d, true);
	}

	n->client->flags.maximized = false;
	n->client->flags.minimized = true;
	n->client->state = STATE_FLOATING;
	n->client->last_state = entry->saved_state;
	n->client->floating_rectangle = entry->saved_floating_rect;
	node_set_hidden(n, true);

	struct wlr_scene_tree *st = client_get_scene_tree(n->client);
	if (st != NULL) {
		wlr_scene_node_reparent(&st->node, server.float_tree);
		wlr_scene_node_set_position(&st->node, n->client->floating_rectangle.x,
			n->client->floating_rectangle.y);
		wlr_scene_node_set_enabled(&st->node, false);
	}

	n->client->flags.shown = false;
	n->desktop = NULL;
	n->output = NULL;
	n->scratchpad = true;
}

void scratchpad_add(node_t *n) {
	if (n == NULL || n->client == NULL || n->scratchpad)
		return;

	wlr_log(WLR_INFO, "scratchpad_add: node=%u app_id=%s state=%d", n->id,
		n->client->app_id[0] ? n->client->app_id : "(none)", n->client->state);

	scratchpad_entry_t *entry = scratchpad_find_entry(n);
	if (entry == NULL) {
		entry = calloc(1, sizeof(*entry));
		if (entry == NULL) {
			wlr_log(WLR_ERROR, "scratchpad_add: allocation failed");
			return;
		}
		entry->node = n;
		wl_list_insert(scratchpad_list.prev, &entry->link);
	}

	scratchpad_park(n, entry);

	transaction_commit_dirty();
	wlr_log(WLR_INFO, "scratchpad_add: done, node=%u saved_state=%d", n->id, entry->saved_state);
}

void scratchpad_remove(node_t *n) {
	if (n == NULL || !n->scratchpad)
		return;

	scratchpad_entry_t *entry = scratchpad_find_entry(n);
	if (entry != NULL) {
		wl_list_remove(&entry->link);
		free(entry);
	}

	n->scratchpad = false;
	wlr_log(WLR_INFO, "scratchpad_remove: node=%u removed from scratchpad", n->id);
}

void scratchpad_show(node_t *n) {
	if (n == NULL || n->client == NULL || !n->scratchpad)
		return;

	scratchpad_entry_t *entry = scratchpad_find_entry(n);
	if (entry == NULL)
		return;

	desktop_t *d = NULL;
	output_t *m = NULL;
	scratchpad_target(&d, &m, entry);
	if (d == NULL) {
		wlr_log(WLR_ERROR, "scratchpad_show: no desktop to show on");
		return;
	}

	wlr_log(WLR_INFO, "scratchpad_show: node=%u output=%s desktop=%s restoring state=%d", n->id,
		m ? m->name : "(none)", d->name, entry->saved_state);

	client_state_t saved_state = entry->saved_state;
	bool saved_maximized = entry->saved_maximized;
	node_t *saved_anchor = entry->saved_anchor;
	bool saved_first_child = entry->saved_first_child;
	split_type_t saved_split_type = entry->saved_split_type;
	struct wlr_scene_tree *st = client_get_scene_tree(n->client);

	n->desktop = d;
	n->output = m;
	n->client->flags.minimized = false;
	n->scratchpad = false;
	node_set_hidden(n, false);

	wl_list_remove(&entry->link);
	free(entry);

	if (saved_state == STATE_FLOATING) {
		// restore as floating
		n->client->state = STATE_FLOATING;
		n->client->last_state = STATE_FLOATING;
		n->client->flags.maximized = false;

		if (st) {
			wlr_scene_node_reparent(&st->node, server.float_tree);
			wlr_scene_node_set_position(&st->node, n->client->floating_rectangle.x,
				n->client->floating_rectangle.y);
			wlr_scene_node_set_enabled(&st->node, true);
		}

		n->client->flags.shown = true;
		d->focus = n;
		focus_node(m, d, n);

		ipc_put_status(SUB_MASK_NODE_ADD, "node_add[%s,%s,%u]\n",
			n->client->app_id[0] ? n->client->app_id : "?", n->client->title[0] ? n->client->title : "?",
			n->id);
	} else {
		// back into the split tree, in the slot it came from
		n->client->state = saved_state == STATE_FULLSCREEN ? STATE_TILED : saved_state;
		n->client->last_state = n->client->state;
		n->client->flags.maximized = saved_maximized;

		if (st != NULL) {
			wlr_scene_node_reparent(&st->node, server.tile_tree);
			wlr_scene_node_set_enabled(&st->node, true);
		}
		n->client->flags.shown = true;

		node_t *ref = saved_anchor;
		if (ref == NULL || ref->client == NULL || node_is_minimized(ref))
			ref = find_public(d);
		insert_node(d, n, ref);
		restore_tree_position(saved_first_child, saved_split_type, n);

		d->focus = n;
		focus_node(m, d, n);
		arrange(m, d, true);

		if (saved_state == STATE_FULLSCREEN)
			client_set_fullscreen(m, d, n, true);

		ipc_put_status(SUB_MASK_NODE_ADD, "node_add[%s,%s,%u]\n",
			n->client->app_id[0] ? n->client->app_id : "?", n->client->title[0] ? n->client->title : "?",
			n->id);
	}

	transaction_commit_dirty();
	wlr_log(WLR_INFO, "scratchpad_show: done, node=%u shown on %s", n->id, d->name);
}

void scratchpad_hide(node_t *n) {
	if (n == NULL || n->client == NULL)
		return;
	if (!n->scratchpad && n->desktop == NULL)
		return;

	wlr_log(WLR_INFO, "scratchpad_hide: node=%u scratchpad=%d", n->id, n->scratchpad);

	scratchpad_add(n);
	transaction_commit_dirty();
	wlr_log(WLR_INFO, "scratchpad_hide: done, node=%u hidden", n->id);
}

void scratchpad_toggle(node_t *n) {
	if (n == NULL || n->client == NULL)
		return;

	output_t *focused = server.focused_output;
	wlr_log(WLR_INFO, "scratchpad_toggle: node=%u scratchpad=%d desktop=%p", n->id, n->scratchpad,
		(void *)n->desktop);

	if (n->scratchpad && n->desktop == NULL) {
		scratchpad_show(n);
		return;
	}

	if (focused == NULL) {
		wlr_log(WLR_ERROR, "scratchpad_toggle: no focused output");
		return;
	}

	if (n->desktop == focused->desk)
		scratchpad_hide(n);
	else
		scratchpad_hide(n), scratchpad_show(n);
}

void scratchpad_toggle_auto(void) {
	output_t *out = server.focused_output;
	if (out == NULL || out->desk == NULL)
		return;

	desktop_t *current_desk = out->desk;

	node_t *focused = current_desk->focus;
	if (focused != NULL && focused->client != NULL && focused->scratchpad &&
			focused->desktop == current_desk) {
		scratchpad_hide(focused);
		transaction_commit_dirty();
		return;
	}

	scratchpad_entry_t *entry;
	wl_list_for_each(entry, &scratchpad_list, link) {
		if (entry->node != NULL && entry->node->client != NULL && entry->node->desktop == current_desk) {
			scratchpad_hide(entry->node);
			transaction_commit_dirty();
			return;
		}
	}

	wl_list_for_each(entry, &scratchpad_list, link) {
		if (entry->node != NULL && entry->node->client != NULL && entry->node->desktop == NULL) {
			scratchpad_show(entry->node);
			transaction_commit_dirty();
			return;
		}
	}

	wlr_log(WLR_DEBUG, "scratchpad_toggle_auto: no scratchpad entries to show");
}

node_t *scratchpad_find_by_app_id(const char *app_id) {
	if (app_id == NULL)
		return NULL;

	scratchpad_entry_t *entry;
	wl_list_for_each(entry, &scratchpad_list, link) {
		if (entry->node != NULL && entry->node->client != NULL && strcmp(entry->node->client->app_id,
			app_id) == 0)
			return entry->node;
	}
	return NULL;
}

node_t *scratchpad_find_by_title(const char *title) {
	if (title == NULL)
		return NULL;

	scratchpad_entry_t *entry;
	wl_list_for_each(entry, &scratchpad_list, link) {
		if (entry->node != NULL && entry->node->client != NULL && strcmp(entry->node->client->title,
			title) == 0)
			return entry->node;
	}
	return NULL;
}

node_t *scratchpad_find(const char *app_id, const char *title) {
	node_t *n = NULL;
	if (app_id != NULL)
		n = scratchpad_find_by_app_id(app_id);
	if (n == NULL && title != NULL)
		n = scratchpad_find_by_title(title);
	return n;
}

int scratchpad_count(void) {
	int count = 0;
	scratchpad_entry_t *entry;
	wl_list_for_each(entry, &scratchpad_list, link)
		count++;
	return count;
}

node_t *scratchpad_nth(int index) {
	if (index < 0)
		return NULL;

	int i = 0;
	scratchpad_entry_t *entry;
	wl_list_for_each(entry, &scratchpad_list, link) {
		if (i == index)
			return entry->node;
		i++;
	}
	return NULL;
}

void scratchpad_init(void) {
	ONCE();
	wl_list_init(&scratchpad_list);
	wlr_log(WLR_INFO, "Scratchpad initialized");
}

void scratchpad_fini(void) {
	ONCE();
	scratchpad_entry_t *entry, *tmp;
	wl_list_for_each_safe(entry, tmp, &scratchpad_list, link) {
		if (entry->node != NULL)
			entry->node->scratchpad = false;
		wl_list_remove(&entry->link);
		free(entry);
	}
	wlr_log(WLR_INFO, "Scratchpad finalized");
}
