#include "bezier.h"
#include "input/keyboard.h"
#include "ipc/args.h"
#include "ipc/registry.h"
#include "spring.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>

static const str_pair_t focus_dirs[] = {
	{"west", BIND_FOCUS_WEST},
	{"w", BIND_FOCUS_WEST},
	{"east", BIND_FOCUS_EAST},
	{"e", BIND_FOCUS_EAST},
	{"north", BIND_FOCUS_NORTH},
	{"n", BIND_FOCUS_NORTH},
	{"south", BIND_FOCUS_SOUTH},
	{"s", BIND_FOCUS_SOUTH},
	IPC_ENUM_END,
};

static const str_pair_t swap_dirs[] = {
	{"west", BIND_SWAP_WEST},
	{"w", BIND_SWAP_WEST},
	{"east", BIND_SWAP_EAST},
	{"e", BIND_SWAP_EAST},
	{"north", BIND_SWAP_NORTH},
	{"n", BIND_SWAP_NORTH},
	{"south", BIND_SWAP_SOUTH},
	{"s", BIND_SWAP_SOUTH},
	IPC_ENUM_END,
};

static const str_pair_t resize_dirs[] = {
	{"left", BIND_RESIZE_LEFT},
	{"right", BIND_RESIZE_RIGHT},
	{"up", BIND_RESIZE_UP},
	{"down", BIND_RESIZE_DOWN},
	IPC_ENUM_END,
};

// names resolve across all bind actions, so each command only accepts its own range
static bool is_focus_action(bind_action_t a) {
	return a >= BIND_FOCUS_NORTH && a <= BIND_FOCUS_EAST;
}

static bool is_swap_action(bind_action_t a) {
	return a >= BIND_SWAP_NORTH && a <= BIND_SWAP_EAST;
}

static bool is_resize_action(bind_action_t a) {
	return a >= BIND_RESIZE_LEFT && a <= BIND_RESIZE_DOWN;
}

static void action_direction(ipc_args_t *a, const char *success, const str_pair_t *dirs,
		size_t dir_count, bool (*in_range)(bind_action_t)) {
	const char *dir;
	if (!ipc_need(a, "direction", &dir))
		return;

	bind_action_t action = BIND_NONE;
	const str_pair_t *match = str_table_find(dirs, dir);
	if (match) {
		action = (bind_action_t)match->value;
	} else {
		bind_action_t full = bind_action_from_alias(dir);
		if (full != BIND_NONE && in_range(full))
			action = full;
	}

	if (action == BIND_NONE) {
		ipc_buf_t b;
		char list[128];
		ipc_buf_init(&b, list, sizeof(list));
		IPC_FORMAT_NAMES(&b, dirs, dir_count);
		ipc_fail(a, "Unknown direction \"%s\", expected one of: %s\n", dir, list);
		return;
	}

	execute_bind_action(action);
	ipc_okf(a, "%s\n", success);
}

static void act_toggle(ipc_args_t *a) {
	static const struct {
		const char *name;
		void (*fn)(void);
	} props[] = {
		{"floating", toggle_floating},
		{"fullscreen", toggle_fullscreen},
		{"maximize", toggle_maximize},
		{"minimize", toggle_minimize},
		{"restore_minimized", restore_minimized},
		{"pseudo_tiled", toggle_pseudo_tiled},
		{"monocle", toggle_monocle},
		{"floating_layout", toggle_floating_layout},
		{"block_out_from_screenshare", toggle_block_out_from_screenshare},
	};

	const char *name;
	if (!ipc_need(a, "property", &name))
		return;

	for (size_t i = 0; i < IPC_ARRAY_LEN(props); i++) {
		if (!streq(name, props[i].name))
			continue;

		props[i].fn();
		ipc_ok(a, "toggled\n");
		return;
	}

	ipc_buf_t b;
	char list[256];
	ipc_buf_init(&b, list, sizeof(list));
	IPC_FORMAT_NAMES(&b, props, IPC_ARRAY_LEN(props));
	ipc_fail(a, "Unknown property \"%s\", expected one of: %s\n", name, list);
}

static void act_send(ipc_args_t *a) {
	static const str_pair_t targets[] = {
		{"next", 1},
		{"prev", -1},
		{"previous", -1},
		IPC_ENUM_END,
	};

	long dir;
	if (!ipc_enum(a, "direction", targets, &dir))
		return;

	if (dir > 0)
		send_to_next_desktop();
	else
		send_to_prev_desktop();
	ipc_ok(a, "sent\n");
}

static void act_bezier(ipc_args_t *a) {
	const char *name;
	double p1x, p1y, p2x, p2y;
	if (!ipc_need(a, "name", &name) || !ipc_double(a, "p1x", -1e6, 1e6, &p1x) || !ipc_double(a, "p1y",
		-1e6, 1e6, &p1y) || !ipc_double(a, "p2x", -1e6, 1e6, &p2x) || !ipc_double(a, "p2y", -1e6, 1e6,
		&p2y))
		return;

	if (a->dry_run) {
		if (name[0] == '\0')
			ipc_fail(a, "Failed to add bezier curve\n");
		else
			ipc_ok(a, "Bezier curve would be added\n");
		return;
	}

	if (bezier_add(name, p1x, p1y, p2x, p2y))
		ipc_ok(a, "Bezier curve added\n");
	else
		ipc_fail(a, "Failed to add bezier curve\n");
}

static void act_spring(ipc_args_t *a) {
	const char *name;
	double stiffness, damping, mass = 1.0;
	if (!ipc_need(a, "name", &name) || !ipc_double(a, "stiffness", 1e-9, 1e9,
		&stiffness) || !ipc_double(a, "damping", 0, 1e9, &damping))
		return;

	if (ipc_peek(a) && !ipc_double(a, "mass", 1e-9, 1e9, &mass))
		return;

	double value_eps = SPRING_EPSILON_DEFAULT;
	double velocity_eps = SPRING_EPSILON_DEFAULT;
	if (ipc_peek(a) && !ipc_double(a, "value_eps", 0, 1e9, &value_eps))
		return;
	if (ipc_peek(a) && !ipc_double(a, "velocity_eps", 0, 1e9, &velocity_eps))
		return;

	if (a->dry_run) {
		if (name[0] == '\0')
			ipc_fail(a, "Failed to add spring curve\n");
		else
			ipc_ok(a, "Spring curve would be added\n");
		return;
	}

	if (spring_add(name, stiffness, damping, mass, value_eps, velocity_eps))
		ipc_ok(a, "Spring curve added\n");
	else
		ipc_fail(a, "Failed to add spring curve\n");
}

void ipc_cmd_actions(ipc_args_t *a) {
	const char *cmd = a->cmd;

	if (streq(cmd, "focus")) {
		action_direction(a, "focused", focus_dirs, IPC_ARRAY_LEN(focus_dirs) - 1, is_focus_action);
	} else if (streq(cmd, "swap")) {
		action_direction(a, "swapped", swap_dirs, IPC_ARRAY_LEN(swap_dirs) - 1, is_swap_action);
	} else if (streq(cmd, "resize")) {
		action_direction(a, "resized", resize_dirs, IPC_ARRAY_LEN(resize_dirs) - 1, is_resize_action);
	} else if (streq(cmd, "toggle")) {
		act_toggle(a);
	} else if (streq(cmd, "send")) {
		act_send(a);
	} else if (streq(cmd, "bezier")) {
		act_bezier(a);
	} else {
		act_spring(a);
	}
}
