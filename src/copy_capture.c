#include "copy_capture.h"
#include "server.h"
#include "output.h"
#include "toplevel.h"
#include "types.h"
#include "xwayland.h"
#include <assert.h>
#include <drm_fourcc.h>
#include <wayland-server-protocol.h>
#include <wlr/interfaces/wlr_ext_image_capture_source_v1.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/render/pass.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_damage_ring.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>
#include "ext-image-capture-source-v1-protocol.h"
#include "ext-image-copy-capture-v1-protocol.h"

struct bwm_image_copy_source {
	struct wlr_ext_image_capture_source_v1 base;
	struct wlr_output *output;
	struct wlr_buffer *last_buffer;
	struct wl_listener output_commit;
	struct wl_listener output_destroy;
	struct wl_listener base_destroy;
};

static const struct wlr_ext_image_capture_source_v1_interface source_impl;

static struct bwm_image_copy_source *source_from_base(struct wlr_ext_image_capture_source_v1 *base) {
	return (struct bwm_image_copy_source *)((char *)base - offsetof(struct bwm_image_copy_source, base));
}

static void output_source_destroy_internal(struct bwm_image_copy_source *src) {
	if (!src)
		return;
	wlr_ext_image_capture_source_v1_finish(&src->base);
	wl_list_remove(&src->output_commit.link);
	wl_list_remove(&src->output_destroy.link);
	wl_list_remove(&src->base_destroy.link);
	if (src->last_buffer)
		wlr_buffer_unlock(src->last_buffer);
	free(src->base.shm_formats);
	free(src);
}

static void output_source_start(struct wlr_ext_image_capture_source_v1 *base,
		bool with_cursors) {
	(void)base;
	(void)with_cursors;
}

static void output_source_stop(struct wlr_ext_image_capture_source_v1 *base) {
	(void)base;
}

static void output_source_request_frame(
		struct wlr_ext_image_capture_source_v1 *base, bool schedule_frame) {
	(void)base;
	(void)schedule_frame;
}

static void block_windows(struct bwm_image_copy_source *src,
		struct wlr_render_pass *pass, float scale) {
	struct bwm_output *bwm_output = output_from_wlr_output(src->output);
	if (!bwm_output)
		return;

	struct bwm_toplevel *tl;
	wl_list_for_each(tl, &server.toplevels, link) {
		if (!tl->node || !tl->node->client)
			continue;
		client_t *c = tl->node->client;
		if (!c->block_out_from_screenshare)
			continue;
		if (!c->shown && c->state != STATE_FULLSCREEN)
			continue;

		struct wlr_box win_rect = {0};
		switch (c->state) {
		case STATE_TILED:
		case STATE_PSEUDO_TILED:
			win_rect = c->tiled_rectangle;
			break;
		case STATE_FLOATING:
			win_rect = c->floating_rectangle;
			break;
		case STATE_FULLSCREEN:
			win_rect = bwm_output->rectangle;
			break;
		}
		if (win_rect.width == 0 || win_rect.height == 0)
			continue;

		int abs_x, abs_y;
		wlr_scene_node_coords(&tl->scene_tree->node, &abs_x, &abs_y);

		int bw = (int)border_width;
		struct wlr_box block_box = {
			.x = (int)((abs_x - bwm_output->rectangle.x - bw) * scale),
			.y = (int)((abs_y - bwm_output->rectangle.y - bw) * scale),
			.width = (int)((win_rect.width + 2 * bw) * scale),
			.height = (int)((win_rect.height + 2 * bw) * scale),
		};

		wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
			.box = block_box,
			.color = { 0, 0, 0, 1 },
			.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
		});
	}

	struct bwm_xwayland_view *xw;
	wl_list_for_each(xw, &server.xwayland.views, link) {
		if (!xw->node || !xw->node->client)
			continue;
		client_t *c = xw->node->client;
		if (!c->block_out_from_screenshare)
			continue;
		if (!c->shown && c->state != STATE_FULLSCREEN)
			continue;

		struct wlr_box win_rect = {0};
		switch (c->state) {
		case STATE_TILED:
		case STATE_PSEUDO_TILED:
			win_rect = c->tiled_rectangle;
			break;
		case STATE_FLOATING:
			win_rect = c->floating_rectangle;
			break;
		case STATE_FULLSCREEN:
			win_rect = bwm_output->rectangle;
			break;
		}
		if (win_rect.width == 0 || win_rect.height == 0)
			continue;

		int abs_x, abs_y;
		wlr_scene_node_coords(&xw->scene_tree->node, &abs_x, &abs_y);

		int bw = (int)border_width;
		struct wlr_box block_box = {
			.x = (int)((abs_x - bwm_output->rectangle.x - bw) * scale),
			.y = (int)((abs_y - bwm_output->rectangle.y - bw) * scale),
			.width = (int)((win_rect.width + 2 * bw) * scale),
			.height = (int)((win_rect.height + 2 * bw) * scale),
		};

		wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
			.box = block_box,
			.color = { 0, 0, 0, 1 },
			.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
		});
	}
}

static void output_source_copy_frame(struct wlr_ext_image_capture_source_v1 *base,
		struct wlr_ext_image_copy_capture_frame_v1 *dst_frame,
		struct wlr_ext_image_capture_source_v1_frame_event *frame_event) {
	(void)base;
	(void)dst_frame;
	(void)frame_event;
}

static struct wlr_ext_image_capture_source_v1_cursor *output_source_get_pointer_cursor(
		struct wlr_ext_image_capture_source_v1 *base,
		struct wlr_seat *seat) {
	(void)base;
	(void)seat;
	return NULL;
}

static const struct wlr_ext_image_capture_source_v1_interface source_impl = {
	.start = output_source_start,
	.stop = output_source_stop,
	.request_frame = output_source_request_frame,
	.copy_frame = output_source_copy_frame,
	.get_pointer_cursor = output_source_get_pointer_cursor,
};

static void source_handle_output_commit(struct wl_listener *listener, void *data) {
	struct bwm_image_copy_source *src =
		wl_container_of(listener, src, output_commit);
	struct wlr_output_event_commit *event = data;
	struct wlr_output *output = src->output;

	if ((event->state->committed & WLR_OUTPUT_STATE_ENABLED) && !output->enabled)
		return;
	if (!(event->state->committed & WLR_OUTPUT_STATE_BUFFER))
		return;

	if (src->last_buffer)
		wlr_buffer_unlock(src->last_buffer);
	src->last_buffer = event->state->buffer;
	wlr_buffer_lock(src->last_buffer);

	pixman_region32_t damage;
	pixman_region32_init(&damage);
	pixman_region32_copy(&damage, &event->state->damage);

	struct wlr_ext_image_capture_source_v1_frame_event frame_event = {
		.damage = &damage,
	};
	wl_signal_emit_mutable(&src->base.events.frame, &frame_event);

	pixman_region32_fini(&damage);
}

static void source_handle_output_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct bwm_image_copy_source *src =
		wl_container_of(listener, src, output_destroy);
	wl_list_remove(&src->output_commit.link);
	wl_list_remove(&src->output_destroy.link);
	if (src->last_buffer) {
		wlr_buffer_unlock(src->last_buffer);
		src->last_buffer = NULL;
	}
	src->output = NULL;
}

static void source_handle_base_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct bwm_image_copy_source *src = wl_container_of(listener, src, base_destroy);
	src->base_destroy.notify = NULL;
	output_source_destroy_internal(src);
}

static struct bwm_image_copy_source *create_output_source(
		struct wlr_output *wlr_output) {
	struct bwm_image_copy_source *src = calloc(1, sizeof(*src));
	if (!src)
		return NULL;

	wlr_ext_image_capture_source_v1_init(&src->base, &source_impl);

	int ow, oh;
	wlr_output_transformed_resolution(wlr_output, &ow, &oh);
	src->base.width = (uint32_t)ow;
	src->base.height = (uint32_t)oh;

	static const uint32_t shm_formats[] = { DRM_FORMAT_ARGB8888 };
	src->base.shm_formats = calloc(1, sizeof(shm_formats));
	if (src->base.shm_formats)
		memcpy(src->base.shm_formats, shm_formats, sizeof(shm_formats));
	src->base.shm_formats_len = 1;

	src->base.dmabuf_device = 0;

	src->output = wlr_output;

	src->output_commit.notify = source_handle_output_commit;
	wl_signal_add(&wlr_output->events.commit, &src->output_commit);
	src->output_destroy.notify = source_handle_output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &src->output_destroy);

	src->base_destroy.notify = source_handle_base_destroy;
	wl_signal_add(&src->base.events.destroy, &src->base_destroy);

	return src;
}

struct bwm_output_capture_mgr {
	struct wl_global *global;
	struct wl_listener display_destroy;
};

static void output_mgr_handle_create_source(struct wl_client *wl_client,
		struct wl_resource *mgr_resource, uint32_t id,
		struct wl_resource *output_resource) {
	(void)mgr_resource;
	struct wlr_output *wlr_output = wlr_output_from_resource(output_resource);
	if (!wlr_output)
		return;

	struct bwm_image_copy_source *src = create_output_source(wlr_output);
	if (!src) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	if (!wlr_ext_image_capture_source_v1_create_resource(&src->base,
			wl_client, id)) {
		output_source_destroy_internal(src);
		wl_client_post_no_memory(wl_client);
		return;
	}
}

static void output_mgr_handle_destroy(struct wl_client *wl_client,
		struct wl_resource *mgr_resource) {
	(void)wl_client;
	wl_resource_destroy(mgr_resource);
}

static const struct ext_output_image_capture_source_manager_v1_interface output_mgr_impl = {
	.create_source = output_mgr_handle_create_source,
	.destroy = output_mgr_handle_destroy,
};

static void output_mgr_bind(struct wl_client *wl_client, void *data,
		uint32_t version, uint32_t id) {
	struct bwm_output_capture_mgr *mgr = data;
	struct wl_resource *resource = wl_resource_create(wl_client,
		&ext_output_image_capture_source_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	wl_resource_set_implementation(resource, &output_mgr_impl, mgr, NULL);
}

static void output_mgr_display_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct bwm_output_capture_mgr *mgr =
		wl_container_of(listener, mgr, display_destroy);
	wl_list_remove(&mgr->display_destroy.link);
	wl_global_destroy(mgr->global);
	free(mgr);
}

struct bwm_copy_mgr {
	struct wl_global *global;
	struct wl_listener display_destroy;
};

struct bwm_copy_session {
	struct wl_resource *resource;
	struct wlr_ext_image_capture_source_v1 *source;
	struct bwm_copy_frame *frame;
	struct wl_listener source_destroy;
};

struct bwm_copy_frame {
	struct wl_resource *resource;
	struct wlr_buffer *buffer;
	bool capturing;
	pixman_region32_t buffer_damage;
	struct bwm_copy_session *session;
};

static struct bwm_copy_session *session_from_resource(struct wl_resource *resource);
static struct bwm_copy_frame *frame_from_resource(struct wl_resource *resource);

static void frame_destroy(struct bwm_copy_frame *frame) {
	if (!frame)
		return;
	wl_resource_set_user_data(frame->resource, NULL);
	if (frame->buffer)
		wlr_buffer_unlock(frame->buffer);
	pixman_region32_fini(&frame->buffer_damage);
	free(frame);
}

static void frame_handle_resource_destroy(struct wl_resource *resource) {
	struct bwm_copy_frame *frame = frame_from_resource(resource);
	if (frame) {
		frame->session->frame = NULL;
		frame_destroy(frame);
	}
}

static void frame_handle_destroy(struct wl_client *wl_client,
		struct wl_resource *frame_resource) {
	(void)wl_client;
	wl_resource_destroy(frame_resource);
}

static void frame_handle_attach_buffer(struct wl_client *wl_client,
		struct wl_resource *frame_resource,
		struct wl_resource *buffer_resource) {
	(void)wl_client;
	struct bwm_copy_frame *frame = frame_from_resource(frame_resource);
	if (!frame)
		return;

	struct wlr_buffer *buffer = wlr_buffer_try_from_resource(buffer_resource);
	if (!buffer) {
		wl_resource_post_error(frame->resource,
			EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_NO_BUFFER,
			"invalid buffer");
		return;
	}

	if (frame->buffer)
		wlr_buffer_unlock(frame->buffer);
	frame->buffer = buffer;
	wlr_buffer_lock(frame->buffer);
}

static void frame_handle_damage_buffer(struct wl_client *wl_client,
		struct wl_resource *frame_resource,
		int32_t x, int32_t y, int32_t width, int32_t height) {
	(void)wl_client;
	struct bwm_copy_frame *frame = frame_from_resource(frame_resource);
	if (!frame)
		return;

	if (x < 0 || y < 0 || width <= 0 || height <= 0) {
		wl_resource_post_error(frame->resource,
			EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_INVALID_BUFFER_DAMAGE,
			"invalid buffer damage");
		return;
	}

	pixman_region32_union_rect(&frame->buffer_damage,
		&frame->buffer_damage, x, y, width, height);
}

static void block_windows_cpu(void *data, uint32_t width, uint32_t height,
		size_t stride, struct bwm_image_copy_source *src, float scale) {
	struct bwm_output *bwm_output = output_from_wlr_output(src->output);
	if (!bwm_output)
		return;

	int count = 0;

	struct bwm_toplevel *tl;
	wl_list_for_each(tl, &server.toplevels, link) {
		if (!tl->node || !tl->node->client)
			continue;
		client_t *c = tl->node->client;
		if (!c->block_out_from_screenshare)
			continue;
		if (!c->shown && c->state != STATE_FULLSCREEN)
			continue;

		struct wlr_box win_rect = {0};
		switch (c->state) {
		case STATE_TILED:
		case STATE_PSEUDO_TILED:
			win_rect = c->tiled_rectangle;
			break;
		case STATE_FLOATING:
			win_rect = c->floating_rectangle;
			break;
		case STATE_FULLSCREEN:
			win_rect = bwm_output->rectangle;
			break;
		}
		if (win_rect.width == 0 || win_rect.height == 0)
			continue;

		int abs_x, abs_y;
		wlr_scene_node_coords(&tl->scene_tree->node, &abs_x, &abs_y);

		int bw = (int)border_width;
		int bx = (int)((abs_x - bwm_output->rectangle.x - bw) * scale);
		int by = (int)((abs_y - bwm_output->rectangle.y - bw) * scale);
		int bw_w = (int)((win_rect.width + 2 * bw) * scale);
		int bw_h = (int)((win_rect.height + 2 * bw) * scale);

		wlr_log(WLR_DEBUG, "ext-copy-capture: CPU block-out tl abs=(%d,%d) "
			"win_rect=(%d,%d,%d,%d) out_rect=(%d,%d) bw=%d "
			"box=(%d,%d,%d,%d) capture=(%dx%d) scale=%.1f",
			abs_x, abs_y, win_rect.x, win_rect.y,
			win_rect.width, win_rect.height,
			bwm_output->rectangle.x, bwm_output->rectangle.y,
			bw, bx, by, bw_w, bw_h, width, height, scale);

		if (bx < 0) { bw_w += bx; bx = 0; }
		if (by < 0) { bw_h += by; by = 0; }
		if (bx + bw_w > (int)width) bw_w = (int)width - bx;
		if (by + bw_h > (int)height) bw_h = (int)height - by;
		if (bw_w <= 0 || bw_h <= 0)
			continue;

		for (int row = 0; row < bw_h; row++) {
			uint32_t *pixels = (uint32_t *)((uint8_t *)data + (by + row) * stride);
			for (int col = 0; col < bw_w; col++) {
				pixels[bx + col] = 0xFF000000;
			}
		}
		count++;
	}

	struct bwm_xwayland_view *xw;
	wl_list_for_each(xw, &server.xwayland.views, link) {
		if (!xw->node || !xw->node->client)
			continue;
		client_t *c = xw->node->client;
		if (!c->block_out_from_screenshare)
			continue;
		if (!c->shown && c->state != STATE_FULLSCREEN)
			continue;

		struct wlr_box win_rect = {0};
		switch (c->state) {
		case STATE_TILED:
		case STATE_PSEUDO_TILED:
			win_rect = c->tiled_rectangle;
			break;
		case STATE_FLOATING:
			win_rect = c->floating_rectangle;
			break;
		case STATE_FULLSCREEN:
			win_rect = bwm_output->rectangle;
			break;
		}
		if (win_rect.width == 0 || win_rect.height == 0)
			continue;

		int abs_x, abs_y;
		wlr_scene_node_coords(&xw->scene_tree->node, &abs_x, &abs_y);

		int bw = (int)border_width;
		int bx = (int)((abs_x - bwm_output->rectangle.x - bw) * scale);
		int by = (int)((abs_y - bwm_output->rectangle.y - bw) * scale);
		int bw_w = (int)((win_rect.width + 2 * bw) * scale);
		int bw_h = (int)((win_rect.height + 2 * bw) * scale);

		wlr_log(WLR_DEBUG, "ext-copy-capture: CPU block-out xw abs=(%d,%d) "
			"box=(%d,%d,%d,%d) scale=%.1f",
			abs_x, abs_y, bx, by, bw_w, bw_h, scale);

		if (bx < 0) { bw_w += bx; bx = 0; }
		if (by < 0) { bw_h += by; by = 0; }
		if (bx + bw_w > (int)width) bw_w = (int)width - bx;
		if (by + bw_h > (int)height) bw_h = (int)height - by;
		if (bw_w <= 0 || bw_h <= 0)
			continue;

		for (int row = 0; row < bw_h; row++) {
			uint32_t *pixels = (uint32_t *)((uint8_t *)data + (by + row) * stride);
			for (int col = 0; col < bw_w; col++) {
				pixels[bx + col] = 0xFF000000;
			}
		}
		count++;
	}

	wlr_log(WLR_DEBUG, "ext-copy-capture: CPU block-out applied %d windows", count);
}

static bool perform_output_capture(struct bwm_copy_frame *frame,
		struct bwm_image_copy_source *src) {
	struct wlr_output *output = src->output;

	if (!src->last_buffer) {
		wlr_log(WLR_DEBUG, "ext-copy-capture: no buffer, rendering fresh frame");
		struct wlr_scene_output *scene_output =
			wlr_scene_get_scene_output(server.scene, output);
		if (!scene_output) {
			wlr_log(WLR_DEBUG, "ext-copy-capture: no scene output");
			return false;
		}
		wlr_damage_ring_add_whole(&scene_output->damage_ring);
		struct wlr_output_state tmp_state;
		wlr_output_state_init(&tmp_state);
		wlr_output_state_set_enabled(&tmp_state, true);
		if (!wlr_scene_output_build_state(scene_output, &tmp_state, NULL)) {
			wlr_log(WLR_DEBUG, "ext-copy-capture: scene build failed");
			wlr_output_state_finish(&tmp_state);
			return false;
		}
		src->last_buffer = tmp_state.buffer;
		wlr_buffer_lock(src->last_buffer);
		wlr_output_state_finish(&tmp_state);
		wlr_damage_ring_add_whole(&scene_output->damage_ring);
		wlr_log(WLR_DEBUG, "ext-copy-capture: fresh render OK, buffer=%p (%dx%d)",
			(void*)src->last_buffer,
			src->last_buffer->width, src->last_buffer->height);
	}

	int phys_w = src->last_buffer->width;
	int phys_h = src->last_buffer->height;

	struct wlr_texture *texture = wlr_texture_from_buffer(output->renderer,
		src->last_buffer);
	if (!texture) {
		wlr_log(WLR_DEBUG, "ext-copy-capture: failed to create texture");
		return false;
	}

	wlr_log(WLR_DEBUG, "ext-copy-capture: texture=%p phys=%dx%d logical=%dx%d",
		(void*)texture, phys_w, phys_h, src->base.width, src->base.height);

	// try rendering directly to client buffer
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		output->renderer, frame->buffer, NULL);
	if (pass) {
		wlr_log(WLR_DEBUG, "ext-copy-capture: direct render pass started");
		wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
			.texture = texture,
			.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
			.dst_box = {
				.width = src->base.width,
				.height = src->base.height,
			},
			.src_box = {
				.width = phys_w,
				.height = phys_h,
			},
		});

		block_windows(src, pass, output->scale);

		wlr_render_pass_submit(pass);
		wlr_texture_destroy(texture);
		wlr_log(WLR_DEBUG, "ext-copy-capture: direct render completed OK");
		goto send_ready;
	}

	wlr_log(WLR_DEBUG, "ext-copy-capture: direct render failed, "
		"falling back to SHM read-pixels + CPU block-out");

	// Client buffer is SHM; read texture directly into client buffer
	uint32_t dst_format;
	size_t dst_stride;
	void *dst_data;

	if (!wlr_buffer_begin_data_ptr_access(frame->buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_WRITE,
			&dst_data, &dst_format, &dst_stride)) {
		wlr_texture_destroy(texture);
		wlr_log(WLR_DEBUG, "ext-copy-capture: client buffer not writable");
		return false;
	}

	wlr_log(WLR_DEBUG, "ext-copy-capture: client fmt=0x%x stride=%zu "
		"size=%dx%d",
		dst_format, dst_stride, phys_w, phys_h);

	if (!wlr_texture_read_pixels(texture, &(struct wlr_texture_read_pixels_options){
			.data = dst_data,
			.format = dst_format,
			.stride = (uint32_t)dst_stride,
			.src_box = { .width = phys_w, .height = phys_h },
		})) {
		wlr_buffer_end_data_ptr_access(frame->buffer);
		wlr_texture_destroy(texture);
		wlr_log(WLR_DEBUG, "ext-copy-capture: read_pixels failed");
		return false;
	}

	wlr_texture_destroy(texture);
	wlr_log(WLR_DEBUG, "ext-copy-capture: read_pixels OK, applying CPU block-out");

	// apply block-out rectangles on the client buffer
	block_windows_cpu(dst_data, phys_w, phys_h, dst_stride, src,
		output->scale);

	wlr_log(WLR_DEBUG, "ext-copy-capture: SHM path completed OK");
	wlr_buffer_end_data_ptr_access(frame->buffer);

send_ready:
	ext_image_copy_capture_frame_v1_send_transform(frame->resource,
		WL_OUTPUT_TRANSFORM_NORMAL);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	time_t tv_sec = now.tv_sec;
	uint32_t tv_sec_hi = (sizeof(tv_sec) > 4) ? tv_sec >> 32 : 0;
	uint32_t tv_sec_lo = tv_sec & 0xFFFFFFFF;
	ext_image_copy_capture_frame_v1_send_presentation_time(
		frame->resource, tv_sec_hi, tv_sec_lo, now.tv_nsec);

	ext_image_copy_capture_frame_v1_send_ready(frame->resource);
	return true;
}

static void frame_handle_capture(struct wl_client *wl_client,
		struct wl_resource *frame_resource) {
	(void)wl_client;
	struct bwm_copy_frame *frame = frame_from_resource(frame_resource);
	if (!frame)
		return;

	if (!frame->buffer) {
		wl_resource_post_error(frame->resource,
			EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_NO_BUFFER,
			"capture without buffer");
		return;
	}
	if (frame->capturing) {
		wl_resource_post_error(frame->resource,
			EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_ALREADY_CAPTURED,
			"already captured");
		return;
	}

	frame->capturing = true;

	struct bwm_copy_session *session = frame->session;
	struct wlr_ext_image_capture_source_v1 *source = session->source;

	if (!source) {
		ext_image_copy_capture_frame_v1_send_failed(frame->resource,
			EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED);
		session->frame = NULL;
		frame_destroy(frame);
		return;
	}

	if (wlr_output_try_from_ext_image_capture_source_v1(source) ||
			source->impl == &source_impl) {
		struct bwm_image_copy_source *img_src = source_from_base(source);
		if (perform_output_capture(frame, img_src)) {
			session->frame = NULL;
			frame_destroy(frame);
			return;
		}
	}

	ext_image_copy_capture_frame_v1_send_failed(frame->resource,
		EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
	session->frame = NULL;
	frame_destroy(frame);
}

static const struct ext_image_copy_capture_frame_v1_interface frame_impl = {
	.destroy = frame_handle_destroy,
	.attach_buffer = frame_handle_attach_buffer,
	.damage_buffer = frame_handle_damage_buffer,
	.capture = frame_handle_capture,
};

static struct bwm_copy_frame *frame_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&ext_image_copy_capture_frame_v1_interface, &frame_impl));
	return wl_resource_get_user_data(resource);
}

static void session_source_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct bwm_copy_session *session =
		wl_container_of(listener, session, source_destroy);
	session->source = NULL;
	wl_list_remove(&session->source_destroy.link);
	wl_list_init(&session->source_destroy.link);
}

static void session_destroy(struct bwm_copy_session *session) {
	if (!session)
		return;
	wl_list_remove(&session->source_destroy.link);
	if (session->source)
		session->source = NULL;
	wl_resource_set_user_data(session->resource, NULL);
	free(session);
}

static void session_handle_resource_destroy(struct wl_resource *resource) {
	struct bwm_copy_session *session = session_from_resource(resource);
	if (session)
		session_destroy(session);
}

static void session_handle_create_frame(struct wl_client *wl_client,
		struct wl_resource *session_resource, uint32_t id) {
	struct bwm_copy_session *session = session_from_resource(session_resource);
	if (!session)
		return;

	if (session->frame) {
		wl_resource_post_error(session->resource,
			EXT_IMAGE_COPY_CAPTURE_SESSION_V1_ERROR_DUPLICATE_FRAME,
			"duplicate frame");
		return;
	}

	struct bwm_copy_frame *frame = calloc(1, sizeof(*frame));
	if (!frame) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	frame->resource = wl_resource_create(wl_client,
		&ext_image_copy_capture_frame_v1_interface,
		wl_resource_get_version(session_resource), id);
	if (!frame->resource) {
		free(frame);
		wl_client_post_no_memory(wl_client);
		return;
	}

	frame->session = session;
	session->frame = frame;
	pixman_region32_init(&frame->buffer_damage);

	wl_resource_set_implementation(frame->resource, &frame_impl, frame,
		frame_handle_resource_destroy);
}

static void session_handle_destroy(struct wl_client *wl_client,
		struct wl_resource *session_resource) {
	(void)wl_client;
	wl_resource_destroy(session_resource);
}

static const struct ext_image_copy_capture_session_v1_interface session_impl = {
	.create_frame = session_handle_create_frame,
	.destroy = session_handle_destroy,
};

static struct bwm_copy_session *session_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&ext_image_copy_capture_session_v1_interface, &session_impl));
	return wl_resource_get_user_data(resource);
}

static uint32_t drm_format_to_wl_shm(uint32_t fmt) {
	switch (fmt) {
	case DRM_FORMAT_ARGB8888: return WL_SHM_FORMAT_ARGB8888;
	case DRM_FORMAT_XRGB8888: return WL_SHM_FORMAT_XRGB8888;
	case DRM_FORMAT_ABGR8888: return WL_SHM_FORMAT_ABGR8888;
	default: return fmt;
	}
}

static void send_buffer_constraints(struct bwm_copy_session *session,
		struct wlr_ext_image_capture_source_v1 *source) {
	if (!source)
		return;

	ext_image_copy_capture_session_v1_send_buffer_size(session->resource,
		source->width, source->height);

	for (size_t i = 0; i < source->shm_formats_len; i++)
		ext_image_copy_capture_session_v1_send_shm_format(
			session->resource, drm_format_to_wl_shm(source->shm_formats[i]));

	if (source->dmabuf_device) {
		dev_t dev = source->dmabuf_device;
		uint32_t dev_arr[2] = { (uint32_t)(dev >> 32), (uint32_t)dev };
		struct wl_array arr;
		wl_array_init(&arr);
		uint32_t *d = wl_array_add(&arr, sizeof(dev_arr));
		if (d)
			memcpy(d, dev_arr, sizeof(dev_arr));
		ext_image_copy_capture_session_v1_send_dmabuf_device(
			session->resource, &arr);
		wl_array_release(&arr);
	}

	for (size_t i = 0; i < source->dmabuf_formats.len; i++) {
		const struct wlr_drm_format *fmt = &source->dmabuf_formats.formats[i];
		struct wl_array mods;
		wl_array_init(&mods);
		for (size_t j = 0; j < fmt->len; j++) {
			uint64_t *m = wl_array_add(&mods, sizeof(uint64_t));
			if (m)
				*m = fmt->modifiers[j];
		}
		ext_image_copy_capture_session_v1_send_dmabuf_format(
			session->resource, fmt->format, &mods);
		wl_array_release(&mods);
	}

	ext_image_copy_capture_session_v1_send_done(session->resource);
}

static void mgr_handle_create_session(struct wl_client *wl_client,
		struct wl_resource *mgr_resource, uint32_t id,
		struct wl_resource *source_resource, uint32_t options) {
	(void)options;
	(void)mgr_resource;

	struct wlr_ext_image_capture_source_v1 *wlr_source =
		wlr_ext_image_capture_source_v1_from_resource(source_resource);
	if (!wlr_source)
		return;

	struct bwm_copy_session *session = calloc(1, sizeof(*session));
	if (!session) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	session->resource = wl_resource_create(wl_client,
		&ext_image_copy_capture_session_v1_interface,
		wl_resource_get_version(source_resource), id);
	if (!session->resource) {
		free(session);
		wl_client_post_no_memory(wl_client);
		return;
	}

	session->source = wlr_source;
	wl_resource_set_implementation(session->resource, &session_impl, session,
		session_handle_resource_destroy);

	session->source_destroy.notify = session_source_destroy;
	wl_signal_add(&wlr_source->events.destroy, &session->source_destroy);

	send_buffer_constraints(session, wlr_source);
}

static void mgr_handle_create_pointer_cursor_session(struct wl_client *wl_client,
		struct wl_resource *mgr_resource, uint32_t id,
		struct wl_resource *source_resource,
		struct wl_resource *pointer_resource) {
	(void)mgr_resource;
	(void)source_resource;
	(void)pointer_resource;
	struct bwm_copy_session *session = calloc(1, sizeof(*session));
	if (!session) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	session->resource = wl_resource_create(wl_client,
		&ext_image_copy_capture_session_v1_interface, 1, id);
	if (!session->resource) {
		free(session);
		wl_client_post_no_memory(wl_client);
		return;
	}

	wl_resource_set_implementation(session->resource, &session_impl, session,
		session_handle_resource_destroy);

	ext_image_copy_capture_session_v1_send_stopped(session->resource);
}

static void mgr_handle_destroy(struct wl_client *wl_client,
		struct wl_resource *mgr_resource) {
	(void)wl_client;
	wl_resource_destroy(mgr_resource);
}

static const struct ext_image_copy_capture_manager_v1_interface copy_mgr_impl = {
	.create_session = mgr_handle_create_session,
	.create_pointer_cursor_session = mgr_handle_create_pointer_cursor_session,
	.destroy = mgr_handle_destroy,
};

static void copy_mgr_bind(struct wl_client *wl_client, void *data,
		uint32_t version, uint32_t id) {
	struct bwm_copy_mgr *mgr = data;
	struct wl_resource *resource = wl_resource_create(wl_client,
		&ext_image_copy_capture_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	wl_resource_set_implementation(resource, &copy_mgr_impl, mgr, NULL);
}

static void copy_mgr_display_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct bwm_copy_mgr *mgr =
		wl_container_of(listener, mgr, display_destroy);
	wl_list_remove(&mgr->display_destroy.link);
	wl_global_destroy(mgr->global);
	free(mgr);
}

static struct bwm_output_capture_mgr *output_mgr = NULL;
static struct bwm_copy_mgr *copy_mgr = NULL;

void image_copy_capture_init(void) {
	output_mgr = calloc(1, sizeof(*output_mgr));
	if (!output_mgr)
		goto err_out;

	output_mgr->global = wl_global_create(server.wl_display,
		&ext_output_image_capture_source_manager_v1_interface, 1,
		output_mgr, output_mgr_bind);
	if (!output_mgr->global)
		goto err_out;

	output_mgr->display_destroy.notify = output_mgr_display_destroy;
	wl_display_add_destroy_listener(server.wl_display,
		&output_mgr->display_destroy);

	copy_mgr = calloc(1, sizeof(*copy_mgr));
	if (!copy_mgr)
		goto err_out;

	copy_mgr->global = wl_global_create(server.wl_display,
		&ext_image_copy_capture_manager_v1_interface, 1,
		copy_mgr, copy_mgr_bind);
	if (!copy_mgr->global)
		goto err_out;

	copy_mgr->display_destroy.notify = copy_mgr_display_destroy;
	wl_display_add_destroy_listener(server.wl_display,
		&copy_mgr->display_destroy);

	wlr_log(WLR_INFO, "ext-image-copy-capture initialized");
	return;

err_out:
	wlr_log(WLR_ERROR, "Failed to initialize ext-image-copy-capture");
	image_copy_capture_fini();
}

void image_copy_capture_fini(void) {
	if (output_mgr) {
		wl_list_remove(&output_mgr->display_destroy.link);
		if (output_mgr->global)
			wl_global_destroy(output_mgr->global);
		free(output_mgr);
		output_mgr = NULL;
	}
	if (copy_mgr) {
		wl_list_remove(&copy_mgr->display_destroy.link);
		if (copy_mgr->global)
			wl_global_destroy(copy_mgr->global);
		free(copy_mgr);
		copy_mgr = NULL;
	}
}
