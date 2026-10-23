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
	test_apply_still_works();
}

TEST_MAIN(run_tests)
