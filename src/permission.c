#include "permission.h"
#include "fs.h"
#include "server.h"
#include "str.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wlr/types/wlr_security_context_v1.h>
#include <wlr/util/log.h>

typedef struct {
	rule_pattern_t exe;
	perm_cap_t cap;
	perm_decision_t decision;
} perm_rule_t;

typedef struct {
	char exe[RULE_RE_MAX];
	perm_cap_t cap;
	perm_decision_t decision;
	int64_t expires;
	bool persistent;
} perm_remember_entry_t;

typedef struct {
	struct wl_client *client;
	perm_subject_t subject;
	struct wl_listener destroy;
	perm_decision_t decision[PERM_CAP_COUNT];
	bool has[PERM_CAP_COUNT];
	struct wl_list link;
} perm_client_t;

static const char *const cap_names[PERM_CAP_COUNT] = {
	[PERM_CAPTURE] = "capture",
	[PERM_OUTPUT] = "output",
	[PERM_INJECT] = "inject",
};

static const char *const mode_names[] = {
	[PERM_DENY] = "deny",
	[PERM_ALLOW] = "allow",
	[PERM_ASK] = "ask",
};

static perm_rule_t rules[MAX_PERMISSION_RULES];
static size_t rule_len;

static perm_remember_entry_t remembered[MAX_PERMISSION_REMEMBER];
static size_t remember_len;

static perm_decision_t defaults[PERM_CAP_COUNT] = {
	[PERM_CAPTURE] = PERM_ALLOW,
	[PERM_OUTPUT] = PERM_ALLOW,
	[PERM_INJECT] = PERM_ALLOW,
};

static struct wl_list clients; // perm_client_t.link

static char state_path[PATH_MAX + 16];
static char state_dir[PATH_MAX + 16];

static void remember_purge(void);
static bool remember_save(void);

const char *permission_cap_name(perm_cap_t cap) {
	if (cap < 0 || cap >= PERM_CAP_COUNT)
		return "?";
	return cap_names[cap];
}

const char *permission_mode_name(perm_decision_t decision) {
	if (decision < PERM_DENY || decision > PERM_ASK)
		return "?";
	return mode_names[decision];
}

bool permission_cap_parse(const char *name, perm_cap_t *out) {
	for (int i = 0; i < PERM_CAP_COUNT; i++) {
		if (streqi(name, cap_names[i])) {
			*out = (perm_cap_t)i;
			return true;
		}
	}
	return false;
}

bool permission_mode_parse(const char *name, perm_decision_t *out) {
	for (int i = PERM_DENY; i <= PERM_ASK; i++) {
		if (streqi(name, mode_names[i])) {
			*out = (perm_decision_t)i;
			return true;
		}
	}
	return false;
}

static bool subject_pattern(rule_pattern_t *p, const char *spec, char *err, size_t errsz) {
	const char *pat = spec;

	if (!spec) {
		snprintf(err, errsz, "missing pattern");
		return false;
	}

	if (str_starts(pat, "exe:"))
		pat += 4;
	else if (str_starts(pat, "exe="))
		pat += 4;

	if (pat[0] == '\0') {
		snprintf(err, errsz, "empty pattern");
		return false;
	}

	bool explicit_mode = str_starts(pat, "exact:") || str_starts(pat, "glob:") ||
		str_starts(pat, "regex:");

	char buf[RULE_RE_MAX + 16];
	if (!explicit_mode && (strchr(pat, '*') || strchr(pat, '?')))
		snprintf(buf, sizeof(buf), "glob:%s", pat);
	else
		snprintf(buf, sizeof(buf), "%s", pat);

	return rule_pattern_set(p, buf, err, errsz);
}

bool permission_subject_valid(const char *spec, char *err, size_t errsz) {
	rule_pattern_t p = {0};
	if (!subject_pattern(&p, spec, err, errsz))
		return false;
	rule_pattern_clear(&p);
	return true;
}

bool permission_subject(struct wl_client *client, perm_subject_t *out) {
	memset(out, 0, sizeof(*out));

	if (!client)
		return false;

	uid_t uid;
	gid_t gid;
	wl_client_get_credentials(client, &out->pid, &uid, &gid);

	char proc[64];
	snprintf(proc, sizeof(proc), "/proc/%d/exe", (int)out->pid);

	char path[PATH_MAX];
	ssize_t len = readlink(proc, path, sizeof(path) - 1);
	if (len > 0) {
		path[len] = '\0';

		static const char deleted[] = " (deleted)";
		size_t deleted_len = sizeof(deleted) - 1;
		size_t path_len = (size_t)len;
		if (path_len > deleted_len &&
				strcmp(path + path_len - deleted_len, deleted) == 0)
			path[path_len - deleted_len] = '\0';

		str_copy(out->exe, sizeof(out->exe), path);
		out->known = true;
	}

	if (server.security_context_manager_v1) {
		const struct wlr_security_context_v1_state *ctx =
			wlr_security_context_manager_v1_lookup_client(
				server.security_context_manager_v1, client);
		if (ctx) {
			out->sandboxed = true;
			str_copy(out->app_id, sizeof(out->app_id), ctx->app_id ? ctx->app_id : "");
		}
	}

	return true;
}

static int matching_rule(const char *exe, perm_cap_t cap) {
	for (size_t i = rule_len; i > 0; i--) {
		const perm_rule_t *r = &rules[i - 1];
		if (r->cap == cap && rule_pattern_match(&r->exe, exe))
			return (int)(i - 1);
	}
	return -1;
}

static bool remember_expired(const perm_remember_entry_t *e, int64_t now) {
	return e->expires != 0 && e->expires <= now;
}

static int remembered_entry(const char *exe, perm_cap_t cap) {
	int64_t now = (int64_t)time(NULL);

	for (size_t i = remember_len; i > 0; i--) {
		const perm_remember_entry_t *e = &remembered[i - 1];
		if (e->cap != cap || remember_expired(e, now))
			continue;
		if (streq(e->exe, exe))
			return (int)(i - 1);
	}
	return -1;
}

perm_result_t permission_decide(const perm_subject_t *subject, perm_cap_t cap) {
	perm_result_t result = {
		.decision = defaults[cap],
		.source = "default",
		.rule_index = -1,
	};

	if (!subject || !subject->known) {
		if (result.decision == PERM_ASK)
			result.decision = PERM_DENY;
		return result;
	}

	int rule = matching_rule(subject->exe, cap);
	if (rule >= 0) {
		result.decision = rules[rule].decision;
		result.source = "policy";
		result.rule_index = rule;
		return result;
	}

	int entry = remembered_entry(subject->exe, cap);
	if (entry >= 0) {
		result.decision = remembered[entry].decision;
		result.source = "remember";
	}

	return result;
}

static void client_destroyed(struct wl_listener *listener, void *data) {
	(void)data;
	perm_client_t *c = wl_container_of(listener, c, destroy);
	wl_list_remove(&c->destroy.link);
	wl_list_remove(&c->link);
	free(c);
}

void permission_cache_clear(void) {
	perm_client_t *c, *tmp;
	wl_list_for_each_safe(c, tmp, &clients, link) {
		wl_list_remove(&c->destroy.link);
		wl_list_remove(&c->link);
		free(c);
	}
}

size_t permission_cache_count(void) {
	size_t n = 0;
	perm_client_t *c;
	wl_list_for_each(c, &clients, link) {
		for (int cap = 0; cap < PERM_CAP_COUNT; cap++) {
			if (!c->has[cap])
				continue;
			n++;
			break;
		}
	}
	return n;
}

static perm_client_t *client_entry(struct wl_client *client) {
	perm_client_t *c;
	wl_list_for_each(c, &clients, link) {
		if (c->client == client)
			return c;
	}

	c = calloc(1, sizeof(*c));
	if (!c)
		return NULL;

	c->client = client;
	permission_subject(client, &c->subject);
	c->destroy.notify = client_destroyed;
	wl_client_add_destroy_listener(client, &c->destroy);
	wl_list_insert(&clients, &c->link);
	return c;
}

perm_result_t permission_check(struct wl_client *client, perm_cap_t cap) {
	perm_client_t *c = client ? client_entry(client) : NULL;

	if (c && c->has[cap])
		return (perm_result_t){.decision = c->decision[cap], .source = "answer", .rule_index = -1};

	perm_subject_t subject = {0};
	if (c)
		subject = c->subject;
	else
		permission_subject(NULL, &subject);

	return permission_decide(&subject, cap);
}

void permission_answer(struct wl_client *client, const perm_subject_t *subject, perm_cap_t cap,
		perm_decision_t decision, perm_remember_t remember) {
	if (remember != PERM_REMEMBER_NONE) {
		if (!subject || !subject->known) {
			wlr_log(WLR_ERROR, "permission: cannot remember %s for an unknown executable",
				permission_cap_name(cap));
		} else if (strlen(subject->exe) >= RULE_RE_MAX) {
			wlr_log(WLR_ERROR, "permission: executable path is too long to remember");
		} else {
			permission_cache_clear();
			if (!permission_remember_add(subject->exe, cap, decision, 0,
					remember == PERM_REMEMBER_ALWAYS))
				wlr_log(WLR_ERROR, "permission: too many remembered answers");
		}
	}

	if (!client)
		return;

	perm_client_t *c = client_entry(client);
	if (!c)
		return;

	c->has[cap] = true;
	c->decision[cap] = decision;
}

int permission_rule_add(const char *exe_spec, perm_cap_t cap, perm_decision_t decision, char *err,
		size_t errsz) {
	if (rule_len >= MAX_PERMISSION_RULES) {
		snprintf(err, errsz, "too many permission rules, the limit is %d",
			MAX_PERMISSION_RULES);
		return -1;
	}

	rule_pattern_t p = {0};
	if (!subject_pattern(&p, exe_spec, err, errsz))
		return -1;

	rules[rule_len].exe = p;
	rules[rule_len].cap = cap;
	rules[rule_len].decision = decision;
	int index = (int)rule_len++;

	permission_cache_clear();

	return index;
}

bool permission_rule_remove(int index) {
	if (index < 0 || (size_t)index >= rule_len)
		return false;

	rule_pattern_clear(&rules[index].exe);

	for (size_t i = (size_t)index; i + 1 < rule_len; i++)
		rules[i] = rules[i + 1];

	rule_len--;
	permission_cache_clear();
	return true;
}

size_t permission_rule_count(void) {
	return rule_len;
}

bool permission_rule_at(size_t index, perm_rule_info_t *out) {
	if (index >= rule_len)
		return false;

	rule_pattern_spec(&rules[index].exe, out->spec, sizeof(out->spec));
	out->cap = rules[index].cap;
	out->decision = rules[index].decision;
	return true;
}

perm_decision_t permission_default(perm_cap_t cap) {
	return defaults[cap];
}

void permission_set_default(perm_cap_t cap, perm_decision_t decision) {
	if (defaults[cap] == decision)
		return;

	defaults[cap] = decision;
	permission_cache_clear();
}

static void remember_purge(void) {
	int64_t now = (int64_t)time(NULL);
	size_t kept = 0;

	for (size_t i = 0; i < remember_len; i++) {
		if (remember_expired(&remembered[i], now))
			continue;
		if (kept != i)
			remembered[kept] = remembered[i];
		kept++;
	}

	remember_len = kept;
}

size_t permission_remember_count(void) {
	remember_purge();
	return remember_len;
}

bool permission_remember_at(size_t index, perm_remember_info_t *out) {
	remember_purge();
	if (index >= remember_len)
		return false;

	const perm_remember_entry_t *e = &remembered[index];
	out->exe = e->exe;
	out->cap = e->cap;
	out->decision = e->decision;
	out->expires = e->expires;
	out->persistent = e->persistent;
	out->shadow_rule = matching_rule(e->exe, e->cap);
	return true;
}

static bool remember_put(const char *exe, perm_cap_t cap, perm_decision_t decision,
		int64_t expires, bool persistent) {
	if (!exe || exe[0] == '\0' || strlen(exe) >= RULE_RE_MAX)
		return false;

	for (size_t i = 0; i < remember_len; i++) {
		if (remembered[i].cap != cap || !streq(remembered[i].exe, exe))
			continue;

		remembered[i].decision = decision;
		remembered[i].expires = expires;
		remembered[i].persistent = persistent;
		return true;
	}

	if (remember_len >= MAX_PERMISSION_REMEMBER)
		return false;

	perm_remember_entry_t *e = &remembered[remember_len++];
	str_copy(e->exe, sizeof(e->exe), exe);
	e->cap = cap;
	e->decision = decision;
	e->expires = expires;
	e->persistent = persistent;
	return true;
}

bool permission_remember_add(const char *exe, perm_cap_t cap, perm_decision_t decision,
		int64_t expires, bool persistent) {
	if (!remember_put(exe, cap, decision, expires, persistent))
		return false;

	if (persistent)
		remember_save();

	permission_cache_clear();

	return true;
}

static bool forget_spec(rule_pattern_t *p, const char *spec, char *err, size_t errsz) {
	if (!spec)
		return true;
	return subject_pattern(p, spec, err, errsz);
}

static bool forget_matches(const rule_pattern_t *p, const perm_remember_entry_t *e) {
	if (!p->pattern[0])
		return true;
	return rule_pattern_match(p, e->exe);
}

size_t permission_remember_matching(const char *spec, char *err, size_t errsz) {
	rule_pattern_t p = {0};
	if (!forget_spec(&p, spec, err, errsz))
		return 0;

	remember_purge();

	size_t n = 0;
	for (size_t i = 0; i < remember_len; i++) {
		if (forget_matches(&p, &remembered[i]))
			n++;
	}

	rule_pattern_clear(&p);
	return n;
}

int permission_remember_forget(const char *spec, char *err, size_t errsz) {
	rule_pattern_t p = {0};
	if (!forget_spec(&p, spec, err, errsz))
		return -1;

	remember_purge();

	int removed = 0;
	size_t kept = 0;
	for (size_t i = 0; i < remember_len; i++) {
		if (forget_matches(&p, &remembered[i])) {
			removed++;
			continue;
		}
		if (kept != i)
			remembered[kept] = remembered[i];
		kept++;
	}
	remember_len = kept;

	rule_pattern_clear(&p);

	if (removed) {
		permission_cache_clear();
		remember_save();
	}

	return removed;
}

const char *permission_state_path(void) {
	return state_path[0] ? state_path : NULL;
}

static void state_paths(void) {
	state_path[0] = '\0';
	state_dir[0] = '\0';

	const char *base = getenv("XDG_STATE_HOME");
	char fallback[PATH_MAX + 16];

	if (!base || base[0] == '\0') {
		const char *home = getenv("HOME");
		if (!home || home[0] == '\0')
			return;
		str_copy(fallback, sizeof(fallback), home);
		str_append(fallback, sizeof(fallback), "/.local/state");
		base = fallback;
	}

	str_copy(state_dir, sizeof(state_dir), base);
	str_append(state_dir, sizeof(state_dir), "/doors");
	str_copy(state_path, sizeof(state_path), state_dir);
	str_append(state_path, sizeof(state_path), "/permissions");
}

static bool remember_save(void) {
	if (state_path[0] == '\0')
		return false;

	remember_purge();

	if (!mkdir_p(state_dir, 0700)) {
		wlr_log(WLR_ERROR, "permission: cannot create %s", state_dir);
		return false;
	}

	char tmp[PATH_MAX + 32];
	snprintf(tmp, sizeof(tmp), "%s.tmp", state_path);

	FILE *f = fopen(tmp, "w");
	if (!f) {
		wlr_log(WLR_ERROR, "permission: cannot write %s: %s", tmp, strerror(errno));
		return false;
	}

	for (size_t i = 0; i < remember_len; i++) {
		const perm_remember_entry_t *e = &remembered[i];
		if (!e->persistent)
			continue;
		fprintf(f, "%s\t%s\t%s\t%lld\n", e->exe, permission_cap_name(e->cap),
			permission_mode_name(e->decision), (long long)e->expires);
	}

	bool ok = !ferror(f);
	if (ok)
		fchmod(fileno(f), 0600);
	if (fclose(f) != 0)
		ok = false;

	if (!ok) {
		wlr_log(WLR_ERROR, "permission: failed to write %s", tmp);
		unlink(tmp);
		return false;
	}

	if (rename(tmp, state_path) != 0) {
		wlr_log(WLR_ERROR, "permission: cannot replace %s: %s", state_path, strerror(errno));
		unlink(tmp);
		return false;
	}

	return true;
}

static void remember_load(void) {
	if (state_path[0] == '\0')
		return;

	FILE *f = fopen(state_path, "r");
	if (!f)
		return;

	char line[RULE_RE_MAX + 128];
	int lineno = 0;

	while (fgets(line, sizeof(line), f)) {
		lineno++;
		str_chomp(line);

		if (line[0] == '\0' || line[0] == '#')
			continue;

		char *fields[4];
		if (str_split(line, "\t", fields, 4) != 4) {
			wlr_log(WLR_ERROR, "permission: %s:%d: malformed line", state_path, lineno);
			continue;
		}

		perm_cap_t cap;
		perm_decision_t decision;
		if (!permission_cap_parse(fields[1], &cap) ||
				!permission_mode_parse(fields[2], &decision)) {
			wlr_log(WLR_ERROR, "permission: %s:%d: unknown capability or mode", state_path,
				lineno);
			continue;
		}

		if (decision == PERM_ASK) {
			wlr_log(WLR_ERROR, "permission: %s:%d: ask is not a remembered answer",
				state_path, lineno);
			continue;
		}

		char *end;
		long long expires = strtoll(fields[3], &end, 10);
		if (end == fields[3] || *end != '\0' || expires < 0) {
			wlr_log(WLR_ERROR, "permission: %s:%d: invalid expiry", state_path, lineno);
			continue;
		}

		if (!remember_put(fields[0], cap, decision, expires, true)) {
			wlr_log(WLR_ERROR, "permission: %s:%d: entry not loaded", state_path, lineno);
			continue;
		}
	}

	fclose(f);
	remember_purge();
}

void permission_init(void) {
	rule_len = 0;
	remember_len = 0;
	defaults[PERM_CAPTURE] = PERM_ALLOW;
	defaults[PERM_OUTPUT] = PERM_ALLOW;
	defaults[PERM_INJECT] = PERM_ALLOW;
	wl_list_init(&clients);

	state_paths();
	remember_load();

	if (state_path[0] && remember_len > 0)
		wlr_log(WLR_INFO, "Permissions: loaded %zu remembered answers from %s",
			remember_len, state_path);
}

void permission_fini(void) {
	for (size_t i = 0; i < rule_len; i++)
		rule_pattern_clear(&rules[i].exe);
	rule_len = 0;

	permission_cache_clear();
	remember_len = 0;

	state_path[0] = '\0';
	state_dir[0] = '\0';
}
