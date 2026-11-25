#include "layout/layout.h"
#include "rule.h"
#include "settings.h"
#include "str.h"
#include "test.h"
#include <wlr/util/box.h>
#include <wlr/util/edges.h>

static void test_arrange(output_t *m, desktop_t *d, struct wlr_box available) {
	(void)m;
	(void)d;
	(void)available;
}

static const layout_impl_t test_layout_a = {
	.name = "registry_test_a",
	.arrange = test_arrange,
	.single_visible = false,
};

static const layout_impl_t test_layout_b = {
	.name = "registry_test_b",
	.arrange = NULL,
	.single_visible = false,
};

static int test_hooks_created;
static int test_hooks_rules;

static void test_client_created(client_t *c) {
	(void)c;
	test_hooks_created++;
}

static void test_apply_rules(client_t *c, const rule_consequence_t *rule) {
	(void)c;
	if (rule->has & RULE_TYPE_STATE)
		test_hooks_rules++;
}

static const layout_impl_t test_layout_hooks = {
	.name = "registry_test_hooks",
	.client_created = test_client_created,
	.apply_rules = test_apply_rules,
	.single_visible = false,
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

static void test_layout_queries(void) {
	desktop_t d = {0};
	wl_list_init(&d.layout_states);

	client_t c = {0};
	node_t n = {0};
	n.client = &c;
	n.desktop = &d;

	uint32_t edges = 0;
	double initial = 0.0;

	// layouts without a callback leave the decisions to the split tree
	d.layout = LAYOUT_TILED;
	CHECK(layout_wants_visible(&d, &n));
	CHECK(!layout_borderless(&d, &c));
	CHECK(!layout_reports_maximized(&d, &c));
	CHECK(!layout_single_visible(&d));
	CHECK(!layout_resize_edges(&d, &n, &edges));
	CHECK(!layout_pointer_resize_begin(NULL, &d, &n, &initial));
	CHECK(!layout_pointer_resize_update(NULL, &d, &n, 1.0));
	CHECK(layout_pan_fingers(&d) == 0);
	CHECK(layout_focus_scroll_distance(&d, &n) == 0.0);
	CHECK(!layout_focus_after_unmap(NULL, &d, &n));
	CHECK(!layout_resize(NULL, &d, DIR_EAST, 0.05));

	// monocle answers the layout policy questions itself
	d.layout = LAYOUT_MONOCLE;
	CHECK(layout_single_visible(&d));
	CHECK(!layout_wants_visible(&d, &n));

	d.focus = &n;
	CHECK(layout_wants_visible(&d, &n));
	CHECK(layout_reports_maximized(&d, &c));

	char err[128];
	CHECK(layout_setting_set(LAYOUT_MONOCLE, "borderless", "true", err, sizeof(err), false));
	CHECK(layout_borderless(&d, &c));
	CHECK(layout_setting_set(LAYOUT_MONOCLE, "borderless", "false", err, sizeof(err), false));
	CHECK(!layout_borderless(&d, &c));

	CHECK(layout_setting_set(LAYOUT_MONOCLE, "gapless", "true", err, sizeof(err), false));
	CHECK(layout_window_gap(&d) == 0);
	CHECK(layout_setting_set(LAYOUT_MONOCLE, "gapless", "false", err, sizeof(err), false));
	settings.smart_gaps = false;
	d.window_gap = 7;
	CHECK(layout_window_gap(&d) == 7);

	// tiled leaves still use the global gap
	d.layout = LAYOUT_TILED;
	CHECK(layout_window_gap(&d) == 7);

	// scroller owns its resizes and pans whenever it is the active layout
	d.layout = LAYOUT_SCROLLER;
	CHECK(layout_setting_set(LAYOUT_SCROLLER, "pan_fingers", "3", err, sizeof(err), false));
	CHECK(layout_pan_fingers(&d) == 3);
	CHECK(layout_resize_edges(&d, &n, &edges));
	CHECK(edges == (WLR_EDGE_LEFT | WLR_EDGE_RIGHT));
	CHECK(layout_resize(NULL, &d, DIR_EAST, 0.05));
	CHECK(layout_resize(NULL, &d, DIR_SOUTH, 0.05));
}

// every declared setting is well formed, and no name is claimed twice
static void test_layout_settings(void) {
	for (size_t l = 0; l < layout_count(); l++) {
		const layout_impl_t *impl = layout_get_impl((layout_t)l);
		CHECK(impl != NULL);
		if (impl == NULL)
			continue;

		for (size_t i = 0; i < impl->settings_count; i++) {
			const setting_t *s = &impl->settings[i];

			CHECK(s->name != NULL && s->name[0] != '\0');
			CHECK(s->ptr != NULL);
			CHECK(layout_setting_find((layout_t)l, s->name) == s);

			if (s->type == SETTING_ENUM)
				CHECK(s->values != NULL);
			if (s->type == SETTING_INT || s->type == SETTING_FLOAT || s->type == SETTING_DOUBLE)
				CHECK(s->fmt != NULL);

			for (size_t j = i + 1; j < impl->settings_count; j++)
				CHECK(!streq(s->name, impl->settings[j].name));

			CHECK(layout_command_find((layout_t)l, s->name) == NULL);
		}

		if (impl->commands != NULL) {
			for (const command_t *c = impl->commands; c->name != NULL; c++)
				CHECK(layout_command_find((layout_t)l, c->name) == c);
		}
	}
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

static void test_layout_identity_and_state(void) {
	layout_t a;
	CHECK(layout_find_by_name("registry_test_a", &a));
	CHECK(layout_id_for(&test_layout_a) == a);
	CHECK(layout_id_for(layout_get_impl(LAYOUT_TILED)) == LAYOUT_TILED);
	CHECK(layout_id_for(layout_get_impl(LAYOUT_MONOCLE)) == LAYOUT_MONOCLE);
	CHECK(layout_id_for(NULL) == (layout_t)-1);

	const layout_impl_t unregistered = {
		.name = "registry_test_unregistered"
	};
	CHECK(layout_id_for(&unregistered) == (layout_t)-1);

	desktop_t d = {0};
	wl_list_init(&d.layout_states);

	int *state = layout_state_get_impl(&d, &test_layout_a, sizeof(*state), NULL);
	CHECK(state != NULL);
	if (state != NULL)
		*state = 42;

	CHECK(layout_state_peek_impl(&d, &test_layout_a) == state);
	layout_state_drop_impl(&d, &test_layout_a);
	CHECK(layout_state_peek_impl(&d, &test_layout_a) == NULL);
	CHECK(layout_state_get_impl(&d, &unregistered, sizeof(int), NULL) == NULL);
	layout_states_clear(&d);

	// floating placement is a layout policy core asks for by layout id
	CHECK(layout_has_floating_placement(LAYOUT_FLOATING));
	CHECK(!layout_has_floating_placement(LAYOUT_TILED));

	client_t c = {0};
	wl_list_init(&c.layout_states);
	c.state = STATE_FLOATING;
	CHECK(layout_places_floating(LAYOUT_FLOATING, &d, &c));
	CHECK(!layout_places_floating(LAYOUT_TILED, &d, &c));

	c.state = STATE_TILED;
	CHECK(!layout_places_floating(LAYOUT_FLOATING, &d, &c));
	CHECK(!layout_places_floating((layout_t)9999, &d, &c));

	// per-client state works like the per-desktop one
	int *client_state = client_layout_state_get_impl(&c, &test_layout_a, sizeof(*client_state), NULL);
	CHECK(client_state != NULL);
	if (client_state != NULL)
		*client_state = 7;

	CHECK(client_layout_state_peek_impl(&c, &test_layout_a) == client_state);
	CHECK(client_layout_state_peek_impl(&c, &unregistered) == NULL);
	client_layout_state_drop_impl(&c, &test_layout_a);
	CHECK(client_layout_state_peek_impl(&c, &test_layout_a) == NULL);

	// layouts hear about new clients and about the rule fields they own
	CHECK(layout_register(&test_layout_hooks) != (layout_t)-1);

	test_hooks_created = 0;
	layout_client_created(&c);
	CHECK(test_hooks_created == 1);

	rule_consequence_t rule = {0};
	rule.has = RULE_TYPE_STATE;
	test_hooks_rules = 0;
	layout_apply_client_rules(&c, &rule);
	CHECK(test_hooks_rules == 1);

	client_layout_states_clear(&c);
	CHECK(client_layout_state_peek(&c, LAYOUT_MASTER_STACK) == NULL);
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
	test_layout_queries();
	test_layout_settings();
	test_plugin_registration();
	test_layout_identity_and_state();
	test_duplicate_and_invalid();
	test_ipc_values();
	printf("%d checks passed\n", test_checks);
	return test_failures ? 1 : 0;
}
