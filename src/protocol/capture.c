#include "layout/tree.h"
#include "output/output.h"
#include "protocol/capture.h"
#include "protocol/layer.h"
#include "server.h"
#include "types.h"
#include <drm_fourcc.h>
#include <math.h>
#include <wlr/render/pass.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>

static bool blocked_view_rect(const output_t *o, const view_t *view, struct wlr_box *logical) {
	if (!view->node || !view->node->client || !view->scene_tree)
		return false;

	client_t *c = view->node->client;
	if (!c->flags.block_out_from_screenshare)
		return false;
	if (!c->flags.shown && c->state != STATE_FULLSCREEN)
		return false;

	struct wlr_box win;
	switch (c->state) {
	case STATE_TILED:
	case STATE_PSEUDO_TILED:
		win = c->tiled_rectangle;
		break;
	case STATE_FLOATING:
		win = c->floating_rectangle;
		break;
	case STATE_FULLSCREEN:
		win = o->rectangle;
		break;
	default:
		return false;
	}

	if (win.width <= 0 || win.height <= 0)
		return false;

	// the scene node position is what actually gets rendered, the client rects
	// only provide the size
	int x, y;
	if (!wlr_scene_node_coords(&view->scene_tree->node, &x, &y))
		return false;

	const int border = effective_border_width(view->node->desktop);
	*logical = (struct wlr_box){
		.x = x - border - o->rectangle.x,
		.y = y - border - o->rectangle.y,
		.width = win.width + 2 * border,
		.height = win.height + 2 * border,
	};
	return true;
}

static bool blocked_layer_rect(const output_t *o, const layer_surface_t *layer,
		struct wlr_box *logical) {
	if (!layer || !layer->mapped || !layer->layer_surface || !layer->scene_tree)
		return false;
	if (!layer->scene_tree->node.enabled)
		return false;
	if (!(layer->rule.has & LAYER_RULE_BLOCK_OUT_FROM_SCREENSHARE) || !layer_rule_flag(&layer->rule,
		LAYER_RULE_BLOCK_OUT_FROM_SCREENSHARE))
		return false;

	const int width = (int)layer->layer_surface->current.actual_width;
	const int height = (int)layer->layer_surface->current.actual_height;
	if (width <= 0 || height <= 0)
		return false;

	int x, y;
	if (!wlr_scene_node_coords(&layer->scene_tree->node, &x, &y))
		return false;

	*logical = (struct wlr_box){
		.x = x - o->rectangle.x,
		.y = y - o->rectangle.y,
		.width = width,
		.height = height,
	};
	return true;
}

static bool append_block_out_rect(const struct wlr_output *output, const output_t *o,
		const int trans_width, const int trans_height, const struct wlr_box logical, struct wlr_box *rects,
		const size_t max, size_t *nrects) {
	if (*nrects >= max)
		return false;

	// the surface doesn't overlap this output
	if (logical.x + logical.width <= 0 || logical.y + logical.height <= 0 ||
		logical.x >= o->rectangle.width || logical.y >= o->rectangle.height)
		return true;

	// output logical coordinates -> buffer coordinates
	const double scale = output->scale;
	const double x0 = floor((double)logical.x * scale);
	const double y0 = floor((double)logical.y * scale);
	const double x1 = ceil((double)(logical.x + logical.width) * scale);
	const double y1 = ceil((double)(logical.y + logical.height) * scale);

	struct wlr_box buffer_box = {
		.x = (int)x0,
		.y = (int)y0,
		.width = (int)(x1 - x0),
		.height = (int)(y1 - y0),
	};
	wlr_box_transform(&buffer_box, &buffer_box, output->transform, trans_width, trans_height);

	const struct wlr_box bounds = {
		.x = 0,
		.y = 0,
		.width = output->width,
		.height = output->height,
	};
	struct wlr_box clipped;
	if (!wlr_box_intersection(&clipped, &buffer_box, &bounds))
		return true;
	if (clipped.width <= 0 || clipped.height <= 0)
		return true;

	rects[(*nrects)++] = clipped;
	return true;
}

size_t capture_block_out_rects(struct wlr_output *output, struct wlr_box *rects, const size_t max) {
	if (!output || !output->enabled || !output->renderer || max == 0)
		return 0;

	output_t *o = output_from_wlr_output(output);
	if (!o)
		return 0;

	if (output->width <= 0 || output->height <= 0)
		return 0;

	int trans_width, trans_height;
	wlr_output_transformed_resolution(output, &trans_width, &trans_height);

	size_t nrects = 0;

	view_t *view;
	wl_list_for_each(view, &server.views, link) {
		if (nrects >= max)
			break;

		struct wlr_box logical;
		if (!blocked_view_rect(o, view, &logical))
			continue;
		if (!append_block_out_rect(output, o, trans_width, trans_height, logical, rects, max, &nrects))
			break;
	}

	for (int i = 0; i < 4; i++) {
		layer_surface_t *layer;
		wl_list_for_each(layer, &o->layers[i], link) {
			if (nrects >= max)
				break;

			struct wlr_box logical;
			if (!blocked_layer_rect(o, layer, &logical))
				continue;
			if (!append_block_out_rect(output, o, trans_width, trans_height, logical, rects, max, &nrects))
				break;
		}
	}

	return nrects;
}

void capture_block_out_render(struct wlr_render_pass *pass, const struct wlr_box *rects,
		const size_t n) {
	for (size_t i = 0; i < n; i++) {
		wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
			.box = rects[i],
			.color = {0, 0, 0, 1},
			.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
		});
	}
}

void capture_block_out_fill_shm(void *data, const uint32_t format, const size_t stride,
		const uint32_t buffer_width, const uint32_t buffer_height, const struct wlr_box *rects,
		const size_t n) {
	if (!data || stride == 0)
		return;

	switch (format) {
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_XBGR8888:
		break;
	default:
		return;
	}

	for (size_t i = 0; i < n; i++) {
		const struct wlr_box *r = &rects[i];
		const int x0 = r->x < 0 ? 0 : r->x;
		const int y0 = r->y < 0 ? 0 : r->y;
		const int x1 = r->x + r->width > (int)buffer_width ? (int)buffer_width : r->x + r->width;
		const int y1 = r->y + r->height > (int)buffer_height ? (int)buffer_height : r->y + r->height;
		if (x1 <= x0 || y1 <= y0)
			continue;

		for (int y = y0; y < y1; y++) {
			uint32_t *row = (uint32_t *)((uint8_t *)data + (size_t)y * stride) + x0;
			for (int x = x0; x < x1; x++)
				*row++ = 0xff000000;
		}
	}
}
