#include "ipc/args.h"
#include "ipc/registry.h"
#include "protocol/session_mgmt.h"

void ipc_cmd_sessions(ipc_args_t *a) {
	ipc_list_send(a, session_mgmt_write_list);
}
