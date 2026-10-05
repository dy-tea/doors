#include "animation.h"
#include "effects/effects.h"
#include "ipc/args.h"
#include "ipc/helpers.h"
#include "ipc/registry.h"
#include "output/config.h"
#include "output/output.h"
#include "protocol/workspace.h"
#include "server.h"
#include "tabs.h"
#include "transaction.h"
#include "tree.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/wayland.h>
#include <wlr/backend/x11.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

typedef struct {
	const char *name;
	struct output_config *oc;
	output_t *mon;
} out_ctx_t;

static out_ctx_t *ctx_of(ipc_args_t *a) {
	return a->ctx;
}

static bool need_output(ipc_args_t *a) {
	out_ctx_t *c = ctx_of(a);
	if (c->mon)
		return true;

	ipc_fail(a, "Monitor \"%s\" not found\n", c->name);
	return false;
}

static void apply_ok(ipc_args_t *a, const char *what) {
	output_config_apply(ctx_of(a)->oc);
	ipc_okf(a, "%s set\n", what);
}

static const cfg_enum_value_t onoff_values[] = {
	{"on", 1},
	{"enable", 1},
	{"true", 1},
	{"off", 0},
	{"disable", 0},
	{"false", 0},
	IPC_ENUM_END,
};

static const cfg_enum_value_t transform_values[] = {
	{"normal", WL_OUTPUT_TRANSFORM_NORMAL},
	{"0", WL_OUTPUT_TRANSFORM_NORMAL},
	{"90", WL_OUTPUT_TRANSFORM_90},
	{"180", WL_OUTPUT_TRANSFORM_180},
	{"270", WL_OUTPUT_TRANSFORM_270},
	{"flipped", WL_OUTPUT_TRANSFORM_FLIPPED_180},
	{"flipped-180", WL_OUTPUT_TRANSFORM_FLIPPED_180},
	{"flipped-90", WL_OUTPUT_TRANSFORM_FLIPPED_90},
	{"flipped-270", WL_OUTPUT_TRANSFORM_FLIPPED_270},
	IPC_ENUM_END,
};

static const cfg_enum_value_t bit_depth_values[] = {
	{"8", OUTPUT_CONFIG_RENDER_BIT_DEPTH_8},
	{"8-bit", OUTPUT_CONFIG_RENDER_BIT_DEPTH_8},
	{"10", OUTPUT_CONFIG_RENDER_BIT_DEPTH_10},
	{"10-bit", OUTPUT_CONFIG_RENDER_BIT_DEPTH_10},
	IPC_ENUM_END,
};

static void out_list(ipc_args_t *a) {
	IPC_REPLY(b);

	ipc_buff(&b, "[");
	output_t *o;
	bool first = true;
	wl_list_for_each(o, &mon_list, link) {
		struct wlr_output *wo = o->wlr_output;
		if (!first)
			ipc_buff(&b, ",");
		first = false;
		ipc_buff(&b, "\n  {\n"
			"    \"name\": \"%s\",\n"
			"    \"description\": \"%s\",\n"
			"    \"make\": \"%s\",\n"
			"    \"model\": \"%s\",\n"
			"    \"serial\": \"%s\",\n"
			"    \"width\": %d,\n"
			"    \"height\": %d,\n"
			"    \"refresh\": %.3f,\n"
			"    \"scale\": %.6g,\n"
			"    \"phys_width\": %d,\n"
			"    \"phys_height\": %d,\n"
			"    \"hdr\": %s,\n"
			"    \"allow_tearing\": %s,\n"
			"    \"enabled\": %s\n"
			"  }", wo->name ? wo->name : "", wo->description ? wo->description : "", wo->make ? wo->make : "",
				wo->model ? wo->model : "", wo->serial ? wo->serial : "", wo->width, wo->height,
				wo->refresh / 1000.0f, wo->scale, wo->phys_width, wo->phys_height, o->hdr ? "true" : "false",
				o->allow_tearing ? "true" : "false", wo->enabled ? "true" : "false");
	}
	ipc_buff(&b, "\n]\n");

	ipc_buf_send(a, &b);
}

static void create_output(struct wlr_backend *backend, void *data) {
	bool *done = data;
	if (*done)
		return;

	if (wlr_backend_is_wl(backend)) {
		wlr_wl_output_create(backend);
		*done = true;
	} else if (wlr_backend_is_headless(backend)) {
		wlr_headless_add_output(backend, 1920, 1080);
		*done = true;
	} else if (wlr_backend_is_x11(backend)) {
		wlr_x11_output_create(backend);
		*done = true;
	}
}

static void out_create(ipc_args_t *a) {
	bool done = false;
	wlr_multi_for_each_backend(server.backend, create_output, &done);

	if (done)
		ipc_ok(a, "Created output\n");
	else
		ipc_fail(a, "Can only create outputs for wayland, x11 or headless backends\n");
}

static const ipc_sub_t output_global_subs[] = {
	IPC_SUBA("list", "--list", "-l", "output list", out_list),
	IPC_SUB("create", NULL, "output create", out_create),
	IPC_SUB_END,
};

static void out_enable(ipc_args_t *a) {
	ctx_of(a)->oc->enable = OUTPUT_CONFIG_ENABLE;
	apply_ok(a, "Output enable");
}

static void out_disable(ipc_args_t *a) {
	ctx_of(a)->oc->enable = OUTPUT_CONFIG_DISABLE;
	apply_ok(a, "Output disable");
}

static void out_mode(ipc_args_t *a) {
	const char *spec;
	if (!ipc_need(a, "resolution", &spec))
		return;

	int width, height;
	float refresh = -1;

	const char *at = strchr(spec, '@');
	size_t res_len = at ? (size_t)(at - spec) : strlen(spec);
	if (at) {
		char *end;
		double v = strtod(at + 1, &end);
		if (end == at + 1 || *end != '\0' || v <= 0) {
			ipc_fail(a, "Invalid refresh rate in \"%s\"\n", spec);
			return;
		}
		refresh = (float)v;
	}

	char res[64];
	if (res_len >= sizeof(res)) {
		ipc_fail(a, "Invalid resolution \"%s\"\n", spec);
		return;
	}
	memcpy(res, spec, res_len);
	res[res_len] = '\0';

	if (sscanf(res, "%dx%d", &width, &height) != 2 || width <= 0 || height <= 0) {
		ipc_fail(a, "Expected WIDTHxHEIGHT[@REFRESH], got \"%s\"\n", spec);
		return;
	}

	out_ctx_t *c = ctx_of(a);
	c->oc->width = width;
	c->oc->height = height;
	c->oc->refresh_rate = refresh;
	apply_ok(a, "Output mode");
}

static void out_position(ipc_args_t *a) {
	int x, y;
	if (!ipc_int(a, "x", INT_MIN, INT_MAX, &x) || !ipc_int(a, "y", INT_MIN, INT_MAX, &y))
		return;

	out_ctx_t *c = ctx_of(a);
	c->oc->x = x;
	c->oc->y = y;
	apply_ok(a, "Output position");
}

static void out_scale(ipc_args_t *a) {
	// -1 is the default (auto scale)
	float scale;
	if (!ipc_float(a, "scale", -1.0f, 16.0f, &scale))
		return;

	ctx_of(a)->oc->scale = scale;
	apply_ok(a, "Output scale");
}

static void out_transform(ipc_args_t *a) {
	long value;
	if (!ipc_enum(a, "transform", transform_values, &value))
		return;

	ctx_of(a)->oc->transform = (int)value;
	apply_ok(a, "Output transform");
}

static void out_dpms(ipc_args_t *a) {
	long value;
	if (!ipc_enum(a, "state", onoff_values, &value))
		return;

	ctx_of(a)->oc->dpms_state = value ? OUTPUT_CONFIG_DPMS_ON : OUTPUT_CONFIG_DPMS_OFF;
	apply_ok(a, "Output dpms");
}

static void out_adaptive_sync(ipc_args_t *a) {
	long value;
	if (!ipc_enum(a, "state", onoff_values, &value))
		return;

	ctx_of(a)->oc->adaptive_sync = value ? OUTPUT_CONFIG_ADAPTIVE_SYNC_ENABLED :
		OUTPUT_CONFIG_ADAPTIVE_SYNC_DISABLED;
	apply_ok(a, "Output adaptive_sync");
}

static void out_render_bit_depth(ipc_args_t *a) {
	long value;
	if (!ipc_enum(a, "value", bit_depth_values, &value))
		return;

	ctx_of(a)->oc->render_bit_depth = (enum output_config_render_bit_depth)value;
	apply_ok(a, "Output render_bit_depth");
}

static void out_hdr(ipc_args_t *a) {
	long value;
	if (!ipc_enum(a, "state", onoff_values, &value))
		return;

	out_ctx_t *c = ctx_of(a);
	c->oc->hdr_enabled = value != 0;
	c->oc->hdr_set = true;
	apply_ok(a, "Output hdr");
}

static void out_tearing(ipc_args_t *a) {
	long value;
	if (!ipc_enum(a, "state", onoff_values, &value))
		return;

	ctx_of(a)->oc->allow_tearing = value ? 1 : 0;
	apply_ok(a, "Output tearing");
}

static struct wlr_color_transform *load_icc(const char *path) {
	int fd = open(path, O_RDONLY | O_NOCTTY | O_CLOEXEC);
	if (fd == -1)
		return NULL;

	struct stat info;
	if (fstat(fd, &info) == -1 || !S_ISREG(info.st_mode) || info.st_size <= 0) {
		close(fd);
		return NULL;
	}

	void *data = malloc(info.st_size);
	if (!data) {
		close(fd);
		return NULL;
	}

	size_t nread = 0;
	while (nread < (size_t)info.st_size) {
		ssize_t r = read(fd, (char *)data + nread, (size_t)info.st_size - nread);
		if (r == -1 && errno == EINTR)
			continue;
		if (r <= 0) {
			free(data);
			close(fd);
			return NULL;
		}
		nread += (size_t)r;
	}
	close(fd);

	struct wlr_color_transform *transform = wlr_color_transform_init_linear_to_icc(data,
		(size_t)info.st_size);
	free(data);
	return transform;
}

static void out_color_profile(ipc_args_t *a) {
	static const cfg_enum_value_t kinds[] = {
		{"gamma22", 0},
		{"srgb", 1},
		{"icc", 2},
		IPC_ENUM_END,
	};

	long kind;
	if (!ipc_enum(a, "profile type", kinds, &kind))
		return;

	struct wlr_color_transform *transform = NULL;
	if (kind == 1) {
		transform = wlr_color_transform_init_linear_to_inverse_eotf(WLR_COLOR_TRANSFER_FUNCTION_SRGB);
		if (!transform) {
			ipc_fail(a, "Failed to create sRGB transform\n");
			return;
		}
	} else if (kind == 2) {
		const char *path;
		if (!ipc_need(a, "ICC file path", &path))
			return;

		transform = load_icc(path);
		if (!transform) {
			ipc_fail(a, "Cannot read ICC profile \"%s\"\n", path);
			return;
		}
	}

	out_ctx_t *c = ctx_of(a);
	wlr_color_transform_unref(c->oc->color_transform);
	c->oc->color_transform = transform;
	apply_ok(a, "Output color_profile");
}

static void out_max_render_time(ipc_args_t *a) {
	const char *arg;
	if (!ipc_need(a, "value", &arg))
		return;

	int ms;
	if (streq(arg, "off")) {
		ms = 0;
	} else {
		char *end;
		long v = strtol(arg, &end, 10);
		if (*end != '\0' || v <= 0) {
			ipc_fail(a, "Expected \"off\" or a positive number of milliseconds\n");
			return;
		}
		ms = (int)v;
	}

	ctx_of(a)->oc->max_render_time = ms;
	apply_ok(a, "Output max_render_time");
}

static void out_modes(ipc_args_t *a) {
	if (!need_output(a))
		return;

	IPC_REPLY(b);

	ipc_buff(&b, "[");
	struct wlr_output *wo = ctx_of(a)->mon->wlr_output;
	struct wlr_output_mode *mode;
	bool first = true;
	wl_list_for_each(mode, &wo->modes, link) {
		if (!first)
			ipc_buff(&b, ",");
		first = false;
		ipc_buff(&b, "\n  {\n    \"width\": %d,\n    \"height\": %d,\n    \"refresh\": %.3f\n  }",
			mode->width, mode->height, mode->refresh / 1000.0f);
	}
	ipc_buff(&b, "\n]\n");

	ipc_buf_send(a, &b);
}

static void out_focus(ipc_args_t *a) {
	if (!need_output(a))
		return;

	output_t *mon = ctx_of(a)->mon;
	server.focused_output = mon;
	focus_node(mon, mon->desk, mon->desk ? mon->desk->focus : NULL);
	ipc_ok(a, "Focused\n");
}

static void out_rename(ipc_args_t *a) {
	if (!need_output(a))
		return;

	output_t *mon = ctx_of(a)->mon;
	if (!ipc_str(a, "name", mon->name, SMALEN))
		return;

	transaction_commit_dirty();
	ipc_ok(a, "Renamed\n");
}

static void out_add_desktops(ipc_args_t *a) {
	if (!need_output(a))
		return;

	output_t *mon = ctx_of(a)->mon;
	ipc_foreach(a, name) {
		desktop_t *d = calloc(1, sizeof(desktop_t));
		if (!d) {
			wlr_log(WLR_ERROR, "Allocation failed");
			ipc_fail(a, "Allocation failed\n");
			return;
		}
		desktop_init(d, mon, name);
		workspace_create_desktop(d->name);
		ipc_put_status(SUB_MASK_DESKTOP_ADD, "desktop_add[%s]\n", d->name);
	}

	transaction_commit_dirty();
	ipc_ok(a, "Desktops added\n");
}

// with no arguments lists desktops, otherwise renames them in order
static void out_desktops(ipc_args_t *a) {
	if (!need_output(a))
		return;

	output_t *mon = ctx_of(a)->mon;
	if (!ipc_peek(a)) {
		IPC_REPLY(b);

		desktop_t *d;
		wl_list_for_each(d, &mon->desk_list, link)
			ipc_buff(&b, "%s\n", d->name);

		ipc_buf_send(a, &b);
		return;
	}

	desktop_t *d = wl_list_empty(&mon->desk_list) ? NULL : wl_container_of(mon->desk_list.next, d,
		link);
	ipc_foreach(a, name) {
		if (d) {
			snprintf(d->name, SMALEN, "%s", name);
			d->name[SMALEN - 1] = '\0';
			workspace_create_desktop(d->name);
			d = d->link.next == &mon->desk_list ? NULL : wl_container_of(d->link.next, d, link);
		} else {
			desktop_t *newd = calloc(1, sizeof(desktop_t));
			if (!newd) {
				wlr_log(WLR_ERROR, "Allocation failed");
				ipc_fail(a, "Allocation failed\n");
				return;
			}
			desktop_init(newd, mon, name);
			workspace_create_desktop(newd->name);
		}
	}

	// desktops beyond given list are dropped
	while (d) {
		desktop_t *next = d->link.next == &mon->desk_list ? NULL : wl_container_of(d->link.next, d, link);
		if (d == mon->desk)
			mon->desk = next;
		wl_list_remove(&d->link);
		desktop_minimized_clear(d);
		free(d);
		d = next;
	}

	transaction_commit_dirty();
	workspace_sync();

	if (mon->desk)
		focus_node(mon, mon->desk, mon->desk->focus);

	ipc_ok(a, "Desktops reset\n");
}

static void out_reorder_desktops(ipc_args_t *a) {
	if (!need_output(a))
		return;

	const char *first;
	if (!ipc_need(a, "desktop name", &first))
		return;

	output_t *mon = ctx_of(a)->mon;
	desktop_t *d = mon->desk;
	while (d != NULL && ipc_peek(a) != NULL) {
		desktop_t *next = d->link.next == &mon->desk_list ? NULL : wl_container_of(d->link.next, d, link);

		ipc_foreach(a, name) {
			if (streq(name, d->name)) {
				snprintf(d->name, SMALEN, "%s", name);
				d->name[SMALEN - 1] = '\0';
				break;
			}
		}

		d = next;
	}

	transaction_commit_dirty();
	ipc_ok(a, "Desktops reordered\n");
}

static void out_swap_desktops(ipc_args_t *a) {
	if (!need_output(a))
		return;

	const char *name;
	if (!ipc_need(a, "target output", &name))
		return;

	output_t *mon = ctx_of(a)->mon;
	output_t *target = ipc_output_by_name(a, name);
	if (!target)
		return;
	if (target == mon) {
		ipc_fail(a, "Cannot swap with self\n");
		return;
	}

	output_t *m0 = mon;
	output_t *m1 = target;
	desktop_t *d0 = m0->desk;
	desktop_t *d1 = m1->desk;

	struct wl_list *a_list = &m0->desk_list;
	struct wl_list *b_list = &m1->desk_list;

	if (wl_list_empty(a_list)) {
		wl_list_insert_list(a_list, b_list);
		wl_list_init(b_list);
	} else if (wl_list_empty(b_list)) {
		wl_list_insert_list(b_list, a_list);
		wl_list_init(a_list);
	} else {
		struct wl_list *a_first = a_list->next;
		struct wl_list *a_last = a_list->prev;
		struct wl_list *b_first = b_list->next;
		struct wl_list *b_last = b_list->prev;
		a_list->next = b_first;
		b_first->prev = a_list;
		a_list->prev = b_last;
		b_last->next = a_list;
		b_list->next = a_first;
		a_first->prev = b_list;
		b_list->prev = a_last;
		a_last->next = b_list;
	}

	m0->desk = d1;
	m1->desk = d0;

	desktop_t *d;
	wl_list_for_each(d, &m0->desk_list, link)
		d->output = m0;
	wl_list_for_each(d, &m1->desk_list, link)
		d->output = m1;

	if (server.focused_output == m0)
		server.focused_output = m1;
	else if (server.focused_output == m1)
		server.focused_output = m0;

	transaction_commit_dirty();
	ipc_ok(a, "Swapped\n");
}

static void out_remove(ipc_args_t *a) {
	if (!need_output(a))
		return;

	output_t *mon = ctx_of(a)->mon;
	if (wl_list_length(&mon_list) == 1) {
		ipc_fail(a, "Cannot remove the only output\n");
		return;
	}
	if (mon->desk) {
		ipc_fail(a, "Cannot remove output with desktops\n");
		return;
	}

	char name[SMALEN];
	snprintf(name, sizeof(name), "%s", mon->name);
	desktop_t *last = mon->last_desk;

	output_teardown(mon);

	set_orphan_active_desk(last);

	output_t *focused = server.focused_output;
	if (focused && focused->desk && focused->desk->focus)
		focus_node(focused, focused->desk, focused->desk->focus);

	ipc_put_status(SUB_MASK_MONITOR_REMOVE, "monitor_remove[%s]\n", name);
	transaction_commit_dirty();
	ipc_ok(a, "Removed\n");
}

static void out_rectangle(ipc_args_t *a) {
	const char *spec;
	if (!ipc_need(a, "rectangle", &spec))
		return;

	if (!need_output(a))
		return;

	output_t *mon = ctx_of(a)->mon;
	int x = mon->rectangle.x;
	int y = mon->rectangle.y;
	int width, height;

	int px, py;
	if (sscanf(spec, "%dx%d:%d,%d", &width, &height, &px, &py) == 4) {
		x = px;
		y = py;
	} else if (sscanf(spec, "%d,%d,%d,%d", &x, &y, &width, &height) != 4) {
		if (sscanf(spec, "%dx%d", &width, &height) != 2) {
			ipc_fail(a, "Expected WIDTHxHEIGHT[:X,Y], X,Y,WIDTH,HEIGHT, or WIDTHxHEIGHT\n");
			return;
		}
	}

	out_ctx_t *c = ctx_of(a);
	c->oc->x = x;
	c->oc->y = y;
	c->oc->width = width;
	c->oc->height = height;
	apply_ok(a, "Output rectangle");
}

const ipc_sub_t output_subs[] = {
	IPC_SUB("enable", NULL, "output <name> enable", out_enable),
	IPC_SUB("disable", NULL, "output <name> disable", out_disable),
	IPC_SUBA("mode", "resolution", "res\0--res", "output <name> mode|resolution|res <WxH[@hz]>",
		out_mode),
	IPC_SUB("modes", NULL, "output <name> modes", out_modes),
	IPC_SUBA("position", "pos", "--pos", "output <name> position|pos <x> <y>", out_position),
	IPC_SUB("scale", NULL, "output <name> scale <factor>", out_scale),
	IPC_SUB("transform", NULL, "output <name> transform <transform>", out_transform),
	IPC_SUB("dpms", NULL, "output <name> dpms <on|off>", out_dpms),
	IPC_SUBA("adaptive_sync", "vrr", "--vrr", "output <name> adaptive_sync|vrr <on|off>",
		out_adaptive_sync),
	IPC_SUB("render_bit_depth", NULL, "output <name> render_bit_depth <8|10>", out_render_bit_depth),
	IPC_SUB("color_profile", NULL, "output <name> color_profile <gamma22|srgb|icc <path>>",
		out_color_profile),
	IPC_SUB("hdr", NULL, "output <name> hdr <on|off>", out_hdr),
	IPC_SUB("tearing", NULL, "output <name> tearing <on|off>", out_tearing),
	IPC_SUB("max_render_time", NULL, "output <name> max_render_time <off|ms>", out_max_render_time),
	IPC_SUBA("focus", "-f", "--focus", "output <name> focus|-f|--focus", out_focus),
	IPC_SUBA("rename", "-n", "--rename", "output <name> rename|-n|--rename <name>", out_rename),
	IPC_SUBA("add-desktops", "-a", "--add-desktops",
		"output <name> add-desktops|-a|--add-desktops <names...>", out_add_desktops),
	IPC_SUBA("desktops", "-d", "--desktops", "output <name> desktops|-d|--desktops [names...]",
		out_desktops),
	IPC_SUBA("reorder-desktops", "-o", "--reorder-desktops",
		"output <name> reorder-desktops|-o|--reorder-desktops <names...>", out_reorder_desktops),
	IPC_SUBA("swap-desktops", "-s", "--swap", "output <name> swap-desktops|-s|--swap <output>",
		out_swap_desktops),
	IPC_SUBA("remove", "-r", "--remove", "output <name> remove|-r|--remove", out_remove),
	IPC_SUBA("rectangle", "-g", "--rectangle", "output <name> rectangle|-g|--rectangle <rect>",
		out_rectangle),
	IPC_SUB_END,
};

void ipc_cmd_output(ipc_args_t *a) {
	if (ipc_sub_dispatch(a, output_global_subs))
		return;

	const char *arg = ipc_peek(a);
	if (!arg) {
		ipc_fail_unknown(a, output_global_subs);
		return;
	}

	const char *name = ipc_take(a);
	if (!ipc_peek(a)) {
		ipc_fail_unknown(a, output_subs);
		return;
	}

	out_ctx_t ctx = {
		.name = name,
		.mon = find_output_by_name(name)
	};

	ctx.oc = output_config_find(name);
	if (!ctx.oc) {
		ctx.oc = output_config_create(name);
		if (!ctx.oc) {
			ipc_fail(a, "Failed to create config for \"%s\"\n", name);
			return;
		}
		output_config_add(ctx.oc);
	}

	a->ctx = &ctx;

	if (!ipc_sub_dispatch(a, output_subs))
		ipc_fail_unknown(a, output_subs);
}
