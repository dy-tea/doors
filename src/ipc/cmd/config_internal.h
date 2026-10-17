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
	const cfg_enum_value_t *values; // CFG_ENUM
	unsigned flags;
	void (*on_set)(void); // side effects of a successful write
} cfg_setting_t;

// every setting reachable through `doorsctl config <name>`
#define CFG_SETTINGS_COUNT 83

extern const cfg_setting_t settings_table[];

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

void cfg_list(ipc_args_t *a);

// rebuilds every tab bar, used by the handlers that change tab appearance
void tabs_rebuild_all(void);
