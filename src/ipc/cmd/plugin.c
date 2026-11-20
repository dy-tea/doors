#include "ipc/args.h"
#include "ipc/registry.h"
#include "plugin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void plugin_load_cmd(ipc_args_t *a) {
	const char *path;
	if (!ipc_need(a, "path", &path) || !ipc_end(a))
		return;

	if (a->dry_run) {
		ipc_ok(a, "Plugin would be loaded\n");
		return;
	}

	const size_t before = layout_count();
	if (!plugin_load(path)) {
		ipc_fail(a, "Failed to load plugin \"%s\"\n", path);
		return;
	}

	IPC_REPLY_GROWING(reply);
	for (size_t i = before; i < layout_count(); i++) {
		const layout_t id = (layout_t)i;
		ipc_buff_sep(&reply, ", ", "\"%s\" (layout '%c')", layout_name(id), layout_to_char(id));
	}
	ipc_buff(&reply, "\n");
	ipc_buf_send(a, &reply);
}

static void list_plugin(const doors_plugin_t *plugin, const char *path, void *user) {
	ipc_buf_t *reply = user;
	ipc_buff(reply, "%s (%s)\n", plugin != NULL && plugin->name != NULL ? plugin->name : path, path);
	if (plugin != NULL && plugin->layouts != NULL) {
		for (size_t i = 0; plugin->layouts[i] != NULL; i++) {
			const layout_impl_t *impl = plugin->layouts[i];
			layout_t id;
			if (impl->name != NULL && layout_find_by_name(impl->name, &id))
				ipc_buff(reply, "  %s (layout '%c')\n", impl->name, layout_to_char(id));
			else
				ipc_buff(reply, "  %s\n", impl->name != NULL ? impl->name : "?");
		}
	}
}

static void plugin_list_cmd(ipc_args_t *a) {
	if (!ipc_end(a))
		return;

	IPC_REPLY_GROWING(reply);
	plugin_iter(list_plugin, &reply);
	if (reply.len == 0)
		ipc_buff(&reply, "no plugins loaded\n");
	ipc_buf_send(a, &reply);
}

const ipc_sub_t plugin_subs[] = {
	IPC_SUB("load", NULL, "plugin load <path>", plugin_load_cmd),
	IPC_SUB("list", NULL, "plugin list", plugin_list_cmd),
	IPC_SUB_END,
};

void ipc_cmd_plugin(ipc_args_t *a) {
	if (!ipc_sub_dispatch(a, plugin_subs))
		ipc_fail_unknown(a, plugin_subs);
}
