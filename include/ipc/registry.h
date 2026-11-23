#pragma once

#include "ipc/args.h"

typedef struct ipc_cmd_t {
	const char *name;
	const char *summary;
	const char *usage; // command-relative usage shown by `doorsctl commands`
	void (*run)(ipc_args_t *a);
	const ipc_sub_t *subs;
	const ipc_sub_t *extra_subs; // more usage entries, not used for dispatch
} ipc_cmd_t;

void ipc_cmd_actions(ipc_args_t *a);
void ipc_cmd_config(ipc_args_t *a);
void ipc_cmd_desktop(ipc_args_t *a);
void ipc_cmd_env(ipc_args_t *a);
void ipc_cmd_globalshortcuts(ipc_args_t *a);
void ipc_cmd_hotkey(ipc_args_t *a);
void ipc_cmd_input(ipc_args_t *a);
void ipc_cmd_layer_rule(ipc_args_t *a);
void ipc_cmd_layout(ipc_args_t *a);

// forwards a legacy name to its layout, true when it matched
bool ipc_cmd_layout_legacy(ipc_args_t *a, const char *name);
void ipc_cmd_node(ipc_args_t *a);
void ipc_cmd_output(ipc_args_t *a);
void ipc_cmd_permission(ipc_args_t *a);
void ipc_cmd_plugin(ipc_args_t *a);
void ipc_cmd_query(ipc_args_t *a);
void ipc_cmd_quit(ipc_args_t *a);
void ipc_cmd_rule(ipc_args_t *a);
void ipc_cmd_scratchpad(ipc_args_t *a);
void ipc_cmd_seat(ipc_args_t *a);
void ipc_cmd_tree(ipc_args_t *a);
void ipc_cmd_sessions(ipc_args_t *a);
bool ipc_cmd_subscribe(ipc_args_t *a); // true when the handler keeps the socket
void ipc_cmd_wm(ipc_args_t *a);

extern const ipc_sub_t config_subs[];
extern const ipc_sub_t desktop_subs[];
extern const ipc_sub_t env_subs[];
extern const ipc_sub_t hotkey_subs[];
extern const ipc_sub_t node_subs[];
extern const ipc_sub_t output_global_subs[];
extern const ipc_sub_t output_subs[];
extern const ipc_sub_t layer_rule_subs[];
extern const ipc_sub_t permission_subs[];
extern const ipc_sub_t plugin_subs[];
extern const ipc_sub_t query_subs[];
extern const ipc_sub_t rule_subs[];
extern const ipc_sub_t scratchpad_subs[];
extern const ipc_sub_t seat_subs[];
extern const ipc_sub_t tree_subs[];
extern const ipc_sub_t wm_subs[];
