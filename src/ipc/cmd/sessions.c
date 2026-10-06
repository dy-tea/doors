#include "ipc/args.h"
#include "ipc/ipc.h"
#include "ipc/registry.h"
#include "protocol/session_mgmt.h"

void ipc_cmd_sessions(ipc_args_t *a) {
	char buf[DOORS_BUFSIZ];
	session_mgmt_write_list(buf, sizeof(buf));
	ipc_ok(a, buf);
}
