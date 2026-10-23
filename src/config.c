#include "config.h"
#include "input/keyboard.h"
#include "ipc/ipc.h"
#include "launcher.h"
#include "layout/layout.h"
#include "layout/master_stack.h"
#include "layout/scroller.h"
#include "once.h"
#include "output/output.h"
#include "protocol/workspace.h"
#include "server.h"
#include "str.h"
#include "tree.h"
#include "types.h"
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_keyboard.h>
#include <xkbcommon/xkbcommon.h>

#define DOORS_CONFIG_DIR "/.config/doors"
#define GLOBAL_CONFIG_DIR "/etc/doors"
#define CONFIG_PATH_MAX (PATH_MAX + sizeof(DOORSHKRC_NAME) + 2)
#define DOORSRC_NAME "doorsrc"
#define DOORSHKRC_NAME "doorshkrc"

static const char *custom_config_dir = NULL;

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

static keyboard_grouping_t keyboard_grouping = KEYBOARD_GROUP_DEFAULT;

static int hotkey_watch_fd = -1;
static char hotkey_config_path[CONFIG_PATH_MAX];
static struct wl_event_loop *hotkey_event_loop = NULL;
static struct wl_event_source *hotkey_event_source = NULL;
static struct wl_event_source *doorsrc_debounce_timer = NULL;
static void setup_inotify_watch(const char *config_path);
static void add_hotkey_listener_to_event_loop(void);

// whether an inotify event name is the file at `path`
static bool file_event_matches(const char *path, const char *name) {
	if (path[0] == '\0' || name == NULL || name[0] == '\0')
		return false;

	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;

	return streq(base, name);
}

static const char *get_config_home(void) {
	if (custom_config_dir)
		return custom_config_dir;

	static char buf[PATH_MAX];
	const char *xdg = getenv("XDG_CONFIG_HOME");
	if (xdg && xdg[0] != '\0') {
		snprintf(buf, sizeof(buf), "%s/doors", xdg);
		return buf;
	}
	snprintf(buf, sizeof(buf), "%s%s", getenv("HOME") ? getenv("HOME") : "/root", DOORS_CONFIG_DIR);
	return buf;
}

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
	int nmods = str_split(tmp, "+", out, 32);
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
	int nflags = str_split(buf, ", \t", tokens, 32);
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

char *bind_flags_text(uint32_t flags, char *buf, size_t size) {
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
		char *key_part = str_trim(buf);
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

typedef struct {
	const char *cmd;
	const char *subcmd;
	const char *subsubcmd;
	bind_action_t action;
} action_entry_t;

#define ENTRY(c, s, ss, a) { c, s, ss, a }
#define ENTRY2(c, s, a)    { c, s, NULL, a }
#define ENTRY1(c, a)       { c, NULL, NULL, a }

static const action_entry_t action_table[] = {
	ENTRY2("focus", "west", BIND_FOCUS_WEST),
	ENTRY2("focus", "south", BIND_FOCUS_SOUTH),
	ENTRY2("focus", "north", BIND_FOCUS_NORTH),
	ENTRY2("focus", "east", BIND_FOCUS_EAST),
	ENTRY2("swap", "west", BIND_SWAP_WEST),
	ENTRY2("swap", "south", BIND_SWAP_SOUTH),
	ENTRY2("swap", "north", BIND_SWAP_NORTH),
	ENTRY2("swap", "east", BIND_SWAP_EAST),
	ENTRY2("presel", "west", BIND_PRESEL_WEST),
	ENTRY2("presel", "south", BIND_PRESEL_SOUTH),
	ENTRY2("presel", "north", BIND_PRESEL_NORTH),
	ENTRY2("presel", "east", BIND_PRESEL_EAST),
	ENTRY2("presel", "cancel", BIND_PRESEL_CANCEL),
	ENTRY2("resize", "left", BIND_RESIZE_LEFT),
	ENTRY2("resize", "right", BIND_RESIZE_RIGHT),
	ENTRY2("resize", "up", BIND_RESIZE_UP),
	ENTRY2("resize", "down", BIND_RESIZE_DOWN),
	ENTRY2("toggle", "floating", BIND_TOGGLE_FLOATING),
	ENTRY2("toggle", "fullscreen", BIND_TOGGLE_FULLSCREEN),
	ENTRY2("toggle", "maximize", BIND_TOGGLE_MAXIMIZE),
	ENTRY2("toggle", "minimize", BIND_TOGGLE_MINIMIZE),
	ENTRY2("toggle", "restore_minimized", BIND_RESTORE_MINIMIZED),
	ENTRY2("toggle", "pseudo_tiled", BIND_TOGGLE_PSEUDO_TILED),
	ENTRY2("toggle", "monocle", BIND_TOGGLE_MONOCLE),
	ENTRY2("toggle", "floating_layout", BIND_TOGGLE_FLOATING_LAYOUT),
	ENTRY2("node", "-c", BIND_NODE_CLOSE),
	ENTRY2("node", "--close", BIND_NODE_CLOSE),
	ENTRY2("node", "-f", BIND_NODE_FOCUS),
	ENTRY2("node", "--focus", BIND_NODE_FOCUS),
	ENTRY2("node", "interactive_move", BIND_INTERACTIVE_MOVE),
	ENTRY2("node", "interactive_resize", BIND_INTERACTIVE_RESIZE),
	ENTRY2("node", "tiling_drag", BIND_TILING_DRAG),
	ENTRY("node", "-t", "tiled", BIND_NODE_STATE_TILED),
	ENTRY("node", "-t", "floating", BIND_NODE_STATE_FLOATING),
	ENTRY("node", "-t", "fullscreen", BIND_NODE_STATE_FULLSCREEN),
	ENTRY("node", "-t", "maximized", BIND_NODE_STATE_MAXIMIZED),
	ENTRY("node", "-t", "minimized", BIND_NODE_STATE_MINIMIZED),
	ENTRY("node", "-t", "pseudo_tiled", BIND_TOGGLE_PSEUDO_TILED),
	ENTRY("node", "--state", "tiled", BIND_NODE_STATE_TILED),
	ENTRY("node", "--state", "floating", BIND_NODE_STATE_FLOATING),
	ENTRY("node", "--state", "fullscreen", BIND_NODE_STATE_FULLSCREEN),
	ENTRY("node", "--state", "maximized", BIND_NODE_STATE_MAXIMIZED),
	ENTRY("node", "--state", "minimized", BIND_NODE_STATE_MINIMIZED),
	ENTRY("node", "--state", "pseudo_tiled", BIND_TOGGLE_PSEUDO_TILED),
	ENTRY("desktop", "-l", "tiled", BIND_DESKTOP_LAYOUT_TILED),
	ENTRY("desktop", "-l", "monocle", BIND_DESKTOP_LAYOUT_MONOCLE),
	ENTRY("desktop", "-l", "master_stack", BIND_DESKTOP_LAYOUT_MASTER_STACK),
	ENTRY("desktop", "-l", "floating", BIND_DESKTOP_LAYOUT_FLOATING),
	ENTRY("desktop", "--layout", "tiled", BIND_DESKTOP_LAYOUT_TILED),
	ENTRY("desktop", "--layout", "monocle", BIND_DESKTOP_LAYOUT_MONOCLE),
	ENTRY("desktop", "--layout", "master_stack", BIND_DESKTOP_LAYOUT_MASTER_STACK),
	ENTRY("desktop", "--layout", "floating", BIND_DESKTOP_LAYOUT_FLOATING),
	ENTRY2("desktop", "last", BIND_DESKTOP_LAST),
	ENTRY1("quit", BIND_QUIT),
};


#undef ENTRY
#undef ENTRY2
#undef ENTRY1

static bind_action_t lookup_action(const char *args[3], int argc) {
	for (size_t i = 0; i < sizeof(action_table) / sizeof(action_table[0]); i++) {
		const action_entry_t *e = &action_table[i];
		if (!streq(args[0], e->cmd))
			continue;
		if (e->subcmd == NULL)
			return e->action;
		if (argc < 2 || !streq(args[1], e->subcmd))
			continue;
		if (e->subsubcmd == NULL)
			return e->action;
		if (argc >= 3 && streq(args[2], e->subsubcmd))
			return e->action;
	}
	return BIND_NONE;
}

bind_action_t parse_action(const char *cmd, int *desktop_index, char *submap_name) {
	*desktop_index = 0;
	if (submap_name)
		submap_name[0] = '\0';

	if (!cmd || cmd[0] == '\0')
		return BIND_NONE;

	if (cmd[0] == '@') {
		if (streq(cmd, "@exit"))
			return BIND_EXIT_SUBMAP;
		if (submap_name)
			str_copy(submap_name, MAXLEN, cmd + 1);
		return BIND_ENTER_SUBMAP;
	}

	const char *rest = str_skip_prefix(cmd, "doorsctl ");
	if (!rest)
		return BIND_EXTERNAL;

	char buf[MAXLEN];
	str_copy(buf, sizeof(buf), rest);

	char *args[16];
	int argc = str_split(buf, " \t", args, 16);

	if (argc == 0)
		return BIND_NONE;

	// dispatch table lookup first
	bind_action_t action = lookup_action((const char **)args, argc);
	if (action != BIND_NONE)
		return action;

	// special cases that need extra logic
	if (streq(args[0], "node") && argc >= 2) {
		if (streq(args[1], "-d") || streq(args[1], "--to-desktop")) {
			if (argc >= 3) {
				int d = atoi(args[2]);
				if (d >= 1 && d <= 10) {
					*desktop_index = d;
					return BIND_SEND_TO_DESKTOP_1;
				}
			}
			return BIND_NODE_TO_DESKTOP;
		}
	}

	if (streq(args[0], "desktop") && argc >= 2) {
		if (streq(args[1], "-f") || streq(args[1], "--focus"))
			return (argc >= 3 && streq(args[2], "last")) ? BIND_DESKTOP_LAST : BIND_DESKTOP_FOCUS;

		int d = atoi(args[1]);
		if (d >= 1 && d <= 10) {
			*desktop_index = d;
			return BIND_DESKTOP_1;
		}
	}

	return BIND_EXTERNAL;
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
		char first = content[0];
		char last = content[content_len - 1];
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
				size_t choice_len = i - choice_start;
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
			size_t choice_len = content_len - choice_start;
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

static bool bind_flags_valid(const keybind_t *kb, bind_action_t action) {
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

	if ((kb->flags & BINDF_RELEASE) && (action == BIND_INTERACTIVE_MOVE ||
			action == BIND_INTERACTIVE_RESIZE || action == BIND_TILING_DRAG)) {
		wlr_log(WLR_ERROR, "Interactive binds are driven by the press, not release");
		return false;
	}

	return true;
}

void add_keybind(uint32_t modifiers, xkb_keysym_t keysym, uint32_t keycode, bool use_keycode,
		uint32_t flags, bind_action_t action, int desktop_index, const char *external_cmd,
		const char *submap_name) {
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
		.flags = flags,
	};
	if (!bind_flags_valid(&preview, action))
		return;

	keybind_t *kb = &kb_array[(*num_ptr)++];
	kb->modifiers = modifiers;
	kb->keysym = keysym;
	kb->keycode = keycode;
	kb->use_keycode = use_keycode;
	kb->action = action;
	kb->desktop_index = desktop_index;
	kb->flags = flags;
	if (submap_name)
		str_copy(kb->submap_name, sizeof(kb->submap_name), submap_name);
	else
		kb->submap_name[0] = '\0';
	if (external_cmd)
		str_copy(kb->external_cmd, sizeof(kb->external_cmd), external_cmd);
	else
		kb->external_cmd[0] = '\0';

	char flag_text[128];
	bind_flags_text(flags, flag_text, sizeof(flag_text));
	wlr_log(WLR_DEBUG,
		"Added keybind: mod=%u keysym=%u keycode=%u action=%d index=%d flags=%s submap=%s", modifiers,
		keysym, keycode, action, desktop_index, flag_text, submap_name ? submap_name : "global");
}

static void add_gesturebind(enum gesture_type type, uint8_t fingers, uint32_t directions,
		const char *input, bind_action_t action, int desktop_index, const char *external_cmd) {
	if (num_gesturebinds >= MAX_GESTUREBINDS) {
		wlr_log(WLR_ERROR, "Maximum number of gesture binds reached");
		return;
	}

	gesturebind_t *gb = &gesture_bindings[num_gesturebinds++];
	gb->type = type;
	gb->fingers = fingers;
	gb->directions = directions;
	gb->input = input ? strdup(input) : NULL;
	gb->action = action;
	gb->desktop_index = desktop_index;
	if (external_cmd) {
		str_copy(gb->external_cmd, sizeof(gb->external_cmd), external_cmd);
	} else {
		gb->external_cmd[0] = '\0';
	}

	wlr_log(WLR_DEBUG, "Added gesturebind: type=%d fingers=%d dirs=%u action=%d", type, fingers,
		directions, action);
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

	int desktop_index = 0;
	char submap_name[MAXLEN];
	submap_name[0] = '\0';
	bind_action_t action = parse_action(expanded_cmd, &desktop_index, submap_name);

	if (action != BIND_EXTERNAL) {
		add_gesturebind(gest.type, gest.fingers, gest.directions, NULL, action, desktop_index, NULL);
	} else {
		add_gesturebind(gest.type, gest.fingers, gest.directions, NULL, action, desktop_index,
			expanded_cmd);
	}
}

static void add_hotcornerbind(int corner_x, int corner_y, bind_action_t action, int desktop_index,
		const char *external_cmd) {
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
	hc->action = action;
	hc->desktop_index = desktop_index;
	if (external_cmd)
		str_copy(hc->external_cmd, sizeof(hc->external_cmd), external_cmd);
	else
		hc->external_cmd[0] = '\0';

	wlr_log(WLR_DEBUG, "Added hotcornerbind: corner=%d (%d,%d) action=%d", hc->corner, corner_x,
		corner_y, action);
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

	int desktop_index = 0;
	char submap_name[MAXLEN];
	submap_name[0] = '\0';
	bind_action_t action = parse_action(command_str, &desktop_index, submap_name);

	if (action != BIND_EXTERNAL)
		add_hotcornerbind(corner_x, corner_y, action, desktop_index, NULL);
	else
		add_hotcornerbind(corner_x, corner_y, action, desktop_index, command_str);
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
		int desktop_index = 0;
		char submap_name[MAXLEN];
		submap_name[0] = '\0';
		bind_action_t action = parse_action(command_str, &desktop_index, submap_name);
		if (action != BIND_EXTERNAL) {
			bell_bind = (keybind_t){
				.action = action,
				.desktop_index = desktop_index,
			};
		} else {
			str_copy(bell_bind.external_cmd, sizeof(bell_bind.external_cmd), command_str);
			bell_bind.action = action;
		}
		wlr_log(WLR_DEBUG, "Parsed bell bind: action=%d cmd='%s'", action, command_str);
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

		int desktop_index = 0;
		char submap_name[MAXLEN];
		submap_name[0] = '\0';
		bind_action_t action = parse_action(single_cmd, &desktop_index, submap_name);
		wlr_log(WLR_DEBUG, "Parsed action: %d for cmd: '%s' submap: '%s'", action, single_cmd,
			submap_name);
		if (action != BIND_EXTERNAL)
			add_keybind(modifiers, keysym, keycode, use_keycode, flags, action, desktop_index, NULL,
				submap_name[0] ? submap_name : NULL);
		else
			add_keybind(modifiers, keysym, keycode, use_keycode, flags, action, desktop_index, single_cmd,
				submap_name[0] ? submap_name : NULL);
	}
}

static char config_path[CONFIG_PATH_MAX];
static char hotkey_init_path[CONFIG_PATH_MAX];
static bool config_ran = false;

void run_config(const char *config_path_arg) {
	if (!config_path_arg || access(config_path_arg, R_OK) != 0)
		return;

	str_copy(config_path, sizeof(config_path), config_path_arg);
}

void run_config_idle(void *data) {
	(void)data;
	if (config_ran || config_path[0] == '\0')
		return;

	config_ran = true;

	wlr_log(WLR_INFO, "Running config: %s", config_path);
	if (fork() == 0) {
		setsid();
		execl("/bin/sh", "/bin/sh", config_path, NULL);
		_exit(1);
	}
}

void load_hotkeys_idle(void *data) {
	(void)data;
	if (hotkey_init_path[0] == '\0')
		return;

	load_hotkeys(hotkey_init_path);
	setup_inotify_watch(hotkey_init_path);
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

void load_hotkeys(const char *config_path) {
	wlr_log(WLR_DEBUG, "Loading hotkeys from path: %s", config_path);
	FILE *f = fopen(config_path, "r");
	if (!f) {
		wlr_log(WLR_INFO, "No hotkey config found at %s", config_path);
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
	int offset = 0;
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
		char first_char = *content_start;

		if (line[0] == '#' || line[0] == '\0')
			continue;

		if (first_char == '@') {
			if (indent == 0) {
				flush_pending_bind(&pending);
				pending.indent = 0;
				char *submap_name = content_start + 1;
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
						size_t cur_len = strlen(pending.command);
						size_t rem = sizeof(pending.command) - cur_len - 1;
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
		"and %zu submaps from %s", num_keybinds, num_gesturebinds, num_hotcornerbinds, num_submaps,
			config_path);
}

static void setup_inotify_watch(const char *config_path) {
	if (hotkey_event_source) {
		wl_event_source_remove(hotkey_event_source);
		hotkey_event_source = NULL;
	}

	if (hotkey_watch_fd >= 0) {
		close(hotkey_watch_fd);
		hotkey_watch_fd = -1;
	}

	hotkey_watch_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (hotkey_watch_fd < 0) {
		wlr_log(WLR_ERROR, "Failed to initialize inotify");
		return;
	}

	str_copy(hotkey_config_path, sizeof(hotkey_config_path), config_path);

	char dir_path[CONFIG_PATH_MAX];
	str_copy(dir_path, sizeof(dir_path), config_path);
	char *last_slash = strrchr(dir_path, '/');
	uint32_t mask = IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE;
	if (last_slash) {
		*last_slash = '\0';
		inotify_add_watch(hotkey_watch_fd, dir_path, mask);
	} else {
		inotify_add_watch(hotkey_watch_fd, ".", mask);
	}

	add_hotkey_listener_to_event_loop();
}

void config_init_with_config_dir(const char *config_dir) {
	ONCE();
	custom_config_dir = config_dir;

	const char *config_home = get_config_home();
	char doorsrc_path[CONFIG_PATH_MAX];
	const char *config_source = config_home;

	if (!custom_config_dir) {
		char test_path[CONFIG_PATH_MAX];
		snprintf(test_path, sizeof(test_path), "%s/%s", config_home, DOORSRC_NAME);
		if (access(test_path, R_OK) != 0) {
			snprintf(test_path, sizeof(test_path), "%s/%s", config_home, DOORSHKRC_NAME);
			if (access(test_path, R_OK) != 0)
				config_source = GLOBAL_CONFIG_DIR;
		}
	}

	snprintf(doorsrc_path, sizeof(doorsrc_path), "%s/%s", config_source, DOORSRC_NAME);
	run_config(doorsrc_path);

	snprintf(hotkey_init_path, sizeof(hotkey_init_path), "%s/%s", config_source, DOORSHKRC_NAME);

	refresh_border_color_cache();
}

void config_fini(void) {
	ONCE();
	if (hotkey_event_source) {
		wl_event_source_remove(hotkey_event_source);
		hotkey_event_source = NULL;
	}
	if (doorsrc_debounce_timer) {
		wl_event_source_remove(doorsrc_debounce_timer);
		doorsrc_debounce_timer = NULL;
	}
	if (hotkey_watch_fd >= 0) {
		close(hotkey_watch_fd);
		hotkey_watch_fd = -1;
	}
	num_keybinds = 0;
	num_gesturebinds = 0;
}

void reload_hotkeys(void) {
	if (hotkey_config_path[0] != '\0') {
		wlr_log(WLR_INFO, "Reloading hotkeys from %s", hotkey_config_path);
		load_hotkeys(hotkey_config_path);
	}
}

static struct wl_event_source *hotkey_debounce_timer = NULL;

static int hotkey_debounce_handler(void *data) {
	(void)data;
	reload_hotkeys();
	hotkey_debounce_timer = NULL;
	return 0;
}

static int doorsrc_debounce_handler(void *data) {
	(void)data;
	doorsrc_debounce_timer = NULL;

	ipc_put_status(SUB_MASK_CONFIG, "config_changed %s\n", config_path);
	return 0;
}

static int hotkey_reload_handler(int fd, uint32_t mask, void *data) {
	(void)data;
	if (!(mask & WL_EVENT_READABLE))
		return 0;

	char buf[sizeof(struct inotify_event) + NAME_MAX + 1];
	ssize_t len = read(fd, buf, sizeof(buf));
	if (len <= 0)
		return 0;

	bool doorsrc = false, hotkeys = false;
	for (ssize_t offset = 0; offset + (ssize_t)sizeof(struct inotify_event) <= len; ) {
		struct inotify_event *event = (struct inotify_event *)(buf + offset);

		if (event->len > 0 && event->name[0] != '\0') {
			doorsrc |= file_event_matches(config_path, event->name);
			hotkeys |= file_event_matches(hotkey_config_path, event->name);
		} else {
			// an event on the directory itself
			hotkeys = true;
		}

		offset += (ssize_t)(sizeof(struct inotify_event) + event->len);
	}

	if (doorsrc) {
		if (doorsrc_debounce_timer) {
			wl_event_source_timer_update(doorsrc_debounce_timer, 100);
		} else if (hotkey_event_loop) {
			doorsrc_debounce_timer = wl_event_loop_add_timer(hotkey_event_loop, doorsrc_debounce_handler,
				NULL);
			if (doorsrc_debounce_timer)
				wl_event_source_timer_update(doorsrc_debounce_timer, 100);
		}
	}

	if (!hotkeys)
		return 0;

	if (hotkey_debounce_timer) {
		wl_event_source_timer_update(hotkey_debounce_timer, 100);
	} else if (hotkey_event_loop) {
		hotkey_debounce_timer = wl_event_loop_add_timer(hotkey_event_loop, hotkey_debounce_handler, NULL);
		if (hotkey_debounce_timer)
			wl_event_source_timer_update(hotkey_debounce_timer, 100);
	}
	return 0;
}

bool keybind_matches(const keybind_t *kb, uint32_t modifiers, xkb_keysym_t keysym,
		uint32_t keycode) {
	if (!kb)
		return false;

	bool mods_ok;
	if (kb->flags & BINDF_IGNORE_MODS) {
		mods_ok = (modifiers & kb->modifiers) == kb->modifiers;
	} else {
		mods_ok = kb->modifiers == modifiers;
	}

	if (kb->use_keycode)
		return mods_ok && (kb->keycode == keycode);

	if (!mods_ok)
		return false;

	if (kb->keysym == keysym)
		return true;
	xkb_keysym_t lower = xkb_keysym_to_lower(keysym);
	return kb->keysym == lower;
}

static bool keybind_matches_now(const keybind_t *kb, uint32_t modifiers, xkb_keysym_t keysym,
		uint32_t keycode, bool prefer_keycode) {
	if (kb->use_keycode != prefer_keycode)
		return false;
	return keybind_matches(kb, modifiers, keysym, keycode);
}

// true for binds that only participate in release-triggered (or catchall) matching
static bool keybind_is_deferred(const keybind_t *kb) {
	return (kb->flags & (BINDF_RELEASE | BINDF_CATCH_ALL)) != 0;
}

keybind_t *find_keybind(uint32_t modifiers, xkb_keysym_t keysym, uint32_t keycode,
		bool prefer_keycode, bool fallback_to_global) {
	if (active_submap) {
		for (size_t i = 0; i < active_submap->num_keybinds; i++) {
			keybind_t *kb = &active_submap->keybinds[i];
			if (keybind_is_deferred(kb))
				continue;
			if (keybind_matches_now(kb, modifiers, keysym, keycode, prefer_keycode))
				return kb;
		}

		// universal binds
		for (size_t i = 0; i < num_keybinds; i++) {
			keybind_t *kb = &keybinds[i];
			if (keybind_is_deferred(kb) || !(kb->flags & BINDF_SUBMAP_UNIVERSAL))
				continue;
			if (keybind_matches_now(kb, modifiers, keysym, keycode, prefer_keycode))
				return kb;
		}

		if (!fallback_to_global)
			return NULL;
	}

	for (size_t i = 0; i < num_keybinds; i++) {
		keybind_t *kb = &keybinds[i];
		if (keybind_is_deferred(kb))
			continue;
		if (keybind_matches_now(kb, modifiers, keysym, keycode, prefer_keycode))
			return kb;
	}

	return NULL;
}

submap_t *find_submap_by_name(const char *name) {
	if (!name || name[0] == '\0')
		return NULL;

	for (size_t i = 0; i < num_submaps; i++)
		if (streq(submaps[i].name, name))
			return &submaps[i];

	return NULL;
}

keybind_t *find_release_bind(uint32_t modifiers, xkb_keysym_t keysym, uint32_t keycode,
		const char *submap_at_press) {
	submap_t *sm = submap_at_press ? find_submap_by_name(submap_at_press) : NULL;
	if (sm) {
		for (size_t i = 0; i < sm->num_keybinds; i++) {
			keybind_t *kb = &sm->keybinds[i];
			if (!(kb->flags & BINDF_RELEASE))
				continue;
			if (keybind_matches(kb, modifiers, keysym, keycode))
				return kb;
		}
		return NULL;
	}

	for (size_t i = 0; i < num_keybinds; i++) {
		keybind_t *kb = &keybinds[i];
		if (!(kb->flags & BINDF_RELEASE))
			continue;
		if (keybind_matches(kb, modifiers, keysym, keycode))
			return kb;
	}

	return NULL;
}

bool is_interactive_bind(const keybind_t *kb) {
	return kb && (kb->action == BIND_INTERACTIVE_MOVE || kb->action == BIND_INTERACTIVE_RESIZE ||
		kb->action == BIND_TILING_DRAG);
}

void execute_bind(bind_t b) {
	switch (b.action) {
	case BIND_QUIT:
		wl_display_terminate(server.wl_display);
		break;
	case BIND_NODE_FOCUS:
		break;
	case BIND_NODE_CLOSE:
		close_focused();
		break;
	case BIND_NODE_STATE_TILED:
		tile_focused();
		break;
	case BIND_NODE_STATE_FLOATING:
		toggle_floating();
		break;
	case BIND_NODE_STATE_FULLSCREEN:
		toggle_fullscreen();
		break;
	case BIND_NODE_STATE_MAXIMIZED:
		toggle_maximize();
		break;
	case BIND_NODE_STATE_MINIMIZED:
		toggle_minimize();
		break;
	case BIND_NODE_TO_DESKTOP:
		if (b.desktop_index > 0) {
			send_to_desktop(b.desktop_index - 1);
		}
		break;
	case BIND_DESKTOP_FOCUS:
		if (b.desktop_index > 0) {
			workspace_switch_to_desktop_by_index(b.desktop_index - 1);
		}
		break;
	case BIND_DESKTOP_LAYOUT_TILED:
		set_tiled_layout();
		break;
	case BIND_DESKTOP_LAYOUT_MONOCLE:
		toggle_monocle();
		break;
	case BIND_FOCUS_NORTH:
	case BIND_FOCUS_WEST:
	case BIND_FOCUS_SOUTH:
	case BIND_FOCUS_EAST:
		focus_dir(b.action - BIND_FOCUS_NORTH);
		break;
	case BIND_SWAP_NORTH:
	case BIND_SWAP_WEST:
	case BIND_SWAP_SOUTH:
	case BIND_SWAP_EAST:
		swap_dir(b.action - BIND_SWAP_NORTH);
		break;
	case BIND_PRESEL_NORTH:
	case BIND_PRESEL_WEST:
	case BIND_PRESEL_SOUTH:
	case BIND_PRESEL_EAST:
		if (mon == NULL || mon->desk == NULL || mon->desk->focus == NULL)
			return;

		presel_dir(mon->desk->focus, b.action - BIND_PRESEL_NORTH);
		break;
	case BIND_PRESEL_CANCEL:
		cancel_presel();
		break;
	case BIND_TOGGLE_FLOATING:
		toggle_floating();
		break;
	case BIND_TOGGLE_FULLSCREEN:
		toggle_fullscreen();
		break;
	case BIND_TOGGLE_MAXIMIZE:
		toggle_maximize();
		break;
	case BIND_TOGGLE_MINIMIZE:
		toggle_minimize();
		break;
	case BIND_RESTORE_MINIMIZED:
		restore_minimized();
		break;
	case BIND_TOGGLE_PSEUDO_TILED:
		toggle_pseudo_tiled();
		break;
	case BIND_TOGGLE_MONOCLE:
		toggle_monocle();
		break;
	case BIND_TOGGLE_MASTER_STACK:
	case BIND_DESKTOP_LAYOUT_MASTER_STACK:
		toggle_master_stack();
		break;
	case BIND_TOGGLE_FLOATING_LAYOUT:
	case BIND_DESKTOP_LAYOUT_FLOATING:
		toggle_floating_layout();
		break;
	case BIND_MASTER_STACK_INC:
		if (mon && mon->desk) {
			master_stack_increment(mon->desk);
			if (mon->desk->layout == LAYOUT_MASTER_STACK)
				arrange(mon, mon->desk, true);
		}
		break;
	case BIND_MASTER_STACK_DEC:
		if (mon && mon->desk) {
			master_stack_decrement(mon->desk);
			if (mon->desk->layout == LAYOUT_MASTER_STACK)
				arrange(mon, mon->desk, true);
		}
		break;
	case BIND_MASTER_STACK_FLIP:
		if (mon && mon->desk) {
			master_stack_flip_orientation(mon->desk);
			if (mon->desk->layout == LAYOUT_MASTER_STACK)
				arrange(mon, mon->desk, true);
		}
		break;
	case BIND_MASTER_STACK_CYCLE_ORIENTATION:
		if (mon && mon->desk) {
			master_stack_cycle_orientation(mon->desk);
			if (mon->desk->layout == LAYOUT_MASTER_STACK)
				arrange(mon, mon->desk, true);
		}
		break;
	case BIND_MASTER_STACK_CYCLE_STACK_LAYOUT:
		if (mon && mon->desk) {
			master_stack_cycle_stack_layout(mon->desk);
			if (mon->desk->layout == LAYOUT_MASTER_STACK)
				arrange(mon, mon->desk, true);
		}
		break;
	case BIND_SCROLLER_CYCLE_PRESET:
	case BIND_SCROLLER_CYCLE_PRESET_BACK:
	case BIND_SCROLLER_CYCLE_HEIGHT:
	case BIND_SCROLLER_CYCLE_HEIGHT_BACK:
	case BIND_SCROLLER_TOGGLE_FULL_WIDTH:
	case BIND_SCROLLER_EXPAND_COLUMN:
	case BIND_SCROLLER_CENTER:
	case BIND_SCROLLER_CENTER_VISIBLE:
	case BIND_FOCUS_COLUMN_FIRST:
	case BIND_FOCUS_COLUMN_LAST:
	case BIND_FOCUS_DOWN_OR_LEFT:
	case BIND_FOCUS_DOWN_OR_RIGHT:
	case BIND_FOCUS_UP_OR_LEFT:
	case BIND_FOCUS_UP_OR_RIGHT:
	case BIND_MOVE_COLUMN_LEFT:
	case BIND_MOVE_COLUMN_RIGHT:
	case BIND_MOVE_COLUMN_UP:
	case BIND_MOVE_COLUMN_DOWN:
	case BIND_MOVE_COLUMN_FIRST:
	case BIND_MOVE_COLUMN_LAST: {
		if (mon == NULL || mon->desk == NULL || mon->desk->layout != LAYOUT_SCROLLER)
			return;

		bool changed = scroller_bind_action(mon->desk, b.action);
		if (changed)
			arrange(mon, mon->desk, true);
		break;
	}
	case BIND_ROTATE_CW:
		rotate_clockwise();
		break;
	case BIND_ROTATE_CCW:
		rotate_counterclockwise();
		break;
	case BIND_FLIP_HORIZONTAL:
	case BIND_FLIP_VERTICAL:
		if (mon == NULL || mon->desk == NULL || mon->desk->root == NULL)
			return;

		flip_tree(mon->desk->root, b.action - BIND_FLIP_HORIZONTAL);
		arrange(mon, mon->desk, true);
		break;
	case BIND_RESIZE_LEFT:
		resize_left();
		break;
	case BIND_RESIZE_RIGHT:
		resize_right();
		break;
	case BIND_RESIZE_UP:
		resize_up();
		break;
	case BIND_RESIZE_DOWN:
		resize_down();
		break;
	case BIND_DESKTOP_NEXT:
		focus_next_desktop();
		break;
	case BIND_DESKTOP_PREV:
		focus_prev_desktop();
		break;
	case BIND_DESKTOP_LAST:
		focus_last_desktop();
		break;
	case BIND_SEND_TO_DESKTOP_NEXT:
		send_to_next_desktop();
		break;
	case BIND_SEND_TO_DESKTOP_PREV:
		send_to_prev_desktop();
		break;
	case BIND_DESKTOP_1:
	case BIND_DESKTOP_2:
	case BIND_DESKTOP_3:
	case BIND_DESKTOP_4:
	case BIND_DESKTOP_5:
	case BIND_DESKTOP_6:
	case BIND_DESKTOP_7:
	case BIND_DESKTOP_8:
	case BIND_DESKTOP_9:
	case BIND_DESKTOP_10:
		workspace_switch_to_desktop_by_index(b.desktop_index - 1);
		break;
	case BIND_SEND_TO_DESKTOP_1:
	case BIND_SEND_TO_DESKTOP_2:
	case BIND_SEND_TO_DESKTOP_3:
	case BIND_SEND_TO_DESKTOP_4:
	case BIND_SEND_TO_DESKTOP_5:
	case BIND_SEND_TO_DESKTOP_6:
	case BIND_SEND_TO_DESKTOP_7:
	case BIND_SEND_TO_DESKTOP_8:
	case BIND_SEND_TO_DESKTOP_9:
	case BIND_SEND_TO_DESKTOP_10:
		send_to_desktop(b.desktop_index - 1);
		break;
	case BIND_EXTERNAL:
	case BIND_NONE:
		launcher_exec(b.external_cmd);
		break;
	case BIND_ENTER_SUBMAP:
		enter_submap(b.submap_name);
		break;
	case BIND_EXIT_SUBMAP:
		exit_submap();
		break;
	case BIND_INTERACTIVE_MOVE:
	case BIND_INTERACTIVE_RESIZE:
	case BIND_TILING_DRAG:
		break;
	}
}

void execute_keybind(const keybind_t *kb) {
	if (!kb)
		return;
	bind_t bind = {
		.action = kb->action,
		.desktop_index = kb->desktop_index,
		.submap_name = kb->submap_name,
		.external_cmd = kb->external_cmd,
	};
	execute_bind(bind);
}

void execute_bell_bind(void) {
	if (bell_bind.action != BIND_NONE || bell_bind.external_cmd[0] != '\0')
		execute_keybind(&bell_bind);
}

void setup_hotkey_event_listener(struct wl_event_loop *event_loop) {
	hotkey_event_loop = event_loop;
	add_hotkey_listener_to_event_loop();
}

static void add_hotkey_listener_to_event_loop(void) {
	if (hotkey_event_source) {
		wl_event_source_remove(hotkey_event_source);
		hotkey_event_source = NULL;
	}

	if (hotkey_event_loop && hotkey_watch_fd >= 0)
		hotkey_event_source = wl_event_loop_add_fd(hotkey_event_loop, hotkey_watch_fd, WL_EVENT_READABLE,
			hotkey_reload_handler, NULL);
}

static submap_t *find_submap(const char *name) {
	for (size_t i = 0; i < num_submaps; i++)
		if (streq(submaps[i].name, name))
			return &submaps[i];
	return NULL;
}

void enter_submap(const char *name) {
	submap_t *sm = find_submap(name);
	if (sm) {
		active_submap = sm;
		wlr_log(WLR_INFO, "Entered submap: %s", name);
	} else {
		wlr_log(WLR_ERROR, "Submap not found: %s", name);
	}
}

void exit_submap(void) {
	if (active_submap) {
		wlr_log(WLR_INFO, "Exited submap: %s", active_submap->name);
		active_submap = NULL;
	}
}

keyboard_grouping_t get_keyboard_grouping(void) {
	return keyboard_grouping;
}

void set_keyboard_grouping(keyboard_grouping_t grouping) {
	if (keyboard_grouping != grouping) {
		keyboard_grouping = grouping;
		wlr_log(WLR_INFO, "Keyboard grouping set to %d", (int)grouping);
		keyboard_reapply_grouping();
	}
}

bool gesturebind_matches(const gesturebind_t *gb, enum gesture_type type, uint8_t fingers) {
	return !(!gb || gb->type != type || (gb->fingers != GESTURE_FINGERS_ANY &&
		gb->fingers != fingers));
}

void execute_gesturebind(const gesturebind_t *gb) {
	if (!gb || gb->action == BIND_ENTER_SUBMAP)
		return;
	bind_t bind = {
		.action = gb->action,
		.desktop_index = gb->desktop_index,
		.external_cmd = gb->external_cmd,
		.submap_name = NULL
	};
	execute_bind(bind);
}

bool hotcornerbind_matches(const hotcornerbind_t *hc, int corner_x, int corner_y) {
	if (!hc)
		return false;
	return hc->corner_x == corner_x && hc->corner_y == corner_y;
}

hotcornerbind_t *hotcorner_bind_match(int corner_x, int corner_y) {
	for (size_t i = 0; i < num_hotcornerbinds; i++)
		if (hotcornerbind_matches(&hotcorner_bindings[i], corner_x, corner_y))
			return &hotcorner_bindings[i];

	return NULL;
}

void execute_hotcornerbind(const hotcornerbind_t *hc) {
	if (!hc || hc->action == BIND_ENTER_SUBMAP)
		return;
	bind_t bind = {
		.action = hc->action,
		.desktop_index = hc->desktop_index,
		.external_cmd = hc->external_cmd,
		.submap_name = NULL
	};
	execute_bind(bind);
}

const char *bind_action_name(bind_action_t action) {
	static const char *names[] = {
		"none",
		"quit",
		"enter_submap",
		"exit_submap",
		"node_focus",
		"node_close",
		"node_state_tiled",
		"node_state_floating",
		"node_state_fullscreen",
		"node_state_maximized",
		"node_state_minimized",
		"node_to_desktop",
		"desktop_focus",
		"desktop_layout_tiled",
		"desktop_layout_monocle",
		"focus_north",
		"focus_west",
		"focus_south",
		"focus_east",
		"swap_north",
		"swap_west",
		"swap_south",
		"swap_east",
		"presel_north",
		"presel_west",
		"presel_south",
		"presel_east",
		"presel_cancel",
		"toggle_floating",
		"toggle_fullscreen",
		"toggle_maximize",
		"toggle_minimize",
		"toggle_pseudo_tiled",
		"toggle_monocle",
		"toggle_master_stack",
		"toggle_floating_layout",
		"desktop_layout_master_stack",
		"desktop_layout_floating",
		"master_stack_inc",
		"master_stack_dec",
		"master_stack_flip",
		"master_stack_cycle_orientation",
		"master_stack_cycle_stack_layout",
		"rotate_cw",
		"rotate_ccw",
		"flip_horizontal",
		"flip_vertical",
		"desktop_next",
		"desktop_prev",
		"desktop_last",
		"send_to_desktop_next",
		"send_to_desktop_prev",
		"send_to_desktop_1",
		"send_to_desktop_2",
		"send_to_desktop_3",
		"send_to_desktop_4",
		"send_to_desktop_5",
		"send_to_desktop_6",
		"send_to_desktop_7",
		"send_to_desktop_8",
		"send_to_desktop_9",
		"send_to_desktop_10",
		"desktop_1",
		"desktop_2",
		"desktop_3",
		"desktop_4",
		"desktop_5",
		"desktop_6",
		"desktop_7",
		"desktop_8",
		"desktop_9",
		"desktop_10",
		"resize_left",
		"resize_right",
		"resize_up",
		"resize_down",
		"interactive_move",
		"interactive_resize",
		"tiling_drag",
		"external",
		"restore_minimized",
		"scroller_cycle_preset",
		"scroller_cycle_preset_back",
		"scroller_cycle_height",
		"scroller_cycle_height_back",
		"scroller_toggle_full_width",
		"scroller_expand_column",
		"scroller_center",
		"scroller_center_visible",
		"focus_column_first",
		"focus_column_last",
		"focus_down_or_left",
		"focus_down_or_right",
		"focus_up_or_left",
		"focus_up_or_right",
		"move_column_left",
		"move_column_right",
		"move_column_up",
		"move_column_down",
		"move_column_to_first",
		"move_column_to_last",
	};
	_Static_assert(sizeof(names) / sizeof(names[0]) == BIND_ACTION_COUNT,
		"bind_action_name() must have exactly one name per bind_action_t value");
	if (action >= 0 && action < (int)(sizeof(names) / sizeof(names[0])))
		return names[action];

	return "unknown";
}

void execute_bind_action(bind_action_t action) {
	bind_t b = {
		.action = action,
		.desktop_index = 0
	};
	execute_bind(b);
}

bind_action_t bind_action_from_name(const char *name) {
	if (!name)
		return BIND_NONE;

#define ACTION_IF_MATCH(n, val)                                                                                        \
	if (streq(name, n))                                                                                            \
	return val

	ACTION_IF_MATCH("west", BIND_FOCUS_WEST);
	ACTION_IF_MATCH("w", BIND_FOCUS_WEST);
	ACTION_IF_MATCH("east", BIND_FOCUS_EAST);
	ACTION_IF_MATCH("e", BIND_FOCUS_EAST);
	ACTION_IF_MATCH("north", BIND_FOCUS_NORTH);
	ACTION_IF_MATCH("n", BIND_FOCUS_NORTH);
	ACTION_IF_MATCH("south", BIND_FOCUS_SOUTH);
	ACTION_IF_MATCH("s", BIND_FOCUS_SOUTH);

	ACTION_IF_MATCH("swap_west", BIND_SWAP_WEST);
	ACTION_IF_MATCH("swap_east", BIND_SWAP_EAST);
	ACTION_IF_MATCH("swap_north", BIND_SWAP_NORTH);
	ACTION_IF_MATCH("swap_south", BIND_SWAP_SOUTH);

	ACTION_IF_MATCH("presel_west", BIND_PRESEL_WEST);
	ACTION_IF_MATCH("presel_east", BIND_PRESEL_EAST);
	ACTION_IF_MATCH("presel_north", BIND_PRESEL_NORTH);
	ACTION_IF_MATCH("presel_south", BIND_PRESEL_SOUTH);
	ACTION_IF_MATCH("presel_cancel", BIND_PRESEL_CANCEL);

	ACTION_IF_MATCH("resize_left", BIND_RESIZE_LEFT);
	ACTION_IF_MATCH("left", BIND_RESIZE_LEFT);
	ACTION_IF_MATCH("resize_right", BIND_RESIZE_RIGHT);
	ACTION_IF_MATCH("right", BIND_RESIZE_RIGHT);
	ACTION_IF_MATCH("resize_up", BIND_RESIZE_UP);
	ACTION_IF_MATCH("up", BIND_RESIZE_UP);
	ACTION_IF_MATCH("resize_down", BIND_RESIZE_DOWN);
	ACTION_IF_MATCH("down", BIND_RESIZE_DOWN);

	ACTION_IF_MATCH("toggle_floating", BIND_TOGGLE_FLOATING);
	ACTION_IF_MATCH("toggle_fullscreen", BIND_TOGGLE_FULLSCREEN);
	ACTION_IF_MATCH("toggle_maximize", BIND_TOGGLE_MAXIMIZE);
	ACTION_IF_MATCH("toggle_minimize", BIND_TOGGLE_MINIMIZE);
	ACTION_IF_MATCH("restore_minimized", BIND_RESTORE_MINIMIZED);
	ACTION_IF_MATCH("toggle_pseudo_tiled", BIND_TOGGLE_PSEUDO_TILED);
	ACTION_IF_MATCH("toggle_monocle", BIND_TOGGLE_MONOCLE);
	ACTION_IF_MATCH("toggle_master_stack", BIND_TOGGLE_MASTER_STACK);
	ACTION_IF_MATCH("toggle_floating_layout", BIND_TOGGLE_FLOATING_LAYOUT);
	ACTION_IF_MATCH("floating_layout", BIND_TOGGLE_FLOATING_LAYOUT);
	ACTION_IF_MATCH("master_stack_inc", BIND_MASTER_STACK_INC);
	ACTION_IF_MATCH("master_stack_dec", BIND_MASTER_STACK_DEC);
	ACTION_IF_MATCH("master_stack_flip", BIND_MASTER_STACK_FLIP);
	ACTION_IF_MATCH("master_stack_cycle_orientation", BIND_MASTER_STACK_CYCLE_ORIENTATION);
	ACTION_IF_MATCH("master_stack_cycle_stack_layout", BIND_MASTER_STACK_CYCLE_STACK_LAYOUT);

	ACTION_IF_MATCH("rotate_cw", BIND_ROTATE_CW);
	ACTION_IF_MATCH("rotate_ccw", BIND_ROTATE_CCW);

	ACTION_IF_MATCH("flip_horizontal", BIND_FLIP_HORIZONTAL);
	ACTION_IF_MATCH("flip_vertical", BIND_FLIP_VERTICAL);

	ACTION_IF_MATCH("desktop_next", BIND_DESKTOP_NEXT);
	ACTION_IF_MATCH("next", BIND_DESKTOP_NEXT);
	ACTION_IF_MATCH("desktop_prev", BIND_DESKTOP_PREV);
	ACTION_IF_MATCH("desktop_last", BIND_DESKTOP_LAST);

	ACTION_IF_MATCH("quit", BIND_QUIT);

	ACTION_IF_MATCH("send_to_desktop_next", BIND_SEND_TO_DESKTOP_NEXT);
	ACTION_IF_MATCH("send_to_desktop_prev", BIND_SEND_TO_DESKTOP_PREV);

	ACTION_IF_MATCH("send_next", BIND_SEND_TO_DESKTOP_NEXT);
	ACTION_IF_MATCH("send_prev", BIND_SEND_TO_DESKTOP_PREV);
	ACTION_IF_MATCH("send", BIND_SEND_TO_DESKTOP_NEXT);

	ACTION_IF_MATCH("clockwise", BIND_ROTATE_CW);
	ACTION_IF_MATCH("cw", BIND_ROTATE_CW);
	ACTION_IF_MATCH("counterclockwise", BIND_ROTATE_CCW);
	ACTION_IF_MATCH("ccw", BIND_ROTATE_CCW);
	ACTION_IF_MATCH("horizontal", BIND_FLIP_HORIZONTAL);
	ACTION_IF_MATCH("h", BIND_FLIP_HORIZONTAL);
	ACTION_IF_MATCH("vertical", BIND_FLIP_VERTICAL);
	ACTION_IF_MATCH("v", BIND_FLIP_VERTICAL);

	ACTION_IF_MATCH("scroller_cycle_preset", BIND_SCROLLER_CYCLE_PRESET);
	ACTION_IF_MATCH("scroller_cycle_preset_back", BIND_SCROLLER_CYCLE_PRESET_BACK);
	ACTION_IF_MATCH("scroller_cycle_height", BIND_SCROLLER_CYCLE_HEIGHT);
	ACTION_IF_MATCH("scroller_cycle_height_back", BIND_SCROLLER_CYCLE_HEIGHT_BACK);
	ACTION_IF_MATCH("scroller_toggle_full_width", BIND_SCROLLER_TOGGLE_FULL_WIDTH);
	ACTION_IF_MATCH("scroller_expand_column", BIND_SCROLLER_EXPAND_COLUMN);
	ACTION_IF_MATCH("scroller_center", BIND_SCROLLER_CENTER);
	ACTION_IF_MATCH("scroller_center_visible", BIND_SCROLLER_CENTER_VISIBLE);
	ACTION_IF_MATCH("focus_column_first", BIND_FOCUS_COLUMN_FIRST);
	ACTION_IF_MATCH("focus_column_last", BIND_FOCUS_COLUMN_LAST);
	ACTION_IF_MATCH("focus_down_or_left", BIND_FOCUS_DOWN_OR_LEFT);
	ACTION_IF_MATCH("focus_down_or_right", BIND_FOCUS_DOWN_OR_RIGHT);
	ACTION_IF_MATCH("focus_up_or_left", BIND_FOCUS_UP_OR_LEFT);
	ACTION_IF_MATCH("focus_up_or_right", BIND_FOCUS_UP_OR_RIGHT);
	ACTION_IF_MATCH("move_column_left", BIND_MOVE_COLUMN_LEFT);
	ACTION_IF_MATCH("move_column_right", BIND_MOVE_COLUMN_RIGHT);
	ACTION_IF_MATCH("move_column_up", BIND_MOVE_COLUMN_UP);
	ACTION_IF_MATCH("move_column_down", BIND_MOVE_COLUMN_DOWN);
	ACTION_IF_MATCH("move_column_to_first", BIND_MOVE_COLUMN_FIRST);
	ACTION_IF_MATCH("move_column_to_last", BIND_MOVE_COLUMN_LAST);

#undef ACTION_IF_MATCH

	return BIND_NONE;
}
