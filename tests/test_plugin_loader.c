#include "layout/layout.h"
#include "plugin.h"
#include "test.h"

#ifndef STRIP_PLUGIN_PATH
#define STRIP_PLUGIN_PATH "strip.so"
#endif

static void count_plugin(const doors_plugin_t *p, const char *path, void *user) {
	(void)p;
	(void)path;
	(*(int *)user)++;
}

static void test_load(void) {
	layout_init();
	plugin_init();
	CHECK(layout_count() == LAYOUT_BUILTIN_COUNT);
	CHECK(!plugin_is_loaded("strip"));

	CHECK(!plugin_load("/fakepath/no_such_plugin.so"));
	CHECK(layout_count() == LAYOUT_BUILTIN_COUNT);

	CHECK(plugin_load(STRIP_PLUGIN_PATH));
	CHECK(plugin_is_loaded("strip"));
	CHECK(layout_count() == LAYOUT_BUILTIN_COUNT + 1);

	layout_t id = (layout_t)-1;
	CHECK(layout_find_by_name("strip", &id));
	CHECK((size_t)id == LAYOUT_BUILTIN_COUNT);
	CHECK_STREQ(layout_name(id), "strip");
	CHECK(layout_get_impl(id) != NULL);
	CHECK_STREQ(layout_get_impl(id)->name, "strip");

	CHECK(!plugin_load(STRIP_PLUGIN_PATH));
	CHECK(layout_count() == LAYOUT_BUILTIN_COUNT + 1);

	int listed = 0;
	plugin_iter(count_plugin, &listed);
	CHECK(listed == 1);
}

int main(void) {
	test_load();
	printf("%d checks passed\n", test_checks);
	return test_failures ? 1 : 0;
}
