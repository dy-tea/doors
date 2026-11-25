#include "ipc/args.h"
#include "ipc/registry.h"
#include "layout/layout.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>

static size_t command_count(const layout_impl_t *impl) {
	size_t count = 0;
	if (impl->commands != NULL) {
		for (const command_t *c = impl->commands; c->name != NULL; c++)
			count++;
	}
	return count;
}

static size_t setting_capacity(const setting_t *s) {
	return s->type == SETTING_STR && s->size > 512 ? s->size : 512;
}

// layout -l
static void layout_list_all(ipc_args_t *a) {
	IPC_REPLY_GROWING(b);

	for (size_t i = 0; i < layout_count(); i++) {
		const layout_impl_t *impl = layout_get_impl((layout_t)i);
		if (impl == NULL)
			continue;

		const size_t commands = command_count(impl);
		ipc_buff(&b, "%-16s %zu %s, %zu %s\n", impl->name, impl->settings_count,
			impl->settings_count == 1 ? "setting" : "settings", commands,
			commands == 1 ? "command" : "commands");
	}

	ipc_buf_send(a, &b);
}

// layout <name> reset [<setting>...]
static void layout_reset(ipc_args_t *a, const layout_t id, const layout_impl_t *impl) {
	const bool named = ipc_left(a) > 0;
	const setting_t **targets = NULL;
	size_t total = 0;
	char *dflt = NULL, *before = NULL;
	ipc_buf_t detail;
	ipc_buf_growable(&detail);
	size_t changed = 0;
	bool layout_state = false;

	if (named) {
		targets = calloc((size_t)ipc_left(a), sizeof(*targets));
		if (targets == NULL) {
			ipc_fail(a, "Out of memory\n");
			goto out;
		}

		ipc_foreach(a, arg) {
			const setting_t *s = layout_setting_find(id, arg);
			if (s == NULL) {
				if (layout_command_find(id, arg) != NULL)
					ipc_fail(a, "\"%s\" is a command, not a setting; \"layout %s reset\" "
						"restores its default\n", arg, impl->name);
				else
					ipc_fail(a, "Unknown setting \"%s\" for layout \"%s\"\n", arg, impl->name);
				goto out;
			}

			targets[total++] = s;
		}
	} else {
		if (impl->settings_count == 0 && impl->reset == NULL) {
			ipc_okf(a, "%s has no settings to reset\n", impl->name);
			goto out;
		}

		total = impl->settings_count;
	}

	size_t cap = 512;
	for (size_t i = 0; i < total; i++) {
		const setting_t *s = named ? targets[i] : &impl->settings[i];
		if (setting_capacity(s) > cap)
			cap = setting_capacity(s);
	}

	dflt = calloc(1, cap);
	before = calloc(1, cap);
	if (dflt == NULL || before == NULL) {
		ipc_fail(a, "Out of memory\n");
		goto out;
	}

	for (size_t i = 0; i < total; i++) {
		const setting_t *s = named ? targets[i] : &impl->settings[i];

		if (!layout_setting_default(id, s->name, dflt, cap)) {
			ipc_fail(a, "%s: no recorded default\n", s->name);
			goto out;
		}
		str_rtrim(dflt);

		setting_format(s, before, cap);
		str_rtrim(before);

		const bool same = streq(before, dflt);
		if (!same) {
			char err[256];
			if (!layout_setting_reset(id, s->name, err, sizeof(err), a->dry_run)) {
				ipc_fail(a, "%s", err);
				goto out;
			}
			changed++;
		}

		if (!named && same)
			continue;

		if (same)
			ipc_buff(&detail, "  %s %s (unchanged)\n", s->name, dflt);
		else
			ipc_buff(&detail, "  %s %s -> %s\n", s->name, before, dflt);
	}

	layout_state = !named && !a->dry_run && impl->reset != NULL;
	if (layout_state)
		impl->reset();

	IPC_REPLY_GROWING(b);

	if (changed == 0 && !layout_state) {
		ipc_buff(&b, "no change: %zu %s already at %s default\n", total,
			total == 1 ? "setting" : "settings", total == 1 ? "its" : "their");
	} else {
		ipc_buff(&b, "%s %zu of %zu %s to %s\n", a->dry_run ? "would reset" : "reset", changed, total,
			total == 1 ? "setting" : "settings", total == 1 ? "its default" : "their defaults");
	}

	ipc_buff(&b, "%s", detail.buf != NULL ? detail.buf : "");
	if (layout_state)
		ipc_buff(&b, "  layout state reset\n");

	if (detail.truncated) {
		ipc_buf_free(&b);
		ipc_fail(a, "Out of memory\n");
		goto out;
	}

	ipc_buf_send(a, &b);

out:
	ipc_buf_free(&detail);
	free(dflt);
	free(before);
	free(targets);
}

// layout <name> -l
static void layout_list_one(ipc_args_t *a, const layout_impl_t *impl) {
	IPC_REPLY_GROWING(b);
	bool ok = true;

	ipc_buff(&b, "%s settings:\n", impl->name);
	for (size_t i = 0; i < impl->settings_count; i++) {
		const setting_t *s = &impl->settings[i];
		const size_t cap = setting_capacity(s);

		char *value = calloc(1, cap), *range = calloc(1, cap);
		if (value == NULL || range == NULL) {
			ok = false;
		} else {
			setting_format(s, value, cap);
			str_rtrim(value);
			setting_range(s, range, cap);

			ipc_buff(&b, "  %-28s %-7s %-22s %s\n", s->name, setting_type_name(s->type), value, range);
		}

		free(value);
		free(range);

		if (!ok)
			break;
	}

	if (!ok) {
		ipc_buf_free(&b);
		ipc_fail(a, "Out of memory\n");
		return;
	}

	ipc_buff(&b, "%s commands:\n", impl->name);
	ipc_buff(&b, "  layout %s reset [<setting>...]\n", impl->name);
	if (impl->commands != NULL) {
		for (const command_t *c = impl->commands; c->name != NULL; c++)
			ipc_buff(&b, "  %s\n", c->usage != NULL ? c->usage : c->name);
	}

	ipc_buf_send(a, &b);
}

static bool layout_by_name(ipc_args_t *a, const char *name, layout_t *out) {
	if (!layout_find_by_name(name, out)) {
		ipc_fail(a, "Unknown layout \"%s\", try \"layout -l\"\n", name);
		return false;
	}
	return true;
}

// runs `layout <name> ...`
static void layout_run(ipc_args_t *a, const char *name) {
	layout_t id;
	if (!layout_by_name(a, name, &id))
		return;

	const layout_impl_t *impl = layout_get_impl(id);
	if (impl == NULL) {
		ipc_fail(a, "Unknown layout \"%s\"\n", name);
		return;
	}

	const char *what = ipc_peek(a);
	if (what == NULL || streq(what, "-l") || streq(what, "--list")) {
		if (what != NULL)
			ipc_take(a);

		layout_list_one(a, impl);
		return;
	}

	const setting_t *s = layout_setting_find(id, what);
	if (s != NULL) {
		ipc_take(a);

		if (ipc_peek(a) == NULL) {
			const size_t cap = setting_capacity(s);
			char *buf = calloc(1, cap);
			if (buf == NULL) {
				ipc_fail(a, "Out of memory\n");
				return;
			}

			setting_format(s, buf, cap);

			const size_t len = strlen(buf);
			if (len > 0 && buf[len - 1] != '\n' && len + 2 < cap) {
				buf[len] = '\n';
				buf[len + 1] = '\0';
			}

			ipc_ok(a, buf);
			free(buf);
			return;
		}

		const char *value = ipc_take(a);
		char err[256];
		if (!setting_apply(s, value, err, sizeof(err), a->dry_run)) {
			ipc_fail(a, "%s", err);
			return;
		}

		ipc_okf(a, "%s %s\n", s->name, a->dry_run ? "would be set" : "set");
		return;
	}

	if (streq(what, "reset") || streq(what, "-r") || streq(what, "--reset")) {
		ipc_take(a);
		layout_reset(a, id, impl);
		return;
	}

	const command_t *c = layout_command_find(id, what);
	if (c != NULL) {
		ipc_take(a);

		if (a->dry_run) {
			ipc_fail(a, "--dry-run is not supported by \"%s\"\n", what);
			return;
		}

		c->run(a);
		return;
	}

	ipc_fail(a, "Unknown setting or command \"%s\" for layout \"%s\", try \"layout %s -l\"\n", what,
		impl->name, impl->name);
}


#define LAYOUT_LEGACY_ARGS 32

// TODO: remove
bool ipc_cmd_layout_legacy(ipc_args_t *a, const char *name) {
	const char *layout_name, *option;
	if (!layout_legacy_lookup(name, &layout_name, &option))
		return false;

	char *argv[LAYOUT_LEGACY_ARGS];
	int count = 0;
	argv[count++] = (char *)option;

	ipc_args_t sub = *a;
	while (ipc_peek(&sub) != NULL) {
		if (count == LAYOUT_LEGACY_ARGS) {
			ipc_fail(a, "Too many arguments for \"%s\"\n", name);
			return true;
		}
		argv[count++] = (char *)ipc_take(&sub);
	}

	sub.v = argv;
	sub.n = count;
	layout_run(&sub, layout_name);
	return true;
}

void ipc_cmd_layout(ipc_args_t *a) {
	const char *name = ipc_peek(a);
	if (name == NULL) {
		ipc_fail(a, "Missing layout name, see \"layout -l\"\n");
		return;
	}

	if (streq(name, "-l") || streq(name, "--list")) {
		ipc_take(a);

		const char *only = ipc_peek(a);
		if (only == NULL) {
			layout_list_all(a);
			return;
		}

		ipc_take(a);
		layout_t id;
		if (layout_by_name(a, only, &id))
			layout_list_one(a, layout_get_impl(id));
		return;
	}

	ipc_take(a);
	layout_run(a, name);
}
