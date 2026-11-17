#pragma once

#include "layout/layout.h"

#include <stdbool.h>

#define DOORS_PLUGIN_API_VERSION 1

typedef struct {
	uint32_t api_version;
	const char *name;
	const layout_impl_t *const *layouts;
} doors_plugin_t;

void plugin_init(void);
bool plugin_load(const char *path);
bool plugin_is_loaded(const char *name);

typedef void (*plugin_iter_fn)(const doors_plugin_t *plugin, const char *path, void *user);
void plugin_iter(plugin_iter_fn fn, void *user);
