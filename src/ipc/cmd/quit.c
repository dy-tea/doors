#include "ipc/args.h"
#include "ipc/registry.h"
#include "server.h"
#include <wlr/util/log.h>

void ipc_cmd_quit(ipc_args_t *a) {
	wlr_log(WLR_INFO, "Quit requested via IPC");
	wl_display_terminate(server.wl_display);
	ipc_ok(a, "quit\n");
}
