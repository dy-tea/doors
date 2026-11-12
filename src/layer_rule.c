#include "ipc/ipc.h"
#include "layer_rule.h"
#include "macros.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct wl_list layer_rule_list;

const char *layer_rule_layer_name(const enum zwlr_layer_shell_v1_layer layer) {
	const char *layer_names[] = {
		[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND] = "background",
		[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM] = "bottom",
		[ZWLR_LAYER_SHELL_V1_LAYER_TOP] = "top",
		[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY] = "overlay"
	};
	return layer_names[layer];
}

static const char *layer_rule_keyboard_name(const enum zwlr_layer_surface_v1_keyboard_interactivity
		interactivity) {
	const char *interactivity_names[] = {
		[ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE] = "none",
		[ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE] = "exclusive",
		[ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND] = "on_demand"
	};
	return interactivity_names[interactivity];
}

layer_rule_t *make_layer_rule(void) {
	layer_rule_t *r = calloc(1, sizeof(*r));
	if (r) {
		r->match.namespace.mode = RULE_MATCH_EXACT;
		r->match.output.mode = RULE_MATCH_EXACT;
		wl_list_init(&r->link);
	}
	return r;
}

void add_layer_rule(layer_rule_t *r) {
	wl_list_insert(layer_rule_list.prev, &r->link);
}

size_t layer_rule_count(void) {
	size_t n = 0;
	layer_rule_t *r;
	wl_list_for_each(r, &layer_rule_list, link)
		n++;
	return n;
}

void free_layer_rule(layer_rule_t *r) {
	if (r == NULL)
		return;

	rule_pattern_clear(&r->match.namespace);
	rule_pattern_clear(&r->match.output);
	free(r);
}

void remove_layer_rule(layer_rule_t *r) {
	if (r == NULL)
		return;

	wl_list_remove(&r->link);
	free_layer_rule(r);
}

bool remove_layer_rule_by_index(const int idx) {
	int i = 0;
	layer_rule_t *r, *tmp;
	wl_list_for_each_safe(r, tmp, &layer_rule_list, link) {
		if (i == idx) {
			remove_layer_rule(r);
			return true;
		}
		i++;
	}
	return false;
}

void list_layer_rules(char *buf, size_t buf_size) {
	size_t offset = 0;
	int idx = 0;

	layer_rule_t *r;
	wl_list_for_each(r, &layer_rule_list, link) {
		char spec[RULE_RE_MAX + 16];
		offset = ipc_buf_append(buf, buf_size, offset, "%d: ", idx);

		rule_pattern_spec(&r->match.namespace, spec, sizeof(spec));
		offset = ipc_buf_append(buf, buf_size, offset, "namespace=%s ", spec);

		if (r->match.output.pattern[0] != '\0') {
			rule_pattern_spec(&r->match.output, spec, sizeof(spec));
			offset = ipc_buf_append(buf, buf_size, offset, "output=%s ", spec);
		}

		offset = ipc_buf_append(buf, buf_size, offset, "-> ");

		if (r->consequence.has & LAYER_RULE_BLUR)
			offset = ipc_buf_append(buf, buf_size, offset, "blur=%s ", layer_rule_flag(&r->consequence,
				LAYER_RULE_BLUR) ? "on" : "off");
		if (r->consequence.has & LAYER_RULE_OPACITY)
			offset = ipc_buf_append(buf, buf_size, offset, "opacity=%.2f ", r->consequence.opacity);
		if (r->consequence.has & LAYER_RULE_ANIMATIONS_DISABLE)
			offset = ipc_buf_append(buf, buf_size, offset, "animations_disable=%s ",
				layer_rule_flag(&r->consequence, LAYER_RULE_ANIMATIONS_DISABLE) ? "on" : "off");
		if (r->consequence.has & LAYER_RULE_KEYBOARD_INTERACTIVITY)
			offset = ipc_buf_append(buf, buf_size, offset, "keyboard_interactivity=%s ",
				layer_rule_keyboard_name(r->consequence.keyboard_interactivity));
		if (r->consequence.has & LAYER_RULE_LAYER)
			offset = ipc_buf_append(buf, buf_size, offset, "layer=%s ",
				layer_rule_layer_name(r->consequence.layer));
		if (r->consequence.has & LAYER_RULE_BLOCK_OUT_FROM_SCREENSHARE)
			offset = ipc_buf_append(buf, buf_size, offset, "block_out_from_screenshare=%s ",
				layer_rule_flag(&r->consequence, LAYER_RULE_BLOCK_OUT_FROM_SCREENSHARE) ? "on" : "off");

		offset = ipc_buf_append(buf, buf_size, offset, "\n");
		idx++;
	}

	if (idx == 0)
		snprintf(buf, buf_size, "No layer rules defined\n");
}

layer_rule_consequence_t *find_matching_layer_rule(const char *namespace, const char *output_name) {
	static layer_rule_consequence_t merged;
	memset(&merged, 0, sizeof(merged));

	layer_rule_t *r;
	wl_list_for_each(r, &layer_rule_list, link) {
		if (!rule_pattern_match(&r->match.namespace, namespace))
			continue;
		if (!rule_pattern_match(&r->match.output, output_name))
			continue;

		const uint32_t bits = r->consequence.has;
		merged.has |= bits;
		merged.flags = (merged.flags & ~bits) | (r->consequence.flags & bits);

		if (bits & LAYER_RULE_OPACITY)
			merged.opacity = r->consequence.opacity;
		if (bits & LAYER_RULE_LAYER)
			merged.layer = r->consequence.layer;
		if (bits & LAYER_RULE_KEYBOARD_INTERACTIVITY)
			merged.keyboard_interactivity = r->consequence.keyboard_interactivity;
	}

	return merged.has ? &merged : NULL;
}

void layer_rule_init(void) {
	ONCE();
	wl_list_init(&layer_rule_list);
}

void layer_rule_fini(void) {
	ONCE();
	layer_rule_t *r, *tmp;
	wl_list_for_each_safe(r, tmp, &layer_rule_list, link)
		remove_layer_rule(r);

	wl_list_init(&layer_rule_list);
}
