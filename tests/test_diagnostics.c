#include "diagnostics.h"
#include "ipc/json.h"
#include "rule.h"
#include "server.h"
#include "settings.h"
#include "str.h"
#include "test.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char tmpdir[256];

static int open_tmp(const char *name, int flags) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", tmpdir, name);
	return open(path, flags, 0600);
}

static size_t slurp(const char *path, char *buf, size_t cap) {
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;

	size_t len = 0;
	ssize_t n;
	while (len + 1 < cap && (n = read(fd, buf + len, cap - len - 1)) > 0)
		len += (size_t)n;
	close(fd);
	buf[len] = '\0';
	return len;
}

static size_t slurp_tmp(const char *name, char *buf, size_t cap) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", tmpdir, name);
	return slurp(path, buf, cap);
}

static size_t slurp_crash(const char *name, char *buf, size_t cap) {
	char path[1200];
	snprintf(path, sizeof(path), "%s/%s", diag_crash_dir(), name);
	return slurp(path, buf, cap);
}

static void test_log_tail(void) {
	int fd = open_tmp("tail.txt", O_WRONLY | O_CREAT | O_TRUNC);
	CHECK(fd >= 0);
	if (fd < 0)
		return;

	diag_log_line("[one] first line\n");
	diag_log_line("[two] second line\n");

	diag_log_tail_write(fd);
	close(fd);

	char buf[1024];
	CHECK(slurp_tmp("tail.txt", buf, sizeof(buf)) > 0);
	CHECK(strstr(buf, "first line") != NULL);
	CHECK(strstr(buf, "second line") != NULL);
	if (strstr(buf, "first line") && strstr(buf, "second line"))
		CHECK(strstr(buf, "first line") < strstr(buf, "second line"));

	char *big = malloc(70000);
	CHECK(big != NULL);
	if (!big)
		return;

	memset(big, 'a', 70000);
	memcpy(big + 70000 - 6, "END\n\0\0", 6);
	diag_log_line(big);
	free(big);

	fd = open_tmp("tail-full.txt", O_WRONLY | O_CREAT | O_TRUNC);
	diag_log_tail_write(fd);
	close(fd);

	char *full = malloc(80000);
	CHECK(full != NULL);
	if (!full)
		return;

	size_t len = slurp_tmp("tail-full.txt", full, 80000);
	CHECK(len == 64 * 1024);
	CHECK(len >= 4 && streq(full + len - 4, "END\n"));
	free(full);
}

static void test_stall_watchdog(void) {
	char buf[2048];

	diag_stage_enter(DIAG_STAGE_RENDER);
	usleep(250 * 1000);
	CHECK(slurp_tmp("stall.txt", buf, sizeof(buf)) > 0);
	CHECK(strstr(buf, "stuck in \"render\"") != NULL);
	CHECK(strstr(buf, "[WARN]") != NULL);

	diag_stage_leave();
	CHECK(slurp_tmp("stall.txt", buf, sizeof(buf)) > 0);
	CHECK(strstr(buf, "finished after") != NULL);
}

static void test_long_stall(void) {
	char buf[2048];

	diag_stage_enter(DIAG_STAGE_COMMIT);
	sleep(2);
	CHECK(slurp_tmp("stall.txt", buf, sizeof(buf)) > 0);
	CHECK(strstr(buf, "stuck in \"commit\"") != NULL);

	diag_stage_leave();
	CHECK(slurp_tmp("stall.txt", buf, sizeof(buf)) > 0);
	CHECK(strstr(buf, "finished after") != NULL);
}

static void test_status_document(void) {
	wl_list_init(&mon_list);
	wl_list_init(&orphan_desk_list);
	wl_list_init(&server.views);
	server.focused_output = NULL;
	rule_init();

	CHECK(diag_status_refresh());

	size_t len = 0;
	const char *doc = diag_status_snapshot(&len);
	CHECK(len > 0);
	CHECK(doc != NULL);

	char err[256];
	json_value_t *root = json_parse(doc, err, sizeof(err));
	CHECK(root != NULL);
	if (!root) {
		fprintf(stderr, "status document did not parse: %s\n", err);
		return;
	}

	CHECK(streq(json_get_string(json_get(root, "status")), "running"));
	CHECK(json_get_string(json_get(root, "version")) != NULL);
	CHECK(json_get_string(json_get(root, "commit")) != NULL);
	CHECK(json_get_string(json_get(root, "backends")) != NULL);
	CHECK(json_get(root, "outputs") != NULL);

	double outputs = -1;
	CHECK(json_get_number(json_get(json_get(root, "counts"), "outputs"), &outputs));
	CHECK(outputs == 0);

	CHECK(json_get(json_get(root, "transactions"), "commits") != NULL);
	CHECK(json_get(root, "focused") != NULL);
	json_free(root);
}

static void test_crash_bundle(void) {
	CHECK(diag_crash_dir() != NULL);
	if (diag_crash_dir() == NULL)
		return;

	diag_log_line("[marker] just before the crash\n");

	pid_t pid = fork();
	CHECK(pid >= 0);
	if (pid == 0) {
		// keep the crash header out of the test output
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			dup2(devnull, STDOUT_FILENO);
			close(devnull);
		}
		diag_crash_bundle(SIGSEGV);
		_exit(0);
	}

	int status = 0;
	CHECK(waitpid(pid, &status, 0) == pid);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

	char buf[4096];
	CHECK(slurp_crash("backtrace.txt", buf, sizeof(buf)) > 0);
	CHECK(strstr(buf, "SIGSEGV") != NULL);
	CHECK(strstr(buf, "version:") != NULL);
	CHECK(strstr(buf, "backtrace:") != NULL);

	static char tail[128 * 1024];
	CHECK(slurp_crash("log-tail.txt", tail, sizeof(tail)) > 0);
	CHECK(strstr(tail, "just before the crash") != NULL);

	CHECK(slurp_crash("status.json", buf, sizeof(buf)) > 0);
	char err[256];
	json_value_t *root = json_parse(buf, err, sizeof(err));
	CHECK(root != NULL);
	json_free(root);
}

static void run_tests(void) {
	str_copy(tmpdir, sizeof(tmpdir), "/tmp/doors-diag-XXXXXX");

	char *base = mkdtemp(tmpdir);
	CHECK(base != NULL);
	if (!base)
		return;

	setenv("XDG_STATE_HOME", tmpdir, 1);
	setenv("DOORS_STALL_MS", "20", 1);

	int stall = open_tmp("stall.txt", O_WRONLY | O_CREAT | O_APPEND);
	CHECK(stall >= 0);

	diag_init(stall);

	test_log_tail();
	test_stall_watchdog();
	test_long_stall();
	test_status_document();
	test_crash_bundle();

	diag_fini();
	close(stall);
}

TEST_MAIN(run_tests)
