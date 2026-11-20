#include "ipc/args.h"
#include "ipc/helpers.h"
#include "parse.h"
#include "settings.h"
#include "str.h"
#include "transaction.h"
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// global settings
settings_t settings = {
	// layout behaviour
	.automatic_scheme = SCHEME_SPIRAL,
	.initial_polarity = FIRST_CHILD,
	.removal_adjustment = true,
	.focus_wrapping = true,
	.focus_on_activate = FOCUS_ON_ACTIVATE_FOCUS,
	.focus_follows_mouse = FOLLOWS_NO,
	.focus_follows_mouse_max_scroll = 0,
	.pointer_follows_focus = false,
	.split_ratio = 0.5,

	// gaps, borders and decoration
	.border_width = 2,
	.window_gap = 10,
	.smart_gaps = false,
	.smart_borders = false,
	.borderless_singleton = false,
	.respect_tiled_min_size = false,
	.hide_lone_tab = false,
	.decoration_mode = DECORATION_ALWAYS,

	// border colours
	.normal_border_color = "444444ff",
	.active_border_color = "555555ff",
	.focused_border_color = "1793dfff",
	.presel_feedback_color = "ff5555ff",
	.tiling_drag_indicator_color = "4d9eff4d",
	.normal_border_theme = {
		{0},
		0,
		0,
		{0},
		0,
		0,
		0
	},
	.active_border_theme = {
		{0},
		0,
		0,
		{0},
		0,
		0,
		0
	},
	.focused_border_theme = {
		{0},
		0,
		0,
		{0},
		0,
		0,
		0
	},

	// shadows
	.shadow_size = 8.0f,
	.shadow_offset_x = 0.0f,
	.shadow_offset_y = 4.0f,
	.shadow_color = {0.0f, 0.0f, 0.0f, 0.5f},

	// dimming
	.dim_inactive = false,
	.dim_below = false,
	.dim_strength = 0.1f,
	.dim_color = {0.0f, 0.0f, 0.0f, 1.0f},

	// animations
	.enable_animations = false,
	.workspace_anim_direction = WORKSPACE_ANIM_VERTICAL,
	.workspace_anim_slide_up = false,

	// blur
	.blur_algorithm = BLUR_ALGORITHM_KAWASE,
	.blur_enabled = true,
	.blur_passes = 1,
	.blur_radius = 5.0f,
	.blur_downsample = 4,
	.blur_full_res = true,
	.blur_offset = 3.0f,
	.blur_saturation = 1.5f,
	.blur_vibrancy = 0.0f,
	.blur_vibrancy_darkness = 0.5f,
	.blur_noise_strength = 0.0f,
	.blur_brightness = 1.0f,
	.blur_contrast = 1.0f,

	// refraction
	.refraction_strength = 30.0f,
	.refraction_edge_size_px = 18.0f,
	.refraction_corner_radius_px = 8.0f,
	.refraction_normal_pow = 6.0f,
	.refraction_rgb_fringing = 22.0f / 30.0f,
	.refraction_texture_repeat_mode = 1,
	.refraction_offset = 1.0f,

	// mica and acrylic
	.mica_enabled = false,
	.mica_tint = {0.12f, 0.12f, 0.14f, 1.0f},
	.mica_tint_strength = 0.35f,
	.acrylic_tint = {1.0f, 1.0f, 1.0f, 1.0f},
	.acrylic_tint_strength = 0.3f,
	.acrylic_noise_strength = 0.02f,
	.acrylic_light_anchor = {0.5f, 0.5f},
	.acrylic_blur_passes = 4,

	// screen shader
	.screen_shader_enabled = false,
	.screen_shader = "none",

	// text and tab bar
	.text_font = "Sans 10",
	.text_height = 18,
	.color_bar_bg = {0.10f, 0.10f, 0.12f, 1.0f},
	.color_tab_bg = {0.18f, 0.18f, 0.20f, 1.0f},
	.color_tab_bg_active = {0.09f, 0.58f, 0.87f, 1.0f},
	.color_tab_text = {0.85f, 0.85f, 0.88f, 1.0f},
	.color_tab_text_active = {1.0f, 1.0f, 1.0f, 1.0f},
	.color_tab_sep = {0.0f, 0.0f, 0.0f, 1.0f},

	// window state
	.enable_minimize = true,
	.enable_maximize = true,
	.minimize_to_scratchpad = false,
	.scratchpad_restore_to_origin = true,

	// input
	.keyboard_grouping = KEYBOARD_GROUP_DEFAULT,

	// idle and power
	.idle_timeout = 0,
	.idle_dpms = true,
	.realtime_scheduling = false,

	// transactions and debugging
	.txn_timeout_ms = 200,
	.debug_txn_timings = false,
	.debug_noatomic = false,
	.debug_txn_wait = false,

	// misc
	.record_history = true,
	.allow_tearing = false,
	.direct_scanout = false,
	.auto_float_dialogs = false,
	.ignore_ewmh_fullscreen = 0,
};

void refresh_border_color_cache(void) {
	parse_color_hex(settings.normal_border_color, settings.normal_border_color_rgba);
	parse_color_hex(settings.active_border_color, settings.active_border_color_rgba);
	parse_color_hex(settings.focused_border_color, settings.focused_border_color_rgba);
	parse_color_hex(settings.presel_feedback_color, settings.presel_feedback_color_rgba);
	parse_color_hex(settings.tiling_drag_indicator_color, settings.tiling_drag_indicator_color_rgba);
}

// global state
struct wl_list mon_list;
uint32_t next_node_id = 1;
uint32_t next_desktop_id = 1;
uint32_t next_monitor_id = 1;
struct wl_list orphan_desk_list;

const char *setting_type_name(const setting_type_t type) {
	static const char *const names[] = {
		[SETTING_BOOL] = "bool",
		[SETTING_INT] = "int",
		[SETTING_FLOAT] = "float",
		[SETTING_DOUBLE] = "double",
		[SETTING_ENUM] = "enum",
		[SETTING_STR] = "str",
		[SETTING_RGBA] = "rgba",
	};
	return names[type];
}

void setting_err(char *err, const size_t errsz, const char *fmt, ...) {
	if (err == NULL || errsz == 0)
		return;

	va_list ap;
	va_start(ap, fmt);
	vsnprintf(err, errsz, fmt, ap);
	va_end(ap);
}

void setting_format_value(const setting_t *s, const void *value, char *buf, const size_t bufsz) {
	switch (s->type) {
	case SETTING_BOOL:
		snprintf(buf, bufsz, "%s", str_bool_text(*(const bool *)value));
		break;
	case SETTING_INT:
		snprintf(buf, bufsz, s->fmt, *(const int *)value);
		break;
	case SETTING_FLOAT:
		snprintf(buf, bufsz, s->fmt, *(const float *)value);
		break;
	case SETTING_DOUBLE:
		snprintf(buf, bufsz, s->fmt, *(const double *)value);
		break;
	case SETTING_ENUM: {
		int v;
		memcpy(&v, value, sizeof(v));
		const char *name = ipc_enum_name(s->values, v);
		snprintf(buf, bufsz, "%s", name ? name : "?");
		break;
	}
	case SETTING_STR:
		snprintf(buf, bufsz, "%s", (const char *)value);
		break;
	case SETTING_RGBA:
		ipc_format_color_float(buf, bufsz, value);
		break;
	}
}

void setting_format(const setting_t *s, char *buf, const size_t bufsz) {
	setting_format_value(s, s->ptr, buf, bufsz);
}

void setting_range(const setting_t *s, char *buf, const size_t bufsz) {
	// integer bounds print exactly, floating ones keep their significant digits
	const char *num = s->type == SETTING_INT ? "%.0f" : "%g";

	if (s->type == SETTING_BOOL) {
		snprintf(buf, bufsz, "true|false");
		return;
	}

	if (s->type == SETTING_ENUM) {
		ipc_buf_t b;
		ipc_buf_init(&b, buf, bufsz);
		for (const str_pair_t *e = s->values; e->name; e++)
			ipc_buff_sep(&b, "|", "%s", e->name);
		return;
	}

	if (s->type == SETTING_STR) {
		snprintf(buf, bufsz, "string");
		return;
	}

	if (s->type == SETTING_RGBA) {
		snprintf(buf, bufsz, "\"r g b [a]\"");
		return;
	}

	if (s->flags & SETTING_POSITIVE) {
		snprintf(buf, bufsz, ">%g", s->min);
		return;
	}

	// the sentinel bounds used by unbounded integer settings
	if (s->type == SETTING_INT && s->min == INT_MIN && s->max == INT_MAX) {
		snprintf(buf, bufsz, "any");
		return;
	}

	const bool excl = (s->flags & SETTING_EXCLUSIVE) != 0;
	char lo[32], hi[32];

	snprintf(lo, sizeof(lo), num, s->min);
	snprintf(hi, sizeof(hi), num, s->max);
	snprintf(buf, bufsz, "%c%s, %s%c", excl ? '(' : '[', lo, hi, excl ? ')' : ']');
}

static bool setting_bool(const setting_t *s, const char *arg, char *err, size_t errsz,
		const bool dry_run) {
	bool v;
	if (!parse_bool(arg, &v)) {
		setting_err(err, errsz, "%s: expected true or false, got \"%s\"\n", s->name, arg);
		return false;
	}

	if (!dry_run)
		*(bool *)s->ptr = v;
	return true;
}

static void setting_store_enum(const setting_t *s, const long value) {
	assert(sizeof(int) == 4);
	int v = (int)value;
	memcpy(s->ptr, &v, sizeof(v));
}

static bool setting_enum(const setting_t *s, const char *arg, char *err, size_t errsz,
		const bool dry_run) {
	const str_pair_t *e = str_table_find(s->values, arg);
	if (e) {
		if (!dry_run)
			setting_store_enum(s, e->value);
		return true;
	}

	ipc_buf_t b;
	char list[256];
	ipc_buf_init(&b, list, sizeof(list));
	for (e = s->values; e->name; e++)
		ipc_buff_sep(&b, ", ", "\"%s\"", e->name);

	setting_err(err, errsz, "%s: expected value to be one of: %s\n", s->name, list);
	return false;
}

static bool setting_clamp_value(const setting_t *s, const double val, const double step, char *err,
		const size_t errsz, double *out) {
	if (!isfinite(val)) {
		setting_err(err, errsz, "%s: value must be a finite number\n", s->name);
		return false;
	}

	const bool positive = (s->flags & SETTING_POSITIVE) != 0;
	const bool lo_excl = (s->flags & (SETTING_POSITIVE | SETTING_EXCLUSIVE)) != 0;
	const bool hi_excl = (s->flags & SETTING_EXCLUSIVE) != 0;

	const bool below = lo_excl ? val <= s->min : val < s->min;
	const bool above = !positive && (hi_excl ? val >= s->max : val > s->max);

	if (!below && !above) {
		*out = val;
		return true;
	}

	if (!(s->flags & SETTING_CLAMP)) {
		if (below)
			setting_err(err, errsz, "%s: value must be greater than %g\n", s->name, s->min);
		else
			setting_err(err, errsz, "%s: value must be less than %g\n", s->name, s->max);
		return false;
	}

	// an exclusive bound clamps to the nearest value inside the range
	*out = below ? (lo_excl ? s->min + step : s->min) : (hi_excl ? s->max - step : s->max);
	return true;
}

static bool setting_int(const setting_t *s, const char *arg, char *err, const size_t errsz,
		const bool dry_run) {
	long val;
	if (!parse_long(arg, &val)) {
		setting_err(err, errsz, "%s: invalid value \"%s\"\n", s->name, arg);
		return false;
	}

	double out;
	if (!setting_clamp_value(s, (double)val, 1.0, err, errsz, &out))
		return false;

	if (!dry_run)
		*((int *)s->ptr) = (int)out;
	return true;
}

static bool setting_float(const setting_t *s, const char *arg, char *err, const size_t errsz,
		const bool dry_run) {
	double val;
	if (!parse_double(arg, &val)) {
		setting_err(err, errsz, "%s: invalid value \"%s\"\n", s->name, arg);
		return false;
	}

	double out;
	if (!setting_clamp_value(s, val, 0.0, err, errsz, &out))
		return false;

	if (!dry_run)
		*((float *)s->ptr) = (float)out;
	return true;
}

static bool setting_double(const setting_t *s, const char *arg, char *err, const size_t errsz,
		const bool dry_run) {
	double val;
	if (!parse_double(arg, &val)) {
		setting_err(err, errsz, "%s: invalid value \"%s\"\n", s->name, arg);
		return false;
	}

	double out;
	if (!setting_clamp_value(s, val, 0.0, err, errsz, &out))
		return false;

	if (!dry_run)
		*((double *)s->ptr) = out;
	return true;
}

static bool setting_str(const setting_t *s, const char *arg, char *err, const size_t errsz,
		const bool dry_run) {
	if (strlen(arg) >= s->size) {
		setting_err(err, errsz, "%s: value is too long\n", s->name);
		return false;
	}

	if (!dry_run)
		snprintf(s->ptr, s->size, "%s", arg);
	return true;
}

static bool setting_rgba(const setting_t *s, const char *arg, char *err, const size_t errsz,
		const bool dry_run) {
	float parsed[4];
	if (!parse_color_floats(arg, parsed)) {
		setting_err(err, errsz, "%s: expected \"R G B [A]\"\n", s->name);
		return false;
	}

	if (!dry_run)
		memcpy(s->ptr, parsed, sizeof(parsed));
	return true;
}

bool setting_apply(const setting_t *s, const char *value, char *err, const size_t errsz,
		const bool dry_run) {
	bool ok = true;

	switch (s->type) {
	case SETTING_BOOL:
		ok = setting_bool(s, value, err, errsz, dry_run);
		break;
	case SETTING_INT:
		ok = setting_int(s, value, err, errsz, dry_run);
		break;
	case SETTING_FLOAT:
		ok = setting_float(s, value, err, errsz, dry_run);
		break;
	case SETTING_DOUBLE:
		ok = setting_double(s, value, err, errsz, dry_run);
		break;
	case SETTING_ENUM:
		ok = setting_enum(s, value, err, errsz, dry_run);
		break;
	case SETTING_STR:
		ok = setting_str(s, value, err, errsz, dry_run);
		break;
	case SETTING_RGBA:
		ok = setting_rgba(s, value, err, errsz, dry_run);
		break;
	}

	if (!ok || dry_run)
		return ok;

	if (s->on_set)
		s->on_set();
	if (s->flags & SETTING_COMMIT)
		transaction_commit_dirty();

	return true;
}
