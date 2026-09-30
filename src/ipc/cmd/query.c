#include "ipc/args.h"
#include "ipc/helpers.h"
#include "ipc/registry.h"
#include "output/output.h"
#include "protocol/workspace.h"
#include "server.h"
#include "tree.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
	output_t *mon;
	desktop_t *desk;
	node_t *node;
	bool use_names;
} query_filter_t;

static bool is_selector(const char *arg) {
	static const char *selectors[] = {
		"-m",
		"--monitor",
		"-d",
		"--desktop",
		"-n",
		"--node",
		"--names",
	};

	if (!arg)
		return false;

	for (size_t i = 0; i < IPC_ARRAY_LEN(selectors); i++) {
		if (streq(arg, selectors[i]))
			return true;
	}
	return false;
}

static bool query_parse_selectors(ipc_args_t *a, query_filter_t *f) {
	while (is_selector(ipc_peek(a))) {
		const char *opt = ipc_take(a);

		if (streq(opt, "--names")) {
			f->use_names = true;
			continue;
		}

		const bool want_monitor = streq(opt, "-m") || streq(opt, "--monitor");
		const bool want_desktop = streq(opt, "-d") || streq(opt, "--desktop");

		const char *what = want_monitor ? "monitor" : want_desktop ? "desktop" : "node";
		const char *value;
		if (!ipc_need(a, what, &value))
			return false;

		if (want_monitor) {
			f->mon = ipc_output_by_name(a, value);
			if (!f->mon)
				return false;
		} else if (want_desktop) {
			f->desk = ipc_desktop_by_name(a, value);
			if (!f->desk)
				return false;
		} else {
			int id = atoi(value);
			if (id <= 0) {
				ipc_fail(a, "Invalid node id \"%s\"\n", value);
				return false;
			}

			view_t *view;
			wl_list_for_each(view, &server.views, link) {
				if (view->node && view->node->id == (uint32_t)id) {
					f->node = view->node;
					break;
				}
			}
			if (!f->node) {
				ipc_fail(a, "Node %d not found\n", id);
				return false;
			}
		}
	}

	return true;
}

static bool view_selected(const view_t *view, const query_filter_t *f) {
	if (f->node && view->node != f->node)
		return false;
	if (f->desk && view->node && view->node->output && view->node->output->desk != f->desk)
		return false;
	if (f->mon && view->node && view->node->output != f->mon)
		return false;
	return true;
}

static void q_tree(ipc_args_t *a) {
	const query_filter_t *f = a->ctx;

	IPC_REPLY(b);

	ipc_buff(&b, "{\n");

	output_t *m = f->mon;
	if (!m && !wl_list_empty(&mon_list))
		m = wl_container_of(mon_list.next, m, link);

	for (; m != NULL; m = f->mon ? NULL : (m->link.next == &mon_list ? NULL :
			wl_container_of(m->link.next, m, link))) {
		ipc_buff(&b, "  \"monitor\": {\"name\": \"%s\", \"id\": %u},\n", m->name, m->id);

		desktop_t *d = f->desk ? f->desk : m->desk;
		for (; d != NULL; d = f->desk ? NULL : (d->link.next == &m->desk_list ? NULL :
				wl_container_of(d->link.next, d, link))) {
			ipc_buff(&b, "  \"desktop\": {\"name\": \"%s\", \"id\": %u, \"layout\": %d},\n", d->name, d->id,
				d->layout);
			if (f->desk)
				break;
		}

		if (f->mon)
			break;
	}

	view_t *view;
	wl_list_for_each(view, &server.views, link) {
		if (!view_selected(view, f))
			continue;

		ipc_buff(&b, "  \"%s\": {\"app_id\": \"%s\", \"title\": \"%s\", \"identifier\": \"%s\"}\n",
			view->type == VIEW_XWAYLAND ? "xwayland" : "toplevel",
			view->node && view->node->client ? view->node->client->app_id : "?",
			view->node && view->node->client ? view->node->client->title : "?",
			view->foreign_identifier ? view->foreign_identifier : "?");
	}

	ipc_buff(&b, "}\n");
	ipc_buf_send(a, &b);
}

static void q_monitors(ipc_args_t *a) {
	const query_filter_t *f = a->ctx;

	IPC_REPLY(b);

	output_t *m;
	wl_list_for_each(m, &mon_list, link) {
		if (f->mon && m != f->mon)
			continue;

		if (f->use_names)
			ipc_buff(&b, "%s\n", m->name);
		else
			ipc_buff(&b, "%u %s\n", m->id, m->name);
	}

	ipc_buf_send(a, &b);
}

static void q_desktops(ipc_args_t *a) {
	const query_filter_t *f = a->ctx;

	IPC_REPLY(b);

	output_t *m;
	wl_list_for_each(m, &mon_list, link) {
		if (f->mon && m != f->mon)
			continue;

		desktop_t *d;
		wl_list_for_each(d, &m->desk_list, link) {
			if (f->desk && d != f->desk)
				continue;

			if (f->use_names)
				ipc_buff(&b, "%s\n", d->name);
			else
				ipc_buff(&b, "%u %s\n", d->id, d->name);
		}
	}

	ipc_buf_send(a, &b);
}

static void q_nodes(ipc_args_t *a) {
	const query_filter_t *f = a->ctx;

	IPC_REPLY(b);

	view_t *view;
	wl_list_for_each(view, &server.views, link) {
		if (!view_selected(view, f))
			continue;

		if (f->use_names) {
			const char *name = "?";
			if (view->node && view->node->client && view->node->client->title[0])
				name = view->node->client->title;
			else if (view->node && view->node->client && view->node->client->app_id[0])
				name = view->node->client->app_id;
			ipc_buff(&b, "%s\n", name);
		} else {
			ipc_buff(&b, "%u %s\n", view->node ? view->node->id : 0,
				view->foreign_identifier ? view->foreign_identifier : "?");
		}
	}

	ipc_buf_send(a, &b);
}

static void q_focused(ipc_args_t *a) {
	const query_filter_t *f = a->ctx;

	output_t *m = server.focused_output;
	if (!m || !m->desk) {
		ipc_fail(a, "No focused desktop\n");
		return;
	}

	node_t *n = m->desk->focus;
	if (!n) {
		ipc_fail(a, "No focused node\n");
		return;
	}

	const char *foreign_id = "?";
	view_t *view;
	wl_list_for_each(view, &server.views, link) {
		if (view->node != n)
			continue;

		foreign_id = view->foreign_identifier ? view->foreign_identifier : "?";
		break;
	}

	IPC_REPLY(b);

	const char *title = n->client ? n->client->title : "";
	const char *label = "?";
	if (n->client && n->client->title[0])
		label = n->client->title;
	else if (n->client && n->client->app_id[0])
		label = n->client->app_id;

	ipc_buff(&b, "{\"monitor\": \"%s\", \"desktop\": \"%s\", ", m->name, m->desk->name);
	if (f->use_names)
		ipc_buff(&b, "\"node\": \"%s\", ", label);
	else
		ipc_buff(&b, "\"id\": %u, ", n->id);

	ipc_buff(&b, "\"title\": \"%s\", \"type\": %d, "
		"\"rect\": {\"x\": %d, \"y\": %d, \"width\": %d, \"height\": %d}, "
		"\"client\": \"%s\", \"identifier\": \"%s\"}\n", title, n->split_type, n->rectangle.x,
			n->rectangle.y, n->rectangle.width, n->rectangle.height, n->client ? n->client->app_id : "?",
			foreign_id);

	ipc_buf_send(a, &b);
}

const ipc_sub_t query_subs[] = {
	IPC_SUB("-T", "--tree", "query [-m|-d|-n] -T|--tree", q_tree),
	IPC_SUB("-M", "--monitors", "query [-m <monitor>] -M|--monitors [--names]", q_monitors),
	IPC_SUB("-D", "--desktops", "query [-m <monitor>] -D|--desktops [--names]", q_desktops),
	IPC_SUB("-N", "--nodes", "query [-m|-d|-n] -N|--nodes [--names]", q_nodes),
	IPC_SUB("-f", "--focused", "query -f|--focused [--names]", q_focused),
	IPC_SUB_END,
};

void ipc_cmd_query(ipc_args_t *a) {
	query_filter_t f = {0};
	if (!query_parse_selectors(a, &f))
		return;

	a->ctx = &f;

	if (!ipc_sub_dispatch(a, query_subs)) {
		ipc_fail_unknown(a, query_subs);
		return;
	}
}
