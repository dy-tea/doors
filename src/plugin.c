#include "plugin.h"
#include "str.h"
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/util/log.h>

typedef struct plugin_entry_t {
	void *handle;
	char *path;
	const doors_plugin_t *plugin;
	struct wl_list link;
} plugin_entry_t;

static struct wl_list plugins;

void plugin_init(void) {
	wl_list_init(&plugins);
}

static plugin_entry_t *plugin_find(const char *name) {
	plugin_entry_t *entry;
	wl_list_for_each(entry, &plugins, link) {
		const doors_plugin_t *p = entry->plugin;
		if (p != NULL && p->name != NULL && streq(p->name, name))
			return entry;
	}
	return NULL;
}

bool plugin_is_loaded(const char *name) {
	return name != NULL && plugin_find(name) != NULL;
}

bool plugin_load(const char *path) {
	if (path == NULL || path[0] == '\0')
		return false;

	void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (handle == NULL) {
		wlr_log(WLR_ERROR, "failed to load plugin \"%s\": %s", path, dlerror());
		return false;
	}

	dlerror();
	const doors_plugin_t *plugin = dlsym(handle, "doors_plugin");
	if (plugin == NULL) {
		wlr_log(WLR_ERROR, "plugin \"%s\" does not export \"doors_plugin\": %s", path, dlerror());
		dlclose(handle);
		return false;
	}

	if (plugin->api_version != DOORS_PLUGIN_API_VERSION) {
		wlr_log(WLR_ERROR, "plugin \"%s\" was built for plugin API %u, but this compositor provides %u",
			path, plugin->api_version, DOORS_PLUGIN_API_VERSION);
		dlclose(handle);
		return false;
	}

	const char *name = plugin->name != NULL ? plugin->name : path;
	if (plugin_is_loaded(name)) {
		wlr_log(WLR_ERROR, "plugin \"%s\" is already loaded", name);
		dlclose(handle);
		return false;
	}

	if (plugin->layouts == NULL) {
		wlr_log(WLR_ERROR, "plugin \"%s\" provides no layouts", name);
		dlclose(handle);
		return false;
	}

	size_t registered = 0;
	for (size_t i = 0; plugin->layouts[i] != NULL; i++) {
		const layout_t id = layout_register(plugin->layouts[i]);
		if (id != (layout_t)-1)
			registered++;
		else
			wlr_log(WLR_ERROR, "plugin \"%s\": failed to register layout \"%s\"", name,
				plugin->layouts[i]->name != NULL ? plugin->layouts[i]->name : "?");
	}

	if (registered == 0) {
		wlr_log(WLR_ERROR, "plugin \"%s\" registered no layouts", name);
		dlclose(handle);
		return false;
	}

	plugin_entry_t *entry = malloc(sizeof(plugin_entry_t));
	if (entry == NULL) {
		wlr_log(WLR_ERROR, "failed to allocate plugin entry for \"%s\"", name);
		dlclose(handle);
		return false;
	}

	entry->handle = handle;
	entry->path = strdup(path);
	entry->plugin = plugin;
	wl_list_insert(plugins.prev, &entry->link);

	wlr_log(WLR_INFO, "loaded plugin \"%s\" (%zu layouts) from %s", name, registered, path);
	return true;
}

void plugin_iter(plugin_iter_fn fn, void *user) {
	if (fn == NULL)
		return;

	plugin_entry_t *entry;
	wl_list_for_each(entry, &plugins, link) {
		const doors_plugin_t *p = entry->plugin;
		fn(p, entry->path, user);
	}
}
