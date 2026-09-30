#include "ipc/args.h"
#include "ipc/ipc.h"
#include "ipc/registry.h"
#include "output/config.h"
#include "output/output.h"
#include "protocol/xdg_toplevel.h"
#include "server.h"
#include "tree.h"

static void wm_dump_state(ipc_args_t *a) {
	static const cfg_enum_value_t focus_on_activate_values[] = {
		{"focus", FOCUS_ON_ACTIVATE_FOCUS},
		{"none", FOCUS_ON_ACTIVATE_NONE},
		{"smart", FOCUS_ON_ACTIVATE_SMART},
		{"urgent", FOCUS_ON_ACTIVATE_URGENT},
		IPC_ENUM_END,
	};

	static const cfg_enum_value_t follows_values[] = {
		{"no", FOLLOWS_NO},
		{"yes", FOLLOWS_YES},
		{"always", FOLLOWS_ALWAYS},
		IPC_ENUM_END,
	};

	IPC_REPLY(b);

	ipc_buff(&b, "{\n  \"monitors\": [\n");

	output_t *o;
	bool first_mon = true;
	wl_list_for_each(o, &mon_list, link) {
		if (!first_mon)
			ipc_buff(&b, ",\n");
		first_mon = false;
		ipc_buff(&b, "    {\"name\": \"%s\", \"id\": %u, \"rect\": {\"x\": %d, \"y\": %d, "
			"\"width\": %d, \"height\": %d}}", o->name, o->id, o->rectangle.x, o->rectangle.y,
				o->rectangle.width, o->rectangle.height);
	}
	ipc_buff(&b, "\n  ],\n");

	ipc_buff(&b, "  \"settings\": {\n");
	ipc_buff(&b, "    \"border_width\": %d,\n", settings.border_width);
	ipc_buff(&b, "    \"window_gap\": %d,\n", settings.window_gap);
	ipc_buff(&b, "    \"split_ratio\": %.2f,\n", settings.split_ratio);
	ipc_buff(&b, "    \"automatic_scheme\": %d,\n", settings.automatic_scheme);
	ipc_buff(&b, "    \"smart_gaps\": %s,\n", settings.smart_gaps ? "true" : "false");
	ipc_buff(&b, "    \"smart_borders\": %s,\n", settings.smart_borders ? "true" : "false");
	ipc_buff(&b, "    \"respect_tiled_min_size\": %s,\n",
		settings.respect_tiled_min_size ? "true" : "false");
	ipc_buff(&b, "    \"focus_wrapping\": %s,\n", settings.focus_wrapping ? "true" : "false");
	const char *focus_on_activate = ipc_enum_name(focus_on_activate_values,
		settings.focus_on_activate);
	const char *follows = ipc_enum_name(follows_values, settings.focus_follows_mouse);

	ipc_buff(&b, "    \"focus_on_activate\": \"%s\",\n", focus_on_activate ? focus_on_activate : "?");
	ipc_buff(&b, "    \"focus_follows_mouse\": \"%s\",\n", follows ? follows : "?");
	ipc_buff(&b, "    \"record_history\": %s,\n", settings.record_history ? "true" : "false");
	ipc_buff(&b, "    \"shadow_size\": %.1f,\n", settings.shadow_size);
	ipc_buff(&b, "    \"shadow_offset_x\": %.1f,\n", settings.shadow_offset_x);
	ipc_buff(&b, "    \"shadow_offset_y\": %.1f,\n", settings.shadow_offset_y);
	ipc_buff(&b, "    \"shadow_color\": [%.3f, %.3f, %.3f, %.3f]\n", settings.shadow_color[0],
		settings.shadow_color[1], settings.shadow_color[2], settings.shadow_color[3]);
	ipc_buff(&b, "  }\n}\n");

	ipc_buf_send(a, &b);
}

static void wm_load_state(ipc_args_t *a) {
	ipc_ok(a, "Not implemented\n");
}

static void wm_add_monitor(ipc_args_t *a) {
	const char *name;
	if (!ipc_need(a, "monitor name", &name))
		return;

	if (find_output_by_name(name)) {
		ipc_fail(a, "Monitor \"%s\" already exists\n", name);
		return;
	}

	struct output_config *oc = output_config_create(name);
	if (!oc) {
		ipc_fail(a, "Failed to add monitor\n");
		return;
	}

	output_config_add(oc);
	ipc_ok(a, "monitor config added\n");
}

static void wm_reorder_monitors(ipc_args_t *a) {
	const char *name;
	if (!ipc_need(a, "monitor name", &name))
		return;

	ipc_ok(a, "Unimplemented\n");
}

static void wm_adopt_orphans(ipc_args_t *a) {
	int adopted = 0;
	view_t *view, *tmp;
	wl_list_for_each_safe(view, tmp, &server.views, link) {
		xdg_toplevel_t *xdg = view_to_xdg(view);
		if (!view->node && xdg && xdg->xdg_toplevel && view->mapped) {
			xdg_toplevel_adopt(xdg);
			adopted++;
		}
	}

	ipc_okf(a, "Adopted %d orphans\n", adopted);
}

static void wm_get_status(ipc_args_t *a) {
	IPC_REPLY(b);

	int output_count = 0;
	output_t *m;
	wl_list_for_each(m, &mon_list, link)
		output_count++;

	ipc_buff(&b, "Status: running\nmonitors: %d\n", output_count);

	m = server.focused_output;
	if (m && m->desk) {
		ipc_buff(&b, "focused_monitor: %s\nfocused_desktop: %s\n", m->name, m->desk->name);
		if (m->desk->focus)
			ipc_buff(&b, "focused_node: %u\n", m->desk->focus->id);
	}

	ipc_buf_send(a, &b);
}

static void wm_record_history(ipc_args_t *a) {
	if (!ipc_toggle(a, &settings.record_history))
		return;

	ipc_okf(a, "record-history %s\n", settings.record_history ? "enabled" : "disabled");
}

static void wm_restart(ipc_args_t *a) {
	server_restart();
	ipc_ok(a, "Restarting\n");
}

const ipc_sub_t wm_subs[] = {
	IPC_SUB("-d", "--dump-state", "wm -d | --dump-state", wm_dump_state),
	IPC_SUB("-l", "--load-state", "wm -l | --load-state <file>", wm_load_state),
	IPC_SUB("-a", "--add-monitor", "wm -a | --add-monitor <name>", wm_add_monitor),
	IPC_SUB("-O", "--reorder-monitors", "wm -O | --reorder-monitors <names...>", wm_reorder_monitors),
	IPC_SUB("-o", "--adopt-orphans", "wm -o | --adopt-orphans", wm_adopt_orphans),
	IPC_SUB("-g", "--get-status", "wm -g | --get-status", wm_get_status),
	IPC_SUB("-h", "--record-history", "wm -h | --record-history [true|false]", wm_record_history),
	IPC_SUB("-r", "--restart", "wm -r | --restart", wm_restart),
	IPC_SUB_END,
};

void ipc_cmd_wm(ipc_args_t *a) {
	if (!ipc_sub_dispatch(a, wm_subs))
		ipc_fail_unknown(a, wm_subs);
}
