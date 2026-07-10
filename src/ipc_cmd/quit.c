#include "ipc.h"
#include "ipc_cmd.h"
#include "server.h"

#include <wlr/util/log.h>

void ipc_cmd_quit(char **args, int num, int client_fd) {
	(void)args;
	(void)num;
	wlr_log(WLR_INFO, "Quit requested via IPC");
	wl_display_terminate(server.wl_display);
	send_success(client_fd, "quit\n");
}
