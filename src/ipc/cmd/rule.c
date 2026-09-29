#include "ipc/args.h"
#include "ipc/helpers.h"
#include "ipc/registry.h"
#include "rule.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const cfg_enum_value_t rule_flag_names[] = {
	{"follow", RULE_TYPE_FOLLOW},
	{"focus", RULE_TYPE_FOCUS},
	{"manage", RULE_TYPE_MANAGE},
	{"locked", RULE_TYPE_LOCKED},
	{"hidden", RULE_TYPE_HIDDEN},
	{"maximized", RULE_TYPE_MAXIMIZED},
	{"minimized", RULE_TYPE_MINIMIZED},
	{"sticky", RULE_TYPE_STICKY},
	{"blur", RULE_TYPE_BLUR},
	{"mica", RULE_TYPE_MICA},
	{"acrylic", RULE_TYPE_ACRYLIC},
	{"shadow", RULE_TYPE_SHADOW},
	{"block_out_from_screenshare", RULE_TYPE_BLOCK_OUT_FROM_SCREENSHARE},
	{"allow_tearing", RULE_TYPE_ALLOW_TEARING},
	{"shortcuts_inhibitor", RULE_TYPE_SHORTCUTS_INHIBITOR},
	{"animations_disable", RULE_TYPE_ANIM_DISABLE},
	IPC_ENUM_END,
};

static const cfg_enum_value_t rule_state_names[] = {
	{"tiled", STATE_TILED},
	{"floating", STATE_FLOATING},
	{"fullscreen", STATE_FULLSCREEN},
	{"pseudo_tiled", STATE_PSEUDO_TILED},
	IPC_ENUM_END,
};

static const cfg_enum_value_t rule_onoff_names[] = {
	{"on", 1},
	{"off", 0},
	IPC_ENUM_END,
};

static size_t key_prefix_len(const char *arg, const char *name) {
	size_t n = strlen(name);
	return (strncmp(arg, name, n) == 0 && arg[n] == '=') ? n + 1 : 0;
}

static bool rule_apply_flag(ipc_args_t *a, rule_t *r, const char *arg, bool *ok) {
	*ok = true;

	for (const cfg_enum_value_t *e = rule_flag_names; e->name; e++) {
		size_t len = key_prefix_len(arg, e->name);
		if (!len)
			continue;

		long on;
		if (!ipc_enum_names(a, e->name, arg + len, rule_onoff_names, &on)) {
			*ok = false;
			return true;
		}

		if (on)
			r->consequence.flags |= e->value;
		else
			r->consequence.flags &= ~e->value;
		r->consequence.has |= e->value;
		return true;
	}
	return false;
}

static bool rule_apply_key(ipc_args_t *a, rule_t *r, const char *arg) {
	if (key_prefix_len(arg, "title")) {
		snprintf(r->match.title, MAXLEN, "%s", arg + 6);
		return true;
	}
	if (key_prefix_len(arg, "tag")) {
		snprintf(r->match.tag, MAXLEN, "%s", arg + 4);
		return true;
	}
	if (key_prefix_len(arg, "app_id")) {
		snprintf(r->match.app_id, MAXLEN, "%s", arg + 7);
		return true;
	}
	if (key_prefix_len(arg, "desktop")) {
		snprintf(r->consequence.desktop, SMALEN, "%s", arg + 8);
		r->consequence.has |= RULE_TYPE_DESKTOP;
		return true;
	}
	if (key_prefix_len(arg, "state")) {
		long state;
		if (!ipc_enum_names(a, "state", arg + 6, rule_state_names, &state))
			return false;

		r->consequence.state = (client_state_t)state;
		r->consequence.has |= RULE_TYPE_STATE;
		return true;
	}
	if (key_prefix_len(arg, "opacity")) {
		float v;
		if (!ipc_float_str(a, arg + 8, "opacity", 0.0f, 1.0f, &v))
			return false;

		r->consequence.opacity = v;
		r->consequence.has |= RULE_TYPE_OPACITY;
		return true;
	}
	if (key_prefix_len(arg, "border_radius")) {
		r->consequence.border_radius = atof(arg + 14);
		r->consequence.has |= RULE_TYPE_BORDER_RADIUS;
		return true;
	}
	if (key_prefix_len(arg, "render_unfocused_fps")) {
		int v;
		if (!ipc_int_str(a, arg + 21, "render_unfocused_fps", 0, 1000, &v))
			return false;

		r->consequence.render_unfocused_fps = v;
		r->consequence.has |= RULE_TYPE_RENDER_UNFOCUSED_FPS;
		return true;
	}
	if (key_prefix_len(arg, "scroller_proportion_single")) {
		float v;
		if (!ipc_float_str(a, arg + 27, "scroller_proportion_single", 0.0f, 1.0f, &v))
			return false;

		r->consequence.scroller_proportion_single = v;
		r->consequence.has |= RULE_TYPE_SCROLLER_PROPORTION_SINGLE;
		return true;
	}
	if (key_prefix_len(arg, "scroller_proportion")) {
		float v;
		if (!ipc_float_str(a, arg + 20, "scroller_proportion", 0.0f, 1.0f, &v))
			return false;

		r->consequence.scroller_proportion = v;
		r->consequence.has |= RULE_TYPE_SCROLLER_PROPORTION;
		return true;
	}

	ipc_fail(a, "unknown rule option \"%s\"\n", arg);
	return false;
}

static void rule_add(ipc_args_t *a) {
	rule_t *r = make_rule();
	if (!r) {
		ipc_fail(a, "failed to create rule\n");
		return;
	}

	r->consequence.has = RULE_TYPE_FOLLOW | RULE_TYPE_FOCUS | RULE_TYPE_MANAGE;
	r->consequence.flags = RULE_TYPE_FOLLOW | RULE_TYPE_FOCUS | RULE_TYPE_MANAGE;

	bool have_app_id = false;

	ipc_foreach(a, arg) {
		if (streq(arg, "one_shot")) {
			r->match.one_shot = true;
			continue;
		}

		// a bare word is the app_id shorthand, and only the first one wins
		if (arg[0] != '-' && !strchr(arg, '=')) {
			if (!have_app_id) {
				snprintf(r->match.app_id, MAXLEN, "%s", arg);
				have_app_id = true;
			}
			continue;
		}

		bool applied;
		if (rule_apply_flag(a, r, arg, &applied)) {
			if (!applied) {
				free(r);
				return;
			}
			continue;
		}

		if (!rule_apply_key(a, r, arg)) {
			free(r);
			return;
		}

		if (key_prefix_len(arg, "app_id"))
			have_app_id = true;
	}

	if (!have_app_id && r->match.title[0] == '\0' && r->match.tag[0] == '\0') {
		free(r);
		ipc_fail(a, "must specify an app_id, title= or tag=\n");
		return;
	}

	add_rule(r);
	ipc_ok(a, "rule added\n");
}

static void rule_remove(ipc_args_t *a) {
	int idx;
	if (!ipc_int(a, "index", 0, INT_MAX, &idx))
		return;

	if (remove_rule_by_index(idx))
		ipc_ok(a, "rule removed\n");
	else
		ipc_fail(a, "invalid index %d\n", idx);
}

static void rule_show_list(ipc_args_t *a) {
	char buf[DOORS_BUFSIZ];
	list_rules(buf, sizeof(buf));
	ipc_ok(a, buf);
}

const ipc_sub_t rule_subs[] = {
	IPC_SUB("-a", "--add", "rule -a | --add <app_id> [one_shot] [key=value ...]", rule_add),
	IPC_SUB("-r", "--remove", "rule -r | --remove <index>", rule_remove),
	IPC_SUB("-l", "--list", "rule -l | --list", rule_show_list),
	IPC_SUB_END,
};

void ipc_cmd_rule(ipc_args_t *a) {
	if (!ipc_sub_dispatch(a, rule_subs))
		ipc_fail_unknown(a, rule_subs);
}
