#include "ipc/args.h"
#include "settings.h"
#include "settings_enum.h"

const str_pair_t automatic_scheme_values[] = {
	{"longest_side", SCHEME_LONGEST_SIDE},
	{"longest-side", SCHEME_LONGEST_SIDE},
	{"alternate", SCHEME_ALTERNATE},
	{"spiral", SCHEME_SPIRAL},
	IPC_ENUM_END,
};

const str_pair_t initial_polarity_values[] = {
	{"first_child", FIRST_CHILD},
	{"first-child", FIRST_CHILD},
	{"second_child", SECOND_CHILD},
	{"second-child", SECOND_CHILD},
	IPC_ENUM_END,
};

const str_pair_t focus_on_activate_values[] = {
	{"focus", FOCUS_ON_ACTIVATE_FOCUS},
	{"none", FOCUS_ON_ACTIVATE_NONE},
	{"smart", FOCUS_ON_ACTIVATE_SMART},
	{"urgent", FOCUS_ON_ACTIVATE_URGENT},
	IPC_ENUM_END,
};

const str_pair_t focus_follows_pointer_values[] = {
	{"no", FOLLOWS_NO},
	{"false", FOLLOWS_NO},
	{"yes", FOLLOWS_YES},
	{"true", FOLLOWS_YES},
	{"always", FOLLOWS_ALWAYS},
	IPC_ENUM_END,
};

const str_pair_t workspace_anim_direction_values[] = {
	{"vertical", WORKSPACE_ANIM_VERTICAL},
	{"horizontal", WORKSPACE_ANIM_HORIZONTAL},
	IPC_ENUM_END,
};

const str_pair_t keyboard_grouping_values[] = {
	{"none", KEYBOARD_GROUP_NONE},
	{"smart", KEYBOARD_GROUP_SMART},
	{"default", KEYBOARD_GROUP_DEFAULT},
	IPC_ENUM_END,
};

const str_pair_t decoration_mode_values[] = {
	{"none", DECORATION_NONE},
	{"tabs", DECORATION_TABS},
	{"always", DECORATION_ALWAYS},
	{"csd", DECORATION_CSD},
	IPC_ENUM_END,
};
