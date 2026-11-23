#include "config_internal.h"
#include "dim.h"
#include "effects/effects.h"
#include "input/keyboard.h"
#include "ipc/args.h"
#include "ipc/registry.h"
#include "layout/layout.h"
#include "output/output.h"
#include "protocol/idle_power.h"
#include "protocol/xdg_toplevel.h"
#include "realtime.h"
#include "server.h"
#include "settings.h"
#include "str.h"
#include "tabs.h"
#include <limits.h>
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

static void on_keyboard_grouping(void) {
	keyboard_reapply_grouping();
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
	if (!settings.screen_shader_enabled)
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

static const str_pair_t focus_on_activate_values[] = {
	{"focus", FOCUS_ON_ACTIVATE_FOCUS},
	{"none", FOCUS_ON_ACTIVATE_NONE},
	{"smart", FOCUS_ON_ACTIVATE_SMART},
	{"urgent", FOCUS_ON_ACTIVATE_URGENT},
	IPC_ENUM_END,
};

static const str_pair_t decoration_mode_values[] = {
	{"none", DECORATION_NONE},
	{"tabs", DECORATION_TABS},
	{"always", DECORATION_ALWAYS},
	{"csd", DECORATION_CSD},
	IPC_ENUM_END,
};

static const str_pair_t focus_follows_pointer_values[] = {
	{"no", FOLLOWS_NO},
	{"false", FOLLOWS_NO},
	{"yes", FOLLOWS_YES},
	{"true", FOLLOWS_YES},
	{"always", FOLLOWS_ALWAYS},
	IPC_ENUM_END,
};

static const str_pair_t workspace_anim_direction_values[] = {
	{"vertical", WORKSPACE_ANIM_VERTICAL},
	{"horizontal", WORKSPACE_ANIM_HORIZONTAL},
	IPC_ENUM_END,
};

static const str_pair_t automatic_scheme_values[] = {
	{"longest_side", SCHEME_LONGEST_SIDE},
	{"longest-side", SCHEME_LONGEST_SIDE},
	{"alternate", SCHEME_ALTERNATE},
	{"spiral", SCHEME_SPIRAL},
	IPC_ENUM_END,
};

static const str_pair_t initial_polarity_values[] = {
	{"first_child", FIRST_CHILD},
	{"first-child", FIRST_CHILD},
	{"second_child", SECOND_CHILD},
	{"second-child", SECOND_CHILD},
	IPC_ENUM_END,
};

static const str_pair_t keyboard_grouping_values[] = {
	{"none", KEYBOARD_GROUP_NONE},
	{"smart", KEYBOARD_GROUP_SMART},
	{"default", KEYBOARD_GROUP_DEFAULT},
	IPC_ENUM_END,
};

#define B(n, a, p, fl, on) {n, a, SETTING_BOOL, &(p), 0, 0, 0, NULL, NULL, fl, on}
#define I(n, a, p, lo, hi, f, fl, on) \
	{n, a, SETTING_INT, &(p), 0, lo, hi, f, NULL, fl, on}
#define F(n, a, p, lo, hi, f, fl, on) \
	{n, a, SETTING_FLOAT, &(p), 0, lo, hi, f, NULL, fl, on}
#define D(n, a, p, lo, hi, f, fl, on) \
	{n, a, SETTING_DOUBLE, &(p), 0, lo, hi, f, NULL, fl, on}
#define E(n, a, p, vals, fl, on) {n, a, SETTING_ENUM, &(p), 0, 0, 0, NULL, vals, fl, on}
#define S(n, a, p, sz, fl, on) {n, a, SETTING_STR, (p), sz, 0, 0, NULL, NULL, fl, on}
#define C(n, a, p, fl, on) {n, a, SETTING_RGBA, (p), 0, 0, 0, NULL, NULL, fl, on}

const setting_t settings_table[] = {
	/* geometry */
	I("border_width", NULL, settings.border_width, INT_MIN, INT_MAX, "%d\n", SETTING_COMMIT, NULL),
	I("window_gap", NULL, settings.window_gap, INT_MIN, INT_MAX, "%d\n", SETTING_COMMIT,
		on_window_gap),

	/* layout behaviour */
	E("automatic_scheme", NULL, settings.automatic_scheme, automatic_scheme_values, SETTING_COMMIT,
		NULL),
	B("borderless_singleton", NULL, settings.borderless_singleton, SETTING_COMMIT, NULL),
	B("smart_gaps", NULL, settings.smart_gaps, SETTING_COMMIT, NULL),
	B("smart_borders", NULL, settings.smart_borders, SETTING_COMMIT, NULL),
	B("respect_tiled_min_size", NULL, settings.respect_tiled_min_size, SETTING_COMMIT, NULL),
	B("focus_wrapping", NULL, settings.focus_wrapping, SETTING_COMMIT, NULL),
	B("hide_lone_tab", NULL, settings.hide_lone_tab, SETTING_COMMIT, NULL),
	B("pointer_follows_focus", NULL, settings.pointer_follows_focus, SETTING_COMMIT, NULL),
	B("auto_float_dialogs", NULL, settings.auto_float_dialogs, 0, NULL),
	B("enable_minimize", NULL, settings.enable_minimize, 0, on_enable_minimize),
	B("enable_maximize", NULL, settings.enable_maximize, 0, on_enable_maximize),
	B("minimize_to_scratchpad", NULL, settings.minimize_to_scratchpad, 0, NULL),
	B("scratchpad_restore_to_origin", NULL, settings.scratchpad_restore_to_origin, 0, NULL),
	I("ignore_ewmh_fullscreen", NULL, settings.ignore_ewmh_fullscreen, 0, 2, "%d\n", 0, NULL),
	D("split_ratio", NULL, settings.split_ratio, 0, 1, "%f\n", SETTING_COMMIT | SETTING_EXCLUSIVE,
		NULL),
	E("focus_on_activate", NULL, settings.focus_on_activate, focus_on_activate_values, SETTING_COMMIT,
		NULL),
	E("focus_follows_pointer", "focus_follows_mouse", settings.focus_follows_mouse,
		focus_follows_pointer_values, SETTING_COMMIT, NULL),
	I("focus_follows_pointer_max_scroll", "focus_follows_mouse_max_scroll",
		settings.focus_follows_mouse_max_scroll, 0, 100, "%d\n", 0, NULL),
	E("initial_polarity", NULL, settings.initial_polarity, initial_polarity_values, 0, NULL),

	/* animations */
	B("enable_animations", NULL, settings.enable_animations, 0, NULL),
	B("workspace_anim_slide_up", NULL, settings.workspace_anim_slide_up, 0, NULL),
	E("workspace_anim_direction", NULL, settings.workspace_anim_direction,
		workspace_anim_direction_values, 0, NULL),

	/* dimming */
	B("dim_inactive", NULL, settings.dim_inactive, 0, on_dim),
	B("dim_below", NULL, settings.dim_below, 0, on_dim),
	F("dim_strength", NULL, settings.dim_strength, 0.0, 1.0, "%.3f\n", 0, on_dim),
	C("dim_color", NULL, settings.dim_color, 0, on_dim),

	/* borders */
	S("normal_border_color", NULL, settings.normal_border_color, sizeof(settings.normal_border_color),
		SETTING_COMMIT, on_border_color),
	S("active_border_color", NULL, settings.active_border_color, sizeof(settings.active_border_color),
		SETTING_COMMIT, on_border_color),
	S("focused_border_color", NULL, settings.focused_border_color,
		sizeof(settings.focused_border_color), SETTING_COMMIT, on_border_color),
	S("presel_feedback_color", NULL, settings.presel_feedback_color,
		sizeof(settings.presel_feedback_color), SETTING_COMMIT, on_border_color),
	S("tiling_drag_indicator_color", NULL, settings.tiling_drag_indicator_color,
		sizeof(settings.tiling_drag_indicator_color), 0, NULL),

	/* text */
	S("text_font", NULL, settings.text_font, sizeof(settings.text_font), 0, on_text),
	I("text_height", NULL, settings.text_height, 0, 0, "%d\n", SETTING_POSITIVE, on_text),

	/* blur */
	B("blur_enabled", NULL, settings.blur_enabled, 0, NULL),
	F("blur_radius", NULL, settings.blur_radius, 0, 0, "%.2f\n", SETTING_POSITIVE, NULL),
	B("blur_full_res", NULL, settings.blur_full_res, 0, NULL),
	I("blur_passes", NULL, settings.blur_passes, 1, 10, "%d\n", 0, NULL),
	I("blur_downsample", NULL, settings.blur_downsample, 1, 8, "%d\n", 0, on_blur_downsample),
	F("blur_offset", NULL, settings.blur_offset, 0.0, 8.0, "%.3f\n", 0, NULL),
	F("blur_saturation", NULL, settings.blur_saturation, 0.0, 3.0, "%.3f\n", 0, NULL),
	F("blur_vibrancy", NULL, settings.blur_vibrancy, 0.0, 1.0, "%.3f\n", 0, NULL),
	F("blur_vibrancy_darkness", NULL, settings.blur_vibrancy_darkness, 0.0, 1.0, "%.3f\n", 0, NULL),
	F("blur_noise_strength", NULL, settings.blur_noise_strength, 0.0, 1.0, "%.3f\n", 0, NULL),
	F("blur_brightness", NULL, settings.blur_brightness, 0.5, 2.0, "%.3f\n", 0, NULL),
	F("blur_contrast", NULL, settings.blur_contrast, 0.5, 2.0, "%.3f\n", 0, NULL),

	/* refraction */
	F("refraction_strength", NULL, settings.refraction_strength, 0.0, 30.0, "%.3f\n", 0, NULL),
	F("refraction_edge_size_px", NULL, settings.refraction_edge_size_px, 0.0, 400.0, "%.3f\n", 0,
		NULL),
	F("refraction_corner_radius_px", NULL, settings.refraction_corner_radius_px, 0.0, 400.0, "%.3f\n",
		0, NULL),
	F("refraction_normal_pow", NULL, settings.refraction_normal_pow, 0.0, 8.0, "%.3f\n", 0, NULL),
	F("refraction_rgb_fringing", NULL, settings.refraction_rgb_fringing, 0.0, 1.0, "%.6f\n", 0, NULL),
	F("refraction_offset", NULL, settings.refraction_offset, 0.0, 8.0, "%.3f\n", 0, NULL),
	I("refraction_texture_repeat_mode", NULL, settings.refraction_texture_repeat_mode, 0, 1, "%d\n", 0,
		NULL),

	/* mica and acrylic */
	B("mica_enabled", NULL, settings.mica_enabled, 0, on_mica),
	F("mica_tint_strength", NULL, settings.mica_tint_strength, 0.0, 1.0, "%.3f\n", 0, on_mica),
	C("mica_tint", NULL, settings.mica_tint, 0, on_mica),
	C("acrylic_tint", NULL, settings.acrylic_tint, 0, NULL),
	F("acrylic_tint_strength", NULL, settings.acrylic_tint_strength, 0.0, 1.0, "%.3f\n", 0, NULL),
	F("acrylic_noise_strength", NULL, settings.acrylic_noise_strength, 0.0, 1.0, "%.3f\n", 0, NULL),
	I("acrylic_blur_passes", NULL, settings.acrylic_blur_passes, 0, 10, "%d\n", 0, NULL),

	/* shadow */
	F("shadow_size", NULL, settings.shadow_size, 0.0, 100.0, "%.1f\n", 0, NULL),
	F("shadow_offset_x", NULL, settings.shadow_offset_x, -100.0, 100.0, "%.1f\n", 0, NULL),
	F("shadow_offset_y", NULL, settings.shadow_offset_y, -100.0, 100.0, "%.1f\n", 0, NULL),
	C("shadow_color", NULL, settings.shadow_color, 0, NULL),

	/* screen shader */
	B("screen_shader_enabled", NULL, settings.screen_shader_enabled, 0, on_screen_shader_enabled),

	/* idle power management */
	I("idle_timeout", NULL, settings.idle_timeout, 0, 86400, "%d\n", 0, on_idle),
	B("idle_dpms", NULL, settings.idle_dpms, 0, on_idle),
	B("realtime_scheduling", NULL, settings.realtime_scheduling, 0, on_realtime_scheduling),

	/* input */
	E("keyboard_grouping", NULL, settings.keyboard_grouping, keyboard_grouping_values, 0,
		on_keyboard_grouping),

	/* misc */
	B("record_history", NULL, settings.record_history, 0, NULL),
	B("allow_tearing", NULL, settings.allow_tearing, SETTING_COMMIT, NULL),
	B("direct_scanout", NULL, settings.direct_scanout, 0, NULL),
	E("decoration_mode", NULL, settings.decoration_mode, decoration_mode_values, SETTING_COMMIT,
		on_decoration_mode),
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

static const setting_t *find_setting(const char *name) {
	for (size_t i = 0; i < IPC_ARRAY_LEN(settings_table); i++) {
		if (streq(name, settings_table[i].name))
			return &settings_table[i];

		if (settings_table[i].alias && streq(name, settings_table[i].alias))
			return &settings_table[i];
	}
	return NULL;
}

static void cfg_get(ipc_args_t *a, const setting_t *s) {
	char buf[512];

	setting_format(s, buf, sizeof(buf));
	size_t len = strlen(buf);
	if (len > 0 && buf[len - 1] != '\n' && len + 2 < sizeof(buf)) {
		buf[len] = '\n';
		buf[len + 1] = '\0';
	}

	ipc_ok(a, buf);
}

// a legacy layout option can be written through its old `config` name
static bool config_apply_legacy(const char *name, const char *value, char *err, size_t errsz,
		const bool dry_run) {
	const char *layout_name, *option;
	if (!layout_legacy_lookup(name, &layout_name, &option))
		return false;

	layout_t id;
	if (layout_find_by_name(layout_name, &id) && layout_setting_find(id, option) != NULL)
		return layout_setting_set(id, option, value, err, errsz, dry_run);

	setting_err(err, errsz, "%s: moved to \"layout %s %s\"\n", name, layout_name, option);
	return false;
}

bool config_apply_value(const char *name, const char *value, char *err, size_t errsz) {
	if (layout_legacy_lookup(name, NULL, NULL))
		return config_apply_legacy(name, value, err, errsz, false);

	const setting_t *s = find_setting(name);
	if (s == NULL) {
		setting_err(err, errsz, "unknown setting \"%s\"\n", name);
		return false;
	}

	return setting_apply(s, value, err, errsz, false);
}

bool config_validate_value(const char *name, const char *value, char *err, size_t errsz) {
	if (layout_legacy_lookup(name, NULL, NULL))
		return config_apply_legacy(name, value, err, errsz, true);

	if (cfg_special_name(name)) {
		if (!cfg_special_settable(name)) {
			setting_err(err, errsz, "%s: no dry-run validation available\n", name);
			return false;
		}

		return cfg_special_apply(name, value, err, errsz, true);
	}

	const setting_t *s = find_setting(name);
	if (s == NULL) {
		setting_err(err, errsz, "unknown setting \"%s\"\n", name);
		return false;
	}

	return setting_apply(s, value, err, errsz, true);
}

typedef struct {
	const setting_t *setting;
	const char *name;
} cfg_reset_target_t;

static const char *cfg_reset_value(const char *name, const char *dflt, char *buf, size_t bufsz) {
	if (cfg_find_gradient(name) && strchr(dflt, ' ') == NULL) {
		str_copy(buf, bufsz, "clear");
		return buf;
	}

	return dflt;
}

static void cfg_reset(ipc_args_t *a) {
	cfg_reset_target_t targets[CFG_INTRO_MAX];
	size_t total = 0;
	bool named = ipc_left(a) > 0;

	if (named) {
		ipc_foreach(a, arg) {
			const setting_t *s = find_setting(arg);

			if (s == NULL && !cfg_special_settable(arg)) {
				if (cfg_special_name(arg))
					ipc_fail(a, "%s: set through its own syntax, so there is no default to restore\n", arg);
				else
					ipc_fail(a, "Unknown setting \"%s\"\n", arg);
				return;
			}

			if (total == CFG_INTRO_MAX) {
				ipc_fail(a, "Too many settings to reset\n");
				return;
			}

			targets[total].setting = s;
			targets[total].name = s ? s->name : arg;
			total++;
		}
	} else {
		for (size_t i = 0; i < CFG_SETTINGS_COUNT; i++) {
			targets[total].setting = &settings_table[i];
			targets[total].name = settings_table[i].name;
			total++;
		}

		for (size_t i = 0; i < cfg_extra_count(); i++) {
			targets[total].setting = NULL;
			targets[total].name = cfg_extra_name(i);
			total++;
		}
	}

	IPC_REPLY_GROWING(detail);

	size_t changed = 0, missing = 0;

	for (size_t i = 0; i < total; i++) {
		const setting_t *s = targets[i].setting;
		const char *name = targets[i].name;

		char dflt[CFG_VALUE_MAX];
		if (!cfg_default_value(name, dflt, sizeof(dflt))) {
			missing++;
			continue;
		}

		char before[CFG_VALUE_MAX];
		if (s)
			setting_format(s, before, sizeof(before));
		else if (!cfg_special_format_value(name, before, sizeof(before)))
			snprintf(before, sizeof(before), "?");
		str_rtrim(before);

		bool same = streq(before, dflt);

		// writing a value that is already there would run its side effects and
		// commit a transaction for nothing
		if (!same) {
			char arg_storage[CFG_VALUE_MAX];
			const char *value = cfg_reset_value(name, dflt, arg_storage, sizeof(arg_storage));

			char err[256];
			bool ok = s ? setting_apply(s, value, err, sizeof(err), a->dry_run) : cfg_special_apply(name,
				value, err, sizeof(err), a->dry_run);
			if (!ok) {
				ipc_buf_free(&detail);
				ipc_fail(a, "%s", err);
				return;
			}

			changed++;
		}

		// listing every setting when nothing is named would bury the few that moved
		if (!named && same)
			continue;

		if (same)
			ipc_buff(&detail, "  %s %s (unchanged)\n", name, dflt);
		else
			ipc_buff(&detail, "  %s %s -> %s\n", name, before, dflt);
	}

	IPC_REPLY_GROWING(b);

	if (changed == 0) {
		ipc_buff(&b, "no change: %zu %s already at %s default\n", total,
			total == 1 ? "setting" : "settings", total == 1 ? "its" : "their");
	} else {
		ipc_buff(&b, "%s %zu of %zu %s to %s\n", a->dry_run ? "would reset" : "reset", changed, total,
			total == 1 ? "setting" : "settings", total == 1 ? "its default" : "their defaults");
	}

	ipc_buff(&b, "%s", detail.buf != NULL ? detail.buf : "");
	ipc_buf_free(&detail);

	if (missing)
		ipc_buff(&b, "%zu %s had no recorded default\n", missing, missing == 1 ? "setting" : "settings");

	ipc_buf_send(a, &b);
}

const ipc_sub_t config_subs[] = {
	IPC_SUB("-l", "--list", "config -l|--list [--json] [--changed] [--no-header]", cfg_list),
	IPC_SUBA("-r", "--reset", "reset\0", "config reset|-r|--reset [<name>...]", cfg_reset),
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

	if (streq(name, "reset")) {
		ipc_take(a);
		cfg_reset(a);
		return;
	}

	ipc_take(a);

	if (a->dry_run && cfg_special_name(name) && !cfg_special_settable(name)) {
		ipc_fail(a, "--dry-run is not supported by \"%s\"\n", name);
		return;
	}

	// old layout options
	if (ipc_cmd_layout_legacy(a, name))
		return;

	if (cfg_special_dispatch(a, name))
		return;

	const setting_t *s = find_setting(name);
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
	if (!setting_apply(s, arg, err, sizeof(err), a->dry_run)) {
		ipc_fail(a, "%s", err);
		return;
	}

	ipc_okf(a, "%s %s\n", s->name, a->dry_run ? "would be set" : "set");
}
