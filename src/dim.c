#include "client.h"
#include "dim.h"
#include "layout/layout.h"
#include "layout/tree.h"
#include "output/output.h"
#include "server.h"
#include "settings.h"
#include "tree.h"
#include "types.h"
#include <cairo.h>
#include <drm_fourcc.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-util.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#define DIM_FULL_PI 3.14159265358979323846

static void dim_get_color(float out[4]) {
	float alpha = settings.dim_color[3] * settings.dim_strength;
	for (int i = 0; i < 3; i++)
		out[i] = settings.dim_color[i] * alpha;
	out[3] = alpha;
}

typedef struct {
	struct wlr_buffer base;
	cairo_surface_t *surface;
} dim_buffer_t;

static void dim_buffer_destroy(struct wlr_buffer *wlr_buffer) {
	dim_buffer_t *buffer = wl_container_of(wlr_buffer, buffer, base);
	cairo_surface_destroy(buffer->surface);
	free(buffer);
}

static bool dim_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer, uint32_t flags,
		void **data, uint32_t *format, size_t *stride) {
	(void)flags;
	dim_buffer_t *buffer = wl_container_of(wlr_buffer, buffer, base);
	*data = cairo_image_surface_get_data(buffer->surface);
	*stride = cairo_image_surface_get_stride(buffer->surface);
	*format = DRM_FORMAT_ARGB8888;
	return true;
}

static void dim_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
	(void)wlr_buffer;
}

static const struct wlr_buffer_impl dim_buffer_impl = {
	.destroy = dim_buffer_destroy,
	.begin_data_ptr_access = dim_buffer_begin_data_ptr_access,
	.end_data_ptr_access = dim_buffer_end_data_ptr_access,
};

static void dim_track_path(cairo_t *cairo, double x, double y, double width, double height,
		double radius) {
	if (radius <= 0.0) {
		cairo_rectangle(cairo, x, y, width, height);
		return;
	}

	double half = (width < height ? width : height) * 0.5;
	if (radius > half)
		radius = half;

	cairo_new_sub_path(cairo);
	cairo_arc(cairo, x + radius, y + radius, radius, DIM_FULL_PI, 1.5 * DIM_FULL_PI);
	cairo_arc(cairo, x + width - radius, y + radius, radius, 1.5 * DIM_FULL_PI, 2.0 * DIM_FULL_PI);
	cairo_arc(cairo, x + width - radius, y + height - radius, radius, 0.0, 0.5 * DIM_FULL_PI);
	cairo_arc(cairo, x + radius, y + height - radius, radius, 0.5 * DIM_FULL_PI, DIM_FULL_PI);
	cairo_close_path(cairo);
}

static struct wlr_buffer *dim_buffer_create(const float color[4], int width, int height,
		double radius) {
	if (width <= 0 || height <= 0)
		return NULL;

	bool rounded = (radius > 0.0);
	int buf_w = rounded ? width : 1;
	int buf_h = rounded ? height : 1;

	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, buf_w, buf_h);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		return NULL;
	}

	cairo_t *cairo = cairo_create(surface);
	if (cairo == NULL) {
		cairo_surface_destroy(surface);
		return NULL;
	}

	cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cairo, color[0], color[1], color[2], color[3]);
	if (rounded) {
		dim_track_path(cairo, 0.0, 0.0, (double)buf_w, (double)buf_h, radius);
		cairo_fill(cairo);
	} else {
		cairo_paint(cairo);
	}
	cairo_surface_flush(surface);
	cairo_destroy(cairo);

	dim_buffer_t *buffer = calloc(1, sizeof(*buffer));
	if (buffer == NULL) {
		cairo_surface_destroy(surface);
		return NULL;
	}
	buffer->surface = surface;

	wlr_buffer_init(&buffer->base, &dim_buffer_impl, buf_w, buf_h);
	return &buffer->base;
}

static bool dim_no_input(struct wlr_scene_buffer *buffer, double *sx, double *sy) {
	(void)buffer;
	(void)sx;
	(void)sy;
	return false;
}

static struct dim_scrim_t *dim_ensure(struct dim_scrim_t **slot, struct wlr_scene_tree *parent) {
	if (*slot != NULL)
		return *slot;

	struct dim_scrim_t *scrim = calloc(1, sizeof(*scrim));
	if (scrim == NULL)
		return NULL;

	scrim->node = wlr_scene_buffer_create(parent, NULL);
	if (scrim->node == NULL) {
		wlr_log(WLR_ERROR, "Failed to create dim scrim");
		free(scrim);
		return NULL;
	}
	scrim->node->point_accepts_input = dim_no_input;

	*slot = scrim;
	return scrim;
}

static int dim_px(double value) {
	return (int)(value >= 0.0 ? value + 0.5 : value - 0.5);
}

static void dim_apply_scrim(struct dim_scrim_t *scrim, const float color[4], int buf_w, int buf_h,
		float radius) {
	if (scrim->has_color && buf_w == scrim->buf_width && buf_h == scrim->buf_height &&
		radius == scrim->radius && memcmp(scrim->color, color, sizeof(scrim->color)) == 0)
		return;

	struct wlr_buffer *buffer = dim_buffer_create(color, buf_w, buf_h, (double)radius);
	if (buffer == NULL) {
		wlr_log(WLR_ERROR, "Failed to create dim buffer");
		return;
	}

	memcpy(scrim->color, color, sizeof(scrim->color));
	scrim->buf_width = buf_w;
	scrim->buf_height = buf_h;
	scrim->radius = radius;
	scrim->has_color = true;

	wlr_scene_buffer_set_buffer(scrim->node, buffer);
	wlr_buffer_drop(buffer);
}

static void dim_place_before(struct wlr_scene_node *node, struct wlr_scene_node *sibling) {
	if (node->parent != sibling->parent)
		wlr_scene_node_reparent(node, sibling->parent);

	struct wl_list *elm = &node->link;
	struct wl_list *pos = &sibling->link;

	wl_list_remove(elm);
	elm->prev = pos->prev;
	elm->next = pos;
	pos->prev->next = elm;
	pos->prev = elm;
}

static void dim_scrim_reset(struct dim_scrim_t *scrim) {
	if (scrim != NULL)
		wlr_scene_node_set_enabled(&scrim->node->node, false);
}

static bool dim_can_dim(view_t *view) {
	if (view == NULL || view->client == NULL || view->scene_tree == NULL)
		return false;
	if (!view->mapped || view->client->flags.freed)
		return false;

	return !view->client->flags.no_dim;
}

static bool view_occupied_box(view_t *view, struct wlr_box *out) {
	client_t *client = view->client;

	if (client->state == STATE_FULLSCREEN) {
		output_t *m = view->node ? view->node->output : NULL;
		if (m == NULL)
			return false;
		*out = m->rectangle;
		return out->width > 0 && out->height > 0;
	}

	// floating and minimized toplevels keep their own rectangle
	*out = IS_FLOATING(client) ||
		client->flags.minimized ? client->floating_rectangle : client->tiled_rectangle;

	return out->width > 0 && out->height > 0;
}

static bool view_dim_box(view_t *view, struct wlr_box *out) {
	if (!view_occupied_box(view, out))
		return false;

	int off_x = 0, off_y = 0;
	if (view->geometry.width > 0 && view->geometry.height > 0) {
		int geo_w = (int)view->geometry.width;
		int geo_h = (int)view->geometry.height;

		if (geo_w < out->width) {
			off_x = (out->width - geo_w) / 2;
			out->width = geo_w;
		}
		if (geo_h < out->height) {
			off_y = (out->height - geo_h) / 2;
			out->height = geo_h;
		}
	}

	out->x = off_x;
	out->y = off_y;
	return out->width > 0 && out->height > 0;
}

static float dim_view_radius(view_t *view) {
	client_t *c = view->client;
	if (c == NULL || c->state == STATE_FULLSCREEN || view->node == NULL)
		return 0.0f;

	int bw = effective_border_width(view->node->desktop);
	if (c->border_radius <= (float)bw)
		return 0.0f;

	return c->border_radius - (float)bw;
}

void dim_update_view(view_t *view) {
	if (view == NULL || view->scene_tree == NULL)
		return;

	struct dim_scrim_t *scrim = view->dim_node;

	if (!settings.dim_below || !dim_can_dim(view)) {
		dim_scrim_reset(scrim);
		return;
	}

	struct wlr_box box;
	if (!view_dim_box(view, &box)) {
		dim_scrim_reset(scrim);
		return;
	}

	scrim = dim_ensure(&view->dim_node, view->scene_tree);
	if (scrim == NULL)
		return;

	float color[4];
	dim_get_color(color);

	float scale = view->node && view->node->output ? view->node->output->wlr_output->scale : 1.0f;
	float radius = dim_view_radius(view);

	dim_apply_scrim(scrim, color, dim_px((double)box.width * scale), dim_px((double)box.height * scale),
		radius * scale);

	wlr_scene_node_set_position(&scrim->node->node, box.x, box.y);
	wlr_scene_buffer_set_dest_size(scrim->node, box.width, box.height);
	dim_place_before(&scrim->node->node, &view->content_tree->node);
	wlr_scene_node_set_enabled(&scrim->node->node, true);
}

void dim_destroy_view(view_t *view) {
	if (view == NULL || view->dim_node == NULL)
		return;

	wlr_scene_node_destroy(&view->dim_node->node->node);
	free(view->dim_node);
	view->dim_node = NULL;
}

void dim_update_output(output_t *output) {
	if (output == NULL)
		return;

	struct dim_scrim_t *scrim = output->dim_node;

	view_t *focused = NULL;
	if (output->desk != NULL && output->desk->focus != NULL && output->desk->focus->client != NULL)
		focused = output->desk->focus->client->view;

	if (!settings.dim_inactive || !dim_can_dim(focused)) {
		dim_scrim_reset(scrim);
		return;
	}

	struct wlr_scene_tree *parent = focused->scene_tree->node.parent;
	if (parent == NULL) {
		dim_scrim_reset(scrim);
		return;
	}

	const struct wlr_box *rect = &output->rectangle;
	if (rect->width <= 0 || rect->height <= 0) {
		dim_scrim_reset(scrim);
		return;
	}

	scrim = dim_ensure(&output->dim_node, parent);
	if (scrim == NULL)
		return;

	float color[4];
	dim_get_color(color);

	dim_apply_scrim(scrim, color, 1, 1, 0.0f);

	wlr_scene_node_set_position(&scrim->node->node, rect->x, rect->y);
	wlr_scene_buffer_set_dest_size(scrim->node, rect->width, rect->height);
	dim_place_before(&scrim->node->node, &focused->scene_tree->node);
	wlr_scene_node_set_enabled(&scrim->node->node, true);
}

bool dim_scrim_below_view(const view_t *view, const output_t *output) {
	if (view == NULL || output == NULL || !settings.dim_inactive)
		return false;

	desktop_t *desk = output->desk;
	if (desk == NULL || desk->focus == NULL || desk->focus->client == NULL)
		return false;

	if (desk->focus->client->view != view)
		return false;

	return dim_can_dim((view_t *)view);
}

void dim_destroy_output(output_t *output) {
	if (output == NULL || output->dim_node == NULL)
		return;

	wlr_scene_node_destroy(&output->dim_node->node->node);
	free(output->dim_node);
	output->dim_node = NULL;
}

void dim_update(void) {
	output_t *m;
	wl_list_for_each(m, &mon_list, link)
		dim_update_output(m);

	view_t *view;
	wl_list_for_each(view, &server.views, link)
		dim_update_view(view);
}

bool dim_output_visible(const output_t *output) {
	return output != NULL && output->dim_node != NULL && output->dim_node->node->node.enabled;
}
