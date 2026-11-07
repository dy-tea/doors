#include "cairo_buffer.h"
#include "settings.h"
#include "str.h"
#include "text.h"
#include <math.h>
#include <pango/pangocairo.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

typedef struct {
	struct wlr_scene_buffer *buffer_node;
	char *text;
	text_node_t props;

	bool visible;
	float scale;

	struct wl_listener outputs_update;
	struct wl_listener destroy;
} text_buffer_t;

int text_node_default_height(void) {
	return settings.text_height;
}

static int get_text_width(const text_node_t *props) {
	int width = props->width;
	if (props->max_width >= 0 && props->max_width < width)
		width = props->max_width;

	if (width < 0)
		width = 0;

	return width;
}

static PangoLayout *make_pango_layout(cairo_t *cairo, const char *text, bool pango_markup) {
	PangoLayout *layout = pango_cairo_create_layout(cairo);
	PangoFontDescription *desc = pango_font_description_from_string(settings.text_font);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);

	if (pango_markup) {
		PangoAttrList *attrs;
		char *parsed_text;
		if (pango_parse_markup(text, -1, 0, &attrs, &parsed_text, NULL, NULL)) {
			pango_layout_set_text(layout, parsed_text, -1);
			pango_layout_set_attributes(layout, attrs);
			pango_attr_list_unref(attrs);
			g_free(parsed_text);
		} else {
			goto settext_default;
		}
	} else {
	settext_default:
		pango_layout_set_text(layout, text, -1);
	}

	return layout;
}

static void text_calc_size(text_buffer_t *buffer) {
	cairo_surface_t *recorder = cairo_recording_surface_create(CAIRO_CONTENT_COLOR_ALPHA, NULL);
	cairo_t *c = cairo_create(recorder);
	cairo_surface_destroy(recorder);
	if (cairo_status(c) != CAIRO_STATUS_SUCCESS) {
		cairo_destroy(c);
		return;
	}

	PangoLayout *layout = make_pango_layout(c, buffer->text, buffer->props.pango_markup);

	int width = 0, height = 0;
	pango_layout_get_pixel_size(layout, &width, &height);

	PangoLayoutIter *iter = pango_layout_get_iter(layout);
	buffer->props.baseline = pango_layout_iter_get_baseline(iter) / PANGO_SCALE;
	pango_layout_iter_free(iter);

	buffer->props.width = width;

	g_object_unref(layout);
	cairo_destroy(c);

	wlr_scene_buffer_set_dest_size(buffer->buffer_node, get_text_width(&buffer->props),
		buffer->props.height);
}

static void render_backing_buffer(const text_buffer_t *buffer) {
	if (!buffer->visible)
		return;

	if (buffer->props.max_width == 0) {
		wlr_scene_buffer_set_buffer(buffer->buffer_node, NULL);
		return;
	}

	const float scale = buffer->scale > 0 ? buffer->scale : 1.0f;
	const int width = (int)ceilf((float)get_text_width(&buffer->props) * scale);
	const int height = (int)ceilf((float)buffer->props.height * scale);

	if (width <= 0 || height <= 0) {
		wlr_scene_buffer_set_buffer(buffer->buffer_node, NULL);
		return;
	}

	cairo_buffer_t *cairo_buffer = cairo_buffer_create_image(width, height);
	if (!cairo_buffer)
		return;

	cairo_t *cairo = cairo_buffer_cairo(cairo_buffer);
	if (!cairo) {
		cairo_buffer_destroy(cairo_buffer);
		return;
	}

	cairo_set_antialias(cairo, CAIRO_ANTIALIAS_BEST);

	const float *bg = buffer->props.background;
	cairo_set_source_rgba(cairo, bg[0], bg[1], bg[2], bg[3]);
	cairo_rectangle(cairo, 0, 0, width, height);
	cairo_fill(cairo);

	const float *fg = buffer->props.color;
	cairo_set_source_rgba(cairo, fg[0], fg[1], fg[2], fg[3]);

	PangoLayout *layout = make_pango_layout(cairo, buffer->text, buffer->props.pango_markup);
	pango_layout_set_width(layout, width * PANGO_SCALE);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

	cairo_scale(cairo, scale, scale);
	cairo_move_to(cairo, 0, 0);
	pango_cairo_show_layout(cairo, layout);
	g_object_unref(layout);

	cairo_surface_flush(cairo_buffer->surface);

	wlr_scene_buffer_set_buffer(buffer->buffer_node, cairo_buffer_base(cairo_buffer));
	wlr_buffer_drop(cairo_buffer_base(cairo_buffer));
}

static void handle_outputs_update(struct wl_listener *listener, void *data) {
	text_buffer_t *buffer = wl_container_of(listener, buffer, outputs_update);
	struct wlr_scene_outputs_update_event *event = data;

	float scale = 0;
	for (size_t i = 0; i < event->size; i++) {
		struct wlr_scene_output *o = event->active[i];
		if (scale < o->output->scale)
			scale = o->output->scale;
	}

	buffer->visible = event->size > 0;

	if (scale != buffer->scale) {
		buffer->scale = scale;
		render_backing_buffer(buffer);
	}
}

static void handle_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	text_buffer_t *buffer = wl_container_of(listener, buffer, destroy);

	wl_list_remove(&buffer->outputs_update.link);
	wl_list_remove(&buffer->destroy.link);

	free(buffer->text);
	free(buffer);
}

text_node_t *text_node_create(struct wlr_scene_tree *parent, const char *text, const float color[4],
		const bool pango_markup) {
	text_buffer_t *buffer = calloc(1, sizeof(*buffer));
	if (!buffer)
		return NULL;

	struct wlr_scene_buffer *node = wlr_scene_buffer_create(parent, NULL);
	if (!node) {
		free(buffer);
		return NULL;
	}

	buffer->buffer_node = node;
	buffer->props.node = &node->node;
	buffer->props.max_width = -1;
	buffer->text = strdup(text ? text : "");
	if (!buffer->text) {
		wlr_scene_node_destroy(&node->node);
		free(buffer);
		return NULL;
	}

	buffer->props.height = settings.text_height;
	buffer->props.pango_markup = pango_markup;
	memcpy(buffer->props.color, color, sizeof(float) * 4);
	buffer->props.background[0] = buffer->props.background[1] = buffer->props.background[2] =
		buffer->props.background[3] = 0;
	buffer->scale = 1.0f;
	buffer->visible = true;

	buffer->destroy.notify = handle_destroy;
	wl_signal_add(&node->node.events.destroy, &buffer->destroy);
	buffer->outputs_update.notify = handle_outputs_update;
	wl_signal_add(&node->events.outputs_update, &buffer->outputs_update);

	text_calc_size(buffer);
	render_backing_buffer(buffer);

	return &buffer->props;
}

void text_node_set_color(text_node_t *node, const float color[4]) {
	if (!node)
		return;
	if (memcmp(node->color, color, sizeof(float) * 4) == 0)
		return;

	memcpy(node->color, color, sizeof(float) * 4);
	text_buffer_t *buffer = wl_container_of(node, buffer, props);
	render_backing_buffer(buffer);
}

void text_node_set_background(text_node_t *node, const float background[4]) {
	if (!node)
		return;
	if (memcmp(node->background, background, sizeof(float) * 4) == 0)
		return;

	memcpy(node->background, background, sizeof(float) * 4);
	text_buffer_t *buffer = wl_container_of(node, buffer, props);
	render_backing_buffer(buffer);
}

void text_node_set_text(text_node_t *node, const char *text) {
	if (!node)
		return;

	text_buffer_t *buffer = wl_container_of(node, buffer, props);
	if (!text)
		text = "";
	if (streq(buffer->text, text))
		return;

	char *new_text = strdup(text);
	if (!new_text)
		return;

	free(buffer->text);
	buffer->text = new_text;
	text_calc_size(buffer);
	render_backing_buffer(buffer);
}

void text_node_set_max_width(text_node_t *node, int max_width) {
	if (!node)
		return;

	text_buffer_t *buffer = wl_container_of(node, buffer, props);
	if (max_width == buffer->props.max_width)
		return;

	buffer->props.max_width = max_width;
	wlr_scene_buffer_set_dest_size(buffer->buffer_node, get_text_width(&buffer->props),
		buffer->props.height);
	render_backing_buffer(buffer);
}
