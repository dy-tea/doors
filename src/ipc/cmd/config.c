#include "animation.h"
#include "config_internal.h"
#include "dim.h"
#include "effects/effects.h"
#include "ipc/args.h"
#include "ipc/helpers.h"
#include "ipc/registry.h"
#include "layout/scroller.h"
#include "output/output.h"
#include "protocol/idle_power.h"
#include "protocol/xdg_toplevel.h"
#include "realtime.h"
#include "server.h"
#include "tabs.h"
#include "text.h"
#include "transaction.h"
#include "tree.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void tabs_rebuild_all(void) {
	output_t *m;
	wl_list_for_each(m, &mon_list, link) {
		desktop_t *d = m->desk;
		while (d) {
			if (d->root)
				tabs_rebuild(d->root);
			if (d->link.next == &m->desk_list)
				break;
			d = wl_container_of(d->link.next, d, link);
		}
	}
}

static void on_border_color(void) {
	refresh_border_colors();
}

static void on_window_gap(void) {
	output_t *m;
	wl_list_for_each(m, &mon_list, link) {
		desktop_t *d;
		wl_list_for_each(d, &m->desk_list, link)
			d->window_gap = settings.window_gap;
	}
}

static void on_decoration_mode(void) {
	tabs_rebuild_all();

	// refresh decor
	view_t *tl;
	wl_list_for_each(tl, &server.views, link)
		tl->impl->set_decorations(tl);
}

static void on_text(void) {
	tabs_rebuild_all();
}

static void on_blur_downsample(void) {
	output_t *m;
	wl_list_for_each(m, &mon_list, link) {
		if (m && m->effects)
			effects_output_resize(m->effects, m->width, m->height, m);
	}
}

static void on_mica(void) {
	output_t *m;
	wl_list_for_each(m, &mon_list, link)
		effects_invalidate_mica(m->effects);
}

static void on_screen_shader_enabled(void) {
	if (!screen_shader_enabled)
		screen_shader_hide_nodes();
}

static void on_realtime_scheduling(void) {
	if (settings.realtime_scheduling)
		set_rr_scheduling();
}

static void on_idle(void) {
	idle_power_reset_timer();
}

static void on_enable_minimize(void) {
	xdg_toplevel_refresh_capabilities();
}

static void on_enable_maximize(void) {
	xdg_toplevel_refresh_capabilities();
}

static void on_dim(void) {
	dim_update();
	output_t *m;
	wl_list_for_each(m, &mon_list, link)
		output_schedule_frame(m);
}

static const cfg_enum_value_t focus_on_activate_values[] = {
	{"focus", FOCUS_ON_ACTIVATE_FOCUS},
	{"none", FOCUS_ON_ACTIVATE_NONE},
	{"smart", FOCUS_ON_ACTIVATE_SMART},
	{"urgent", FOCUS_ON_ACTIVATE_URGENT},
	IPC_ENUM_END,
};

static const cfg_enum_value_t decoration_mode_values[] = {
	{"none", DECORATION_NONE},
	{"tabs", DECORATION_TABS},
	{"always", DECORATION_ALWAYS},
	{"csd", DECORATION_CSD},
	IPC_ENUM_END,
};

static const cfg_enum_value_t focus_follows_pointer_values[] = {
	{"no", FOLLOWS_NO},
	{"false", FOLLOWS_NO},
	{"yes", FOLLOWS_YES},
	{"true", FOLLOWS_YES},
	{"always", FOLLOWS_ALWAYS},
	IPC_ENUM_END,
};

static const cfg_enum_value_t workspace_anim_direction_values[] = {
	{"vertical", WORKSPACE_ANIM_VERTICAL},
	{"horizontal", WORKSPACE_ANIM_HORIZONTAL},
	IPC_ENUM_END,
};

static const cfg_enum_value_t automatic_scheme_values[] = {
	{"longest_side", SCHEME_LONGEST_SIDE},
	{"longest-side", SCHEME_LONGEST_SIDE},
	{"alternate", SCHEME_ALTERNATE},
	{"spiral", SCHEME_SPIRAL},
	IPC_ENUM_END,
};

static const cfg_enum_value_t initial_polarity_values[] = {
	{"first_child", FIRST_CHILD},
	{"first-child", FIRST_CHILD},
	{"second_child", SECOND_CHILD},
	{"second-child", SECOND_CHILD},
	IPC_ENUM_END,
};

static const cfg_enum_value_t scroller_center_mode_values[] = {
	{"never", SCROLLER_CENTER_NEVER},
	{"always", SCROLLER_CENTER_ALWAYS},
	{"on-overflow", SCROLLER_CENTER_ON_OVERFLOW},
	{"on_overflow", SCROLLER_CENTER_ON_OVERFLOW},
	IPC_ENUM_END,
};

#define B(n, a, p, fl, on) {n, a, CFG_BOOL, &(p), 0, 0, 0, NULL, NULL, fl, on}
#define I(n, a, p, lo, hi, f, fl, on) \
	{n, a, CFG_INT, &(p), 0, lo, hi, f, NULL, fl, on}
#define F(n, a, p, lo, hi, f, fl, on) \
	{n, a, CFG_FLOAT, &(p), 0, lo, hi, f, NULL, fl, on}
#define D(n, a, p, lo, hi, f, fl, on) \
	{n, a, CFG_DOUBLE, &(p), 0, lo, hi, f, NULL, fl, on}
#define E(n, a, p, vals, fl, on) {n, a, CFG_ENUM, &(p), 0, 0, 0, NULL, vals, fl, on}
#define S(n, a, p, sz, fl, on) {n, a, CFG_STR, (p), sz, 0, 0, NULL, NULL, fl, on}
#define C(n, a, p, fl, on) {n, a, CFG_RGBA, (p), 0, 0, 0, NULL, NULL, fl, on}

const cfg_setting_t settings_table[] = {
	/* geometry */
	I("border_width", NULL, settings.border_width, INT_MIN, INT_MAX, "%d\n", CFG_COMMIT, NULL),
	I("window_gap", NULL, settings.window_gap, INT_MIN, INT_MAX, "%d\n", CFG_COMMIT, on_window_gap),

	/* layout behaviour */
	B("borderless_monocle", NULL, settings.borderless_monocle, CFG_COMMIT, NULL),
	B("borderless_singleton", NULL, settings.borderless_singleton, CFG_COMMIT, NULL),
	B("smart_gaps", NULL, settings.smart_gaps, CFG_COMMIT, NULL),
	B("smart_borders", NULL, settings.smart_borders, CFG_COMMIT, NULL),
	B("respect_tiled_min_size", NULL, settings.respect_tiled_min_size, CFG_COMMIT, NULL),
	B("focus_wrapping", NULL, settings.focus_wrapping, CFG_COMMIT, NULL),
	B("hide_lone_tab", NULL, settings.hide_lone_tab, CFG_COMMIT, NULL),
	B("gapless_monocle", NULL, settings.gapless_monocle, CFG_COMMIT, NULL),
	I("monocle_top_padding", NULL, settings.monocle_padding.top, 0, INT_MAX, "%d\n", CFG_COMMIT, NULL),
	I("monocle_right_padding", NULL, settings.monocle_padding.right, 0, INT_MAX, "%d\n", CFG_COMMIT,
		NULL),
	I("monocle_bottom_padding", NULL, settings.monocle_padding.bottom, 0, INT_MAX, "%d\n", CFG_COMMIT,
		NULL),
	I("monocle_left_padding", NULL, settings.monocle_padding.left, 0, INT_MAX, "%d\n", CFG_COMMIT,
		NULL),
	B("pointer_follows_focus", NULL, settings.pointer_follows_focus, CFG_COMMIT, NULL),
	B("record_history", NULL, settings.record_history, 0, NULL),
	B("allow_tearing", NULL, settings.allow_tearing, CFG_COMMIT, NULL),
	B("auto_float_dialogs", NULL, settings.auto_float_dialogs, 0, NULL),
	B("enable_minimize", NULL, settings.enable_minimize, 0, on_enable_minimize),
	B("enable_maximize", NULL, settings.enable_maximize, 0, on_enable_maximize),
	B("minimize_to_scratchpad", NULL, settings.minimize_to_scratchpad, 0, NULL),
	B("scratchpad_restore_to_origin", NULL, settings.scratchpad_restore_to_origin, 0, NULL),
	D("split_ratio", NULL, settings.split_ratio, 0, 1, "%f\n", CFG_COMMIT | CFG_EXCLUSIVE, NULL),
	E("focus_on_activate", NULL, settings.focus_on_activate, focus_on_activate_values, CFG_COMMIT,
		NULL),
	E("decoration_mode", NULL, settings.decoration_mode, decoration_mode_values, CFG_COMMIT,
		on_decoration_mode),
	E("automatic_scheme", NULL, settings.automatic_scheme, automatic_scheme_values, CFG_COMMIT, NULL),
	E("initial_polarity", NULL, settings.initial_polarity, initial_polarity_values, 0, NULL),
	E("focus_follows_pointer", "focus_follows_mouse", settings.focus_follows_mouse,
		focus_follows_pointer_values, CFG_COMMIT, NULL),
	E("workspace_anim_direction", NULL, settings.workspace_anim_direction,
		workspace_anim_direction_values, 0, NULL),
	B("enable_animations", NULL, settings.enable_animations, 0, NULL),
	B("workspace_anim_slide_up", NULL, settings.workspace_anim_slide_up, 0, NULL),
	I("ignore_ewmh_fullscreen", NULL, settings.ignore_ewmh_fullscreen, 0, 2, "%d\n", 0, NULL),

	/* dimming */
	B("dim_inactive", NULL, settings.dim_inactive, 0, on_dim),
	B("dim_below", NULL, settings.dim_below, 0, on_dim),
	F("dim_strength", NULL, settings.dim_strength, 0.0, 1.0, "%.3f\n", 0, on_dim),
	C("dim_color", NULL, settings.dim_color, 0, on_dim),

	/* borders */
	S("normal_border_color", NULL, settings.normal_border_color, sizeof(settings.normal_border_color),
		CFG_COMMIT, on_border_color),
	S("active_border_color", NULL, settings.active_border_color, sizeof(settings.active_border_color),
		CFG_COMMIT, on_border_color),
	S("focused_border_color", NULL, settings.focused_border_color,
		sizeof(settings.focused_border_color), CFG_COMMIT, on_border_color),
	S("presel_feedback_color", NULL, settings.presel_feedback_color,
		sizeof(settings.presel_feedback_color), CFG_COMMIT, on_border_color),
	S("tiling_drag_indicator_color", NULL, settings.tiling_drag_indicator_color,
		sizeof(settings.tiling_drag_indicator_color), 0, NULL),

	/* text */
	S("text_font", NULL, text_font, sizeof(text_font), 0, on_text),
	I("text_height", NULL, text_height, 0, 0, "%d\n", CFG_POSITIVE, on_text),

	/* scroller */
	B("scroller_always_center_single_column", NULL, settings.scroller_always_center_single_column, 0,
		NULL),
	E("scroller_center_focused_column", NULL, settings.scroller_center_focused_column,
		scroller_center_mode_values, 0, NULL),
	I("scroller_left_strut", NULL, settings.scroller_struts.left, 0, INT_MAX, "%d\n", CFG_COMMIT,
		NULL),
	I("scroller_right_strut", NULL, settings.scroller_struts.right, 0, INT_MAX, "%d\n", CFG_COMMIT,
		NULL),
	I("scroller_top_strut", NULL, settings.scroller_struts.top, 0, INT_MAX, "%d\n", CFG_COMMIT, NULL),
	I("scroller_bottom_strut", NULL, settings.scroller_struts.bottom, 0, INT_MAX, "%d\n", CFG_COMMIT,
		NULL),

	/* blur */
	B("blur_enabled", NULL, blur_enabled, 0, NULL),
	F("blur_radius", NULL, blur_radius, 0, 0, "%.2f\n", CFG_POSITIVE, NULL),
	B("blur_full_res", NULL, blur_full_res, 0, NULL),
	I("blur_passes", NULL, blur_passes, 1, 10, "%d\n", 0, NULL),
	I("blur_downsample", NULL, blur_downsample, 1, 8, "%d\n", 0, on_blur_downsample),
	F("blur_offset", NULL, blur_offset, 0.0, 8.0, "%.3f\n", 0, NULL),
	F("blur_saturation", NULL, blur_saturation, 0.0, 3.0, "%.3f\n", 0, NULL),
	F("blur_vibrancy", NULL, blur_vibrancy, 0.0, 1.0, "%.3f\n", 0, NULL),
	F("blur_vibrancy_darkness", NULL, blur_vibrancy_darkness, 0.0, 1.0, "%.3f\n", 0, NULL),
	F("blur_noise_strength", NULL, blur_noise_strength, 0.0, 1.0, "%.3f\n", 0, NULL),
	F("blur_brightness", NULL, blur_brightness, 0.5, 2.0, "%.3f\n", 0, NULL),
	F("blur_contrast", NULL, blur_contrast, 0.5, 2.0, "%.3f\n", 0, NULL),

	/* refraction */
	F("refraction_strength", NULL, refraction_strength, 0.0, 30.0, "%.3f\n", 0, NULL),
	F("refraction_edge_size_px", NULL, refraction_edge_size_px, 0.0, 400.0, "%.3f\n", 0, NULL),
	F("refraction_corner_radius_px", NULL, refraction_corner_radius_px, 0.0, 400.0, "%.3f\n", 0, NULL),
	F("refraction_normal_pow", NULL, refraction_normal_pow, 0.0, 8.0, "%.3f\n", 0, NULL),
	F("refraction_rgb_fringing", NULL, refraction_rgb_fringing, 0.0, 1.0, "%.6f\n", 0, NULL),
	F("refraction_offset", NULL, refraction_offset, 0.0, 8.0, "%.3f\n", 0, NULL),
	I("refraction_texture_repeat_mode", NULL, refraction_texture_repeat_mode, 0, 1, "%d\n", 0, NULL),

	/* mica and acrylic */
	B("mica_enabled", NULL, mica_enabled, 0, on_mica),
	F("mica_tint_strength", NULL, mica_tint_strength, 0.0, 1.0, "%.3f\n", 0, on_mica),
	C("mica_tint", NULL, mica_tint, 0, on_mica),
	C("acrylic_tint", NULL, acrylic_tint, 0, NULL),
	F("acrylic_tint_strength", NULL, acrylic_tint_strength, 0.0, 1.0, "%.3f\n", 0, NULL),
	F("acrylic_noise_strength", NULL, acrylic_noise_strength, 0.0, 1.0, "%.3f\n", 0, NULL),
	I("acrylic_blur_passes", NULL, acrylic_blur_passes, 0, 10, "%d\n", 0, NULL),

	/* shadow */
	F("shadow_size", NULL, settings.shadow_size, 0.0, 100.0, "%.1f\n", 0, NULL),
	F("shadow_offset_x", NULL, settings.shadow_offset_x, -100.0, 100.0, "%.1f\n", 0, NULL),
	F("shadow_offset_y", NULL, settings.shadow_offset_y, -100.0, 100.0, "%.1f\n", 0, NULL),
	C("shadow_color", NULL, settings.shadow_color, 0, NULL),

	/* screen shader */
	B("screen_shader_enabled", NULL, screen_shader_enabled, 0, on_screen_shader_enabled),

	/* idle power management */
	I("idle_timeout", NULL, settings.idle_timeout, 0, 86400, "%d\n", 0, on_idle),
	B("idle_dpms", NULL, settings.idle_dpms, 0, on_idle),
	B("realtime_scheduling", NULL, settings.realtime_scheduling, 0, on_realtime_scheduling),
};

// the introspection sizes its buffers from CFG_SETTINGS_COUNT
_Static_assert(IPC_ARRAY_LEN(settings_table) == CFG_SETTINGS_COUNT,
	"CFG_SETTINGS_COUNT is out of date");

#undef B
#undef I
#undef F
#undef D
#undef E
#undef S
#undef C

static const cfg_setting_t *find_setting(const char *name) {
	for (size_t i = 0; i < IPC_ARRAY_LEN(settings_table); i++) {
		if (streq(name, settings_table[i].name))
			return &settings_table[i];

		if (settings_table[i].alias && streq(name, settings_table[i].alias))
			return &settings_table[i];
	}
	return NULL;
}

void cfg_errf(char *err, size_t errsz, const char *fmt, ...) {
	if (err == NULL || errsz == 0)
		return;

	va_list ap;
	va_start(ap, fmt);
	vsnprintf(err, errsz, fmt, ap);
	va_end(ap);
}

static bool cfg_set_bool(const cfg_setting_t *s, const char *arg, char *err, size_t errsz) {
	bool v;
	if (!ipc_parse_bool(arg, &v)) {
		cfg_errf(err, errsz, "%s: expected true or false, got \"%s\"\n", s->name, arg);
		return false;
	}

	*(bool *)s->ptr = v;
	return true;
}

static void cfg_store_enum(const cfg_setting_t *s, long value) {
	assert(sizeof(int) == 4);
	int v = (int)value;
	memcpy(s->ptr, &v, sizeof(v));
}

static bool cfg_set_enum(const cfg_setting_t *s, const char *arg, char *err, size_t errsz) {
	for (const cfg_enum_value_t *e = s->values; e->name; e++) {
		if (strcmp(arg, e->name) == 0) {
			cfg_store_enum(s, e->value);
			return true;
		}
	}

	ipc_buf_t b;
	char list[256];
	ipc_buf_init(&b, list, sizeof(list));
	for (const cfg_enum_value_t *e = s->values; e->name; e++)
		ipc_buff(&b, "%s\"%s\"", b.len > 0 ? ", " : "", e->name);

	cfg_errf(err, errsz, "%s: expected value to be one of: %s\n", s->name, list);
	return false;
}

static bool cfg_range(const cfg_setting_t *s, double val, double step, char *err, size_t errsz,
		double *out) {
	if (!isfinite(val)) {
		cfg_errf(err, errsz, "%s: value must be a finite number\n", s->name);
		return false;
	}

	bool positive = (s->flags & CFG_POSITIVE) != 0;
	bool lo_excl = (s->flags & (CFG_POSITIVE | CFG_EXCLUSIVE)) != 0;
	bool hi_excl = (s->flags & CFG_EXCLUSIVE) != 0;

	bool below = lo_excl ? val <= s->min : val < s->min;
	bool above = !positive && (hi_excl ? val >= s->max : val > s->max);

	if (!below && !above) {
		*out = val;
		return true;
	}

	if (!(s->flags & CFG_CLAMP)) {
		if (below)
			cfg_errf(err, errsz, "%s: value must be greater than %g\n", s->name, s->min);
		else
			cfg_errf(err, errsz, "%s: value must be less than %g\n", s->name, s->max);
		return false;
	}

	// an exclusive bound clamps to the nearest value inside the range
	*out = below ? (lo_excl ? s->min + step : s->min) : (hi_excl ? s->max - step : s->max);
	return true;
}

static bool cfg_set_int(const cfg_setting_t *s, const char *arg, char *err, size_t errsz) {
	char *end;
	errno = 0;
	long val = strtol(arg, &end, 10);
	if (end == arg || *end != '\0' || errno == ERANGE) {
		cfg_errf(err, errsz, "%s: invalid value \"%s\"\n", s->name, arg);
		return false;
	}

	double out;
	if (!cfg_range(s, (double)val, 1.0, err, errsz, &out))
		return false;

	*((int *)s->ptr) = (int)out;
	return true;
}

static bool cfg_set_float(const cfg_setting_t *s, const char *arg, char *err, size_t errsz) {
	char *end;
	double val = strtod(arg, &end);
	if (end == arg || *end != '\0') {
		cfg_errf(err, errsz, "%s: invalid value \"%s\"\n", s->name, arg);
		return false;
	}

	double out;
	if (!cfg_range(s, val, 0.0, err, errsz, &out))
		return false;

	*((float *)s->ptr) = (float)out;
	return true;
}

static bool cfg_set_double(const cfg_setting_t *s, const char *arg, char *err, size_t errsz) {
	char *end;
	double val = strtod(arg, &end);
	if (end == arg || *end != '\0') {
		cfg_errf(err, errsz, "%s: invalid value \"%s\"\n", s->name, arg);
		return false;
	}

	double out;
	if (!cfg_range(s, val, 0.0, err, errsz, &out))
		return false;

	*((double *)s->ptr) = out;
	return true;
}

static bool cfg_set_str(const cfg_setting_t *s, const char *arg, char *err, size_t errsz) {
	if (strlen(arg) >= s->size) {
		cfg_errf(err, errsz, "%s: value is too long\n", s->name);
		return false;
	}

	snprintf(s->ptr, s->size, "%s", arg);
	return true;
}

static bool cfg_set_rgba(const cfg_setting_t *s, const char *arg, char *err, size_t errsz) {
	float parsed[4];
	if (!ipc_parse_color_float(arg, parsed)) {
		cfg_errf(err, errsz, "%s: expected \"R G B [A]\"\n", s->name);
		return false;
	}

	float *rgba = s->ptr;
	memcpy(rgba, parsed, sizeof(parsed));
	return true;
}

void cfg_format_value(const cfg_setting_t *s, char *buf, size_t bufsz) {
	switch (s->type) {
	case CFG_BOOL:
		snprintf(buf, bufsz, "%s", *(bool *)s->ptr ? "true" : "false");
		break;
	case CFG_INT:
		snprintf(buf, bufsz, s->fmt, *(int *)s->ptr);
		break;
	case CFG_FLOAT:
		snprintf(buf, bufsz, s->fmt, *(float *)s->ptr);
		break;
	case CFG_DOUBLE:
		snprintf(buf, bufsz, s->fmt, *(double *)s->ptr);
		break;
	case CFG_ENUM: {
		int v;
		memcpy(&v, s->ptr, sizeof(v));
		const char *value = ipc_enum_name(s->values, v);
		snprintf(buf, bufsz, "%s", value ? value : "?");
		break;
	}
	case CFG_STR:
		snprintf(buf, bufsz, "%s", (char *)s->ptr);
		break;
	case CFG_RGBA:
		ipc_format_color_float(buf, bufsz, s->ptr);
		break;
	}
}

static void cfg_get(ipc_args_t *a, const cfg_setting_t *s) {
	char buf[512];

	cfg_format_value(s, buf, sizeof(buf));
	size_t len = strlen(buf);
	if (len > 0 && buf[len - 1] != '\n' && len + 2 < sizeof(buf)) {
		buf[len] = '\n';
		buf[len + 1] = '\0';
	}

	ipc_ok(a, buf);
}

// writes a setting, running its side effects and committing when it asks for it
static bool cfg_apply(const cfg_setting_t *s, const char *arg, char *err, size_t errsz) {
	bool ok = true;

	switch (s->type) {
	case CFG_BOOL:
		ok = cfg_set_bool(s, arg, err, errsz);
		break;
	case CFG_INT:
		ok = cfg_set_int(s, arg, err, errsz);
		break;
	case CFG_FLOAT:
		ok = cfg_set_float(s, arg, err, errsz);
		break;
	case CFG_DOUBLE:
		ok = cfg_set_double(s, arg, err, errsz);
		break;
	case CFG_ENUM:
		ok = cfg_set_enum(s, arg, err, errsz);
		break;
	case CFG_STR:
		ok = cfg_set_str(s, arg, err, errsz);
		break;
	case CFG_RGBA:
		ok = cfg_set_rgba(s, arg, err, errsz);
		break;
	}

	if (!ok)
		return false;

	if (s->on_set)
		s->on_set();
	if (s->flags & CFG_COMMIT)
		transaction_commit_dirty();

	return true;
}

bool config_apply_value(const char *name, const char *value, char *err, size_t errsz) {
	const cfg_setting_t *s = find_setting(name);
	if (s == NULL) {
		cfg_errf(err, errsz, "unknown setting \"%s\"\n", name);
		return false;
	}

	return cfg_apply(s, value, err, errsz);
}

const ipc_sub_t config_subs[] = {
	IPC_SUB("-l", "--list", "config --list [--json] [--changed] [--no-header]", cfg_list),
	IPC_SUB_END,
};

void ipc_cmd_config(ipc_args_t *a) {
	const char *name = ipc_peek(a);
	if (!name) {
		ipc_fail(a, "Missing setting name\n");
		return;
	}

	if (name[0] == '-') {
		if (!ipc_sub_dispatch(a, config_subs))
			ipc_fail_unknown(a, config_subs);
		return;
	}

	ipc_take(a);

	if (cfg_special_dispatch(a, name))
		return;

	const cfg_setting_t *s = find_setting(name);
	if (!s) {
		ipc_fail(a, "Unknown setting \"%s\"\n", name);
		return;
	}

	if (!ipc_peek(a)) {
		cfg_get(a, s);
		return;
	}

	const char *arg = ipc_take(a);

	char err[256];
	if (!cfg_apply(s, arg, err, sizeof(err))) {
		ipc_fail(a, "%s", err);
		return;
	}

	ipc_okf(a, "%s set\n", s->name);
}
