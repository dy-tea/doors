#include "ipc/args.h"
#include "ipc/registry.h"
#include <stdlib.h>
#include <string.h>

static bool valid_env_name(const char *name) {
	return name[0] != '\0' && strchr(name, '=') == NULL;
}

static void env_set(ipc_args_t *a) {
	const char *name, *value;
	if (!ipc_need(a, "name", &name) || !ipc_need(a, "value", &value))
		return;
	if (!ipc_end(a))
		return;

	if (!valid_env_name(name)) {
		ipc_fail(a, "Invalid variable name \"%s\"\n", name);
		return;
	}
	if (setenv(name, value, 1) != 0) {
		ipc_fail(a, "Failed to set variable\n");
		return;
	}

	ipc_ok(a, "Environment variable set\n");
}

static void env_unset(ipc_args_t *a) {
	const char *name;
	if (!ipc_need(a, "name", &name) || !ipc_end(a))
		return;

	if (!valid_env_name(name)) {
		ipc_fail(a, "Invalid variable name \"%s\"\n", name);
		return;
	}
	if (unsetenv(name) != 0) {
		ipc_fail(a, "Failed to unset variable\n");
		return;
	}

	ipc_ok(a, "Environment variable unset\n");
}

const ipc_sub_t env_subs[] = {
	IPC_SUB("set", NULL, "env set <name> <value>", env_set),
	IPC_SUB("unset", NULL, "env unset <name>", env_unset),
	IPC_SUB_END,
};

void ipc_cmd_env(ipc_args_t *a) {
	if (!ipc_sub_dispatch(a, env_subs))
		ipc_fail_unknown(a, env_subs);
}
