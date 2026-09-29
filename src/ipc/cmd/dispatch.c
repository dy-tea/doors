#include "ipc/args.h"
#include "ipc/helpers.h"
#include "ipc/ipc.h"
#include "ipc/registry.h"
#include <stdlib.h>
#include <wlr/util/log.h>

#define CMD(name, summary, run) {name, summary, run, NULL}

static void ipc_cmd_commands(ipc_args_t *a);

static const ipc_cmd_t commands[] = {
	CMD("balance", "even out the focused desktop", ipc_cmd_actions),
	CMD("bezier", "register a bezier easing curve", ipc_cmd_actions),
	CMD("commands", "list every command and its subcommands", ipc_cmd_commands),
	CMD("config", "get or set a setting", ipc_cmd_config),
	{"desktop", "focus, create, rename or reorder desktops", ipc_cmd_desktop, desktop_subs},
	{"env", "set or unset environment variables for launched clients", ipc_cmd_env, env_subs},
	CMD("equalize", "split the tree evenly", ipc_cmd_actions),
	CMD("flip", "flip the layout axis", ipc_cmd_actions),
	CMD("focus", "move focus in a direction", ipc_cmd_actions),
	CMD("globalshortcuts", "list bound global shortcuts", ipc_cmd_globalshortcuts),
	CMD("hotkey", "list or add keybinds", ipc_cmd_hotkey),
	CMD("input", "set an input device property", ipc_cmd_input),
	CMD("keyboard_grouping", "set the keyboard grouping mode", ipc_cmd_keyboard_grouping),
	{"master_stack", "drive the master_stack layout", ipc_cmd_master_stack, master_stack_subs},
	{"node", "act on the focused window", ipc_cmd_node, node_subs},
	{"output", "configure outputs and desktops", ipc_cmd_output, output_subs},
	CMD("presel", "set the preselection direction", ipc_cmd_actions),
	{"query", "query monitors, desktops and nodes", ipc_cmd_query, query_subs},
	CMD("quit", "shut the compositor down", ipc_cmd_quit),
	CMD("resize", "resize in a direction", ipc_cmd_actions),
	CMD("rotate", "rotate the layout", ipc_cmd_actions),
	{"rule", "manage window rules", ipc_cmd_rule, rule_subs},
	CMD("scratchpad", "show, hide and list scratchpad entries", ipc_cmd_scratchpad),
	{"scroller", "drive the scroller layout", ipc_cmd_scroller, scroller_subs},
	{"seat", "list seats", ipc_cmd_seat, seat_subs},
	CMD("send", "send the focused window to another desktop", ipc_cmd_actions),
	CMD("spring", "register a spring animation curve", ipc_cmd_actions),
	CMD("swap", "swap with the neighbour in a direction", ipc_cmd_actions),
	CMD("toggle", "toggle a window state", ipc_cmd_actions),
	{"wm", "compositor level commands", ipc_cmd_wm, wm_subs},
};

static void ipc_cmd_commands(ipc_args_t *a) {
	char buf[DOORS_BUFSIZ];
	ipc_buf_t b;
	ipc_buf_init(&b, buf, sizeof(buf));

	for (size_t i = 0; i < IPC_ARRAY_LEN(commands); i++) {
		const ipc_cmd_t *c = &commands[i];
		ipc_buff(&b, "%-18s %s", c->name, c->summary);
		if (c->subs)
			ipc_sub_usage(&b, c->subs);
		ipc_buff(&b, "\n");
	}

	ipc_buf_send(a, &b);
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

	const char *name = args[0];
	ipc_args_t a;
	ipc_args_init(&a, args + 1, num - 1, client_fd, name);

	bool owns_client_fd = false;

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

	free(args);
	return owns_client_fd;
}
