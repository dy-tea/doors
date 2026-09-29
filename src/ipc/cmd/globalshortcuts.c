#include "ipc/args.h"
#include "ipc/ipc.h"
#include "ipc/registry.h"
#include "protocol/global_shortcuts.h"

void ipc_cmd_globalshortcuts(ipc_args_t *a) {
	char buf[DOORS_BUFSIZ];
	global_shortcuts_list(buf, sizeof(buf));
	ipc_ok(a, buf);
}
