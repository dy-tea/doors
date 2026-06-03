#pragma once

#include <stdbool.h>
#include <wlr/types/wlr_scene.h>

extern char text_font[128];
extern int text_height;

typedef struct text_node_t {
  int width;
  int max_width;
  int height;
  int baseline;
  bool pango_markup;
  float color[4];
  float background[4];

  struct wlr_scene_node *node;
} text_node_t;

text_node_t *text_node_create(struct wlr_scene_tree *parent,
	const char *text, float color[4], bool pango_markup);

void text_node_set_color(text_node_t *node, float color[4]);
void text_node_set_background(text_node_t *node, float background[4]);
void text_node_set_text(text_node_t *node, const char *text);
void text_node_set_max_width(text_node_t *node, int max_width);

int text_node_default_height(void);
