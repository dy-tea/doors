#include "ipc.h"
#include "ipc_cmd.h"
#include "ipc_helpers.h"
#include "scratchpad.h"
#include "transaction.h"
#include <stdio.h>
#include <string.h>

static void send_entry(node_t *n, char *buf, size_t buf_size, size_t *offset) {
	if (n == NULL || n->client == NULL)
		return;

	// where it was minimized from
	const char *desk = n->desktop != NULL ? n->desktop->name : "-";

	*offset += snprintf(buf + *offset, buf_size - *offset, "%s\n\t%s\n\t%s\n\t%s\n\t%u\n\t%s\n",
		n->client->app_id[0] ? n->client->app_id : "?", n->client->title[0] ? n->client->title : "?", desk,
		n->client->state == STATE_FULLSCREEN ? "fullscreen" : n->client->state == STATE_FLOATING ?
		"floating" : n->client->state == STATE_PSEUDO_TILED ? "pseudo_tiled" : "tiled", n->id,
		n->client->flags.maximized ? "maximized" : "-");
}

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
	} else if (streq("list", *args)) {
		int count = scratchpad_count();
		char buf[4096];
		size_t offset = 0;
		snprintf(buf, sizeof(buf), "%d\n", count);
		for (int i = 0; i < count; i++) {
			node_t *n = scratchpad_nth(i);
			if (n == NULL || n->client == NULL)
				continue;
			send_entry(n, buf, sizeof(buf), &offset);
		}
		send_success(client_fd, buf);
		return;
	} else if (streq("hide", *args)) {
		if (num >= 2) {
			node_t *n = NULL;
			if (strncmp(args[1], "app_id:", 7) == 0)
				n = scratchpad_find_by_app_id(args[1] + 7);
			else if (strncmp(args[1], "title:", 6) == 0)
				n = scratchpad_find_by_title(args[1] + 6);

			if (n) {
				scratchpad_add(n);
				transaction_commit_dirty();
				send_success(client_fd, "scratchpad hidden\n");
				return;
			}
			send_failure(client_fd, "scratchpad hide: matching entry not found\n");
			return;
		}
		send_failure(client_fd, "scratchpad hide: missing app_id:/title: argument\n");
		return;
	} else {
		send_failure(client_fd, "scratchpad: unknown subcommand (use show, hide, list)\n");
	}
}
