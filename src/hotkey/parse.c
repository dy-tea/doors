#include "config.h"
#include "input/keyboard.h"
#include "macros.h"
#include "parse.h"
#include "str.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

keybind_t keybinds[MAX_KEYBINDS];
size_t num_keybinds = 0;
keybind_t bell_bind = {0};
gesturebind_t gesture_bindings[MAX_GESTUREBINDS];
size_t num_gesturebinds = 0;
hotcornerbind_t hotcorner_bindings[MAX_HOTCORNERBINDS];
size_t num_hotcornerbinds = 0;
submap_t *active_submap = NULL;

#define MAX_SUBMAPS 32
static submap_t submaps[MAX_SUBMAPS];
static size_t num_submaps = 0;
static submap_t *current_parsing_submap = NULL;

static const str_pair_t mod_map[] = {
	{"super", WLR_MODIFIER_LOGO},
	{"mod4", WLR_MODIFIER_LOGO},
	{"alt", WLR_MODIFIER_ALT},
	{"mod1", WLR_MODIFIER_ALT},
	{"ctrl", WLR_MODIFIER_CTRL},
	{"control", WLR_MODIFIER_CTRL},
	{"shift", WLR_MODIFIER_SHIFT},
	{"caps", WLR_MODIFIER_CAPS},
	{"capslock", WLR_MODIFIER_CAPS},
	{"num", WLR_MODIFIER_MOD2},
	{"numlock", WLR_MODIFIER_MOD2},
	{"mod2", WLR_MODIFIER_MOD2},
	{"mod3", WLR_MODIFIER_MOD3},
	{"mod5", WLR_MODIFIER_MOD5},
	{NULL, 0},
};

uint32_t parse_modifiers(const char *mod_str) {
	uint32_t mods = 0;
	if (!mod_str || mod_str[0] == '\0')
		return 0;

	char *tmp = strdup(mod_str);
	if (!tmp) {
		wlr_log(WLR_ERROR, "Allocation failed");
		return 0;
	}

	char *out[32];
	int nmods = parse_split(tmp, "+", out, 32);
	for (int i = 0; i < nmods; i++) {
		const str_pair_t *e = str_table_find(mod_map, str_trim(out[i]));
		if (e)
			mods |= (uint32_t)e->value;
	}

	free(tmp);
	return mods;
}

xkb_keysym_t parse_keysym(const char *name) {
	if (!name || name[0] == '\0')
		return XKB_KEY_NoSymbol;

	return xkb_keysym_from_name(name, XKB_KEYSYM_CASE_INSENSITIVE);
}

uint32_t parse_keycode(const char *name) {
	if (!name || name[0] == '\0')
		return 0;

	static const char *mouse_buttons[] = {
		"mouse_left",
		"mouse_right",
		"mouse_middle",
		"mouse_back",
		"mouse_forward"
	};

	for (size_t i = 0; i < sizeof(mouse_buttons) / sizeof(mouse_buttons[0]); i++)
		if (streq(name, mouse_buttons[i]))
			return mouse_button_to_keycode(i);

	if (name[0] >= '1' && name[0] <= '9' && name[1] == '\0')
		return name[0] - '0' + 1;

	if (streq(name, "0"))
		return 11;

	return 0;
}

static const str_pair_t bind_flag_names[] = {
	{"locked", BINDF_LOCKED},
	{"release", BINDF_RELEASE},
	{"repeat", BINDF_REPEAT},
	{"non_consuming", BINDF_NON_CONSUMING},
	{"ignore_mods", BINDF_IGNORE_MODS},
	{"universal", BINDF_SUBMAP_UNIVERSAL},
	{"submap_universal", BINDF_SUBMAP_UNIVERSAL},
	{"catch_all", BINDF_CATCH_ALL},
	{"catchall", BINDF_CATCH_ALL},
	{NULL, 0},
};

bool parse_bind_flags(const char *str, uint32_t *out_flags) {
	if (out_flags)
		*out_flags = 0;
	if (!str || str[0] == '\0')
		return true;

	char buf[MAXLEN];
	str_copy(buf, sizeof(buf), str);

	bool ok = true;
	uint32_t flags = 0;
	char *tokens[32];
	int nflags = parse_split(buf, ", \t", tokens, 32);
	for (int i = 0; i < nflags; i++) {
		const str_pair_t *e = str_table_find(bind_flag_names, tokens[i]);
		if (e)
			flags |= (uint32_t)e->value;
		else {
			wlr_log(WLR_ERROR, "Unknown bind flag '%s'", tokens[i]);
			ok = false;
		}
	}

	if (ok && out_flags)
		*out_flags = flags;
	return ok;
}

char *bind_flags_text(const uint32_t flags, char *buf, const size_t size) {
	buf[0] = '\0';
	if (flags == 0)
		return buf;

	for (const str_pair_t *e = bind_flag_names; e->name; e++) {
		if (!(flags & (uint32_t)e->value))
			continue;
		str_append_sep(buf, size, ",", e->name);
	}
	return buf;
}

const char *parse_hotkey_spec(const char *spec, uint32_t *modifiers, xkb_keysym_t *keysym,
		uint32_t *keycode, bool *use_keycode, uint32_t *flags) {
	if (modifiers)
		*modifiers = 0;
	if (keysym)
		*keysym = XKB_KEY_NoSymbol;
	if (keycode)
		*keycode = 0;
	if (use_keycode)
		*use_keycode = false;
	if (flags)
		*flags = 0;

	if (!spec)
		return "No hotkey given";

	char buf[MAXLEN * 2];
	str_copy(buf, sizeof(buf), spec);

	char *bracket = strrchr(buf, '[');
	if (bracket) {
		char *end = strchr(bracket, ']');
		if (end) {
			uint32_t parsed = 0;
			*end = '\0';
			if (parse_bind_flags(bracket + 1, &parsed)) {
				*bracket = '\0';
				if (flags)
					*flags = parsed;
			}
		}
	}

	uint32_t mods = 0;
	xkb_keysym_t sym = XKB_KEY_NoSymbol;
	uint32_t kc = 0;
	bool use_kc = false;

	bool is_catchall = false;
	char *left = strrchr(buf, '+');
	if (left) {
		*left = '\0';
		mods = parse_modifiers(buf);
		char *key_part = str_trim(left + 1);
		if (streqi(key_part, "catchall"))
			is_catchall = true;
		else {
			sym = parse_keysym(key_part);
			kc = parse_keycode(key_part);
			if (kc > 0)
				use_kc = true;
		}
	} else {
		const char *key_part = str_trim(buf);
		if (streqi(key_part, "catchall"))
			is_catchall = true;
		else {
			sym = parse_keysym(key_part);
			kc = parse_keycode(key_part);
			if (kc > 0)
				use_kc = true;
		}
	}

	if (is_catchall) {
		if (flags)
			*flags |= BINDF_CATCH_ALL;
		if (mods != 0)
			wlr_log(WLR_ERROR, "Mods on a catchall bind are ignored");
		mods = 0;
		sym = XKB_KEY_NoSymbol;
		kc = 0;
		use_kc = false;
	} else if (sym == XKB_KEY_NoSymbol && kc == 0) {
		return "Unknown keysym";
	}

	if (modifiers)
		*modifiers = mods;
	if (keysym)
		*keysym = sym;
	if (keycode)
		*keycode = kc;
	if (use_keycode)
		*use_keycode = use_kc;

	return NULL;
}

bind_kind_t parse_bind_kind(const char *cmd, char *submap_name) {
	if (submap_name)
		submap_name[0] = '\0';

	if (streq(cmd, "@exit"))
		return BIND_KIND_EXIT_SUBMAP;

	if (cmd[0] == '@') {
		if (submap_name)
			str_copy(submap_name, MAXLEN, cmd + 1);
		return BIND_KIND_ENTER_SUBMAP;
	}

	return BIND_KIND_COMMAND;
}

// expansions
#define MAX_EXPANSIONS 64
typedef struct {
	char strings[MAX_EXPANSIONS][MAXLEN];
	size_t count;
} expansion_result_t;

static void expand_braces_recursive(const char *input, char *prefix, size_t prefix_len,
	expansion_result_t *result);

static void expand_braces(const char *input, expansion_result_t *result) {
	result->count = 0;
	char prefix[MAXLEN] = {0};
	expand_braces_recursive(input, prefix, 0, result);
}

static void expand_braces_recursive(const char *input, char *prefix, size_t prefix_len,
		expansion_result_t *result) {
	const char *brace_start = strchr(input, '{');
	if (!brace_start) {
		if (result->count < MAX_EXPANSIONS) {
			snprintf(result->strings[result->count], MAXLEN, "%s%s", prefix, input);
			result->count++;
		} else {
			wlr_log(WLR_ERROR, "Brace expansion exceeds %d results, dropping: %s%s", MAX_EXPANSIONS, prefix,
				input);
		}
		return;
	}

	// copy prefix
	size_t pre_brace_len = brace_start - input;
	if (prefix_len + pre_brace_len >= MAXLEN) {
		wlr_log(WLR_ERROR, "Brace expansion prefix exceeds %d bytes, truncating: %s%s", MAXLEN, prefix,
			input);
		pre_brace_len = MAXLEN - prefix_len - 1;
	}
	char new_prefix[MAXLEN];
	memcpy(new_prefix, prefix, prefix_len);
	memcpy(new_prefix + prefix_len, input, pre_brace_len);
	new_prefix[prefix_len + pre_brace_len] = '\0';

	// find closing brace
	const char *brace_end = brace_start + 1;
	int depth = 1;
	while (*brace_end && depth > 0) {
		if (*brace_end == '{')
			depth++;
		else if (*brace_end == '}')
			depth--;
		brace_end++;
	}

	if (depth != 0) {
		if (result->count < MAX_EXPANSIONS) {
			str_append(new_prefix, MAXLEN, brace_start);
			str_copy(result->strings[result->count], MAXLEN, new_prefix);
			result->count++;
		}
		return;
	}

	// get content
	size_t content_len = (brace_end - 1) - (brace_start + 1);
	if (content_len >= MAXLEN) {
		wlr_log(WLR_ERROR, "Brace expansion group exceeds %d bytes, truncating", MAXLEN);
		content_len = MAXLEN - 1;
	}
	char content[MAXLEN];
	memcpy(content, brace_start + 1, content_len);
	content[content_len] = '\0';

	const char *rest = brace_end;

	int choice_depth = 0;
	size_t choice_start = 0;

	bool is_range = false;
	size_t range_len = content_len;
	if (range_len >= 3) {
		char first = content[0];
		char last = content[range_len - 1];
		bool is_dash = false;
		for (size_t di = 1; di < range_len - 1; di++) {
			if (content[di] == '-') {
				is_dash = true;
				break;
			}
		}
		if (is_dash && ((first >= '0' && first <= '9' && last >= '0' && last <= '9') || (first >= 'a' &&
			first <= 'z' && last >= 'a' && last <= 'z') || (first >= 'A' && first <= 'Z' && last >= 'A' &&
			last <= 'Z')))
			is_range = true;
	}

	if (is_range) {
		const char first = content[0];
		const char last = content[content_len - 1];
		for (char c = first; c <= last; c++) {
			char choice[2] = {
				c,
				'\0'
			};
			char full_input[MAXLEN * 2];
			snprintf(full_input, sizeof(full_input), "%s%s", choice, rest);
			expand_braces_recursive(full_input, new_prefix, strlen(new_prefix), result);
		}
	} else {
		// comma-separated
		for (size_t i = 0; i <= content_len; i++) {
			if (content[i] == '{') {
				choice_depth++;
			} else if (content[i] == '}') {
				choice_depth--;
			} else if (content[i] == ',' && choice_depth == 0) {
				char choice[MAXLEN];
				const size_t choice_len = i - choice_start;
				if (choice_len < MAXLEN) {
					memcpy(choice, content + choice_start, choice_len);
					choice[choice_len] = '\0';
				} else {
					memcpy(choice, content + choice_start, MAXLEN - 1);
					choice[MAXLEN - 1] = '\0';
				}

				char full_input[MAXLEN * 2];
				snprintf(full_input, sizeof(full_input), "%s%s", choice, rest);

				expand_braces_recursive(full_input, new_prefix, strlen(new_prefix), result);

				choice_start = i + 1;
			}
		}

		// last choice
		if (choice_start <= content_len) {
			char choice[MAXLEN];
			const size_t choice_len = content_len - choice_start;
			if (choice_len < MAXLEN) {
				memcpy(choice, content + choice_start, choice_len);
				choice[choice_len] = '\0';
			} else {
				memcpy(choice, content + choice_start, MAXLEN - 1);
				choice[MAXLEN - 1] = '\0';
			}

			char full_input[MAXLEN * 2];
			snprintf(full_input, sizeof(full_input), "%s%s", choice, rest);

			expand_braces_recursive(full_input, new_prefix, strlen(new_prefix), result);
		}
	}
}

static char *expand_sequence(const char *input, char *output, size_t out_size) {
	expansion_result_t result;
	expand_braces(input, &result);
	str_copy(output, out_size, result.count > 0 ? result.strings[0] : input);
	return output;
}

static bool bind_flags_valid(const keybind_t *kb) {
	if ((kb->flags & BINDF_REPEAT) && (kb->flags & BINDF_RELEASE)) {
		wlr_log(WLR_ERROR, "A bind cannot be both repeat and release");
		return false;
	}

	if (bind_keycode_is_mouse(kb->keycode)) {
		if (kb->flags & BINDF_REPEAT) {
			wlr_log(WLR_ERROR, "Mouse binds cannot repeat");
			return false;
		}
		if (kb->flags & BINDF_NON_CONSUMING) {
			wlr_log(WLR_ERROR, "Mouse binds cannot be non_consuming");
			return false;
		}
	}

	return true;
}

void add_keybind(const uint32_t modifiers, const xkb_keysym_t keysym, const uint32_t keycode,
		const bool use_keycode, const uint32_t flags, const bind_kind_t kind, const char *submap_name,
		const char *cmd) {
	size_t *num_ptr;
	keybind_t *kb_array;

	if (current_parsing_submap) {
		if (current_parsing_submap->num_keybinds >= MAX_KEYBINDS) {
			wlr_log(WLR_ERROR, "Maximum number of keybinds reached for submap %s",
				current_parsing_submap->name);
			return;
		}
		num_ptr = &current_parsing_submap->num_keybinds;
		kb_array = current_parsing_submap->keybinds;
	} else {
		if (num_keybinds >= MAX_KEYBINDS) {
			wlr_log(WLR_ERROR, "Maximum number of keybinds reached");
			return;
		}
		num_ptr = &num_keybinds;
		kb_array = keybinds;
	}

	keybind_t preview = {
		.modifiers = modifiers,
		.keysym = keysym,
		.keycode = keycode,
		.use_keycode = use_keycode,
		.kind = kind,
		.flags = flags,
	};
	if (!bind_flags_valid(&preview))
		return;

	keybind_t *kb = &kb_array[(*num_ptr)++];
	kb->modifiers = modifiers;
	kb->keysym = keysym;
	kb->keycode = keycode;
	kb->use_keycode = use_keycode;
	kb->kind = kind;
	kb->flags = flags;
	if (submap_name)
		str_copy(kb->submap_name, sizeof(kb->submap_name), submap_name);
	else
		kb->submap_name[0] = '\0';
	if (cmd)
		str_copy(kb->cmd, sizeof(kb->cmd), cmd);
	else
		kb->cmd[0] = '\0';

	char flag_text[128];
	bind_flags_text(flags, flag_text, sizeof(flag_text));
	wlr_log(WLR_DEBUG, "Added keybind: mod=%u keysym=%u keycode=%u kind=%d flags=%s cmd='%s' submap=%s",
		modifiers, keysym, keycode, kind, flag_text, kb->cmd, submap_name ? submap_name : "global");
}

static void add_gesturebind(const enum gesture_type type, const uint8_t fingers,
		const uint32_t directions, const char *input, const char *cmd) {
	if (num_gesturebinds >= MAX_GESTUREBINDS) {
		wlr_log(WLR_ERROR, "Maximum number of gesture binds reached");
		return;
	}

	gesturebind_t *gb = &gesture_bindings[num_gesturebinds++];
	gb->type = type;
	gb->fingers = fingers;
	gb->directions = directions;
	gb->input = input ? strdup(input) : NULL;
	if (cmd)
		str_copy(gb->cmd, sizeof(gb->cmd), cmd);
	else
		gb->cmd[0] = '\0';

	wlr_log(WLR_DEBUG, "Added gesturebind: type=%d fingers=%d dirs=%u cmd='%s'", type, fingers,
		directions, gb->cmd);
}

static void parse_gesture_hotkey_line(const char *gesture_str, const char *command_str) {
	char gesture_buf[MAXLEN];
	char command_buf[MAXLEN];

	str_copy(gesture_buf, sizeof(gesture_buf), gesture_str);
	str_copy(command_buf, sizeof(command_buf), command_str);

	char expanded_cmd[MAXLEN];
	expand_sequence(command_buf, expanded_cmd, sizeof(expanded_cmd));

	gesture_t gest;
	char *err = gesture_parse(gesture_buf, &gest);
	if (err) {
		wlr_log(WLR_ERROR, "Failed to parse gesture '%s': %s", gesture_buf, err);
		return;
	}

	if (parse_bind_kind(expanded_cmd, NULL) != BIND_KIND_COMMAND) {
		wlr_log(WLR_ERROR, "Gesture binds cannot enter or exit a submap");
		return;
	}

	add_gesturebind(gest.type, gest.fingers, gest.directions, NULL, expanded_cmd);
}

static void add_hotcornerbind(const int corner_x, const int corner_y, const char *cmd) {
	if (num_hotcornerbinds >= MAX_HOTCORNERBINDS) {
		wlr_log(WLR_ERROR, "Maximum number of hotcorner binds reached");
		return;
	}

	hotcornerbind_t *hc = &hotcorner_bindings[num_hotcornerbinds++];
	hc->corner_x = corner_x;
	hc->corner_y = corner_y;
	if (corner_x == -1 && corner_y == -1)
		hc->corner = HOTCORNER_TOPLEFT;
	else if (corner_x == 1 && corner_y == -1)
		hc->corner = HOTCORNER_TOPRIGHT;
	else if (corner_x == -1 && corner_y == 1)
		hc->corner = HOTCORNER_BOTTOMLEFT;
	else if (corner_x == 1 && corner_y == 1)
		hc->corner = HOTCORNER_BOTTOMRIGHT;
	else
		hc->corner = HOTCORNER_NONE;
	if (cmd)
		str_copy(hc->cmd, sizeof(hc->cmd), cmd);
	else
		hc->cmd[0] = '\0';

	wlr_log(WLR_DEBUG, "Added hotcornerbind: corner=%d (%d,%d) cmd='%s'", hc->corner, corner_x,
		corner_y, hc->cmd);
}

static void parse_hotcorner_hotkey_line(const char *hotcorner_str, const char *command_str) {
	int corner_x = 0, corner_y = 0;

	if (streqi(hotcorner_str, "topleft")) {
		corner_x = -1;
		corner_y = -1;
	} else if (streqi(hotcorner_str, "topright")) {
		corner_x = 1;
		corner_y = -1;
	} else if (streqi(hotcorner_str, "bottomleft")) {
		corner_x = -1;
		corner_y = 1;
	} else if (streqi(hotcorner_str, "bottomright")) {
		corner_x = 1;
		corner_y = 1;
	} else {
		wlr_log(WLR_ERROR, "Unknown hotcorner '%s'", hotcorner_str);
		return;
	}

	if (parse_bind_kind(command_str, NULL) != BIND_KIND_COMMAND) {
		wlr_log(WLR_ERROR, "Hotcorner binds cannot enter or exit a submap");
		return;
	}

	add_hotcornerbind(corner_x, corner_y, command_str);
}

static void parse_hotkey_line(const char *hotkey_str, const char *command_str) {
	const char *gesture_spec = str_skip_prefix(hotkey_str, "gesture ");
	if (gesture_spec) {
		parse_gesture_hotkey_line(gesture_spec, command_str);
		return;
	}

	const char *hotcorner_spec = str_skip_prefix(hotkey_str, "hotcorner ");
	if (hotcorner_spec) {
		parse_hotcorner_hotkey_line(hotcorner_spec, command_str);
		return;
	}

	if (streq(hotkey_str, "bell")) {
		char submap_name[MAXLEN];
		bell_bind = (keybind_t){
			.kind = parse_bind_kind(command_str, submap_name),
		};
		str_copy(bell_bind.cmd, sizeof(bell_bind.cmd), command_str);
		if (submap_name[0] != '\0')
			str_copy(bell_bind.submap_name, sizeof(bell_bind.submap_name), submap_name);
		wlr_log(WLR_DEBUG, "Parsed bell bind: kind=%d cmd='%s'", bell_bind.kind, command_str);
		return;
	}

	char hotkey_buf[MAXLEN];
	char command_buf[MAXLEN];

	str_copy(hotkey_buf, sizeof(hotkey_buf), hotkey_str);
	str_copy(command_buf, sizeof(command_buf), command_str);

	// expand braces
	expansion_result_t hk_expansions;
	expansion_result_t cmd_expansions;
	expand_braces(hotkey_buf, &hk_expansions);
	expand_braces(command_buf, &cmd_expansions);

	wlr_log(WLR_DEBUG, "Hotkey=[%s] cmd=[%s] hk_expans=%zu cmd_expans=%zu", hotkey_buf, command_buf,
		hk_expansions.count, cmd_expansions.count);

	size_t num_pairs = 0;

	if (hk_expansions.count == 0 && cmd_expansions.count == 0) {
		num_pairs = 1;
	} else if (hk_expansions.count > 0 && cmd_expansions.count > 0) {
		num_pairs = (hk_expansions.count > cmd_expansions.count) ? hk_expansions.count :
			cmd_expansions.count;
	} else if (hk_expansions.count > 0) {
		num_pairs = hk_expansions.count;
	} else {
		num_pairs = cmd_expansions.count;
	}

	for (size_t i = 0; i < num_pairs; i++) {
		const char *single_hotkey = (hk_expansions.count > 0) ? hk_expansions.strings[i %
			hk_expansions.count] : hotkey_buf;
		const char *single_cmd = (cmd_expansions.count > 0) ? cmd_expansions.strings[i %
			cmd_expansions.count] : command_buf;

		wlr_log(WLR_DEBUG, "  Pair %zu: hotkey=[%s] cmd=[%s]", i, single_hotkey, single_cmd);

		uint32_t modifiers = 0;
		xkb_keysym_t keysym = XKB_KEY_NoSymbol;
		uint32_t keycode = 0;
		bool use_keycode = false;
		uint32_t flags = 0;

		const char *error = parse_hotkey_spec(single_hotkey, &modifiers, &keysym, &keycode, &use_keycode,
			&flags);
		if (error) {
			wlr_log(WLR_ERROR, "Invalid hotkey '%s': %s", single_hotkey, error);
			continue;
		}

		if (flags & BINDF_CATCH_ALL && current_parsing_submap == NULL) {
			wlr_log(WLR_ERROR, "Catch-all binds are only allowed inside a submap");
			continue;
		}

		char submap_name[MAXLEN];
		const bind_kind_t kind = parse_bind_kind(single_cmd, submap_name);
		wlr_log(WLR_DEBUG, "Parsed kind: %d for cmd: '%s' submap: '%s'", kind, single_cmd, submap_name);
		add_keybind(modifiers, keysym, keycode, use_keycode, flags, kind,
			submap_name[0] ? submap_name : NULL, single_cmd);
	}
}

static submap_t *find_or_create_submap(const char *name) {
	for (size_t i = 0; i < num_submaps; i++)
		if (streq(submaps[i].name, name))
			return &submaps[i];

	if (num_submaps >= MAX_SUBMAPS) {
		wlr_log(WLR_ERROR, "Maximum number of submaps reached");
		return NULL;
	}

	submap_t *sm = &submaps[num_submaps++];
	str_copy(sm->name, sizeof(sm->name), name);
	sm->num_keybinds = 0;
	sm->parent = NULL;
	return sm;
}

submap_t *find_submap_by_name(const char *name) {
	if (!name || name[0] == '\0')
		return NULL;

	for (size_t i = 0; i < num_submaps; i++)
		if (streq(submaps[i].name, name))
			return &submaps[i];

	return NULL;
}

struct pending_bind {
	char hotkey[MAXLEN * 2];
	char command[MAXLEN * 2];
	size_t indent;
};

static void flush_pending_bind(struct pending_bind *p) {
	if (p->hotkey[0] == '\0')
		return;
	char hotkey[MAXLEN * 2], command[MAXLEN * 2];
	snprintf(hotkey, sizeof(hotkey), "%s", p->hotkey);
	snprintf(command, sizeof(command), "%s", p->command);
	parse_hotkey_line(hotkey, command);
	p->hotkey[0] = '\0';
	p->command[0] = '\0';
	p->indent = 0;
}

void load_hotkeys(const char *path) {
	wlr_log(WLR_DEBUG, "Loading hotkeys from path: %s", path);
	FILE *f = fopen(path, "r");
	if (!f) {
		wlr_log(WLR_INFO, "No hotkey config found at %s", path);
		return;
	}

	bind_state_reset();
	num_keybinds = 0;
	num_submaps = 0;
	num_hotcornerbinds = 0;
	num_gesturebinds = 0;
	active_submap = NULL;
	current_parsing_submap = NULL;
	bell_bind = (keybind_t){0};

	char line[MAXLEN * 2];
	unsigned long offset = 0;
	struct pending_bind pending = {0};

	while (fgets(line, sizeof(line), f)) {
		wlr_log(WLR_DEBUG, "Config line: [%s]", line);
		str_chomp(line);

		size_t indent = 0;
		while (line[indent] == ' ' || line[indent] == '\t')
			indent++;

		char *ptr = line + indent;
		char *content_start = ptr;
		while (*content_start == ' ' || *content_start == '\t')
			content_start++;
		const char first_char = *content_start;

		if (line[0] == '#' || line[0] == '\0')
			continue;

		if (first_char == '@') {
			if (indent == 0) {
				flush_pending_bind(&pending);
				pending.indent = 0;
				const char *submap_name = content_start + 1;
				current_parsing_submap = find_or_create_submap(submap_name);
			} else {
				str_copy(pending.command, sizeof(pending.command), content_start);
				offset = strlen(pending.command);
			}
			continue;
		}

		if (current_parsing_submap != NULL && indent == 0) {
			flush_pending_bind(&pending);
			current_parsing_submap = NULL;
		}

		if (indent == 0 && strstr(ptr, "->")) {
			char *arrow = strstr(ptr, "->");
			if (arrow) {
				flush_pending_bind(&pending);
				size_t key_part_len = arrow - ptr;
				while (key_part_len > 0 && ptr[key_part_len - 1] == ' ')
					key_part_len--;
				if (key_part_len < sizeof(pending.hotkey)) {
					strncpy(pending.hotkey, ptr, key_part_len);
					pending.hotkey[key_part_len] = '\0';
				}
				char *cmd_part = arrow + 2;
				while (*cmd_part == ' ')
					cmd_part++;
				str_copy(pending.command, sizeof(pending.command), cmd_part);
				pending.indent = indent;
				offset = strlen(pending.command);
				continue;
			}
		}

		if (isgraph((unsigned char)first_char) || (first_char != '\0' &&
				!isspace((unsigned char)first_char))) {
			if (pending.hotkey[0] != '\0') {
				if (indent == 0 || indent <= pending.indent) {
					flush_pending_bind(&pending);
				} else {
					if (pending.command[0] != '\0') {
						const size_t cur_len = strlen(pending.command);
						const size_t rem = sizeof(pending.command) - cur_len - 1;
						if (rem > 0)
							str_append_sep(pending.command, sizeof(pending.command), " ", content_start);
					} else {
						str_copy(pending.command, sizeof(pending.command), content_start);
					}
					offset = strlen(pending.command);
					continue;
				}
			}
			str_copy(pending.hotkey, sizeof(pending.hotkey), content_start);
			pending.command[0] = '\0';
			pending.indent = indent;
			offset = 0;
		} else if (pending.hotkey[0] != '\0') {
			if (pending.command[0] != '\0' && offset > 0 && pending.command[offset - 1] != ' ') {
				pending.command[offset++] = ' ';
			}
			bool last_was_space = false;
			for (size_t i = 0; ptr[i] && offset < (int)sizeof(pending.command) - 1; i++) {
				if (isspace((unsigned char)ptr[i])) {
					if (!last_was_space && offset > 0 && pending.command[offset - 1] != ' ') {
						pending.command[offset++] = ' ';
						last_was_space = true;
					}
				} else {
					pending.command[offset++] = ptr[i];
					last_was_space = false;
				}
			}
			pending.command[offset] = '\0';
		}
	}

	flush_pending_bind(&pending);

	current_parsing_submap = NULL;

	fclose(f);
	wlr_log(WLR_INFO, "Loaded %zu keybinds, %zu gesturebinds, %zu hotcornerbinds "
		"and %zu submaps from %s", num_keybinds, num_gesturebinds, num_hotcornerbinds, num_submaps, path);
}
