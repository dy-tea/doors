#include "ipc/args.h"
#include "ipc/registry.h"
#include "layer_rule.h"
#include "parse.h"
#include <limits.h>
#include <string.h>

static const str_pair_t layer_names[] = {
	{"background", ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND},
	{"bottom", ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM},
	{"top", ZWLR_LAYER_SHELL_V1_LAYER_TOP},
	{"overlay", ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY},
	IPC_ENUM_END,
};

static const str_pair_t keyboard_names[] = {
	{"none", ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE},
	{"exclusive", ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE},
	{"on_demand", ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND},
	IPC_ENUM_END,
};

static const str_pair_t onoff_names[] = {
	{"on", 1},
	{"off", 0},
	IPC_ENUM_END,
};

static const str_pair_t layer_rule_flag_names[] = {
	{"blur", LAYER_RULE_BLUR},
	{"animations_disable", LAYER_RULE_ANIMATIONS_DISABLE},
	{"block_out_from_screenshare", LAYER_RULE_BLOCK_OUT_FROM_SCREENSHARE},
	IPC_ENUM_END,
};

static bool layer_rule_apply_flag(ipc_args_t *a, layer_rule_t *r, const char *arg, bool *ok) {
	*ok = true;

	for (const str_pair_t *e = layer_rule_flag_names; e->name; e++) {
		const char *value;
		if (!parse_kv_named(arg, e->name, &value))
			continue;

		long on;
		if (!ipc_enum_names(a, e->name, value, onoff_names, &on)) {
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

static void layer_rule_add(ipc_args_t *a) {
	if (layer_rule_count() >= MAX_LAYER_RULES) {
		ipc_fail(a, "Too many layer rules, the limit is %d\n", MAX_LAYER_RULES);
		return;
	}

	layer_rule_t *r = make_layer_rule();
	if (!r) {
		ipc_fail(a, "Failed to create layer rule\n");
		return;
	}

	bool have_namespace = false;
	char errbuf[256] = {0};

	ipc_foreach(a, arg) {
		if (arg[0] != '-' && !strchr(arg, '=')) {
			if (!have_namespace) {
				if (!rule_pattern_set(&r->match.namespace, arg, errbuf, sizeof(errbuf))) {
					free_layer_rule(r);
					ipc_fail(a, "namespace: %s\n", errbuf);
					return;
				}
				have_namespace = true;
			}
			continue;
		}

		const char *value;
		if (parse_kv_named(arg, "namespace", &value)) {
			if (!rule_pattern_set(&r->match.namespace, value, errbuf, sizeof(errbuf))) {
				free_layer_rule(r);
				ipc_fail(a, "namespace: %s\n", errbuf);
				return;
			}
			have_namespace = true;
			continue;
		}
		if (parse_kv_named(arg, "output", &value)) {
			if (!rule_pattern_set(&r->match.output, value, errbuf, sizeof(errbuf))) {
				free_layer_rule(r);
				ipc_fail(a, "output: %s\n", errbuf);
				return;
			}
			continue;
		}
		if (parse_kv_named(arg, "opacity", &value)) {
			float v;
			if (!ipc_float_str(a, value, "opacity", 0.0f, 1.0f, &v)) {
				free_layer_rule(r);
				return;
			}
			r->consequence.opacity = v;
			r->consequence.has |= LAYER_RULE_OPACITY;
			continue;
		}
		if (parse_kv_named(arg, "layer", &value)) {
			long v;
			if (!ipc_enum_names(a, "layer", value, layer_names, &v)) {
				free_layer_rule(r);
				return;
			}
			r->consequence.layer = (enum zwlr_layer_shell_v1_layer)v;
			r->consequence.has |= LAYER_RULE_LAYER;
			continue;
		}
		if (parse_kv_named(arg, "keyboard_interactivity", &value)) {
			long v;
			if (!ipc_enum_names(a, "keyboard_interactivity", value, keyboard_names, &v)) {
				free_layer_rule(r);
				return;
			}
			r->consequence.keyboard_interactivity = (enum zwlr_layer_surface_v1_keyboard_interactivity)v;
			r->consequence.has |= LAYER_RULE_KEYBOARD_INTERACTIVITY;
			continue;
		}

		bool applied;
		if (layer_rule_apply_flag(a, r, arg, &applied)) {
			if (!applied) {
				free_layer_rule(r);
				return;
			}
			continue;
		}

		free_layer_rule(r);
		ipc_fail(a, "Unknown layer rule option \"%s\"\n", arg);
		return;
	}

	if (!have_namespace || r->match.namespace.pattern[0] == '\0') {
		free_layer_rule(r);
		ipc_fail(a, "Must specify a namespace\n");
		return;
	}

	if (a->dry_run) {
		free_layer_rule(r);
		ipc_ok(a, "layer rule would be added\n");
		return;
	}

	add_layer_rule(r);
	ipc_ok(a, "layer rule added\n");
}

static void layer_rule_remove(ipc_args_t *a) {
	int idx;
	if (!ipc_int(a, "index", 0, INT_MAX, &idx))
		return;

	if (remove_layer_rule_by_index(idx))
		ipc_ok(a, "layer rule removed\n");
	else
		ipc_fail(a, "Invalid index %d\n", idx);
}

static void layer_rule_show_list(ipc_args_t *a) {
	char buf[DOORS_BUFSIZ];
	list_layer_rules(buf, sizeof(buf));
	ipc_ok(a, buf);
}

const ipc_sub_t layer_rule_subs[] = {
	IPC_SUB("-a", "--add", "layer_rule -a | --add <namespace> [output=<pattern>] "
		"[blur=on|off] [opacity=0-1] [animations_disable=on|off] "
		"[keyboard_interactivity=none|exclusive|on_demand] "
		"[layer=background|bottom|top|overlay] [block_out_from_screenshare=on|off]", layer_rule_add),
	IPC_SUB("-r", "--remove", "layer_rule -r | --remove <index>", layer_rule_remove),
	IPC_SUB("-l", "--list", "layer_rule -l | --list", layer_rule_show_list),
	IPC_SUB_END,
};

void ipc_cmd_layer_rule(ipc_args_t *a) {
	if (!ipc_sub_dispatch(a, layer_rule_subs))
		ipc_fail_unknown(a, layer_rule_subs);
}
