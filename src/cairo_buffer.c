#include "cairo_buffer.h"
#include <drm_fourcc.h>
#include <stdlib.h>
#include <wlr/types/wlr_buffer.h>

void cairo_buffer_destroy(cairo_buffer_t *buffer) {
	if (!buffer)
		return;

	if (buffer->cairo) {
		cairo_destroy(buffer->cairo);
		buffer->cairo = NULL;
	}
	if (buffer->surface) {
		cairo_surface_destroy(buffer->surface);
		buffer->surface = NULL;
	}

	free(buffer);
}

static void cairo_buffer_impl_destroy(struct wlr_buffer *wlr_buffer) {
	cairo_buffer_t *buffer = wl_container_of(wlr_buffer, buffer, base);
	cairo_buffer_destroy(buffer);
}

static bool cairo_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer, uint32_t flags,
		void **data, uint32_t *format, size_t *stride) {
	(void)flags;
	cairo_buffer_t *buffer = wl_container_of(wlr_buffer, buffer, base);
	*data = cairo_image_surface_get_data(buffer->surface);
	*stride = cairo_image_surface_get_stride(buffer->surface);
	*format = DRM_FORMAT_ARGB8888;
	return true;
}

static void cairo_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
	(void)wlr_buffer;
}

static const struct wlr_buffer_impl cairo_buffer_impl = {
	.destroy = cairo_buffer_impl_destroy,
	.begin_data_ptr_access = cairo_buffer_begin_data_ptr_access,
	.end_data_ptr_access = cairo_buffer_end_data_ptr_access,
};

cairo_buffer_t *cairo_buffer_create(cairo_surface_t *surface, const int width, const int height) {
	if (!surface || width <= 0 || height <= 0)
		return NULL;

	cairo_buffer_t *buffer = calloc(1, sizeof(*buffer));
	if (!buffer)
		return NULL;

	buffer->surface = surface;
	wlr_buffer_init(&buffer->base, &cairo_buffer_impl, width, height);
	return buffer;
}

cairo_buffer_t *cairo_buffer_create_image(const int width, const int height) {
	if (width <= 0 || height <= 0)
		return NULL;

	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		return NULL;
	}

	cairo_buffer_t *buffer = cairo_buffer_create(surface, width, height);
	if (!buffer)
		cairo_surface_destroy(surface);

	return buffer;
}

cairo_t *cairo_buffer_cairo(cairo_buffer_t *buffer) {
	if (!buffer)
		return NULL;

	if (!buffer->cairo)
		buffer->cairo = cairo_create(buffer->surface);

	return buffer->cairo;
}

struct wlr_buffer *cairo_buffer_base(cairo_buffer_t *buffer) {
	return buffer ? &buffer->base : NULL;
}
