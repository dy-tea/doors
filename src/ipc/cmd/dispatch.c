#include "diagnostics.h"
#include "ipc/args.h"
#include "ipc/ipc.h"
#include "ipc/registry.h"
#include "parse.h"
#include "str.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wlr/util/log.h>

#define CMD(name, summary, usage, run) {name, summary, usage, run, NULL, NULL}
#define CMD_SUB(name, summary, usage, run, subs) {name, summary, usage, run, subs, NULL}
#define CMD_SUB_EXTRA(name, summary, usage, run, subs, extra) \
	{name, summary, usage, run, subs, extra}

static void ipc_cmd_commands(ipc_args_t *a);

// commands with a parse-without-mutate path
static const char *const dry_run_commands[] = {
	"bezier",
	"config",
	"env",
	"hotkey",
	"input",
	"layer_rule",
	"layout",
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
	CMD("bezier", "register a bezier easing curve", "bezier <name> <p1x> <p1y> <p2x> <p2y>",
		ipc_cmd_actions),
	CMD("commands", "list every command and its subcommands", "commands", ipc_cmd_commands),
	CMD_SUB("config", "get, set or reset a setting", NULL, ipc_cmd_config, config_subs),
	CMD_SUB("desktop", "focus, create, rename or reorder desktops", NULL, ipc_cmd_desktop,
		desktop_subs),
	CMD_SUB("env", "set or unset environment variables for launched clients", NULL, ipc_cmd_env,
		env_subs),
	CMD("focus", "move focus in a direction", "focus west|east|north|south", ipc_cmd_actions),
	CMD("globalshortcuts", "list bound global shortcuts", "globalshortcuts", ipc_cmd_globalshortcuts),
	CMD_SUB("hotkey", "list or add keybinds", NULL, ipc_cmd_hotkey, hotkey_subs),
	CMD("input", "set an input device property", "input [<device>] [type:<type>] <property> [<value>]",
		ipc_cmd_input),
	CMD_SUB("layer_rule", "manage layer-shell surface rules", NULL, ipc_cmd_layer_rule,
		layer_rule_subs),
	CMD_SUB("layout", "list layouts, or get, set and run a layout's own options and commands", NULL,
		ipc_cmd_layout, NULL),
	CMD_SUB("node", "act on the focused window", NULL, ipc_cmd_node, node_subs),
	CMD_SUB_EXTRA("output", "configure outputs and desktops", NULL, ipc_cmd_output, output_subs,
		output_global_subs),
	CMD_SUB("permission", "manage capture, output and input permissions",
		"permission <subject> <capability>=<mode>...", ipc_cmd_permission, permission_subs),
	CMD_SUB("plugin", "load or list plugins", NULL, ipc_cmd_plugin, plugin_subs),
	CMD_SUB("query", "query monitors, desktops, layers and nodes", NULL, ipc_cmd_query, query_subs),
	CMD("quit", "shut the compositor down", "quit", ipc_cmd_quit),
	CMD("resize", "resize in a direction", "resize left|right|up|down", ipc_cmd_actions),
	CMD_SUB("rule", "manage window rules", NULL, ipc_cmd_rule, rule_subs),
	CMD_SUB("scratchpad", "show, hide and list scratchpad entries", NULL, ipc_cmd_scratchpad,
		scratchpad_subs),
	CMD_SUB("seat", "list seats", NULL, ipc_cmd_seat, seat_subs),
	CMD("send", "send the focused window to another desktop", "send next|prev|previous",
		ipc_cmd_actions),
	CMD("sessions", "list application sessions", "sessions", ipc_cmd_sessions),
	CMD("spring", "register a spring animation curve",
		"spring <name> <stiffness> <damping> [<mass>] [<value_eps>] [<velocity_eps>]", ipc_cmd_actions),
	CMD("subscribe", "subscribe to events",
		"subscribe [report|monitor|desktop|node|config|permission|all] [-c|--count <n>] "
		"[-f|--fifo]", NULL),
	CMD("swap", "swap with the neighbour in a direction", "swap west|east|north|south",
		ipc_cmd_actions),
	CMD("toggle", "toggle a window state",
		"toggle floating|fullscreen|maximize|minimize|restore_minimized|pseudo_tiled|monocle|"
		"floating_layout|block_out_from_screenshare", ipc_cmd_actions),
	CMD_SUB("tree", "drive the tiled (BSP) tree layout", NULL, ipc_cmd_tree, tree_subs),
	CMD_SUB("wm", "compositor level commands", NULL, ipc_cmd_wm, wm_subs),
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
		if (c->usage)
			fprintf(f, "\n  doorsctl %s", c->usage);
		for (const ipc_sub_t *s = c->subs; s && s->flag; s++) {
			if (s->usage)
				fprintf(f, "\n  doorsctl %s", s->usage);
		}
		for (const ipc_sub_t *s = c->extra_subs; s && s->flag; s++) {
			if (s->usage)
				fprintf(f, "\n  doorsctl %s", s->usage);
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

// dispatch command through the ipc socket
static bool ipc_dispatch(ipc_args_t *a, bool *owns_client_fd) {
	*owns_client_fd = false;

	if (streq("subscribe", a->cmd)) {
		*owns_client_fd = ipc_cmd_subscribe(a);
		return true;
	}

	for (size_t i = 0; i < IPC_ARRAY_LEN(commands); i++) {
		if (!streq(commands[i].name, a->cmd))
			continue;
		commands[i].run(a);
		return true;
	}

	ipc_fail(a, "Unknown command \"%s\"\n", a->cmd);
	return false;
}

// runs a command line without a client, used by hotkey binds. replies are discarded
bool ipc_run_line(const char *line) {
	char buf[DOORS_BUFSIZ];
	str_copy(buf, sizeof(buf), line);

	char *args[32];
	int argc = parse_split_quoted(buf, args, 32);

	int first = 0;
	if (argc > 0 && streq(args[0], "doorsctl"))
		first = 1;
	if (first >= argc)
		return false;

	// subscribing needs a socket to attach to
	if (streq(args[first], "subscribe"))
		return false;

	ipc_args_t a;
	ipc_args_init(&a, args + first + 1, argc - first - 1, -1, args[first]);

	diag_stage_enter(DIAG_STAGE_IPC);
	bool owns_client_fd = false;
	const bool found = ipc_dispatch(&a, &owns_client_fd);
	diag_stage_leave();

	return found;
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
	ipc_dispatch(&a, &owns_client_fd);
	diag_stage_leave();

	free(args);
	return owns_client_fd;
}
