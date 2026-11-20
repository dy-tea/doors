#include "ipc/args.h"
#include "layout/layout.h"
#include "str.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>

const setting_t *layout_setting_find(const layout_t layout, const char *name) {
	if (name == NULL)
		return NULL;

	const layout_impl_t *impl = layout_get_impl(layout);
	if (impl == NULL || impl->settings == NULL)
		return NULL;

	for (size_t i = 0; i < impl->settings_count; i++) {
		const setting_t *s = &impl->settings[i];
		if (streq(name, s->name) || (s->alias != NULL && streq(name, s->alias)))
			return s;
	}
	return NULL;
}

const command_t *layout_command_find(const layout_t layout, const char *name) {
	if (name == NULL)
		return NULL;

	const layout_impl_t *impl = layout_get_impl(layout);
	if (impl == NULL || impl->commands == NULL)
		return NULL;

	for (const command_t *c = impl->commands; c->name != NULL; c++) {
		if (streq(name, c->name))
			return c;
	}
	return NULL;
}

bool layout_setting_format(const layout_t layout, const char *name, char *buf, const size_t bufsz) {
	const setting_t *s = layout_setting_find(layout, name);
	if (s == NULL)
		return false;

	setting_format(s, buf, bufsz);
	return true;
}

bool layout_setting_set(const layout_t layout, const char *name, const char *value, char *err,
		const size_t errsz, const bool dry_run) {
	const setting_t *s = layout_setting_find(layout, name);
	if (s == NULL) {
		setting_err(err, errsz, "unknown setting \"%s\"\n", name);
		return false;
	}

	return setting_apply(s, value, err, errsz, dry_run);
}

typedef struct layout_defaults_t {
	layout_t layout;
	unsigned char *data;
	size_t *offset;
	size_t *size;
	size_t count;
	struct wl_list link;
} layout_defaults_t;

static struct wl_list layout_defaults;
static bool layout_defaults_ready;

size_t setting_storage_size(const setting_t *s) {
	switch (s->type) {
	case SETTING_BOOL:
		return sizeof(bool);
	case SETTING_INT:
	case SETTING_ENUM:
		return sizeof(int);
	case SETTING_FLOAT:
		return sizeof(float);
	case SETTING_DOUBLE:
		return sizeof(double);
	case SETTING_STR:
		return s->size;
	case SETTING_RGBA:
		return 4 * sizeof(float);
	}
	return 0;
}

static size_t setting_storage_align(const setting_t *s) {
	switch (s->type) {
	case SETTING_BOOL:
		return _Alignof(bool);
	case SETTING_INT:
	case SETTING_ENUM:
		return _Alignof(int);
	case SETTING_FLOAT:
	case SETTING_RGBA:
		return _Alignof(float);
	case SETTING_DOUBLE:
		return _Alignof(double);
	case SETTING_STR:
		return 1;
	}
	return 1;
}

static void defaults_entry_free(layout_defaults_t *entry) {
	free(entry->data);
	free(entry->offset);
	free(entry->size);
	free(entry);
}

void layout_defaults_init(void) {
	if (layout_defaults_ready) {
		layout_defaults_t *entry, *tmp;
		wl_list_for_each_safe(entry, tmp, &layout_defaults, link) {
			wl_list_remove(&entry->link);
			defaults_entry_free(entry);
		}
	}

	wl_list_init(&layout_defaults);
	layout_defaults_ready = true;
}

static layout_defaults_t *defaults_for(const layout_t layout) {
	if (!layout_defaults_ready)
		return NULL;

	layout_defaults_t *entry;
	wl_list_for_each(entry, &layout_defaults, link) {
		if (entry->layout == layout)
			return entry;
	}
	return NULL;
}

void layout_defaults_capture(const layout_t layout) {
	const layout_impl_t *impl = layout_get_impl(layout);
	if (impl == NULL || impl->settings == NULL || impl->settings_count == 0)
		return;
	if (defaults_for(layout) != NULL)
		return;

	layout_defaults_t *entry = calloc(1, sizeof(*entry));
	if (entry == NULL)
		return;

	entry->count = impl->settings_count;
	entry->offset = calloc(entry->count, sizeof(*entry->offset));
	entry->size = calloc(entry->count, sizeof(*entry->size));
	if (entry->offset == NULL || entry->size == NULL) {
		defaults_entry_free(entry);
		return;
	}

	size_t total = 0;
	for (size_t i = 0; i < entry->count; i++) {
		const size_t align = setting_storage_align(&impl->settings[i]);
		total = (total + align - 1) & ~(align - 1);

		entry->offset[i] = total;
		entry->size[i] = setting_storage_size(&impl->settings[i]);
		total += entry->size[i];
	}

	entry->data = malloc(total > 0 ? total : 1);
	if (entry->data == NULL) {
		defaults_entry_free(entry);
		return;
	}

	for (size_t i = 0; i < entry->count; i++)
		memcpy(entry->data + entry->offset[i], impl->settings[i].ptr, entry->size[i]);

	entry->layout = layout;
	wl_list_insert(layout_defaults.prev, &entry->link);
}

// the recorded default storage of a setting, NULL when it was not captured
static const void *setting_default(const layout_t layout, const setting_t *s) {
	const layout_impl_t *impl = layout_get_impl(layout);
	if (impl == NULL || impl->settings == NULL)
		return NULL;

	size_t index = SIZE_MAX;
	for (size_t i = 0; i < impl->settings_count; i++) {
		if (&impl->settings[i] == s) {
			index = i;
			break;
		}
	}
	if (index == SIZE_MAX)
		return NULL;

	const layout_defaults_t *entry = defaults_for(layout);
	if (entry == NULL || index >= entry->count)
		return NULL;

	return entry->data + entry->offset[index];
}

bool layout_setting_default(const layout_t layout, const char *name, char *buf,
		const size_t bufsz) {
	const setting_t *s = layout_setting_find(layout, name);
	if (s == NULL)
		return false;

	const void *value = setting_default(layout, s);
	if (value == NULL)
		return false;

	setting_format_value(s, value, buf, bufsz);
	return true;
}

bool layout_setting_reset(const layout_t layout, const char *name, char *err, const size_t errsz,
		const bool dry_run) {
	const setting_t *s = layout_setting_find(layout, name);
	if (s == NULL) {
		setting_err(err, errsz, "unknown setting \"%s\"\n", name);
		return false;
	}

	char dflt[512];
	if (!layout_setting_default(layout, s->name, dflt, sizeof(dflt))) {
		setting_err(err, errsz, "%s: no recorded default\n", s->name);
		return false;
	}

	str_rtrim(dflt);
	return setting_apply(s, dflt, err, errsz, dry_run);
}

typedef struct {
	const char *name;
	const char *layout;
	const char *option;
} layout_legacy_t;

static const layout_legacy_t legacy_settings[] = {
	{"scroller_always_center_single_column", "scroller", "always_center_single_column"},
	{"scroller_center_focused_column", "scroller", "center_focused_column"},
	{"scroller_left_strut", "scroller", "left_strut"},
	{"scroller_right_strut", "scroller", "right_strut"},
	{"scroller_top_strut", "scroller", "top_strut"},
	{"scroller_bottom_strut", "scroller", "bottom_strut"},
	{"scroller_pan_fingers", "scroller", "pan_fingers"},
	{"scroller_snap_percent", "scroller", "snap_percent"},
	{"scroller_default_column_width", "scroller", "default_column_width"},
	{"scroller_default_proportion", "scroller", "default_column_width"},
	{"scroller_preset_column_widths", "scroller", "preset_column_widths"},
	{"scroller_proportion_preset", "scroller", "preset_column_widths"},
	{"scroller_preset_window_heights", "scroller", "preset_window_heights"},
	{"borderless_monocle", "monocle", "borderless"},
	{"gapless_monocle", "monocle", "gapless"},
	{"monocle_top_padding", "monocle", "top_padding"},
	{"monocle_right_padding", "monocle", "right_padding"},
	{"monocle_bottom_padding", "monocle", "bottom_padding"},
	{"monocle_left_padding", "monocle", "left_padding"},
};

bool layout_legacy_lookup(const char *name, const char **layout_out, const char **option) {
	if (name == NULL)
		return false;

	for (size_t i = 0; i < IPC_ARRAY_LEN(legacy_settings); i++) {
		if (!streq(name, legacy_settings[i].name))
			continue;

		if (layout_out != NULL)
			*layout_out = legacy_settings[i].layout;
		if (option != NULL)
			*option = legacy_settings[i].option;
		return true;
	}

	return false;
}
