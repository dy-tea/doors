#include "ipc/ipc.h"
#include "test.h"

#define MONITOR_EVENTS                                                                               \
	(SUB_MASK_MONITOR_ADD | SUB_MASK_MONITOR_REMOVE | SUB_MASK_MONITOR_FOCUS |                    \
		SUB_MASK_MONITOR_CHANGE)
#define DESKTOP_EVENTS                                                                               \
	(SUB_MASK_DESKTOP_ADD | SUB_MASK_DESKTOP_REMOVE | SUB_MASK_DESKTOP_FOCUS |                    \
		SUB_MASK_DESKTOP_CHANGE | SUB_MASK_DESKTOP_LAYOUT)
#define NODE_EVENTS                                                                                  \
	(SUB_MASK_NODE_ADD | SUB_MASK_NODE_REMOVE | SUB_MASK_NODE_FOCUS | SUB_MASK_NODE_CHANGE |      \
		SUB_MASK_NODE_STATE | SUB_MASK_NODE_FLAG)

static subscriber_mask_t mask_for(const char *word) {
	subscriber_mask_t mask = 0;
	CHECK(subscribe_mask_for(word, &mask));
	return mask;
}

// every word and alias resolves to its own mask
static void test_words(void) {
	CHECK(mask_for("report") == SUB_MASK_REPORT);
	CHECK(mask_for("R") == SUB_MASK_REPORT);
	CHECK(mask_for("monitor") == MONITOR_EVENTS);
	CHECK(mask_for("M") == MONITOR_EVENTS);
	CHECK(mask_for("monitor_add") == SUB_MASK_MONITOR_ADD);
	CHECK(mask_for("monitor_remove") == SUB_MASK_MONITOR_REMOVE);
	CHECK(mask_for("monitor_focus") == SUB_MASK_MONITOR_FOCUS);
	CHECK(mask_for("monitor_change") == SUB_MASK_MONITOR_CHANGE);
	CHECK(mask_for("desktop") == DESKTOP_EVENTS);
	CHECK(mask_for("D") == DESKTOP_EVENTS);
	CHECK(mask_for("desktop_add") == SUB_MASK_DESKTOP_ADD);
	CHECK(mask_for("desktop_remove") == SUB_MASK_DESKTOP_REMOVE);
	CHECK(mask_for("desktop_focus") == SUB_MASK_DESKTOP_FOCUS);
	CHECK(mask_for("desktop_change") == SUB_MASK_DESKTOP_CHANGE);
	CHECK(mask_for("desktop_layout") == SUB_MASK_DESKTOP_LAYOUT);
	CHECK(mask_for("node") == NODE_EVENTS);
	CHECK(mask_for("N") == NODE_EVENTS);
	CHECK(mask_for("node_add") == SUB_MASK_NODE_ADD);
	CHECK(mask_for("node_remove") == SUB_MASK_NODE_REMOVE);
	CHECK(mask_for("node_focus") == SUB_MASK_NODE_FOCUS);
	CHECK(mask_for("node_change") == SUB_MASK_NODE_CHANGE);
	CHECK(mask_for("node_state") == SUB_MASK_NODE_STATE);
	CHECK(mask_for("node_flag") == SUB_MASK_NODE_FLAG);
	CHECK(mask_for("config") == SUB_MASK_CONFIG);
	CHECK(mask_for("config_changed") == SUB_MASK_CONFIG);
	CHECK(mask_for("permission") == SUB_MASK_PERMISSION);
	CHECK(mask_for("P") == SUB_MASK_PERMISSION);
	CHECK(mask_for("permission_request") == SUB_MASK_PERMISSION);
	CHECK(mask_for("permission_resolved") == SUB_MASK_PERMISSION);
	CHECK(mask_for("all") == SUB_MASK_ALL);
	CHECK(mask_for("A") == SUB_MASK_ALL);
}

// the packed name lists are terminated, so a short alias cannot pick up the
// mask of the entry that happens to sit next to it in .rodata
static void test_no_bleed(void) {
	CHECK(mask_for("R") == SUB_MASK_REPORT);
	CHECK(mask_for("M") == MONITOR_EVENTS);
	CHECK(mask_for("D") == DESKTOP_EVENTS);
	CHECK(mask_for("N") == NODE_EVENTS);
	CHECK(mask_for("P") == SUB_MASK_PERMISSION);
	CHECK(mask_for("A") == SUB_MASK_ALL);
}

static void test_unknown(void) {
	subscriber_mask_t mask = 0;

	CHECK(!subscribe_mask_for("bogus", &mask));
	CHECK(mask == 0);
	CHECK(!subscribe_mask_for("", &mask));
	CHECK(mask == 0);
	CHECK(!subscribe_mask_for("config_reloaded", &mask));
	CHECK(mask == 0);
}

// several words in one request accumulate
static void test_accumulate(void) {
	subscriber_mask_t mask = 0;

	CHECK(subscribe_mask_for("config", &mask));
	CHECK(subscribe_mask_for("node_add", &mask));
	CHECK(mask == (SUB_MASK_CONFIG | SUB_MASK_NODE_ADD));
}

static void run_tests(void) {
	test_words();
	test_no_bleed();
	test_unknown();
	test_accumulate();
}

TEST_MAIN(run_tests)
