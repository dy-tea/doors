#include "permission.h"
#include "str.h"
#include "test.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>

static void clean_state(void) {
	while (permission_rule_count() > 0)
		permission_rule_remove(0);

	permission_remember_forget(NULL, NULL, 0);
	permission_cache_clear();

	for (int cap = 0; cap < PERM_CAP_COUNT; cap++)
		permission_set_default((perm_cap_t)cap, PERM_ALLOW);
}

static perm_subject_t subject_of(const char *exe) {
	perm_subject_t s = {0};
	str_copy(s.exe, sizeof(s.exe), exe);
	s.known = true;
	return s;
}

static void test_names(void) {
	perm_cap_t cap;
	perm_decision_t mode;

	CHECK(permission_cap_parse("capture", &cap) && cap == PERM_CAPTURE);
	CHECK(permission_cap_parse("INJECT", &cap) && cap == PERM_INJECT);
	CHECK(!permission_cap_parse("clipboard", &cap));

	CHECK(permission_mode_parse("ask", &mode) && mode == PERM_ASK);
	CHECK(permission_mode_parse("DENY", &mode) && mode == PERM_DENY);
	CHECK(!permission_mode_parse("maybe", &mode));

	CHECK_STREQ(permission_cap_name(PERM_OUTPUT), "output");
	CHECK_STREQ(permission_mode_name(PERM_ALLOW), "allow");
}

static void test_subject_and_cache(void) {
	clean_state();

	struct wl_display *display = wl_display_create();
	int fds[2];
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

	struct wl_client *client = wl_client_create(display, fds[0]);
	CHECK(client != NULL);

	perm_subject_t s;
	CHECK(permission_subject(client, &s));
	CHECK(s.pid == getpid());
	CHECK(s.known);
	CHECK(!s.sandboxed);
	CHECK(s.app_id[0] == '\0');

	char exe[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	CHECK(n > 0);
	if (n > 0) {
		exe[n] = '\0';
		CHECK_STREQ(s.exe, exe);
	}

	perm_result_t r = permission_check(client, PERM_CAPTURE);
	CHECK(r.decision == PERM_ALLOW);
	CHECK_STREQ(r.source, "default");
	CHECK(permission_cache_count() == 0);

	permission_answer(client, &s, PERM_CAPTURE, PERM_DENY, PERM_REMEMBER_NONE);
	CHECK(permission_cache_count() == 1);
	r = permission_check(client, PERM_CAPTURE);
	CHECK(r.decision == PERM_DENY);
	CHECK_STREQ(r.source, "answer");

	// a policy change drops the answer the client was holding
	char spec[PATH_MAX + 16];
	snprintf(spec, sizeof(spec), "exact:%s", exe);
	char err[256] = {0};
	CHECK(permission_rule_add(spec, PERM_CAPTURE, PERM_ALLOW, err, sizeof(err)) >= 0);

	r = permission_check(client, PERM_CAPTURE);
	CHECK(r.decision == PERM_ALLOW);
	CHECK_STREQ(r.source, "policy");

	wl_client_destroy(client);
	CHECK(permission_cache_count() == 0);

	close(fds[1]);
	wl_display_destroy(display);
}

static void test_policy_precedence(void) {
	clean_state();

	char err[256] = {0};

	CHECK(permission_rule_add("exe:*", PERM_CAPTURE, PERM_DENY, err, sizeof(err)) == 0);
	CHECK(permission_rule_add("exe:/usr/bin/grim", PERM_CAPTURE, PERM_ALLOW, err, sizeof(err)) == 1);

	perm_subject_t grim = subject_of("/usr/bin/grim");
	perm_result_t r = permission_decide(&grim, PERM_CAPTURE);
	CHECK(r.decision == PERM_ALLOW);
	CHECK_STREQ(r.source, "policy");
	CHECK(r.rule_index == 1);

	perm_subject_t other = subject_of("/usr/bin/other");
	r = permission_decide(&other, PERM_CAPTURE);
	CHECK(r.decision == PERM_DENY);
	CHECK(r.rule_index == 0);

	// the rule only covers its own capability
	r = permission_decide(&other, PERM_OUTPUT);
	CHECK(r.decision == PERM_ALLOW);
	CHECK_STREQ(r.source, "default");

	// removing the override leaves the broad rule in charge
	CHECK(permission_rule_remove(1));
	r = permission_decide(&grim, PERM_CAPTURE);
	CHECK(r.decision == PERM_DENY);

	// a bare wildcard is a glob, matched against the whole path
	CHECK(permission_rule_add("exe:*firefox", PERM_CAPTURE, PERM_ASK, err, sizeof(err)) == 1);
	perm_subject_t firefox = subject_of("/usr/lib/firefox/firefox");
	r = permission_decide(&firefox, PERM_CAPTURE);
	CHECK(r.decision == PERM_ASK);
	CHECK_STREQ(r.source, "policy");
}

static void test_patterns(void) {
	clean_state();

	char err[256] = {0};

	CHECK(permission_subject_valid("exe:*grim", err, sizeof(err)));
	CHECK(permission_subject_valid("exe:glob:*grim", err, sizeof(err)));
	CHECK(permission_subject_valid("exe:regex:^/usr/bin/.+$", err, sizeof(err)));
	CHECK(permission_subject_valid("exe=exact:/usr/bin/grim", err, sizeof(err)));

	CHECK(!permission_subject_valid("exe:", err, sizeof(err)));
	CHECK(!permission_subject_valid("regex:[", err, sizeof(err)));
	CHECK(permission_rule_add("regex:[", PERM_CAPTURE, PERM_ALLOW, err, sizeof(err)) < 0);
	CHECK(permission_rule_count() == 0);

	// caseless matching reaches the stored pattern
	CHECK(permission_rule_add("exe:glob:i:*FIREFOX", PERM_CAPTURE, PERM_DENY, err, sizeof(err)) >= 0);
	perm_subject_t ff = subject_of("/usr/bin/firefox");
	CHECK(permission_decide(&ff, PERM_CAPTURE).decision == PERM_DENY);
}

static void test_unknown_subject(void) {
	clean_state();

	perm_subject_t unknown = {0};

	perm_result_t r = permission_decide(&unknown, PERM_CAPTURE);
	CHECK(r.decision == PERM_ALLOW);
	CHECK_STREQ(r.source, "default");

	// an opted-in prompt must not become an allow when the exe is unreadable
	permission_set_default(PERM_CAPTURE, PERM_ASK);
	r = permission_decide(&unknown, PERM_CAPTURE);
	CHECK(r.decision == PERM_DENY);
	CHECK_STREQ(r.source, "default");

	// a known subject with no rule still resolves to ask
	perm_subject_t known = subject_of("/usr/bin/unknown");
	CHECK(permission_decide(&known, PERM_CAPTURE).decision == PERM_ASK);
}

static void test_remember(void) {
	clean_state();

	char err[256] = {0};
	int64_t past = (int64_t)time(NULL) - 60;

	CHECK(permission_remember_add("/usr/bin/grim", PERM_CAPTURE, PERM_DENY, 0, true));

	perm_subject_t grim = subject_of("/usr/bin/grim");
	perm_result_t r = permission_decide(&grim, PERM_CAPTURE);
	CHECK(r.decision == PERM_DENY);
	CHECK_STREQ(r.source, "remember");

	// policy outranks a remembered answer and marks it shadowed
	CHECK(permission_rule_add("exe:/usr/bin/grim", PERM_CAPTURE, PERM_ALLOW, err, sizeof(err)) >= 0);
	CHECK(permission_decide(&grim, PERM_CAPTURE).decision == PERM_ALLOW);

	perm_remember_info_t info;
	CHECK(permission_remember_at(0, &info));
	CHECK_STREQ(info.exe, "/usr/bin/grim");
	CHECK(info.cap == PERM_CAPTURE);
	CHECK(info.decision == PERM_DENY);
	CHECK(info.expires == 0);
	CHECK(info.persistent);
	CHECK(info.shadow_rule == 0);

	// an expired entry is ignored and dropped
	CHECK(permission_remember_add("/usr/bin/old", PERM_CAPTURE, PERM_DENY, past, true));
	CHECK(permission_remember_count() == 1);
	perm_subject_t old = subject_of("/usr/bin/old");
	CHECK(permission_decide(&old, PERM_CAPTURE).decision == PERM_ALLOW);

	// only matching entries are forgotten
	CHECK(permission_remember_add("/usr/lib/firefox/firefox", PERM_CAPTURE, PERM_ALLOW, 0, true));
	CHECK(permission_remember_count() == 2);
	CHECK(permission_remember_matching("exe:*firefox", err, sizeof(err)) == 1);
	CHECK(permission_remember_count() == 2);
	CHECK(permission_remember_forget("exe:*firefox", err, sizeof(err)) == 1);
	CHECK(permission_remember_count() == 1);
	CHECK(permission_remember_forget("regex:[", err, sizeof(err)) == -1);
	CHECK(permission_remember_count() == 1);
}

static void test_persistence(void) {
	clean_state();

	CHECK(permission_remember_add("/usr/bin/persist", PERM_CAPTURE, PERM_DENY, 0, true));
	CHECK(permission_remember_add("/usr/bin/session", PERM_CAPTURE, PERM_DENY, 0, false));

	permission_fini();
	permission_init();

	CHECK(permission_remember_count() == 1);
	perm_remember_info_t info;
	CHECK(permission_remember_at(0, &info));
	CHECK_STREQ(info.exe, "/usr/bin/persist");
	CHECK(info.persistent);

	perm_subject_t persist = subject_of("/usr/bin/persist");
	CHECK(permission_decide(&persist, PERM_CAPTURE).decision == PERM_DENY);

	perm_subject_t session = subject_of("/usr/bin/session");
	CHECK(permission_decide(&session, PERM_CAPTURE).decision == PERM_ALLOW);

	CHECK(permission_state_path() != NULL);
	FILE *f = permission_state_path() ? fopen(permission_state_path(), "r") : NULL;
	CHECK(f != NULL);
	if (f) {
		char line[RULE_RE_MAX + 128];
		CHECK(fgets(line, sizeof(line), f) != NULL);
		CHECK(strchr(line, '\t') != NULL);
		fclose(f);
	}
}

static void run_tests(void) {
	char tmp[] = "/tmp/doors-permissions-XXXXXX";
	char *dir = mkdtemp(tmp);
	CHECK(dir != NULL);
	if (!dir)
		return;

	setenv("XDG_STATE_HOME", dir, 1);
	permission_init();

	test_names();
	test_subject_and_cache();
	test_policy_precedence();
	test_patterns();
	test_unknown_subject();
	test_remember();
	test_persistence();

	char path[PATH_MAX];
	str_copy(path, sizeof(path), permission_state_path() ? permission_state_path() : "");
	permission_fini();

	if (path[0])
		unlink(path);

	char sub[PATH_MAX];
	snprintf(sub, sizeof(sub), "%s/doors", dir);
	rmdir(sub);
	rmdir(dir);

	unsetenv("XDG_STATE_HOME");
}

TEST_MAIN(run_tests)
