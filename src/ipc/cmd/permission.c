#include "ipc/args.h"
#include "ipc/ipc.h"
#include "ipc/registry.h"
#include "permission.h"
#include "str.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
	perm_cap_t cap;
	perm_decision_t mode;
} perm_assignment_t;

#define MAX_ASSIGNMENTS 16

static bool parse_assignments(ipc_args_t *a, perm_assignment_t *list, size_t *count) {
	const char *arg = ipc_peek(a);
	if (!arg) {
		ipc_fail(a, "Missing <capability>=<mode>\n");
		return false;
	}

	size_t n = 0;
	ipc_foreach(a, word) {
		const char *eq = strchr(word, '=');
		if (!eq || eq == word || eq[1] == '\0') {
			ipc_fail(a, "Expected <capability>=<mode>, got \"%s\"\n", word);
			return false;
		}

		char name[32];
		size_t len = (size_t)(eq - word);
		if (len >= sizeof(name)) {
			ipc_fail(a, "Unknown capability \"%.*s\"\n", (int)len, word);
			return false;
		}
		memcpy(name, word, len);
		name[len] = '\0';

		perm_cap_t cap;
		perm_decision_t mode;
		if (!permission_cap_parse(name, &cap)) {
			ipc_fail(a, "Unknown capability \"%s\", expected capture, output or inject\n", name);
			return false;
		}
		if (!permission_mode_parse(eq + 1, &mode)) {
			ipc_fail(a, "Unknown mode \"%s\", expected allow, deny or ask\n", eq + 1);
			return false;
		}

		if (n == MAX_ASSIGNMENTS) {
			ipc_fail(a, "Too many assignments, the limit is %d\n", MAX_ASSIGNMENTS);
			return false;
		}

		list[n].cap = cap;
		list[n].mode = mode;
		n++;
	}

	*count = n;
	return true;
}

static void permission_list(ipc_args_t *a) {
	IPC_REPLY(b);

	ipc_buff(&b, "default");
	for (int cap = 0; cap < PERM_CAP_COUNT; cap++) {
		ipc_buff(&b, "\t%s=%s", permission_cap_name((perm_cap_t)cap),
			permission_mode_name(permission_default((perm_cap_t)cap)));
	}
	ipc_buff(&b, "\n");

	size_t rule_count = permission_rule_count();
	for (size_t i = 0; i < rule_count; i++) {
		perm_rule_info_t info;
		if (!permission_rule_at(i, &info))
			continue;
		ipc_buff(&b, "%zu\texe:%s\t%s=%s\n", i, info.spec, permission_cap_name(info.cap),
			permission_mode_name(info.decision));
	}

	size_t remember_count = permission_remember_count();
	for (size_t i = 0; i < remember_count; i++) {
		perm_remember_info_t info;
		if (!permission_remember_at(i, &info))
			continue;

		char expiry[48];
		if (info.expires)
			snprintf(expiry, sizeof(expiry), "expires=%lld", (long long)info.expires);
		else
			snprintf(expiry, sizeof(expiry), "expires=never");

		ipc_buff(&b, "remember\texe:%s\t%s=%s\t%s", info.exe, permission_cap_name(info.cap),
			permission_mode_name(info.decision), expiry);
		if (info.shadow_rule >= 0)
			ipc_buff(&b, "\tshadowed by rule %d", info.shadow_rule);
		ipc_buff(&b, "\n");
	}

	size_t pending_count = permission_pending_count();
	for (size_t i = 0; i < pending_count; i++) {
		perm_pending_info_t info;
		if (!permission_pending_at(i, &info))
			continue;

		const perm_subject_t *s = info.subject;
		ipc_buff(&b, "pending\t%s\t%s\texe:%s", info.token, permission_cap_name(info.cap),
			s->known && s->exe[0] ? s->exe : "?");
		if (s->app_id[0])
			ipc_buff(&b, "\tapp_id=%s", s->app_id);
		ipc_buff(&b, "\t%ds left\n", info.seconds_left);
	}

	ipc_buff(&b, "%zu rule%s, %zu remembered, %zu pending\n", rule_count, rule_count == 1 ? "" : "s",
		remember_count, pending_count);
	ipc_buf_send(a, &b);
}

static void permission_add(ipc_args_t *a, const char *subject) {
	perm_assignment_t list[MAX_ASSIGNMENTS];
	size_t n = 0;
	if (!parse_assignments(a, list, &n))
		return;

	char err[256] = {0};
	if (!permission_subject_valid(subject, err, sizeof(err))) {
		ipc_fail(a, "exe: %s\n", err);
		return;
	}

	if (permission_rule_count() + n > MAX_PERMISSION_RULES) {
		ipc_fail(a, "Too many permission rules, the limit is %d\n", MAX_PERMISSION_RULES);
		return;
	}

	if (a->dry_run) {
		ipc_okf(a, "%zu permission rule%s would be added\n", n, n == 1 ? "" : "s");
		return;
	}

	for (size_t i = 0; i < n; i++) {
		if (permission_rule_add(subject, list[i].cap, list[i].mode, err, sizeof(err)) < 0) {
			ipc_fail(a, "exe: %s\n", err);
			return;
		}
	}

	ipc_okf(a, "%zu permission rule%s added\n", n, n == 1 ? "" : "s");
}

static void permission_set_defaults(ipc_args_t *a) {
	perm_assignment_t list[MAX_ASSIGNMENTS];
	size_t n = 0;
	if (!parse_assignments(a, list, &n))
		return;

	if (a->dry_run) {
		ipc_okf(a, "%zu default%s would be set\n", n, n == 1 ? "" : "s");
		return;
	}

	for (size_t i = 0; i < n; i++)
		permission_set_default(list[i].cap, list[i].mode);

	ipc_okf(a, "%zu default%s set\n", n, n == 1 ? "" : "s");
}

static void permission_remove(ipc_args_t *a) {
	int idx;
	if (!ipc_int(a, "index", 0, INT_MAX, &idx))
		return;

	perm_rule_info_t info;
	if (!permission_rule_at((size_t)idx, &info)) {
		ipc_fail(a, "Invalid index %d\n", idx);
		return;
	}

	if (a->dry_run) {
		ipc_okf(a, "permission rule %d would be removed\n", idx);
		return;
	}

	permission_rule_remove(idx);
	ipc_okf(a, "Permission rule %d removed\n", idx);
}

static void permission_forget(ipc_args_t *a) {
	const char *subject = ipc_peek(a);
	if (subject)
		ipc_take(a);
	if (!ipc_end(a))
		return;

	char err[256] = {0};
	if (subject && !permission_subject_valid(subject, err, sizeof(err))) {
		ipc_fail(a, "exe: %s\n", err);
		return;
	}

	if (a->dry_run) {
		size_t n = subject ? permission_remember_matching(subject, err,
			sizeof(err)) : permission_remember_count();
		ipc_okf(a, "%zu remembered permission%s would be forgotten\n", n, n == 1 ? "" : "s");
		return;
	}

	int n = permission_remember_forget(subject, err, sizeof(err));
	if (n < 0) {
		ipc_fail(a, "exe: %s\n", err);
		return;
	}

	if (n == 0)
		ipc_ok(a, subject ? "No matching remembered permissions\n" : "No remembered permissions\n");
	else
		ipc_okf(a, "%d remembered permission%s forgotten\n", n, n == 1 ? "" : "s");
}

static bool pending_token_valid(const char *token) {
	if (strlen(token) != PERM_TOKEN_LEN)
		return false;

	for (const char *c = token; *c; c++) {
		if (str_hex_digit(*c) < 0)
			return false;
	}
	return true;
}

static bool pending_token_known(const char *token) {
	size_t n = permission_pending_count();
	for (size_t i = 0; i < n; i++) {
		perm_pending_info_t info;
		if (permission_pending_at(i, &info) && streq(info.token, token))
			return true;
	}
	return false;
}

static void permission_answer_prompt(ipc_args_t *a) {
	const char *token = ipc_peek(a);
	if (!token) {
		ipc_fail(a, "Missing <token>\n");
		return;
	}
	if (!pending_token_valid(token)) {
		ipc_fail(a, "Expected a %d digit hex token, got \"%s\"\n", PERM_TOKEN_LEN, token);
		return;
	}
	if (!pending_token_known(token)) {
		ipc_fail(a, "No pending permission request with token \"%s\"\n", token);
		return;
	}
	ipc_take(a);

	const char *mode = ipc_peek(a);
	if (!mode) {
		ipc_fail(a, "Missing <allow|deny>\n");
		return;
	}

	perm_decision_t decision;
	if (!permission_mode_parse(mode, &decision) || decision == PERM_ASK) {
		ipc_fail(a, "Expected allow or deny, got \"%s\"\n", mode);
		return;
	}
	ipc_take(a);

	perm_remember_t remember = PERM_REMEMBER_NONE;
	const char *opt;
	while ((opt = ipc_peek(a))) {
		ipc_take(a);

		const char *value = str_skip_prefix(opt, "--remember=");
		if (!value) {
			ipc_fail(a, "Unknown argument \"%s\"\n", opt);
			return;
		}
		if (streq(value, "session")) {
			remember = PERM_REMEMBER_SESSION;
		} else if (streq(value, "always")) {
			remember = PERM_REMEMBER_ALWAYS;
		} else {
			ipc_fail(a, "Expected --remember=session or --remember=always, got \"%s\"\n", opt);
			return;
		}
	}

	if (a->dry_run) {
		ipc_okf(a, "Permission request %s would be answered %s\n", token, permission_mode_name(decision));
		return;
	}

	permission_answer_token(token, decision, remember);
	ipc_okf(a, "Permission request %s %s\n", token, permission_mode_name(decision));
}

const ipc_sub_t permission_subs[] = {
	IPC_SUB("default", NULL, "permission default <capability>=<mode> ...", permission_set_defaults),
	IPC_SUB("answer", NULL, "permission answer <token> <allow|deny> [--remember=session|always]",
		permission_answer_prompt),
	IPC_SUB("-l", "--list", "permission -l | --list", permission_list),
	IPC_SUB("-r", "--remove", "permission -r | --remove <index>", permission_remove),
	IPC_SUB("--forget", NULL, "permission --forget [<subject>]", permission_forget),
	IPC_SUB_END,
};

static void permission_usage(ipc_args_t *a) {
	IPC_REPLY(b);
	ipc_buff(&b, "Usage:\n  permission <subject> <capability>=<mode> ...\n");
	ipc_buff(&b, "  permission answer <token> <allow|deny> [--remember=session|always]\n");
	ipc_sub_usage(&b, permission_subs);
	ipc_buff(&b, "\n");
	send_failure(a->fd, b.buf);
}

void ipc_cmd_permission(ipc_args_t *a) {
	const char *first = ipc_peek(a);
	if (!first) {
		permission_usage(a);
		return;
	}

	if (first[0] == '-' || streq(first, "default") || streq(first, "answer")) {
		if (!ipc_sub_dispatch(a, permission_subs))
			ipc_fail_unknown(a, permission_subs);
		return;
	}

	ipc_take(a);
	permission_add(a, first);
}
