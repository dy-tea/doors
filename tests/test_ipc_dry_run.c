#include "bezier.h"
#include "config.h"
#include "input/input.h"
#include "ipc/ipc.h"
#include "rule.h"
#include "settings.h"
#include "spring.h"
#include "tabs.h"
#include "test.h"
#include <stdlib.h>
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
	memcpy(before, color_tab_bg, sizeof(before));

	const char msg[] = "--dry-run\0config\0tab_color_bg\0"
		"0.25 0.5 0.75 1\0";
	char reply[512];

	size_t n = exchange(msg, sizeof(msg) - 1, reply, sizeof(reply));
	CHECK(n > 1);
	CHECK(reply[0] == '\0');
	CHECK(strstr(reply + 1, "would be set") != NULL);
	CHECK(memcmp(before, color_tab_bg, sizeof(before)) == 0);
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
	test_dry_run_hotkey();
	test_dry_run_env();
	test_dry_run_curves();
	test_dry_run_input();
	test_apply_still_works();
}

TEST_MAIN(run_tests)
