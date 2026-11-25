#include "ipc/args.h"
#include "layout/layout.h"
#include "layout/scroller.h"
#include "layout/scroller_internal.h"
#include "output/output.h"
#include "parse.h"
#include "settings.h"
#include "transaction.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static const scroller_size_t preset_widths_default[] = {
	{SCROLLER_SIZE_PROPORTION, 1.0 / 3.0},
	{SCROLLER_SIZE_PROPORTION, 0.5},
	{SCROLLER_SIZE_PROPORTION, 2.0 / 3.0},
};

static const scroller_size_t preset_heights_default[] = {
	{SCROLLER_SIZE_PROPORTION, 1.0 / 3.0},
	{SCROLLER_SIZE_PROPORTION, 0.5},
	{SCROLLER_SIZE_PROPORTION, 2.0 / 3.0},
};

scroller_config_t scroller_config = {
	.center_focused_column = SCROLLER_CENTER_NEVER,
	.pan_fingers = 3,
	.snap_percent = 30,
	.default_column_width = {SCROLLER_SIZE_PROPORTION, 0.5},
	.preset_column_widths = (scroller_size_t *)preset_widths_default,
	.preset_column_widths_count = (int)(sizeof(preset_widths_default) /
		sizeof(preset_widths_default[0])),
	.preset_window_heights = (scroller_size_t *)preset_heights_default,
	.preset_window_heights_count = (int)(sizeof(preset_heights_default) /
		sizeof(preset_heights_default[0])),
};
typedef struct {
	output_t *mon;
	desktop_t *desk;
} scroller_target_t;

static bool scr_target(ipc_args_t *a, const bool need_client, scroller_target_t *t) {
	return layout_command_target(a, need_client, &t->mon, &t->desk);
}

static bool scr_target_tile(ipc_args_t *a, scroller_target_t *t) {
	desktop_t *desk = NULL;
	if (!layout_command_target(a, false, &t->mon, &desk))
		return false;

	t->desk = desk;
	if (desk->focus == NULL || desk->focus->client == NULL || !scroller_has_tile(desk,
			desk->focus->client)) {
		ipc_fail(a, "No focused tiled window\n");
		return false;
	}

	return true;
}

static void scr_commit(ipc_args_t *a, const scroller_target_t *t, const char *msg) {
	layout_command_commit(a, t->mon, t->desk, msg);
}

static bool scr_parse_size(ipc_args_t *a, const char *what, scroller_size_t *out) {
	const char *value;
	if (!ipc_need(a, what, &value))
		return false;

	if (!parse_scroller_size(value, out)) {
		ipc_fail(a, "%s: invalid value \"%s\", expected a proportion in (0, 1] or a size like \"640px\"\n",
			what, value);
		return false;
	}

	return true;
}

static void scr_proportion(ipc_args_t *a) {
	scroller_size_t size;
	if (!scr_parse_size(a, "proportion", &size))
		return;

	scroller_target_t t;
	if (!scr_target_tile(a, &t))
		return;

	if (!scroller_set_column_width(t.desk, size)) {
		ipc_fail(a, "No focused tiled window\n");
		return;
	}

	scr_commit(a, &t, "Column width set\n");
}

static void scr_resize(ipc_args_t *a) {
	float delta;
	if (!ipc_float(a, "delta", -1e6, 1e6, &delta))
		return;

	scroller_target_t t;
	if (!scr_target_tile(a, &t))
		return;

	if (!scroller_resize_width(t.desk, delta)) {
		ipc_fail(a, "No focused tiled window\n");
		return;
	}

	scr_commit(a, &t, "Resized\n");
}

static void scr_stack(ipc_args_t *a) {
	scroller_target_t t;
	if (!scr_target(a, true, &t))
		return;

	if (!scroller_consume_into_column(t.desk)) {
		ipc_fail(a, "Nothing to stack into\n");
		return;
	}

	scr_commit(a, &t, "Stacked\n");
}

static void scr_unstack(ipc_args_t *a) {
	scroller_target_t t;
	if (!scr_target(a, true, &t))
		return;

	if (!scroller_expel_from_column(t.desk)) {
		ipc_fail(a, "Nothing to unstack\n");
		return;
	}

	scr_commit(a, &t, "Unstacked\n");
}

static void scr_cycle_preset(ipc_args_t *a, const bool heights) {
	scroller_target_t t;
	if (!scr_target_tile(a, &t))
		return;

	int step = 1;
	const char *dir = ipc_peek(a);
	if (dir != NULL) {
		if (streq(dir, "backward") || streq(dir, "backwards"))
			step = -1;
		else if (!streq(dir, "forward") && !streq(dir, "forwards")) {
			ipc_fail(a, "Expected forward or backward\n");
			return;
		}
		ipc_take(a);
	}

	bool ok = heights ? scroller_cycle_height_preset(t.desk, step) : scroller_cycle_width_preset(t.desk,
		step);
	if (!ok) {
		ipc_fail(a, "No presets configured\n");
		return;
	}

	scr_commit(a, &t, "Cycled to next preset\n");
}

static void scr_cycle_width(ipc_args_t *a) {
	scr_cycle_preset(a, false);
}

static void scr_cycle_height(ipc_args_t *a) {
	scr_cycle_preset(a, true);
}

static void scr_toggle_full_width(ipc_args_t *a) {
	scroller_target_t t;
	if (!scr_target(a, true, &t))
		return;

	if (!scroller_toggle_column_full_width(t.desk)) {
		ipc_fail(a, "No focused tiled window\n");
		return;
	}

	scr_commit(a, &t, "Column width toggled\n");
}

static void scr_expand_column(ipc_args_t *a) {
	scroller_target_t t;
	if (!scr_target(a, true, &t))
		return;

	if (!scroller_expand_column_to_available_width(t.desk)) {
		ipc_fail(a, "No focused tiled window\n");
		return;
	}

	scr_commit(a, &t, "Column expanded\n");
}

static void scr_set_height(ipc_args_t *a) {
	scroller_size_t size;
	if (!scr_parse_size(a, "proportion or px size", &size))
		return;

	scroller_target_t t;
	if (!scr_target(a, true, &t))
		return;

	if (!scroller_set_window_height(t.desk, size)) {
		ipc_fail(a, "No focused tiled window\n");
		return;
	}

	scr_commit(a, &t, "Window height set\n");
}

static void scr_reset_height(ipc_args_t *a) {
	scroller_target_t t;
	if (!scr_target(a, true, &t))
		return;

	if (!scroller_reset_window_height(t.desk)) {
		ipc_fail(a, "No focused tiled window\n");
		return;
	}

	scr_commit(a, &t, "Window height reset\n");
}

static void scr_center(ipc_args_t *a) {
	scroller_target_t t;
	if (!scr_target(a, true, &t))
		return;

	scroller_center_window(t.desk, t.desk->focus->client);
	scr_commit(a, &t, "Window centered\n");
}

static void scr_center_visible(ipc_args_t *a) {
	scroller_target_t t;
	if (!scr_target(a, true, &t))
		return;

	scroller_center_visible_columns(t.desk);
	scr_commit(a, &t, "Visible columns centered\n");
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

static bool scroller_swap_dir(const direction_t dir, desktop_t *d) {
	return scroller_swap(NULL, d, dir);
}

static void scr_action(ipc_args_t *a, const int kind, const bool swap) {
	const char *what;
	if (!ipc_need(a, "action", &what))
		return;

	const scr_action_t *match = NULL;
	for (size_t i = 0; i < IPC_ARRAY_LEN(scr_actions); i++) {
		if (!streq(scr_actions[i].name, what))
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

	scroller_target_t t;
	if (!scr_target(a, true, &t))
		return;

	bool ok;
	if (swap) {
		// swapping only makes sense for the four cardinal directions
		if (match->kind != SCR_FOCUS) {
			ipc_fail(a, "\"%s\" cannot be swapped\n", what);
			return;
		}

		if (streq(what, "left") || streq(what, "west") || streq(what, "focus_left"))
			ok = scroller_swap_dir(DIR_WEST, t.desk);
		else if (streq(what, "right") || streq(what, "east") || streq(what, "focus_right"))
			ok = scroller_swap_dir(DIR_EAST, t.desk);
		else if (streq(what, "up") || streq(what, "north") || streq(what, "focus_up"))
			ok = scroller_swap_dir(DIR_NORTH, t.desk);
		else if (streq(what, "down") || streq(what, "south") || streq(what, "focus_down"))
			ok = scroller_swap_dir(DIR_SOUTH, t.desk);
		else
			ok = false;
	} else {
		ok = match->fn(t.desk);
	}

	if (!ok) {
		ipc_fail(a, "Nothing to %s\n", what);
		return;
	}

	arrange(t.mon, t.desk, true);
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

static void scr_format_size(const scroller_size_t size, ipc_buf_t *b, const bool last) {
	if (size.type == SCROLLER_SIZE_FIXED)
		ipc_buff(b, "%.0fpx%s", size.value, last ? "\n" : ",");
	else
		ipc_buff(b, "%.2f%s", size.value, last ? "\n" : ",");
}

// parses a comma separated preset list into an array
static bool scr_parse_presets(const char *value, scroller_size_t **out, int *out_count, char *err,
		const size_t errsz) {
	char *copy = strdup(value);
	if (copy == NULL) {
		setting_err(err, errsz, "Memory allocation failed\n");
		return false;
	}

	const size_t len = strlen(copy);
	if (len >= 2 && copy[0] == '[' && copy[len - 1] == ']') {
		memmove(copy, copy + 1, len - 2);
		copy[len - 2] = '\0';
	}

	int count = 1;
	for (char *p = copy; *p; p++) {
		if (*p == ',')
			count++;
	}

	char **tokens = malloc((size_t)count * sizeof(*tokens));
	scroller_size_t *presets = malloc((size_t)count * sizeof(*presets));
	if (tokens == NULL || presets == NULL) {
		free(tokens);
		free(presets);
		free(copy);
		setting_err(err, errsz, "Memory allocation failed\n");
		return false;
	}

	const int parsed = parse_split(copy, ",", tokens, count);
	for (int i = 0; i < parsed; i++) {
		if (!parse_scroller_size(tokens[i], &presets[i])) {
			setting_err(err, errsz, "invalid value \"%s\", expected a proportion in (0, 1] "
				"or a size like \"640px\"\n", tokens[i]);
			free(tokens);
			free(presets);
			free(copy);
			return false;
		}
	}
	free(tokens);
	free(copy);

	*out = presets;
	*out_count = parsed;
	return true;
}

static void scr_presets_release(scroller_size_t **list, int *count) {
	if (*list != NULL && *list != preset_widths_default && *list != preset_heights_default)
		free(*list);

	*list = NULL;
	*count = 0;
}

// preset_column_widths, preset_column_heights
static void scr_presets(ipc_args_t *a, const bool heights) {
	scroller_size_t **slot = heights ? &scroller_config.preset_window_heights :
		&scroller_config.preset_column_widths;
	int *count = heights ? &scroller_config.preset_window_heights_count :
		&scroller_config.preset_column_widths_count;
	const char *name = heights ? "preset_window_heights" : "preset_column_widths";

	if (ipc_peek(a) == NULL) {
		IPC_REPLY_GROWING(b);
		for (int i = 0; i < *count; i++)
			scr_format_size((*slot)[i], &b, i == *count - 1);
		ipc_buf_send(a, &b);
		return;
	}

	const char *value;
	if (!ipc_need(a, "comma separated values", &value))
		return;

	char err[256];
	scroller_size_t *list = NULL;
	int parsed = 0;
	if (!scr_parse_presets(value, &list, &parsed, err, sizeof(err))) {
		ipc_fail(a, "%s", err);
		return;
	}

	if (a->dry_run) {
		free(list);
	} else {
		scr_presets_release(slot, count);
		*slot = list;
		*count = parsed;
	}

	ipc_okf(a, "%s %s\n", name, a->dry_run ? "would be set" : "set");
}

static void scr_preset_widths(ipc_args_t *a) {
	scr_presets(a, false);
}

static void scr_preset_heights(ipc_args_t *a) {
	scr_presets(a, true);
}

static void scroller_apply_default_column_width(void) {
	output_t *m;
	wl_list_for_each(m, &mon_list, link) {
		desktop_t *d;
		wl_list_for_each(d, &m->desk_list, link)
			scroller_set_default_column_width(d, scroller_config.default_column_width);
	}
}

static void scr_default_column_width(ipc_args_t *a) {
	if (ipc_peek(a) == NULL) {
		char buf[64];
		ipc_buf_t b;
		ipc_buf_init(&b, buf, sizeof(buf));
		scr_format_size(scroller_config.default_column_width, &b, true);
		ipc_buf_send(a, &b);
		return;
	}

	const char *value;
	if (!ipc_need(a, "proportion or px size", &value))
		return;

	scroller_size_t size;
	if (!parse_scroller_size(value, &size)) {
		ipc_fail(a, "invalid value \"%s\", expected a proportion in (0, 1] or a size like \"640px\"\n",
			value);
		return;
	}

	if (!a->dry_run) {
		scroller_config.default_column_width = size;
		scroller_apply_default_column_width();
	}

	ipc_okf(a, "default_column_width %s\n", a->dry_run ? "would be set" : "set");
}

// restores the values the layout keeps outside its setting table
void scroller_reset_settings(void) {
	scroller_config.default_column_width = (scroller_size_t){
		SCROLLER_SIZE_PROPORTION,
		0.5
	};
	scroller_apply_default_column_width();

	scr_presets_release(&scroller_config.preset_column_widths,
		&scroller_config.preset_column_widths_count);
	scroller_config.preset_column_widths = (scroller_size_t *)preset_widths_default;
	scroller_config.preset_column_widths_count = (int)IPC_ARRAY_LEN(preset_widths_default);

	scr_presets_release(&scroller_config.preset_window_heights,
		&scroller_config.preset_window_heights_count);
	scroller_config.preset_window_heights = (scroller_size_t *)preset_heights_default;
	scroller_config.preset_window_heights_count = (int)IPC_ARRAY_LEN(preset_heights_default);

	transaction_commit_dirty();
}

static const str_pair_t center_mode_values[] = {
	{"never", SCROLLER_CENTER_NEVER},
	{"always", SCROLLER_CENTER_ALWAYS},
	{"on-overflow", SCROLLER_CENTER_ON_OVERFLOW},
	{"on_overflow", SCROLLER_CENTER_ON_OVERFLOW},
	IPC_ENUM_END,
};

const setting_t scroller_settings[SCROLLER_SETTINGS_COUNT] = {
	{
		.name = "always_center_single_column",
		.type = SETTING_BOOL,
		.ptr = &scroller_config.always_center_single_column,
	},
	{
		.name = "center_focused_column",
		.type = SETTING_ENUM,
		.ptr = &scroller_config.center_focused_column,
		.values = center_mode_values,
	},
	{
		.name = "left_strut",
		.type = SETTING_INT,
		.ptr = &scroller_config.strut_left,
		.min = 0,
		.max = INT_MAX,
		.fmt = "%d\n",
		.flags = SETTING_COMMIT,
	},
	{
		.name = "right_strut",
		.type = SETTING_INT,
		.ptr = &scroller_config.strut_right,
		.min = 0,
		.max = INT_MAX,
		.fmt = "%d\n",
		.flags = SETTING_COMMIT,
	},
	{
		.name = "top_strut",
		.type = SETTING_INT,
		.ptr = &scroller_config.strut_top,
		.min = 0,
		.max = INT_MAX,
		.fmt = "%d\n",
		.flags = SETTING_COMMIT,
	},
	{
		.name = "bottom_strut",
		.type = SETTING_INT,
		.ptr = &scroller_config.strut_bottom,
		.min = 0,
		.max = INT_MAX,
		.fmt = "%d\n",
		.flags = SETTING_COMMIT,
	},
	{
		.name = "pan_fingers",
		.type = SETTING_INT,
		.ptr = &scroller_config.pan_fingers,
		.min = 2,
		.max = 5,
		.fmt = "%d\n",
	},
	{
		.name = "snap_percent",
		.type = SETTING_INT,
		.ptr = &scroller_config.snap_percent,
		.min = 0,
		.max = 100,
		.fmt = "%d\n",
	},
};

const command_t scroller_commands[] = {
	{"proportion", "layout scroller proportion <0-1|px>", scr_proportion},
	{"resize", "layout scroller resize <delta>", scr_resize},
	{"stack", "layout scroller stack", scr_stack},
	{"unstack", "layout scroller unstack", scr_unstack},
	{"cycle_preset", "layout scroller cycle_preset [forward|backward]", scr_cycle_width},
	{"cycle_width", "layout scroller cycle_width [forward|backward]", scr_cycle_width},
	{"cycle_height", "layout scroller cycle_height [forward|backward]", scr_cycle_height},
	{"toggle_full_width", "layout scroller toggle_full_width", scr_toggle_full_width},
	{"expand_column", "layout scroller expand_column", scr_expand_column},
	{"set_height", "layout scroller set_height <0-1|px>", scr_set_height},
	{"reset_height", "layout scroller reset_height", scr_reset_height},
	{"center", "layout scroller center", scr_center},
	{"center_visible", "layout scroller center_visible", scr_center_visible},
	{"action", "layout scroller action <name>", scr_action_any},
	{"focus", "layout scroller focus <name>", scr_action_focus},
	{"move", "layout scroller move <name>", scr_action_move},
	{"swap", "layout scroller swap <name>", scr_action_swap},
	{"default_column_width", "layout scroller default_column_width [<0-1|px>]",
		scr_default_column_width},
	{"preset_column_widths", "layout scroller preset_column_widths [<values>]", scr_preset_widths},
	{"preset_window_heights", "layout scroller preset_window_heights [<values>]", scr_preset_heights},
	{NULL, NULL, NULL},
};

_Static_assert(IPC_ARRAY_LEN(scroller_settings) == SCROLLER_SETTINGS_COUNT,
	"invalid SCROLLER_SETTINGS_COUNT");
