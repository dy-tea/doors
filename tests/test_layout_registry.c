#include "layout/layout.h"
#include "str.h"
#include "test.h"
#include <wlr/util/box.h>

static void test_arrange(output_t *m, desktop_t *d, struct wlr_box available) {
	(void)m;
	(void)d;
	(void)available;
}

static const layout_impl_t test_layout_a = {
	.name = "registry_test_a",
	.arrange = test_arrange,
	.collect = NULL,
	.single_visible = false,
	.has_directional_nav = false,
};

static const layout_impl_t test_layout_b = {
	.name = "registry_test_b",
	.arrange = NULL,
	.collect = NULL,
	.single_visible = false,
	.has_directional_nav = false,
};

static void test_builtin_registry(void) {
	CHECK(layout_count() == LAYOUT_BUILTIN_COUNT);
	CHECK_STREQ(layout_name(LAYOUT_TILED), "tiled");
	CHECK_STREQ(layout_name(LAYOUT_MONOCLE), "monocle");
	CHECK(layout_get_impl(LAYOUT_TILED) != NULL);
	CHECK(layout_get_impl((layout_t)LAYOUT_BUILTIN_COUNT) == NULL);
	CHECK(layout_to_char(LAYOUT_TILED) == 'T');
	CHECK(layout_to_char(LAYOUT_FLOATING) == 'F');

	// lookup by name covers the built-in layouts
	layout_t id;
	CHECK(layout_find_by_name("scroller", &id) && id == LAYOUT_SCROLLER);
	CHECK(!layout_find_by_name("no_such_layout", &id));
	CHECK(!layout_find_by_name(NULL, &id));
}

static void test_plugin_registration(void) {
	layout_t a = layout_register(&test_layout_a);
	CHECK(a != (layout_t)-1);
	CHECK((size_t)a == LAYOUT_BUILTIN_COUNT);
	CHECK(layout_count() == LAYOUT_BUILTIN_COUNT + 1);
	CHECK_STREQ(layout_name(a), "registry_test_a");
	CHECK(layout_get_impl(a) == &test_layout_a);

	// status chars must not collide with the built-in ones
	const char a_char = layout_to_char(a);
	CHECK(a_char != 'T' && a_char != 'M' && a_char != 'S' && a_char != 'K' && a_char != 'F');
	CHECK(layout_plugin_char(a) == a_char);
	CHECK(layout_plugin_char((layout_t)9999) == '?');
	CHECK(layout_plugin_char(LAYOUT_TILED) == '?');

	layout_t b = layout_register(&test_layout_b);
	CHECK(b == (layout_t)(a + 1));
	CHECK(layout_to_char(b) != a_char);

	// out of range ids resolve to nothing
	CHECK(layout_get_impl((layout_t)9999) == NULL);
	CHECK(layout_name((layout_t)9999) == NULL);
}

static void test_duplicate_and_invalid(void) {
	// duplicate name must be rejected
	CHECK(layout_register(&test_layout_a) == (layout_t)-1);

	// missing/empty name must be rejected
	CHECK(layout_register(NULL) == (layout_t)-1);
	const layout_impl_t unnamed = {
		.name = ""
	};
	CHECK(layout_register(&unnamed) == (layout_t)-1);
}

static void test_ipc_values(void) {
	const str_pair_t *values = layout_ipc_values();
	CHECK(values != NULL);

	size_t n = 0;
	bool found_a = false;
	for (; values[n].name != NULL; n++) {
		if (streq(values[n].name, "registry_test_a")) {
			layout_t id = 0;
			CHECK(layout_find_by_name("registry_test_a", &id));
			CHECK(values[n].value == (long)id);
			found_a = true;
		}
	}
	CHECK(found_a);
	// built-ins first, then plugin layouts
	CHECK(n == layout_count());
	CHECK_STREQ(values[0].name, "tiled");
}

int main(void) {
	layout_init();
	test_builtin_registry();
	test_plugin_registration();
	test_duplicate_and_invalid();
	test_ipc_values();
	printf("%d checks passed\n", test_checks);
	return test_failures ? 1 : 0;
}
