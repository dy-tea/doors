#include "layout/tree.h"
#include "once.h"
#include "output/output.h"
#include "protocol/capture.h"
#include "server.h"
#include "types.h"
#include "view.h"
#include <drm_fourcc.h>
#include <math.h>
#include <wlr/render/pass.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>
#include <wlr/util/transform.h>

static bool blocked_view_rect(output_t *o, view_t *view, struct wlr_box *logical) {
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

	int border = effective_border_width(view->node->desktop);
	*logical = (struct wlr_box){
		.x = x - border - o->rectangle.x,
		.y = y - border - o->rectangle.y,
		.width = win.width + 2 * border,
		.height = win.height + 2 * border,
	};
	return true;
}

size_t capture_block_out_rects(struct wlr_output *output, struct wlr_box *rects, size_t max) {
	if (!output || !output->enabled || !output->renderer || max == 0)
		return 0;

	output_t *o = output_from_wlr_output(output);
	if (!o)
		return 0;

	int buffer_width = output->width;
	int buffer_height = output->height;
	if (buffer_width <= 0 || buffer_height <= 0)
		return 0;

	int trans_width, trans_height;
	wlr_output_transformed_resolution(output, &trans_width, &trans_height);

	float scale = output->scale;
	size_t nrects = 0;

	view_t *view;
	wl_list_for_each(view, &server.views, link) {
		if (nrects >= max)
			break;

		struct wlr_box logical;
		if (!blocked_view_rect(o, view, &logical))
			continue;

		// the window doesn't overlap this output
		if (logical.x + logical.width <= 0 || logical.y + logical.height <= 0 ||
			logical.x >= o->rectangle.width || logical.y >= o->rectangle.height)
			continue;

		// output logical coordinates -> buffer coordinates
		double x0 = floor((double)logical.x * scale);
		double y0 = floor((double)logical.y * scale);
		double x1 = ceil((double)(logical.x + logical.width) * scale);
		double y1 = ceil((double)(logical.y + logical.height) * scale);

		struct wlr_box buffer_box = {
			.x = (int)x0,
			.y = (int)y0,
			.width = (int)(x1 - x0),
			.height = (int)(y1 - y0),
		};
		wlr_box_transform(&buffer_box, &buffer_box, output->transform, trans_width, trans_height);

		struct wlr_box bounds = {
			0,
			0,
			buffer_width,
			buffer_height
		};
		if (!wlr_box_intersection(&rects[nrects], &buffer_box, &bounds))
			continue;
		if (rects[nrects].width <= 0 || rects[nrects].height <= 0)
			continue;

		nrects++;
	}

	return nrects;
}

void capture_block_out_render(struct wlr_render_pass *pass, const struct wlr_box *rects, size_t n) {
	for (size_t i = 0; i < n; i++) {
		wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
			.box = rects[i],
			.color = {0, 0, 0, 1},
			.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
		});
	}
}

void capture_block_out_fill_shm(void *data, uint32_t format, size_t stride, uint32_t buffer_width,
		uint32_t buffer_height, const struct wlr_box *rects, size_t n) {
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
		int x0 = r->x < 0 ? 0 : r->x;
		int y0 = r->y < 0 ? 0 : r->y;
		int x1 = r->x + r->width > (int)buffer_width ? (int)buffer_width : r->x + r->width;
		int y1 = r->y + r->height > (int)buffer_height ? (int)buffer_height : r->y + r->height;
		if (x1 <= x0 || y1 <= y0)
			continue;

		for (int y = y0; y < y1; y++) {
			uint32_t *row = (uint32_t *)((uint8_t *)data + (size_t)y * stride) + x0;
			for (int x = x0; x < x1; x++)
				*row++ = 0xff000000;
		}
	}
}
