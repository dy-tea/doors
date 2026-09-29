#include "bezier.h"
#include "input/keyboard.h"
#include "ipc/args.h"
#include "ipc/helpers.h"
#include "ipc/registry.h"
#include "output/output.h"
#include "server.h"
#include "spring.h"
#include "transaction.h"
#include "tree.h"
#include <stdlib.h>
#include <string.h>

static void action_direction(ipc_args_t *a, const char *success, bool (*filter)(bind_action_t)) {
	const char *dir;
	if (!ipc_need(a, "direction", &dir))
		return;

	bind_action_t action = bind_action_from_name(dir);
	if (action == BIND_NONE || (filter && !filter(action))) {
		ipc_fail(a, "Unknown direction \"%s\"\n", dir);
		return;
	}

	execute_bind_action(action);
	ipc_okf(a, "%s\n", success);
}

static bool is_rotate_action(bind_action_t a) {
	return a == BIND_ROTATE_CW || a == BIND_ROTATE_CCW;
}

static bool is_flip_action(bind_action_t a) {
	return a == BIND_FLIP_HORIZONTAL || a == BIND_FLIP_VERTICAL;
}

static bool is_presel_action(bind_action_t a) {
	return a >= BIND_PRESEL_NORTH && a <= BIND_PRESEL_CANCEL;
}

static void act_presel(ipc_args_t *a) {
	const char *dir;
	if (!ipc_need(a, "direction", &dir))
		return;

	bind_action_t action = bind_action_from_name(dir);
	if (action == BIND_NONE || !is_presel_action(action)) {
		ipc_fail(a, "Unknown direction \"%s\"\n", dir);
		return;
	}

	execute_bind_action(action);
	ipc_okf(a, "Presel %s\n", action == BIND_PRESEL_CANCEL ? "cancelled" : "set");
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
	for (size_t i = 0; i < IPC_ARRAY_LEN(props); i++)
		ipc_buff(&b, "%s\"%s\"", i > 0 ? ", " : "", props[i].name);
	ipc_fail(a, "Unknown property \"%s\", expected one of: %s\n", name, list);
}

static void act_send(ipc_args_t *a) {
	static const cfg_enum_value_t targets[] = {
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

static void act_tree(ipc_args_t *a, void (*fn)(node_t * )) {
	output_t *m = server.focused_output;
	if (!m || !m->desk) {
		ipc_fail(a, "No focused desktop\n");
		return;
	}
	if (!m->desk->root) {
		ipc_fail(a, "No tree\n");
		return;
	}

	fn(m->desk->root);
	transaction_commit_dirty();
	ipc_okf(a, "%sed\n", a->cmd);
}

static void act_bezier(ipc_args_t *a) {
	const char *name;
	double p1x, p1y, p2x, p2y;
	if (!ipc_need(a, "name", &name) || !ipc_double(a, "p1x", -1e6, 1e6, &p1x) || !ipc_double(a, "p1y",
		-1e6, 1e6, &p1y) || !ipc_double(a, "p2x", -1e6, 1e6, &p2x) || !ipc_double(a, "p2y", -1e6, 1e6,
		&p2y))
		return;

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

	if (spring_add(name, stiffness, damping, mass, value_eps, velocity_eps))
		ipc_ok(a, "Spring curve added\n");
	else
		ipc_fail(a, "Failed to add spring curve\n");
}

void ipc_cmd_actions(ipc_args_t *a) {
	const char *cmd = a->cmd;

	if (streq(cmd, "focus")) {
		action_direction(a, "focused", NULL);
	} else if (streq(cmd, "swap")) {
		action_direction(a, "swapped", NULL);
	} else if (streq(cmd, "resize")) {
		action_direction(a, "resized", NULL);
	} else if (streq(cmd, "rotate")) {
		action_direction(a, "rotated", is_rotate_action);
	} else if (streq(cmd, "flip")) {
		action_direction(a, "flipped", is_flip_action);
	} else if (streq(cmd, "presel")) {
		act_presel(a);
	} else if (streq(cmd, "toggle")) {
		act_toggle(a);
	} else if (streq(cmd, "send")) {
		act_send(a);
	} else if (streq(cmd, "balance")) {
		act_tree(a, balance_tree);
	} else if (streq(cmd, "equalize")) {
		act_tree(a, equalize_tree);
	} else if (streq(cmd, "bezier")) {
		act_bezier(a);
	} else {
		act_spring(a);
	}
}
