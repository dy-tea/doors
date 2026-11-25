#include "config.h"
#include "config_internal.h"
#include "ipc/ipc.h"
#include "launcher.h"
#include "macros.h"
#include "settings.h"
#include "str.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static char config_path[CONFIG_PATH_MAX];
static char hotkey_init_path[CONFIG_PATH_MAX];
static bool config_ran = false;

const char *config_doorsrc_path(void) {
	return config_path;
}

const char *config_hotkeys_path(void) {
	return hotkey_init_path;
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
	hotkey_watch_fini();
	num_keybinds = 0;
	num_gesturebinds = 0;
}

// doorsctl commands run through ipc, everything else is a shell command
static void execute_bind_command(const char *cmd) {
	const char *ipc_cmd = str_skip_prefix(cmd, "doorsctl ");
	if (ipc_cmd)
		ipc_run_line(ipc_cmd);
	else
		launcher_exec(cmd);
}

void execute_keybind(const keybind_t *kb) {
	if (!kb)
		return;

	switch (kb->kind) {
	case BIND_KIND_COMMAND:
		execute_bind_command(kb->cmd);
		break;
	case BIND_KIND_ENTER_SUBMAP:
		enter_submap(kb->submap_name);
		break;
	case BIND_KIND_EXIT_SUBMAP:
		exit_submap();
		break;
	}
}

void execute_bell_bind(void) {
	if (bell_bind.kind != BIND_KIND_COMMAND || bell_bind.cmd[0] != '\0')
		execute_keybind(&bell_bind);
}

bool keybind_matches(const keybind_t *kb, const uint32_t modifiers, const xkb_keysym_t keysym,
		const uint32_t keycode) {
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
	const xkb_keysym_t lower = xkb_keysym_to_lower(keysym);
	return kb->keysym == lower;
}

static bool keybind_matches_now(const keybind_t *kb, const uint32_t modifiers,
		const xkb_keysym_t keysym, const uint32_t keycode, const bool prefer_keycode) {
	if (kb->use_keycode != prefer_keycode)
		return false;
	return keybind_matches(kb, modifiers, keysym, keycode);
}

// true for binds that only participate in release-triggered (or catchall) matching
static bool keybind_is_deferred(const keybind_t *kb) {
	return (kb->flags & (BINDF_RELEASE | BINDF_CATCH_ALL)) != 0;
}

keybind_t *find_keybind(const uint32_t modifiers, const xkb_keysym_t keysym, const uint32_t keycode,
		const bool prefer_keycode, const bool fallback_to_global) {
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

keybind_t *find_release_bind(const uint32_t modifiers, const xkb_keysym_t keysym,
		const uint32_t keycode, const char *submap_at_press) {
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

void enter_submap(const char *name) {
	submap_t *sm = find_submap_by_name(name);
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
	return settings.keyboard_grouping;
}

bool gesturebind_matches(const gesturebind_t *gb, const enum gesture_type type,
		const uint8_t fingers) {
	return !(!gb || gb->type != type || (gb->fingers != GESTURE_FINGERS_ANY &&
		gb->fingers != fingers));
}

void execute_gesturebind(const gesturebind_t *gb) {
	if (gb && gb->cmd[0] != '\0')
		execute_bind_command(gb->cmd);
}

bool hotcornerbind_matches(const hotcornerbind_t *hc, const int corner_x, const int corner_y) {
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
	if (hc && hc->cmd[0] != '\0')
		execute_bind_command(hc->cmd);
}
