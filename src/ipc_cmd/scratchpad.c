#include "scratchpad.h"

#include "ipc.h"
#include "ipc_cmd.h"
#include "ipc_helpers.h"
#include "transaction.h"

#include <stdio.h>
#include <string.h>

void ipc_cmd_scratchpad(char **args, int num, int client_fd) {
	if (num < 1) {
		send_failure(client_fd, "scratchpad: missing arguments\n");
		return;
	}

	if (streq("show", *args)) {
		if (num >= 2) {
			node_t *n = NULL;
			if (strncmp(args[1], "app_id:", 7) == 0)
				n = scratchpad_find_by_app_id(args[1] + 7);
			else if (strncmp(args[1], "title:", 6) == 0)
				n = scratchpad_find_by_title(args[1] + 6);

			if (n) {
				scratchpad_show(n);
				transaction_commit_dirty();
				send_success(client_fd, "scratchpad shown\n");
				return;
			}
			send_failure(client_fd, "scratchpad show: matching entry not found\n");
			return;
		}

		scratchpad_toggle_auto();
		transaction_commit_dirty();
		send_success(client_fd, "scratchpad toggled\n");
		return;
	} else {
		send_failure(client_fd, "scratchpad: unknown subcommand (use show)\n");
	}
}
