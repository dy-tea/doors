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
	PERM_REMEMBER_SESSION, // until doors exits
	PERM_REMEMBER_ALWAYS, // written to the state file
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
