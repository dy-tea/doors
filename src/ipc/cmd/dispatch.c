#include "diagnostics.h"
#include "ipc/args.h"
#include "ipc/ipc.h"
#include "ipc/registry.h"
#include "str.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wlr/util/log.h>

#define CMD(name, summary, run) {name, summary, run, NULL}

static void ipc_cmd_commands(ipc_args_t *a);

// commands with a parse-without-mutate path
static const char *const dry_run_commands[] = {
	"bezier",
	"config",
	"env",
	"hotkey",
	"input",
	"permission",
	"rule",
	"spring",
};

static bool dry_run_supported(const char *name) {
	for (size_t i = 0; i < IPC_ARRAY_LEN(dry_run_commands); i++) {
		if (streq(name, dry_run_commands[i]))
			return true;
	}
	return false;
}

static const ipc_cmd_t commands[] = {
	CMD("bezier", "register a bezier easing curve", ipc_cmd_actions),
	CMD("commands", "list every command and its subcommands", ipc_cmd_commands),
	{"config", "get, set or reset a setting", ipc_cmd_config, config_subs},
	{"desktop", "focus, create, rename or reorder desktops", ipc_cmd_desktop, desktop_subs},
	{"env", "set or unset environment variables for launched clients", ipc_cmd_env, env_subs},
	CMD("focus", "move focus in a direction", ipc_cmd_actions),
	CMD("globalshortcuts", "list bound global shortcuts", ipc_cmd_globalshortcuts),
	CMD("hotkey", "list or add keybinds", ipc_cmd_hotkey),
	CMD("input", "set an input device property", ipc_cmd_input),
	CMD("keyboard_grouping", "set the keyboard grouping mode", ipc_cmd_keyboard_grouping),
	{"master_stack", "drive the master_stack layout", ipc_cmd_master_stack, master_stack_subs},
	{"node", "act on the focused window", ipc_cmd_node, node_subs},
	{"output", "configure outputs and desktops", ipc_cmd_output, output_subs},
	{"permission", "manage capture, output and input permissions", ipc_cmd_permission,
		permission_subs},
	{"query", "query monitors, desktops and nodes", ipc_cmd_query, query_subs},
	CMD("quit", "shut the compositor down", ipc_cmd_quit),
	CMD("resize", "resize in a direction", ipc_cmd_actions),
	{"rule", "manage window rules", ipc_cmd_rule, rule_subs},
	CMD("scratchpad", "show, hide and list scratchpad entries", ipc_cmd_scratchpad),
	{"scroller", "drive the scroller layout", ipc_cmd_scroller, scroller_subs},
	{"seat", "list seats", ipc_cmd_seat, seat_subs},
	CMD("send", "send the focused window to another desktop", ipc_cmd_actions),
	CMD("sessions", "list application sessions and what they remember", ipc_cmd_sessions),
	CMD("spring", "register a spring animation curve", ipc_cmd_actions),
	CMD("swap", "swap with the neighbour in a direction", ipc_cmd_actions),
	CMD("toggle", "toggle a window state", ipc_cmd_actions),
	{"tree", "drive the tiled (BSP) tree layout", ipc_cmd_tree, tree_subs},
	{"wm", "compositor level commands", ipc_cmd_wm, wm_subs},
};

static void ipc_cmd_commands(ipc_args_t *a) {
	char *msg = NULL;
	size_t msg_sz = 0;
	FILE *f = open_memstream(&msg, &msg_sz);
	if (!f) {
		ipc_fail(a, "Failed to allocate reply buffer\n");
		return;
	}

	for (size_t i = 0; i < IPC_ARRAY_LEN(commands); i++) {
		const ipc_cmd_t *c = &commands[i];
		fprintf(f, "%-18s %s", c->name, c->summary);
		if (c->subs) {
			for (const ipc_sub_t *s = c->subs; s->flag; s++) {
				if (s->usage)
					fprintf(f, "\n  %s", s->usage);
			}
		}
		fputc('\n', f);
	}

	if (fclose(f) != 0) {
		free(msg);
		ipc_fail(a, "Failed to build reply\n");
		return;
	}

	send_success(a->fd, msg ? msg : "");
	free(msg);
}

bool process_ipc_message(char *msg, int msg_len, int client_fd) {
	wlr_log(WLR_DEBUG, "Processing message: %.*s", msg_len, msg);

	int cap = 16;
	int num = 0;
	char **args = calloc(cap, sizeof(char *));

	if (!args) {
		send_failure(client_fd, "Memory error\n");
		return false;
	}

	for (int i = 0, j = 0; i < msg_len; i++) {
		if (num >= cap) {
			cap *= 2;
			char **new = realloc(args, cap * sizeof(char *));
			if (!new) {
				free(args);
				send_failure(client_fd, "Memory error\n");
				return false;
			}
			args = new;
		}
		if (msg[i] == '\0') {
			args[num++] = msg + j;
			j = i + 1;
		}
	}

	if (num < 1) {
		free(args);
		send_failure(client_fd, "No arguments\n");
		return false;
	}

	bool dry_run = false;
	int first = 0;
	for (; first < num; first++) {
		const char *arg = args[first];

		if (streq(arg, "--dry-run")) {
			dry_run = true;
			continue;
		}

		break;
	}

	if (first >= num) {
		free(args);
		send_failure(client_fd, "No command after flags\n");
		return false;
	}

	const char *name = args[first];

	if (dry_run && !dry_run_supported(name)) {
		free(args);
		char msg[128];
		snprintf(msg, sizeof(msg), "--dry-run is not supported by \"%s\"\n", name);
		send_failure(client_fd, msg);
		return false;
	}

	ipc_args_t a;
	ipc_args_init(&a, args + first + 1, num - first - 1, client_fd, name);
	a.dry_run = dry_run;

	bool owns_client_fd = false;
	diag_stage_enter(DIAG_STAGE_IPC);

	if (streq("subscribe", name)) {
		owns_client_fd = ipc_cmd_subscribe(&a);
	} else {
		const ipc_cmd_t *cmd = NULL;
		for (size_t i = 0; i < IPC_ARRAY_LEN(commands); i++) {
			if (streq(commands[i].name, name)) {
				cmd = &commands[i];
				break;
			}
		}

		if (cmd)
			cmd->run(&a);
		else
			ipc_fail(&a, "Unknown command \"%s\"\n", name);
	}

	diag_stage_leave();
	free(args);
	return owns_client_fd;
}
