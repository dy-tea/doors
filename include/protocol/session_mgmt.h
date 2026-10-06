#pragma once

#include "types.h"

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_xdg_shell.h>

typedef struct {
	char output[SMALEN];
	char desktop[SMALEN];
} session_placement_t;

void session_management_init(void);
void session_management_fini(void);
struct wl_global *session_mgr_get_global(void);

void session_mgmt_handle_initial_commit(struct wlr_xdg_toplevel *xdg_toplevel);

// output and desktop a session wants this toplevel restored onto
void session_mgmt_get_placement(struct wlr_xdg_toplevel *xdg_toplevel, session_placement_t *out);

// apply state remembered for toplevel, runs once its node exists
void session_mgmt_handle_mapped(struct wlr_xdg_toplevel *xdg_toplevel);

// persist current state of toplevel if managed by a session
void session_mgmt_handle_state_changed(struct wlr_xdg_toplevel *xdg_toplevel);

// drop toplevel from any session managing it
void session_mgmt_handle_destroy(struct wlr_xdg_toplevel *xdg_toplevel);

// appends a description of every live session and what it remembers
size_t session_mgmt_write_list(char *buf, size_t buf_size);
