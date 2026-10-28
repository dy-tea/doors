#include "config.h"
#include "ipc/args.h"
#include "ipc/helpers.h"
#include "ipc/registry.h"
#include "output/output.h"
#include "server.h"
#include "transaction.h"
#include "tree.h"

static void tree_balance(ipc_args_t *a) {
	output_t *m = server.focused_output;
	if (!m || !m->desk) {
		ipc_fail(a, "No focused desktop\n");
		return;
	}
	if (!m->desk->root) {
		ipc_fail(a, "No tree\n");
		return;
	}

	balance_tree(m->desk->root);
	transaction_commit_dirty();
	ipc_ok(a, "balanced\n");
}

static void tree_equalize(ipc_args_t *a) {
	output_t *m = server.focused_output;
	if (!m || !m->desk) {
		ipc_fail(a, "No focused desktop\n");
		return;
	}
	if (!m->desk->root) {
		ipc_fail(a, "No tree\n");
		return;
	}

	equalize_tree(m->desk->root);
	transaction_commit_dirty();
	ipc_ok(a, "equalized\n");
}

static void tree_rotate(ipc_args_t *a) {
	const char *arg;
	if (!ipc_need(a, "direction", &arg))
		return;

	bind_action_t action = bind_action_from_name(arg);
	if (action != BIND_ROTATE_CW && action != BIND_ROTATE_CCW) {
		ipc_fail(a, "Unknown direction \"%s\", expected clockwise|counterclockwise\n", arg);
		return;
	}

	execute_bind_action(action);
	ipc_ok(a, "rotated\n");
}

static void tree_flip(ipc_args_t *a) {
	const char *arg;
	if (!ipc_need(a, "axis", &arg))
		return;

	bind_action_t action = bind_action_from_name(arg);
	if (action != BIND_FLIP_HORIZONTAL && action != BIND_FLIP_VERTICAL) {
		ipc_fail(a, "Unknown axis \"%s\", expected horizontal|vertical\n", arg);
		return;
	}

	execute_bind_action(action);
	ipc_ok(a, "flipped\n");
}

static void tree_presel(ipc_args_t *a) {
	const char *arg;
	if (!ipc_need(a, "direction", &arg))
		return;

	if (streq(arg, "cancel")) {
		execute_bind_action(BIND_PRESEL_CANCEL);
		ipc_ok(a, "Presel cancelled\n");
		return;
	}

	bind_action_t action = bind_action_from_name(arg);
	if (action < BIND_PRESEL_NORTH || action > BIND_PRESEL_CANCEL) {
		char buf[64];
		snprintf(buf, sizeof(buf), "presel_%s", arg);
		action = bind_action_from_name(buf);
	}
	if (action < BIND_PRESEL_NORTH || action > BIND_PRESEL_CANCEL) {
		ipc_fail(a, "Unknown direction \"%s\"\n", arg);
		return;
	}

	execute_bind_action(action);
	ipc_ok(a, "Presel set\n");
}

static void tree_presel_ratio(ipc_args_t *a) {
	double rat;
	if (!ipc_double(a, "ratio", 0, 1, &rat))
		return;
	if (rat <= 0 || rat >= 1) {
		ipc_fail(a, "Ratio must be between 0 and 1\n");
		return;
	}

	output_t *m;
	node_t *n = ipc_focused(a, &m);
	if (!n)
		return;
	if (n->vacant) {
		ipc_fail(a, "Focused node is a receptacle\n");
		return;
	}

	if (!n->presel)
		n->presel = make_presel();
	n->presel->split_ratio = rat;

	transaction_commit_dirty();
	ipc_ok(a, "Presel ratio set\n");
}

static const str_pair_t circulate_values[] = {
	{"forward", 1},
	{"f", 1},
	{"backward", -1},
	{"back", -1},
	{"b", -1},
	IPC_ENUM_END,
};

static void tree_circulate(ipc_args_t *a) {
	long value;
	if (!ipc_enum(a, "direction", circulate_values, &value))
		return;

	output_t *m;
	node_t *n = ipc_focused(a, &m);
	if (!n)
		return;

	node_t *next = value > 0 ? next_leaf(n, m->desk->root) : prev_leaf(n, m->desk->root);
	if (next) {
		m->desk->focus = next;
		focus_node(m, m->desk, next);
	}

	ipc_ok(a, "Circulated\n");
}

const ipc_sub_t tree_subs[] = {
	IPC_SUB("balance", NULL, "tree balance", tree_balance),
	IPC_SUB("equalize", NULL, "tree equalize", tree_equalize),
	IPC_SUB("rotate", NULL, "tree rotate <clockwise|counterclockwise>", tree_rotate),
	IPC_SUB("flip", NULL, "tree flip <horizontal|vertical>", tree_flip),
	IPC_SUB("presel", NULL, "tree presel <west|east|north|south|cancel>", tree_presel),
	IPC_SUB("presel_ratio", "presel-ratio", "tree presel_ratio <ratio>", tree_presel_ratio),
	IPC_SUB("circulate", NULL, "tree circulate <forward|backward>", tree_circulate),
	IPC_SUB_END,
};

void ipc_cmd_tree(ipc_args_t *a) {
	if (!ipc_sub_dispatch(a, tree_subs))
		ipc_fail_unknown(a, tree_subs);
}
