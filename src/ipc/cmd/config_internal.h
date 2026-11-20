#pragma once

#include "ipc/args.h"
#include "settings.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

// room for the longest value a setting can print, or a default is "-"
#define CFG_VALUE_MAX 512

// settings_table plus the settings with their own syntax, and most targets a reset can name
#define CFG_INTRO_MAX 256

// every global setting reachable through `doorsctl config <name>`
#define CFG_SETTINGS_COUNT 74

extern const setting_t settings_table[CFG_SETTINGS_COUNT];

typedef enum {
	GRAD_NORMAL,
	GRAD_ACTIVE,
	GRAD_FOCUSED,
} grad_theme_t;

typedef struct {
	const char *name;
	grad_theme_t theme;
	bool second; // gradient2 rather than gradient
	bool lerp; // lerp factor rather than colour stops
} cfg_gradient_t;

// NULL when the name is not a border gradient setting
const cfg_gradient_t *cfg_find_gradient(const char *name);

// handles the settings that need their own syntax, returns false when the name
// is not one of them
bool cfg_special_dispatch(ipc_args_t *a, const char *name);

// whether the name is handled by cfg_special_dispatch
bool cfg_special_name(const char *name);

void cfg_list(ipc_args_t *a);

// compiled default of a setting
bool cfg_default_value(const char *name, char *buf, size_t bufsz);

// settings with their own syntax
size_t cfg_extra_count(void);
const char *cfg_extra_name(size_t i);

// current value of a setting with its own syntax
bool cfg_special_format_value(const char *name, char *buf, size_t bufsz);

bool cfg_special_settable(const char *name);
bool cfg_special_apply(const char *name, const char *value, char *err, size_t errsz, bool dry_run);

// rebuilds every tab bar, used by the handlers that change tab appearance
void tabs_rebuild_all(void);
