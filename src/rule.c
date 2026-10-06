#include "ipc/ipc.h"
#include "layout/scroller.h"
#include "once.h"
#include "rule.h"
#include "settings.h"
#include "surface.h"
#include "tree.h"
#include "types.h"
#include "view.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct wl_list rule_list;

rule_t *make_rule(void) {
	rule_t *r = calloc(1, sizeof(rule_t));
	if (r) {
		r->match.app_id[0] = '\0';
		r->match.title[0] = '\0';
		r->match.tag[0] = '\0';
		r->match.one_shot = false;
		wl_list_init(&r->link);
	}
	return r;
}

void add_rule(rule_t *r) {
	wl_list_insert(rule_list.prev, &r->link);
}

void remove_rule(rule_t *r) {
	if (r == NULL)
		return;

	wl_list_remove(&r->link);
	free(r);
}

bool remove_rule_by_index(int idx) {
	int i = 0;
	rule_t *r;
	wl_list_for_each(r, &rule_list, link) {
		if (i == idx) {
			wl_list_remove(&r->link);
			free(r);
			return true;
		}
		i++;
	}
	return false;
}

void list_rules(char *buf, size_t buf_size) {
	size_t offset = 0;
	int idx = 0;

	rule_t *r;
	wl_list_for_each(r, &rule_list, link) {
		offset = ipc_buf_append(buf, buf_size, offset, "%d: ", idx);

		if (r->match.app_id[0] != '\0')
			offset = ipc_buf_append(buf, buf_size, offset, "app_id=%s ", r->match.app_id);
		if (r->match.title[0] != '\0')
			offset = ipc_buf_append(buf, buf_size, offset, "title=%s ", r->match.title);
		if (r->match.tag[0] != '\0')
			offset = ipc_buf_append(buf, buf_size, offset, "tag=%s ", r->match.tag);

		if (r->match.one_shot)
			offset = ipc_buf_append(buf, buf_size, offset, "one_shot ");

		offset = ipc_buf_append(buf, buf_size, offset, "-> ");

		if (r->consequence.has & RULE_TYPE_DESKTOP)
			offset = ipc_buf_append(buf, buf_size, offset, "desktop=%s ", r->consequence.desktop);
		if (r->consequence.has & RULE_TYPE_MONITOR)
			offset = ipc_buf_append(buf, buf_size, offset, "monitor=%s ", r->consequence.monitor);
		if (r->consequence.has & RULE_TYPE_STATE) {
			const char *state_str = "unknown";
			switch (r->consequence.state) {
			case STATE_TILED:
				state_str = "tiled";
				break;
			case STATE_FLOATING:
				state_str = "floating";
				break;
			case STATE_FULLSCREEN:
				state_str = "fullscreen";
				break;
			case STATE_PSEUDO_TILED:
				state_str = "pseudo_tiled";
				break;
			}
			offset = ipc_buf_append(buf, buf_size, offset, "state=%s ", state_str);
		}
		if (r->consequence.has & RULE_TYPE_FOLLOW)
			offset = ipc_buf_append(buf, buf_size, offset, "follow=%s ",
				r->consequence.flags & RULE_TYPE_FOLLOW ? "on" : "off");
		if (r->consequence.has & RULE_TYPE_FOCUS)
			offset = ipc_buf_append(buf, buf_size, offset, "focus=%s ",
				r->consequence.flags & RULE_TYPE_FOCUS ? "on" : "off");
		if (r->consequence.has & RULE_TYPE_MANAGE)
			offset = ipc_buf_append(buf, buf_size, offset, "manage=%s ",
				r->consequence.flags & RULE_TYPE_MANAGE ? "on" : "off");
		if (r->consequence.has & RULE_TYPE_LOCKED)
			offset = ipc_buf_append(buf, buf_size, offset, "locked=%s ",
				r->consequence.flags & RULE_TYPE_LOCKED ? "on" : "off");
		if (r->consequence.has & RULE_TYPE_HIDDEN)
			offset = ipc_buf_append(buf, buf_size, offset, "hidden=%s ",
				r->consequence.flags & RULE_TYPE_HIDDEN ? "on" : "off");
		if (r->consequence.has & RULE_TYPE_MAXIMIZED)
			offset = ipc_buf_append(buf, buf_size, offset, "maximized=%s ",
				r->consequence.flags & RULE_TYPE_MAXIMIZED ? "on" : "off");
		if (r->consequence.has & RULE_TYPE_MINIMIZED)
			offset = ipc_buf_append(buf, buf_size, offset, "minimized=%s ",
				r->consequence.flags & RULE_TYPE_MINIMIZED ? "on" : "off");
		if (r->consequence.has & RULE_TYPE_STICKY)
			offset = ipc_buf_append(buf, buf_size, offset, "sticky=%s ",
				r->consequence.flags & RULE_TYPE_STICKY ? "on" : "off");
		if (r->consequence.has & RULE_TYPE_SCROLLER_PROPORTION)
			offset = ipc_buf_append(buf, buf_size, offset, "scroller_proportion=%.2f ",
				r->consequence.scroller_proportion);
		if (r->consequence.has & RULE_TYPE_SCROLLER_PROPORTION_SINGLE)
			offset = ipc_buf_append(buf, buf_size, offset, "scroller_proportion_single=%.2f ",
				r->consequence.scroller_proportion_single);
		if (r->consequence.has & RULE_TYPE_BLUR)
			offset = ipc_buf_append(buf, buf_size, offset, "blur=%s ",
				r->consequence.flags & RULE_TYPE_BLUR ? "on" : "off");
		if (r->consequence.has & RULE_TYPE_MICA)
			offset = ipc_buf_append(buf, buf_size, offset, "mica=%s ",
				r->consequence.flags & RULE_TYPE_MICA ? "on" : "off");
		if (r->consequence.has & RULE_TYPE_ACRYLIC)
			offset = ipc_buf_append(buf, buf_size, offset, "acrylic=%s ",
				r->consequence.flags & RULE_TYPE_ACRYLIC ? "on" : "off");
		if (r->consequence.has & RULE_TYPE_BORDER_RADIUS)
			offset = ipc_buf_append(buf, buf_size, offset, "border_radius=%.1f ",
				r->consequence.border_radius);
		if (r->consequence.has & RULE_TYPE_SHADOW)
			offset = ipc_buf_append(buf, buf_size, offset, "shadow=%s ",
				r->consequence.flags & RULE_TYPE_SHADOW ? "on" : "off");
		if (r->consequence.has & RULE_TYPE_ANIM_DISABLE)
			offset = ipc_buf_append(buf, buf_size, offset, "animations_disable=%s ",
				r->consequence.flags & RULE_TYPE_ANIM_DISABLE ? "on" : "off");
		if (r->consequence.has & RULE_TYPE_BLOCK_OUT_FROM_SCREENSHARE)
			offset = ipc_buf_append(buf, buf_size, offset, "block_out_from_screenshare=%s ",
				r->consequence.flags & RULE_TYPE_BLOCK_OUT_FROM_SCREENSHARE ? "on" : "off");
		if (r->consequence.has & RULE_TYPE_ALLOW_TEARING)
			offset = ipc_buf_append(buf, buf_size, offset, "allow_tearing=%s ",
				r->consequence.flags & RULE_TYPE_ALLOW_TEARING ? "on" : "off");
		if (r->consequence.has & RULE_TYPE_SHORTCUTS_INHIBITOR)
			offset = ipc_buf_append(buf, buf_size, offset, "shortcuts_inhibitor=%s ",
				r->consequence.flags & RULE_TYPE_SHORTCUTS_INHIBITOR ? "on" : "off");
		if (r->consequence.has & RULE_TYPE_RENDER_UNFOCUSED_FPS)
			offset = ipc_buf_append(buf, buf_size, offset, "render_unfocused_fps=%d ",
				r->consequence.render_unfocused_fps);
		if (r->consequence.has & RULE_TYPE_OPACITY)
			offset = ipc_buf_append(buf, buf_size, offset, "opacity=%.1f ", r->consequence.opacity);

		offset = ipc_buf_append(buf, buf_size, offset, "\n");

		idx++;
	}

	if (idx == 0) {
		snprintf(buf, buf_size, "No rules defined\n");
	}
}

static bool match_string(const char *pattern, const char *value) {
	if (pattern == NULL || pattern[0] == '\0')
		return true;
	if (value == NULL || value[0] == '\0')
		return false;
	return strcmp(pattern, value) == 0;
}

rule_consequence_t *find_matching_rule(const char *app_id, const char *title, const char *tag) {
	static rule_consequence_t merged;
	memset(&merged, 0, sizeof(merged));

	rule_t *r, *tmp;
	wl_list_for_each_safe(r, tmp, &rule_list, link) {
		bool app_id_matches = match_string(r->match.app_id, app_id);
		bool title_matches = match_string(r->match.title, title);
		bool tag_matches = match_string(r->match.tag, tag);

		if (app_id_matches && title_matches && tag_matches) {
			rule_type_t bits = r->consequence.has;
			merged.has |= bits;
			merged.flags = (merged.flags & ~bits) | (r->consequence.flags & bits);

			if (bits & RULE_TYPE_DESKTOP)
				strncpy(merged.desktop, r->consequence.desktop, SMALEN);
			if (bits & RULE_TYPE_MONITOR)
				strncpy(merged.monitor, r->consequence.monitor, SMALEN);
			if (bits & RULE_TYPE_STATE)
				merged.state = r->consequence.state;
			if (bits & RULE_TYPE_SCROLLER_PROPORTION)
				merged.scroller_proportion = r->consequence.scroller_proportion;
			if (bits & RULE_TYPE_SCROLLER_PROPORTION_SINGLE)
				merged.scroller_proportion_single = r->consequence.scroller_proportion_single;
			if (bits & RULE_TYPE_BORDER_RADIUS)
				merged.border_radius = r->consequence.border_radius;
			if (bits & RULE_TYPE_OPACITY)
				merged.opacity = r->consequence.opacity;
			if (bits & RULE_TYPE_RENDER_UNFOCUSED_FPS)
				merged.render_unfocused_fps = r->consequence.render_unfocused_fps;

			if (r->match.one_shot)
				remove_rule(r);
		}
	}

	return merged.has ? &merged : NULL;
}

void rule_apply_consequence(node_t *node, client_t *client, const rule_consequence_t *rule) {
	if (!rule)
		return;

	if (rule->has & RULE_TYPE_STATE)
		client->state = rule->state;

	if (rule->has & RULE_TYPE_HIDDEN)
		node_set_hidden(node, rule_flag(rule, RULE_TYPE_HIDDEN));
	if (rule->has & RULE_TYPE_MAXIMIZED)
		client->flags.maximized = rule_flag(rule, RULE_TYPE_MAXIMIZED);
	if (rule->has & RULE_TYPE_MINIMIZED)
		client->flags.minimized = rule_flag(rule, RULE_TYPE_MINIMIZED);
	if (rule->has & RULE_TYPE_STICKY)
		node->sticky = rule_flag(rule, RULE_TYPE_STICKY);
	if (rule->has & RULE_TYPE_LOCKED)
		node->locked = rule_flag(rule, RULE_TYPE_LOCKED);

	if (rule->has & RULE_TYPE_SCROLLER_PROPORTION || rule->has & RULE_TYPE_SCROLLER_PROPORTION_SINGLE)
		scroller_apply_client_rules(client,
			rule->has & RULE_TYPE_SCROLLER_PROPORTION ? rule->scroller_proportion : 0.0f,
			rule->has & RULE_TYPE_SCROLLER_PROPORTION_SINGLE ? rule->scroller_proportion_single : 0.0f);

	if (rule->has & RULE_TYPE_BLOCK_OUT_FROM_SCREENSHARE)
		client->flags.block_out_from_screenshare = rule_flag(rule, RULE_TYPE_BLOCK_OUT_FROM_SCREENSHARE);

	if (rule->has & RULE_TYPE_ALLOW_TEARING) {
		client->flags.allow_tearing = rule_flag(rule, RULE_TYPE_ALLOW_TEARING);
		client->flags.allow_tearing_from_rule = true;
	}

	if (rule->has & RULE_TYPE_RENDER_UNFOCUSED_FPS)
		client->render_unfocused_fps = rule->render_unfocused_fps;

	if (rule->has & RULE_TYPE_BLUR) {
		client->flags.blur = rule_flag(rule, RULE_TYPE_BLUR);
		client->flags.blur_from_rule = true;
	}

	if (rule->has & RULE_TYPE_MICA)
		client->flags.mica = rule_flag(rule, RULE_TYPE_MICA);

	if (rule->has & RULE_TYPE_ACRYLIC)
		client->flags.acrylic = rule_flag(rule, RULE_TYPE_ACRYLIC);

	if (rule->has & RULE_TYPE_BORDER_RADIUS)
		client->border_radius = rule->border_radius;

	if (rule->has & RULE_TYPE_OPACITY)
		client->opacity = rule->opacity;

	if (rule->has & RULE_TYPE_ANIM_DISABLE)
		client->flags.anim_disabled = rule_flag(rule, RULE_TYPE_ANIM_DISABLE);

	if (rule->has & RULE_TYPE_SHADOW) {
		client->flags.shadow = rule_flag(rule, RULE_TYPE_SHADOW);
		client->shadow_size = settings.shadow_size;
		client->shadow_offset_x = settings.shadow_offset_x;
		client->shadow_offset_y = settings.shadow_offset_y;
		memcpy(client->shadow_color, settings.shadow_color, sizeof(settings.shadow_color));
	}
}

// build the shader/region nodes a rule asks for, on top of the flags
// rule_apply_consequence already set on the client
void rule_apply_view_consequence(view_t *view, const rule_consequence_t *rule) {
	if (view == NULL || rule == NULL || view->client == NULL)
		return;

	if (rule->has & RULE_TYPE_BLUR)
		surface_client_set_effect(view->client, EFFECT_BLUR, rule_flag(rule, RULE_TYPE_BLUR));
	if (rule->has & RULE_TYPE_MICA)
		surface_client_set_effect(view->client, EFFECT_MICA, rule_flag(rule, RULE_TYPE_MICA));
	if (rule->has & RULE_TYPE_ACRYLIC)
		surface_client_set_effect(view->client, EFFECT_ACRYLIC, rule_flag(rule, RULE_TYPE_ACRYLIC));
	if (rule->has & RULE_TYPE_BORDER_RADIUS)
		surface_client_set_border_radius(view->client, rule->border_radius);
	if (rule->has & RULE_TYPE_SHADOW)
		surface_client_set_shadow(view->client, rule_flag(rule, RULE_TYPE_SHADOW));
	if (rule->has & RULE_TYPE_OPACITY)
		surface_set_opacity(&view->scene_tree->node, rule->opacity);
}

void rule_init(void) {
	ONCE();
	wl_list_init(&rule_list);
}

void rule_fini(void) {
	ONCE();
	rule_t *r, *tmp;
	wl_list_for_each_safe(r, tmp, &rule_list, link) {
		wl_list_remove(&r->link);
		free(r);
	}
	wl_list_init(&rule_list);
}
