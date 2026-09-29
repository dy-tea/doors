#include "input/keyboard.h"
#include "ipc/args.h"
#include "ipc/helpers.h"
#include "ipc/registry.h"
#include "layout/layout.h"
#include "output/output.h"
#include "protocol/workspace.h"
#include "server.h"
#include "transaction.h"
#include "tree.h"

static void swap_desktops(desktop_t *d0, desktop_t *d1) {
	struct wl_list *a = &d0->link;
	struct wl_list *b = &d1->link;

	if (a == b)
		return;

	struct wl_list *a_prev = a->prev;
	struct wl_list *a_next = a->next;
	struct wl_list *b_prev = b->prev;
	struct wl_list *b_next = b->next;

	if (a->next == b) {
		a_prev->next = b;
		b->prev = a_prev;
		b->next = a;
		a->prev = b;
		a->next = b_next;
		b_next->prev = a;
	} else if (b->next == a) {
		b_prev->next = a;
		a->prev = b_prev;
		a->next = b;
		b->prev = a;
		b->next = a_next;
		a_next->prev = b;
	} else {
		a_prev->next = b;
		b->prev = a_prev;
		a_next->prev = b;
		b->next = a_next;
		b_prev->next = a;
		a->prev = b_prev;
		b_next->prev = a;
		a->next = b_next;
	}
}

static const cfg_enum_value_t layout_values[] = {
	{"tiled", LAYOUT_TILED},
	{"monocle", LAYOUT_MONOCLE},
	{"scroller", LAYOUT_SCROLLER},
	{"master_stack", LAYOUT_MASTER_STACK},
	{"floating", LAYOUT_FLOATING},
	IPC_ENUM_END,
};

static const cfg_enum_value_t bubble_values[] = {
	{"up", -1},
	{"prev", -1},
	{"down", 1},
	{"next", 1},
	IPC_ENUM_END,
};

// desktop <name> switches focus without a subcommand
static void desk_focus(ipc_args_t *a, desktop_t *desk) {
	workspace_switch_to_desktop(desk->name);
	ipc_ok(a, "focused\n");
}

// desktop [-f] [next|last|prev]
static void desk_focus_relative(ipc_args_t *a, const char *rel) {
	output_t *m;
	desktop_t *desk = ipc_focused_desk(a, &m);
	if (!desk)
		return;

	if (rel == NULL) {
		desk_focus(a, desk);
	} else if (streq(rel, "last")) {
		focus_last_desktop();
		ipc_ok(a, "Focused\n");
	} else if (streq(rel, "next") || streq(rel, "next.local")) {
		focus_next_desktop();
		ipc_ok(a, "Focused\n");
	} else if (streq(rel, "prev") || streq(rel, "prev.local") || streq(rel, "previous")) {
		focus_prev_desktop();
		ipc_ok(a, "Focused\n");
	} else {
		desk_focus(a, desk);
	}
}

static void desk_layout(ipc_args_t *a) {
	output_t *m;
	desktop_t *desk = ipc_focused_desk(a, &m);
	if (!desk)
		return;

	long value;
	if (!ipc_enum(a, "layout", layout_values, &value))
		return;

	const char *all = ipc_peek(a);
	if (all && streq(all, "--all")) {
		ipc_take(a);
		output_t *o;
		wl_list_for_each(o, &mon_list, link) {
			desktop_t *d;
			wl_list_for_each(d, &o->desk_list, link) {
				layout_set(d, (layout_t)value);
				arrange(o, d, true);
				ipc_put_status(SUB_MASK_DESKTOP_LAYOUT, "desktop_layout[%s,%c]\n", d->name,
					layout_to_char(d->layout));
			}
		}
	} else {
		layout_set(desk, (layout_t)value);
		arrange(m, desk, true);
		if (desk->focus != NULL)
			focus_node(m, desk, desk->focus);
		ipc_put_status(SUB_MASK_DESKTOP_LAYOUT, "desktop_layout[%s,%c]\n", desk->name,
			layout_to_char(desk->layout));
	}

	ipc_ok(a, "Layout changed\n");
}

static void desk_rename(ipc_args_t *a) {
	output_t *m;
	desktop_t *desk = ipc_focused_desk(a, &m);
	if (!desk)
		return;

	if (!ipc_str(a, "name", desk->name, SMALEN))
		return;

	ipc_put_status(SUB_MASK_DESKTOP_CHANGE, "desktop_change[%s]\n", desk->name);
	transaction_commit_dirty();
	ipc_ok(a, "Renamed\n");
}

static void desk_swap(ipc_args_t *a) {
	output_t *mon;
	desktop_t *desk = ipc_focused_desk(a, &mon);
	if (!desk)
		return;

	const char *name;
	if (!ipc_need(a, "target desktop", &name))
		return;

	desktop_t *target = find_desktop_by_name_in_monitor(mon, name);
	if (!target) {
		ipc_fail(a, "Target desktop \"%s\" not found\n", name);
		return;
	}
	if (target == desk) {
		ipc_fail(a, "Cannot swap with self\n");
		return;
	}

	output_t *m0 = desk->output;
	output_t *m1 = target->output;
	swap_desktops(desk, target);

	if (m0 != m1) {
		desk->output = m1;
		target->output = m0;
	}

	if (mon->desk == desk)
		mon->desk = target;
	else if (mon->desk == target)
		mon->desk = desk;

	transaction_commit_dirty();
	ipc_ok(a, "Swapped\n");
}

static void desk_remove(ipc_args_t *a) {
	output_t *mon;
	desktop_t *desk = ipc_focused_desk(a, &mon);
	if (!desk)
		return;

	if (wl_list_length(&mon->desk_list) == 1) {
		ipc_fail(a, "Cannot remove the only desktop\n");
		return;
	}

	desktop_t *next = desk->link.next != &mon->desk_list ? wl_container_of(desk->link.next, desk,
		link) : NULL;
	desktop_t *prev = desk->link.prev != &mon->desk_list ? wl_container_of(desk->link.prev, desk,
		link) : NULL;

	if (desk->link.prev == &mon->desk_list && mon->desk)
		mon->desk = next;

	wl_list_remove(&desk->link);

	if (mon->desk == desk) {
		mon->desk = next ? next : prev;
		if (mon->desk)
			focus_node(mon, mon->desk, mon->desk->focus);
	}
	if (mon->last_desk == desk)
		mon->last_desk = next ? next : prev;

	ipc_put_status(SUB_MASK_DESKTOP_REMOVE, "desktop_remove[%s]\n", desk->name);
	desktop_minimized_clear(desk);
	free(desk);
	transaction_commit_dirty();
	ipc_ok(a, "Removed\n");
}

static void desk_bubble(ipc_args_t *a) {
	output_t *mon;
	desktop_t *desk = ipc_focused_desk(a, &mon);
	if (!desk)
		return;

	long dir;
	if (!ipc_enum(a, "direction", bubble_values, &dir))
		return;

	if (dir < 0 && desk->link.prev != &mon->desk_list)
		swap_desktops(desk, wl_container_of(desk->link.prev, desk, link));
	else if (dir > 0 && desk->link.next != &mon->desk_list)
		swap_desktops(desk, wl_container_of(desk->link.next, desk, link));

	transaction_commit_dirty();
	ipc_ok(a, "Bubbled\n");
}

static void desk_to_monitor(ipc_args_t *a) {
	output_t *mon;
	desktop_t *desk = ipc_focused_desk(a, &mon);
	if (!desk)
		return;

	const char *name;
	if (!ipc_need(a, "monitor", &name))
		return;

	output_t *target = find_output_by_name(name);
	if (!target) {
		ipc_fail(a, "Monitor \"%s\" not found\n", name);
		return;
	}
	if (desk->output == target) {
		ipc_fail(a, "Already on target monitor\n");
		return;
	}
	if (wl_list_empty(&target->desk_list)) {
		ipc_fail(a, "Target monitor has no desktop\n");
		return;
	}

	output_t *src_mon = desk->output;
	wl_list_remove(&desk->link);
	wl_list_insert(target->desk_list.prev, &desk->link);
	desk->output = target;

	if (src_mon->desk == desk) {
		src_mon->desk = wl_list_empty(&src_mon->desk_list) ? NULL :
			wl_container_of(src_mon->desk_list.next, desk, link);
		if (src_mon->desk)
			focus_node(src_mon, src_mon->desk, src_mon->desk->focus);
	}

	transaction_commit_dirty();
	ipc_ok(a, "Desktop moved to monitor\n");
}

static void desk_focus_sub(ipc_args_t *a) {
	desk_focus_relative(a, ipc_peek(a));
	ipc_end(a);
}

const ipc_sub_t desktop_subs[] = {
	IPC_SUB("-f", "--focus", "desktop [-f|--focus] [next|last|prev]", desk_focus_sub),
	IPC_SUB("-l", "--layout", "desktop -l|--layout <layout> [--all]", desk_layout),
	IPC_SUB("-n", "--rename", "desktop -n|--rename <name>", desk_rename),
	IPC_SUB("-s", "--swap", "desktop -s|--swap <desktop>", desk_swap),
	IPC_SUB("-r", "--remove", "desktop -r|--remove", desk_remove),
	IPC_SUB("-b", "--bubble", "desktop -b|--bubble <up|down>", desk_bubble),
	IPC_SUB("-m", "--to-monitor", "desktop -m|--to-monitor <monitor>", desk_to_monitor),
	IPC_SUB_END,
};

// desktop next|last|prev move focus without naming a desktop
static bool is_relative_word(const char *arg) {
	return streq(arg, "next") || streq(arg, "last") || streq(arg, "prev") || streq(arg, "previous");
}

void ipc_cmd_desktop(ipc_args_t *a) {
	const char *arg = ipc_peek(a);
	if (arg && is_relative_word(arg)) {
		desk_focus_relative(a, ipc_take(a));
		return;
	}

	output_t *mon = server.focused_output;
	if (arg && arg[0] != '-') {
		desktop_t *desk = find_desktop_by_name_in_monitor(mon, arg);
		if (!desk) {
			char *end;
			long idx = strtol(arg, &end, 10);
			if (*end == '\0' && idx >= 1 && idx <= 10) {
				workspace_switch_to_desktop_by_index(idx - 1);
				ipc_ok(a, "Focused\n");
				return;
			}
			ipc_fail(a, "Unknown desktop \"%s\"\n", arg);
			return;
		}
		ipc_take(a);

		// with nothing else to do the command was a plain focus request
		if (!ipc_peek(a)) {
			desk_focus(a, desk);
			return;
		}
	}

	if (!ipc_sub_dispatch(a, desktop_subs))
		ipc_fail_unknown(a, desktop_subs);
}
