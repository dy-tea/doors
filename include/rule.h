#pragma once

#include "types.h"

#include <stdbool.h>
#include <stddef.h>
#include <wayland-server-core.h>

#define MAX_RULES 128

typedef enum {
	RULE_TYPE_NONE = 0,
	RULE_TYPE_DESKTOP = 1 << 0,
	RULE_TYPE_MONITOR = 1 << 1,
	RULE_TYPE_STATE = 1 << 2,
	RULE_TYPE_FOLLOW = 1 << 3,
	RULE_TYPE_FOCUS = 1 << 4,
	RULE_TYPE_MANAGE = 1 << 5,
	RULE_TYPE_LOCKED = 1 << 6,
	RULE_TYPE_HIDDEN = 1 << 7,
	RULE_TYPE_STICKY = 1 << 8,
	RULE_TYPE_SCROLLER_PROPORTION = 1 << 9,
	RULE_TYPE_SCROLLER_PROPORTION_SINGLE = 1 << 10,
	RULE_TYPE_BLUR = 1 << 11,
	RULE_TYPE_MICA = 1 << 12,
	RULE_TYPE_ACRYLIC = 1 << 13,
	RULE_TYPE_BORDER_RADIUS = 1 << 14,
	RULE_TYPE_SHADOW = 1 << 15,
	RULE_TYPE_BLOCK_OUT_FROM_SCREENSHARE = 1 << 16,
	RULE_TYPE_ALLOW_TEARING = 1 << 17,
	RULE_TYPE_SHORTCUTS_INHIBITOR = 1 << 18,
	RULE_TYPE_RENDER_UNFOCUSED_FPS = 1 << 19,
	RULE_TYPE_OPACITY = 1 << 20,
	RULE_TYPE_ANIM_DISABLE = 1 << 21,
	RULE_TYPE_MAXIMIZED = 1 << 22,
	RULE_TYPE_MINIMIZED = 1 << 23,
	RULE_TYPE_LAST = 1 << 24,
} rule_type_t;

typedef enum {
	RULE_MATCH_EXACT,
	RULE_MATCH_GLOB,
	RULE_MATCH_REGEX,
} rule_match_mode_t;

#define RULE_RE_MAX 512 // PCRE2 pattern length limit

typedef struct rule_pattern {
	char pattern[RULE_RE_MAX]; // as written by the user
	uint8_t mode; // rule_match_mode_t
	bool caseless;
	void *code; // compiled pcre2_code, NULL unless mode is GLOB or REGEX
} rule_pattern_t;

typedef struct {
	rule_pattern_t app_id;
	rule_pattern_t title;
	rule_pattern_t tag;
	bool one_shot;
} rule_match_t;

typedef struct {
	uint32_t has; // what fields are set in this rule
	uint32_t flags; // value of boolean fields

	// non-boolean fields
	char desktop[SMALEN];
	char monitor[SMALEN];
	client_state_t state;
	float scroller_proportion;
	float scroller_proportion_single;
	float border_radius;
	float opacity;
	int render_unfocused_fps;
} rule_consequence_t;

static inline bool rule_flag(const rule_consequence_t *rule, rule_type_t flag) {
	return (rule->flags & flag) != 0;
}

bool rule_pattern_set(rule_pattern_t *p, const char *spec, char *errbuf, size_t errsz);
bool rule_pattern_match(const rule_pattern_t *p, const char *value);
void rule_pattern_clear(rule_pattern_t *p);
size_t rule_count(void);

typedef struct rule_t {
	rule_match_t match;
	rule_consequence_t consequence;
	struct wl_list link;
} rule_t;

extern struct wl_list rule_list;

void rule_init(void);
void rule_fini(void);
rule_t *make_rule(void);
void add_rule(rule_t *r);
void remove_rule(rule_t *r);
void free_rule(rule_t *r);
bool remove_rule_by_index(int idx);
void list_rules(char *buf, size_t buf_size);
rule_consequence_t *find_matching_rule(const char *app_id, const char *title, const char *tag);
void rule_apply_consequence(node_t *node, client_t *client, const rule_consequence_t *rule);
void rule_apply_view_consequence(view_t *view, const rule_consequence_t *rule);
