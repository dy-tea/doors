#include "config.h"
#include "test.h"

typedef struct {
	const char *alias;
	bind_action_t action;
} alias_expect_t;

static const alias_expect_t aliases[] = {
	{"floating_layout", BIND_TOGGLE_FLOATING_LAYOUT},
	{"next", BIND_DESKTOP_NEXT},
	{"send", BIND_SEND_TO_DESKTOP_NEXT},
	{"send_next", BIND_SEND_TO_DESKTOP_NEXT},
	{"send_prev", BIND_SEND_TO_DESKTOP_PREV},
	{"clockwise", BIND_ROTATE_CW},
	{"cw", BIND_ROTATE_CW},
	{"counterclockwise", BIND_ROTATE_CCW},
	{"ccw", BIND_ROTATE_CCW},
	{"horizontal", BIND_FLIP_HORIZONTAL},
	{"h", BIND_FLIP_HORIZONTAL},
	{"vertical", BIND_FLIP_VERTICAL},
	{"v", BIND_FLIP_VERTICAL},
};

static void test_canonical_round_trip(void) {
	for (int a = 0; a < BIND_ACTION_COUNT; a++) {
		const char *name = bind_action_name((bind_action_t)a);
		CHECK(name != NULL);
		CHECK(bind_action_from_alias(name) == (bind_action_t)a);
	}
}

static void test_names_unique(void) {
	for (int a = 0; a < BIND_ACTION_COUNT; a++) {
		for (int b = a + 1; b < BIND_ACTION_COUNT; b++) {
			if (strcmp(bind_action_name((bind_action_t)a), bind_action_name((bind_action_t)b)) == 0)
				CHECK(false);
		}
	}
}

static void test_aliases(void) {
	for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++)
		CHECK(bind_action_from_alias(aliases[i].alias) == aliases[i].action);
}

static void test_aliases_are_not_canonical(void) {
	for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
		for (int a = 0; a < BIND_ACTION_COUNT; a++) {
			if (strcmp(aliases[i].alias, bind_action_name((bind_action_t)a)) == 0) {
				CHECK(bind_action_from_alias(aliases[i].alias) == (bind_action_t)a);
			}
		}
	}
}

static void test_directions_are_command_scoped(void) {
	CHECK(bind_action_from_alias("west") == BIND_NONE);
	CHECK(bind_action_from_alias("north") == BIND_NONE);
	CHECK(bind_action_from_alias("left") == BIND_NONE);
	CHECK(bind_action_from_alias("focus_west") == BIND_FOCUS_WEST);
	CHECK(bind_action_from_alias("swap_west") == BIND_SWAP_WEST);
	CHECK(bind_action_from_alias("presel_west") == BIND_PRESEL_WEST);
	CHECK(bind_action_from_alias("resize_left") == BIND_RESIZE_LEFT);
}

static void test_unknown(void) {
	CHECK(bind_action_from_alias("no_such_action") == BIND_NONE);
	CHECK(bind_action_from_alias("") == BIND_NONE);
	CHECK(bind_action_from_alias(NULL) == BIND_NONE);
}

static void test_name_out_of_range(void) {
	CHECK_STREQ(bind_action_name((bind_action_t) - 1), "unknown");
	CHECK_STREQ(bind_action_name((bind_action_t)BIND_ACTION_COUNT), "unknown");
}

static void run_tests(void) {
	test_canonical_round_trip();
	test_names_unique();
	test_aliases();
	test_aliases_are_not_canonical();
	test_directions_are_command_scoped();
	test_unknown();
	test_name_out_of_range();
}

TEST_MAIN(run_tests)
