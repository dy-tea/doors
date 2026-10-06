#pragma once

#include <pixman.h>
#include <wayland-server.h>
#include <wlr/render/pass.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/box.h>

#define CAPTURE_MAX_BLOCKED_RECTS 128

/**
 * Collects the rectangles, in output buffer coordinates, that have to be hidden
 * from screen capture because the toplevels covering them are configured with
 * block_out_from_screenshare.
 *
 * Returns the number of rectangles written to rects.
 */
size_t capture_block_out_rects(struct wlr_output *output, struct wlr_box *rects, size_t max);

/**
 * Draws opaque black rectangles into an ongoing render pass.
 */
void capture_block_out_render(struct wlr_render_pass *pass, const struct wlr_box *rects, size_t n);

/**
 * Fills opaque black into a shared memory buffer.
 */
void capture_block_out_fill_shm(void *data, uint32_t format, size_t stride, uint32_t buffer_width,
	uint32_t buffer_height, const struct wlr_box *rects, size_t n);
