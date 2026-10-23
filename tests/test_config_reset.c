#include "config_internal.h"
#include "ipc/ipc.h"
#include "settings.h"
#include "str.h"
#include "tabs.h"
#include "test.h"
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-util.h>

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


#define SEND(msg, reply) exchange(msg, sizeof(msg) - 1, reply, sizeof(reply))

// resetting a setting that was changed at runtime puts the compiled value back
static void test_reset_one(void) {
	bool before = settings.record_history;
	settings.record_history = !before;

	char reply[4096];
	size_t n = SEND("config\0reset\0record_history\0", reply);
	CHECK(n > 1);
	CHECK(reply[0] == '\0');
	CHECK(settings.record_history == before);
	CHECK(strstr(reply + 1, "reset 1 of 1 setting") != NULL);
	CHECK(strstr(reply + 1, "record_history") != NULL);
}

// the flag form is what `doorsctl config --reset` forwards
static void test_reset_flag_form(void) {
	bool before = settings.record_history;
	settings.record_history = !before;

	char reply[4096];
	size_t n = SEND("config\0--reset\0record_history\0", reply);
	CHECK(n > 1);
	CHECK(reply[0] == '\0');
	CHECK(settings.record_history == before);
}

// a name that is already at its default is reported, not rewritten
static void test_reset_unchanged(void) {
	char reply[4096];
	size_t n = SEND("config\0reset\0record_history\0", reply);
	CHECK(n > 1);
	CHECK(reply[0] == '\0');
	CHECK(strstr(reply + 1, "no change") != NULL);
	CHECK(strstr(reply + 1, "(unchanged)") != NULL);
}

// every name is checked before anything is written
static void test_reset_unknown_name(void) {
	bool before = settings.record_history;
	settings.record_history = !before;

	char reply[4096];
	size_t n = SEND("config\0reset\0record_history\0no_such_setting\0", reply);
	CHECK(n > 1);
	CHECK(reply[0] == '\x01');
	CHECK(strstr(reply + 1, "unknown setting") != NULL || strstr(reply + 1,
		"Unknown setting") != NULL);
	CHECK(settings.record_history != before);

	settings.record_history = before;
}

// the multi-part animation family has no single value to restore
static void test_reset_special_name(void) {
	char reply[4096];
	size_t n = SEND("config\0reset\0animation\0", reply);
	CHECK(n > 1);
	CHECK(reply[0] == '\x01');
	CHECK(strstr(reply + 1, "own syntax") != NULL);
}

// a setting with its own syntax but a single value is restored like any other
static void test_reset_special_setting(void) {
	char dflt[128];
	CHECK(cfg_default_value("tab_color_bg", dflt, sizeof(dflt)));

	float saved[4];
	memcpy(saved, color_tab_bg, sizeof(saved));
	color_tab_bg[0] = saved[0] > 0.5f ? 0.0f : 1.0f;

	char reply[4096];
	size_t n = SEND("config\0reset\0tab_color_bg\0", reply);
	CHECK(n > 1);
	CHECK(reply[0] == '\0');
	CHECK(strstr(reply + 1, "reset 1 of 1 setting") != NULL);
	CHECK(strstr(reply + 1, "tab_color_bg") != NULL);

	memset(reply, 0, sizeof(reply));
	n = SEND("config\0tab_color_bg\0", reply);
	CHECK(n > 1);
	CHECK(reply[0] == '\0');

	char got[128];
	str_copy(got, sizeof(got), reply + 1);
	str_rtrim(got);
	CHECK_STREQ(got, dflt);

	memcpy(color_tab_bg, saved, sizeof(saved));
}

// a gradient whose default has no stops is restored as "clear"
static void test_reset_gradient(void) {
	char dflt[128];
	CHECK(cfg_default_value("focused_border_gradient", dflt, sizeof(dflt)));

	char reply[4096];
	size_t n = SEND("config\0focused_border_gradient\0#ff0000 45\0", reply);
	CHECK(n > 1);
	CHECK(reply[0] == '\0');

	n = SEND("config\0reset\0focused_border_gradient\0", reply);
	CHECK(n > 1);
	CHECK(reply[0] == '\0');
	CHECK(strstr(reply + 1, "reset 1 of 1 setting") != NULL);

	memset(reply, 0, sizeof(reply));
	n = SEND("config\0focused_border_gradient\0", reply);
	CHECK(n > 1);
	CHECK(reply[0] == '\0');

	char got[128];
	str_copy(got, sizeof(got), reply + 1);
	str_rtrim(got);
	CHECK_STREQ(got, dflt);
}

// an alias resets the setting it stands for
static void test_reset_special_alias(void) {
	char dflt[128];
	CHECK(cfg_default_value("scroller_default_column_width", dflt, sizeof(dflt)));

	char reply[4096];
	size_t n = SEND("config\0scroller_default_proportion\0" "0.25\0", reply);
	CHECK(n > 1);
	CHECK(reply[0] == '\0');

	n = SEND("config\0reset\0scroller_default_proportion\0", reply);
	CHECK(n > 1);
	CHECK(reply[0] == '\0');
	CHECK(strstr(reply + 1, "reset 1 of 1 setting") != NULL);
	CHECK(strstr(reply + 1, "scroller_default_column_width") != NULL);

	memset(reply, 0, sizeof(reply));
	n = SEND("config\0scroller_default_column_width\0", reply);
	CHECK(n > 1);
	CHECK(reply[0] == '\0');

	char got[128];
	str_copy(got, sizeof(got), reply + 1);
	str_rtrim(got);
	CHECK_STREQ(got, dflt);
}

// no names means every setting, and only the ones that moved are written
static void test_reset_all(void) {
	bool record = settings.record_history;
	bool dialogs = settings.auto_float_dialogs;
	settings.record_history = !record;
	settings.auto_float_dialogs = !dialogs;

	char reply[4096];
	size_t n = SEND("config\0reset\0", reply);
	CHECK(n > 1);
	CHECK(reply[0] == '\0');
	CHECK(settings.record_history == record);
	CHECK(settings.auto_float_dialogs == dialogs);
	CHECK(strstr(reply + 1, "reset 2 of") != NULL);
	CHECK(strstr(reply + 1, "record_history") != NULL);
	CHECK(strstr(reply + 1, "auto_float_dialogs") != NULL);

	// nothing left to do the second time
	n = SEND("config\0reset\0", reply);
	CHECK(n > 1);
	CHECK(reply[0] == '\0');
	CHECK(strstr(reply + 1, "no change") != NULL);
}

static void test_reset_dry_run(void) {
	bool before = settings.record_history;
	settings.record_history = !before;

	char reply[4096];
	size_t n = SEND("--dry-run\0config\0reset\0record_history\0", reply);
	CHECK(n > 1);
	CHECK(reply[0] == '\0');
	CHECK(strstr(reply + 1, "would reset 1 of 1 setting") != NULL);
	CHECK(settings.record_history != before);

	settings.record_history = before;
}

static void run_tests(void) {
	wl_list_init(&mon_list);
	wl_list_init(&orphan_desk_list);
	config_snapshot_defaults();

	test_reset_one();
	test_reset_flag_form();
	test_reset_unchanged();
	test_reset_unknown_name();
	test_reset_special_name();
	test_reset_special_setting();
	test_reset_gradient();
	test_reset_special_alias();
	test_reset_all();
	test_reset_dry_run();
}

TEST_MAIN(run_tests)
