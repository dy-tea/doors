#pragma once

#include <cairo.h>
#include <stdbool.h>
#include <wlr/interfaces/wlr_buffer.h>

// wlr_buffer backed by a cairo image surface
typedef struct cairo_buffer_t {
	struct wlr_buffer base;
	cairo_surface_t *surface;
	cairo_t *cairo;
} cairo_buffer_t;

cairo_buffer_t *cairo_buffer_create(cairo_surface_t *surface, int width, int height);
cairo_buffer_t *cairo_buffer_create_image(int width, int height);
cairo_t *cairo_buffer_cairo(cairo_buffer_t *buffer);
struct wlr_buffer *cairo_buffer_base(cairo_buffer_t *buffer);

void cairo_buffer_destroy(cairo_buffer_t *buffer);
