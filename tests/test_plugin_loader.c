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

	// a plugin layout carries its own settings and commands
	char err[128] = {0};
	CHECK(layout_setting_set(id, "gap", "20", err, sizeof(err), false));
	CHECK(err[0] == '\0');

	char value[64] = {0};
	CHECK(layout_setting_format(id, "gap", value, sizeof(value)));
	CHECK_STREQ(value, "20\n");

	// registration records the plugin's defaults for `layout strip reset`
	memset(value, 0, sizeof(value));
	CHECK(layout_setting_default(id, "gap", value, sizeof(value)));
	CHECK_STREQ(value, "0\n");
	CHECK(layout_setting_reset(id, "gap", err, sizeof(err), false));
	memset(value, 0, sizeof(value));
	CHECK(layout_setting_format(id, "gap", value, sizeof(value)));
	CHECK_STREQ(value, "0\n");
	CHECK(layout_setting_find(id, "nope") == NULL);

	const command_t *command = layout_command_find(id, "focus_first");
	CHECK(command != NULL);
	CHECK_STREQ(command->usage, "layout strip focus_first");
	CHECK(layout_command_find(id, "nope") == NULL);

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
