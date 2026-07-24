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
#include <stdlib.h>
#include <string.h>

void ipc_cmd_focus(char **args, int num, int client_fd) {
	if (num < 1) {
		send_failure(client_fd, "focus: missing direction\n");
		return;
	}

	bind_action_t action = bind_action_from_name(*args);
	if (action != BIND_NONE) {
		execute_bind_action(action);
		send_success(client_fd, "focused\n");
	} else {
		send_failure(client_fd, "focus: unknown direction\n");
	}
}

void ipc_cmd_swap(char **args, int num, int client_fd) {
	if (num < 1) {
		send_failure(client_fd, "swap: missing direction\n");
		return;
	}

	bind_action_t action = bind_action_from_name(*args);
	if (action != BIND_NONE) {
		execute_bind_action(action);
		send_success(client_fd, "swapped\n");
	} else {
		send_failure(client_fd, "swap: unknown direction\n");
	}
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

void ipc_cmd_resize(char **args, int num, int client_fd) {
	if (num < 1) {
		send_failure(client_fd, "resize: missing direction\n");
		return;
	}

	bind_action_t action = bind_action_from_name(*args);
	if (action != BIND_NONE) {
		execute_bind_action(action);
		send_success(client_fd, "resized\n");
	} else {
		send_failure(client_fd, "resize: unknown direction\n");
	}
}

void ipc_cmd_toggle(char **args, int num, int client_fd) {
	if (num < 1) {
		send_failure(client_fd, "toggle: missing property\n");
		return;
	}

	if (streq("floating", *args)) {
		toggle_floating();
		send_success(client_fd, "toggled\n");
	} else if (streq("fullscreen", *args)) {
		toggle_fullscreen();
		send_success(client_fd, "toggled\n");
	} else if (streq("pseudo_tiled", *args)) {
		toggle_pseudo_tiled();
		send_success(client_fd, "toggled\n");
	} else if (streq("monocle", *args)) {
		toggle_monocle();
		send_success(client_fd, "toggled\n");
	} else if (streq("block_out_from_screenshare", *args)) {
		toggle_block_out_from_screenshare();
		send_success(client_fd, "toggled\n");
	} else {
		send_failure(client_fd, "toggle: unknown property\n");
	}
}

void ipc_cmd_rotate(char **args, int num, int client_fd) {
	if (num < 1) {
		send_failure(client_fd, "rotate: missing direction\n");
		return;
	}

	bind_action_t action = bind_action_from_name(*args);
	if (action == BIND_ROTATE_CW || action == BIND_ROTATE_CCW) {
		execute_bind_action(action);
		send_success(client_fd, "rotated\n");
	} else {
		send_failure(client_fd, "rotate: unknown direction\n");
	}
}

void ipc_cmd_flip(char **args, int num, int client_fd) {
	if (num < 1) {
		send_failure(client_fd, "flip: missing direction\n");
		return;
	}

	bind_action_t action = bind_action_from_name(*args);
	if (action == BIND_FLIP_HORIZONTAL || action == BIND_FLIP_VERTICAL) {
		execute_bind_action(action);
		send_success(client_fd, "flipped\n");
	} else {
		send_failure(client_fd, "flip: unknown direction\n");
	}
}

void ipc_cmd_equalize(char **args, int num, int client_fd) {
	(void)args;
	(void)num;

	output_t *m = server.focused_output;
	if (!m || !m->desk) {
		send_failure(client_fd, "equalize: no focused desktop\n");
		return;
	}

	if (!m->desk->root) {
		send_failure(client_fd, "equalize: no tree\n");
		return;
	}

	equalize_tree(m->desk->root);
	transaction_commit_dirty();
	send_success(client_fd, "equalized\n");
}

void ipc_cmd_balance(char **args, int num, int client_fd) {
	(void)args;
	(void)num;

	output_t *m = server.focused_output;
	if (!m || !m->desk) {
		send_failure(client_fd, "balance: no focused desktop\n");
		return;
	}

	if (!m->desk->root) {
		send_failure(client_fd, "balance: no tree\n");
		return;
	}

	balance_tree(m->desk->root);
	transaction_commit_dirty();
	send_success(client_fd, "balanced\n");
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
