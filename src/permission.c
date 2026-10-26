#include "fs.h"
#include "ipc/ipc.h"
#include "permission.h"
#include "server.h"
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
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

typedef struct perm_pending_t {
	char token[PERM_TOKEN_LEN + 1];
	struct wl_client *client;
	perm_subject_t subject;
	perm_cap_t cap;
	struct wl_list waiters; // perm_waiter_t.link
	struct wl_event_source *timer;
	struct wl_listener client_destroy;
	int64_t deadline_ms;
	struct wl_list link;
} perm_pending_t;

struct perm_waiter {
	struct wl_list link;
	perm_resolve_fn resolve;
	void *data;
	perm_pending_t *pending;
	bool cancelled;
};

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

static int64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const char *subject_name(const perm_subject_t *s) {
	return s->known && s->exe[0] ? s->exe : "?";
}

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
static struct wl_list pending; // perm_pending_t.link

static uint32_t ask_timeout_ms = PERM_ASK_TIMEOUT_MS;
static uint32_t token_counter;

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

	bool explicit_mode = str_starts(pat, "exact:") || str_starts(pat, "glob:") || str_starts(pat,
		"regex:");

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
		if (path_len > deleted_len && strcmp(path + path_len - deleted_len, deleted) == 0)
			path[path_len - deleted_len] = '\0';

		str_copy(out->exe, sizeof(out->exe), path);
		out->known = true;
	}

	if (server.security_context_manager_v1) {
		const struct wlr_security_context_v1_state *ctx =
			wlr_security_context_manager_v1_lookup_client(server.security_context_manager_v1, client);
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
		return (perm_result_t){
			.decision = c->decision[cap],
			.source = "answer",
			.rule_index = -1
		};

	perm_subject_t subject = {0};
	if (c)
		subject = c->subject;
	else
		permission_subject(NULL, &subject);

	return permission_decide(&subject, cap);
}

static void pending_publish_request(const perm_pending_t *p) {
	char app[SMALEN + 16] = "";
	if (p->subject.app_id[0])
		snprintf(app, sizeof(app), " app_id=%s", p->subject.app_id);

	ipc_put_status(SUB_MASK_PERMISSION, "permission_request %s %s exe=%s%s\n", p->token,
		permission_cap_name(p->cap), subject_name(&p->subject), app);
}

static perm_pending_t *pending_find(struct wl_client *client, perm_cap_t cap) {
	perm_pending_t *p;
	wl_list_for_each(p, &pending, link) {
		if (p->client == client && p->cap == cap)
			return p;
	}
	return NULL;
}

static void pending_make_token(char out[PERM_TOKEN_LEN + 1]) {
	uint32_t value = 0;
	if (getrandom(&value, sizeof(value), 0) != (ssize_t)sizeof(value)) {
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		value = (uint32_t)ts.tv_nsec ^ (uint32_t)ts.tv_sec ^ ((uint32_t)getpid() << 16) ^ ++token_counter;
	}

	for (; ; ) {
		snprintf(out, PERM_TOKEN_LEN + 1, "%08x", (unsigned)value);

		bool used = false;
		perm_pending_t *p;
		wl_list_for_each(p, &pending, link) {
			if (streq(p->token, out)) {
				used = true;
				break;
			}
		}
		if (!used)
			return;

		value++;
	}
}

static void pending_destroy(perm_pending_t *p) {
	if (p->timer)
		wl_event_source_remove(p->timer);
	if (p->client_destroy.link.next)
		wl_list_remove(&p->client_destroy.link);
	wl_list_remove(&p->link);
	free(p);
}

static void pending_resolve(perm_pending_t *p, perm_decision_t decision, const char *reason) {
	char token[PERM_TOKEN_LEN + 1];
	str_copy(token, sizeof(token), p->token);
	perm_cap_t cap = p->cap;

	// detach the waiters so a resolving callback may destroy the frame that
	// owns its waiter without touching the pending request
	struct wl_list waiters;
	wl_list_init(&waiters);
	while (!wl_list_empty(&p->waiters)) {
		struct wl_list *link = p->waiters.next;
		wl_list_remove(link);
		wl_list_insert(&waiters, link);
	}

	pending_destroy(p);

	ipc_put_status(SUB_MASK_PERMISSION, "permission_resolved %s %s %s\n", token,
		permission_mode_name(decision), reason);
	wlr_log(WLR_INFO, "permission: %s %s resolved %s (%s)", permission_cap_name(cap), token,
		permission_mode_name(decision), reason);

	// callbacks can destroy frames that own another waiter in this list, so
	// never cache the next link across a callback
	while (!wl_list_empty(&waiters)) {
		perm_waiter_t *w = wl_container_of(waiters.next, w, link);
		wl_list_remove(&w->link);
		w->pending = NULL;
		if (!w->cancelled && w->resolve)
			w->resolve(w->data, decision == PERM_ALLOW);
		free(w);
	}
}

static int pending_timeout(void *data) {
	pending_resolve(data, PERM_DENY, "timeout");
	return 0;
}

static void pending_client_destroyed(struct wl_listener *listener, void *data) {
	(void)data;
	perm_pending_t *p = wl_container_of(listener, p, client_destroy);
	pending_resolve(p, PERM_DENY, "client-gone");
}

static perm_pending_t *pending_create(struct wl_client *client, const perm_subject_t *subject,
		perm_cap_t cap) {
	perm_pending_t *p = calloc(1, sizeof(*p));
	if (!p)
		return NULL;

	p->client = client;
	p->subject = *subject;
	p->cap = cap;
	wl_list_init(&p->waiters);
	pending_make_token(p->token);
	p->deadline_ms = now_ms() + ask_timeout_ms;

	// the pending request owns this listener, so clearing the answer cache
	// cannot leave a prompt pointing at a freed client
	p->client_destroy.notify = pending_client_destroyed;
	wl_client_add_destroy_listener(client, &p->client_destroy);

	if (server.wl_display && ask_timeout_ms > 0) {
		p->timer = wl_event_loop_add_timer(wl_display_get_event_loop(server.wl_display), pending_timeout,
			p);
		if (p->timer)
			wl_event_source_timer_update(p->timer, (int)ask_timeout_ms);
	}

	wl_list_insert(&pending, &p->link);
	pending_publish_request(p);
	wlr_log(WLR_INFO, "permission: asking for %s from %s (token %s)", permission_cap_name(cap),
		subject_name(subject), p->token);

	return p;
}

perm_waiter_t *permission_park(struct wl_client *client, perm_cap_t cap, perm_resolve_fn resolve,
		void *data) {
	if (!client)
		return NULL;

	if (server.locked) {
		wlr_log(WLR_INFO, "permission: ask for %s denied, the session is locked",
			permission_cap_name(cap));
		return NULL;
	}

	perm_client_t *c = client_entry(client);
	if (!c || !c->subject.known) {
		wlr_log(WLR_INFO, "permission: ask for %s denied, the executable is unknown",
			permission_cap_name(cap));
		return NULL;
	}

	perm_pending_t *p = pending_find(client, cap);
	if (!p) {
		p = pending_create(client, &c->subject, cap);
		if (!p)
			return NULL;
	}

	perm_waiter_t *w = calloc(1, sizeof(*w));
	if (!w)
		return NULL;

	w->resolve = resolve;
	w->data = data;
	w->pending = p;
	wl_list_insert(&p->waiters, &w->link);
	return w;
}

void permission_unpark(perm_waiter_t *waiter) {
	if (!waiter)
		return;

	// detached by pending_resolve: it is still alive until the loop frees it,
	// but its callback must not run now that the frame is gone
	if (!waiter->pending) {
		waiter->cancelled = true;
		return;
	}

	wl_list_remove(&waiter->link);
	waiter->pending = NULL;
	free(waiter);
}

bool permission_prompt(struct wl_client *client, perm_cap_t cap) {
	if (!client)
		return false;

	if (server.locked) {
		wlr_log(WLR_INFO, "permission: ask for %s denied, the session is locked",
			permission_cap_name(cap));
		return false;
	}

	perm_client_t *c = client_entry(client);
	if (!c || !c->subject.known)
		return false;
	if (c->has[cap])
		return false;
	if (pending_find(client, cap))
		return true;

	return pending_create(client, &c->subject, cap) != NULL;
}

bool permission_answer_token(const char *token, perm_decision_t decision,
		perm_remember_t remember) {
	if (!token || (decision != PERM_ALLOW && decision != PERM_DENY))
		return false;

	perm_pending_t *p = NULL;
	perm_pending_t *it;
	wl_list_for_each(it, &pending, link) {
		if (streq(it->token, token)) {
			p = it;
			break;
		}
	}
	if (!p)
		return false;

	struct wl_client *client = p->client;
	perm_subject_t subject = p->subject;
	perm_cap_t cap = p->cap;

	permission_answer(client, &subject, cap, decision, remember);
	pending_resolve(p, decision, "answer");
	return true;
}

size_t permission_pending_count(void) {
	size_t n = 0;
	perm_pending_t *p;
	wl_list_for_each(p, &pending, link)
		n++;
	return n;
}

bool permission_pending_at(size_t index, perm_pending_info_t *out) {
	perm_pending_t *p;
	size_t i = 0;
	wl_list_for_each(p, &pending, link) {
		if (i++ != index)
			continue;

		int64_t left = p->deadline_ms - now_ms();
		out->token = p->token;
		out->subject = &p->subject;
		out->cap = p->cap;
		out->seconds_left = left <= 0 ? 0 : (int)((left + 999) / 1000);
		return true;
	}
	return false;
}

void permission_log_denial(struct wl_client *client, perm_cap_t cap, const perm_result_t *result) {
	perm_subject_t s = {0};
	permission_subject(client, &s);

	char where[96] = "";
	if (result) {
		if (result->source)
			snprintf(where, sizeof(where), " (%s", result->source);
		else
			snprintf(where, sizeof(where), " (");
		if (result->rule_index >= 0)
			snprintf(where + strlen(where), sizeof(where) - strlen(where), " rule %d", result->rule_index);
		str_append(where, sizeof(where), ")");
	}

	wlr_log(WLR_INFO, "permission: denied %s for %s%s%s%s", permission_cap_name(cap), subject_name(&s),
		s.app_id[0] ? " app_id=" : "", s.app_id, where);
}

void permission_set_ask_timeout(uint32_t ms) {
	ask_timeout_ms = ms;
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
			if (!permission_remember_add(subject->exe, cap, decision, 0, remember == PERM_REMEMBER_ALWAYS))
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
		snprintf(err, errsz, "too many permission rules, the limit is %d", MAX_PERMISSION_RULES);
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

static bool remember_put(const char *exe, perm_cap_t cap, perm_decision_t decision, int64_t expires,
		bool persistent) {
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
		if (!permission_cap_parse(fields[1], &cap) || !permission_mode_parse(fields[2], &decision)) {
			wlr_log(WLR_ERROR, "permission: %s:%d: unknown capability or mode", state_path, lineno);
			continue;
		}

		if (decision == PERM_ASK) {
			wlr_log(WLR_ERROR, "permission: %s:%d: ask is not a remembered answer", state_path, lineno);
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
	ask_timeout_ms = PERM_ASK_TIMEOUT_MS;
	wl_list_init(&clients);
	wl_list_init(&pending);

	state_paths();
	remember_load();

	if (state_path[0] && remember_len > 0)
		wlr_log(WLR_INFO, "Permissions: loaded %zu remembered answers from %s", remember_len, state_path);
}

void permission_fini(void) {
	perm_pending_t *p, *tmp;
	wl_list_for_each_safe(p, tmp, &pending, link)
		pending_resolve(p, PERM_DENY, "shutdown");

	for (size_t i = 0; i < rule_len; i++)
		rule_pattern_clear(&rules[i].exe);
	rule_len = 0;

	permission_cache_clear();
	remember_len = 0;

	state_path[0] = '\0';
	state_dir[0] = '\0';
}
