#include "ext-image-capture-source-v1-protocol.h"
#include "once.h"
#include "protocol/capture.h"
#include "protocol/copy_capture.h"
#include "server.h"
#include "types.h"
#include <assert.h>
#include <stdlib.h>
#include <wayland-server.h>
#include <wlr/interfaces/wlr_ext_image_capture_source_v1.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pass.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/addon.h>
#include <wlr/util/log.h>


typedef struct output_capture_source_t {
	struct wlr_ext_image_capture_source_v1 base;
	struct wlr_addon addon;

	struct wlr_output *output;

	size_t num_started;
	bool software_cursors_locked;

	struct wl_listener output_commit;
} output_capture_source_t;

typedef struct output_source_frame_event_t {
	struct wlr_ext_image_capture_source_v1_frame_event base;
	struct wlr_buffer *buffer;
	struct timespec when;
} output_source_frame_event_t;

static output_capture_source_t *source_from_base(struct wlr_ext_image_capture_source_v1 *base) {
	return (output_capture_source_t *)((char *)base - offsetof(output_capture_source_t, base));
}

static void source_update_constraints(output_capture_source_t *source) {
	struct wlr_output *output = source->output;
	if (!output->enabled || !output->renderer)
		return;

	if (!wlr_output_configure_primary_swapchain(output, NULL, &output->swapchain))
		return;

	wlr_ext_image_capture_source_v1_set_constraints_from_swapchain(&source->base, output->swapchain,
		output->renderer);
}

static bool copy_dmabuf(struct wlr_ext_image_copy_capture_frame_v1 *frame, struct wlr_buffer *src,
		struct wlr_renderer *renderer, const struct wlr_box *rects, size_t nrects) {
	struct wlr_texture *texture = wlr_texture_from_buffer(renderer, src);
	if (!texture)
		return false;

	bool ok = false;
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(renderer, frame->buffer, NULL);
	if (!pass)
		goto out;

	wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
		.texture = texture,
		.clip = &frame->buffer_damage,
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
	});

	capture_block_out_render(pass, rects, nrects);

	ok = wlr_render_pass_submit(pass);

out:
	wlr_texture_destroy(texture);
	return ok;
}

static bool copy_shm(struct wlr_ext_image_copy_capture_frame_v1 *frame, struct wlr_buffer *src,
		struct wlr_renderer *renderer, const struct wlr_box *rects, size_t nrects) {
	void *data;
	uint32_t format;
	size_t stride;
	if (!wlr_buffer_begin_data_ptr_access(frame->buffer, WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data,
		&format, &stride))
		return false;

	struct wlr_texture *texture = wlr_texture_from_buffer(renderer, src);
	if (!texture) {
		wlr_buffer_end_data_ptr_access(frame->buffer);
		return false;
	}

	bool ok = wlr_texture_read_pixels(texture, &(struct wlr_texture_read_pixels_options){
		.data = data,
		.format = format,
		.stride = (uint32_t)stride,
	});
	wlr_texture_destroy(texture);

	if (ok && nrects > 0)
		capture_block_out_fill_shm(data, format, stride, frame->buffer->width, frame->buffer->height,
			rects, nrects);

	wlr_buffer_end_data_ptr_access(frame->buffer);
	return ok;
}

static bool copy_buffer_to_frame(struct wlr_ext_image_copy_capture_frame_v1 *frame,
		struct wlr_buffer *src, struct wlr_renderer *renderer, const struct wlr_box *rects, size_t nrects,
		bool have_dmabuf, bool have_shm) {
	struct wlr_buffer *dst = frame->buffer;

	enum ext_image_copy_capture_frame_v1_failure_reason reason =
		EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS;
	bool ok = false;

	struct wlr_dmabuf_attributes dmabuf;
	if (src->width != dst->width || src->height != dst->height) {
		// the buffer doesn't match the source dimensions
	} else if (wlr_buffer_get_dmabuf(dst, &dmabuf)) {
		if (have_dmabuf) {
			reason = EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN;
			ok = copy_dmabuf(frame, src, renderer, rects, nrects);
		}
	} else if (have_shm) {
		reason = EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN;
		ok = copy_shm(frame, src, renderer, rects, nrects);
	}

	if (!ok)
		wlr_ext_image_copy_capture_frame_v1_fail(frame, reason);

	return ok;
}

static void output_source_start(struct wlr_ext_image_capture_source_v1 *base, bool with_cursors) {
	output_capture_source_t *source = source_from_base(base);

	source->num_started++;
	if (source->num_started > 1)
		return;

	wlr_output_lock_attach_render(source->output, true);
	if (with_cursors) {
		wlr_output_lock_software_cursors(source->output, true);
		source->software_cursors_locked = true;
	}
}

static void output_source_stop(struct wlr_ext_image_capture_source_v1 *base) {
	output_capture_source_t *source = source_from_base(base);

	if (source->num_started == 0)
		return;

	source->num_started--;
	if (source->num_started > 0)
		return;

	wlr_output_lock_attach_render(source->output, false);
	if (source->software_cursors_locked) {
		wlr_output_lock_software_cursors(source->output, false);
		source->software_cursors_locked = false;
	}
}

static void output_source_request_frame(struct wlr_ext_image_capture_source_v1 *base,
		bool schedule_frame) {
	output_capture_source_t *source = source_from_base(base);

	if (schedule_frame)
		wlr_output_update_needs_frame(source->output);
}

static void output_source_copy_frame(struct wlr_ext_image_capture_source_v1 *base,
		struct wlr_ext_image_copy_capture_frame_v1 *frame,
		struct wlr_ext_image_capture_source_v1_frame_event *frame_event) {
	output_capture_source_t *source = source_from_base(base);
	output_source_frame_event_t *event = (output_source_frame_event_t *)((char *)frame_event -
		offsetof(output_source_frame_event_t, base));

	if (!event->buffer) {
		wlr_ext_image_copy_capture_frame_v1_fail(frame,
			EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED);
		return;
	}

	struct wlr_box rects[CAPTURE_MAX_BLOCKED_RECTS];
	size_t nrects = capture_block_out_rects(source->output, rects, CAPTURE_MAX_BLOCKED_RECTS);

	if (copy_buffer_to_frame(frame, event->buffer, source->output->renderer, rects, nrects,
			source->base.dmabuf_formats.len > 0, source->base.shm_formats_len > 0)) {
		wlr_ext_image_copy_capture_frame_v1_ready(frame, source->output->transform, &event->when);
	}
}

static struct wlr_ext_image_capture_source_v1_cursor *output_source_get_pointer_cursor(struct
		wlr_ext_image_capture_source_v1 *base, struct wlr_seat *seat) {
	(void)base;
	(void)seat;
	return NULL;
}

static const struct wlr_ext_image_capture_source_v1_interface output_source_impl = {
	.start = output_source_start,
	.stop = output_source_stop,
	.request_frame = output_source_request_frame,
	.copy_frame = output_source_copy_frame,
	.get_pointer_cursor = output_source_get_pointer_cursor,
};

static void source_handle_output_commit(struct wl_listener *listener, void *data) {
	output_capture_source_t *source = wl_container_of(listener, source, output_commit);
	struct wlr_output_event_commit *event = data;
	struct wlr_output *output = source->output;

	if ((event->state->committed & WLR_OUTPUT_STATE_ENABLED) && !output->enabled)
		return;

	if (event->state->committed & (WLR_OUTPUT_STATE_MODE | WLR_OUTPUT_STATE_SCALE |
			WLR_OUTPUT_STATE_TRANSFORM | WLR_OUTPUT_STATE_RENDER_FORMAT | WLR_OUTPUT_STATE_ENABLED)) {
		source_update_constraints(source);
	}

	if (!(event->state->committed & WLR_OUTPUT_STATE_BUFFER))
		return;

	struct wlr_buffer *buffer = event->state->buffer;

	pixman_region32_t full_damage;
	pixman_region32_init_rect(&full_damage, 0, 0, buffer->width, buffer->height);

	const pixman_region32_t *damage = (event->state->committed & WLR_OUTPUT_STATE_DAMAGE) ?
		&event->state->damage : &full_damage;

	output_source_frame_event_t frame_event = {
		.base = {
			.damage = damage,
		},
		.buffer = buffer,
		.when = event->when,
	};
	wl_signal_emit_mutable(&source->base.events.frame, &frame_event.base);

	pixman_region32_fini(&full_damage);
}

static void source_addon_destroy(struct wlr_addon *addon) {
	output_capture_source_t *source = wl_container_of(addon, source, addon);
	wlr_ext_image_capture_source_v1_finish(&source->base);
	wl_list_remove(&source->output_commit.link);
	wlr_addon_finish(&source->addon);
	free(source);
}

static const struct wlr_addon_interface output_addon_impl = {
	.name = "doors_ext_image_capture_source_v1",
	.destroy = source_addon_destroy,
};

static output_capture_source_t *output_source_get_or_create(struct wlr_output *output) {
	struct wlr_addon *addon = wlr_addon_find(&output->addons, NULL, &output_addon_impl);
	if (addon) {
		output_capture_source_t *existing = wl_container_of(addon, existing, addon);
		return existing;
	}

	output_capture_source_t *source = calloc(1, sizeof(*source));
	if (!source)
		return NULL;

	source->output = output;
	wlr_ext_image_capture_source_v1_init(&source->base, &output_source_impl);
	wlr_addon_init(&source->addon, &output->addons, NULL, &output_addon_impl);

	source->output_commit.notify = source_handle_output_commit;
	wl_signal_add(&output->events.commit, &source->output_commit);

	source_update_constraints(source);

	return source;
}

typedef struct output_capture_mgr_t {
	struct wl_global *global;
	struct wl_listener display_destroy;
} output_capture_mgr_t;

static void output_mgr_handle_create_source(struct wl_client *wl_client,
		struct wl_resource *mgr_resource, uint32_t id, struct wl_resource *output_resource) {
	(void)mgr_resource;

	struct wlr_output *output = wlr_output_from_resource(output_resource);
	if (!output) {
		wlr_ext_image_capture_source_v1_create_resource(NULL, wl_client, id);
		return;
	}

	output_capture_source_t *source = output_source_get_or_create(output);
	if (!source) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	if (!wlr_ext_image_capture_source_v1_create_resource(&source->base, wl_client, id))
		wl_client_post_no_memory(wl_client);
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

static void output_mgr_bind(struct wl_client *wl_client, void *data, uint32_t version,
		uint32_t id) {
	output_capture_mgr_t *mgr = data;

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
	output_capture_mgr_t *mgr = wl_container_of(listener, mgr, display_destroy);
	wl_list_remove(&mgr->display_destroy.link);
	wl_global_destroy(mgr->global);
	free(mgr);
}

static struct wlr_ext_image_copy_capture_manager_v1 *copy_capture_manager = NULL;
static output_capture_mgr_t *output_source_manager = NULL;

void image_copy_capture_init(void) {
	ONCE();

	copy_capture_manager = wlr_ext_image_copy_capture_manager_v1_create(server.wl_display, 1);
	if (!copy_capture_manager) {
		wlr_log(WLR_ERROR, "Failed to create ext-image-copy-capture manager");
		return;
	}

	output_source_manager = calloc(1, sizeof(*output_source_manager));
	if (!output_source_manager) {
		wlr_log(WLR_ERROR, "Failed to create ext-output-image-capture-source manager");
		return;
	}

	output_source_manager->global = wl_global_create(server.wl_display,
		&ext_output_image_capture_source_manager_v1_interface, 1, output_source_manager, output_mgr_bind);
	if (!output_source_manager->global) {
		wlr_log(WLR_ERROR, "Failed to create ext-output-image-capture-source global");
		free(output_source_manager);
		output_source_manager = NULL;
		return;
	}

	output_source_manager->display_destroy.notify = output_mgr_display_destroy;
	wl_display_add_destroy_listener(server.wl_display, &output_source_manager->display_destroy);

	wlr_log(WLR_INFO, "Initialized ext-image-copy-capture (block-out supported)");
}

void image_copy_capture_fini(void) {
	ONCE();
}

struct wl_global *image_copy_capture_get_global(void) {
	return copy_capture_manager ? copy_capture_manager->global : NULL;
}

struct wl_global *image_capture_source_get_global(void) {
	return output_source_manager ? output_source_manager->global : NULL;
}
