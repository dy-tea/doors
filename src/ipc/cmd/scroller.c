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

static bool cfg_parse_size_arg(ipc_args_t *a, const char *what, scroller_size_t *out) {
	const char *value;
	if (!ipc_need(a, what, &value))
		return false;

	size_t len = strlen(value);
	if (len > 2 && strcmp(value + len - 2, "px") == 0) {
		char *end;
		double val = strtod(value, &end);
		if (end != value + len - 2 || val <= 0.0 || val > 65535.0)
			goto invalid;

		out->type = SCROLLER_SIZE_FIXED;
		out->value = val;
		return true;
	}

	char *end;
	double val = strtod(value, &end);
	if (end == value || *end != '\0' || val <= 0.0 || val > 1.0)
		goto invalid;

	out->type = SCROLLER_SIZE_PROPORTION;
	out->value = val;
	return true;

invalid:
	ipc_fail(a, "%s: invalid value \"%s\", expected a proportion in (0, 1] or a size like \"640px\"\n",
		what, value);
	return false;
}

static void scr_proportion(ipc_args_t *a) {
	scroller_size_t size;
	if (!cfg_parse_size_arg(a, "proportion", &size))
		return;

	output_t *mon;
	desktop_t *desk = ipc_focused_desk(a, &mon);
	if (!desk)
		return;

	int col;
	if (!find_focused_tile(a, desk, &col))
		return;

	if (!scroller_set_column_width(desk, size)) {
		ipc_fail(a, "No focused tiled window\n");
		return;
	}

	arrange(mon, desk, true);
	ipc_ok(a, "Column width set\n");
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

	if (!scroller_resize_width(desk, delta)) {
		ipc_fail(a, "No focused tiled window\n");
		return;
	}

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

static void scr_cycle_preset(ipc_args_t *a, bool heights) {
	output_t *mon;
	desktop_t *desk = ipc_focused_desk(a, &mon);
	if (!desk)
		return;

	int col;
	if (!find_focused_tile(a, desk, &col))
		return;

	int step = 1;
	const char *dir = ipc_peek(a);
	if (dir != NULL) {
		if (strcmp(dir, "backward") == 0 || strcmp(dir, "backwards") == 0)
			step = -1;
		else if (strcmp(dir, "forward") != 0 && strcmp(dir, "forwards") != 0) {
			ipc_fail(a, "Expected forward or backward\n");
			return;
		}
		ipc_take(a);
	}

	bool ok = heights ? scroller_cycle_height_preset(desk, step) : scroller_cycle_width_preset(desk,
		step);
	if (!ok) {
		ipc_fail(a, "No presets configured\n");
		return;
	}

	arrange(mon, desk, true);
	ipc_ok(a, "Cycled to next preset\n");
}

static void scr_cycle_width(ipc_args_t *a) {
	scr_cycle_preset(a, false);
}

static void scr_cycle_height(ipc_args_t *a) {
	scr_cycle_preset(a, true);
}

static void scr_toggle_full_width(ipc_args_t *a) {
	output_t *mon;
	desktop_t *desk = ipc_focused_desk(a, &mon);
	if (!desk)
		return;

	if (!find_focused_client(a, desk))
		return;

	if (!scroller_toggle_column_full_width(desk)) {
		ipc_fail(a, "No focused tiled window\n");
		return;
	}

	arrange(mon, desk, true);
	ipc_ok(a, "Column width toggled\n");
}

static void scr_expand_column(ipc_args_t *a) {
	output_t *mon;
	desktop_t *desk = ipc_focused_desk(a, &mon);
	if (!desk)
		return;

	if (!find_focused_client(a, desk))
		return;

	if (!scroller_expand_column_to_available_width(desk)) {
		ipc_fail(a, "No focused tiled window\n");
		return;
	}

	arrange(mon, desk, true);
	ipc_ok(a, "Column expanded\n");
}

static void scr_set_height(ipc_args_t *a) {
	scroller_size_t size;
	if (!cfg_parse_size_arg(a, "proportion or px size", &size))
		return;

	output_t *mon;
	desktop_t *desk = ipc_focused_desk(a, &mon);
	if (!desk)
		return;

	if (!find_focused_client(a, desk))
		return;

	if (!scroller_set_window_height(desk, size)) {
		ipc_fail(a, "No focused tiled window\n");
		return;
	}

	arrange(mon, desk, true);
	ipc_ok(a, "Window height set\n");
}

static void scr_reset_height(ipc_args_t *a) {
	output_t *mon;
	desktop_t *desk = ipc_focused_desk(a, &mon);
	if (!desk)
		return;

	if (!find_focused_client(a, desk))
		return;

	if (!scroller_reset_window_height(desk)) {
		ipc_fail(a, "No focused tiled window\n");
		return;
	}

	arrange(mon, desk, true);
	ipc_ok(a, "Window height reset\n");
}

static void scr_center(ipc_args_t *a) {
	output_t *mon;
	desktop_t *desk = ipc_focused_desk(a, &mon);
	if (!desk || !find_focused_client(a, desk))
		return;

	scroller_center_window(desk, desk->focus->client);
	arrange(mon, desk, true);
	ipc_ok(a, "Window centered\n");
}

static void scr_center_visible(ipc_args_t *a) {
	output_t *mon;
	desktop_t *desk = ipc_focused_desk(a, &mon);
	if (!desk)
		return;

	if (!find_focused_client(a, desk))
		return;

	scroller_center_visible_columns(desk);
	arrange(mon, desk, true);
	ipc_ok(a, "Visible columns centered\n");
}

typedef enum {
	SCR_FOCUS,
	SCR_MOVE,
} scr_kind_t;

typedef struct {
	const char *name;
	scr_kind_t kind;
	bool (*fn)(desktop_t * );
} scr_action_t;

static const scr_action_t scr_actions[] = {
	{"left", SCR_FOCUS, scroller_focus_prev},
	{"right", SCR_FOCUS, scroller_focus_next},
	{"up", SCR_FOCUS, scroller_focus_up},
	{"down", SCR_FOCUS, scroller_focus_down},
	{"west", SCR_FOCUS, scroller_focus_prev},
	{"east", SCR_FOCUS, scroller_focus_next},
	{"north", SCR_FOCUS, scroller_focus_up},
	{"south", SCR_FOCUS, scroller_focus_down},
	{"focus_left", SCR_FOCUS, scroller_focus_prev},
	{"focus_right", SCR_FOCUS, scroller_focus_next},
	{"focus_up", SCR_FOCUS, scroller_focus_up},
	{"focus_down", SCR_FOCUS, scroller_focus_down},
	{"focus_column_first", SCR_FOCUS, scroller_focus_column_first},
	{"focus_column_last", SCR_FOCUS, scroller_focus_column_last},
	{"focus_down_or_left", SCR_FOCUS, scroller_focus_down_or_left},
	{"focus_down_or_right", SCR_FOCUS, scroller_focus_down_or_right},
	{"focus_up_or_left", SCR_FOCUS, scroller_focus_up_or_left},
	{"focus_up_or_right", SCR_FOCUS, scroller_focus_up_or_right},
	{"move_column_left", SCR_MOVE, scroller_move_column_left},
	{"move_column_right", SCR_MOVE, scroller_move_column_right},
	{"move_column_up", SCR_MOVE, scroller_move_column_up},
	{"move_column_down", SCR_MOVE, scroller_move_column_down},
	{"move_column_to_first", SCR_MOVE, scroller_move_column_to_first},
	{"move_column_to_last", SCR_MOVE, scroller_move_column_to_last},
	{"consume_window_into_column", SCR_MOVE, scroller_consume_into_column},
	{"expel_window_from_column", SCR_MOVE, scroller_expel_from_column},
};

static bool scroller_swap_dir(direction_t dir, desktop_t *d) {
	return scroller_swap(NULL, d, dir);
}

static void scr_action(ipc_args_t *a, int kind, bool swap) {
	const char *what;
	if (!ipc_need(a, "action", &what))
		return;

	const scr_action_t *match = NULL;
	for (size_t i = 0; i < IPC_ARRAY_LEN(scr_actions); i++) {
		if (strcmp(scr_actions[i].name, what) != 0)
			continue;
		if (kind >= 0 && (int)scr_actions[i].kind != kind)
			continue;

		match = &scr_actions[i];
		break;
	}

	if (match == NULL) {
		ipc_fail(a, "Unknown scroller action \"%s\"\n", what);
		return;
	}

	output_t *mon;
	desktop_t *desk = ipc_focused_desk(a, &mon);
	if (!desk)
		return;

	if (!find_focused_client(a, desk))
		return;

	bool ok;
	if (swap) {
		// swapping only makes sense for the four cardinal directions
		if (match->kind != SCR_FOCUS) {
			ipc_fail(a, "\"%s\" cannot be swapped\n", what);
			return;
		}

		if (strcmp(what, "left") == 0 || strcmp(what, "west") == 0 || strcmp(what, "focus_left") == 0)
			ok = scroller_swap_dir(DIR_WEST, desk);
		else if (strcmp(what, "right") == 0 || strcmp(what, "east") == 0 || strcmp(what,
			"focus_right") == 0)
			ok = scroller_swap_dir(DIR_EAST, desk);
		else if (strcmp(what, "up") == 0 || strcmp(what, "north") == 0 || strcmp(what, "focus_up") == 0)
			ok = scroller_swap_dir(DIR_NORTH, desk);
		else if (strcmp(what, "down") == 0 || strcmp(what, "south") == 0 || strcmp(what,
			"focus_down") == 0)
			ok = scroller_swap_dir(DIR_SOUTH, desk);
		else
			ok = false;
	} else {
		ok = match->fn(desk);
	}

	if (!ok) {
		ipc_fail(a, "Nothing to %s\n", what);
		return;
	}

	arrange(mon, desk, true);
	ipc_okf(a, "%s\n", what);
}

static void scr_action_any(ipc_args_t *a) {
	scr_action(a, -1, false);
}

static void scr_action_focus(ipc_args_t *a) {
	scr_action(a, SCR_FOCUS, false);
}

static void scr_action_move(ipc_args_t *a) {
	scr_action(a, SCR_MOVE, false);
}

static void scr_action_swap(ipc_args_t *a) {
	scr_action(a, SCR_FOCUS, true);
}

const ipc_sub_t scroller_subs[] = {
	IPC_SUB("proportion", "set_width", "scroller proportion <0-1|px>", scr_proportion),
	IPC_SUB("resize", NULL, "scroller resize <delta>", scr_resize),
	IPC_SUB("stack", NULL, "scroller stack", scr_stack),
	IPC_SUB("unstack", NULL, "scroller unstack", scr_unstack),
	IPC_SUB("cycle_preset", NULL, "scroller cycle_preset [forward|backward]", scr_cycle_width),
	IPC_SUB("cycle_width", NULL, "scroller cycle_width [forward|backward]", scr_cycle_width),
	IPC_SUB("cycle_height", NULL, "scroller cycle_height [forward|backward]", scr_cycle_height),
	IPC_SUB("toggle_full_width", NULL, "scroller toggle_full_width", scr_toggle_full_width),
	IPC_SUB("expand_column", NULL, "scroller expand_column", scr_expand_column),
	IPC_SUB("set_height", NULL, "scroller set_height <0-1|px>", scr_set_height),
	IPC_SUB("reset_height", NULL, "scroller reset_height", scr_reset_height),
	IPC_SUB("center", NULL, "scroller center", scr_center),
	IPC_SUB("center_visible", NULL, "scroller center_visible", scr_center_visible),
	IPC_SUB("action", NULL, "scroller action <name>", scr_action_any),
	IPC_SUB("focus", NULL, "scroller focus <name>", scr_action_focus),
	IPC_SUB("move", NULL, "scroller move <name>", scr_action_move),
	IPC_SUB("swap", NULL, "scroller swap <name>", scr_action_swap),
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
