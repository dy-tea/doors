#pragma once

#include "input/gesture.h"
#include "macros.h"
#include "settings.h"

#include <stdint.h>
#include <wayland-server.h>
#include <xkbcommon/xkbcommon.h>

#define MAX_KEYBINDS 256
#define MAX_GESTUREBINDS 64
#define MAX_HOTCORNERBINDS 16

typedef struct submap submap_t;

typedef enum {
	BIND_KIND_COMMAND = 0,
	BIND_KIND_ENTER_SUBMAP,
	BIND_KIND_EXIT_SUBMAP,
} bind_kind_t;

typedef enum {
	BINDF_NONE = 0,
	BINDF_LOCKED = 1 << 0,
	BINDF_RELEASE = 1 << 1,
	BINDF_REPEAT = 1 << 2,
	BINDF_NON_CONSUMING = 1 << 3,
	BINDF_IGNORE_MODS = 1 << 4,
	BINDF_SUBMAP_UNIVERSAL = 1 << 5,
	BINDF_CATCH_ALL = 1 << 6,
} bind_flag_t;

#define MOUSE_BUTTON_SYNTH_KEYCODE_BASE 0x20000000
#define MOUSE_BUTTON_KEYCODE_OFFSET 272
#define mouse_button_to_keycode(button_index) \
	(MOUSE_BUTTON_SYNTH_KEYCODE_BASE + ((button_index) + MOUSE_BUTTON_KEYCODE_OFFSET))
#define keycode_to_mouse_button_index(keycode) \
	((int)(keycode) - MOUSE_BUTTON_SYNTH_KEYCODE_BASE - MOUSE_BUTTON_KEYCODE_OFFSET)

typedef struct {
	uint32_t modifiers;
	xkb_keysym_t keysym;
	uint32_t keycode;
	bool use_keycode;
	bind_kind_t kind;
	uint32_t flags; // bind_flag_t bitmask
	char submap_name[MAXLEN];
	char cmd[MAXLEN];
} keybind_t;

#define bind_keycode_is_mouse(keycode) ((keycode) >= MOUSE_BUTTON_SYNTH_KEYCODE_BASE)

typedef struct bind_event_result_t {
	bool consumed, forward;
	keybind_t *bind;
} bind_event_result_t;

typedef struct {
	enum gesture_type type;
	uint8_t fingers;
	uint32_t directions;
	char *input;
	char cmd[MAXLEN];
} gesturebind_t;

typedef struct {
	enum hotcorner corner;
	int corner_x, corner_y;
	char cmd[MAXLEN];
} hotcornerbind_t;

struct submap {
	char name[MAXLEN * 2];
	keybind_t keybinds[MAX_KEYBINDS];
	size_t num_keybinds;
	submap_t *parent;
};

extern keybind_t keybinds[MAX_KEYBINDS];
extern size_t num_keybinds;
extern keybind_t bell_bind;
extern gesturebind_t gesture_bindings[MAX_GESTUREBINDS];
extern size_t num_gesturebinds;
extern hotcornerbind_t hotcorner_bindings[MAX_HOTCORNERBINDS];
extern size_t num_hotcornerbinds;
extern submap_t *active_submap;

void config_init_with_config_dir(const char *config_dir);
void config_fini(void);
void run_config(const char *config_path);
void run_config_idle(void *data);
void load_hotkeys_idle(void *data);
void load_hotkeys(const char *path);
void reload_hotkeys(void);
bool keybind_matches(const keybind_t *kb, uint32_t modifiers, xkb_keysym_t keysym,
	uint32_t keycode);
keybind_t *find_keybind(uint32_t modifiers, xkb_keysym_t keysym, uint32_t keycode,
	bool prefer_keycode, bool fallback_to_global);
void execute_keybind(const keybind_t *kb);
void execute_bell_bind(void);
void setup_hotkey_event_listener(struct wl_event_loop *event_loop);
void enter_submap(const char *name);
void exit_submap(void);

keyboard_grouping_t get_keyboard_grouping(void);

bool gesturebind_matches(const gesturebind_t *gb, enum gesture_type type, uint8_t fingers);
void execute_gesturebind(const gesturebind_t *gb);

bool hotcornerbind_matches(const hotcornerbind_t *hc, int corner_x, int corner_y);
hotcornerbind_t *hotcorner_bind_match(int corner_x, int corner_y);
void execute_hotcornerbind(const hotcornerbind_t *hc);

bool parse_bind_flags(const char *str, uint32_t *out_flags);
char *bind_flags_text(uint32_t flags, char *buf, size_t size);

// parses a "<modifiers>+ <key>[ [flags]]> " hotkey specification.
const char *parse_hotkey_spec(const char *spec, uint32_t *modifiers, xkb_keysym_t *keysym,
	uint32_t *keycode, bool *use_keycode, uint32_t *flags);

// classifies a hotkey command, filling submap_name for BIND_KIND_ENTER_SUBMAP
bind_kind_t parse_bind_kind(const char *cmd, char *submap_name);

uint32_t parse_modifiers(const char *mod_str);
xkb_keysym_t parse_keysym(const char *name);
uint32_t parse_keycode(const char *name);
void add_keybind(uint32_t modifiers, xkb_keysym_t keysym, uint32_t keycode, bool use_keycode,
	uint32_t flags, bind_kind_t kind, const char *submap_name, const char *cmd);
submap_t *find_submap_by_name(const char *name);
keybind_t *find_release_bind(uint32_t modifiers, xkb_keysym_t keysym, uint32_t keycode,
	const char *submap_at_press);
