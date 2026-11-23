#include "ipc/ipc.h"
#include "test.h"
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

static void test_resize_directions(void) {
	char reply[256];

	SEND("resize\0left\0", reply);
	CHECK(reply[0] == '\0');
	SEND("resize\0right\0", reply);
	CHECK(reply[0] == '\0');
	SEND("resize\0up\0", reply);
	CHECK(reply[0] == '\0');
	SEND("resize\0down\0", reply);
	CHECK(reply[0] == '\0');

	SEND("resize\0sideways\0", reply);
	CHECK(reply[0] == '\x01');
	CHECK(strstr(reply + 1, "Unknown direction") != NULL);
}

static void run_tests(void) {
	test_resize_directions();
}

TEST_MAIN(run_tests)
