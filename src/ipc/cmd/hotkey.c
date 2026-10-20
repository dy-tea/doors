#include "config.h"
#include "ipc/args.h"
#include "ipc/registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_keyboard.h>
#include <xkbcommon/xkbcommon.h>

static void hotkey_list(ipc_args_t *a) {
	IPC_REPLY(b);
	for (size_t i = 0; i < num_keybinds; i++) {
		keybind_t *kb = &keybinds[i];

		char mods[64] = {0};
		if (kb->modifiers & WLR_MODIFIER_LOGO)
			str_append_sep(mods, sizeof(mods), "+", "super");
		if (kb->modifiers & WLR_MODIFIER_ALT)
			str_append_sep(mods, sizeof(mods), "+", "alt");
		if (kb->modifiers & WLR_MODIFIER_CTRL)
			str_append_sep(mods, sizeof(mods), "+", "ctrl");
		if (kb->modifiers & WLR_MODIFIER_SHIFT)
			str_append_sep(mods, sizeof(mods), "+", "shift");

		char key_name[64] = {0};
		if (kb->use_keycode)
			snprintf(key_name, sizeof(key_name), "keycode_%u", kb->keycode);
		else
			xkb_keysym_get_name(kb->keysym, key_name, sizeof(key_name));

		const char *action_label;
		char action_buf[MAXLEN * 2 + 32];
		if (kb->action == BIND_EXTERNAL)
			action_label = kb->external_cmd;
		else
			action_label = bind_action_name(kb->action);

		if (kb->submap_name[0])
			snprintf(action_buf, sizeof(action_buf), "%s [submap: %s]", action_label, kb->submap_name);
		else
			snprintf(action_buf, sizeof(action_buf), "%s", action_label);

		char flags_buf[64];
		bind_flags_text(kb->flags, flags_buf, sizeof(flags_buf));
		if (flags_buf[0]) {
			char flag_part[80];
			snprintf(flag_part, sizeof(flag_part), " [flags: %s]", flags_buf);
			str_append(action_buf, sizeof(action_buf), flag_part);
		}

		ipc_buff(&b, "%zu\t%s\t%s\t%s\n", i, mods[0] ? mods : "(none)", key_name, action_buf);
	}
	ipc_buff(&b, "Total: %zu keybinds\n", num_keybinds);
	ipc_buf_send(a, &b);
}

static void hotkey_add(ipc_args_t *a) {
	const char *spec;
	if (!ipc_need(a, "<modifiers+key>", &spec))
		return;

	uint32_t modifiers = 0;
	xkb_keysym_t keysym = XKB_KEY_NoSymbol;
	uint32_t keycode = 0;
	bool use_keycode = false;
	uint32_t flags = 0;

	const char *error = parse_hotkey_spec(spec, &modifiers, &keysym, &keycode, &use_keycode, &flags);
	if (error) {
		ipc_fail(a, "Invalid hotkey '%s': %s\n", spec, error);
		return;
	}

	if (flags & BINDF_CATCH_ALL) {
		ipc_fail(a, "Catch-all binds are only allowed inside a submap\n");
		return;
	}

	const char *command;
	if (!ipc_need(a, "<command>", &command))
		return;

	// the rest of the line is the external command to run
	char cmd[MAXLEN] = {0};
	str_copy(cmd, sizeof(cmd), command);
	ipc_foreach(a, rest) {
		if (!str_append_sep(cmd, sizeof(cmd), " ", rest))
			break;
	}

	int desktop_index = 0;
	char submap_name[MAXLEN];
	submap_name[0] = '\0';
	bind_action_t action = parse_action(cmd, &desktop_index, submap_name);

	add_keybind(modifiers, keysym, keycode, use_keycode, flags, action, desktop_index,
		(action == BIND_EXTERNAL) ? cmd : NULL, submap_name[0] ? submap_name : NULL);

	ipc_ok(a, "Hotkey added\n");
}

static const ipc_sub_t hotkey_subs[] = {
	IPC_SUB("list", NULL, "hotkey list", hotkey_list),
	IPC_SUB("add", NULL, "hotkey add <modifiers+key>[flags] <command>", hotkey_add),
	IPC_SUB_END,
};

void ipc_cmd_hotkey(ipc_args_t *a) {
	const char *first = ipc_peek(a);
	if (first && streq(first, "list") && ipc_left(a) == 1) {
		ipc_take(a);
		hotkey_list(a);
		return;
	}

	if (first && streq(first, "add"))
		ipc_take(a);

	if (!ipc_peek(a)) {
		ipc_fail_unknown(a, hotkey_subs);
		return;
	}

	hotkey_add(a);
}
