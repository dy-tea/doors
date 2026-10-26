#pragma once

#include "rule.h"
#include "types.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <wayland-server-core.h>

#define MAX_PERMISSION_RULES 64
#define MAX_PERMISSION_REMEMBER 64
#define PERM_TOKEN_LEN 8
#define PERM_ASK_TIMEOUT_MS 15000

typedef enum {
	PERM_CAPTURE = 0,
	PERM_OUTPUT,
	PERM_INJECT,
	PERM_CAP_COUNT,
} perm_cap_t;

typedef enum {
	PERM_DENY = 0,
	PERM_ALLOW,
	PERM_ASK,
} perm_decision_t;

typedef enum {
	PERM_REMEMBER_NONE = 0,
	PERM_REMEMBER_SESSION,
	PERM_REMEMBER_ALWAYS,
} perm_remember_t;

typedef struct {
	pid_t pid;
	char exe[PATH_MAX]; // resolved from /proc/<pid>/exe, empty when unreadable
	char app_id[SMALEN]; // security-context app_id, when the client has one
	bool sandboxed;
	bool known; // exe was resolved
} perm_subject_t;

typedef struct {
	perm_decision_t decision;
	const char *source; // "policy", "answer", "remember", "default"
	int rule_index; // index of the matching policy rule, -1 otherwise
} perm_result_t;

typedef struct {
	char spec[RULE_RE_MAX + 16]; // pattern, ready to print after "exe:"
	perm_cap_t cap;
	perm_decision_t decision;
} perm_rule_info_t;

typedef struct {
	const char *exe;
	perm_cap_t cap;
	perm_decision_t decision;
	int64_t expires; // epoch seconds, 0 when it never expires
	bool persistent; // false for --remember=session
	int shadow_rule; // index of a policy rule that outranks it, -1 otherwise
} perm_remember_info_t;

typedef struct perm_waiter perm_waiter_t;

// called exactly once when a parked request is answered, times out, the client
// disconnects or the compositor shuts down. data is the caller's.
typedef void (*perm_resolve_fn)(void *data, bool allow);

typedef struct {
	const char *token;
	const perm_subject_t *subject;
	perm_cap_t cap;
	int seconds_left;
} perm_pending_info_t;

void permission_init(void);
void permission_fini(void);

const char *permission_cap_name(perm_cap_t cap);
const char *permission_mode_name(perm_decision_t decision);
bool permission_cap_parse(const char *name, perm_cap_t *out);
bool permission_mode_parse(const char *name, perm_decision_t *out);

// resolve the peer behind a clientm false only when client is NULL
bool permission_subject(struct wl_client *client, perm_subject_t *out);

// policy, then remembered answer, then default
perm_result_t permission_decide(const perm_subject_t *subject, perm_cap_t cap);

// resolution for a live client
perm_result_t permission_check(struct wl_client *client, perm_cap_t cap);

// record answer to parked prompt and optionally remember it
void permission_answer(struct wl_client *client, const perm_subject_t *subject, perm_cap_t cap,
	perm_decision_t decision, perm_remember_t remember);

// hold an operation until the user answers
perm_waiter_t *permission_park(struct wl_client *client, perm_cap_t cap, perm_resolve_fn resolve,
	void *data);

// cancel a parked operation, safe from inside the resolve callback
void permission_unpark(perm_waiter_t *waiter);

// show prompt
bool permission_prompt(struct wl_client *client, perm_cap_t cap);

// answer by token, returns false when no request has that token
bool permission_answer_token(const char *token, perm_decision_t decision, perm_remember_t remember);

size_t permission_pending_count(void);
bool permission_pending_at(size_t index, perm_pending_info_t *out);

// log denial at INFO with the subject and the rule that matched
void permission_log_denial(struct wl_client *client, perm_cap_t cap, const perm_result_t *result);

// timeout for unanswered asks, in milliseconds. 0 disables the timer.
void permission_set_ask_timeout(uint32_t ms);

bool permission_subject_valid(const char *spec, char *err, size_t errsz);

// return new index, or -1 with err filled in
int permission_rule_add(const char *exe_spec, perm_cap_t cap, perm_decision_t decision, char *err,
	size_t errsz);
bool permission_rule_remove(int index);
size_t permission_rule_count(void);
bool permission_rule_at(size_t index, perm_rule_info_t *out);

perm_decision_t permission_default(perm_cap_t cap);
void permission_set_default(perm_cap_t cap, perm_decision_t decision);

size_t permission_remember_count(void);
bool permission_remember_at(size_t index, perm_remember_info_t *out);
bool permission_remember_add(const char *exe, perm_cap_t cap, perm_decision_t decision,
	int64_t expires, bool persistent);

int permission_remember_forget(const char *spec, char *err, size_t errsz);
size_t permission_remember_matching(const char *spec, char *err, size_t errsz);
const char *permission_state_path(void);

void permission_cache_clear(void);
size_t permission_cache_count(void);
