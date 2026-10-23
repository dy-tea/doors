#include "ipc/ipc.h"
#include "settings.h"
#include "test.h"
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

// settings with their own syntax have no single value to restore
static void test_reset_special_name(void) {
	char reply[4096];
	size_t n = SEND("config\0reset\0tab_color_bg\0", reply);
	CHECK(n > 1);
	CHECK(reply[0] == '\x01');
	CHECK(strstr(reply + 1, "own syntax") != NULL);
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
	CHECK(strstr(reply + 1, "reset 2 of 83 settings") != NULL);
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
	config_snapshot_defaults();

	test_reset_one();
	test_reset_flag_form();
	test_reset_unchanged();
	test_reset_unknown_name();
	test_reset_special_name();
	test_reset_all();
	test_reset_dry_run();
}

TEST_MAIN(run_tests)
