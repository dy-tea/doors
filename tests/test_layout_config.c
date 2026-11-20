#include "ipc/args.h"
#include "ipc/ipc.h"
#include "layout/layout.h"
#include "settings.h"
#include "str.h"
#include "test.h"
#include <stdio.h>
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


#define SEND(msg, reply) exchange(msg, sizeof(msg) - 1, reply, sizeof(reply))

// body of reply without success byte and trailing newlines
static void reply_value(const char *reply, char *out, size_t outsz) {
	str_copy(out, outsz, reply + 1);
	str_rtrim(out);
}

static void test_list_layouts(void) {
	char reply[4096];

	size_t n = SEND("layout\0-l\0", reply);
	CHECK(n > 1);
	CHECK(reply[0] == '\0');
	CHECK(strstr(reply + 1, "monocle") != NULL);
	CHECK(strstr(reply + 1, "scroller") != NULL);
	CHECK(strstr(reply + 1, "master_stack") != NULL);

	n = SEND("layout\0monocle\0-l\0", reply);
	CHECK(n > 1);
	CHECK(reply[0] == '\0');
	CHECK(strstr(reply + 1, "borderless") != NULL);
	CHECK(strstr(reply + 1, "layout monocle padding") != NULL);

	n = SEND("layout\0-l\0scroller\0", reply);
	CHECK(n > 1);
	CHECK(reply[0] == '\0');
	CHECK(strstr(reply + 1, "pan_fingers") != NULL);
	CHECK(strstr(reply + 1, "layout scroller cycle_width") != NULL);
}

static void test_settings(void) {
	char reply[4096], value[128];

	SEND("layout\0monocle\0borderless\0true\0", reply);
	CHECK(reply[0] == '\0');
	CHECK(strstr(reply + 1, "borderless set") != NULL);

	SEND("layout\0monocle\0borderless\0", reply);
	CHECK(reply[0] == '\0');
	reply_value(reply, value, sizeof(value));
	CHECK_STREQ(value, "true");

	SEND("layout\0monocle\0borderless\0maybe\0", reply);
	CHECK(reply[0] == '\x01');
	CHECK(strstr(reply + 1, "expected true or false") != NULL);

	SEND("layout\0scroller\0pan_fingers\0" "4\0", reply);
	CHECK(reply[0] == '\0');
	SEND("layout\0scroller\0pan_fingers\0", reply);
	reply_value(reply, value, sizeof(value));
	CHECK_STREQ(value, "4");

	SEND("layout\0scroller\0pan_fingers\0" "9\0", reply);
	CHECK(reply[0] == '\x01');
	CHECK(strstr(reply + 1, "less than") != NULL);

	SEND("config\0borderless_monocle\0true\0", reply);
	CHECK(reply[0] == '\0');

	SEND("layout\0monocle\0borderless\0false\0", reply);
	CHECK(reply[0] == '\0');
}

static void test_padding_command(void) {
	char reply[4096], value[128];

	SEND("layout\0monocle\0padding\0" "8\0", reply);
	CHECK(reply[0] == '\0');

	SEND("layout\0monocle\0padding\0", reply);
	CHECK(reply[0] == '\0');
	reply_value(reply, value, sizeof(value));
	CHECK_STREQ(value, "8 8 8 8");

	SEND("layout\0monocle\0top_padding\0" "12\0", reply);
	CHECK(reply[0] == '\0');

	SEND("layout\0monocle\0padding\0", reply);
	reply_value(reply, value, sizeof(value));
	CHECK_STREQ(value, "12 8 8 8");
}

static void test_dry_run(void) {
	char reply[4096], value[128];

	SEND("layout\0monocle\0borderless\0true\0", reply);
	CHECK(reply[0] == '\0');

	SEND("--dry-run\0layout\0monocle\0borderless\0false\0", reply);
	CHECK(reply[0] == '\0');
	CHECK(strstr(reply + 1, "would be set") != NULL);

	SEND("layout\0monocle\0borderless\0", reply);
	reply_value(reply, value, sizeof(value));
	CHECK_STREQ(value, "true");

	SEND("--dry-run\0layout\0monocle\0padding\0" "3\0", reply);
	CHECK(reply[0] == '\x01');
	CHECK(strstr(reply + 1, "not supported") != NULL);
}

static void test_reset(void) {
	char reply[4096], value[128];

	SEND("layout\0monocle\0reset\0", reply);
	CHECK(reply[0] == '\0');

	SEND("layout\0monocle\0borderless\0true\0", reply);
	SEND("layout\0monocle\0top_padding\0" "12\0", reply);

	SEND("layout\0monocle\0reset\0", reply);
	CHECK(reply[0] == '\0');
	CHECK(strstr(reply + 1, "reset 2 of 6 settings") != NULL);

	SEND("layout\0monocle\0borderless\0", reply);
	reply_value(reply, value, sizeof(value));
	CHECK_STREQ(value, "false");

	SEND("layout\0monocle\0top_padding\0", reply);
	reply_value(reply, value, sizeof(value));
	CHECK_STREQ(value, "0");

	// naming settings only touches those
	SEND("layout\0monocle\0borderless\0true\0", reply);
	SEND("layout\0monocle\0top_padding\0" "9\0", reply);

	SEND("layout\0monocle\0reset\0borderless\0", reply);
	CHECK(reply[0] == '\0');
	CHECK(strstr(reply + 1, "reset 1 of 1 setting") != NULL);

	SEND("layout\0monocle\0borderless\0", reply);
	reply_value(reply, value, sizeof(value));
	CHECK_STREQ(value, "false");

	SEND("layout\0monocle\0top_padding\0", reply);
	reply_value(reply, value, sizeof(value));
	CHECK_STREQ(value, "9");

	SEND("layout\0monocle\0top_padding\0" "0\0", reply);

	SEND("layout\0monocle\0borderless\0true\0", reply);
	SEND("--dry-run\0layout\0monocle\0reset\0borderless\0", reply);
	CHECK(reply[0] == '\0');
	CHECK(strstr(reply + 1, "would reset") != NULL);

	SEND("layout\0monocle\0borderless\0", reply);
	reply_value(reply, value, sizeof(value));
	CHECK_STREQ(value, "true");

	SEND("layout\0monocle\0borderless\0false\0", reply);

	SEND("layout\0scroller\0default_column_width\0" "0.25\0", reply);
	SEND("layout\0scroller\0preset_column_widths\0" "0.4\0", reply);

	SEND("layout\0scroller\0reset\0", reply);
	CHECK(reply[0] == '\0');
	CHECK(strstr(reply + 1, "layout state reset") != NULL);

	SEND("layout\0scroller\0default_column_width\0", reply);
	reply_value(reply, value, sizeof(value));
	CHECK_STREQ(value, "0.50");

	SEND("layout\0scroller\0preset_column_widths\0", reply);
	reply_value(reply, value, sizeof(value));
	CHECK_STREQ(value, "0.33,0.50,0.67");

	SEND("layout\0monocle\0borderless\0true\0", reply);
	SEND("layout\0monocle\0gapless\0true\0", reply);
	SEND("layout\0monocle\0top_padding\0" "1\0", reply);
	SEND("layout\0monocle\0right_padding\0" "2\0", reply);
	SEND("layout\0monocle\0bottom_padding\0" "3\0", reply);
	SEND("layout\0monocle\0left_padding\0" "4\0", reply);

	SEND("layout\0monocle\0reset\0", reply);
	CHECK(reply[0] == '\0');
	CHECK(strstr(reply + 1, "reset 6 of 6 settings") != NULL);
	CHECK(strstr(reply + 1, "borderless true -> false") != NULL);
	CHECK(strstr(reply + 1, "left_padding 4 -> 0") != NULL);
	CHECK(strstr(reply + 1, "(+") == NULL);

	SEND("layout\0scroller\0reset\0preset_column_widths\0", reply);
	CHECK(reply[0] == '\x01');
	CHECK(strstr(reply + 1, "is a command, not a setting") != NULL);
}

static void test_reply_buffer(void) {
	ipc_buf_t b;

	ipc_buf_growable(&b);
	for (int i = 0; i < 500; i++)
		ipc_buff(&b, "  setting_%d 0.50 -> 0.33\n", i);

	CHECK(!b.truncated);
	CHECK(b.len > DOORS_BUFSIZ);
	CHECK(strlen(b.buf) == b.len);
	ipc_buf_free(&b);
	CHECK(b.buf == NULL);

	char storage[32];
	ipc_buf_init(&b, storage, sizeof(storage));
	ipc_buff(&b, "%s", "0123456789012345678901234567890123456789");

	CHECK(b.truncated);
	CHECK(strlen(storage) == sizeof(storage) - 1);
}

static void test_errors(void) {
	char reply[4096];

	SEND("layout\0", reply);
	CHECK(reply[0] == '\x01');
	CHECK(strstr(reply + 1, "Missing layout name") != NULL);

	SEND("layout\0no_such_layout\0-l\0", reply);
	CHECK(reply[0] == '\x01');
	CHECK(strstr(reply + 1, "Unknown layout") != NULL);

	SEND("layout\0monocle\0no_such_option\0", reply);
	CHECK(reply[0] == '\x01');
	CHECK(strstr(reply + 1, "Unknown setting or command") != NULL);

	SEND("layout\0monocle\0padding\0" "-1\0", reply);
	CHECK(reply[0] == '\x01');
}

static void run_tests(void) {
	wl_list_init(&mon_list);
	layout_init();

	test_list_layouts();
	test_settings();
	test_padding_command();
	test_dry_run();
	test_reset();
	test_reply_buffer();
	test_errors();
}

TEST_MAIN(run_tests)
