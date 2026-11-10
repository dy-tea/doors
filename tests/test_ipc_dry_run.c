#include "bezier.h"
#include "config.h"
#include "input/input.h"
#include "ipc/ipc.h"
#include "layer_rule.h"
#include "output/output.h"
#include "protocol/layer.h"
#include "rule.h"
#include "settings.h"
#include "spring.h"
#include "tabs.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static size_t exchange(const char *msg, size_t len, char *reply, size_t replysz) {
	int fds[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
		CHECK(false);
		return 0;
	}

	process_ipc_message((char *)msg, (int)len, fds[0]);
	close(fds[0]);

	ssize_t n = recv(fds[1], reply, replysz - 1, 0);
	close(fds[1]);

	if (n < 0)
		n = 0;

	reply[n] = '\0';
	return (size_t)n;
}

static void test_dry_run_valid(void) {
	int before = settings.border_width;
	const char msg[] = "--dry-run\0config\0border_width\0"
		"7\0";
	char reply[512];

	size_t n = exchange(msg, sizeof(msg) - 1, reply, sizeof(reply));
	CHECK(n > 1);
	CHECK(reply[0] == '\0');
	CHECK(strstr(reply + 1, "would be set") != NULL);
	CHECK(settings.border_width == before);
}

static void test_dry_run_invalid(void) {
	int before = settings.border_width;
	const char msg[] = "--dry-run\0config\0border_width\0"
		"two\0";
	char reply[512];

	size_t n = exchange(msg, sizeof(msg) - 1, reply, sizeof(reply));
	CHECK(n > 1);
	CHECK(reply[0] == '\x01');
	CHECK(strstr(reply + 1, "invalid value") != NULL);
	CHECK(settings.border_width == before);
}

static void test_dry_run_unsupported(void) {
	const char msg[] = "--dry-run\0node\0--close\0";
	char reply[512];

	size_t n = exchange(msg, sizeof(msg) - 1, reply, sizeof(reply));
	CHECK(n > 1);
	CHECK(reply[0] == '\x01');
	CHECK(strstr(reply + 1, "not supported") != NULL);
}

static void test_dry_run_special_setting(void) {
	const char msg[] = "--dry-run\0config\0animation\0duration\0"
		"300\0";
	char reply[512];

	size_t n = exchange(msg, sizeof(msg) - 1, reply, sizeof(reply));
	CHECK(n > 1);
	CHECK(reply[0] == '\x01');
	CHECK(strstr(reply + 1, "not supported") != NULL);
}

// a setting with its own syntax but a single value can be checked too
static void test_dry_run_special_value(void) {
	float before[4];
	memcpy(before, settings.color_tab_bg, sizeof(before));

	const char msg[] = "--dry-run\0config\0tab_color_bg\0"
		"0.25 0.5 0.75 1\0";
	char reply[512];

	size_t n = exchange(msg, sizeof(msg) - 1, reply, sizeof(reply));
	CHECK(n > 1);
	CHECK(reply[0] == '\0');
	CHECK(strstr(reply + 1, "would be set") != NULL);
	CHECK(memcmp(before, settings.color_tab_bg, sizeof(before)) == 0);
}

static void test_dry_run_rule(void) {
	rule_init();
	size_t before = rule_count();

	const char msg[] = "--dry-run\0rule\0-a\0firefox\0state=floating\0";
	char reply[512];

	size_t n = exchange(msg, sizeof(msg) - 1, reply, sizeof(reply));
	CHECK(n > 1);
	CHECK(reply[0] == '\0');
	CHECK(strstr(reply + 1, "would be added") != NULL);
	CHECK(rule_count() == before);
}

static void test_dry_run_layer_rule(void) {
	layer_rule_init();
	size_t before = layer_rule_count();

	const char msg[] = "--dry-run\0layer_rule\0-a\0waybar\0blur=on\0";
	char reply[512];

	size_t n = exchange(msg, sizeof(msg) - 1, reply, sizeof(reply));
	CHECK(n > 1);
	CHECK(reply[0] == '\0');
	CHECK(strstr(reply + 1, "would be added") != NULL);
	CHECK(layer_rule_count() == before);

	const char missing_namespace[] = "--dry-run\0layer_rule\0-a\0blur=on\0";
	n = exchange(missing_namespace, sizeof(missing_namespace) - 1, reply, sizeof(reply));
	CHECK(n > 1);
	CHECK(reply[0] == '\x01');
	CHECK(strstr(reply + 1, "Must specify a namespace") != NULL);
	CHECK(layer_rule_count() == before);

	const char unknown_option[] = "--dry-run\0layer_rule\0-a\0waybar\0title=x\0";
	n = exchange(unknown_option, sizeof(unknown_option) - 1, reply, sizeof(reply));
	CHECK(n > 1);
	CHECK(reply[0] == '\x01');
	CHECK(strstr(reply + 1, "Unknown layer rule option") != NULL);
	CHECK(layer_rule_count() == before);
}

static void test_query_layers(void) {
	wl_list_init(&mon_list);

	output_t *output = calloc(1, sizeof(*output));
	CHECK(output != NULL);
	if (!output)
		return;

	snprintf(output->name, sizeof(output->name), "DP-1");
	for (int i = 0; i < 4; i++)
		wl_list_init(&output->layers[i]);
	wl_list_insert(&mon_list, &output->link);

	struct wlr_layer_surface_v1 *wlr_layer = calloc(1, sizeof(*wlr_layer));
	layer_surface_t *layer = calloc(1, sizeof(*layer));
	CHECK(wlr_layer != NULL && layer != NULL);

	bool linked = false;
	if (wlr_layer && layer) {
		wlr_layer->namespace = strdup("waybar");
		layer->layer_surface = wlr_layer;
		layer->layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
		wl_list_init(&layer->link);
		wl_list_insert(&output->layers[ZWLR_LAYER_SHELL_V1_LAYER_TOP], &layer->link);
		linked = true;

		const char msg[] = "query\0--layers\0";
		char reply[512];
		size_t n = exchange(msg, sizeof(msg) - 1, reply, sizeof(reply));
		CHECK(n > 1);
		CHECK(reply[0] == '\0');
		CHECK(strstr(reply + 1, "\"namespace\": \"waybar\"") != NULL);
		CHECK(strstr(reply + 1, "\"output\": \"DP-1\"") != NULL);
		CHECK(strstr(reply + 1, "\"layer\": \"top\"") != NULL);
		CHECK(strstr(reply + 1, "waybar DP-1 top") == NULL);

		const char names_msg[] = "query\0-L\0--names\0";
		n = exchange(names_msg, sizeof(names_msg) - 1, reply, sizeof(reply));
		CHECK(n > 1);
		CHECK(reply[0] == '\0');
		CHECK(strstr(reply + 1, "waybar") != NULL);
		CHECK(strstr(reply + 1, "DP-1") == NULL);
	}

	if (linked)
		wl_list_remove(&layer->link);
	if (wlr_layer) {
		free(wlr_layer->namespace);
		free(wlr_layer);
	}
	free(layer);
	wl_list_remove(&output->link);
	free(output);
	wl_list_init(&mon_list);
}

static void test_dry_run_hotkey(void) {
	size_t before = num_keybinds;

	const char msg[] = "--dry-run\0hotkey\0add\0super+Return\0foot\0";
	char reply[512];

	size_t n = exchange(msg, sizeof(msg) - 1, reply, sizeof(reply));
	CHECK(n > 1);
	CHECK(reply[0] == '\0');
	CHECK(strstr(reply + 1, "would be added") != NULL);
	CHECK(num_keybinds == before);
}

static void test_dry_run_env(void) {
	unsetenv("DOORS_DRY_RUN_PROBE");

	const char msg[] = "--dry-run\0env\0set\0DOORS_DRY_RUN_PROBE\0value\0";
	char reply[512];

	size_t n = exchange(msg, sizeof(msg) - 1, reply, sizeof(reply));
	CHECK(n > 1);
	CHECK(reply[0] == '\0');
	CHECK(strstr(reply + 1, "would be set") != NULL);
	CHECK(getenv("DOORS_DRY_RUN_PROBE") == NULL);
}

static void test_dry_run_curves(void) {
	bezier_init();
	spring_init();

	const char bezier_msg[] = "--dry-run\0bezier\0dry-run-probe\0"
		"0.1\0"
		"0.2\0"
		"0.3\0"
		"0.4\0";
	char reply[512];

	size_t n = exchange(bezier_msg, sizeof(bezier_msg) - 1, reply, sizeof(reply));
	CHECK(n > 1);
	CHECK(reply[0] == '\0');
	CHECK(strstr(reply + 1, "would be added") != NULL);
	CHECK(!bezier_exists("dry-run-probe"));

	const char spring_msg[] = "--dry-run\0spring\0dry-run-probe\0"
		"100\0"
		"10\0";
	n = exchange(spring_msg, sizeof(spring_msg) - 1, reply, sizeof(reply));
	CHECK(n > 1);
	CHECK(reply[0] == '\0');
	CHECK(strstr(reply + 1, "would be added") != NULL);
	CHECK(!spring_exists("dry-run-probe"));
}

static void test_dry_run_input(void) {
	size_t before = num_input_configs;

	const char msg[] = "--dry-run\0input\0type:pointer\0tap\0true\0";
	char reply[512];

	size_t n = exchange(msg, sizeof(msg) - 1, reply, sizeof(reply));
	CHECK(n > 1);
	CHECK(reply[0] == '\0');
	CHECK(num_input_configs == before);

	const char bad[] = "--dry-run\0input\0type:pointer\0no_such_property\0x\0";
	n = exchange(bad, sizeof(bad) - 1, reply, sizeof(reply));
	CHECK(n > 1);
	CHECK(reply[0] == '\x01');
	CHECK(strstr(reply + 1, "Unknown property") != NULL);
	CHECK(num_input_configs == before);
}

static const char *reply_body(const char *reply) {
	const char *colon = strchr(reply + 1, ':');
	return colon ? colon + 1 : reply + 1;
}

static void check_parity(const char *node_msg, size_t node_len, const char *tree_msg,
		size_t tree_len) {
	char node_reply[512];
	char tree_reply[512];

	size_t nn = exchange(node_msg, node_len, node_reply, sizeof(node_reply));
	size_t tn = exchange(tree_msg, tree_len, tree_reply, sizeof(tree_reply));

	CHECK(nn > 1);
	CHECK(tn > 1);
	CHECK(node_reply[0] == tree_reply[0]);
	CHECK(strcmp(reply_body(node_reply), reply_body(tree_reply)) == 0);
}

static void test_shared_subcommand_parity(void) {
	const char node_ratio[] = "node\0-o\0"
		"0.4\0";
	const char tree_ratio[] = "tree\0presel_ratio\0"
		"0.4\0";
	check_parity(node_ratio, sizeof(node_ratio) - 1, tree_ratio, sizeof(tree_ratio) - 1);

	const char node_bad[] = "node\0-o\0"
		"bogus\0";
	const char tree_bad[] = "tree\0presel_ratio\0"
		"bogus\0";
	check_parity(node_bad, sizeof(node_bad) - 1, tree_bad, sizeof(tree_bad) - 1);

	const char node_circ[] = "node\0-C\0"
		"forward\0";
	const char tree_circ[] = "tree\0circulate\0"
		"forward\0";
	check_parity(node_circ, sizeof(node_circ) - 1, tree_circ, sizeof(tree_circ) - 1);

	const char node_circ_bad[] = "node\0-C\0"
		"sideways\0";
	const char tree_circ_bad[] = "tree\0circulate\0"
		"sideways\0";
	check_parity(node_circ_bad, sizeof(node_circ_bad) - 1, tree_circ_bad, sizeof(tree_circ_bad) - 1);
}

static void test_apply_still_works(void) {
	int before = settings.border_width;
	const char msg[] = "config\0border_width\0"
		"31\0";
	char reply[512];

	size_t n = exchange(msg, sizeof(msg) - 1, reply, sizeof(reply));
	CHECK(n > 1);
	CHECK(reply[0] == '\0');
	CHECK(settings.border_width == 31);

	settings.border_width = before;
}

static void run_tests(void) {
	test_dry_run_valid();
	test_dry_run_invalid();
	test_dry_run_unsupported();
	test_dry_run_special_setting();
	test_dry_run_special_value();
	test_dry_run_rule();
	test_dry_run_layer_rule();
	test_query_layers();
	test_dry_run_hotkey();
	test_dry_run_env();
	test_dry_run_curves();
	test_dry_run_input();
	test_shared_subcommand_parity();
	test_apply_still_works();
}

TEST_MAIN(run_tests)
