#include "config.h"
#include "config_internal.h"
#include "ipc/ipc.h"
#include "str.h"
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/util/log.h>

#define WATCH_PATH_MAX (PATH_MAX + 16)

static int hotkey_watch_fd = -1;
static char hotkey_config_path[WATCH_PATH_MAX];
static struct wl_event_loop *hotkey_event_loop = NULL;
static struct wl_event_source *hotkey_event_source = NULL;
static struct wl_event_source *doorsrc_debounce_timer = NULL;
static struct wl_event_source *hotkey_debounce_timer = NULL;

static int hotkey_reload_handler(int fd, uint32_t mask, void *data);

// whether an inotify event name is the file at `path`
static bool file_event_matches(const char *path, const char *name) {
	if (path[0] == '\0' || name == NULL || name[0] == '\0')
		return false;

	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;

	return streq(base, name);
}

static void add_hotkey_listener_to_event_loop(void) {
	if (hotkey_event_source) {
		wl_event_source_remove(hotkey_event_source);
		hotkey_event_source = NULL;
	}

	if (hotkey_event_loop && hotkey_watch_fd >= 0)
		hotkey_event_source = wl_event_loop_add_fd(hotkey_event_loop, hotkey_watch_fd, WL_EVENT_READABLE,
			hotkey_reload_handler, NULL);
}

static void setup_inotify_watch(const char *path) {
	if (hotkey_event_source) {
		wl_event_source_remove(hotkey_event_source);
		hotkey_event_source = NULL;
	}

	if (hotkey_watch_fd >= 0) {
		close(hotkey_watch_fd);
		hotkey_watch_fd = -1;
	}

	hotkey_watch_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (hotkey_watch_fd < 0) {
		wlr_log(WLR_ERROR, "Failed to initialize inotify");
		return;
	}

	str_copy(hotkey_config_path, sizeof(hotkey_config_path), path);

	char dir_path[WATCH_PATH_MAX];
	str_copy(dir_path, sizeof(dir_path), path);
	char *last_slash = strrchr(dir_path, '/');
	const uint32_t mask = IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE;
	if (last_slash) {
		*last_slash = '\0';
		inotify_add_watch(hotkey_watch_fd, dir_path, mask);
	} else {
		inotify_add_watch(hotkey_watch_fd, ".", mask);
	}

	add_hotkey_listener_to_event_loop();
}

void reload_hotkeys(void) {
	if (hotkey_config_path[0] != '\0') {
		wlr_log(WLR_INFO, "Reloading hotkeys from %s", hotkey_config_path);
		load_hotkeys(hotkey_config_path);
	}
}

static int hotkey_debounce_handler(void *data) {
	(void)data;
	reload_hotkeys();
	hotkey_debounce_timer = NULL;
	return 0;
}

static int doorsrc_debounce_handler(void *data) {
	(void)data;
	doorsrc_debounce_timer = NULL;

	ipc_put_status(SUB_MASK_CONFIG, "config_changed %s\n", config_doorsrc_path());
	return 0;
}

static int hotkey_reload_handler(int fd, uint32_t mask, void *data) {
	(void)data;
	if (!(mask & WL_EVENT_READABLE))
		return 0;

	char buf[sizeof(struct inotify_event) + NAME_MAX + 1];
	const ssize_t len = read(fd, buf, sizeof(buf));
	if (len <= 0)
		return 0;

	bool doorsrc = false, hotkeys = false;
	for (ssize_t offset = 0; offset + (ssize_t)sizeof(struct inotify_event) <= len; ) {
		struct inotify_event *event = (struct inotify_event *)(buf + offset);

		if (event->len > 0 && event->name[0] != '\0') {
			doorsrc |= file_event_matches(config_doorsrc_path(), event->name);
			hotkeys |= file_event_matches(hotkey_config_path, event->name);
		} else {
			// an event on the directory itself
			hotkeys = true;
		}

		offset += (ssize_t)(sizeof(struct inotify_event) + event->len);
	}

	if (doorsrc) {
		if (doorsrc_debounce_timer) {
			wl_event_source_timer_update(doorsrc_debounce_timer, 100);
		} else if (hotkey_event_loop) {
			doorsrc_debounce_timer = wl_event_loop_add_timer(hotkey_event_loop, doorsrc_debounce_handler,
				NULL);
			if (doorsrc_debounce_timer)
				wl_event_source_timer_update(doorsrc_debounce_timer, 100);
		}
	}

	if (!hotkeys)
		return 0;

	if (hotkey_debounce_timer) {
		wl_event_source_timer_update(hotkey_debounce_timer, 100);
	} else if (hotkey_event_loop) {
		hotkey_debounce_timer = wl_event_loop_add_timer(hotkey_event_loop, hotkey_debounce_handler, NULL);
		if (hotkey_debounce_timer)
			wl_event_source_timer_update(hotkey_debounce_timer, 100);
	}
	return 0;
}

void load_hotkeys_idle(void *data) {
	(void)data;
	const char *path = config_hotkeys_path();
	if (path[0] == '\0')
		return;

	load_hotkeys(path);
	setup_inotify_watch(path);
}

void setup_hotkey_event_listener(struct wl_event_loop *event_loop) {
	hotkey_event_loop = event_loop;
	add_hotkey_listener_to_event_loop();
}

void hotkey_watch_fini(void) {
	if (hotkey_event_source) {
		wl_event_source_remove(hotkey_event_source);
		hotkey_event_source = NULL;
	}
	if (doorsrc_debounce_timer) {
		wl_event_source_remove(doorsrc_debounce_timer);
		doorsrc_debounce_timer = NULL;
	}
	if (hotkey_debounce_timer) {
		wl_event_source_remove(hotkey_debounce_timer);
		hotkey_debounce_timer = NULL;
	}
	if (hotkey_watch_fd >= 0) {
		close(hotkey_watch_fd);
		hotkey_watch_fd = -1;
	}
	hotkey_config_path[0] = '\0';
}
