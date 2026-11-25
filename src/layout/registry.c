#include "layout/layout.h"
#include "layout/registry.h"
#include "str.h"
#include "types.h"
#include <stdlib.h>
#include <string.h>
#include <wlr/util/log.h>

#define REGISTRY_LEN(arr) (sizeof(arr) / sizeof((arr)[0]))

static const struct {
	layout_t id;
	char status_char;
	const layout_impl_t *impl;
} builtin_layouts[] = {
	{LAYOUT_TILED, 'T', &tiled_impl},
	{LAYOUT_MONOCLE, 'M', &monocle_impl},
	{LAYOUT_SCROLLER, 'S', &scroller_impl},
	{LAYOUT_MASTER_STACK, 'K', &master_stack_impl},
	{LAYOUT_FLOATING, 'F', &floating_impl},
};

typedef struct layout_entry_t {
	layout_t id;
	char status_char;
	const layout_impl_t *impl;
	struct wl_list link;
} layout_entry_t;

static struct wl_list layouts;
static size_t layout_total;
static bool layouts_ready;
static bool defaults_ready;
static str_pair_t *layout_ipc_table;

static bool build_ipc_values(void);

static layout_entry_t *entry_for(const layout_t id) {
	layout_entry_t *entry;
	wl_list_for_each(entry, &layouts, link) {
		if (entry->id == id)
			return entry;
	}
	return NULL;
}

static char next_status_char(void) {
	for (char c = 'A'; c <= 'Z'; c++) {
		bool taken = false;
		const layout_entry_t *entry;
		wl_list_for_each(entry, &layouts, link) {
			if (entry->status_char == c) {
				taken = true;
				break;
			}
		}
		if (!taken)
			return c;
	}
	return '?';
}

static layout_t next_layout_id(void) {
	layout_t id = 0;
	const layout_entry_t *entry;
	wl_list_for_each(entry, &layouts, link) {
		if (entry->id >= id)
			id = entry->id + 1;
	}
	return id;
}

static layout_entry_t *entry_add(const layout_impl_t *impl, const char status_char,
		const layout_t id) {
	if (entry_for(id) != NULL) {
		wlr_log(WLR_ERROR, "layout id %d is already registered", (int)id);
		return NULL;
	}

	layout_entry_t *entry = malloc(sizeof(*entry));
	if (entry == NULL)
		return NULL;

	entry->impl = impl;
	entry->status_char = status_char;
	entry->id = id;
	wl_list_insert(layouts.prev, &entry->link);
	layout_total++;
	return entry;
}

static void layouts_ensure(void) {
	if (layouts_ready)
		return;

	layouts_ready = true;
	wl_list_init(&layouts);

	for (size_t i = 0; i < REGISTRY_LEN(builtin_layouts); i++) {
		const layout_impl_t *impl = builtin_layouts[i].impl;
		if (entry_add(impl, builtin_layouts[i].status_char, builtin_layouts[i].id) == NULL)
			wlr_log(WLR_ERROR, "failed to register built-in layout \"%s\"", impl->name);
	}
}

void layout_init(void) {
	layouts_ensure();
	layout_defaults_init();
	defaults_ready = true;

	const layout_entry_t *entry;
	wl_list_for_each(entry, &layouts, link)
		layout_defaults_capture(entry->id);
}

layout_t layout_register(const layout_impl_t *impl) {
	layouts_ensure();

	if (impl == NULL || impl->name == NULL || impl->name[0] == '\0')
		return (layout_t)-1;

	if (layout_find_by_name(impl->name, NULL))
		return (layout_t)-1;

	layout_entry_t *entry = entry_add(impl, next_status_char(), next_layout_id());
	if (entry == NULL) {
		wlr_log(WLR_ERROR, "failed to allocate registry entry for layout \"%s\"", impl->name);
		return (layout_t)-1;
	}

	if (defaults_ready)
		layout_defaults_capture(entry->id);

	if (!build_ipc_values())
		wlr_log(WLR_ERROR, "failed to rebuild layout IPC table");

	wlr_log(WLR_INFO, "registered layout \"%s\" (id %d)", impl->name, (int)entry->id);
	return entry->id;
}

size_t layout_count(void) {
	layouts_ensure();
	return layout_total;
}

const char *layout_name(const layout_t layout) {
	layouts_ensure();
	const layout_entry_t *entry = entry_for(layout);
	return entry != NULL ? entry->impl->name : NULL;
}

char layout_to_char(const layout_t layout) {
	layouts_ensure();
	const layout_entry_t *entry = entry_for(layout);
	return entry != NULL ? entry->status_char : '?';
}

bool layout_find_by_name(const char *name, layout_t *out) {
	layouts_ensure();
	if (name == NULL)
		return false;

	const layout_entry_t *entry;
	wl_list_for_each(entry, &layouts, link) {
		if (streq(entry->impl->name, name)) {
			if (out != NULL)
				*out = entry->id;
			return true;
		}
	}

	return false;
}

layout_t layout_id_for(const layout_impl_t *impl) {
	layouts_ensure();
	if (impl == NULL)
		return (layout_t)-1;

	const layout_entry_t *entry;
	wl_list_for_each(entry, &layouts, link) {
		if (entry->impl == impl)
			return entry->id;
	}

	return (layout_t)-1;
}

const str_pair_t *layout_ipc_values(void) {
	layouts_ensure();
	if (layout_ipc_table == NULL)
		build_ipc_values();
	return layout_ipc_table;
}

const layout_impl_t *layout_get_impl(const layout_t layout) {
	layouts_ensure();
	const layout_entry_t *entry = entry_for(layout);
	return entry != NULL ? entry->impl : NULL;
}

static bool build_ipc_values(void) {
	layouts_ensure();

	str_pair_t *values = malloc((layout_total + 1) * sizeof(str_pair_t));
	if (values == NULL)
		return false;

	size_t n = 0;
	const layout_entry_t *entry;
	wl_list_for_each(entry, &layouts, link)
		values[n++] = (str_pair_t){
			entry->impl->name,
			(long)entry->id
		};
	values[n] = (str_pair_t){
		NULL,
		0
	};

	free(layout_ipc_table);
	layout_ipc_table = values;
	return true;
}
