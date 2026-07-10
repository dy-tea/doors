#include "ipc_cmd.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

void send_success(int client_fd, const char *msg);
void send_failure(int client_fd, const char *msg);

static bool valid_env_name(const char *name) { return name[0] != '\0' && strchr(name, '=') == NULL; }

void ipc_cmd_env(char **args, int num, int client_fd) {
	if (num < 1) {
		send_failure(client_fd, "env: usage: set <name> <value> | unset <name>\n");
		return;
	}

	if (strcmp(args[0], "set") == 0) {
		if (num != 3) {
			send_failure(client_fd, "env set: usage: set <name> <value>\n");
			return;
		}

		if (!valid_env_name(args[1])) {
			send_failure(client_fd, "env set: invalid variable name\n");
			return;
		}

		if (setenv(args[1], args[2], 1) != 0) {
			send_failure(client_fd, "env set: failed to set variable\n");
			return;
		}

		send_success(client_fd, "environment variable set\n");
		return;
	}

	if (strcmp(args[0], "unset") == 0) {
		if (num != 2) {
			send_failure(client_fd, "env unset: usage: unset <name>\n");
			return;
		}

		if (!valid_env_name(args[1])) {
			send_failure(client_fd, "env unset: invalid variable name\n");
			return;
		}

		if (unsetenv(args[1]) != 0) {
			send_failure(client_fd, "env unset: failed to unset variable\n");
			return;
		}

		send_success(client_fd, "environment variable unset\n");
		return;
	}

	send_failure(client_fd, "env: unknown subcommand (use set or unset)\n");
}
