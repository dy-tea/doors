#include "ipc/args.h"
#include "ipc/ipc.h"
#include "ipc/registry.h"
#include "scratchpad.h"
#include "transaction.h"
#include <string.h>

// "app_id:firefox" or "title:Untitled"
static node_t *find_entry(ipc_args_t *a, const char *what) {
	const char *spec;
	if (!ipc_need(a, what, &spec))
		return NULL;

	if (strncmp(spec, "app_id:", 7) == 0)
		return scratchpad_find_by_app_id(spec + 7);
	if (strncmp(spec, "title:", 6) == 0)
		return scratchpad_find_by_title(spec + 6);

	ipc_fail(a, "Expected app_id:<value> or title:<value>\n");
	return NULL;
}

static void sp_show(ipc_args_t *a) {
	if (!ipc_peek(a)) {
		scratchpad_toggle_auto();
		transaction_commit_dirty();
		ipc_ok(a, "Scratchpad toggled\n");
		return;
	}

	node_t *n = find_entry(a, "entry");
	if (!n) {
		ipc_fail(a, "Matching entry not found\n");
		return;
	}

	scratchpad_show(n);
	transaction_commit_dirty();
	ipc_ok(a, "Scratchpad shown\n");
}

static void sp_hide(ipc_args_t *a) {
	node_t *n = find_entry(a, "entry");
	if (!n) {
		ipc_fail(a, "Matching entry not found\n");
		return;
	}

	scratchpad_add(n);
	transaction_commit_dirty();
	ipc_ok(a, "Scratchpad hidden\n");
}

static void sp_list(ipc_args_t *a) {
	char buf[DOORS_BUFSIZ];
	ipc_buf_t b;
	ipc_buf_init(&b, buf, sizeof(buf));

	int count = scratchpad_count();
	ipc_buff(&b, "%d\n", count);

	for (int i = 0; i < count; i++) {
		node_t *n = scratchpad_nth(i);
		if (n == NULL || n->client == NULL)
			continue;

		// where it was minimized from
		const char *desk = n->desktop != NULL ? n->desktop->name : "-";

		const char *state = "tiled";
		if (n->client->state == STATE_FULLSCREEN)
			state = "fullscreen";
		else if (n->client->state == STATE_FLOATING)
			state = "floating";
		else if (n->client->state == STATE_PSEUDO_TILED)
			state = "pseudo_tiled";

		ipc_buff(&b, "%s\n\t%s\n\t%s\n\t%s\n\t%u\n\t%s\n", n->client->app_id[0] ? n->client->app_id : "?",
			n->client->title[0] ? n->client->title : "?", desk, state, n->id,
			n->client->flags.maximized ? "maximized" : "-");
	}

	ipc_buf_send(a, &b);
}

static const ipc_sub_t scratchpad_subs[] = {
	IPC_SUB("show", NULL, "scratchpad show [app_id:<id>|title:<title>]", sp_show),
	IPC_SUB("hide", NULL, "scratchpad hide <app_id:<id>|title:<title>>", sp_hide),
	IPC_SUB("list", NULL, "scratchpad list", sp_list),
	IPC_SUB_END,
};

void ipc_cmd_scratchpad(ipc_args_t *a) {
	if (!ipc_sub_dispatch(a, scratchpad_subs))
		ipc_fail_unknown(a, scratchpad_subs);
}
