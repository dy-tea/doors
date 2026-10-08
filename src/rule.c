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
#include <strings.h>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#define RULE_MATCH_LIMIT 100000
#define RULE_MATCH_DEPTH_LIMIT 10000

struct wl_list rule_list;

static const struct {
	const char *name;
	rule_match_mode_t mode;
} rule_mode_names[] = {
	{"exact", RULE_MATCH_EXACT},
	{"glob", RULE_MATCH_GLOB},
	{"regex", RULE_MATCH_REGEX},
};

rule_t *make_rule(void) {
	rule_t *r = calloc(1, sizeof(rule_t));
	if (r) {
		r->match.app_id.mode = RULE_MATCH_EXACT;
		r->match.title.mode = RULE_MATCH_EXACT;
		r->match.tag.mode = RULE_MATCH_EXACT;
		r->match.one_shot = false;
		wl_list_init(&r->link);
	}
	return r;
}

void add_rule(rule_t *r) {
	wl_list_insert(rule_list.prev, &r->link);
}

size_t rule_count(void) {
	size_t n = 0;
	rule_t *r;
	wl_list_for_each(r, &rule_list, link)
		n++;
	return n;
}

void free_rule(rule_t *r) {
	if (r == NULL)
		return;

	rule_pattern_clear(&r->match.app_id);
	rule_pattern_clear(&r->match.title);
	rule_pattern_clear(&r->match.tag);
	free(r);
}

void remove_rule(rule_t *r) {
	if (r == NULL)
		return;

	wl_list_remove(&r->link);
	free_rule(r);
}

bool remove_rule_by_index(int idx) {
	int i = 0;
	rule_t *r, *tmp;
	wl_list_for_each_safe(r, tmp, &rule_list, link) {
		if (i == idx) {
			remove_rule(r);
			return true;
		}
		i++;
	}
	return false;
}

// format the pattern back as a spec that rule -a accepts, so -l round-trips
static void rule_pattern_spec(const rule_pattern_t *p, char *out, size_t outsz) {
	const char *mode = "";
	if (p->mode == RULE_MATCH_GLOB)
		mode = "glob:";
	else if (p->mode == RULE_MATCH_REGEX)
		mode = "regex:";

	snprintf(out, outsz, "%s%s%s", mode, p->caseless ? "i:" : "", p->pattern);
}

void list_rules(char *buf, size_t buf_size) {
	size_t offset = 0;
	int idx = 0;

	rule_t *r;
	wl_list_for_each(r, &rule_list, link) {
		offset = ipc_buf_append(buf, buf_size, offset, "%d: ", idx);

		char spec[RULE_RE_MAX + 16];
		if (r->match.app_id.pattern[0] != '\0') {
			rule_pattern_spec(&r->match.app_id, spec, sizeof(spec));
			offset = ipc_buf_append(buf, buf_size, offset, "app_id=%s ", spec);
		}
		if (r->match.title.pattern[0] != '\0') {
			rule_pattern_spec(&r->match.title, spec, sizeof(spec));
			offset = ipc_buf_append(buf, buf_size, offset, "title=%s ", spec);
		}
		if (r->match.tag.pattern[0] != '\0') {
			rule_pattern_spec(&r->match.tag, spec, sizeof(spec));
			offset = ipc_buf_append(buf, buf_size, offset, "tag=%s ", spec);
		}

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

static bool glob_to_regex(const char *glob, char *out, size_t outsz) {
	size_t o = 0;

#define EMIT(...) \
	do { \
		int _n = snprintf(out + o, outsz - o, __VA_ARGS__); \
		if (_n < 0 || (size_t)_n >= outsz - o) \
			return false; \
		o += _n; \
	} while (0)

	EMIT("(?s)^");

	for (const char *p = glob; *p; p++) {
		switch (*p) {
		case '*':
			EMIT(".*");
			break;
		case '?':
			EMIT(".");
			break;
		case '[': {
			const char *body = p + 1;
			bool negate = false;
			if (*body == '!' || *body == '^') {
				negate = true;
				body++;
			}
			if (*body == ']')
				body++;
			const char *end = strchr(body, ']');
			if (!end)
				return false;

			EMIT("[");
			if (negate)
				EMIT("^");
			for (const char *r = body; r < end; r++) {
				if (*r == '\\')
					EMIT("\\\\");
				EMIT("%c", *r);
			}
			EMIT("]");
			p = end;
			break;
		}
		case '{': {
			const char *end = strchr(p, '}');
			if (!end)
				return false;

			EMIT("(?:");
			for (const char *r = p + 1; r < end; r++) {
				if (*r == '\\')
					EMIT("\\\\");
				EMIT("%c", *r == ',' ? '|' : *r);
			}
			EMIT(")");
			p = end;
			break;
		}
		default:
			if (strchr(".^$+()|\\{}", *p))
				EMIT("\\%c", *p);
			else
				EMIT("%c", *p);
			break;
		}
	}

	EMIT("$");

#undef EMIT

	return true;
}

static void pattern_error(char *errbuf, size_t errsz, const char *src, int code, size_t offset) {
	PCRE2_UCHAR msg[256];
	int rc = pcre2_get_error_message(code, msg, sizeof(msg));
	snprintf(errbuf, errsz, "invalid pattern at offset %zu: %s (%s)", offset,
		rc < 0 ? "unknown error" : (const char *)msg, src);
}

static bool pattern_compile(rule_pattern_t *p, char *errbuf, size_t errsz) {
	p->code = NULL;

	if (p->mode == RULE_MATCH_EXACT)
		return true;

	char src[RULE_RE_MAX * 2 + 32];
	if (p->mode == RULE_MATCH_GLOB) {
		if (!glob_to_regex(p->pattern, src, sizeof(src))) {
			snprintf(errbuf, errsz, "invalid glob: %s", p->pattern);
			return false;
		}
	} else {
		snprintf(src, sizeof(src), "%s", p->pattern);
	}

	int code = 0;
	PCRE2_SIZE offset = 0;
	uint32_t options = PCRE2_UTF;
	if (p->caseless)
		options |= PCRE2_CASELESS;

	pcre2_code *re = pcre2_compile((PCRE2_SPTR)src, PCRE2_ZERO_TERMINATED, options, &code, &offset,
		NULL);
	if (!re) {
		pattern_error(errbuf, errsz, p->pattern, code, offset);
		return false;
	}

	p->code = re;
	return true;
}

bool rule_pattern_set(rule_pattern_t *p, const char *spec, char *errbuf, size_t errsz) {
	rule_pattern_clear(p);
	p->mode = RULE_MATCH_EXACT;

	const char *pat = spec;
	bool have_mode = false;

	if (spec) {
		const char *colon = strchr(spec, ':');
		if (colon && colon != spec) {
			for (size_t i = 0; i < sizeof(rule_mode_names) / sizeof(rule_mode_names[0]); i++) {
				size_t len = strlen(rule_mode_names[i].name);
				if ((size_t)(colon - spec) != len || strncmp(spec, rule_mode_names[i].name, len))
					continue;

				p->mode = rule_mode_names[i].mode;
				pat = colon + 1;
				have_mode = true;
				break;
			}
		}
	}

	if (have_mode && pat[0] == 'i' && pat[1] == ':') {
		p->caseless = true;
		pat += 2;
	}

	if (strlen(pat) >= sizeof(p->pattern)) {
		snprintf(errbuf, errsz, "pattern is too long, the limit is %zu characters",
			sizeof(p->pattern) - 1);
		return false;
	}

	snprintf(p->pattern, sizeof(p->pattern), "%s", pat);

	if (!pattern_compile(p, errbuf, errsz)) {
		p->pattern[0] = '\0';
		p->mode = RULE_MATCH_EXACT;
		p->caseless = false;
		return false;
	}

	return true;
}

bool rule_pattern_match(const rule_pattern_t *p, const char *value) {
	if (p->pattern[0] == '\0')
		return true;
	if (value == NULL || value[0] == '\0')
		return false;

	if (p->mode == RULE_MATCH_EXACT)
		return p->caseless ? strcasecmp(p->pattern, value) == 0 : strcmp(p->pattern, value) == 0;

	pcre2_code *re = p->code;
	if (!re)
		return false;

	pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);
	if (!md)
		return false;

	static pcre2_match_context *mc;
	if (!mc) {
		mc = pcre2_match_context_create(NULL);
		if (mc) {
			pcre2_set_match_limit(mc, RULE_MATCH_LIMIT);
			pcre2_set_depth_limit(mc, RULE_MATCH_DEPTH_LIMIT);
		}
	}

	int rc = pcre2_match(re, (PCRE2_SPTR)value, strlen(value), 0, PCRE2_NO_UTF_CHECK, md, mc);
	pcre2_match_data_free(md);

	if (rc >= 0)
		return true;

	if (rc == PCRE2_ERROR_MATCHLIMIT || rc == PCRE2_ERROR_DEPTHLIMIT)
		wlr_log(WLR_ERROR, "Rule pattern hit the match limit, treating as no match: %s", p->pattern);

	return false;
}

void rule_pattern_clear(rule_pattern_t *p) {
	if (p->code)
		pcre2_code_free(p->code);
	p->code = NULL;
	p->pattern[0] = '\0';
	p->mode = RULE_MATCH_EXACT;
	p->caseless = false;
}

rule_consequence_t *find_matching_rule(const char *app_id, const char *title, const char *tag) {
	static rule_consequence_t merged;
	memset(&merged, 0, sizeof(merged));

	rule_t *r, *tmp;
	wl_list_for_each_safe(r, tmp, &rule_list, link) {
		bool app_id_matches = rule_pattern_match(&r->match.app_id, app_id);
		bool title_matches = rule_pattern_match(&r->match.title, title);
		bool tag_matches = rule_pattern_match(&r->match.tag, tag);

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
	wl_list_for_each_safe(r, tmp, &rule_list, link)
		remove_rule(r);

	wl_list_init(&rule_list);
}
