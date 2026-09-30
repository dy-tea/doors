#pragma once

#include "surface.h"

#include <wayland-server-core.h>
#include <wayland-server.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_tearing_control_v1.h>
#include <wlr/util/box.h>

struct view_t {
	view_type_t type;
	const struct view_impl_t *impl;

	struct wl_list link; // server.views

	struct wlr_scene_tree *scene_tree; // Parent container
	struct wlr_scene_tree *content_tree; // Client content
	struct wlr_scene_tree *saved_surface_tree; // Saved buffer snapshot
	struct wlr_scene_buffer *output_handler; // For tracking output enter/leave events

	surface_blur_t *blur;
	surface_rounded_t *rounded;
	surface_shadow_t *shadow;

	struct wlr_ext_foreign_toplevel_handle_v1 *ext_foreign_toplevel;
	struct wlr_foreign_toplevel_handle_v1 *foreign_toplevel;
	char *foreign_identifier;

	struct wlr_ext_image_capture_source_v1 *image_capture_source;
	struct wlr_scene_surface *image_capture_surface;
	struct wlr_scene *image_capture;
	struct wlr_scene_tree *image_capture_tree;
	void *capture_renderer;

	struct wlr_scene_tree *border_tree;
	struct wlr_scene_rect *border_rects[4];

	node_t *node;
	client_t *client;

	struct wlr_box geometry;
	struct wlr_box last_requested;

	bool mapped, configured, wants_fade;
	int max_render_time;

	enum wp_tearing_control_v1_presentation_hint tearing_hint;

	struct wl_listener outputs_update;
};

typedef struct view_impl_t {
	void (*set_activated)(view_t *view, bool activated);
	void (*close)(view_t *view);
	void (*set_decorations)(view_t *view);
	void (*configure)(view_t *view, struct wlr_box rect);
} view_impl_t;

xdg_toplevel_t *view_to_xdg(view_t *view);
xwayland_toplevel_t *view_to_xwayland(view_t *view);

struct wlr_surface *view_wlr_surface(view_t *view);
bool view_is_ready(view_t *view);

view_t *view_from_wlr_surface(struct wlr_surface *surface);

void view_resolve_content_layout(view_t *view, struct wlr_box container,
	struct wlr_box *content_offset, struct wlr_box *border_size);

void view_center_and_clip_surface(view_t *view);
bool view_get_surface_offset(view_t *view, int *ox, int *oy);

void view_set_activated(view_t *view, bool activated);
void view_close(view_t *view);
void view_configure(view_t *view, struct wlr_box rect);

void view_set_title(view_t *view, const char *title);
void view_set_app_id(view_t *view, const char *app_id);

void view_create_foreign_toplevels(view_t *view, const char *app_id, const char *title);
void view_destroy_foreign_toplevels(view_t *view);

void view_save_buffer(view_t *view);
void view_remove_saved_buffer(view_t *view);
void view_send_frame_done(view_t *view);
void view_send_frame_done_iterator(struct wlr_scene_buffer *scene_buffer, int x, int y, void *data);
void view_freeze_sibling_buffers(desktop_t *d, node_t *n);

bool view_output_handler_point_accepts_input(struct wlr_scene_buffer *buffer, double *x, double *y);
void view_handle_outputs_update(struct wl_listener *listener, void *data);

bool view_init(view_t *view, view_type_t type);
void view_destroy(view_t *view);
void view_free_effects(view_t *view);
