#include "animation.h"
#include "effects/effects.h"
#include "input/keyboard.h"
#include "ipc/args.h"
#include "ipc/helpers.h"
#include "layout/floating.h"
#include "layout/layout.h"
#include "output/output.h"
#include "protocol/workspace.h"
#include "scratchpad.h"
#include "server.h"
#include "surface.h"
#include "tabs.h"
#include "transaction.h"
#include "tree.h"
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>

static void hide_node_client(node_t *n) {
	n->client->flags.shown = false;
	struct wlr_scene_tree *st = client_get_scene_tree(n->client);
	if (st)
		wlr_scene_node_set_enabled(&st->node, false);
}

static void unhide_leaves(desktop_t *desk) {
	for (node_t *ni = first_extrema(desk->root); ni; ni = next_leaf(ni, desk->root)) {
		if (!ni->client)
			continue;
		if (node_is_minimized(ni))
			continue;

		ni->client->flags.shown = true;
		bool configured = true;
		if (ni->client->view)
			configured = ni->client->view->configured;

		if (!configured)
			continue;

		struct wlr_scene_tree *st = client_get_scene_tree(ni->client);
		if (st)
			wlr_scene_node_set_enabled(&st->node, true);
	}
}

static void hide_leaves(desktop_t *desk) {
	for (node_t *ni = first_extrema(desk->root); ni; ni = next_leaf(ni, desk->root)) {
		if (!ni->client)
			continue;

		ni->client->flags.shown = false;
		struct wlr_scene_tree *st = client_get_scene_tree(ni->client);
		if (st)
			wlr_scene_node_set_enabled(&st->node, false);
	}
}

static void unlink_and_refocus(node_t *n, desktop_t *src, output_t *mon) {
	hide_node_client(n);
	remove_node(src, n);
	if (src->root) {
		node_t *nf = first_extrema(src->root);
		if (nf) {
			src->focus = nf;
			focus_node(mon, src, nf);
		} else {
			src->focus = NULL;
		}
	} else {
		src->focus = NULL;
	}
	n->destroying = false;
	n->ntxnrefs = 0;
}

static node_t *focused_client(ipc_args_t *a, output_t **mon) {
	node_t *n = ipc_focused(a, mon);
	if (!n)
		return NULL;
	if (!n->client) {
		ipc_fail(a, "Focused node has no client\n");
		return NULL;
	}
	return n;
}

static const cfg_enum_value_t direction_values[] = {
	{"north", DIR_NORTH},
	{"n", DIR_NORTH},
	{"south", DIR_SOUTH},
	{"s", DIR_SOUTH},
	{"east", DIR_EAST},
	{"e", DIR_EAST},
	{"west", DIR_WEST},
	{"w", DIR_WEST},
	IPC_ENUM_END,
};

static const cfg_enum_value_t layer_values[] = {
	{"below", LAYER_BELOW},
	{"normal", LAYER_NORMAL},
	{"above", LAYER_ABOVE},
	IPC_ENUM_END,
};

static const cfg_enum_value_t circulate_values[] = {
	{"forward", 1},
	{"f", 1},
	{"backward", -1},
	{"back", -1},
	{"b", -1},
	IPC_ENUM_END,
};

// look up a node on the focused desktop by its reported id
static node_t *find_node_by_id(ipc_args_t *a, desktop_t *desk, const char *what) {
	const char *arg;
	if (!ipc_need(a, what, &arg))
		return NULL;

	int id = atoi(arg);
	if (id <= 0) {
		ipc_fail(a, "invalid %s \"%s\"\n", what, arg);
		return NULL;
	}

	for (node_t *n = first_extrema(desk->root); n != NULL; n = next_leaf(n, desk->root)) {
		if (n->id == (uint32_t)id)
			return n;
	}

	ipc_fail(a, "%s %d not found\n", what, id);
	return NULL;
}

static void node_focus(ipc_args_t *a) {
	output_t *m = server.focused_output;
	if (m && m->desk && m->desk->focus && m->desk->focus->client) {
		focus_node(m, m->desk, m->desk->focus);
		ipc_ok(a, "Focused\n");
	} else {
		ipc_fail(a, "No focused node\n");
	}
}

static void node_close(ipc_args_t *a) {
	output_t *m = server.focused_output;
	if (m && m->desk && m->desk->focus && m->desk->focus->client) {
		kill_node(m->desk, m->desk->focus);
		ipc_ok(a, "Closed\n");
	} else {
		ipc_fail(a, "No focused node to close\n");
	}
}

static void node_state(ipc_args_t *a) {
	static const struct {
		const char *name;
		void (*apply)(void);
	} states[] = {
		{"tiled", tile_focused},
		{"floating", toggle_floating},
		{"fullscreen", toggle_fullscreen},
		{"maximized", toggle_maximize},
		{"minimized", toggle_minimize},
	};

	const char *name;
	if (!ipc_need(a, "state", &name))
		return;

	for (size_t i = 0; i < IPC_ARRAY_LEN(states); i++) {
		if (!streq(name, states[i].name))
			continue;

		states[i].apply();

		output_t *m = server.focused_output;
		if (m && m->desk && m->desk->focus && m->desk->focus->client)
			ipc_ok(a, "State changed\n");
		else
			ipc_fail(a, "No focused node\n");
		return;
	}

	ipc_buf_t b;
	char list[128];
	ipc_buf_init(&b, list, sizeof(list));
	for (size_t i = 0; i < IPC_ARRAY_LEN(states); i++)
		ipc_buff(&b, "%s\"%s\"", i > 0 ? ", " : "", states[i].name);
	ipc_fail(a, "Unknown state, expected one of: %s\n", list);
}

static void node_to_desktop(ipc_args_t *a) {
	const char *name;
	if (!ipc_need(a, "desktop", &name))
		return;

	desktop_t *target = find_desktop_by_name(name);
	if (!target) {
		ipc_fail(a, "Desktop \"%s\" not found\n", name);
		return;
	}

	output_t *m = server.focused_output;
	if (!m || !m->desk || !m->desk->focus) {
		ipc_fail(a, "No focused node\n");
		return;
	}
	if (m->desk == target) {
		ipc_fail(a, "Already on target desktop\n");
		return;
	}

	node_t *n = m->desk->focus;
	if (!n->client) {
		ipc_fail(a, "Focused node has no client\n");
		return;
	}

	desktop_t *src = m->desk;
	unlink_and_refocus(n, src, m);
	insert_node(target, n, find_public(target));
	target->focus = n;

	hide_leaves(target);
	arrange(m, target, false);

	for (node_t *it = first_extrema(src->root); it != NULL; it = next_leaf(it, src->root)) {
		if (!it->client)
			continue;
		if (node_is_minimized(it))
			continue;

		it->client->flags.shown = true;
		bool configured = true;
		if (it->client->view)
			configured = it->client->view->configured;
		if (!configured)
			continue;

		struct wlr_scene_tree *st = client_get_scene_tree(it->client);
		if (st)
			wlr_scene_node_set_enabled(&st->node, true);
	}
	arrange(m, src, true);

	ipc_ok(a, "node sent to desktop\n");
}

static bool *node_flag_field(node_t *n, const char *key) {
	if (streq(key, "sticky"))
		return &n->sticky;
	if (streq(key, "private"))
		return &n->private_node;
	if (streq(key, "locked"))
		return &n->locked;
	if (streq(key, "marked"))
		return &n->marked;
	return NULL;
}

static bool node_flag_key_matches(const char *arg, const ipc_sub_t *sub) {
	(void)sub;
	return arg[0] != '-';
}

static void node_flag(ipc_args_t *a) {
	output_t *m;
	node_t *n = ipc_focused(a, &m);
	if (!n)
		return;

	const char *arg;
	if (!ipc_need(a, "flag", &arg))
		return;

	const char *eq = strchr(arg, '=');
	char key[MAXLEN];
	const char *val = NULL;
	if (eq) {
		size_t klen = (size_t)(eq - arg);
		if (klen >= sizeof(key)) {
			ipc_fail(a, "Flag name too long\n");
			return;
		}
		memcpy(key, arg, klen);
		key[klen] = '\0';
		val = eq + 1;
	} else {
		snprintf(key, sizeof(key), "%s", arg);
	}

	// opacity and border_radius take a number, every other flag takes a bool
	bool numeric = streq(key, "opacity") || streq(key, "border_radius");

	bool want = false;
	if (val && !numeric) {
		if (streq(val, "true") || streq(val, "on") || streq(val, "1"))
			want = true;
		else if (!streq(val, "false") && !streq(val, "off") && !streq(val, "0")) {
			ipc_fail(a, "Expected true or false for \"%s\", got \"%s\"\n", key, val);
			return;
		}
	}

	bool *field = node_flag_field(n, key);
	if (streq(key, "hidden")) {
		node_set_hidden(n, val ? want : !n->hidden);
		transaction_commit_dirty();
		ipc_ok(a, "Flag changed\n");
	} else if (field != NULL) {
		*field = val ? want : !*field;
		transaction_commit_dirty();
		ipc_ok(a, "Flag changed\n");
	} else if (streq(key, "blur")) {
		if (!n->client) {
			ipc_fail(a, "Focused node has no client\n");
			return;
		}
		n->client->flags.blur = val ? want : !n->client->flags.blur;
		n->client->flags.blur_from_rule = true;
		surface_client_set_effect(n->client, EFFECT_BLUR, n->client->flags.blur);
		ipc_ok(a, "Flag changed\n");
	} else if (streq(key, "mica") || streq(key, "acrylic")) {
		if (!n->client) {
			ipc_fail(a, "Focused node has no client\n");
			return;
		}

		bool mica = streq(key, "mica");
		surface_effect_t effect = mica ? EFFECT_MICA : EFFECT_ACRYLIC;
		bool cur = mica ? n->client->flags.mica : n->client->flags.acrylic;
		bool on = val ? want : !cur;

		if (mica)
			n->client->flags.mica = on;
		else
			n->client->flags.acrylic = on;

		surface_client_set_effect(n->client, effect, on);
		ipc_ok(a, "flag changed\n");
	} else if (streq(key, "shadow")) {
		if (!n->client) {
			ipc_fail(a, "Focused node has no client\n");
			return;
		}
		bool on = val ? want : !n->client->flags.shadow;
		n->client->flags.shadow = on;
		n->client->shadow_size = settings.shadow_size;
		n->client->shadow_offset_x = settings.shadow_offset_x;
		n->client->shadow_offset_y = settings.shadow_offset_y;
		memcpy(n->client->shadow_color, settings.shadow_color, sizeof(settings.shadow_color));
		surface_client_set_shadow(n->client, on);
		ipc_ok(a, "flag changed\n");
	} else if (numeric) {
		if (!val) {
			ipc_fail(a, "Flag \"%s\" requires a value\n", key);
			return;
		}
		if (!n->client) {
			ipc_fail(a, "Focused node has no client\n");
			return;
		}

		if (streq(key, "opacity")) {
			double o;
			if (!ipc_double_str(a, val, key, 0, 1, &o))
				return;

			n->client->opacity = (float)o;

			struct wlr_scene_node *node = n->client->view ? &n->client->view->scene_tree->node : NULL;
			if (!node) {
				ipc_fail(a, "No toplevel or xwayland view\n");
				return;
			}
			surface_set_opacity(node, (float)o);
			ipc_okf(a, "%s set\n", key);
		} else {
			double r;
			if (!ipc_double_str(a, val, key, 0, INFINITY, &r))
				return;

			surface_client_set_border_radius(n->client, (float)r);
			ipc_okf(a, "%s set\n", key);
		}
	} else {
		ipc_fail(a, "Unknown flag \"%s\"\n", key);
	}
}

static void node_scratchpad(ipc_args_t *a) {
	output_t *m;
	if (!focused_client(a, &m))
		return;

	scratchpad_add(m->desk->focus);
	ipc_ok(a, "Sent to scratchpad\n");
}

static void node_move(ipc_args_t *a) {
	int dx, dy;
	if (!ipc_int(a, "dx", INT_MIN, INT_MAX, &dx) || !ipc_int(a, "dy", INT_MIN, INT_MAX, &dy))
		return;

	output_t *m;
	node_t *n = focused_client(a, &m);
	if (!n)
		return;

	if (IS_FLOATING(n->client)) {
		struct wlr_box moved = n->client->floating_rectangle;
		moved.x += dx;
		moved.y += dy;
		float_node_set_rect(n, moved);
	} else {
		float_node(m, m->desk, n, NULL);
	}

	ipc_ok(a, "moved\n");
}

typedef struct {
	const char *name;
	int8_t mx, my, mw, mh;
} resize_handle_t;

static const resize_handle_t resize_handles[] = {
	{"northwest", 1, 1, -1, -1},
	{"nw", 1, 1, -1, -1},
	{"left", 1, 1, -1, -1},
	{"north", 0, 1, 0, -1},
	{"n", 0, 1, 0, -1},
	{"northeast", 0, 1, 1, -1},
	{"ne", 0, 1, 1, -1},
	{"east", 0, 0, 1, 0},
	{"e", 0, 0, 1, 0},
	{"right", 0, 0, 1, 0},
	{"southeast", 0, 0, 1, 1},
	{"se", 0, 0, 1, 1},
	{"south", 0, 0, 0, 1},
	{"s", 0, 0, 0, 1},
	{"southwest", 1, 0, -1, 1},
	{"sw", 1, 0, -1, 1},
	{"west", 1, 0, -1, 0},
	{"w", 1, 0, -1, 0},
	{"center", 1, 1, 1, 1},
	{"c", 1, 1, 1, 1},
};

static void node_resize(ipc_args_t *a) {
	const char *handle;
	int dx, dy;
	if (!ipc_need(a, "handle", &handle) || !ipc_int(a, "dx", INT_MIN, INT_MAX, &dx) || !ipc_int(a, "dy",
		INT_MIN, INT_MAX, &dy))
		return;

	const resize_handle_t *h = NULL;
	for (size_t i = 0; i < IPC_ARRAY_LEN(resize_handles); i++) {
		if (streq(handle, resize_handles[i].name)) {
			h = &resize_handles[i];
			break;
		}
	}
	if (!h) {
		ipc_fail(a, "Unknown handle \"%s\", expected a compass direction or \"center\"\n", handle);
		return;
	}

	output_t *m;
	node_t *n = focused_client(a, &m);
	if (!n)
		return;

	bool was_floating = IS_FLOATING(n->client);
	struct wlr_box resized = node_current_rect(n);
	resized.x += h->mx * dx;
	resized.y += h->my * dy;
	resized.width += h->mw * dx;
	resized.height += h->mh * dy;

	if (resized.width < 50)
		resized.width = 50;
	if (resized.height < 50)
		resized.height = 50;

	// transaction sends configure and brings border along
	if (was_floating)
		float_node_set_rect(n, resized);
	else
		float_node(m, m->desk, n, &resized);

	ipc_ok(a, "Resized\n");
}

static void node_activate(ipc_args_t *a) {
	output_t *m;
	node_t *n = ipc_focused(a, &m);
	if (!n)
		return;

	activate_node(m, m->desk, n);
	ipc_ok(a, "Activated\n");
}

static void node_kill(ipc_args_t *a) {
	output_t *m;
	node_t *n = ipc_focused(a, &m);
	if (!n)
		return;

	kill_node(m->desk, n);
	transaction_commit_dirty();
	ipc_ok(a, "killed\n");
}

static void node_to_monitor(ipc_args_t *a) {
	bool follow = false;
	const char *name = NULL;
	const char *arg;
	while ((arg = ipc_peek(a)) != NULL) {
		ipc_take(a);
		if (streq(arg, "--follow"))
			follow = true;
		else if (!name)
			name = arg;
		else {
			ipc_fail(a, "Unexpected argument \"%s\"\n", arg);
			return;
		}
	}

	if (!name) {
		ipc_fail(a, "Missing monitor\n");
		return;
	}

	output_t *target = find_output_by_name(name);
	if (!target) {
		ipc_fail(a, "Monitor \"%s\" not found\n", name);
		return;
	}

	output_t *m = server.focused_output;
	if (!m || !m->desk) {
		ipc_fail(a, "No focused desktop\n");
		return;
	}
	if (m == target) {
		ipc_fail(a, "Already on target monitor\n");
		return;
	}

	node_t *n = m->desk->focus;
	if (!n || !n->client) {
		ipc_fail(a, "Focused node has no client\n");
		return;
	}

	desktop_t *target_desk = target->desk;
	if (!target_desk && !wl_list_empty(&target->desk_list))
		target_desk = wl_container_of(target->desk_list.next, target_desk, link);
	if (!target_desk) {
		ipc_fail(a, "Target monitor has no desktops\n");
		return;
	}

	desktop_t *src_desk = m->desk;
	unlink_and_refocus(n, src_desk, m);
	insert_node(target_desk, n, find_public(target_desk));
	target_desk->focus = n;
	unhide_leaves(target_desk);

	arrange(target, target_desk, true);
	arrange(m, src_desk, src_desk->root != NULL);
	if (follow)
		focus_node(target, target_desk, n);

	ipc_ok(a, "Node sent to monitor\n");
}

static void node_to_node(ipc_args_t *a) {
	output_t *m;
	node_t *n1 = ipc_focused(a, &m);
	if (!n1)
		return;
	if (!n1->client) {
		ipc_fail(a, "Focused node has no client\n");
		return;
	}

	node_t *n2 = find_node_by_id(a, m->desk, "target node");
	if (!n2)
		return;

	if (n1 == n2) {
		ipc_fail(a, "Cannot transfer to self\n");
		return;
	}

	desktop_t *src_desk = m->desk;
	desktop_t *target_desk = src_desk;
	unlink_and_refocus(n1, src_desk, m);

	if (n2->first_child) {
		n1->parent = n2;
		n2->second_child = n1;
	} else {
		n1->parent = n2;
		n2->first_child = n1;
	}

	target_desk->focus = n1;
	if (target_desk == m->desk)
		focus_node(m, target_desk, n1);

	unhide_leaves(target_desk);
	arrange(m, target_desk, true);
	if (src_desk != target_desk)
		arrange(m, src_desk, src_desk->root != NULL);

	ipc_ok(a, "Node sent to node\n");
}

static void node_layer(ipc_args_t *a) {
	long value;
	if (!ipc_enum(a, "layer", layer_values, &value))
		return;

	output_t *m;
	node_t *n = focused_client(a, &m);
	if (!n)
		return;

	n->client->layer = (stack_layer_t)value;
	transaction_commit_dirty();
	ipc_ok(a, "Layer changed\n");
}

static void node_type_split(ipc_args_t *a, output_t *m, node_t *target, long split_type) {
	split_type_t prev_st = target->split_type;
	node_set_split_type(target, (split_type_t)split_type);

	if (prev_st == TYPE_TABBED && split_type != TYPE_TABBED) {
		tabs_destroy(target);
		for (node_t *leaf = first_extrema(target); leaf != NULL && leaf != target; leaf = next_leaf(leaf,
				target)) {
			if (leaf->client == NULL || leaf->client->state == STATE_FLOATING)
				continue;
			if (node_is_minimized(leaf))
				continue;

			leaf->client->flags.shown = true;
			struct wlr_scene_tree *st = client_get_scene_tree(leaf->client);
			if (st)
				wlr_scene_node_set_enabled(&st->node, true);
		}
	}

	arrange(m, m->desk, true);

	// reapply decoration mode for all leaves
	for (node_t *leaf = first_extrema(target); leaf != NULL && leaf != target; leaf = next_leaf(leaf,
			target)) {
		view_t *view = leaf->client ? leaf->client->view : NULL;
		if (view)
			view->impl->set_decorations(view);
	}

	if (m->desk->focus != NULL)
		focus_node(m, m->desk, m->desk->focus);
	ipc_ok(a, "type changed\n");
}

static void node_type_tab(ipc_args_t *a, output_t *m, node_t *n, int forward) {
	node_t *t = tabbed_ancestor(n);
	if (!t) {
		ipc_fail(a, "Focused node not in tab group\n");
		return;
	}

	node_t *next = forward ? tab_next_leaf(t, n) : tab_prev_leaf(t, n);
	if (next) {
		focus_node(m, m->desk, next);
		arrange(m, m->desk, true);
	}
	ipc_okf(a, "%s tab\n", forward ? "Next" : "Prev");
}

static void node_type(ipc_args_t *a) {
	output_t *m;
	node_t *n = ipc_focused(a, &m);
	if (!n)
		return;

	static const cfg_enum_value_t split_types[] = {
		{"tabbed", TYPE_TABBED},
		{"horizontal", TYPE_HORIZONTAL},
		{"vertical", TYPE_VERTICAL},
	};

	const char *name = ipc_peek(a);
	if (!name) {
		// no argument cycles between the two plain split directions
		node_set_split_type(n, (split_type_t)((n->split_type + 1) % 2));
		transaction_commit_dirty();
		ipc_ok(a, "Type changed\n");
		return;
	}

	if (streq(name, "next_tab") || streq(name, "next.tab")) {
		ipc_take(a);
		node_type_tab(a, m, n, 1);
		return;
	}
	if (streq(name, "prev_tab") || streq(name, "prev.tab")) {
		ipc_take(a);
		node_type_tab(a, m, n, 0);
		return;
	}

	long value;
	if (!ipc_enum_opt(a, "type", split_types, &value))
		return;

	node_t *target = n->parent;
	if (!target) {
		ipc_fail(a, "Focused node has no parent\n");
		return;
	}
	node_type_split(a, m, target, value);
}

static void node_ratio(ipc_args_t *a) {
	output_t *m;
	node_t *n = ipc_focused(a, &m);
	if (!n)
		return;

	const char *arg;
	if (!ipc_need(a, "ratio", &arg))
		return;

	double rat;
	if (arg[0] == '+' || arg[0] == '-') {
		double delta;
		if (!ipc_delta_str(a, "ratio", arg, &delta))
			return;

		if (delta > -1 && delta < 1) {
			rat = n->split_ratio + delta;
		} else {
			int max = (n->split_type == TYPE_HORIZONTAL) ? n->rectangle.height : n->rectangle.width;
			rat = ((max * n->split_ratio) + delta) / max;
		}
	} else if (!ipc_double_str(a, arg, "ratio", 0, 1, &rat)) {
		return;
	}

	if (rat <= 0 || rat >= 1) {
		ipc_fail(a, "Ratio must be between 0 and 1\n");
		return;
	}

	node_set_split_ratio(n, rat);
	transaction_commit_dirty();
	ipc_ok(a, "Ratio changed\n");
}

static void node_circulate(ipc_args_t *a) {
	long value;
	if (!ipc_enum(a, "direction", circulate_values, &value))
		return;

	output_t *m;
	node_t *n = ipc_focused(a, &m);
	if (!n)
		return;

	node_t *next = value > 0 ? next_leaf(n, m->desk->root) : prev_leaf(n, m->desk->root);
	if (next) {
		m->desk->focus = next;
		focus_node(m, m->desk, next);
	}

	ipc_ok(a, "Circulated\n");
}

static void node_insert_receptacle(ipc_args_t *a) {
	output_t *m = server.focused_output;
	if (!m || !m->desk) {
		ipc_fail(a, "No focused desktop\n");
		return;
	}

	node_t *n = m->desk->focus;
	node_t *receptacle = make_node(0);
	receptacle->vacant = true;
	receptacle->split_type = TYPE_VERTICAL;
	receptacle->split_ratio = 0.5;

	if (n && !is_leaf(n)) {
		if (n->first_child) {
			receptacle->parent = n;
			n->second_child->parent = receptacle;
			receptacle->first_child = n->second_child;
			n->second_child = receptacle;
		} else {
			n->first_child = receptacle;
			receptacle->parent = n;
		}
	} else if (n) {
		node_t *parent = n->parent;
		if (parent) {
			if (parent->first_child == n)
				parent->first_child = receptacle;
			else
				parent->second_child = receptacle;
			receptacle->parent = parent;

			if (n->split_type == TYPE_VERTICAL) {
				receptacle->first_child = n;
				n->parent = receptacle;
			} else {
				receptacle->second_child = n;
				n->parent = receptacle;
			}
		} else {
			m->desk->root = receptacle;
			receptacle->first_child = n;
			n->parent = receptacle;
		}
		receptacle->split_type = TYPE_VERTICAL;
	} else {
		m->desk->root = receptacle;
	}

	transaction_commit_dirty();
	ipc_ok(a, "Receptacle inserted\n");
}

static void node_presel_dir(ipc_args_t *a) {
	const char *arg;
	if (!ipc_need(a, "direction", &arg))
		return;

	output_t *m;
	node_t *n = ipc_focused(a, &m);
	if (!n)
		return;
	if (n->vacant) {
		ipc_fail(a, "Focused node is a receptacle\n");
		return;
	}

	if (streq(arg, "cancel")) {
		free(n->presel);
		n->presel = NULL;
		ipc_ok(a, "Presel cancelled\n");
		return;
	}

	long dir;
	if (!ipc_enum_names(a, "direction", arg, direction_values, &dir))
		return;

	presel_dir(n, (direction_t)dir);
	transaction_commit_dirty();
	ipc_ok(a, "Presel set\n");
}

static void node_presel_ratio(ipc_args_t *a) {
	double rat;
	if (!ipc_double(a, "ratio", 0, 1, &rat))
		return;
	if (rat <= 0 || rat >= 1) {
		ipc_fail(a, "Ratio must be between 0 and 1\n");
		return;
	}

	output_t *m;
	node_t *n = ipc_focused(a, &m);
	if (!n)
		return;
	if (n->vacant) {
		ipc_fail(a, "Focused node is a receptacle\n");
		return;
	}

	if (!n->presel)
		n->presel = make_presel();
	n->presel->split_ratio = rat;

	transaction_commit_dirty();
	ipc_ok(a, "Presel ratio set\n");
}

static void node_swap(ipc_args_t *a) {
	output_t *m;
	node_t *n1 = ipc_focused(a, &m);
	if (!n1)
		return;

	node_t *n2 = find_node_by_id(a, m->desk, "target node");
	if (!n2)
		return;

	if (n1 == n2) {
		ipc_fail(a, "Cannot swap with self\n");
		return;
	}

	swap_nodes(m, m->desk, n1, m, m->desk, n2);
	m->desk->focus = n2;
	transaction_commit_dirty();
	ipc_ok(a, "Swapped\n");
}

const ipc_sub_t node_subs[] = {
	IPC_SUB("-f", "--focus", "node -f | --focus", node_focus),
	IPC_SUB("-c", "--close", "node -c | --close", node_close),
	IPC_SUB("-t", "--state", "node -t | --state <tiled|floating|fullscreen|maximized|minimized>",
		node_state),
	IPC_SUB("-d", "--to-desktop", "node -d | --to-desktop <desktop>", node_to_desktop),
	{"-g", "--flag", NULL, "node -g | --flag <key>[=true|false]", node_flag, node_flag_key_matches},
	IPC_SUB("-S", "--scratchpad", "node -S | --scratchpad", node_scratchpad),
	IPC_SUB("-v", "--move", "node -v | --move <dx> <dy>", node_move),
	IPC_SUB("-z", "--resize", "node -z | --resize <handle> <dx> <dy>", node_resize),
	IPC_SUB("-a", "--activate", "node -a | --activate", node_activate),
	IPC_SUB("-k", "--kill", "node -k | --kill", node_kill),
	IPC_SUB("-m", "--to-monitor", "node -m | --to-monitor <monitor> [--follow]", node_to_monitor),
	IPC_SUB("-n", "--to-node", "node -n | --to-node <id>", node_to_node),
	IPC_SUB("-l", "--layer", "node -l | --layer <below|normal|above>", node_layer),
	IPC_SUB("-y", "--type", "node -y | --type [tabbed|horizontal|vertical|next_tab|prev_tab]",
		node_type),
	IPC_SUB("-r", "--ratio", "node -r | --ratio [+|-]<ratio|px>", node_ratio),
	IPC_SUB("-C", "--circulate", "node -C | --circulate <forward|backward>", node_circulate),
	IPC_SUB("-i", "--insert-receptacle", "node -i | --insert-receptacle", node_insert_receptacle),
	IPC_SUB("-p", "--presel-dir", "node -p | --presel-dir <direction|cancel>", node_presel_dir),
	IPC_SUB("-o", "--presel-ratio", "node -o | --presel-ratio <ratio>", node_presel_ratio),
	IPC_SUB("-s", "--swap", "node -s | --swap <id>", node_swap),
	IPC_SUB_END,
};

void ipc_cmd_node(ipc_args_t *a) {
	if (!ipc_sub_dispatch(a, node_subs))
		ipc_fail_unknown(a, node_subs);
}
