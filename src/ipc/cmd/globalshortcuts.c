#include "ipc/args.h"
#include "ipc/registry.h"
#include "protocol/global_shortcuts.h"

void ipc_cmd_globalshortcuts(ipc_args_t *a) {
	ipc_list_send(a, global_shortcuts_list);
}
