#include "bezier.h"
#include "config.h"
#include "ipc.h"
#include "ipc_cmd.h"
#include "ipc_helpers.h"
#include "keyboard.h"
#include "output.h"
#include "server.h"
#include "spring.h"
#include "transaction.h"
#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void ipc_cmd_bind_dir(char **args, int num, int client_fd, const char *verb,
		const char *success_msg, bool (*filter)(bind_action_t)) {
	if (num < 1) {
		char buf[128];
		snprintf(buf, sizeof(buf), "%s: missing direction\n", verb);
		send_failure(client_fd, buf);
		return;
	}

	bind_action_t action = bind_action_from_name(*args);
	if (action != BIND_NONE && (!filter || filter(action))) {
		execute_bind_action(action);
		send_success(client_fd, success_msg);
	} else {
		char buf[128];
		snprintf(buf, sizeof(buf), "%s: unknown direction\n", verb);
		send_failure(client_fd, buf);
	}
}

static bool is_rotate_action(bind_action_t a) {
	return a == BIND_ROTATE_CW || a == BIND_ROTATE_CCW;
}

static bool is_flip_action(bind_action_t a) {
	return a == BIND_FLIP_HORIZONTAL || a == BIND_FLIP_VERTICAL;
}

void ipc_cmd_focus(char **args, int num, int client_fd) {
	ipc_cmd_bind_dir(args, num, client_fd, "focus", "focused\n", NULL);
}

void ipc_cmd_swap(char **args, int num, int client_fd) {
	ipc_cmd_bind_dir(args, num, client_fd, "swap", "swapped\n", NULL);
}

void ipc_cmd_resize(char **args, int num, int client_fd) {
	ipc_cmd_bind_dir(args, num, client_fd, "resize", "resized\n", NULL);
}

void ipc_cmd_presel(char **args, int num, int client_fd) {
	if (num < 1) {
		send_failure(client_fd, "presel: missing direction\n");
		return;
	}

	bind_action_t action = bind_action_from_name(*args);
	if (action != BIND_NONE) {
		execute_bind_action(action);
		const char *name = *args;
		if (strcmp(name, "cancel") == 0) {
			send_success(client_fd, "presel cancelled\n");
		} else {
			send_success(client_fd, "presel set\n");
		}
	} else {
		send_failure(client_fd, "presel: unknown direction\n");
	}
}

void ipc_cmd_rotate(char **args, int num, int client_fd) {
	ipc_cmd_bind_dir(args, num, client_fd, "rotate", "rotated\n", is_rotate_action);
}

void ipc_cmd_flip(char **args, int num, int client_fd) {
	ipc_cmd_bind_dir(args, num, client_fd, "flip", "flipped\n", is_flip_action);
}

static void ipc_cmd_tree_op(char **args, int num, int client_fd, const char *verb,
		const char *success_msg, void (*fn)(node_t * )) {
	(void)args;
	(void)num;

	output_t *m = server.focused_output;
	if (!m || !m->desk) {
		char buf[128];
		snprintf(buf, sizeof(buf), "%s: no focused desktop\n", verb);
		send_failure(client_fd, buf);
		return;
	}

	if (!m->desk->root) {
		char buf[128];
		snprintf(buf, sizeof(buf), "%s: no tree\n", verb);
		send_failure(client_fd, buf);
		return;
	}

	fn(m->desk->root);
	transaction_commit_dirty();
	send_success(client_fd, success_msg);
}

void ipc_cmd_equalize(char **args, int num, int client_fd) {
	ipc_cmd_tree_op(args, num, client_fd, "equalize", "equalized\n", equalize_tree);
}

void ipc_cmd_balance(char **args, int num, int client_fd) {
	ipc_cmd_tree_op(args, num, client_fd, "balance", "balanced\n", balance_tree);
}

static const struct {
	const char *name;
	void (*toggle_fn)(void);
} toggle_props[] = {
	{"floating", toggle_floating},
	{"fullscreen", toggle_fullscreen},
	{"maximize", toggle_maximize},
	{"minimize", toggle_minimize},
	{"pseudo_tiled", toggle_pseudo_tiled},
	{"monocle", toggle_monocle},
	{"floating_layout", toggle_floating_layout},
	{"block_out_from_screenshare", toggle_block_out_from_screenshare},
};

void ipc_cmd_toggle(char **args, int num, int client_fd) {
	if (num < 1) {
		send_failure(client_fd, "toggle: missing property\n");
		return;
	}

	for (size_t i = 0; i < sizeof(toggle_props) / sizeof(toggle_props[0]); i++) {
		if (streq(toggle_props[i].name, *args)) {
			toggle_props[i].toggle_fn();
			send_success(client_fd, "toggled\n");
			return;
		}
	}
	send_failure(client_fd, "toggle: unknown property\n");
}

void ipc_cmd_send(char **args, int num, int client_fd) {
	if (num < 1) {
		send_failure(client_fd, "send: missing direction\n");
		return;
	}

	const char *arg = *args;
	if (strcmp(arg, "next") == 0) {
		send_to_next_desktop();
		send_success(client_fd, "sent\n");
	} else if (strcmp(arg, "prev") == 0 || strcmp(arg, "previous") == 0) {
		send_to_prev_desktop();
		send_success(client_fd, "sent\n");
	} else {
		send_failure(client_fd, "send: unknown direction\n");
	}
}

void ipc_cmd_bezier(char **args, int num, int client_fd) {
	if (num < 5) {
		send_failure(client_fd, "usage: bezier <name> <p1x> <p1y> <p2x> <p2y>\n");
		return;
	}
	double p1x = atof(args[1]), p1y = atof(args[2]);
	double p2x = atof(args[3]), p2y = atof(args[4]);
	if (bezier_add(args[0], p1x, p1y, p2x, p2y))
		send_success(client_fd, "bezier curve added\n");
	else
		send_failure(client_fd, "failed to add bezier curve\n");
}

void ipc_cmd_spring(char **args, int num, int client_fd) {
	if (num < 3) {
		send_failure(client_fd,
			"usage: spring <name> <stiffness> <damping> [mass] [value_eps] [velocity_eps]\n");
		return;
	}
	double stiffness = atof(args[1]);
	double damping = atof(args[2]);
	double mass = num >= 4 ? atof(args[3]) : 1.0;
	double value_eps = num >= 5 ? atof(args[4]) : SPRING_EPSILON_DEFAULT;
	double velocity_eps = num >= 6 ? atof(args[5]) : SPRING_EPSILON_DEFAULT;
	if (spring_add(args[0], stiffness, damping, mass, value_eps, velocity_eps))
		send_success(client_fd, "spring curve added\n");
	else
		send_failure(client_fd, "failed to add spring curve\n");
}
