#include "animation.h"
#include "bezier.h"
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
#include "spring.h"
#include "tabs.h"
#include "text.h"
#include "transaction.h"
#include "tree.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

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

static void tabs_rebuild_all(void) {
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

static const cfg_setting_t settings_table[] = {
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
	F("scroller_default_proportion", NULL, settings.scroller_default_proportion, 0.1, 1.0, "%.2f\n", CFG_CLAMP,
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

static bool cfg_set_bool(ipc_args_t *a, const cfg_setting_t *s) {
	return ipc_bool(a, "value", (bool *)s->ptr);
}

static void cfg_store_enum(const cfg_setting_t *s, long value) {
	assert(sizeof(int) == 4);
	int v = (int)value;
	memcpy(s->ptr, &v, sizeof(v));
}

static bool cfg_set_enum(ipc_args_t *a, const cfg_setting_t *s) {
	long value;
	if (!ipc_enum(a, "value", s->values, &value))
		return false;

	cfg_store_enum(s, value);
	return true;
}

static bool cfg_range(ipc_args_t *a, const cfg_setting_t *s, double val, double step, double *out) {
	if (!isfinite(val)) {
		ipc_fail(a, "%s: value must be a finite number\n", s->name);
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
			ipc_fail(a, "%s: value must be greater than %g\n", s->name, s->min);
		else
			ipc_fail(a, "%s: value must be less than %g\n", s->name, s->max);
		return false;
	}

	// an exclusive bound clamps to the nearest value inside the range
	*out = below ? (lo_excl ? s->min + step : s->min) : (hi_excl ? s->max - step : s->max);
	return true;
}

static bool cfg_set_int(ipc_args_t *a, const cfg_setting_t *s) {
	const char *arg;
	if (!ipc_need(a, "value", &arg))
		return false;

	char *end;
	errno = 0;
	long val = strtol(arg, &end, 10);
	if (end == arg || *end != '\0' || errno == ERANGE) {
		ipc_fail(a, "%s: invalid value \"%s\"\n", s->name, arg);
		return false;
	}

	double out;
	if (!cfg_range(a, s, (double)val, 1.0, &out))
		return false;

	*((int *)s->ptr) = (int)out;
	return true;
}

static bool cfg_set_float(ipc_args_t *a, const cfg_setting_t *s) {
	const char *arg;
	if (!ipc_need(a, "value", &arg))
		return false;

	char *end;
	double val = strtod(arg, &end);
	if (end == arg || *end != '\0') {
		ipc_fail(a, "%s: invalid value \"%s\"\n", s->name, arg);
		return false;
	}

	double out;
	if (!cfg_range(a, s, val, 0.0, &out))
		return false;

	*((float *)s->ptr) = (float)out;
	return true;
}

static bool cfg_set_double(ipc_args_t *a, const cfg_setting_t *s) {
	const char *arg;
	if (!ipc_need(a, "value", &arg))
		return false;

	char *end;
	double val = strtod(arg, &end);
	if (end == arg || *end != '\0') {
		ipc_fail(a, "%s: invalid value \"%s\"\n", s->name, arg);
		return false;
	}

	double out;
	if (!cfg_range(a, s, val, 0.0, &out))
		return false;

	*((double *)s->ptr) = out;
	return true;
}

static bool cfg_set_str(ipc_args_t *a, const cfg_setting_t *s) {
	return ipc_str(a, "value", s->ptr, s->size);
}

static bool cfg_set_rgba(ipc_args_t *a, const cfg_setting_t *s) {
	const char *arg;
	if (!ipc_need(a, "value", &arg))
		return false;

	float *rgba = s->ptr;
	float parsed[4];
	if (!ipc_parse_color_float(arg, parsed)) {
		ipc_fail(a, "Expected \"R G B [A]\"\n");
		return false;
	}

	memcpy(rgba, parsed, sizeof(parsed));
	return true;
}

static void cfg_get(ipc_args_t *a, const cfg_setting_t *s) {
	char buf[512];

	switch (s->type) {
	case CFG_BOOL:
		snprintf(buf, sizeof(buf), "%s\n", *(bool *)s->ptr ? "true" : "false");
		break;
	case CFG_INT:
		snprintf(buf, sizeof(buf), s->fmt, *(int *)s->ptr);
		break;
	case CFG_FLOAT:
		snprintf(buf, sizeof(buf), s->fmt, *(float *)s->ptr);
		break;
	case CFG_DOUBLE:
		snprintf(buf, sizeof(buf), s->fmt, *(double *)s->ptr);
		break;
	case CFG_ENUM: {
		int v;
		memcpy(&v, s->ptr, sizeof(v));
		const char *value = ipc_enum_name(s->values, v);
		snprintf(buf, sizeof(buf), "%s\n", value ? value : "?");
		break;
	}
	case CFG_STR:
		snprintf(buf, sizeof(buf), "%s\n", (char *)s->ptr);
		break;
	case CFG_RGBA:
		ipc_format_color_float(buf, sizeof(buf), s->ptr);
		break;
	}

	ipc_ok(a, buf);
}

static bool cfg_set(ipc_args_t *a, const cfg_setting_t *s) {
	bool ok = true;

	switch (s->type) {
	case CFG_BOOL:
		ok = cfg_set_bool(a, s);
		break;
	case CFG_INT:
		ok = cfg_set_int(a, s);
		break;
	case CFG_FLOAT:
		ok = cfg_set_float(a, s);
		break;
	case CFG_DOUBLE:
		ok = cfg_set_double(a, s);
		break;
	case CFG_ENUM:
		ok = cfg_set_enum(a, s);
		break;
	case CFG_STR:
		ok = cfg_set_str(a, s);
		break;
	case CFG_RGBA:
		ok = cfg_set_rgba(a, s);
		break;
	}

	if (!ok)
		return false;

	if (s->on_set)
		s->on_set();
	if (s->flags & CFG_COMMIT)
		transaction_commit_dirty();

	ipc_okf(a, "%s set\n", s->name);
	return true;
}

static void cfg_tab_color(ipc_args_t *a, const char *suffix) {
	static const struct {
		const char *name;
		float *color;
	} colors[] = {
		{"bar_bg", color_bar_bg},
		{"bg", color_tab_bg},
		{"bg_active", color_tab_bg_active},
		{"text", color_tab_text},
		{"text_active", color_tab_text_active},
		{"sep", color_tab_sep},
	};

	for (size_t i = 0; i < IPC_ARRAY_LEN(colors); i++) {
		if (!streq(suffix, colors[i].name))
			continue;

		if (!ipc_peek(a)) {
			char buf[128];
			ipc_format_color_float(buf, sizeof(buf), colors[i].color);
			ipc_ok(a, buf);
			return;
		}

		cfg_setting_t tmp = {
			.name = "tab_color",
			.type = CFG_RGBA,
			.ptr = colors[i].color
		};
		if (!cfg_set_rgba(a, &tmp))
			return;

		tabs_rebuild_all();
		ipc_okf(a, "tab_color_%s set\n", colors[i].name);
		return;
	}

	ipc_fail(a, "Unknown tab color \"%s\"\n", suffix);
}

static void cfg_scroller_presets(ipc_args_t *a) {
	if (!ipc_peek(a)) {
		char buf[512];
		ipc_buf_t b;
		ipc_buf_init(&b, buf, sizeof(buf));
		for (int i = 0; i < settings.scroller_proportion_preset_count; i++) {
			ipc_buff(&b, "%.2f%s", settings.scroller_proportion_preset[i],
				i < settings.scroller_proportion_preset_count - 1 ? "," : "\n");
		}
		ipc_buf_send(a, &b);
		return;
	}

	const char *value;
	if (!ipc_need(a, "comma separated values", &value))
		return;

	int count = 1;
	for (const char *p = value; *p; p++) {
		if (*p == ',')
			count++;
	}

	float *presets = malloc(count * sizeof(float));
	if (!presets) {
		ipc_fail(a, "Memory allocation failed\n");
		return;
	}

	char *copy = strdup(value);
	if (!copy) {
		free(presets);
		ipc_fail(a, "Memory allocation failed\n");
		return;
	}

	int i = 0;
	for (char *tok = strtok(copy, ","); tok && i < count; tok = strtok(NULL, ",")) {
		if (!ipc_parse_float(tok, 0.1f, 1.0f, &presets[i])) {
			free(copy);
			free(presets);
			ipc_fail(a, "Invalid value \"%s\" in proportion preset list\n", tok);
			return;
		}
		i++;
	}
	free(copy);

	free(settings.scroller_proportion_preset);
	settings.scroller_proportion_preset = presets;
	settings.scroller_proportion_preset_count = i;

	ipc_ok(a, "scroller_proportion_preset set\n");
}

// <normal|active|focused>_border_gradient[2|_lerp]
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

static const cfg_gradient_t gradients[] = {
	{"normal_border_gradient", GRAD_NORMAL, false, false},
	{"active_border_gradient", GRAD_ACTIVE, false, false},
	{"focused_border_gradient", GRAD_FOCUSED, false, false},
	{"normal_border_gradient2", GRAD_NORMAL, true, false},
	{"active_border_gradient2", GRAD_ACTIVE, true, false},
	{"focused_border_gradient2", GRAD_FOCUSED, true, false},
	{"normal_border_gradient_lerp", GRAD_NORMAL, false, true},
	{"active_border_gradient_lerp", GRAD_ACTIVE, false, true},
	{"focused_border_gradient_lerp", GRAD_FOCUSED, false, true},
};

static const cfg_gradient_t *find_gradient(const char *name) {
	for (size_t i = 0; i < IPC_ARRAY_LEN(gradients); i++) {
		if (streq(gradients[i].name, name))
			return &gradients[i];
	}
	return NULL;
}

static void cfg_border_gradient(ipc_args_t *a, const cfg_gradient_t *g) {
	static border_theme_t *const themes[] = {
		[GRAD_NORMAL] = &settings.normal_border_theme,
		[GRAD_ACTIVE] = &settings.active_border_theme,
		[GRAD_FOCUSED] = &settings.focused_border_theme,
	};
	border_theme_t *bt = themes[g->theme];

	if (g->lerp) {
		if (!ipc_peek(a)) {
			ipc_okf(a, "%f\n", bt->gradient_lerp);
			return;
		}

		float v;
		if (!ipc_float(a, "value", 0.0f, 1.0f, &v))
			return;

		bt->gradient_lerp = v;
		refresh_border_colors();
		transaction_commit_dirty();
		ipc_okf(a, "%s set\n", g->name);
		return;
	}

	float *grad = g->second ? bt->gradient2 : bt->gradient;
	int *gcount = g->second ? &bt->gradient2_count : &bt->gradient_count;
	float *gangle = g->second ? &bt->gradient2_angle : &bt->gradient_angle;

	if (!ipc_peek(a)) {
		// leave room for the trailing newline and its terminator
		char buf[512];
		ipc_format_gradient(buf, sizeof(buf) - 2, grad, *gcount, *gangle);
		size_t len = strlen(buf);
		buf[len] = '\n';
		buf[len + 1] = '\0';
		ipc_ok(a, buf);
		return;
	}

	char joined[512] = {0};
	ipc_buf_t b;
	ipc_buf_init(&b, joined, sizeof(joined));
	ipc_foreach(a, stop)
		ipc_buff(&b, "%s%s", b.len > 0 ? " " : "", stop);

	if (streq(joined, "clear")) {
		*gcount = 0;
		*gangle = 0.0f;
	} else if (!ipc_parse_gradient(joined, grad, gcount, gangle)) {
		ipc_fail(a, "Expected one or more #RRGGBB stops and an optional angle\n");
		return;
	}

	refresh_border_colors();
	transaction_commit_dirty();
	ipc_okf(a, "%s set\n", g->name);
}

static void cfg_acrylic_light_anchor(ipc_args_t *a) {
	if (!ipc_peek(a)) {
		char buf[128];
		snprintf(buf, sizeof(buf), "%.3f %.3f\n", acrylic_light_anchor[0], acrylic_light_anchor[1]);
		ipc_ok(a, buf);
		return;
	}

	float x, y;
	if (!ipc_float(a, "x", -1.0f, 1.0f, &x) || !ipc_float(a, "y", -1.0f, 1.0f, &y))
		return;

	acrylic_light_anchor[0] = x;
	acrylic_light_anchor[1] = y;
	ipc_ok(a, "acrylic_light_anchor set\n");
}

static void cfg_blur_algorithm(ipc_args_t *a) {
	if (!ipc_peek(a)) {
		ipc_okf(a, "%s\n", effects_algorithm_to_str(blur_algorithm));
		return;
	}

	const char *name;
	if (!ipc_need(a, "algorithm", &name))
		return;

	enum blur_algorithm algo = blur_algorithm_from_str(name);
	if (algo == BLUR_ALGORITHM_NONE && !streq(name, "none")) {
		ipc_fail(a, "Unknown algorithm\n");
		return;
	}

	blur_algorithm = algo;
	ipc_ok(a, "blur_algorithm set\n");
}

static void cfg_screen_shader(ipc_args_t *a) {
	if (!ipc_peek(a)) {
		ipc_okf(a, "%s\n", screen_shader_get_name());
		return;
	}

	const char *name;
	if (!ipc_need(a, "shader", &name))
		return;

	if (!screen_shader_set(name)) {
		ipc_fail(a, "Unknown shader (builtin: none grayscale invert sepia nightlight)\n");
		return;
	}

	ipc_ok(a, "screen_shader set\n");
}

static void cfg_screen_shader_file(ipc_args_t *a) {
	const char *path;
	if (!ipc_need(a, "path", &path))
		return;

	if (!screen_shader_load_file(path)) {
		ipc_fail(a, "Failed to load shader\n");
		return;
	}

	ipc_ok(a, "screen_shader_file loaded\n");
}

static void cfg_animation_bezier(ipc_args_t *a) {
	if (!ipc_peek(a)) {
		ipc_okf(a, "%s\n", animation_get_bezier());
		return;
	}

	const char *name;
	if (!ipc_need(a, "curve", &name))
		return;

	if (!bezier_exists(name)) {
		ipc_fail(a, "No such bezier curve\n");
		return;
	}

	animation_set_bezier(name);
	ipc_ok(a, "animation_bezier set\n");
}

static void cfg_animation_duration(ipc_args_t *a) {
	if (!ipc_peek(a)) {
		ipc_okf(a, "%u\n", animation_get_duration());
		return;
	}

	const char *arg;
	if (!ipc_need(a, "duration", &arg))
		return;

	int ms;
	if (!ipc_int_str(a, arg, "duration", 1, INT_MAX, &ms) || !ipc_end(a))
		return;

	animation_set_duration((uint32_t)ms);
	ipc_ok(a, "animation_duration set\n");
}

// config animation <type> [spring|bezier|duration|enabled] [value]
static void cfg_animation(ipc_args_t *a) {
	if (!ipc_peek(a)) {
		ipc_fail(a, "Expected a type\n");
		return;
	}

	const char *type;
	if (!ipc_need(a, "type", &type))
		return;

	if (!ipc_peek(a)) {
		int idx = animation_type_from_name(type);
		if (idx < 0) {
			ipc_fail(a, "Unknown type \"%s\"\n", type);
			return;
		}

		const char *spring = animation_type_get_spring(type);
		const char *bezier = animation_type_get_bezier(type);
		uint32_t dur = animation_type_get_duration(type);
		bool enabled = animation_type_get_enabled(type);

		char buf[256];
		ipc_buf_t b;
		ipc_buf_init(&b, buf, sizeof(buf));
		if (spring)
			ipc_buff(&b, "spring: %s\n", spring);
		else
			ipc_buff(&b, "bezier: %s\n", bezier ? bezier : "(global default)");
		ipc_buff(&b, "duration: %u\n", dur > 0 ? dur : animation_get_duration());
		ipc_buff(&b, "enabled: %s\n", enabled ? "true" : "false");
		ipc_ok(a, buf);
		return;
	}

	const char *what = ipc_take(a);

	if (streq(what, "spring") || streq(what, "bezier")) {
		// an explicit empty name means "fall back to the default curve"
		const char *curve = "";
		if (ipc_peek(a)) {
			curve = ipc_take(a);
			if (curve[0] != '\0') {
				bool exists = streq(what, "spring") ? spring_exists(curve) : bezier_exists(curve);
				if (!exists) {
					ipc_fail(a, "No such %s curve \"%s\"\n", what, curve);
					return;
				}
			}
		}

		bool ok = streq(what, "spring") ? animation_set_type_spring(type,
			curve) : animation_set_type_config(type, curve, 0);
		if (!ok) {
			ipc_fail(a, "Unknown type \"%s\"\n", type);
			return;
		}

		ipc_okf(a, "Animation type %s set\n", what);
		return;
	}

	if (streq(what, "duration")) {
		int ms;
		if (!ipc_int(a, "value", 1, INT_MAX, &ms))
			return;

		if (!animation_set_type_config(type, NULL, (uint32_t)ms)) {
			ipc_fail(a, "Unknown type \"%s\"\n", type);
			return;
		}

		ipc_ok(a, "Animation type duration set\n");
		return;
	}

	if (streq(what, "enabled")) {
		if (!ipc_peek(a)) {
			ipc_okf(a, "%s\n", animation_type_get_enabled(type) ? "enabled" : "disabled");
			return;
		}

		bool on = animation_type_get_enabled(type);
		if (!ipc_toggle(a, &on))
			return;

		if (!animation_type_set_enabled(type, on)) {
			ipc_fail(a, "Unknown type \"%s\"\n", type);
			return;
		}

		ipc_okf(a, "%s\n", on ? "enabled" : "disabled");
		return;
	}

	ipc_fail(a, "Expected one of: spring, bezier, duration, enabled\n");
}

typedef struct {
	const char *name;
	void (*fn)(ipc_args_t *a);
} cfg_named_handler_t;

static const cfg_named_handler_t special_settings[] = {
	{"scroller_proportion_preset", cfg_scroller_presets},
	{"acrylic_light_anchor", cfg_acrylic_light_anchor},
	{"blur_algorithm", cfg_blur_algorithm},
	{"screen_shader", cfg_screen_shader},
	{"screen_shader_file", cfg_screen_shader_file},
	{"animation_bezier", cfg_animation_bezier},
	{"animation_duration", cfg_animation_duration},
	{"animation", cfg_animation},
	{NULL, NULL},
};

void ipc_cmd_config(ipc_args_t *a) {
	const char *name = ipc_peek(a);
	if (!name) {
		ipc_fail(a, "Missing setting name\n");
		return;
	}

	ipc_take(a);

	if (strncmp(name, "tab_color_", 10) == 0) {
		cfg_tab_color(a, name + 10);
		return;
	}

	const cfg_gradient_t *grad = find_gradient(name);
	if (grad) {
		cfg_border_gradient(a, grad);
		return;
	}

	for (const cfg_named_handler_t *sp = special_settings; sp->name; sp++) {
		if (!streq(name, sp->name))
			continue;

		sp->fn(a);
		return;
	}

	const cfg_setting_t *s = find_setting(name);
	if (!s) {
		ipc_fail(a, "Unknown setting \"%s\"\n", name);
		return;
	}

	if (ipc_peek(a))
		cfg_set(a, s);
	else
		cfg_get(a, s);
}
