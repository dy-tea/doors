#include "ipc/args.h"
#include "ipc/ipc.h"
#include "ipc/registry.h"
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wlr/util/log.h>

static subscriber_t *make_subscriber(int client_fd, char *fifo_path, subscriber_mask_t mask,
		int count) {
	subscriber_t *sb = calloc(1, sizeof(*sb));
	if (!sb) {
		return NULL;
	}
	sb->client_fd = client_fd;
	sb->fifo_path = fifo_path;
	sb->mask = mask;
	sb->count = count;
	wl_list_init(&sb->link);
	sb->event_source = NULL;
	return sb;
}

void remove_subscriber(subscriber_t *sb) {
	if (!sb)
		return;

	wl_list_remove(&sb->link);

	if (sb->event_source) {
		wl_event_source_remove(sb->event_source);
	}

	close(sb->client_fd);
	if (sb->fifo_path) {
		unlink(sb->fifo_path);
		free(sb->fifo_path);
	}
	free(sb);
}

void add_subscriber(subscriber_t *sb) {
	wl_list_insert(subscriber_list.prev, &sb->link);

	int flags = fcntl(sb->client_fd, F_GETFD);
	fcntl(sb->client_fd, F_SETFD, flags & ~FD_CLOEXEC);

	if (sb->mask & SUB_MASK_REPORT) {
		if (!ipc_print_report(sb->client_fd)) {
			remove_subscriber(sb);
		} else if (sb->count > 0 && --sb->count == 0) {
			remove_subscriber(sb);
		}
	}
}


#define MONITOR_EVENTS \
	(SUB_MASK_MONITOR_ADD | SUB_MASK_MONITOR_REMOVE | SUB_MASK_MONITOR_FOCUS | \
		SUB_MASK_MONITOR_CHANGE)
#define DESKTOP_EVENTS \
	(SUB_MASK_DESKTOP_ADD | SUB_MASK_DESKTOP_REMOVE | SUB_MASK_DESKTOP_FOCUS | \
		SUB_MASK_DESKTOP_CHANGE | SUB_MASK_DESKTOP_LAYOUT)
#define NODE_EVENTS \
	(SUB_MASK_NODE_ADD | SUB_MASK_NODE_REMOVE | SUB_MASK_NODE_FOCUS | \
		SUB_MASK_NODE_CHANGE | SUB_MASK_NODE_STATE | SUB_MASK_NODE_FLAG)

#define NAMES1(a) a "\0"
#define NAMES2(a, b) a "\0" b "\0"

static const struct {
	const char *names; // NAMES1 or NAMES2
	subscriber_mask_t mask;
} event_words[] = {
	{NAMES2("report", "R"), SUB_MASK_REPORT},
	{NAMES2("monitor", "M"), MONITOR_EVENTS},
	{NAMES1("monitor_add"), SUB_MASK_MONITOR_ADD},
	{NAMES1("monitor_remove"), SUB_MASK_MONITOR_REMOVE},
	{NAMES1("monitor_focus"), SUB_MASK_MONITOR_FOCUS},
	{NAMES1("monitor_change"), SUB_MASK_MONITOR_CHANGE},
	{NAMES2("desktop", "D"), DESKTOP_EVENTS},
	{NAMES1("desktop_add"), SUB_MASK_DESKTOP_ADD},
	{NAMES1("desktop_remove"), SUB_MASK_DESKTOP_REMOVE},
	{NAMES1("desktop_focus"), SUB_MASK_DESKTOP_FOCUS},
	{NAMES1("desktop_change"), SUB_MASK_DESKTOP_CHANGE},
	{NAMES1("desktop_layout"), SUB_MASK_DESKTOP_LAYOUT},
	{NAMES2("node", "N"), NODE_EVENTS},
	{NAMES1("node_add"), SUB_MASK_NODE_ADD},
	{NAMES1("node_remove"), SUB_MASK_NODE_REMOVE},
	{NAMES1("node_focus"), SUB_MASK_NODE_FOCUS},
	{NAMES1("node_change"), SUB_MASK_NODE_CHANGE},
	{NAMES1("node_state"), SUB_MASK_NODE_STATE},
	{NAMES1("node_flag"), SUB_MASK_NODE_FLAG},
	{NAMES2("config", "config_changed"), SUB_MASK_CONFIG},
	{NAMES2("all", "A"), SUB_MASK_ALL},
};

bool subscribe_mask_for(const char *word, subscriber_mask_t *mask) {
	for (size_t i = 0; i < IPC_ARRAY_LEN(event_words); i++) {
		for (const char *name = event_words[i].names; *name; name += strlen(name) + 1) {
			if (streq(word, name)) {
				*mask |= event_words[i].mask;
				return true;
			}
		}
	}
	return false;
}

bool ipc_cmd_subscribe(ipc_args_t *a) {
	subscriber_mask_t mask = 0;
	int count = -1;
	char *fifo_path = NULL;
	bool explicit_fifo = false;

	while (ipc_peek(a)) {
		const char *opt = ipc_take(a);

		if (streq(opt, "-c") || streq(opt, "--count")) {
			if (!ipc_int(a, "count", 1, INT_MAX, &count))
				return false;
		} else if (streq(opt, "-f") || streq(opt, "--fifo")) {
			explicit_fifo = true;
		} else if (subscribe_mask_for(opt, &mask)) {
			// handled by the table
		} else {
			ipc_fail(a, "Unknown argument \"%s\"\n", opt);
			return false;
		}
	}

	if (mask == 0)
		mask = SUB_MASK_REPORT;

	if (!explicit_fifo) {
		char template[] = "/tmp/doors_fifo_XXXXXX";
		int fd = mkstemp(template);
		if (fd < 0) {
			ipc_fail(a, "Failed to create fifo path\n");
			return false;
		}
		close(fd);
		unlink(template);
		fifo_path = strdup(template);
		if (!fifo_path) {
			ipc_fail(a, "Memory error\n");
			return false;
		}
		if (mkfifo(fifo_path, 0666) == -1) {
			free(fifo_path);
			fifo_path = NULL;
		}
	}

	int client_fd = a->fd;

	if (fifo_path) {
		int fifo_fd = open(fifo_path, O_RDWR);
		if (fifo_fd < 0) {
			free(fifo_path);
			ipc_fail(a, "Failed to open fifo\n");
			return false;
		}

		// hand the fifo path back as the reply, then stream into the fifo
		char reply[DOORS_BUFSIZ];
		snprintf(reply, sizeof(reply), "%s\n", fifo_path);
		send_success(client_fd, reply);
		close(client_fd);
		client_fd = fifo_fd;
	}

	subscriber_t *sb = make_subscriber(client_fd, fifo_path, mask, count);
	if (!sb) {
		if (fifo_path)
			free(fifo_path);
		ipc_fail(a, "Failed to create subscriber\n");
		return false;
	}

	add_subscriber(sb);
	return true;
}
