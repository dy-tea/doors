#include "ipc/args.h"
#include "ipc/helpers.h"
#include "ipc/ipc.h"
#include "ipc/json.h"
#include "ipc/registry.h"
#include "output/config.h"
#include "output/output.h"
#include "protocol/workspace.h"
#include "protocol/xdg_toplevel.h"
#include "server.h"
#include "settings.h"
#include "transaction.h"
#include "tree.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wlr/types/wlr_output_layout.h>

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

	static const cfg_enum_value_t automatic_scheme_values[] = {
		{"longest_side", SCHEME_LONGEST_SIDE},
		{"alternate", SCHEME_ALTERNATE},
		{"spiral", SCHEME_SPIRAL},
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
	const char *scheme = ipc_enum_name(automatic_scheme_values, settings.automatic_scheme);
	ipc_buff(&b, "    \"automatic_scheme\": \"%s\",\n", scheme ? scheme : "?");
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

static bool monitors_reorder(char **names, int count) {
	for (int i = 0; i < count; i++) {
		if (find_output_by_name(names[i]) == NULL)
			return false;
	}

	struct wl_list ordered;
	wl_list_init(&ordered);

	for (int i = 0; i < count; i++) {
		output_t *target = find_output_by_name(names[i]);

		wl_list_remove(&target->link);
		wl_list_insert(ordered.prev, &target->link);
	}

	output_t *m, *tmp;
	wl_list_for_each_safe(m, tmp, &mon_list, link) {
		wl_list_remove(&m->link);
		wl_list_insert(ordered.prev, &m->link);
	}

	wl_list_init(&mon_list);
	wl_list_insert_list(mon_list.prev, &ordered);

	return true;
}

// what is wrong with a list of monitor names
typedef enum {
	MONITORS_OK,
	MONITORS_UNKNOWN,
	MONITORS_DUPLICATE,
} monitors_status_t;

// every name must exist and may only be given once
static monitors_status_t monitors_check_names(char **names, int count, const char **bad) {
	for (int i = 0; i < count; i++) {
		if (find_output_by_name(names[i]) == NULL) {
			*bad = names[i];
			return MONITORS_UNKNOWN;
		}

		for (int j = 0; j < i; j++) {
			if (streq(names[i], names[j])) {
				*bad = names[i];
				return MONITORS_DUPLICATE;
			}
		}
	}

	return MONITORS_OK;
}


#define STATE_MAX_SIZE (1024 * 1024)

static char *state_read_file(const char *path, char *err, size_t errsz) {
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd == -1) {
		snprintf(err, errsz, "cannot open \"%s\": %s\n", path, strerror(errno));
		return NULL;
	}

	struct stat info;
	if (fstat(fd,
			&info) == -1 || !S_ISREG(info.st_mode) || info.st_size <= 0 || info.st_size > STATE_MAX_SIZE) {
		snprintf(err, errsz, "\"%s\" is not a regular file of a sane size\n", path);
		close(fd);
		return NULL;
	}

	char *text = malloc((size_t)info.st_size + 1);
	if (text == NULL) {
		snprintf(err, errsz, "out of memory\n");
		close(fd);
		return NULL;
	}

	size_t len = 0;
	while (len < (size_t)info.st_size) {
		ssize_t n = read(fd, text + len, (size_t)info.st_size - len);
		if (n == -1 && errno == EINTR)
			continue;
		if (n <= 0) {
			snprintf(err, errsz, "cannot read \"%s\"\n", path);
			free(text);
			close(fd);
			return NULL;
		}
		len += (size_t)n;
	}
	close(fd);

	text[len] = '\0';
	return text;
}

// a state value in the shape `config <name> <value>` takes it
static bool state_value_arg(const json_value_t *value, char *buf, size_t bufsz) {
	switch (value->type) {
	case JSON_BOOL:
		snprintf(buf, bufsz, "%s", value->value.boolean ? "true" : "false");
		return true;
	case JSON_NUMBER:
		snprintf(buf, bufsz, "%.10g", value->value.number);
		return true;
	case JSON_STRING:
		if (strlen(value->value.string) >= bufsz)
			return false;
		snprintf(buf, bufsz, "%s", value->value.string);
		return true;
	case JSON_ARRAY: {
		// colours are dumped as a list of components
		if (value->value.list.count < 3 || value->value.list.count > 4)
			return false;

		ipc_buf_t b;
		ipc_buf_init(&b, buf, bufsz);
		for (size_t i = 0; i < value->value.list.count; i++) {
			const json_value_t *item = value->value.list.items[i];
			if (item->type != JSON_NUMBER)
				return false;
			ipc_buff(&b, "%s%.6g", b.len > 0 ? " " : "", item->value.number);
		}
		return !b.truncated;
	}
	case JSON_NULL:
	case JSON_OBJECT:
		break;
	}

	return false;
}

static bool state_load_settings(const json_value_t *object, ipc_args_t *a, int *applied) {
	if (object == NULL)
		return true;

	if (object->type != JSON_OBJECT) {
		ipc_fail(a, "\"settings\" must be an object\n");
		return false;
	}

	for (size_t i = 0; i < object->value.list.count; i++) {
		const char *name = object->value.list.keys[i];
		const json_value_t *value = object->value.list.items[i];

		char arg[256];
		if (!state_value_arg(value, arg, sizeof(arg))) {
			ipc_fail(a, "settings.%s: unsupported value\n", name);
			return false;
		}

		char err[256];
		if (!config_apply_value(name, arg, err, sizeof(err))) {
			ipc_fail(a, "%s", err);
			return false;
		} (*applied)++;
	}

	return true;
}

typedef struct {
	char name[SMALEN];
	bool positioned;
	int x, y;
} state_monitor_t;

static bool state_load_monitors(const json_value_t *monitors, ipc_args_t *a, int *restored) {
	if (monitors == NULL)
		return true;

	if (monitors->type != JSON_ARRAY) {
		ipc_fail(a, "\"monitors\" must be an array\n");
		return false;
	}

	size_t count = monitors->value.list.count;
	if (count == 0)
		return true;

	state_monitor_t *entries = calloc(count, sizeof(state_monitor_t));
	char **names = calloc(count, sizeof(char *));
	if (entries == NULL || names == NULL) {
		free(entries);
		free(names);
		ipc_fail(a, "Out of memory\n");
		return false;
	}

	size_t found = 0;
	bool ok = true;
	for (size_t i = 0; i < count; i++) {
		const json_value_t *item = monitors->value.list.items[i];
		const char *name = item->type == JSON_OBJECT ? json_get_string(json_get(item, "name")) : NULL;
		if (name == NULL || strlen(name) >= SMALEN) {
			ipc_fail(a, "monitors[%zu]: missing \"name\"\n", i);
			ok = false;
			break;
		}

		snprintf(entries[i].name, sizeof(entries[i].name), "%s", name);

		double x, y;
		const json_value_t *rect = json_get(item, "rect");
		if (rect != NULL && json_get_number(json_get(rect, "x"), &x) && json_get_number(json_get(rect,
				"y"), &y)) {
			entries[i].x = (int)lround(x);
			entries[i].y = (int)lround(y);
			entries[i].positioned = true;
		}

		if (find_output_by_name(entries[i].name) != NULL)
			names[found++] = entries[i].name;
	}

	// the order of the file is the order the monitors get listed in
	if (found > 0) {
		if (monitors_reorder(names, (int)found))
			(*restored) += (int)found;
		else {
			ipc_fail(a, "Failed to reorder monitors\n");
			ok = false;
		}
	}

	for (size_t i = 0; ok && i < found; i++) {
		state_monitor_t *entry = &entries[i];
		if (!entry->positioned)
			continue;

		output_t *m = find_output_by_name(entry->name);
		if (m->rectangle.x == entry->x && m->rectangle.y == entry->y)
			continue;

		struct output_config *oc = output_config_find(entry->name);
		if (oc == NULL) {
			oc = output_config_create(entry->name);
			if (oc == NULL) {
				ipc_fail(a, "Out of memory\n");
				ok = false;
				break;
			}
			output_config_add(oc);
		}

		oc->x = entry->x;
		oc->y = entry->y;
		output_config_apply(oc);
		output_update_geometry(m);
	}

	free(entries);
	free(names);

	if (ok && found > 0)
		workspace_sync();

	return ok;
}

static void wm_load_state(ipc_args_t *a) {
	const char *path;
	if (!ipc_need(a, "state file", &path))
		return;

	char err[512];
	char *text = state_read_file(path, err, sizeof(err));
	if (text == NULL) {
		ipc_fail(a, "%s", err);
		return;
	}

	json_value_t *root = json_parse(text, err, sizeof(err));
	free(text);
	if (root == NULL) {
		ipc_fail(a, "Cannot parse \"%s\": %s\n", path, err);
		return;
	}

	if (root->type != JSON_OBJECT) {
		json_free(root);
		ipc_fail(a, "Expected a state object\n");
		return;
	}

	int applied_monitors = 0;
	int applied_settings = 0;

	bool ok = state_load_monitors(json_get(root, "monitors"), a,
		&applied_monitors) && state_load_settings(json_get(root, "settings"), a, &applied_settings);

	json_free(root);

	if (!ok)
		return;

	transaction_commit_dirty();
	ipc_okf(a, "State loaded (%d monitors, %d settings)\n", applied_monitors, applied_settings);
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
	if (ipc_left(a) <= 0) {
		ipc_fail(a, "Missing monitor name\n");
		return;
	}

	char **names = a->v;
	int count = a->n;

	const char *bad;
	switch (monitors_check_names(names, count, &bad)) {
	case MONITORS_OK:
		break;
	case MONITORS_DUPLICATE:
		ipc_fail(a, "Monitor \"%s\" given twice\n", bad);
		return;
	case MONITORS_UNKNOWN:
		ipc_fail(a, "Monitor \"%s\" not found\n", bad);
		return;
	}

	monitors_reorder(names, count);

	// the workspace protocol hands out workspaces in monitor order
	workspace_sync();

	ipc_ok(a, "Monitors reordered\n");
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
