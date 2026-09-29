#include "config.h"
#include "ipc/args.h"
#include "ipc/helpers.h"
#include "ipc/registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_keyboard.h>
#include <xkbcommon/xkbcommon.h>

static void hotkey_list(ipc_args_t *a) {
	char buf[DOORS_BUFSIZ];
	ipc_buf_t b;
	ipc_buf_init(&b, buf, sizeof(buf));
	for (size_t i = 0; i < num_keybinds; i++) {
		keybind_t *kb = &keybinds[i];

		char mods[64] = {0};
		if (kb->modifiers & WLR_MODIFIER_LOGO)
			strcat(mods, "super+");
		if (kb->modifiers & WLR_MODIFIER_ALT)
			strcat(mods, "alt+");
		if (kb->modifiers & WLR_MODIFIER_CTRL)
			strcat(mods, "ctrl+");
		if (kb->modifiers & WLR_MODIFIER_SHIFT)
			strcat(mods, "shift+");
		if (mods[0])
			mods[strlen(mods) - 1] = '\0';

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

		ipc_buff(&b, "%zu\t%s\t%s\t%s\n", i, mods[0] ? mods : "(none)", key_name, action_buf);
	}
	ipc_buff(&b, "Total: %zu keybinds\n", num_keybinds);
	ipc_buf_send(a, &b);
}

static void hotkey_add(ipc_args_t *a) {
	const char *spec;
	if (!ipc_need(a, "<modifiers+key>", &spec))
		return;

	char hotkey_str[MAXLEN];
	snprintf(hotkey_str, sizeof(hotkey_str), "%s", spec);

	uint32_t modifiers = 0;
	xkb_keysym_t keysym = XKB_KEY_NoSymbol;
	uint32_t keycode = 0;
	bool use_keycode = false;

	char *plus = strrchr(hotkey_str, '+');
	if (plus) {
		*plus = '\0';
		modifiers = parse_modifiers(hotkey_str);
		char *key_part = plus + 1;
		while (*key_part == ' ')
			key_part++;
		keysym = parse_keysym(key_part);
		keycode = parse_keycode(key_part);
		if (keycode > 0)
			use_keycode = true;
	} else {
		keysym = parse_keysym(hotkey_str);
		keycode = parse_keycode(hotkey_str);
		if (keycode > 0)
			use_keycode = true;
	}

	if (keysym == XKB_KEY_NoSymbol && keycode == 0) {
		ipc_fail(a, "Unknown keysym\n");
		return;
	}

	const char *command;
	if (!ipc_need(a, "<command>", &command))
		return;

	// the rest of the line is the external command to run
	char cmd[MAXLEN] = {0};
	int offset = snprintf(cmd, sizeof(cmd), "%s", command);
	ipc_foreach(a, rest) {
		if ((size_t)offset + 1 >= sizeof(cmd))
			break;
		offset += snprintf(cmd + offset, sizeof(cmd) - offset, " %s", rest);
	}

	int desktop_index = 0;
	char submap_name[MAXLEN];
	submap_name[0] = '\0';
	bind_action_t action = parse_action(cmd, &desktop_index, submap_name);

	add_keybind(modifiers, keysym, keycode, use_keycode, action, desktop_index,
		(action == BIND_EXTERNAL) ? cmd : NULL, submap_name[0] ? submap_name : NULL);

	ipc_ok(a, "Hotkey added\n");
}

static const ipc_sub_t hotkey_subs[] = {
	IPC_SUB("list", NULL, "hotkey list", hotkey_list),
	IPC_SUB_END,
};

void ipc_cmd_hotkey(ipc_args_t *a) {
	const char *first = ipc_peek(a);
	if (first && streq(first, "list") && ipc_left(a) == 1) {
		ipc_take(a);
		hotkey_list(a);
		return;
	}

	if (!ipc_peek(a)) {
		ipc_fail_unknown(a, hotkey_subs);
		return;
	}

	hotkey_add(a);
}
