#pragma once

#include "types.h"

#include <stdbool.h>
#include <stddef.h>

#define MAX_RULES 128

typedef struct {
	char app_id[MAXLEN];
	char title[MAXLEN];
	char tag[MAXLEN];
	bool one_shot;
} rule_match_t;

typedef struct {
	char desktop[SMALEN];
	char monitor[SMALEN];
	client_state_t state;
	bool has_state;
	bool has_desktop;
	bool has_monitor;
	bool follow;
	bool has_follow;
	bool focus;
	bool has_focus;
	bool manage;
	bool has_manage;
	bool locked;
	bool has_locked;
	bool hidden;
	bool has_hidden;
	bool sticky;
	bool has_sticky;
	float scroller_proportion;
	bool has_scroller_proportion;
	float scroller_proportion_single;
	bool has_scroller_proportion_single;
	bool blur;
	bool has_blur;
	bool mica;
	bool has_mica;
	bool acrylic;
	bool has_acrylic;
	float border_radius;
	bool has_border_radius;
	bool shadow;
	bool has_shadow;
	bool block_out_from_screenshare;
	bool has_block_out_from_screenshare;
	bool allow_tearing;
	bool has_allow_tearing;
	bool shortcuts_inhibitor;
	bool has_shortcuts_inhibitor;
	bool render_unfocused;
	bool has_render_unfocused;
} rule_consequence_t;

typedef struct rule_t {
	rule_match_t match;
	rule_consequence_t consequence;
	struct rule_t *next;
} rule_t;

extern rule_t *rule_head;
extern rule_t *rule_tail;

void rule_init(void);
void rule_fini(void);
rule_t *make_rule(void);
void add_rule(rule_t *r);
void remove_rule(rule_t *r);
bool remove_rule_by_index(int idx);
void list_rules(char *buf, size_t buf_size);
rule_consequence_t *find_matching_rule(const char *app_id, const char *title, const char *tag);
void rule_apply_consequence(node_t *node, client_t *client, const rule_consequence_t *rule);
