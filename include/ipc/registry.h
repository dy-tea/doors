#pragma once

#include "ipc/args.h"

typedef struct ipc_cmd_t {
	const char *name;
	const char *summary;
	void (*run)(ipc_args_t *a);
	const ipc_sub_t *subs;
} ipc_cmd_t;

void ipc_cmd_actions(ipc_args_t *a);
void ipc_cmd_config(ipc_args_t *a);
void ipc_cmd_desktop(ipc_args_t *a);
void ipc_cmd_env(ipc_args_t *a);
void ipc_cmd_globalshortcuts(ipc_args_t *a);
void ipc_cmd_hotkey(ipc_args_t *a);
void ipc_cmd_input(ipc_args_t *a);
void ipc_cmd_keyboard_grouping(ipc_args_t *a);
void ipc_cmd_master_stack(ipc_args_t *a);
void ipc_cmd_node(ipc_args_t *a);
void ipc_cmd_output(ipc_args_t *a);
void ipc_cmd_query(ipc_args_t *a);
void ipc_cmd_quit(ipc_args_t *a);
void ipc_cmd_rule(ipc_args_t *a);
void ipc_cmd_scratchpad(ipc_args_t *a);
void ipc_cmd_scroller(ipc_args_t *a);
void ipc_cmd_seat(ipc_args_t *a);
void ipc_cmd_sessions(ipc_args_t *a);
bool ipc_cmd_subscribe(ipc_args_t *a); // true when the handler keeps the socket
void ipc_cmd_wm(ipc_args_t *a);

extern const ipc_sub_t desktop_subs[];
extern const ipc_sub_t env_subs[];
extern const ipc_sub_t master_stack_subs[];
extern const ipc_sub_t node_subs[];
extern const ipc_sub_t output_subs[];
extern const ipc_sub_t query_subs[];
extern const ipc_sub_t rule_subs[];
extern const ipc_sub_t scroller_subs[];
extern const ipc_sub_t seat_subs[];
extern const ipc_sub_t wm_subs[];
