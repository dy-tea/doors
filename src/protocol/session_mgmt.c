#include "fs.h"
#include "ipc/json.h"
#include "layout/floating.h"
#include "layout/layout.h"
#include "layout/tree.h"
#include "once.h"
#include "output/output.h"
#include "protocol/session_mgmt.h"
#include "protocol/workspace.h"
#include "protocol/xdg_toplevel.h"
#include "server.h"
#include "tree.h"
#include "xx-session-management-v1-protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#define SESSION_MANAGER_VERSION 1

// longest session identifier accepted from a client, also the width of generated ids
#define SESSION_ID_MAX 64

// longest toplevel name accepted from a client
#define SESSION_NAME_MAX 256

// upper bound for geometry read back from a session file
#define SESSION_COORD_MAX 65536

// what a session remembers about a single named toplevel
typedef struct {
	char *name; // lookup key, owned by session_states_t
	char output[SMALEN];
	char desktop[SMALEN];

	struct wlr_box rect;

	bool floating;
	bool maximized;
	bool fullscreen;
	bool minimized;
} session_state_t;

typedef struct {
	session_state_t *items;
	size_t count;
	size_t cap;
} session_states_t;

struct session_mgr_t;

typedef struct session_t {
	struct wl_list link; // session_mgr_t.sessions

	struct session_mgr_t *manager;
	struct wl_client *client;
	struct wl_resource *resource;
	char *id;

	struct wl_list toplevels; // session_toplevel_t.link

	session_states_t states;
	bool restored;

	enum xx_session_manager_v1_reason reason;
} session_t;

typedef struct session_toplevel_t {
	struct wl_list link; // session_t.toplevels

	session_t *session;
	struct wl_resource *resource; // xx_toplevel_session_v1
	struct wl_resource *toplevel_resource; // xdg_toplevel
	struct wlr_xdg_toplevel *xdg_toplevel;
	char *name;

	session_state_t restore;

	// before initial commit of toplevel
	bool restore_pending;

	// initial commit happened before surface was initialized
	bool restore_unapplied;
} session_toplevel_t;

typedef struct session_mgr_t {
	struct wl_global *global;
	struct wl_list sessions; // session_t.link
	char *storage_dir;
	bool storage_dir_checked;

	struct {
		struct wl_listener display_destroy;
	};
} session_mgr_t;

static void session_destroy(session_t *session);
static void session_toplevel_forget(session_toplevel_t *tl);
static void session_toplevel_detach(session_toplevel_t *tl);
static session_toplevel_t *manager_find_toplevel(session_mgr_t *manager,
	struct wlr_xdg_toplevel *xdg_toplevel);
static bool session_states_save(session_t *session);

static session_mgr_t *session_manager = NULL;

// returns the directory session state is kept in, NULL when it cannot be created
static const char *session_storage_dir(session_mgr_t *manager) {
	if (manager->storage_dir)
		return manager->storage_dir;
	if (manager->storage_dir_checked)
		return NULL;

	manager->storage_dir_checked = true;

	const char *state_home = getenv("XDG_STATE_HOME");
	char path[PATH_MAX];

	if (state_home && state_home[0] == '/') {
		snprintf(path, sizeof(path), "%s/doors/sessions", state_home);
	} else {
		const char *home = getenv("HOME");
		if (!home || home[0] != '/') {
			wlr_log(WLR_ERROR, "Session storage is disabled, no usable HOME");
			return NULL;
		}
		snprintf(path, sizeof(path), "%s/.local/state/doors/sessions", home);
	}

	if (!mkdir_p(path, 0700)) {
		wlr_log(WLR_ERROR, "Failed to create %s: %s", path, strerror(errno));
		return NULL;
	}

	manager->storage_dir = strdup(path);
	return manager->storage_dir ? manager->storage_dir : NULL;
}

// session ids end up in a path, so they are restricted to a safe charset
static bool session_id_is_valid(const char *id) {
	if (id == NULL || id[0] == '\0' || strlen(id) > SESSION_ID_MAX)
		return false;

	for (const char *p = id; *p != '\0'; p++) {
		if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' ||
				*p == '_' || *p == '.') {
			continue;
		}
		return false;
	}

	return true;
}

static bool session_name_is_valid(const char *name) {
	return name != NULL && name[0] != '\0' && strlen(name) <= SESSION_NAME_MAX;
}

// copies a bounded string, false when it does not fit
static bool session_copy_bounded(char *dst, size_t dst_size, const char *src) {
	if (src == NULL)
		return false;

	size_t len = strlen(src);
	if (len >= dst_size)
		return false;

	memcpy(dst, src, len + 1);
	return true;
}

static char *session_path(session_mgr_t *manager, const char *id) {
	const char *dir = session_storage_dir(manager);
	if (!dir)
		return NULL;

	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s.json", dir, id);
	return strdup(path);
}

// reads a whole file into a NUL terminated buffer, NULL on failure
static char *session_read_file(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;

	size_t cap = 4096;
	size_t len = 0;
	char *buf = malloc(cap);
	if (!buf) {
		fclose(f);
		return NULL;
	} while (!feof(f)) {
		if (len == cap - 1) {
			char *grown = realloc(buf, cap * 2);
			if (!grown) {
				free(buf);
				fclose(f);
				return NULL;
			}
			buf = grown;
			cap *= 2;
		}

		size_t n = fread(buf + len, 1, cap - 1 - len, f);
		len += n;
	}

	bool failed = ferror(f) != 0;
	fclose(f);

	if (failed) {
		free(buf);
		return NULL;
	}

	buf[len] = '\0';
	return buf;
}

// writes a JSON string body, escaping everything that needs it
static void session_write_json_string(FILE *f, const char *s) {
	for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
		switch (*p) {
		case '"':
			fputs("\\\"", f);
			break;
		case '\\':
			fputs("\\\\", f);
			break;
		case '\b':
			fputs("\\b", f);
			break;
		case '\f':
			fputs("\\f", f);
			break;
		case '\n':
			fputs("\\n", f);
			break;
		case '\r':
			fputs("\\r", f);
			break;
		case '\t':
			fputs("\\t", f);
			break;
		default:
			if (*p < 0x20)
				fprintf(f, "\\u%04x", *p);
			else
				fputc(*p, f);
			break;
		}
	}
}

// geometry read back from disk is untrusted, keep it in a sane range
static int session_coord_from_json(const json_value_t *value) {
	double num;
	if (!json_get_number(value, &num))
		return 0;

	if (num < -SESSION_COORD_MAX)
		return -SESSION_COORD_MAX;
	if (num > SESSION_COORD_MAX)
		return SESSION_COORD_MAX;
	return (int)num;
}

static bool session_bool_from_json(const json_value_t *entry, const char *key) {
	const json_value_t *value = json_get(entry, key);
	return value != NULL && value->type == JSON_BOOL && value->value.boolean;
}

static bool session_state_is_restorable(const session_state_t *state) {
	return state->rect.width > 0 && state->rect.height > 0;
}

// copies everything but the name, which stays owned by the states array
static void session_state_copy(session_state_t *dst, const session_state_t *src) {
	memcpy(dst->output, src->output, sizeof(dst->output));
	memcpy(dst->desktop, src->desktop, sizeof(dst->desktop));
	dst->rect = src->rect;
	dst->floating = src->floating;
	dst->maximized = src->maximized;
	dst->fullscreen = src->fullscreen;
	dst->minimized = src->minimized;
}

static session_state_t *session_states_find(session_states_t *states, const char *name) {
	for (size_t i = 0; i < states->count; i++) {
		if (strcmp(states->items[i].name, name) == 0)
			return &states->items[i];
	}
	return NULL;
}

static session_state_t *session_states_get(session_states_t *states, const char *name) {
	session_state_t *state = session_states_find(states, name);
	if (state)
		return state;

	if (states->count == states->cap) {
		size_t cap = states->cap ? states->cap * 2 : 8;
		session_state_t *items = realloc(states->items, cap * sizeof(*items));
		if (!items)
			return NULL;
		states->items = items;
		states->cap = cap;
	}

	session_state_t *item = &states->items[states->count++];
	memset(item, 0, sizeof(*item));
	item->name = strdup(name);
	if (!item->name) {
		states->count--;
		return NULL;
	}

	return item;
}

static void session_states_remove(session_states_t *states, const char *name) {
	for (size_t i = 0; i < states->count; i++) {
		if (strcmp(states->items[i].name, name) != 0)
			continue;
		free(states->items[i].name);
		memmove(&states->items[i], &states->items[i + 1],
			(states->count - i - 1) * sizeof(*states->items));
		states->count--;
		return;
	}
}

static void session_states_clear(session_states_t *states) {
	for (size_t i = 0; i < states->count; i++)
		free(states->items[i].name);
	free(states->items);
	states->items = NULL;
	states->count = 0;
	states->cap = 0;
}

// folds one entry of a session file into the in memory states, false when entry is unusable
static bool session_states_read_entry(session_t *session, const json_value_t *entry) {
	if (entry == NULL || entry->type != JSON_OBJECT)
		return false;

	const char *name = json_get_string(json_get(entry, "name"));
	if (!session_name_is_valid(name))
		return false;

	session_state_t parsed = {0};
	parsed.rect.x = session_coord_from_json(json_get(entry, "x"));
	parsed.rect.y = session_coord_from_json(json_get(entry, "y"));
	parsed.rect.width = session_coord_from_json(json_get(entry, "width"));
	parsed.rect.height = session_coord_from_json(json_get(entry, "height"));
	parsed.floating = session_bool_from_json(entry, "floating");
	parsed.maximized = session_bool_from_json(entry, "maximized");
	parsed.fullscreen = session_bool_from_json(entry, "fullscreen");
	parsed.minimized = session_bool_from_json(entry, "minimized");

	if (!session_state_is_restorable(&parsed))
		return false;

	session_copy_bounded(parsed.output, sizeof(parsed.output), json_get_string(json_get(entry,
		"output")));
	session_copy_bounded(parsed.desktop, sizeof(parsed.desktop), json_get_string(json_get(entry,
		"desktop")));

	session_state_t *state = session_states_get(&session->states, name);
	if (!state)
		return false;

	session_state_copy(state, &parsed);
	session->restored = true;
	return true;
}

static void session_states_load(session_t *session) {
	session_mgr_t *manager = session->manager;

	char *path = session_path(manager, session->id);
	if (!path)
		return;

	char *buf = session_read_file(path);
	free(path);
	if (!buf)
		return;

	char err[128];
	json_value_t *root = json_parse(buf, err, sizeof(err));
	free(buf);
	if (!root) {
		wlr_log(WLR_ERROR, "Failed to parse session %s: %s", session->id, err);
		return;
	}

	const json_value_t *toplevels = json_get(root, "toplevels");
	if (toplevels && toplevels->type == JSON_ARRAY) {
		for (size_t i = 0; i < toplevels->value.list.count; i++)
			session_states_read_entry(session, toplevels->value.list.items[i]);
	}

	json_free(root);
}

static bool session_states_save(session_t *session) {
	session_mgr_t *manager = session->manager;
	char *path = session_path(manager, session->id);
	if (!path)
		return false;

	char *path_tmp = NULL;
	if (asprintf(&path_tmp, "%s.tmp", path) < 0) {
		free(path);
		return false;
	}

	int fd = open(path_tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0) {
		wlr_log(WLR_ERROR, "Failed to write session %s: %s", session->id, strerror(errno));
		free(path_tmp);
		free(path);
		return false;
	}

	FILE *f = fdopen(fd, "w");
	if (!f) {
		wlr_log(WLR_ERROR, "Failed to write session %s: %s", session->id, strerror(errno));
		close(fd);
		free(path_tmp);
		free(path);
		return false;
	}

	fputs("{\n\t\"id\": \"", f);
	session_write_json_string(f, session->id);
	fputs("\",\n\t\"toplevels\": [\n", f);

	for (size_t i = 0; i < session->states.count; i++) {
		session_state_t *state = &session->states.items[i];

		fputs("\t\t{\"name\": \"", f);
		session_write_json_string(f, state->name);
		fputs("\", \"floating\": ", f);
		fputs(state->floating ? "true" : "false", f);
		fputs(", \"maximized\": ", f);
		fputs(state->maximized ? "true" : "false", f);
		fputs(", \"fullscreen\": ", f);
		fputs(state->fullscreen ? "true" : "false", f);
		fputs(", \"minimized\": ", f);
		fputs(state->minimized ? "true" : "false", f);
		fputs(", \"output\": \"", f);
		session_write_json_string(f, state->output);
		fputs("\", \"desktop\": \"", f);
		session_write_json_string(f, state->desktop);
		fprintf(f, "\", \"x\": %d, \"y\": %d, \"width\": %d, \"height\": %d}%s\n", state->rect.x,
			state->rect.y, state->rect.width, state->rect.height, i + 1 < session->states.count ? "," : "");
	}

	fputs("\t]\n}\n", f);

	bool failed = ferror(f) != 0;
	if (fclose(f) != 0)
		failed = true;
	if (failed) {
		wlr_log(WLR_ERROR, "Failed to write session %s: %s", session->id, strerror(errno));
		unlink(path_tmp);
		free(path_tmp);
		free(path);
		return false;
	}

	if (rename(path_tmp, path) != 0) {
		wlr_log(WLR_ERROR, "Failed to store session %s: %s", session->id, strerror(errno));
		unlink(path_tmp);
		free(path_tmp);
		free(path);
		return false;
	}

	free(path_tmp);
	free(path);
	return true;
}

static void session_state_delete(session_t *session, const char *name) {
	session_states_remove(&session->states, name);
	session_states_save(session);
}

// remember current state of a toplevel, no-op when nothing to save
static void session_toplevel_save_state(session_toplevel_t *tl) {
	xdg_toplevel_t *toplevel = tl->xdg_toplevel->base->data;
	if (!toplevel)
		return;

	node_t *n = toplevel->view.node;
	if (!n || !n->client)
		return;

	struct wlr_box rect = node_current_rect(n);
	if (rect.width <= 0 || rect.height <= 0)
		return;

	session_state_t *state = session_states_get(&tl->session->states, tl->name);
	if (!state)
		return;

	client_t *c = n->client;
	client_state_t base = c->state == STATE_FULLSCREEN ? c->last_state : c->state;

	session_state_t next = {0};
	next.floating = base == STATE_FLOATING || base == STATE_PSEUDO_TILED;
	next.maximized = client_is_maximized(c);
	next.fullscreen = c->state == STATE_FULLSCREEN;
	next.minimized = c->flags.minimized;
	next.rect = rect;
	if (n->output)
		session_copy_bounded(next.output, sizeof(next.output), n->output->name);
	if (n->desktop)
		session_copy_bounded(next.desktop, sizeof(next.desktop), n->desktop->name);

	// nothing worth writing to disk
	if (state->floating == next.floating && state->maximized == next.maximized &&
			state->fullscreen == next.fullscreen && state->minimized == next.minimized &&
			wlr_box_equal(&state->rect, &next.rect) && strcmp(state->output,
			next.output) == 0 && strcmp(state->desktop, next.desktop) == 0) {
		return;
	}

	session_state_copy(state, &next);
	session_states_save(tl->session);
}

static session_toplevel_t *session_find_toplevel(session_t *session,
		struct wlr_xdg_toplevel *xdg_toplevel) {
	session_toplevel_t *tl;
	wl_list_for_each(tl, &session->toplevels, link) {
		if (tl->xdg_toplevel == xdg_toplevel)
			return tl;
	}
	return NULL;
}

static session_toplevel_t *session_find_name(session_t *session, const char *name) {
	session_toplevel_t *tl;
	wl_list_for_each(tl, &session->toplevels, link) {
		if (strcmp(tl->name, name) == 0)
			return tl;
	}
	return NULL;
}

// a toplevel may only be tracked by one session, otherwise its state would be
// written by several clients at once
static session_toplevel_t *manager_find_toplevel(session_mgr_t *manager,
		struct wlr_xdg_toplevel *xdg_toplevel) {
	if (!manager)
		return NULL;

	session_t *session;
	wl_list_for_each(session, &manager->sessions, link) {
		session_toplevel_t *tl = session_find_toplevel(session, xdg_toplevel);
		if (tl)
			return tl;
	}
	return NULL;
}

// keep remembered rectangle on the output it is being restored onto, that
// output may have moved, changed scale or been unplugged since
static struct wlr_box session_clamp_rect(output_t *m, desktop_t *d, struct wlr_box rect) {
	if (rect.width <= 0 || rect.height <= 0 || m == NULL || d == NULL)
		return rect;

	struct wlr_box area = desktop_usable_area(m, d);
	if (area.width <= 0 || area.height <= 0)
		return rect;

	if (rect.width > area.width)
		rect.width = area.width;
	if (rect.height > area.height)
		rect.height = area.height;

	// center if off output
	if (!wlr_box_intersects(&area, &rect)) {
		rect.x = area.x + (area.width - rect.width) / 2;
		rect.y = area.y + (area.height - rect.height) / 2;
		return rect;
	}

	// partly off an edge, slide it back until it fits
	if (rect.x < area.x)
		rect.x = area.x;
	if (rect.y < area.y)
		rect.y = area.y;
	if (rect.x + rect.width > area.x + area.width)
		rect.x = area.x + area.width - rect.width;
	if (rect.y + rect.height > area.y + area.height)
		rect.y = area.y + area.height - rect.height;

	return rect;
}

static void toplevel_session_handle_destroy(struct wl_client *wl_client,
		struct wl_resource *resource) {
	(void)wl_client;
	session_toplevel_t *tl = wl_resource_get_user_data(resource);
	if (!tl)
		return;

	tl->resource = NULL;
	wl_resource_set_user_data(resource, NULL);
	wl_resource_destroy(resource);
}

static void toplevel_session_handle_remove(struct wl_client *wl_client,
		struct wl_resource *resource) {
	(void)wl_client;
	session_toplevel_forget(wl_resource_get_user_data(resource));
}

static void toplevel_session_resource_destroy(struct wl_resource *resource) {
	session_toplevel_t *tl = wl_resource_get_user_data(resource);
	if (!tl)
		return;

	tl->resource = NULL;
	wl_list_remove(&tl->link);
	free(tl->name);
	free(tl);
}

// detaches toplevel from its session
static void session_toplevel_detach(session_toplevel_t *tl) {
	struct wl_resource *resource = tl->resource;

	tl->resource = NULL;
	wl_list_remove(&tl->link);
	if (resource) {
		wl_resource_set_user_data(resource, NULL);
		wl_resource_destroy(resource);
	}

	free(tl->name);
	free(tl);
}

// stops managing toplevel and forgets its state
static void session_toplevel_forget(session_toplevel_t *tl) {
	if (!tl->resource)
		return;

	session_state_delete(tl->session, tl->name);
	session_toplevel_detach(tl);
}

static const struct xx_toplevel_session_v1_interface toplevel_session_impl = {
	.destroy = toplevel_session_handle_destroy,
	.remove = toplevel_session_handle_remove,
};

// seeds toplevel's next configure with remembered size and state
static void session_restore_configure(session_toplevel_t *tl,
		struct wlr_xdg_toplevel *xdg_toplevel) {
	if (!session_state_is_restorable(&tl->restore))
		return;

	if (tl->restore.maximized)
		wlr_xdg_toplevel_set_maximized(xdg_toplevel, true);
	if (tl->restore.fullscreen)
		wlr_xdg_toplevel_set_fullscreen(xdg_toplevel, true);

	if (tl->restore.maximized || tl->restore.fullscreen)
		return;

	output_t *m = server.focused_output;
	struct wlr_box area = desktop_usable_area(m, m ? m->desk : NULL);
	int width = tl->restore.rect.width;
	int height = tl->restore.rect.height;
	if (area.width > 0 && width > area.width)
		width = area.width;
	if (area.height > 0 && height > area.height)
		height = area.height;

	if (width > 0 && height > 0)
		wlr_xdg_toplevel_set_size(xdg_toplevel, width, height);
}

// creates toplevel session object, returns NULL and raises the error on failure
static session_toplevel_t *session_add_toplevel(struct wl_client *wl_client,
		struct wl_resource *resource, uint32_t id, struct wl_resource *toplevel_resource, const char *name,
		bool restore) {
	session_t *session = wl_resource_get_user_data(resource);
	if (!session)
		return NULL;

	struct wlr_xdg_toplevel *xdg_toplevel = wlr_xdg_toplevel_from_resource(toplevel_resource);
	if (!xdg_toplevel) {
		wlr_log(WLR_ERROR, "Session toplevel: the object is not an xdg_toplevel");
		return NULL;
	}

	if (!session_name_is_valid(name)) {
		wl_resource_post_error(resource, XX_SESSION_V1_ERROR_NAME_IN_USE,
			"name must be between 1 and %d bytes", SESSION_NAME_MAX);
		return NULL;
	}

	if (session_find_toplevel(session, xdg_toplevel)) {
		wl_resource_post_error(resource, XX_SESSION_V1_ERROR_NAME_IN_USE,
			"toplevel is already managed by this session");
		return NULL;
	}

	if (manager_find_toplevel(session->manager, xdg_toplevel)) {
		wl_resource_post_error(resource, XX_SESSION_V1_ERROR_NAME_IN_USE,
			"toplevel is already managed by another session");
		return NULL;
	}

	if (session_find_name(session, name)) {
		wl_resource_post_error(resource, XX_SESSION_V1_ERROR_NAME_IN_USE,
			"another toplevel of this session already uses the name '%s'", name);
		return NULL;
	}

	if (restore && (xdg_toplevel->base->initial_commit || xdg_toplevel->base->surface->mapped)) {
		wl_resource_post_error(resource, XX_SESSION_V1_ERROR_ALREADY_MAPPED,
			"toplevel has already been committed");
		return NULL;
	}

	session_toplevel_t *tl = calloc(1, sizeof(*tl));
	if (!tl) {
		wl_client_post_no_memory(wl_client);
		return NULL;
	}

	tl->name = strdup(name);
	if (!tl->name) {
		free(tl);
		wl_client_post_no_memory(wl_client);
		return NULL;
	}

	tl->session = session;
	tl->xdg_toplevel = xdg_toplevel;
	tl->toplevel_resource = toplevel_resource;

	tl->resource = wl_resource_create(wl_client, &xx_toplevel_session_v1_interface,
		wl_resource_get_version(resource), id);
	if (!tl->resource) {
		free(tl->name);
		free(tl);
		wl_client_post_no_memory(wl_client);
		return NULL;
	}
	wl_resource_set_implementation(tl->resource, &toplevel_session_impl, tl,
		toplevel_session_resource_destroy);

	wl_list_insert(&session->toplevels, &tl->link);

	if (restore) {
		session_state_t *state = session_states_find(&session->states, name);
		if (state)
			session_state_copy(&tl->restore, state);
		tl->restore_pending = true;
	} else {
		session_toplevel_save_state(tl);
	}

	return tl;
}

static void session_handle_destroy(struct wl_client *wl_client, struct wl_resource *resource) {
	(void)wl_client;
	session_destroy(wl_resource_get_user_data(resource));
}

static void session_handle_remove(struct wl_client *wl_client, struct wl_resource *resource) {
	(void)wl_client;
	session_t *session = wl_resource_get_user_data(resource);
	if (!session)
		return;

	// forget everything we know about this session
	char *path = session_path(session->manager, session->id);
	if (path) {
		unlink(path);
		free(path);
	}

	session_destroy(session);
}

static void session_handle_add_toplevel(struct wl_client *wl_client, struct wl_resource *resource,
		uint32_t id, struct wl_resource *toplevel, const char *name) {
	session_add_toplevel(wl_client, resource, id, toplevel, name, false);
}

static void session_handle_restore_toplevel(struct wl_client *wl_client,
		struct wl_resource *resource, uint32_t id, struct wl_resource *toplevel, const char *name) {
	session_add_toplevel(wl_client, resource, id, toplevel, name, true);
}

// the client went away, drop the session but keep what it stored
static void session_resource_destroy(struct wl_resource *resource) {
	session_t *session = wl_resource_get_user_data(resource);
	if (!session)
		return;

	session->resource = NULL;
	session_destroy(session);
}

static const struct xx_session_v1_interface session_impl = {
	.destroy = session_handle_destroy,
	.remove = session_handle_remove,
	.add_toplevel = session_handle_add_toplevel,
	.restore_toplevel = session_handle_restore_toplevel,
};

static void session_destroy(session_t *session) {
	if (!session)
		return;

	// make every toplevel session of this session inert
	session_toplevel_t *tl, *tmp;
	wl_list_for_each_safe(tl, tmp, &session->toplevels, link) {
		session_toplevel_detach(tl);
	}

	struct wl_resource *resource = session->resource;
	session->resource = NULL;
	if (resource)
		wl_resource_set_user_data(resource, NULL);

	wl_list_remove(&session->link);
	session_states_clear(&session->states);
	free(session->id);
	free(session);

	if (resource)
		wl_resource_destroy(resource);
}

// marks a session as taken over by another client
static void session_replace(session_t *session) {
	if (session->resource)
		xx_session_v1_send_replaced(session->resource);
	session_destroy(session);
}

static session_t *manager_find_session(session_mgr_t *manager, const char *id) {
	session_t *session;
	wl_list_for_each(session, &manager->sessions, link) {
		if (session->id && strcmp(session->id, id) == 0)
			return session;
	}
	return NULL;
}

static char *manager_generate_id(void) {
	unsigned char raw[SESSION_ID_MAX / 2];
	const size_t len = sizeof(raw);

	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return NULL;

	ssize_t n = read(fd, raw, len);
	int saved_errno = errno;
	close(fd);

	if (n != (ssize_t)len) {
		wlr_log(WLR_ERROR, "Failed to read random bytes: %s", strerror(saved_errno));
		return NULL;
	}

	char id[SESSION_ID_MAX + 1];
	for (size_t i = 0; i < len; i++)
		snprintf(id + i * 2, 3, "%02x", raw[i]);
	id[SESSION_ID_MAX] = '\0';

	return strdup(id);
}

static void manager_handle_get_session(struct wl_client *wl_client, struct wl_resource *resource,
		uint32_t id, uint32_t reason, const char *session_id) {
	session_mgr_t *manager = wl_resource_get_user_data(resource);

	if (!xx_session_manager_v1_reason_is_valid(reason, wl_resource_get_version(resource))) {
		wl_resource_post_error(resource, XX_SESSION_MANAGER_V1_ERROR_IN_USE, "invalid reason %u", reason);
		return;
	}

	// an unusable id is treated as if NULL had been passed
	if (session_id && session_id[0] != '\0' && !session_id_is_valid(session_id))
		session_id = NULL;

	session_t *previous = session_id ? manager_find_session(manager, session_id) : NULL;
	if (previous && previous->client == wl_client) {
		wl_resource_post_error(resource, XX_SESSION_MANAGER_V1_ERROR_IN_USE,
			"session '%s' is already in use", session_id);
		return;
	}

	// another client took this session over, make the old one inert
	if (previous) {
		wlr_log(WLR_INFO, "Session %s taken over by another client", session_id);
		session_replace(previous);
	}

	bool known = false;
	if (session_id) {
		char *path = session_path(manager, session_id);
		if (path) {
			known = access(path, R_OK) == 0;
			free(path);
		}
	}

	struct wl_resource *session_resource = wl_resource_create(wl_client, &xx_session_v1_interface,
		wl_resource_get_version(resource), id);
	if (!session_resource) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	session_t *session = calloc(1, sizeof(*session));
	if (!session) {
		wl_resource_destroy(session_resource);
		wl_client_post_no_memory(wl_client);
		return;
	}

	wl_list_init(&session->toplevels);
	session->manager = manager;
	session->client = wl_client;
	session->reason = reason;
	session->resource = session_resource;

	if (known) {
		session->id = strdup(session_id);
		if (!session->id)
			wl_client_post_no_memory(wl_client);
	} else {
		session->id = manager_generate_id();
		if (!session->id) {
			wl_resource_destroy(session_resource);
			free(session);
			wl_resource_post_error(resource, XX_SESSION_MANAGER_V1_ERROR_IN_USE,
				"could not generate a session id");
			return;
		}
	}

	if (!session->id) {
		wl_resource_destroy(session_resource);
		free(session);
		return;
	}

	wl_list_insert(&manager->sessions, &session->link);
	wl_resource_set_implementation(session_resource, &session_impl, session, session_resource_destroy);

	if (known)
		session_states_load(session);

	if (known && session->restored) {
		wlr_log(WLR_INFO, "Session %s restored (reason %u)", session->id, reason);
		xx_session_v1_send_restored(session_resource);
	} else {
		wlr_log(WLR_INFO, "Session %s created (reason %u)", session->id, reason);
		xx_session_v1_send_created(session_resource, session->id);
		session_states_save(session);
	}
}

static void manager_handle_destroy(struct wl_client *wl_client,
		struct wl_resource *manager_resource) {
	(void)wl_client;
	wl_resource_destroy(manager_resource);
}

static const struct xx_session_manager_v1_interface manager_impl = {
	.destroy = manager_handle_destroy,
	.get_session = manager_handle_get_session,
};

static void manager_bind(struct wl_client *wl_client, void *data, uint32_t version, uint32_t id) {
	session_mgr_t *manager = data;

	struct wl_resource *resource = wl_resource_create(wl_client, &xx_session_manager_v1_interface,
		version, id);
	if (!resource) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	wl_resource_set_implementation(resource, &manager_impl, manager, NULL);
}

static void manager_destroy(session_mgr_t *manager) {
	session_t *session, *tmp;
	wl_list_for_each_safe(session, tmp, &manager->sessions, link) {
		session_destroy(session);
	}

	wl_list_remove(&manager->display_destroy.link);
	if (manager->global)
		wl_global_destroy(manager->global);
	free(manager->storage_dir);
	free(manager);

	if (session_manager == manager)
		session_manager = NULL;
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	session_mgr_t *manager = wl_container_of(listener, manager, display_destroy);
	manager_destroy(manager);
}

struct wl_global *session_mgr_get_global(void) {
	return session_manager ? session_manager->global : NULL;
}

void session_management_init(void) {
	ONCE();
	if (!session_manager)
		session_manager = calloc(1, sizeof(*session_manager));
	if (!session_manager) {
		wlr_log(WLR_ERROR, "Failed to allocate session manager");
		return;
	}

	wl_list_init(&session_manager->sessions);

	session_manager->global = wl_global_create(server.wl_display, &xx_session_manager_v1_interface,
		SESSION_MANAGER_VERSION, session_manager, manager_bind);
	if (!session_manager->global) {
		wlr_log(WLR_ERROR, "Failed to create session manager global");
		free(session_manager);
		session_manager = NULL;
		return;
	}

	session_manager->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(server.wl_display, &session_manager->display_destroy);

	wlr_log(WLR_INFO, "Session manager initialized");
}

void session_management_fini(void) {
	ONCE();
	if (!session_manager)
		return;

	session_mgr_t *manager = session_manager;
	manager_destroy(manager);
}

void session_mgmt_handle_initial_commit(struct wlr_xdg_toplevel *xdg_toplevel) {
	session_toplevel_t *tl = manager_find_toplevel(session_manager, xdg_toplevel);
	if (!tl || !tl->restore_pending)
		return;

	tl->restore_pending = false;

	if (!tl->resource || !session_state_is_restorable(&tl->restore))
		return;

	if (!xdg_toplevel->base->initialized) {
		tl->restore_unapplied = true;
		return;
	}

	xx_toplevel_session_v1_send_restored(tl->resource, tl->toplevel_resource);
	session_restore_configure(tl, xdg_toplevel);
}

void session_mgmt_get_placement(struct wlr_xdg_toplevel *xdg_toplevel, session_placement_t *out) {
	memset(out, 0, sizeof(*out));

	session_toplevel_t *tl = manager_find_toplevel(session_manager, xdg_toplevel);
	if (!tl || !session_state_is_restorable(&tl->restore))
		return;

	if (tl->session->reason == XX_SESSION_MANAGER_V1_REASON_LAUNCH)
		return;

	memcpy(out->output, tl->restore.output, sizeof(out->output));
	memcpy(out->desktop, tl->restore.desktop, sizeof(out->desktop));
}

void session_mgmt_handle_mapped(struct wlr_xdg_toplevel *xdg_toplevel) {
	session_toplevel_t *tl = manager_find_toplevel(session_manager, xdg_toplevel);
	if (!tl)
		return;

	xdg_toplevel_t *toplevel = xdg_toplevel->base->data;
	if (!toplevel)
		return;

	node_t *n = toplevel->view.node;
	if (!n || !n->client)
		return;

	if (tl->restore_unapplied) {
		tl->restore_unapplied = false;
		session_restore_configure(tl, xdg_toplevel);
	}

	if (session_state_is_restorable(&tl->restore)) {
		output_t *m = n->output ? n->output : server.focused_output;
		desktop_t *d = desktop_for_node(n);
		if (!d)
			d = m ? m->desk : NULL;

		if (m && d) {
			struct wlr_box rect = session_clamp_rect(m, d, tl->restore.rect);

			if (tl->restore.floating) {
				if (!IS_FLOATING(n->client))
					float_node(m, d, n, &rect);
				else
					float_node_set_rect(n, rect);
			}
			if (tl->restore.maximized)
				client_set_maximized(m, d, n, true);
			if (tl->restore.fullscreen)
				client_set_fullscreen(m, d, n, true);
			if (tl->restore.minimized)
				client_set_minimized(m, d, n, true);

			arrange(m, d, true);
		}

		memset(&tl->restore, 0, sizeof(tl->restore));
	}

	session_toplevel_save_state(tl);
}

void session_mgmt_handle_state_changed(struct wlr_xdg_toplevel *xdg_toplevel) {
	session_toplevel_t *tl = manager_find_toplevel(session_manager, xdg_toplevel);
	if (tl)
		session_toplevel_save_state(tl);
}

void session_mgmt_handle_destroy(struct wlr_xdg_toplevel *xdg_toplevel) {
	session_toplevel_t *tl = manager_find_toplevel(session_manager, xdg_toplevel);
	if (tl)
		session_toplevel_detach(tl);
}

size_t session_mgmt_write_list(char *buf, size_t buf_size) {
	if (buf_size == 0)
		return 0;

	size_t off = 0;
#define WRITE(...) do { \
		int _n = snprintf(buf + off, buf_size - off, __VA_ARGS__); \
		if (_n < 0) \
			return off; \
		off += (size_t)_n; \
		if (off >= buf_size) \
			return buf_size - 1; \
	} while (0)

	if (!session_manager) {
		WRITE("sessions: not initialized\n");
		return off;
	}

	const char *dir = session_storage_dir(session_manager);
	WRITE("sessions: %s\n", dir ? dir : "(storage unavailable)");

	if (wl_list_empty(&session_manager->sessions)) {
		WRITE("  no active sessions\n");
		return off;
	}

	session_t *session;
	wl_list_for_each(session, &session_manager->sessions, link) {
		WRITE("  %s reason=%u toplevels=%d\n", session->id, session->reason,
			wl_list_length(&session->toplevels));

		session_toplevel_t *tl;
		wl_list_for_each(tl, &session->toplevels, link) {
			const session_state_t *state = session_states_find(&session->states, tl->name);
			if (!state) {
				WRITE("    %s (nothing recorded yet)%s%s\n", tl->name,
					tl->restore_pending ? " (awaiting commit)" : "", tl->resource ? "" : " (inert)");
				continue;
			}

			WRITE("    %s [%s%s%s%s] %dx%d at %d,%d on %s/%s%s%s\n", tl->name,
				state->floating ? "floating " : "", state->maximized ? "maximized " : "",
				state->fullscreen ? "fullscreen " : "", state->minimized ? "minimized " : "", state->rect.width,
				state->rect.height, state->rect.x, state->rect.y, state->output, state->desktop,
				tl->restore_pending ? " (awaiting commit)" : "", tl->resource ? "" : " (inert)");
		}

		size_t known = 0;
		for (size_t i = 0; i < session->states.count; i++) {
			if (!session_find_name(session, session->states.items[i].name))
				known++;
		}
		if (known > 0)
			WRITE("    (%zu remembered toplevel%s not open)\n", known, known == 1 ? "" : "s");
	}

#undef WRITE
	return off;
}
