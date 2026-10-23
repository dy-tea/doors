#include "animation.h"
#include "config_internal.h"
#include "effects/effects.h"
#include "ipc/helpers.h"
#include "settings.h"
#include "tabs.h"
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CFG_INTRO_MAX 256
#define CFG_DEFAULTS_ARENA (32 * 1024)

// setting the introspection commands can report on
typedef struct cfg_intro {
	const char *name;
	const char *type;
	const char *range; // accepted values, or NULL when there is nothing to say
	void (*format)(const void *self, char *buf, size_t bufsz);
	const void *self;
} cfg_intro_t;

// setting handled outside settings_table, reported by name only
typedef struct cfg_extra {
	const char *name;
	const char *type;
	const char *range;
	void (*format)(const struct cfg_extra *e, char *buf, size_t bufsz);
	const void *ref; // what format reads, NULL when unused
} cfg_extra_t;

const char *cfg_type_name(cfg_type_t type) {
	switch (type) {
	case CFG_BOOL:
		return "bool";
	case CFG_INT:
		return "int";
	case CFG_FLOAT:
		return "float";
	case CFG_DOUBLE:
		return "double";
	case CFG_ENUM:
		return "enum";
	case CFG_STR:
		return "str";
	case CFG_RGBA:
		return "rgba";
	}
	return "?";
}

static void cfg_intro_format_setting(const void *self, char *buf, size_t bufsz) {
	cfg_format_value(self, buf, bufsz);
	str_rtrim(buf);
}

// describes what a numeric setting accepts, e.g. "[0.5, 2]", "(0, 1)", ">0"
void cfg_format_range(const cfg_setting_t *s, char *buf, size_t bufsz) {
	// integer bounds print exactly, floating ones keep their significant digits
	const char *num = s->type == CFG_INT ? "%.0f" : "%g";

	if (s->type == CFG_BOOL) {
		snprintf(buf, bufsz, "true|false");
		return;
	}

	if (s->type == CFG_ENUM) {
		ipc_buf_t b;
		ipc_buf_init(&b, buf, bufsz);
		for (const str_pair_t *e = s->values; e->name; e++)
			ipc_buff_sep(&b, "|", "%s", e->name);
		return;
	}

	if (s->type == CFG_STR) {
		snprintf(buf, bufsz, "string");
		return;
	}

	if (s->type == CFG_RGBA) {
		snprintf(buf, bufsz, "\"r g b [a]\"");
		return;
	}

	if (s->flags & CFG_POSITIVE) {
		snprintf(buf, bufsz, ">%g", s->min);
		return;
	}

	// the sentinel bounds used by unbounded integer settings
	if (s->type == CFG_INT && s->min == INT_MIN && s->max == INT_MAX) {
		snprintf(buf, bufsz, "any");
		return;
	}

	bool lo_excl = (s->flags & CFG_EXCLUSIVE) != 0;
	bool hi_excl = (s->flags & CFG_EXCLUSIVE) != 0;
	char lo[32], hi[32];

	snprintf(lo, sizeof(lo), num, s->min);
	snprintf(hi, sizeof(hi), num, s->max);
	snprintf(buf, bufsz, "%c%s, %s%c", lo_excl ? '(' : '[', lo, hi, hi_excl ? ')' : ']');
}

static void cfg_extra_gradient(const cfg_extra_t *e, char *buf, size_t bufsz) {
	static border_theme_t *const themes[] = {
		[GRAD_NORMAL] = &settings.normal_border_theme,
		[GRAD_ACTIVE] = &settings.active_border_theme,
		[GRAD_FOCUSED] = &settings.focused_border_theme,
	};

	const cfg_gradient_t *g = cfg_find_gradient(e->name);
	if (g == NULL) {
		snprintf(buf, bufsz, "?");
		return;
	}

	const border_theme_t *bt = themes[g->theme];

	if (g->lerp) {
		snprintf(buf, bufsz, "%.3f", bt->gradient_lerp);
		return;
	}

	ipc_format_gradient(buf, bufsz, g->second ? bt->gradient2 : bt->gradient,
		g->second ? bt->gradient2_count : bt->gradient_count,
		g->second ? bt->gradient2_angle : bt->gradient_angle);
}

static void cfg_extra_vec2(const cfg_extra_t *e, char *buf, size_t bufsz) {
	const float *v = e->ref;
	snprintf(buf, bufsz, "%.3f %.3f", v[0], v[1]);
}

static void cfg_extra_blur_algorithm(const cfg_extra_t *e, char *buf, size_t bufsz) {
	(void)e;

	snprintf(buf, bufsz, "%s", effects_algorithm_to_str(blur_algorithm));
}

static void cfg_extra_screen_shader(const cfg_extra_t *e, char *buf, size_t bufsz) {
	(void)e;

	snprintf(buf, bufsz, "%s", screen_shader_get_name());
}

static void cfg_extra_scroller_widths(const cfg_extra_t *e, char *buf, size_t bufsz) {
	(void)e;

	ipc_buf_t b;
	ipc_buf_init(&b, buf, bufsz);
	int count = settings.scroller_preset_column_widths_count;
	for (int i = 0; i < count; i++)
		cfg_format_scroller_size(settings.scroller_preset_column_widths[i], &b, i == count - 1);
}

static void cfg_extra_scroller_heights(const cfg_extra_t *e, char *buf, size_t bufsz) {
	(void)e;

	ipc_buf_t b;
	ipc_buf_init(&b, buf, bufsz);
	int count = settings.scroller_preset_window_heights_count;
	for (int i = 0; i < count; i++)
		cfg_format_scroller_size(settings.scroller_preset_window_heights[i], &b, i == count - 1);
}

static void cfg_extra_scroller_default_width(const cfg_extra_t *e, char *buf, size_t bufsz) {
	(void)e;

	ipc_buf_t b;
	ipc_buf_init(&b, buf, bufsz);
	cfg_format_scroller_size(settings.scroller_default_column_width, &b, true);
}

static void cfg_extra_animation_bezier(const cfg_extra_t *e, char *buf, size_t bufsz) {
	(void)e;

	snprintf(buf, bufsz, "%s", animation_get_bezier());
}

static void cfg_extra_animation_duration(const cfg_extra_t *e, char *buf, size_t bufsz) {
	(void)e;

	snprintf(buf, bufsz, "%u", animation_get_duration());
}


#define CFG_GRADIENT_ENTRY(n) {n, "gradient", "#rrggbb... [angle]", cfg_extra_gradient, NULL}
#define CFG_GRADIENT_LERP_ENTRY(n) {n, "float", "[0, 1]", cfg_extra_gradient, NULL}
#define CFG_COLOR_ENTRY(n, c) {n, "rgba", "\"r g b [a]\"", NULL, c}

static const cfg_extra_t cfg_extras[] = {
	// tab bar colours
	CFG_COLOR_ENTRY("tab_color_bar_bg", color_bar_bg),
	CFG_COLOR_ENTRY("tab_color_bg", color_tab_bg),
	CFG_COLOR_ENTRY("tab_color_bg_active", color_tab_bg_active),
	CFG_COLOR_ENTRY("tab_color_text", color_tab_text),
	CFG_COLOR_ENTRY("tab_color_text_active", color_tab_text_active),
	CFG_COLOR_ENTRY("tab_color_sep", color_tab_sep),

	// border gradients
	CFG_GRADIENT_ENTRY("normal_border_gradient"),
	CFG_GRADIENT_ENTRY("active_border_gradient"),
	CFG_GRADIENT_ENTRY("focused_border_gradient"),
	CFG_GRADIENT_ENTRY("normal_border_gradient2"),
	CFG_GRADIENT_ENTRY("active_border_gradient2"),
	CFG_GRADIENT_ENTRY("focused_border_gradient2"),
	CFG_GRADIENT_LERP_ENTRY("normal_border_gradient_lerp"),
	CFG_GRADIENT_LERP_ENTRY("active_border_gradient_lerp"),
	CFG_GRADIENT_LERP_ENTRY("focused_border_gradient_lerp"),

	// scroller presets
	{
		"scroller_preset_column_widths", "list", "proportion|px, ...", cfg_extra_scroller_widths, NULL
	},
	{"scroller_preset_window_heights", "list", "proportion|px, ...", cfg_extra_scroller_heights, NULL},
	{"scroller_default_column_width", "size", "proportion|px", cfg_extra_scroller_default_width, NULL},

	// materials and effects
	{
		"acrylic_light_anchor", "vec2", "-1 .. 1", cfg_extra_vec2, acrylic_light_anchor
	},
	{"blur_algorithm", "enum", "none|kawase|gaussian|box|refraction|lens_refraction",
		cfg_extra_blur_algorithm, NULL},
	{"screen_shader", "enum", "none|grayscale|invert|sepia|nightlight", cfg_extra_screen_shader, NULL},

	// animation
	{
		"animation_bezier", "curve", "registered bezier curve", cfg_extra_animation_bezier, NULL
	},
	{"animation_duration", "int", "ms", cfg_extra_animation_duration, NULL},
};

// a colour is the common case, so entries without their own reader are assumed to hold one
static void cfg_extra_format(const cfg_extra_t *e, char *buf, size_t bufsz) {
	if (e->format) {
		e->format(e, buf, bufsz);
		return;
	}

	ipc_format_color_float(buf, bufsz, e->ref);
}

static void cfg_intro_format_extra(const void *self, char *buf, size_t bufsz) {
	cfg_extra_format(self, buf, bufsz);
	str_rtrim(buf);
}

_Static_assert(CFG_SETTINGS_COUNT + IPC_ARRAY_LEN(cfg_extras) <= CFG_INTRO_MAX,
	"CFG_INTRO_MAX is too small for the config surface");

static size_t cfg_intro_count(void) {
	return CFG_SETTINGS_COUNT + IPC_ARRAY_LEN(cfg_extras);
}

static void cfg_intro_get(size_t i, cfg_intro_t *out) {
	if (i < CFG_SETTINGS_COUNT) {
		const cfg_setting_t *s = &settings_table[i];
		out->name = s->name;
		out->type = cfg_type_name(s->type);
		out->range = NULL;
		out->format = cfg_intro_format_setting;
		out->self = s;
		return;
	}

	const cfg_extra_t *e = &cfg_extras[i - CFG_SETTINGS_COUNT];
	out->name = e->name;
	out->type = e->type;
	out->range = e->range;
	out->format = cfg_intro_format_extra;
	out->self = e;
}

static char cfg_defaults_arena[CFG_DEFAULTS_ARENA];
static size_t cfg_defaults_len;
static size_t cfg_defaults_at[CFG_INTRO_MAX];
static bool cfg_defaults_ready;

void config_snapshot_defaults(void) {
	if (cfg_defaults_ready)
		return;
	cfg_defaults_ready = true;

	size_t count = cfg_intro_count();

	for (size_t i = 0; i < count && i < CFG_INTRO_MAX; i++) {
		cfg_intro_t in;
		cfg_intro_get(i, &in);

		char buf[CFG_VALUE_MAX];
		in.format(in.self, buf, sizeof(buf));

		size_t len = strlen(buf);
		if (cfg_defaults_len + len + 1 > sizeof(cfg_defaults_arena)) {
			cfg_defaults_at[i] = SIZE_MAX;
			continue;
		}

		memcpy(cfg_defaults_arena + cfg_defaults_len, buf, len + 1);
		cfg_defaults_at[i] = cfg_defaults_len;
		cfg_defaults_len += len + 1;
	}
}

static const char *cfg_intro_default(size_t i) {
	if (!cfg_defaults_ready || i >= CFG_INTRO_MAX || cfg_defaults_at[i] == SIZE_MAX)
		return "-";
	return cfg_defaults_arena + cfg_defaults_at[i];
}

bool cfg_default_value(const char *name, char *buf, size_t bufsz) {
	if (!cfg_defaults_ready)
		return false;

	for (size_t i = 0; i < CFG_SETTINGS_COUNT; i++) {
		const cfg_setting_t *s = &settings_table[i];
		if (!streq(name, s->name) && !(s->alias && streq(name, s->alias)))
			continue;

		if (cfg_defaults_at[i] == SIZE_MAX)
			return false;

		snprintf(buf, bufsz, "%s", cfg_defaults_arena + cfg_defaults_at[i]);
		return true;
	}

	return false;
}

// how many of the settings with their own syntax (animation, gradients, tab
// colours, scroller presets) differ from the value recorded at startup, since
// `config reset` cannot restore those and should not claim otherwise
size_t cfg_unresettable_changed(void) {
	size_t changed = 0;

	for (size_t i = CFG_SETTINGS_COUNT; i < cfg_intro_count(); i++) {
		cfg_intro_t in;
		cfg_intro_get(i, &in);

		char value[CFG_VALUE_MAX];
		in.format(in.self, value, sizeof(value));

		if (!streq(value, cfg_intro_default(i)))
			changed++;
	}

	return changed;
}


#define CFG_COL_NAME 34
#define CFG_COL_TYPE 10
#define CFG_COL_VALUE 26

static void cfg_json_string(ipc_buf_t *b, const char *s) {
	ipc_buff(b, "\"");

	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		switch (*p) {
		case '"':
			ipc_buff(b, "\\\"");
			break;
		case '\\':
			ipc_buff(b, "\\\\");
			break;
		case '\n':
			ipc_buff(b, "\\n");
			break;
		case '\r':
			ipc_buff(b, "\\r");
			break;
		case '\t':
			ipc_buff(b, "\\t");
			break;
		default:
			if (*p < 0x20)
				ipc_buff(b, "\\u%04x", *p);
			else
				ipc_buff(b, "%c", *p);
		}
	}

	ipc_buff(b, "\"");
}

// right pads `s` to `width`, leaving longer values alone
static void cfg_column(ipc_buf_t *b, const char *s, int width) {
	ipc_buff(b, "%-*s", width, s);
}

static void cfg_list_json(ipc_args_t *a, bool changed) {
	size_t cap = 64 * 1024;
	char *storage = malloc(cap);

	if (!storage) {
		ipc_fail(a, "Memory allocation failed\n");
		return;
	}

	ipc_buf_t b;
	ipc_buf_init(&b, storage, cap);
	ipc_buff(&b, "[\n");

	bool first = true;
	size_t count = cfg_intro_count();

	for (size_t i = 0; i < count; i++) {
		cfg_intro_t in;
		cfg_intro_get(i, &in);

		char value[CFG_VALUE_MAX];
		in.format(in.self, value, sizeof(value));

		const char *dflt = cfg_intro_default(i);
		if (changed && streq(value, dflt))
			continue;

		char range[256] = {0};
		if (!in.range) {
			cfg_setting_t *s = (cfg_setting_t *)in.self;
			if (i < CFG_SETTINGS_COUNT)
				cfg_format_range(s, range, sizeof(range));
			else
				range[0] = '\0';
		} else {
			snprintf(range, sizeof(range), "%s", in.range);
		}

		if (!first)
			ipc_buff(&b, ",\n");
		first = false;

		ipc_buff(&b, "  {\"name\": ");
		cfg_json_string(&b, in.name);
		ipc_buff(&b, ", \"type\": ");
		cfg_json_string(&b, in.type);
		ipc_buff(&b, ", \"value\": ");
		cfg_json_string(&b, value);
		ipc_buff(&b, ", \"default\": ");
		cfg_json_string(&b, dflt);
		ipc_buff(&b, ", \"range\": ");
		cfg_json_string(&b, range);
		ipc_buff(&b, "}");
	}

	ipc_buff(&b, "%s]\n", first ? "" : "\n");

	if (b.truncated)
		ipc_fail(a, "Listing did not fit in the reply buffer\n");
	else
		ipc_buf_send(a, &b);

	free(storage);
}

static void cfg_list_text(ipc_args_t *a, bool changed, bool header) {
	size_t cap = 64 * 1024;
	char *storage = malloc(cap);

	if (!storage) {
		ipc_fail(a, "Memory allocation failed\n");
		return;
	}

	ipc_buf_t b;
	ipc_buf_init(&b, storage, cap);

	if (header) {
		cfg_column(&b, "NAME", CFG_COL_NAME);
		cfg_column(&b, "TYPE", CFG_COL_TYPE);
		cfg_column(&b, "VALUE", CFG_COL_VALUE);
		cfg_column(&b, "DEFAULT", CFG_COL_VALUE);
		ipc_buff(&b, "RANGE\n");
	}

	size_t count = cfg_intro_count();

	for (size_t i = 0; i < count; i++) {
		cfg_intro_t in;
		cfg_intro_get(i, &in);

		char value[CFG_VALUE_MAX];
		in.format(in.self, value, sizeof(value));

		const char *dflt = cfg_intro_default(i);
		if (changed && streq(value, dflt))
			continue;

		char range[256] = {0};
		if (!in.range) {
			if (i < CFG_SETTINGS_COUNT)
				cfg_format_range((cfg_setting_t *)in.self, range, sizeof(range));
		} else {
			snprintf(range, sizeof(range), "%s", in.range);
		}

		cfg_column(&b, in.name, CFG_COL_NAME);
		cfg_column(&b, in.type, CFG_COL_TYPE);
		cfg_column(&b, value, CFG_COL_VALUE);
		cfg_column(&b, dflt, CFG_COL_VALUE);
		ipc_buff(&b, "%s\n", range);
	}

	if (b.truncated)
		ipc_fail(a, "Listing did not fit in the reply buffer\n");
	else
		ipc_buf_send(a, &b);

	free(storage);
}

// doorsctl config --list [--json] [--changed] [--no-header]
void cfg_list(ipc_args_t *a) {
	bool json = false, changed = false, header = true;

	ipc_foreach(a, arg) {
		if (streq(arg, "--json"))
			json = true;
		else if (streq(arg, "--changed"))
			changed = true;
		else if (streq(arg, "--no-header"))
			header = false;
		else {
			ipc_fail(a, "Unknown option \"%s\"\n", arg);
			return;
		}
	}

	if (json)
		cfg_list_json(a, changed);
	else
		cfg_list_text(a, changed, header);
}
