#include "config_internal.h"
#include "animation.h"
#include "bezier.h"
#include "effects/effects.h"
#include "ipc/helpers.h"
#include "layout/scroller.h"
#include "output/output.h"
#include "server.h"
#include "settings.h"
#include "spring.h"
#include "str.h"
#include "tabs.h"
#include "transaction.h"
#include "tree.h"
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

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

		const char *value;
		if (!ipc_need(a, "value", &value))
			return;

		float parsed[4];
		if (!ipc_parse_color_float(value, parsed)) {
			ipc_fail(a, "tab_color_%s: expected \"R G B [A]\"\n", colors[i].name);
			return;
		}

		memcpy(colors[i].color, parsed, sizeof(parsed));
		tabs_rebuild_all();
		ipc_okf(a, "tab_color_%s set\n", colors[i].name);
		return;
	}

	ipc_fail(a, "Unknown tab color \"%s\"\n", suffix);
}

static bool cfg_parse_scroller_size(const char *tok, scroller_size_t *out) {
	size_t len = strlen(tok);

	if (len > 2 && streq(tok + len - 2, "px")) {
		char *end;
		double val = strtod(tok, &end);
		if (end != tok + len - 2 || !isfinite(val) || val <= 0.0 || val > 65535.0)
			return false;

		out->type = SCROLLER_SIZE_FIXED;
		out->value = val;
		return true;
	}

	char *end;
	double val = strtod(tok, &end);
	if (end == tok || *end != '\0' || !isfinite(val) || val <= 0.0 || val > 1.0)
		return false;

	out->type = SCROLLER_SIZE_PROPORTION;
	out->value = val;
	return true;
}

void cfg_format_scroller_size(scroller_size_t size, ipc_buf_t *b, bool last) {
	if (size.type == SCROLLER_SIZE_FIXED)
		ipc_buff(b, "%.0fpx%s", size.value, last ? "\n" : ",");
	else
		ipc_buff(b, "%.2f%s", size.value, last ? "\n" : ",");
}

static void on_scroller_default_column_width(void) {
	output_t *m;
	wl_list_for_each(m, &mon_list, link) {
		desktop_t *d;
		wl_list_for_each(d, &m->desk_list, link)
			if (d->scroller_state)
				d->scroller_state->default_column_width = settings.scroller_default_column_width;
	}
}

static void cfg_scroller_preset_list(ipc_args_t *a, const char *name, const scroller_size_t *slot,
		int count_slot, void (*store)(scroller_size_t * , int)) {
	if (!ipc_peek(a)) {
		char buf[512];
		ipc_buf_t b;
		ipc_buf_init(&b, buf, sizeof(buf));
		for (int i = 0; i < count_slot; i++)
			cfg_format_scroller_size(slot[i], &b, i == count_slot - 1);
		ipc_buf_send(a, &b);
		return;
	}

	const char *value;
	if (!ipc_need(a, "comma separated values", &value))
		return;

	char *copy = strdup(value);
	if (!copy) {
		ipc_fail(a, "Memory allocation failed\n");
		return;
	}

	size_t len = strlen(copy);
	if (len >= 2 && copy[0] == '[' && copy[len - 1] == ']') {
		memmove(copy, copy + 1, len - 2);
		copy[len - 2] = '\0';
	}

	int count = 1;
	for (char *p = copy; *p; p++) {
		if (*p == ',')
			count++;
	}

	scroller_size_t *presets = malloc((size_t)count * sizeof(*presets));
	if (!presets) {
		free(copy);
		ipc_fail(a, "Memory allocation failed\n");
		return;
	}

	int i = 0;
	for (char *tok = strtok(copy, ","); tok && i < count; tok = strtok(NULL, ",")) {
		if (!cfg_parse_scroller_size(tok, &presets[i])) {
			ipc_fail(a, "%s: invalid value \"%s\", expected a proportion in (0, 1] or a size like "
				"\"640px\"\n", name, tok);
			free(presets);
			free(copy);
			return;
		}
		i++;
	}
	free(copy);

	store(presets, i);
	ipc_okf(a, "%s set\n", name);
}

static void cfg_scroller_preset_column_widths(ipc_args_t *a) {
	cfg_scroller_preset_list(a, "scroller_preset_column_widths", settings.scroller_preset_column_widths,
		settings.scroller_preset_column_widths_count, settings_set_scroller_preset_column_widths);
}

static void cfg_scroller_preset_window_heights(ipc_args_t *a) {
	cfg_scroller_preset_list(a, "scroller_preset_window_heights",
		settings.scroller_preset_window_heights, settings.scroller_preset_window_heights_count,
		settings_set_scroller_preset_window_heights);
}

// scroller_default_column_width accepts either a proportion or a "Npx" fixed size.
static void cfg_scroller_default_column_width(ipc_args_t *a) {
	if (!ipc_peek(a)) {
		char buf[64];
		ipc_buf_t b;
		ipc_buf_init(&b, buf, sizeof(buf));
		cfg_format_scroller_size(settings.scroller_default_column_width, &b, true);
		ipc_buf_send(a, &b);
		return;
	}

	const char *value;
	if (!ipc_need(a, "proportion or px size", &value))
		return;

	scroller_size_t size;
	if (!cfg_parse_scroller_size(value, &size)) {
		ipc_fail(a, "scroller_default_column_width: invalid value \"%s\", expected a proportion in "
			"(0, 1] or a size like \"640px\"\n", value);
		return;
	}

	settings.scroller_default_column_width = size;
	on_scroller_default_column_width();
	ipc_ok(a, "scroller_default_column_width set\n");
}

// <normal|active|focused>_border_gradient[2|_lerp]
#include "config_internal.h"

#include "effects/effects.h"
#include "ipc/helpers.h"
#include "output/output.h"
#include "server.h"
#include "settings.h"
#include "tabs.h"
#include "transaction.h"
#include "tree.h"
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// the gradient table backs both the border gradient settings and their entries
// in the `config --list` output
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

const cfg_gradient_t *cfg_find_gradient(const char *name) {
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
		ipc_buff(&b, "enabled: %s\n", str_bool_text(enabled));
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
	{"scroller_preset_column_widths", cfg_scroller_preset_column_widths},
	{"scroller_proportion_preset", cfg_scroller_preset_column_widths},
	{"scroller_preset_window_heights", cfg_scroller_preset_window_heights},
	{"scroller_default_column_width", cfg_scroller_default_column_width},
	{"scroller_default_proportion", cfg_scroller_default_column_width},
	{"acrylic_light_anchor", cfg_acrylic_light_anchor},
	{"blur_algorithm", cfg_blur_algorithm},
	{"screen_shader", cfg_screen_shader},
	{"screen_shader_file", cfg_screen_shader_file},
	{"animation_bezier", cfg_animation_bezier},
	{"animation_duration", cfg_animation_duration},
	{"animation", cfg_animation},
	{NULL, NULL},
};

// settings that need their own syntax rather than the plain table entry
bool cfg_special_name(const char *name) {
	if (!name || name[0] == '\0')
		return false;

	if (str_skip_prefix(name, "tab_color_"))
		return true;

	if (cfg_find_gradient(name))
		return true;

	for (const cfg_named_handler_t *sp = special_settings; sp->name; sp++) {
		if (streq(name, sp->name))
			return true;
	}

	return false;
}

bool cfg_special_dispatch(ipc_args_t *a, const char *name) {
	const char *tab_color = str_skip_prefix(name, "tab_color_");
	if (tab_color) {
		cfg_tab_color(a, tab_color);
		return true;
	}

	const cfg_gradient_t *grad = cfg_find_gradient(name);
	if (grad) {
		cfg_border_gradient(a, grad);
		return true;
	}

	for (const cfg_named_handler_t *sp = special_settings; sp->name; sp++) {
		if (!streq(name, sp->name))
			continue;

		sp->fn(a);
		return true;
	}

	return false;
}
