#pragma once

#include "rule.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_layer_shell_v1.h>

#define MAX_LAYER_RULES 64

typedef enum {
	LAYER_RULE_NONE = 0,
	LAYER_RULE_BLUR = 1 << 0,
	LAYER_RULE_OPACITY = 1 << 1,
	LAYER_RULE_ANIMATIONS_DISABLE = 1 << 2,
	LAYER_RULE_KEYBOARD_INTERACTIVITY = 1 << 3,
	LAYER_RULE_LAYER = 1 << 4,
	LAYER_RULE_BLOCK_OUT_FROM_SCREENSHARE = 1 << 5,
} layer_rule_type_t;

typedef struct {
	uint32_t has; // what fields are set in this rule
	uint32_t flags; // value of boolean fields

	// non-boolean fields
	float opacity;
	enum zwlr_layer_shell_v1_layer layer;
	enum zwlr_layer_surface_v1_keyboard_interactivity keyboard_interactivity;
} layer_rule_consequence_t;

static inline bool layer_rule_flag(const layer_rule_consequence_t *rule, layer_rule_type_t flag) {
	return (rule->flags & flag) != 0;
}

typedef struct {
	rule_pattern_t namespace;
	rule_pattern_t output; // optional
} layer_rule_match_t;

typedef struct layer_rule_t {
	layer_rule_match_t match;
	layer_rule_consequence_t consequence;
	struct wl_list link;
} layer_rule_t;

extern struct wl_list layer_rule_list;

const char *layer_rule_layer_name(enum zwlr_layer_shell_v1_layer layer);

void layer_rule_init(void);
void layer_rule_fini(void);
layer_rule_t *make_layer_rule(void);
void add_layer_rule(layer_rule_t *r);
void remove_layer_rule(layer_rule_t *r);
void free_layer_rule(layer_rule_t *r);
bool remove_layer_rule_by_index(int idx);
size_t layer_rule_count(void);
void list_layer_rules(char *buf, size_t buf_size);
layer_rule_consequence_t *find_matching_layer_rule(const char *namespace, const char *output_name);
