#include "layout/layout.h"
#include "layout/registry.h"
#include "str.h"
#include "types.h"
#include <stdlib.h>
#include <string.h>
#include <wlr/util/log.h>

#define REGISTRY_LEN(arr) (sizeof(arr) / sizeof((arr)[0]))

static const layout_impl_t *registry[] = {
	[LAYOUT_TILED] = &tiled_impl,
	[LAYOUT_MONOCLE] = &monocle_impl,
	[LAYOUT_SCROLLER] = &scroller_impl,
	[LAYOUT_MASTER_STACK] = &master_stack_impl,
	[LAYOUT_FLOATING] = &floating_impl,
};

typedef struct dynamic_layout_t {
	const layout_impl_t *impl;
	char status_char;
	layout_t id;
	struct wl_list link;
} dynamic_layout_t;

static struct wl_list dynamic_layouts;
static size_t dynamic_layout_count;
static str_pair_t *dynamic_ipc_values;

static bool build_ipc_values(void);

void layout_init(void) {
	wl_list_init(&dynamic_layouts);
	layout_defaults_init();
	for (size_t i = 0; i < REGISTRY_LEN(registry); i++) {
		if (registry[i] != NULL)
			layout_defaults_capture((layout_t)i);
	}
}

static const dynamic_layout_t *dynamic_layout_for(layout_t layout) {
	const size_t i = (size_t)layout;
	if (i < LAYOUT_BUILTIN_COUNT)
		return NULL;

	const dynamic_layout_t *entry;
	wl_list_for_each(entry, &dynamic_layouts, link) {
		if (entry->id == layout)
			return entry;
	}
	return NULL;
}

char layout_plugin_char(layout_t l) {
	const dynamic_layout_t *entry = dynamic_layout_for(l);
	return entry != NULL ? entry->status_char : '?';
}

layout_t layout_register(const layout_impl_t *impl) {
	if (impl == NULL || impl->name == NULL || impl->name[0] == '\0')
		return (layout_t)-1;

	if (layout_find_by_name(impl->name, NULL))
		return (layout_t)-1;

	const char builtin_chars[] = "TMSKF";
	char status_char = '?';
	for (char c = 'A'; c <= 'Z'; c++) {
		if (strchr(builtin_chars, c) != NULL)
			continue;

		bool taken = false;
		const dynamic_layout_t *entry;
		wl_list_for_each(entry, &dynamic_layouts, link) {
			if (entry->status_char == c) {
				taken = true;
				break;
			}
		}
		if (!taken) {
			status_char = c;
			break;
		}
	}

	dynamic_layout_t *entry = malloc(sizeof(dynamic_layout_t));
	if (entry == NULL) {
		wlr_log(WLR_ERROR, "failed to allocate registry entry for layout \"%s\"", impl->name);
		return (layout_t)-1;
	}

	entry->impl = impl;
	entry->status_char = status_char;
	entry->id = (layout_t)(LAYOUT_BUILTIN_COUNT + dynamic_layout_count);
	wl_list_insert(dynamic_layouts.prev, &entry->link);
	dynamic_layout_count++;
	layout_defaults_capture(entry->id);

	if (!build_ipc_values())
		wlr_log(WLR_ERROR, "failed to rebuild layout IPC table");

	wlr_log(WLR_INFO, "registered layout \"%s\" (id %d)", impl->name, entry->id);
	return entry->id;
}

size_t layout_count(void) {
	return LAYOUT_BUILTIN_COUNT + dynamic_layout_count;
}

const char *layout_name(layout_t layout) {
	const size_t i = (size_t)layout;
	if (i < REGISTRY_LEN(registry))
		return registry[i] != NULL ? registry[i]->name : NULL;

	const dynamic_layout_t *entry = dynamic_layout_for(layout);
	return entry != NULL ? entry->impl->name : NULL;
}

bool layout_find_by_name(const char *name, layout_t *out) {
	if (name == NULL)
		return false;

	for (size_t i = 0; i < REGISTRY_LEN(registry); i++) {
		if (registry[i] != NULL && streq(registry[i]->name, name)) {
			if (out != NULL)
				*out = (layout_t)i;
			return true;
		}
	}

	const dynamic_layout_t *entry;
	wl_list_for_each(entry, &dynamic_layouts, link) {
		if (streq(entry->impl->name, name)) {
			if (out != NULL)
				*out = entry->id;
			return true;
		}
	}

	return false;
}

layout_t layout_id_for(const layout_impl_t *impl) {
	if (impl == NULL)
		return (layout_t)-1;

	for (size_t i = 0; i < REGISTRY_LEN(registry); i++) {
		if (registry[i] == impl)
			return (layout_t)i;
	}

	const dynamic_layout_t *entry;
	wl_list_for_each(entry, &dynamic_layouts, link) {
		if (entry->impl == impl)
			return entry->id;
	}

	return (layout_t)-1;
}

const str_pair_t *layout_ipc_values(void) {
	if (dynamic_ipc_values == NULL)
		build_ipc_values();
	return dynamic_ipc_values;
}

const layout_impl_t *layout_get_impl(layout_t layout) {
	const size_t i = (size_t)layout;
	if (i < REGISTRY_LEN(registry))
		return registry[i];

	const dynamic_layout_t *entry = dynamic_layout_for(layout);
	return entry != NULL ? entry->impl : NULL;
}

static bool build_ipc_values(void) {
	str_pair_t *values = malloc((dynamic_layout_count + 6) * sizeof(str_pair_t));
	if (values == NULL)
		return false;

	static const str_pair_t builtins[] = {
		{"tiled", LAYOUT_TILED},
		{"monocle", LAYOUT_MONOCLE},
		{"scroller", LAYOUT_SCROLLER},
		{"master_stack", LAYOUT_MASTER_STACK},
		{"floating", LAYOUT_FLOATING},
	};
	size_t n = 0;
	for (size_t i = 0; i < REGISTRY_LEN(builtins); i++)
		values[n++] = builtins[i];
	const dynamic_layout_t *entry;
	wl_list_for_each(entry, &dynamic_layouts, link)
		values[n++] = (str_pair_t){
			entry->impl->name,
			(long)entry->id
		};
	values[n] = (str_pair_t){
		NULL,
		0
	};

	free(dynamic_ipc_values);
	dynamic_ipc_values = values;
	return true;
}
