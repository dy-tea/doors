#define _GNU_SOURCE

#include "commit.h"
#include "config.h"
#include "diagnostics.h"
#include "diagnostics_internal.h"
#include "ipc/args.h"
#include "log.h"
#include "output/output.h"
#include "rule.h"
#include "server.h"
#include "settings.h"
#include "str.h"
#include "transaction.h"
#include "version.h"
#include "view.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend/drm.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/libinput.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/wayland.h>
#include <wlr/backend/x11.h>
#include <wlr/config.h>
#include <wlr/render/wlr_renderer.h>
#ifdef WLR_HAS_GLES2_RENDERER
#include <wlr/render/gles2.h>
#endif
#ifdef WLR_HAS_VULKAN_RENDERER
#include <wlr/render/vulkan.h>
#endif
#include <wlr/render/pixman.h>
#include <wlr/types/wlr_output.h>

#define STATUS_CAP (16 * 1024)

static char status_snapshot[STATUS_CAP];
static _Atomic size_t status_snapshot_len;
static struct timespec started;

void diag_status_init(void) {
	clock_gettime(CLOCK_MONOTONIC, &started);
}

static void json_str(ipc_buf_t *b, const char *s) {
	ipc_buff(b, "\"");

	if (s != NULL) {
		for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
			switch (*p) {
			case '"':
				ipc_buff(b, "\\\"");
				break;
			case '\\':
				ipc_buff(b, "\\\\");
				break;
			case '\n':
				ipc_buff(b, "\\n");
				break;
			case '\r':
				ipc_buff(b, "\\r");
				break;
			case '\t':
				ipc_buff(b, "\\t");
				break;
			default:
				if (*p < 0x20)
					ipc_buff(b, "\\u%04x", *p);
				else
					ipc_buff(b, "%c", *p);
			}
		}
	}

	ipc_buff(b, "\"");
}

static const char *backend_kind(struct wlr_backend *backend) {
	if (wlr_backend_is_drm(backend))
		return "drm";
	if (wlr_backend_is_wl(backend))
		return "wayland";
	if (wlr_backend_is_x11(backend))
		return "x11";
	if (wlr_backend_is_headless(backend))
		return "headless";
	if (wlr_backend_is_libinput(backend))
		return "libinput";
	return "unknown";
}

typedef struct {
	char text[128];
} backend_names_t;

static void collect_backend(struct wlr_backend *backend, void *data) {
	backend_names_t *names = data;
	const char *kind = backend_kind(backend);
	size_t len = strlen(kind);

	// session can hold more than one backend of same kind
	for (const char *p = names->text; *p; ) {
		const char *end = strchr(p, '+');
		size_t n = end ? (size_t)(end - p) : strlen(p);
		if (n == len && strncmp(p, kind, len) == 0)
			return;
		if (end == NULL)
			break;
		p = end + 1;
	}

	str_append_sep(names->text, sizeof(names->text), "+", kind);
}

static void backend_name(char *buf, size_t bufsz) {
	buf[0] = '\0';

	if (server.backend == NULL)
		return;

	if (wlr_backend_is_multi(server.backend)) {
		backend_names_t names = {0};
		wlr_multi_for_each_backend(server.backend, collect_backend, &names);
		str_copy(buf, bufsz, names.text[0] ? names.text : "multi");
		return;
	}

	str_copy(buf, bufsz, backend_kind(server.backend));
}

static const char *renderer_api(void) {
	if (server.renderer == NULL)
		return "none";

#ifdef WLR_HAS_GLES2_RENDERER
	if (wlr_renderer_is_gles2(server.renderer))
		return "gles2";
#endif
#ifdef WLR_HAS_VULKAN_RENDERER
	if (wlr_renderer_is_vk(server.renderer))
		return "vulkan";
#endif
	if (wlr_renderer_is_pixman(server.renderer))
		return "pixman";

	return "unknown";
}

static void gpu_json(ipc_buf_t *b) {
#if defined(__linux__)
	if (server.renderer == NULL)
		return;

	int fd = wlr_renderer_get_drm_fd(server.renderer);
	if (fd < 0)
		return;

	struct stat st;
	if (fstat(fd, &st) == -1 || !S_ISCHR(st.st_mode))
		return;

	unsigned int maj = (unsigned int)((st.st_rdev >> 8) & 0xfff);
	unsigned int min = (unsigned int)((st.st_rdev & 0xff) | ((st.st_rdev >> 12) & 0xfff00));

	char path[128];
	snprintf(path, sizeof(path), "/sys/dev/char/%u:%u/device/uevent", maj, min);

	FILE *f = fopen(path, "re");
	if (f == NULL)
		return;

	char driver[64] = {0};
	char pci[64] = {0};

	char line[256];
	while (fgets(line, sizeof(line), f) != NULL) {
		const char *rest = str_skip_prefix(line, "DRIVER=");
		if (rest != NULL) {
			str_copy(driver, sizeof(driver), rest);
			str_chomp(driver);
			continue;
		}

		rest = str_skip_prefix(line, "PCI_ID=");
		if (rest != NULL) {
			str_copy(pci, sizeof(pci), rest);
			str_chomp(pci);
		}
	}
	fclose(f);

	if (driver[0] == '\0' && pci[0] == '\0')
		return;

	ipc_buff(b, ", \"gpu\": {");
	bool first = true;
	if (driver[0] != '\0') {
		ipc_buff(b, "\"driver\": ");
		json_str(b, driver);
		first = false;
	}
	if (pci[0] != '\0') {
		ipc_buff(b, "%s\"pci\": ", first ? "" : ", ");
		json_str(b, pci);
	}
	ipc_buff(b, "}");
#else
	(void)b;
#endif
}

static size_t count_desktops(void) {
	size_t count = wl_list_length(&orphan_desk_list);

	output_t *o;
	wl_list_for_each(o, &mon_list, link)
		count += wl_list_length(&o->desk_list);

	return count;
}

static bool status_render(char *buf, size_t bufsz) {
	ipc_buf_t b;
	ipc_buf_init(&b, buf, bufsz);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	long uptime = (long)(now.tv_sec - started.tv_sec);

	ipc_buff(&b, "{\n");
	ipc_buff(&b, "  \"status\": \"running\",\n");
	ipc_buff(&b, "  \"version\": ");
	json_str(&b, DOORS_VERSION);
	ipc_buff(&b, ",\n  \"commit\": ");
	json_str(&b, DOORS_COMMIT);
	ipc_buff(&b, ",\n  \"pid\": %d,\n", (int)getpid());
	ipc_buff(&b, "  \"uptime_s\": %ld,\n", uptime > 0 ? uptime : 0);

	char backend[128];
	backend_name(backend, sizeof(backend));
	ipc_buff(&b, "  \"backends\": ");
	json_str(&b, backend[0] ? backend : "unknown");

	ipc_buff(&b, ",\n  \"renderer\": {\"api\": ");
	json_str(&b, renderer_api());
	gpu_json(&b);
	ipc_buff(&b, "}");

	ipc_buff(&b, ",\n  \"log\": ");
	json_str(&b, log_get_path() ? log_get_path() : "");
	ipc_buff(&b, ",\n  \"stalls\": %zu", diag_stall_count());

	ipc_buff(&b, ",\n  \"outputs\": [");

	bool first = true;
	output_t *o;
	wl_list_for_each(o, &mon_list, link) {
		ipc_buff(&b, "%s\n    {", first ? "" : ",");
		first = false;

		ipc_buff(&b, "\"name\": ");
		json_str(&b, o->name);
		ipc_buff(&b, ", \"id\": %u, \"enabled\": %s", o->id, str_bool_text(o->enabled));

		if (o->wlr_output != NULL) {
			ipc_buff(&b, ", \"width\": %d, \"height\": %d", o->wlr_output->width, o->wlr_output->height);
			ipc_buff(&b, ", \"refresh_hz\": %.2f", o->wlr_output->refresh / 1000.0);
			ipc_buff(&b, ", \"scale\": %.2f", (double)o->wlr_output->scale);
			if (o->wlr_output->make != NULL && o->wlr_output->model != NULL) {
				ipc_buff(&b, ", \"make\": ");
				json_str(&b, o->wlr_output->make);
				ipc_buff(&b, ", \"model\": ");
				json_str(&b, o->wlr_output->model);
			}
		}

		ipc_buff(&b, ", \"rect\": {\"x\": %d, \"y\": %d, \"width\": %d, \"height\": %d}}", o->rectangle.x,
			o->rectangle.y, o->rectangle.width, o->rectangle.height);
	}
	ipc_buff(&b, "%s],\n", first ? "" : "\n  ");

	output_t *focused = server.focused_output;
	if (focused != NULL && focused->desk != NULL) {
		desktop_t *desk = focused->desk;
		ipc_buff(&b, "  \"focused\": {\"output\": ");
		json_str(&b, focused->name);
		ipc_buff(&b, ", \"desktop\": ");
		json_str(&b, desk->name);

		node_t *focus = desk->focus;
		if (focus != NULL) {
			ipc_buff(&b, ", \"node\": %u", focus->id);
			if (focus->client != NULL) {
				ipc_buff(&b, ", \"app_id\": ");
				json_str(&b, focus->client->app_id);
				ipc_buff(&b, ", \"title\": ");
				json_str(&b, focus->client->title);
			}
		}
		ipc_buff(&b, "},\n");
	} else {
		ipc_buff(&b, "  \"focused\": null,\n");
	}

	size_t views = 0, mapped = 0;
	view_t *view;
	wl_list_for_each(view, &server.views, link) {
		views++;
		if (view->mapped)
			mapped++;
	}

	ipc_buff(&b, "  \"counts\": {\"outputs\": %zu, \"desktops\": %zu, \"views\": %zu, "
		"\"mapped_views\": %zu, \"rules\": %zu, \"keybinds\": %zu},\n", (size_t)wl_list_length(&mon_list),
			count_desktops(), views, mapped, rule_count(), num_keybinds);

	transaction_stats_t txn;
	transaction_stats(&txn);
	ipc_buff(&b, "  \"transactions\": {\"commits\": %zu, \"timeouts\": %zu, \"pending\": %zu, "
		"\"waiting\": %zu}\n", txn.commits, txn.timeouts, txn.pending, txn.waiting);

	ipc_buff(&b, "}\n");

	return !b.truncated;
}

bool diag_status_refresh(void) {
	if (!status_render(status_snapshot, sizeof(status_snapshot)))
		return false;

	atomic_store_explicit(&status_snapshot_len, strlen(status_snapshot), memory_order_release);
	return true;
}

const char *diag_status_snapshot(size_t *len) {
	if (len != NULL)
		*len = atomic_load_explicit(&status_snapshot_len, memory_order_acquire);
	return status_snapshot;
}
