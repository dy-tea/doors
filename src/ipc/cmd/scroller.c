#include "ipc/args.h"
#include "ipc/helpers.h"
#include "ipc/registry.h"
#include "layout/layout.h"
#include "layout/master_stack.h"
#include "layout/scroller.h"
#include "output/output.h"
#include "server.h"
#include "tree.h"
#include <limits.h>

// find column and tile index for a focused client in the scroller state
static bool find_focused_tile(ipc_args_t *a, desktop_t *desk, int *out_col) {
	if (!desk->focus || !desk->focus->client || !desk->scroller_state) {
		ipc_fail(a, "No focused tiled window\n");
		return false;
	}

	scroller_state_t *s = desk->scroller_state;
	for (int i = 0; i < s->column_count; i++) {
		for (int j = 0; j < s->columns[i].tile_count; j++) {
			if (s->columns[i].tiles[j].client != desk->focus->client)
				continue;

			if (out_col)
				*out_col = i;
			return true;
		}
	}

	ipc_fail(a, "No focused tiled window\n");
	return false;
}

static bool find_focused_client(ipc_args_t *a, desktop_t *desk) {
	if (!desk->focus || !desk->focus->client) {
		ipc_fail(a, "No focused client\n");
		return false;
	}
	return true;
}

static void scr_proportion(ipc_args_t *a) {
	float value;
	if (!ipc_float(a, "proportion", 0.1f, 1.0f, &value))
		return;

	output_t *mon;
	desktop_t *desk = ipc_focused_desk(a, &mon);
	if (!desk)
		return;

	int col;
	if (!find_focused_tile(a, desk, &col))
		return;

	scroller_state_t *s = desk->scroller_state;
	s->columns[col].width.type = SCROLLER_WIDTH_PROPORTION;
	s->columns[col].width.value = value;
	arrange(mon, desk, true);
	ipc_ok(a, "Proportion set\n");
}

static void scr_resize(ipc_args_t *a) {
	float delta;
	if (!ipc_float(a, "delta", -1e6, 1e6, &delta))
		return;

	output_t *mon;
	desktop_t *desk = ipc_focused_desk(a, &mon);
	if (!desk)
		return;

	int col;
	if (!find_focused_tile(a, desk, &col))
		return;

	scroller_state_t *s = desk->scroller_state;
	double prop = s->columns[col].width.value + delta;
	if (prop < 0.1)
		prop = 0.1;
	if (prop > 1.0)
		prop = 1.0;

	s->columns[col].width.type = SCROLLER_WIDTH_PROPORTION;
	s->columns[col].width.value = prop;
	arrange(mon, desk, true);
	ipc_ok(a, "Resized\n");
}

static void scr_stack(ipc_args_t *a) {
	output_t *mon;
	desktop_t *desk = ipc_focused_desk(a, &mon);
	if (!desk || !find_focused_client(a, desk))
		return;

	if (!scroller_consume_into_column(desk)) {
		ipc_fail(a, "Nothing to stack into\n");
		return;
	}

	arrange(mon, desk, true);
	ipc_ok(a, "Stacked\n");
}

static void scr_unstack(ipc_args_t *a) {
	output_t *mon;
	desktop_t *desk = ipc_focused_desk(a, &mon);
	if (!desk || !find_focused_client(a, desk))
		return;

	if (!scroller_expel_from_column(desk)) {
		ipc_fail(a, "Nothing to unstack\n");
		return;
	}

	arrange(mon, desk, true);
	ipc_ok(a, "Unstacked\n");
}

static void scr_cycle_preset(ipc_args_t *a) {
	if (!scroller_proportion_preset || scroller_proportion_preset_count == 0) {
		ipc_fail(a, "No presets configured\n");
		return;
	}

	output_t *mon;
	desktop_t *desk = ipc_focused_desk(a, &mon);
	if (!desk)
		return;

	int col;
	if (!find_focused_tile(a, desk, &col))
		return;

	scroller_state_t *s = desk->scroller_state;
	double cur = s->columns[col].width.value;
	int next = 0;
	for (int i = 0; i < scroller_proportion_preset_count; i++) {
		if (fabs(scroller_proportion_preset[i] - cur) < 0.01f) {
			next = i + 1;
			break;
		}
	}
	if (next >= scroller_proportion_preset_count)
		next = 0;

	s->columns[col].width.type = SCROLLER_WIDTH_PROPORTION;
	s->columns[col].width.value = scroller_proportion_preset[next];
	arrange(mon, desk, true);
	ipc_ok(a, "Cycled to next preset\n");
}

static void scr_center(ipc_args_t *a) {
	output_t *mon;
	desktop_t *desk = ipc_focused_desk(a, &mon);
	if (!desk || !find_focused_client(a, desk))
		return;

	scroller_center_window(desk, desk->focus->client);
	ipc_ok(a, "Window centered\n");
}

const ipc_sub_t scroller_subs[] = {
	IPC_SUB("proportion", "set_proportion", "scroller proportion <0.1-1.0>", scr_proportion),
	IPC_SUB("resize", NULL, "scroller resize <delta>", scr_resize),
	IPC_SUB("stack", NULL, "scroller stack", scr_stack),
	IPC_SUB("unstack", NULL, "scroller unstack", scr_unstack),
	IPC_SUB("cycle_preset", NULL, "scroller cycle_preset", scr_cycle_preset),
	IPC_SUB("center", NULL, "scroller center", scr_center),
	IPC_SUB_END,
};

void ipc_cmd_scroller(ipc_args_t *a) {
	if (!ipc_sub_dispatch(a, scroller_subs))
		ipc_fail_unknown(a, scroller_subs);
}

static const cfg_enum_value_t orientation_values[] = {
	{"left", MASTER_LEFT},
	{"right", MASTER_RIGHT},
	{"top", MASTER_TOP},
	{"bottom", MASTER_BOTTOM},
	{"down", MASTER_BOTTOM},
	{"center", MASTER_CENTER},
	IPC_ENUM_END,
};

static void ms_cycle_orientation(ipc_args_t *a) {
	output_t *mon;
	desktop_t *desk = ipc_focused_desk(a, &mon);
	if (!desk)
		return;

	master_stack_cycle_orientation(desk);
	arrange(mon, desk, true);
	ipc_ok(a, "Orientation cycled\n");
}

static void ms_orientation(ipc_args_t *a) {
	long orientation;
	if (!ipc_enum(a, "orientation", orientation_values, &orientation))
		return;

	output_t *mon;
	desktop_t *desk = ipc_focused_desk(a, &mon);
	if (!desk)
		return;

	master_stack_set_orientation(desk, (master_area_orientation_t)orientation);
	arrange(mon, desk, true);
	ipc_ok(a, "Orientation set\n");
}

static void ms_cycle_stack_layout(ipc_args_t *a) {
	output_t *mon;
	desktop_t *desk = ipc_focused_desk(a, &mon);
	if (!desk)
		return;

	master_stack_cycle_stack_layout(desk);
	arrange(mon, desk, true);
	ipc_ok(a, "Stack layout cycled\n");
}

static void ms_inc(ipc_args_t *a) {
	output_t *mon;
	desktop_t *desk = ipc_focused_desk(a, &mon);
	if (!desk)
		return;

	master_stack_increment(desk);
	arrange(mon, desk, true);
	ipc_ok(a, "Master added\n");
}

static void ms_dec(ipc_args_t *a) {
	output_t *mon;
	desktop_t *desk = ipc_focused_desk(a, &mon);
	if (!desk)
		return;

	master_stack_decrement(desk);
	arrange(mon, desk, true);
	ipc_ok(a, "master removed\n");
}

static void ms_flip(ipc_args_t *a) {
	output_t *mon;
	desktop_t *desk = ipc_focused_desk(a, &mon);
	if (!desk)
		return;

	master_stack_flip_orientation(desk);
	arrange(mon, desk, true);
	ipc_ok(a, "orientation flipped\n");
}

static void ms_promote(ipc_args_t *a) {
	output_t *mon;
	desktop_t *desk = ipc_focused_desk(a, &mon);
	if (!desk)
		return;

	if (!master_stack_promote(desk)) {
		ipc_fail(a, "Focus a secondary tiled window\n");
		return;
	}

	arrange(mon, desk, true);
	ipc_ok(a, "focused window promoted\n");
}

static void ms_demote(ipc_args_t *a) {
	output_t *mon;
	desktop_t *desk = ipc_focused_desk(a, &mon);
	if (!desk)
		return;

	if (!master_stack_demote(desk)) {
		ipc_fail(a, "Focus a master tiled window\n");
		return;
	}

	arrange(mon, desk, true);
	ipc_ok(a, "Focused window demoted\n");
}

static void ms_set_count(ipc_args_t *a) {
	int count;
	if (!ipc_int(a, "count", 0, INT_MAX, &count))
		return;

	output_t *mon;
	desktop_t *desk = ipc_focused_desk(a, &mon);
	if (!desk)
		return;

	master_stack_set_count(desk, count);
	arrange(mon, desk, true);
	ipc_ok(a, "Master count set\n");
}

static void ms_set_ratio(ipc_args_t *a) {
	float ratio;
	if (!ipc_float(a, "ratio", 0.1f, 0.9f, &ratio))
		return;

	output_t *mon;
	desktop_t *desk = ipc_focused_desk(a, &mon);
	if (!desk)
		return;

	desk->master_stack.ratio = ratio;
	arrange(mon, desk, true);
	ipc_ok(a, "Ratio set\n");
}

const ipc_sub_t master_stack_subs[] = {
	IPC_SUB("cycle_orientation", NULL, "master_stack cycle_orientation", ms_cycle_orientation),
	IPC_SUB("orientation", NULL, "master_stack orientation <left|right|top|bottom|center>",
		ms_orientation),
	IPC_SUB("cycle_stack_layout", NULL, "master_stack cycle_stack_layout", ms_cycle_stack_layout),
	IPC_SUB("inc", NULL, "master_stack inc", ms_inc),
	IPC_SUB("dec", NULL, "master_stack dec", ms_dec),
	IPC_SUB("flip", NULL, "master_stack flip", ms_flip),
	IPC_SUB("promote", NULL, "master_stack promote", ms_promote),
	IPC_SUB("demote", NULL, "master_stack demote", ms_demote),
	IPC_SUB("set_count", NULL, "master_stack set_count <count>", ms_set_count),
	IPC_SUB("set_ratio", NULL, "master_stack set_ratio <0.1-0.9>", ms_set_ratio),
	IPC_SUB_END,
};

void ipc_cmd_master_stack(ipc_args_t *a) {
	if (!ipc_sub_dispatch(a, master_stack_subs))
		ipc_fail_unknown(a, master_stack_subs);
}
