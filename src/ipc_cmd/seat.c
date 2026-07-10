#include "seat.h"

#include "config.h"
#include "ipc.h"
#include "ipc_cmd.h"
#include "ipc_helpers.h"
#include "keyboard.h"
#include "server.h"

#include <stdio.h>

void ipc_cmd_seat(char **args, int num, int client_fd) {
	if (num < 1) {
		send_failure(client_fd, "seat: missing argument (use list)\n");
		return;
	}

	if (streq("list", *args)) {
		char buf[4096];
		int offset = 0;
		seat_t *s;
		wl_list_for_each(s, &server.seats, link) {
			offset += snprintf(buf + offset, sizeof(buf) - offset, "%s", s->name);
			if (s == seat_default())
				offset += snprintf(buf + offset, sizeof(buf) - offset, " (default)");
			offset += snprintf(buf + offset, sizeof(buf) - offset, "\n");
		}
		if (offset == 0)
			offset = snprintf(buf, sizeof(buf), "No seats\n");
		send_success(client_fd, buf);
	} else {
		send_failure(client_fd, "seat: unknown subcommand (use list)\n");
	}
}

void ipc_cmd_keyboard_grouping(char **args, int num, int client_fd) {
	if (num < 1) {
		send_failure(client_fd, "keyboard_grouping: missing argument\n");
		return;
	}

	char *mode = *args;
	keyboard_grouping_t grouping;
	if (streq("none", mode)) {
		grouping = KEYBOARD_GROUP_NONE;
	} else if (streq("smart", mode)) {
		grouping = KEYBOARD_GROUP_SMART;
	} else if (streq("default", mode)) {
		grouping = KEYBOARD_GROUP_DEFAULT;
	} else {
		send_failure(client_fd, "keyboard_grouping: invalid mode (use none, smart, default)\n");
		return;
	}

	set_keyboard_grouping(grouping);
	keyboard_reapply_grouping();
	send_success(client_fd, "keyboard_grouping set\n");
}
