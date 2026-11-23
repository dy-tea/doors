#include "input/keyboard.h"
#include "ipc/args.h"
#include "ipc/helpers.h"
#include "ipc/registry.h"
#include "layout/layout.h"
#include "output/output.h"
#include "server.h"
#include "transaction.h"
#include "tree.h"
#include "types.h"

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

static const str_pair_t rotate_dirs[] = {
	{"clockwise", 1},
	{"cw", 1},
	{"counterclockwise", -1},
	{"ccw", -1},
	IPC_ENUM_END,
};

static void tree_rotate(ipc_args_t *a) {
	long dir;
	if (!ipc_enum(a, "direction", rotate_dirs, &dir))
		return;

	if (dir > 0)
		rotate_clockwise();
	else
		rotate_counterclockwise();

	ipc_ok(a, "rotated\n");
}

static const str_pair_t flip_axes[] = {
	{"horizontal", FLIP_HORIZONTAL},
	{"h", FLIP_HORIZONTAL},
	{"vertical", FLIP_VERTICAL},
	{"v", FLIP_VERTICAL},
	IPC_ENUM_END,
};

static void tree_flip(ipc_args_t *a) {
	long axis;
	if (!ipc_enum(a, "axis", flip_axes, &axis))
		return;

	output_t *m = server.focused_output;
	if (!m || !m->desk || !m->desk->root) {
		ipc_fail(a, "No tree\n");
		return;
	}

	flip_tree(m->desk->root, (flip_t)axis);
	arrange(m, m->desk, true);
	ipc_ok(a, "flipped\n");
}

static const str_pair_t presel_dirs[] = {
	{"west", DIR_WEST},
	{"east", DIR_EAST},
	{"north", DIR_NORTH},
	{"south", DIR_SOUTH},
	{"cancel", -1},
	IPC_ENUM_END,
};

static void tree_presel(ipc_args_t *a) {
	long dir;
	if (!ipc_enum(a, "direction", presel_dirs, &dir))
		return;

	if (dir < 0) {
		cancel_presel();
		ipc_ok(a, "Presel cancelled\n");
		return;
	}

	output_t *m = server.focused_output;
	if (!m || !m->desk || !m->desk->focus) {
		ipc_fail(a, "No focused node\n");
		return;
	}

	presel_dir(m->desk->focus, (direction_t)dir);
	ipc_ok(a, "Presel set\n");
}

const ipc_sub_t tree_subs[] = {
	IPC_SUB("balance", NULL, "tree balance", tree_balance),
	IPC_SUB("equalize", NULL, "tree equalize", tree_equalize),
	IPC_SUB("rotate", NULL, "tree rotate clockwise|counterclockwise", tree_rotate),
	IPC_SUB("flip", NULL, "tree flip horizontal|vertical", tree_flip),
	IPC_SUB("presel", NULL, "tree presel west|east|north|south|cancel", tree_presel),
	IPC_SUB("presel_ratio", "presel-ratio", "tree presel_ratio <ratio>", ipc_presel_ratio),
	IPC_SUB("circulate", NULL, "tree circulate forward|backward", ipc_circulate),
	IPC_SUB_END,
};

void ipc_cmd_tree(ipc_args_t *a) {
	if (!ipc_sub_dispatch(a, tree_subs))
		ipc_fail_unknown(a, tree_subs);
}
