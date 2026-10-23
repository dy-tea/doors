#pragma once

#include "ipc/args.h"
#include "types.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
	CFG_BOOL,
	CFG_INT,
	CFG_FLOAT,
	CFG_DOUBLE,
	CFG_ENUM,
	CFG_STR,
	CFG_RGBA,
} cfg_type_t;

// transaction_commit_dirty() after a successful write
#define CFG_COMMIT (1 << 0)

// clamp out-of-range values instead of rejecting them
#define CFG_CLAMP (1 << 1)

// value must be greater than min, with no upper bound
#define CFG_POSITIVE (1 << 2)

// both bounds are exclusive
#define CFG_EXCLUSIVE (1 << 3)

typedef struct cfg_setting {
	const char *name;
	const char *alias;
	cfg_type_t type;
	void *ptr;
	size_t size; // capacity for CFG_STR
	double min, max;
	const char *fmt; // get format for the numeric types
	const str_pair_t *values; // CFG_ENUM
	unsigned flags;
	void (*on_set)(void); // side effects of a successful write
} cfg_setting_t;

// room for the longest value a setting can print, or a default is "-"
#define CFG_VALUE_MAX 512

// settings_table plus the settings with their own syntax, and most targets a reset can name
#define CFG_INTRO_MAX 256

// every setting reachable through `doorsctl config <name>`
#define CFG_SETTINGS_COUNT 85

extern const cfg_setting_t settings_table[CFG_SETTINGS_COUNT];

const char *cfg_type_name(cfg_type_t type);

// the value as `doorsctl config <name>` would print it, with a trailing newline
void cfg_format_value(const cfg_setting_t *s, char *buf, size_t bufsz);

// what a setting accepts, e.g. "[0.5, 2]", "(0, 1)", ">0"
void cfg_format_range(const cfg_setting_t *s, char *buf, size_t bufsz);

void cfg_errf(char *err, size_t errsz, const char *fmt, ...);

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

void cfg_format_scroller_size(scroller_size_t size, ipc_buf_t *b, bool last);

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

// name a setting with its own syntax is listed and stored under
const char *cfg_special_canonical(const char *name);

bool cfg_special_settable(const char *name);
bool cfg_special_apply(const char *name, const char *value, char *err, size_t errsz, bool dry_run);

// rebuilds every tab bar, used by the handlers that change tab appearance
void tabs_rebuild_all(void);
