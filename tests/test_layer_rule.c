#include "layer_rule.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

static layer_rule_t *add_test_rule(const char *ns, const char *output) {
	layer_rule_t *r = make_layer_rule();
	if (!r)
		return NULL;

	char err[256] = {0};
	if (!rule_pattern_set(&r->match.namespace, ns, err, sizeof(err))) {
		free_layer_rule(r);
		return NULL;
	}
	if (output && !rule_pattern_set(&r->match.output, output, err, sizeof(err))) {
		free_layer_rule(r);
		return NULL;
	}

	r->consequence.has |= LAYER_RULE_OPACITY;
	r->consequence.opacity = 0.5f;
	add_layer_rule(r);
	return r;
}

static void test_matching(void) {
	add_test_rule("exact-bar", NULL);
	add_test_rule("glob:notif*", NULL);
	add_test_rule("regex:i:^swaync$", NULL);

	layer_rule_consequence_t *m = find_matching_layer_rule("exact-bar", "DP-1");
	CHECK(m != NULL);
	CHECK_NEAR(m->opacity, 0.5f, 0.0001);

	CHECK(find_matching_layer_rule("exact-bar-extra", "DP-1") == NULL);
	CHECK(find_matching_layer_rule("notifications", "DP-1") != NULL);
	CHECK(find_matching_layer_rule("notif", "DP-1") != NULL);
	CHECK(find_matching_layer_rule("Other", "DP-1") == NULL);
	CHECK(find_matching_layer_rule("SWAYNC", "DP-1") != NULL);
	CHECK(find_matching_layer_rule("swaync2", "DP-1") == NULL);
}

static void test_output_filter(void) {
	layer_rule_t *dp1 = add_test_rule("output-test", "DP-1");
	CHECK(dp1 != NULL);
	dp1->consequence.opacity = 0.3f;

	layer_rule_t *dp2 = add_test_rule("output-test", "DP-2");
	CHECK(dp2 != NULL);
	dp2->consequence.opacity = 0.4f;

	layer_rule_consequence_t *m = find_matching_layer_rule("output-test", "DP-1");
	CHECK(m != NULL);
	CHECK_NEAR(m->opacity, 0.3f, 0.0001);

	m = find_matching_layer_rule("output-test", "DP-2");
	CHECK(m != NULL);
	CHECK_NEAR(m->opacity, 0.4f, 0.0001);

	CHECK(find_matching_layer_rule("output-test", "HDMI-A-1") == NULL);
}

static void test_merge_and_override(void) {
	layer_rule_t *first = add_test_rule("merge-test", NULL);
	CHECK(first != NULL);
	first->consequence.opacity = 0.6f;
	first->consequence.has |= LAYER_RULE_LAYER;
	first->consequence.layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;

	layer_rule_t *second = add_test_rule("merge-test", NULL);
	CHECK(second != NULL);
	second->consequence.opacity = 0.8f;
	second->consequence.has |= LAYER_RULE_LAYER | LAYER_RULE_BLUR |
		LAYER_RULE_BLOCK_OUT_FROM_SCREENSHARE;
	second->consequence.layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
	second->consequence.flags |= LAYER_RULE_BLUR | LAYER_RULE_BLOCK_OUT_FROM_SCREENSHARE;

	layer_rule_consequence_t *m = find_matching_layer_rule("merge-test", "DP-1");
	CHECK(m != NULL);
	CHECK_NEAR(m->opacity, 0.8f, 0.0001);
	CHECK(m->layer == ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY);
	CHECK(layer_rule_flag(m, LAYER_RULE_BLUR));
	CHECK(layer_rule_flag(m, LAYER_RULE_BLOCK_OUT_FROM_SCREENSHARE));
}

static void test_remove_and_list(void) {
	size_t before = layer_rule_count();
	CHECK(before > 0);

	CHECK(remove_layer_rule_by_index(0));
	CHECK(layer_rule_count() == before - 1);
	CHECK(find_matching_layer_rule("exact-bar", "DP-1") == NULL);

	char buf[4096];
	list_layer_rules(buf, sizeof(buf));
	CHECK(strstr(buf, "namespace=merge-test") != NULL);
	CHECK(strstr(buf, "layer=overlay") != NULL);
	CHECK(strstr(buf, "blur=on") != NULL);
	CHECK(strstr(buf, "block_out_from_screenshare=on") != NULL);
	CHECK(strstr(buf, "namespace=exact-bar") == NULL);

	CHECK(!remove_layer_rule_by_index(999));
}

static void run_tests(void) {
	layer_rule_init();
	test_matching();
	test_output_filter();
	test_merge_and_override();
	test_remove_and_list();
	layer_rule_fini();
}

TEST_MAIN(run_tests)
