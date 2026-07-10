#include "scroller.h"

#include "ipc.h"
#include "ipc_cmd.h"
#include "ipc_helpers.h"
#include "master_stack.h"
#include "output.h"
#include "server.h"
#include "tree.h"

#include <limits.h>
#include <stdlib.h>

void ipc_cmd_scroller(char **args, int num, int client_fd) {
	if (num < 1) {
		send_failure(client_fd, "scroller: missing arguments\n");
		return;
	}

	output_t *mon = server.focused_output;
	if (!mon || !mon->desk) {
		send_failure(client_fd, "no desktop\n");
		return;
	}

	desktop_t *desk = mon->desk;

	if (streq("proportion", *args)) {
		if (num < 2) {
			send_failure(client_fd, "scroller proportion: missing value\n");
			return;
		}

		if (!desk->focus || !desk->focus->client) {
			send_failure(client_fd, "scroller proportion: no focused client\n");
			return;
		}

		float value = atof(args[1]);
		if (value < 0.1f || value > 1.0f) {
			send_failure(client_fd, "scroller proportion: value must be between 0.1 and 1.0\n");
			return;
		}

		client_t *head = scroller_get_stack_head(desk->focus->client);
		head->scroller_proportion = value;
		arrange(mon, desk, true);
		send_success(client_fd, "proportion set\n");

	} else if (streq("stack", *args)) {
		if (!desk->focus || !desk->focus->client) {
			send_failure(client_fd, "scroller stack: no focused client\n");
			return;
		}

		node_t *target = prev_leaf(desk->focus, desk->root);
		if (!target || !target->client) {
			send_failure(client_fd, "scroller stack: no target to stack with\n");
			return;
		}

		client_t *head = scroller_get_stack_head(target->client);
		scroller_stack_push(head, desk->focus->client);
		arrange(mon, desk, true);
		send_success(client_fd, "stacked\n");

	} else if (streq("unstack", *args)) {
		if (!desk->focus || !desk->focus->client) {
			send_failure(client_fd, "scroller unstack: no focused client\n");
			return;
		}

		scroller_stack_remove(desk->focus->client);
		arrange(mon, desk, true);
		send_success(client_fd, "unstacked\n");

	} else if (streq("resize", *args)) {
		if (num < 2) {
			send_failure(client_fd, "scroller resize: missing delta\n");
			return;
		}

		if (!desk->focus || !desk->focus->client) {
			send_failure(client_fd, "scroller resize: no focused client\n");
			return;
		}

		float delta = atof(args[1]);
		scroller_resize_width(desk->focus->client, delta);
		arrange(mon, desk, true);
		send_success(client_fd, "resized\n");

	} else if (streq("set_proportion", *args)) {
		if (num < 2) {
			send_failure(client_fd, "scroller set_proportion: missing value\n");
			return;
		}

		if (!desk->focus || !desk->focus->client) {
			send_failure(client_fd, "scroller set_proportion: no focused client\n");
			return;
		}

		float value = atof(args[1]);
		scroller_set_proportion(desk->focus->client, value);
		arrange(mon, desk, true);
		send_success(client_fd, "proportion set\n");

	} else if (streq("cycle_preset", *args)) {
		if (!desk->focus || !desk->focus->client) {
			send_failure(client_fd, "scroller cycle_preset: no focused client\n");
			return;
		}

		if (scroller_proportion_preset_count == 0) {
			send_failure(client_fd, "scroller cycle_preset: no presets configured\n");
			return;
		}

		scroller_cycle_proportion_preset(desk->focus->client);
		arrange(mon, desk, true);
		send_success(client_fd, "cycled to next preset\n");

	} else if (streq("center", *args)) {
		if (!desk->focus || !desk->focus->client) {
			send_failure(client_fd, "scroller center: no focused client\n");
			return;
		}

		scroller_center_window(desk, desk->focus->client);
		send_success(client_fd, "window centered\n");

	} else {
		send_failure(client_fd, "scroller: unknown subcommand\n");
	}
}

void ipc_cmd_master_stack(char **args, int num, int client_fd) {
	if (num < 1) {
		send_failure(client_fd, "master_stack: missing arguments\n");
		return;
	}

	output_t *mon = server.focused_output;
	if (!mon || !mon->desk) {
		send_failure(client_fd, "no desktop\n");
		return;
	}

	desktop_t *desk = mon->desk;

	if (streq("cycle_orientation", *args)) {
		master_stack_cycle_orientation();
		arrange(mon, desk, true);
		send_success(client_fd, "orientation cycled\n");
	} else if (streq("orientation", *args)) {
		if (num < 2) {
			send_failure(client_fd, "master_stack orientation: missing orientation\n");
			return;
		}

		master_area_orientation_t orientation;
		if (streq("left", args[1]))
			orientation = MASTER_LEFT;
		else if (streq("right", args[1]))
			orientation = MASTER_RIGHT;
		else if (streq("top", args[1]))
			orientation = MASTER_TOP;
		else if (streq("bottom", args[1]) || streq("down", args[1]))
			orientation = MASTER_BOTTOM;
		else if (streq("center", args[1]))
			orientation = MASTER_CENTER;
		else {
			send_failure(client_fd, "master_stack orientation: expected left, right, center, top, or bottom\n");
			return;
		}

		master_stack_set_orientation(orientation);
		arrange(mon, desk, true);
		send_success(client_fd, "orientation set\n");
	} else if (streq("cycle_stack_layout", *args)) {
		master_stack_cycle_stack_layout();
		arrange(mon, desk, true);
		send_success(client_fd, "stack layout cycled\n");
	} else if (streq("inc", *args)) {
		master_stack_increment(desk);
		arrange(mon, desk, true);
		send_success(client_fd, "master added\n");
	} else if (streq("dec", *args)) {
		master_stack_decrement(desk);
		arrange(mon, desk, true);
		send_success(client_fd, "master removed\n");
	} else if (streq("promote", *args)) {
		if (!master_stack_promote(desk)) {
			send_failure(client_fd, "master_stack promote: focus a secondary tiled window\n");
			return;
		}
		arrange(mon, desk, true);
		send_success(client_fd, "focused window promoted\n");
	} else if (streq("demote", *args)) {
		if (!master_stack_demote(desk)) {
			send_failure(client_fd, "master_stack demote: focus a master tiled window\n");
			return;
		}
		arrange(mon, desk, true);
		send_success(client_fd, "focused window demoted\n");
	} else if (streq("flip", *args)) {
		master_stack_flip_orientation();
		arrange(mon, desk, true);
		send_success(client_fd, "orientation flipped\n");
	} else if (streq("set_count", *args)) {
		if (num < 2) {
			send_failure(client_fd, "master_stack set_count: missing count\n");
			return;
		}
		char *end = NULL;
		long count = strtol(args[1], &end, 10);
		if (!end || *end != '\0' || count < 0 || count > INT_MAX) {
			send_failure(client_fd, "master_stack set_count: count must be a non-negative integer\n");
			return;
		}
		master_stack_set_count(desk, (int)count);
		arrange(mon, desk, true);
		send_success(client_fd, "master count set\n");
	} else if (streq("set_ratio", *args)) {
		if (num < 2) {
			send_failure(client_fd, "master_stack set_ratio: missing ratio\n");
			return;
		}
		float val = atof(args[1]);
		if (val < 0.1f || val > 0.9f) {
			send_failure(client_fd, "master_stack set_ratio: ratio must be between 0.1 and 0.9\n");
			return;
		}
		master_stack_ratio = val;
		arrange(mon, desk, true);
		send_success(client_fd, "ratio set\n");
	} else {
		send_failure(client_fd, "master_stack: unknown subcommand\n");
	}
}
