#include "launcher.h"
#include "once.h"
#include "output.h"
#include "server.h"
#include "toplevel.h"
#include "tree.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_shell.h>

static pid_t get_parent_pid(pid_t child) {
	pid_t parent = -1;
	char file_name[100];
	char *buffer = NULL;
	const char *sep = " ";
	FILE *stat = NULL;
	size_t buf_size = 0;

	snprintf(file_name, sizeof(file_name), "/proc/%d/stat", child);

	if ((stat = fopen(file_name, "r"))) {
		if (getline(&buffer, &buf_size, stat) != -1) {
			strtok(buffer, sep);
			strtok(NULL, sep);
			strtok(NULL, sep);
			char *token = strtok(NULL, sep);
			parent = strtol(token, NULL, 10);
		}
		free(buffer);
		fclose(stat);
	}

	if (parent)
		return (parent == child) ? -1 : parent;

	return -1;
}

static void token_handle_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	launcher_ctx_t *ctx = wl_container_of(listener, ctx, token_destroy);
	wl_list_remove(&ctx->token_destroy.link);
	wl_list_remove(&ctx->seat_destroy.link);
	wl_list_remove(&ctx->link);
	free(ctx->desktop_name);
	free(ctx);
}

void launcher_ctx_consume(launcher_ctx_t *ctx) {
	wl_list_remove(&ctx->token_destroy.link);
	wl_list_init(&ctx->token_destroy.link);

	if (!ctx->activated)
		wlr_xdg_activation_token_v1_destroy(ctx->token);

	ctx->token = NULL;

	wl_list_remove(&ctx->link);
	wl_list_init(&ctx->link);
}

void launcher_ctx_destroy(launcher_ctx_t *ctx) {
	if (ctx == NULL)
		return;

	wl_list_remove(&ctx->token_destroy.link);
	wl_list_remove(&ctx->seat_destroy.link);
	wl_list_remove(&ctx->link);

	if (ctx->token)
		wlr_xdg_activation_token_v1_destroy(ctx->token);

	free(ctx->desktop_name);
	free(ctx);
}

launcher_ctx_t *launcher_ctx_find_pid(pid_t pid) {
	if (wl_list_empty(&server.pending_launcher_ctxs))
		return NULL;

	launcher_ctx_t *ctx = NULL;
	do {
		launcher_ctx_t *_ctx = NULL;
		wl_list_for_each(_ctx, &server.pending_launcher_ctxs, link) {
			if (pid == _ctx->pid) {
				ctx = _ctx;
				break;
			}
		}
		pid = get_parent_pid(pid);
	} while (pid > 1);

	return ctx;
}

launcher_ctx_t *launcher_ctx_create(struct wlr_xdg_activation_token_v1 *token,
		const char *desktop_name) {
	launcher_ctx_t *ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL)
		return NULL;

	ctx->desktop_name = strdup(desktop_name);
	if (!ctx->desktop_name) {
		wlr_log(WLR_ERROR, "allocation failed");
		free(ctx);
		return NULL;
	}
	ctx->token = token;
	ctx->had_focused_surface = token->surface != NULL;

	wl_list_init(&ctx->seat_destroy.link);

	ctx->token_destroy.notify = token_handle_destroy;
	wl_signal_add(&token->events.destroy, &ctx->token_destroy);

	wl_list_init(&ctx->link);
	wl_list_insert(&server.pending_launcher_ctxs, &ctx->link);

	token->data = ctx;
	return ctx;
}

const char *launcher_ctx_get_token_name(launcher_ctx_t *ctx) {
	return wlr_xdg_activation_token_v1_get_name(ctx->token);
}

launcher_ctx_t *launcher_ctx_create_internal(void) {
	const char *desktop_name = NULL;
	output_t *output = server.focused_output;
	if (output && output->desk)
		desktop_name = output->desk->name;

	if (desktop_name == NULL) {
		wlr_log(WLR_DEBUG, "xdg_activation: no focused desktop, skipping token");
		return NULL;
	}

	struct wlr_xdg_activation_token_v1 *token =
		wlr_xdg_activation_token_v1_create(server.xdg_activation_v1);
	if (token == NULL) {
		wlr_log(WLR_ERROR, "xdg_activation: failed to create activation token");
		return NULL;
	}

	launcher_ctx_t *ctx = launcher_ctx_create(token, desktop_name);
	if (ctx == NULL) {
		wlr_xdg_activation_token_v1_destroy(token);
		return NULL;
	}

	return ctx;
}

static void handle_xdg_activation_request_activate(struct wl_listener *listener, void *data) {
	(void)listener;
	const struct wlr_xdg_activation_v1_request_activate_event *event = data;

	if (event->surface == NULL)
		return;

	struct wlr_xdg_surface *xdg_surface = wlr_xdg_surface_try_from_wlr_surface(event->surface);
	if (xdg_surface == NULL) {
		wlr_log(WLR_DEBUG, "xdg_activation: surface is not an xdg surface");
		return;
	}

	launcher_ctx_t *ctx = event->token->data;

	if (!xdg_surface->surface->mapped) {
		if (ctx == NULL)
			return;
		if (ctx->activated)
			return;
		ctx->activated = true;
		wlr_log(WLR_DEBUG, "xdg_activation: startup notification for unmapped surface");
		return;
	}

	toplevel_t *toplevel = xdg_surface->data;
	if (toplevel == NULL)
		return;

	wlr_log(WLR_DEBUG, "xdg_activation: activating toplevel %p", (void *)toplevel);

	wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
	if (toplevel->node && toplevel->node->output && toplevel->node->desktop)
		activate_node(toplevel->node->output, toplevel->node->desktop, toplevel->node);
}

static void handle_xdg_activation_new_token(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_xdg_activation_token_v1 *token = data;

	const char *desktop_name = NULL;
	output_t *output = server.focused_output;
	if (output && output->desk)
		desktop_name = output->desk->name;

	if (desktop_name == NULL) {
		wlr_log(WLR_DEBUG, "xdg_activation: no focused desktop, skipping token");
		return;
	}

	launcher_ctx_t *ctx = launcher_ctx_create(token, desktop_name);
	if (ctx == NULL) {
		wlr_log(WLR_ERROR, "xdg_activation: failed to create launcher context");
		return;
	}

	wlr_log(WLR_DEBUG, "xdg_activation: new token for desktop '%s'", desktop_name);
}

// Ignored signals stay ignored through exec() and the signal mask is inherited too, programs
// that wait for their own children or write to pipes rely on the defaults, so every forked
// child gets them back (same as sway). SIGPIPE is ignored by the log setup.
static void restore_signals(void) {
	sigset_t set;
	sigemptyset(&set);
	sigprocmask(SIG_SETMASK, &set, NULL);

	struct sigaction sa_dfl = {.sa_handler = SIG_DFL};
	sigaction(SIGCHLD, &sa_dfl, NULL);
	sigaction(SIGPIPE, &sa_dfl, NULL);
}

void launcher_init(void) {
	ONCE();
	wl_list_init(&server.pending_launcher_ctxs);

	// Commands started by doors are children of the compositor and nothing waits for them.
	// Ignoring SIGCHLD makes the kernel reap them instead of leaving zombies behind.
	struct sigaction sa_ign = {.sa_handler = SIG_IGN};
	sigaction(SIGCHLD, &sa_ign, NULL);
	pthread_atfork(NULL, NULL, restore_signals);

	// xdg activation
	server.xdg_activation_v1 = wlr_xdg_activation_v1_create(server.wl_display);
	if (!server.xdg_activation_v1) {
		wlr_log(WLR_ERROR, "Failed to create xdg activation");
		exit(EXIT_FAILURE);
	}
	server.xdg_activation_request_activate.notify = handle_xdg_activation_request_activate;
	wl_signal_add(&server.xdg_activation_v1->events.request_activate,
		&server.xdg_activation_request_activate);
	server.xdg_activation_new_token.notify = handle_xdg_activation_new_token;
	wl_signal_add(&server.xdg_activation_v1->events.new_token, &server.xdg_activation_new_token);
}

void launcher_fini(void) {
	ONCE();
	wl_list_remove(&server.xdg_activation_request_activate.link);
	wl_list_remove(&server.xdg_activation_new_token.link);

	launcher_ctx_t *ctx, *tmp;
	wl_list_for_each_safe(ctx, tmp, &server.pending_launcher_ctxs, link)
		launcher_ctx_destroy(ctx);
}

void launcher_exec(const char *cmd) {
	if (!cmd || cmd[0] == '\0')
		return;

	bool no_startup_id = false;
	const char *cmd_str = cmd;

	if (strncmp(cmd_str, "--no-startup-id", 15) == 0 && (cmd_str[15] == ' ' || cmd_str[15] == '\t')) {
		no_startup_id = true;
		cmd_str += 15;
		while (*cmd_str == ' ' || *cmd_str == '\t')
			cmd_str++;
	}

	if (cmd_str[0] == '\0')
		return;

	wlr_log(WLR_DEBUG, "Executing %s", cmd_str);

	launcher_ctx_t *ctx = launcher_ctx_create_internal();

	pid_t child = fork();
	if (child == 0) {
		setsid();

		if (ctx) {
			const char *token = launcher_ctx_get_token_name(ctx);
			if (token) {
				setenv("XDG_ACTIVATION_TOKEN", token, 1);
				if (!no_startup_id)
					setenv("DESKTOP_STARTUP_ID", token, 1);
			}
		}

		execlp("sh", "sh", "-c", cmd_str, (void *)NULL);
		wlr_log(WLR_ERROR, "execlp failed: %s", strerror(errno));
		_exit(1);
	} else if (child < 0) {
		wlr_log(WLR_ERROR, "fork() failed: %s", strerror(errno));
		if (ctx)
			launcher_ctx_destroy(ctx);
		return;
	}

	if (ctx)
		ctx->pid = child;
}
