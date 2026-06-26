#include "animation.h"
#include "bezier.h"
#include "layer.h"
#include "output.h"
#include "spring.h"
#include "toplevel.h"
#include "tree.h"
#include "types.h"
#include <stdio.h>
#include <stdlib.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>
#include <limits.h>

#define MIN(a, b) a < b ? a : b

typedef struct {
  struct wl_list link;
  struct wlr_scene_buffer *buffer;
  int x;
  int y;
  int width;
  int height;
} snapshot_buffer_t;

typedef struct {
  struct wl_list link;
  bool snapshot_resize;
  bool snapshot_buffers_initialized;
  bool updated_buffers_initialized;
  bool use_content_tree;
  bool workspace_switch;
  bool slide_out;
  node_t *node;
  toplevel_t *toplevel;
  struct wlr_scene_tree *scene_tree;
  struct wlr_scene_tree *saved_tree;
  struct wlr_box from;
  struct wlr_box to;
  struct timespec start;
  uint32_t duration_ms;
  struct wl_list snapshot_buffers;
  struct wl_list updated_buffers;
  output_t *output;
  float from_opacity;
  float to_opacity;
  bool use_spring;
  double spring_position;
  double spring_velocity;
  struct timespec spring_last_tick;
  bool spring_done;
  char curve_name[64];
  double eased;
  double progress;
  int min_buf_y;
} animation_entry_t;

static struct wl_list animations;
static uint32_t ANIMATION_DURATION_MS = 180;
static char default_bezier_name[64] = "default";

#define ANIM_TYPE_COUNT 7
static const char *anim_type_names[ANIM_TYPE_COUNT] = {
  "geometry", "resize", "fade_in", "fade_out", "fade_in_layer", "fade_out_layer", "workspace_slide"
};

typedef struct {
  char bezier_name[64];
  uint32_t duration_ms;
  char spring_name[64];
  bool enabled;
} animation_type_config_t;

static animation_type_config_t anim_type_configs[ANIM_TYPE_COUNT];

static void apply_config_to_entry(animation_entry_t *entry, int type_index) {
  if (type_index >= 0 && type_index < ANIM_TYPE_COUNT) {
    animation_type_config_t *cfg = &anim_type_configs[type_index];

    // spring takes priority over bezier
    if (cfg->spring_name[0] != '\0' && spring_exists(cfg->spring_name)) {
      entry->use_spring = true;
      entry->spring_position = 0.0;
      entry->spring_velocity = 0.0;
      entry->spring_done = false;
      clock_gettime(CLOCK_MONOTONIC, &entry->spring_last_tick);
      snprintf(entry->curve_name, sizeof(entry->curve_name), "%s", cfg->spring_name);
      return;
    }

    if (cfg->bezier_name[0] != '\0' && bezier_exists(cfg->bezier_name))
      snprintf(entry->curve_name, sizeof(entry->curve_name), "%s", cfg->bezier_name);

    if (cfg->duration_ms > 0)
      entry->duration_ms = cfg->duration_ms;
  }
}

int animation_type_from_name(const char *name) {
  for (int i = 0; i < ANIM_TYPE_COUNT; i++)
    if (strcmp(name, anim_type_names[i]) == 0)
      return i;

  return -1;
}

bool animation_set_type_config(const char *type_name, const char *bezier_name, uint32_t duration_ms) {
  int idx = animation_type_from_name(type_name);
  if (idx < 0) return false;

  if (bezier_name) {
    if (bezier_name[0] == '\0' || bezier_exists(bezier_name))
      snprintf(anim_type_configs[idx].bezier_name, sizeof(anim_type_configs[idx].bezier_name), "%s", bezier_name);
    else
      return false;
  }
  if (duration_ms > 0)
    anim_type_configs[idx].duration_ms = duration_ms;

  return true;
}

const char *animation_type_get_bezier(const char *type_name) {
  int idx = animation_type_from_name(type_name);
  if (idx < 0) return NULL;
  return anim_type_configs[idx].bezier_name[0] ? anim_type_configs[idx].bezier_name : NULL;
}

uint32_t animation_type_get_duration(const char *type_name) {
  int idx = animation_type_from_name(type_name);
  if (idx < 0) return 0;
  return anim_type_configs[idx].duration_ms;
}

static void snapshot_buffer_iterator(struct wlr_scene_buffer *buffer, int sx, int sy, void *data);
static void updated_buffer_iterator(struct wlr_scene_buffer *buffer, int sx, int sy, void *data);

static animation_entry_t *create_animation_entry(void) {
  animation_entry_t *entry = calloc(1, sizeof(*entry));
  if (!entry)
    return NULL;

  wl_list_init(&entry->snapshot_buffers);
  entry->snapshot_buffers_initialized = true;
  wl_list_init(&entry->updated_buffers);
  entry->updated_buffers_initialized = true;
  entry->use_content_tree = false;
  entry->from_opacity = 1.0f;
  entry->to_opacity = 1.0f;
  wl_list_insert(&animations, &entry->link);
  wlr_log(WLR_DEBUG, "animation: created entry %p", (void *)entry);
  return entry;
}

static animation_entry_t *find_animation(node_t *node) {
  animation_entry_t *entry;
  wl_list_for_each(entry, &animations, link)
    if (entry->node == node)
      return entry;

  return NULL;
}

static double elapsed_ms(struct timespec start, struct timespec now) {
  return (now.tv_sec - start.tv_sec) * 1000.0 +
    (now.tv_nsec - start.tv_nsec) / 1000000.0;
}

void animation_set_bezier(const char *name) {
  if (name && name[0] != '\0') {
    if (bezier_exists(name)) {
      snprintf(default_bezier_name, sizeof(default_bezier_name), "%s", name);
      wlr_log(WLR_DEBUG, "animation: default bezier set to '%s'", name);
    } else {
      wlr_log(WLR_ERROR, "animation: no such bezier curve '%s'", name);
    }
  }
}

const char *animation_get_bezier(void) {
  return default_bezier_name;
}

void animation_set_duration(uint32_t ms) {
  if (ms > 0) {
    ANIMATION_DURATION_MS = ms;
    wlr_log(WLR_DEBUG, "animation: default duration set to %u ms", ms);
  }
}

uint32_t animation_get_duration(void) {
  return ANIMATION_DURATION_MS;
}

static void set_opacity_iterator(struct wlr_scene_buffer *buffer, int sx, int sy, void *data) {
  (void)sx; (void)sy;
  float *opacity = data;
  if (buffer)
    wlr_scene_buffer_set_opacity(buffer, *opacity);
}

static void schedule_output(output_t *output) {
  if (!output || !output->wlr_output || !output->enabled) return;

  wlr_output_schedule_frame(output->wlr_output);
}

static struct wlr_fbox clamp_buffer_source_box(struct wlr_scene_buffer *buffer,
    struct wlr_fbox box) {
  if (!buffer || !buffer->buffer)
    return (struct wlr_fbox){0};

  if (box.x < 0.0f) {
    box.width += box.x;
    box.x = 0.0f;
  }
  if (box.y < 0.0f) {
    box.height += box.y;
    box.y = 0.0f;
  }

  float max_w = (float)buffer->buffer->width;
  float max_h = (float)buffer->buffer->height;
  if (max_w < 1.0f || max_h < 1.0f)
    return (struct wlr_fbox){0, 0, 1, 1};
  if (box.x > max_w) box.x = max_w;
  if (box.y > max_h) box.y = max_h;
  if (box.x + box.width > max_w) box.width = max_w - box.x;
  if (box.y + box.height > max_h) box.height = max_h - box.y;

  if (box.width < 1.0f) box.width = 1.0f;
  if (box.height < 1.0f) box.height = 1.0f;
  if (box.x + box.width > max_w) box.x = max_w - box.width;
  if (box.y + box.height > max_h) box.y = max_h - box.height;
  if (box.x < 0.0f) box.x = 0.0f;
  if (box.y < 0.0f) box.y = 0.0f;

  return box;
}

static void snapshot_buffer_iterator(struct wlr_scene_buffer *buffer, int sx, int sy, void *data) {
  animation_entry_t *entry = data;

  if (!buffer || !buffer->buffer) return;

  snapshot_buffer_t *copy = calloc(1, sizeof(*copy));
  if (!copy) return;

  copy->buffer = buffer;
  copy->x = sx;
  copy->y = sy;
  copy->width = buffer->dst_width > 0 ? buffer->dst_width : buffer->WLR_PRIVATE.buffer_width;
  copy->height = buffer->dst_height > 0 ? buffer->dst_height : buffer->WLR_PRIVATE.buffer_height;
  wl_list_insert(&entry->snapshot_buffers, &copy->link);
}

static void updated_buffer_iterator(struct wlr_scene_buffer *buffer, int sx, int sy, void *data) {
  animation_entry_t *entry = data;

  if (!buffer || !buffer->buffer) return;
  if (!entry || !entry->scene_tree) return;

  struct wlr_scene_buffer *sbuf = wlr_scene_buffer_create(entry->scene_tree, NULL);
  if (!sbuf) return;

  wlr_scene_buffer_set_dest_size(sbuf, buffer->dst_width, buffer->dst_height);
  wlr_scene_buffer_set_opaque_region(sbuf, &buffer->opaque_region);
  struct wlr_fbox src_box = clamp_buffer_source_box(buffer, buffer->src_box);
  wlr_scene_buffer_set_source_box(sbuf, &src_box);
  wlr_scene_node_set_position(&sbuf->node, sx, sy);
  wlr_scene_buffer_set_transform(sbuf, buffer->transform);
  wlr_scene_buffer_set_buffer(sbuf, buffer->buffer);

  snapshot_buffer_t *copy = calloc(1, sizeof(*copy));
  if (!copy) {
    wlr_scene_node_destroy(&sbuf->node);
    return;
  }

  copy->buffer = sbuf;
  copy->x = sx;
  copy->y = sy;
  copy->width = buffer->dst_width > 0 ? buffer->dst_width : buffer->WLR_PRIVATE.buffer_width;
  copy->height = buffer->dst_height > 0 ? buffer->dst_height : buffer->WLR_PRIVATE.buffer_height;
  wl_list_insert(&entry->updated_buffers, &copy->link);
}

typedef struct {
  animation_entry_t *entry;
  int anim_w;
  int anim_h;
} clip_iter_data;

static void content_buffer_clip_iterator(struct wlr_scene_buffer *buffer, int sx, int sy, void *data) {
  clip_iter_data *d = data;
  (void)d;
  if (!buffer || !buffer->buffer) return;

  int vis_x1 = sx < 0 ? 0 : sx;
  int vis_y1 = sy < 0 ? 0 : sy;
  int vis_x2 = sx + (buffer->dst_width > 0 ? buffer->dst_width : buffer->WLR_PRIVATE.buffer_width);
  if (vis_x2 > d->anim_w) vis_x2 = d->anim_w;
  int vis_y2 = sy + (buffer->dst_height > 0 ? buffer->dst_height : buffer->WLR_PRIVATE.buffer_height);
  if (vis_y2 > d->anim_h) vis_y2 = d->anim_h;

  if (vis_x2 <= vis_x1 || vis_y2 <= vis_y1) {
    wlr_scene_node_set_position(&buffer->node, vis_x1, vis_y1);
    wlr_scene_buffer_set_dest_size(buffer, 1, 1);
    return;
  }

  struct wlr_fbox src_fbox = {
    .x = (float)(vis_x1 - sx),
    .y = (float)(vis_y1 - sy),
    .width = (float)(vis_x2 - vis_x1),
    .height = (float)(vis_y2 - vis_y1),
  };

  src_fbox = clamp_buffer_source_box(buffer, src_fbox);
  wlr_scene_buffer_set_source_box(buffer, &src_fbox);
  wlr_scene_node_set_position(&buffer->node, vis_x1, vis_y1);
  wlr_scene_buffer_set_dest_size(buffer, (int)src_fbox.width, (int)src_fbox.height);
}

static void detect_buffer_iterator(struct wlr_scene_buffer *buffer, int sx, int sy, void *data) {
  (void)sx; (void)sy;
  bool *found = data;
  if (!found) return;

  if (buffer && buffer->buffer)
    *found = true;
}

static void destroy_snapshot_buffers(animation_entry_t *entry) {
  if (!entry || !entry->snapshot_buffers_initialized) return;

  snapshot_buffer_t *copy, *tmp;
  wl_list_for_each_safe(copy, tmp, &entry->snapshot_buffers, link) {
    wl_list_remove(&copy->link);
    free(copy);
  }

  entry->snapshot_buffers_initialized = false;
  if (entry->updated_buffers_initialized) {
    wl_list_for_each_safe(copy, tmp, &entry->updated_buffers, link) {
      wl_list_remove(&copy->link);
      free(copy);
    }
    entry->updated_buffers_initialized = false;
  }
}

static void tick_entry(animation_entry_t *entry, struct timespec now) {
  if (entry->use_spring) {
    double dt = elapsed_ms(entry->spring_last_tick, now) / 1000.0;
    entry->spring_last_tick = now;
    spring_curve_t *curve = spring_find(entry->curve_name);

    if (curve) {
      entry->eased = spring_evaluate(curve, dt, &entry->spring_position, &entry->spring_velocity, &entry->spring_done);
    } else {
      entry->eased = 1.0;
      entry->spring_done = true;
    }
  } else {
    entry->progress = elapsed_ms(entry->start, now) / (double)entry->duration_ms;
    if (entry->progress < 0.0) entry->progress = 0.0;
    if (entry->progress > 1.0) entry->progress = 1.0;

    const char *bname = entry->curve_name[0] ? entry->curve_name : default_bezier_name;
    entry->eased = bezier_evaluate(bname, entry->progress);
  }
}

static bool is_entry_done(animation_entry_t *entry) {
  if (entry->use_spring) return entry->spring_done;

  return entry->progress >= 1.0;
}

bool animation_set_type_spring(const char *type_name, const char *spring_name) {
  int idx = animation_type_from_name(type_name);

  if (idx < 0) return false;
  if (!spring_name) return false;

  if (spring_name[0] == '\0' || spring_exists(spring_name)) {
    snprintf(anim_type_configs[idx].spring_name, sizeof(anim_type_configs[idx].spring_name), "%s", spring_name);
    return true;
  }

  return false;
}

const char *animation_type_get_spring(const char *type_name) {
  int idx = animation_type_from_name(type_name);
  if (idx < 0) return NULL;

  return anim_type_configs[idx].spring_name[0] ? anim_type_configs[idx].spring_name : NULL;
}

bool animation_type_set_enabled(const char *type_name, bool enabled) {
  int idx = animation_type_from_name(type_name);
  if (idx < 0) return false;
  anim_type_configs[idx].enabled = enabled;
  return true;
}

bool animation_type_get_enabled(const char *type_name) {
  int idx = animation_type_from_name(type_name);
  if (idx < 0) return true;
  return anim_type_configs[idx].enabled;
}

void animation_init(void) {
  wl_list_init(&animations);

  for (int i = 0; i < ANIM_TYPE_COUNT; i++)
    anim_type_configs[i].enabled = true;
}

void animation_fini(void) {
  animation_entry_t *entry, *tmp;
  wl_list_for_each_safe(entry, tmp, &animations, link) {
    if (entry->from_opacity != entry->to_opacity && entry->scene_tree) {
      float full = 1.0f;
      wlr_scene_node_for_each_buffer(&entry->scene_tree->node, set_opacity_iterator, &full);
    }
    destroy_snapshot_buffers(entry);
    wl_list_remove(&entry->link);
    free(entry);
  }
}

void animation_cancel_node(struct node_t *node) {
  animation_entry_t *entry = find_animation(node);
  if (!entry) return;

  wlr_log(WLR_DEBUG, "animation: cancel node %u entry=%p", node ? node->id : 0, (void*)entry);

  if (entry->from_opacity != entry->to_opacity && entry->scene_tree) {
    float full = 1.0f;
    wlr_scene_node_for_each_buffer(&entry->scene_tree->node, set_opacity_iterator, &full);
  }

  destroy_snapshot_buffers(entry);
  wl_list_remove(&entry->link);
  free(entry);
}

void animation_cancel_toplevel(struct toplevel_t *toplevel) {
  if (!toplevel) return;

  animation_entry_t *entry, *tmp;
  wl_list_for_each_safe(entry, tmp, &animations, link) {
    if (entry->toplevel != toplevel &&
        entry->scene_tree != toplevel->scene_tree &&
        entry->scene_tree != toplevel->content_tree &&
        entry->scene_tree != toplevel->saved_surface_tree)
      continue;

    wlr_log(WLR_DEBUG, "animation: cancel toplevel entry=%p node=%u",
      (void*)entry, entry->node ? entry->node->id : 0);

    if (entry->from_opacity != entry->to_opacity && entry->scene_tree) {
      float full = 1.0f;
      wlr_scene_node_for_each_buffer(&entry->scene_tree->node, set_opacity_iterator, &full);
    }

    destroy_snapshot_buffers(entry);
    wl_list_remove(&entry->link);
    free(entry);
  }

  toplevel_remove_saved_buffer(toplevel);
}

void animation_cancel_scene_tree(struct wlr_scene_tree *scene_tree) {
  animation_entry_t *entry, *tmp;
  wl_list_for_each_safe(entry, tmp, &animations, link) {
    if (entry->scene_tree != scene_tree) continue;

    if (entry->from_opacity != entry->to_opacity) {
      float full = 1.0f;
      wlr_scene_node_for_each_buffer(&entry->scene_tree->node, set_opacity_iterator, &full);
    }

    if (entry->saved_tree)
      wlr_scene_node_destroy(&entry->saved_tree->node);

    destroy_snapshot_buffers(entry);
    wl_list_remove(&entry->link);
    free(entry);
  }
}

bool animation_start_snapshot_resize(toplevel_t *toplevel, struct wlr_box from, struct wlr_box to) {
  if (!toplevel || !toplevel->saved_surface_tree || !toplevel->node || !enable_animations)
    return false;

  if (!anim_type_configs[1].enabled) return false;

  animation_cancel_node(toplevel->node);

  animation_entry_t *entry = find_animation(toplevel->node);
  if (!entry) {
    entry = create_animation_entry();
    if (!entry) return false;
  }

  entry->snapshot_resize = true;
  entry->node = toplevel->node;
  entry->toplevel = toplevel;
  entry->scene_tree = toplevel->saved_surface_tree;
  entry->from = from;
  entry->to = to;
  clock_gettime(CLOCK_MONOTONIC, &entry->start);
  entry->duration_ms = ANIMATION_DURATION_MS;
  apply_config_to_entry(entry, 1);
  entry->use_content_tree = false;
  entry->from_opacity = 1.0f;
  entry->to_opacity = 1.0f;

  destroy_snapshot_buffers(entry);
  wl_list_init(&entry->snapshot_buffers);
  wlr_scene_node_for_each_buffer(&toplevel->saved_surface_tree->node, snapshot_buffer_iterator, entry);

  wl_list_init(&entry->updated_buffers);
  if (toplevel->content_tree)
    wlr_scene_node_for_each_buffer(&toplevel->content_tree->node, updated_buffer_iterator, entry);

  bool growing = to.width >= from.width && to.height >= from.height;
  if (toplevel->content_tree && growing) {
    entry->use_content_tree = true;
    wlr_log(WLR_DEBUG, "animation: using content_tree for snapshot resize entry=%p node=%u", (void*)entry, entry->node ? entry->node->id : 0);
    wlr_scene_node_set_enabled(&toplevel->content_tree->node, true);
    if (toplevel->saved_surface_tree)
      wlr_scene_node_set_enabled(&toplevel->saved_surface_tree->node, false);
    entry->scene_tree = toplevel->content_tree;

    int init_clip_w = from.width;
    int init_clip_h = from.height;
    if (toplevel->geometry.width > 0 && toplevel->geometry.height > 0) {
      if ((int)toplevel->geometry.width < init_clip_w)
        init_clip_w = toplevel->geometry.width;
      if ((int)toplevel->geometry.height < init_clip_h)
        init_clip_h = toplevel->geometry.height;
    }
    struct wlr_box init_clip = {
      .x = toplevel->geometry.x,
      .y = toplevel->geometry.y,
      .width = init_clip_w,
      .height = init_clip_h,
    };
    wlr_scene_subsurface_tree_set_clip(&toplevel->content_tree->node, &init_clip);
  }

  if (wl_list_empty(&entry->snapshot_buffers)) {
    destroy_snapshot_buffers(entry);
    wl_list_remove(&entry->link);
    free(entry);
    return false;
  }

  int min_buf_y = INT_MAX;
  snapshot_buffer_t *sb;
  wl_list_for_each(sb, &entry->snapshot_buffers, link)
    if (sb->y < min_buf_y)
    	min_buf_y = sb->y;

  entry->min_buf_y = (min_buf_y == INT_MAX) ? 0 : min_buf_y;

  wlr_scene_node_set_position(&toplevel->scene_tree->node, from.x, from.y);
  wlr_scene_node_set_position(&toplevel->saved_surface_tree->node, 0, 0);

  wlr_log(WLR_DEBUG, "animation: start snapshot resize entry=%p node=%u from=(%d,%d %dx%d) to=(%d,%d %dx%d)",
    (void*)entry, entry->node ? entry->node->id : 0,
    from.x, from.y, from.width, from.height,
    to.x, to.y, to.width, to.height);

  schedule_output(toplevel->node->output);
  return true;
}

bool animation_fade_in(struct toplevel_t *toplevel) {
  if (!toplevel || !toplevel->node || !toplevel->scene_tree || !enable_animations)
    return false;

  if (!anim_type_configs[2].enabled) return false;

  animation_entry_t *entry = find_animation(toplevel->node);
  if (entry) {
    entry->from_opacity = 0.0f;
    entry->to_opacity = 1.0f;
  } else {
    entry = create_animation_entry();
    if (!entry)
      return false;

    entry->node = toplevel->node;
    entry->scene_tree = toplevel->scene_tree;
    entry->output = toplevel->node->output;
    entry->from.x = toplevel->scene_tree->node.x;
    entry->from.y = toplevel->scene_tree->node.y;
    entry->to = entry->from;
    entry->from_opacity = 0.0f;
    entry->to_opacity = 1.0f;
    clock_gettime(CLOCK_MONOTONIC, &entry->start);
    entry->duration_ms = ANIMATION_DURATION_MS;
    apply_config_to_entry(entry, 2);
  }

  float zero = 0.0f;
  wlr_scene_node_for_each_buffer(&toplevel->scene_tree->node, set_opacity_iterator, &zero);

  wlr_log(WLR_DEBUG, "animation: fade_in entry=%p", (void*)entry);
  schedule_output(toplevel->node->output);
  return true;
}

bool animation_fade_in_layer(layer_surface_t *layer) {
  if (!layer || !layer->scene_tree || !layer->output || !enable_animations)
    return false;

  if (!anim_type_configs[4].enabled) return false;

  animation_entry_t *entry = create_animation_entry();
  if (!entry) return false;

  entry->node = NULL;
  entry->toplevel = NULL;
  entry->scene_tree = layer->scene_tree;
  entry->output = layer->output;
  entry->from_opacity = 0.0f;
  entry->to_opacity = 1.0f;
  clock_gettime(CLOCK_MONOTONIC, &entry->start);
  entry->duration_ms = ANIMATION_DURATION_MS;
  apply_config_to_entry(entry, 4);

  float zero = 0.0f;
  wlr_scene_node_for_each_buffer(&layer->scene_tree->node, set_opacity_iterator, &zero);

  wlr_log(WLR_DEBUG, "animation: fade_in_layer entry=%p", (void*)entry);
  schedule_output(entry->output);
  return true;
}

bool animation_fade_out(toplevel_t *toplevel) {
  if (!toplevel || !toplevel->scene_tree || !toplevel->node ||
      !toplevel->node->output || !enable_animations)
    return false;

  if (!anim_type_configs[3].enabled) return false;

  animation_entry_t *entry = create_animation_entry();
  if (!entry) return false;

  entry->node = NULL;
  entry->toplevel = toplevel;
  entry->scene_tree = toplevel->scene_tree;
  entry->output = toplevel->node->output;
  entry->from_opacity = 1.0f;
  entry->to_opacity = 0.0f;
  clock_gettime(CLOCK_MONOTONIC, &entry->start);
  entry->duration_ms = ANIMATION_DURATION_MS;
  apply_config_to_entry(entry, 3);

  wlr_log(WLR_DEBUG, "animation: fade_out entry=%p", (void*)entry);
  schedule_output(entry->output);
  return true;
}

bool animation_fade_out_layer(layer_surface_t *layer) {
  if (!layer || !layer->saved_tree || !layer->output || !enable_animations)
    return false;

  if (!anim_type_configs[5].enabled) return false;

  animation_entry_t *entry = create_animation_entry();
  if (!entry) return false;

  entry->node = NULL;
  entry->toplevel = NULL;
  entry->scene_tree = layer->saved_tree;
  entry->output = layer->output;
  entry->from_opacity = 1.0f;
  entry->to_opacity = 0.0f;
  entry->saved_tree = layer->saved_tree;
  clock_gettime(CLOCK_MONOTONIC, &entry->start);
  entry->duration_ms = ANIMATION_DURATION_MS;
  apply_config_to_entry(entry, 5);

  wlr_log(WLR_DEBUG, "animation: fade_out_layer entry=%p saved_tree=%p",
    (void*)entry, (void*)layer->saved_tree);
  schedule_output(entry->output);
  return true;
}

bool animation_workspace_switch_active(void) {
  animation_entry_t *entry;
  wl_list_for_each(entry, &animations, link)
    if (entry->workspace_switch)
      return true;

  return false;
}

bool animation_start_workspace_slide(output_t *output, node_t *node,
		struct wlr_scene_tree *scene_tree, struct wlr_box from, struct wlr_box to, bool slide_out) {
  if (!node || !scene_tree || !output || !enable_animations) return false;

  if (!anim_type_configs[6].enabled) return false;

  animation_cancel_node(node);

  animation_entry_t *entry = find_animation(node);
  if (!entry) {
    entry = create_animation_entry();
    if (!entry) return false;
  }

  entry->node = node;
  entry->scene_tree = scene_tree;
  entry->from = from;
  entry->to = to;
  entry->workspace_switch = true;
  entry->slide_out = slide_out;
  clock_gettime(CLOCK_MONOTONIC, &entry->start);
  entry->duration_ms = ANIMATION_DURATION_MS;
  apply_config_to_entry(entry, 6);
  entry->from_opacity = 1.0f;
  entry->to_opacity = 1.0f;

  wlr_scene_node_set_position(&scene_tree->node, entry->from.x, entry->from.y);
  schedule_output(output);
  wlr_log(WLR_DEBUG, "animation: workspace_slide entry=%p node=%u from=(%d,%d) to=(%d,%d)",
    (void*)entry, node->id, from.x, from.y, to.x, to.y);
  return true;
}

bool animation_has_fade_out(struct wlr_scene_tree *scene_tree) {
  if (!scene_tree) return false;

  animation_entry_t *entry;
  wl_list_for_each(entry, &animations, link)
    if (entry->scene_tree == scene_tree && entry->to_opacity < entry->from_opacity)
      return true;

  return false;
}

bool animation_apply_geometry(node_t *node, struct wlr_scene_tree *scene_tree, struct wlr_box target, bool animate) {
  struct wlr_box from;
  from.x = scene_tree->node.x;
  from.y = scene_tree->node.y;
  from.width = target.width;
  from.height = target.height;
  return animation_apply_geometry_from(node, scene_tree, from, target, animate);
}

bool animation_apply_geometry_from(node_t *node, struct wlr_scene_tree *scene_tree,
    struct wlr_box from, struct wlr_box target, bool animate) {
  if (!node || !scene_tree) return false;

  output_t *output = node->output;
  if (!animate || !enable_animations || !output || !output->enabled || !node->client || !node->client->shown) {
    animation_cancel_node(node);
    wlr_scene_node_set_position(&scene_tree->node, target.x, target.y);
    return false;
  }

  if (!anim_type_configs[0].enabled) {
    animation_cancel_node(node);
    wlr_scene_node_set_position(&scene_tree->node, target.x, target.y);
    return false;
  }

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);

  animation_entry_t *entry = find_animation(node);
  if (!entry) {
    entry = create_animation_entry();
    if (!entry) {
      wlr_scene_node_set_position(&scene_tree->node, target.x, target.y);
      return false;
    }
  }

  entry->node = node;
  entry->scene_tree = scene_tree;
  entry->from = from;
  entry->to = target;
  entry->start = now;
  entry->duration_ms = ANIMATION_DURATION_MS;
  apply_config_to_entry(entry, 0);

  if (entry->from.x == entry->to.x && entry->from.y == entry->to.y) {
    animation_cancel_node(node);
    wlr_scene_node_set_position(&scene_tree->node, target.x, target.y);
    return false;
  }

  wlr_scene_node_set_position(&scene_tree->node, entry->from.x, entry->from.y);
  schedule_output(output);
  return true;
}

static void update_snapshot_entry(animation_entry_t *entry, struct timespec now) {
  (void)now;
  if (!entry->toplevel || !entry->toplevel->saved_surface_tree) return;

  double eased = entry->eased;
  int x = (int)(entry->from.x + (entry->to.x - entry->from.x) * eased);
  int y = (int)(entry->from.y + (entry->to.y - entry->from.y) * eased);
  int width = (int)(entry->from.width + (entry->to.width - entry->from.width) * eased);
  int height = (int)(entry->from.height + (entry->to.height - entry->from.height) * eased);

  if (entry->from.width <= 0 || entry->from.height <= 0) return;
  if (width < 1) width = 1;
  if (height < 1) height = 1;

  double scale_x = (double)width / (double)entry->from.width;
  double scale_y = (double)height / (double)entry->from.height;

  wlr_scene_node_set_position(&entry->toplevel->scene_tree->node, x, y);

  if (entry->use_content_tree && entry->toplevel->content_tree) {
    // clip to the smaller of surface geometry or animation target to prevent over/under-rendering
    int clip_w = width;
    int clip_h = height;
    if (entry->toplevel->geometry.width > 0 && entry->toplevel->geometry.height > 0) {
      if ((int)entry->toplevel->geometry.width < width)
        clip_w = entry->toplevel->geometry.width;
      else if ((int)entry->toplevel->geometry.width > width)
        clip_w = width;
      if ((int)entry->toplevel->geometry.height < height)
        clip_h = entry->toplevel->geometry.height;
      else if ((int)entry->toplevel->geometry.height > height)
        clip_h = height;
    }
    struct wlr_box clip = {
      .x = entry->toplevel->geometry.x,
      .y = entry->toplevel->geometry.y,
      .width = clip_w,
      .height = clip_h,
    };
    wlr_scene_subsurface_tree_set_clip(&entry->toplevel->content_tree->node, &clip);

    bool has_children = !wl_list_empty(&entry->toplevel->content_tree->children);
    bool any_buf = false;
    if (has_children) wlr_scene_node_for_each_buffer(&entry->toplevel->content_tree->node, detect_buffer_iterator, &any_buf);

    wlr_log(WLR_DEBUG, "animation: content_tree children=%d any_buf=%d entry=%p node=%u",
      has_children, any_buf, (void*)entry, entry->node ? entry->node->id : 0);
    goto borders_update;
  }

  if (!entry->use_content_tree && entry->toplevel && entry->toplevel->content_tree &&
      entry->to.width > entry->from.width) {
    bool has_buf = false;
    wlr_scene_node_for_each_buffer(&entry->toplevel->content_tree->node, detect_buffer_iterator, &has_buf);
    if (has_buf) {
      entry->use_content_tree = true;
      wlr_log(WLR_DEBUG, "animation: switching to content_tree mid-animation entry=%p node=%u", (void*)entry, entry->node ? entry->node->id : 0);
      wlr_scene_node_set_enabled(&entry->toplevel->content_tree->node, true);
      if (entry->toplevel->saved_surface_tree)
        wlr_scene_node_set_enabled(&entry->toplevel->saved_surface_tree->node, false);
      entry->scene_tree = entry->toplevel->content_tree;
    }
  }

  snapshot_buffer_t *copy;
  bool used_updated = false;

  if (entry->to.width > entry->from.width && entry->updated_buffers_initialized && !wl_list_empty(&entry->updated_buffers)) {
    used_updated = true;
    if (entry->use_content_tree && entry->toplevel && entry->toplevel->content_tree) {
      clip_iter_data d = { .entry = entry, .anim_w = width, .anim_h = height };
      wlr_scene_node_for_each_buffer(&entry->toplevel->content_tree->node, content_buffer_clip_iterator, &d);
    } else {
      wl_list_for_each(copy, &entry->updated_buffers, link) {
	      int vis_x1 = copy->x < 0 ? 0 : copy->x;
	      int vis_y1 = copy->y < 0 ? 0 : copy->y;
	      int vis_x2 = copy->x + copy->width;
	      if (vis_x2 > width) vis_x2 = width;
	      int vis_y2 = copy->y + copy->height;
	      if (vis_y2 > height) vis_y2 = height;

	      if (vis_x2 <= vis_x1 || vis_y2 <= vis_y1) {
	        wlr_scene_node_set_position(&copy->buffer->node, vis_x1, vis_y1);
	        wlr_scene_buffer_set_dest_size(copy->buffer, 1, 1);
	        continue;
	      }

	      struct wlr_fbox src_fbox = {
	        .x = (float)(vis_x1 - copy->x),
	        .y = (float)(vis_y1 - copy->y),
	        .width = (float)(vis_x2 - vis_x1),
	        .height = (float)(vis_y2 - vis_y1),
	      };

	      src_fbox = clamp_buffer_source_box(copy->buffer, src_fbox);
	      wlr_scene_buffer_set_source_box(copy->buffer, &src_fbox);
	      wlr_scene_node_set_position(&copy->buffer->node, vis_x1, vis_y1);
	      wlr_scene_buffer_set_dest_size(copy->buffer, (int)src_fbox.width, (int)src_fbox.height);
      }
    }
  }

  if (!used_updated) {
    if (entry->to.width > entry->from.width && !entry->use_content_tree && wl_list_empty(&entry->updated_buffers)) {
      wl_list_for_each(copy, &entry->snapshot_buffers, link) {
        struct wlr_fbox full_src = {
          .x = 0.0f,
          .y = 0.0f,
          .width = (float)copy->width,
          .height = (float)copy->height,
        };

        int dx = (int)(copy->x * scale_x);
        int dy = (int)(copy->y * scale_y);
        int dw = (int)(copy->width * scale_x);
        int dh = (int)(copy->height * scale_y);

        // clip to surface geometry when saved buffer is larger
        if (entry->toplevel && entry->toplevel->geometry.width > 0 && entry->toplevel->geometry.height > 0) {
          int max_w = MIN(width, entry->toplevel->geometry.width);
          int max_h = MIN(height, entry->toplevel->geometry.height);
          if (dw > max_w) dw = max_w;
          if (dh > max_h) dh = max_h;
        }

        if (dw <= 0) dw = 1;
        if (dh <= 0) dh = 1;

        full_src = clamp_buffer_source_box(copy->buffer, full_src);
        wlr_scene_buffer_set_source_box(copy->buffer, &full_src);
        wlr_scene_node_set_position(&copy->buffer->node, dx, dy);
        wlr_scene_buffer_set_dest_size(copy->buffer, dw, dh);
      }
      goto borders_update;
    }

    wl_list_for_each(copy, &entry->snapshot_buffers, link) {
      int vis_x1 = copy->x < 0 ? 0 : copy->x;
      int vis_y1 = copy->y < 0 ? 0 : copy->y;
      int vis_x2 = copy->x + copy->width;
      if (vis_x2 > width) vis_x2 = width;
      int vis_y2 = copy->y + copy->height;
      if (vis_y2 > height) vis_y2 = height;

      if (vis_x2 <= vis_x1 || vis_y2 <= vis_y1) {
        wlr_scene_node_set_position(&copy->buffer->node, vis_x1, vis_y1);
        wlr_scene_buffer_set_dest_size(copy->buffer, 1, 1);
        continue;
      }

      struct wlr_fbox src_fbox = {
        .x = (float)(vis_x1 - copy->x),
        .y = (float)(vis_y1 - copy->y),
        .width = (float)(vis_x2 - vis_x1),
        .height = (float)(vis_y2 - vis_y1),
      };

      src_fbox = clamp_buffer_source_box(copy->buffer, src_fbox);
      wlr_scene_buffer_set_source_box(copy->buffer, &src_fbox);
      wlr_scene_node_set_position(&copy->buffer->node, vis_x1, vis_y1);
      wlr_scene_buffer_set_dest_size(copy->buffer, (int)src_fbox.width, (int)src_fbox.height);
    }
  }

borders_update:
  if (entry->toplevel && entry->toplevel->border_tree) {
    unsigned int bw = 0;
    if (entry->node && entry->node->client)
      bw = entry->node->client->border_width;

    int bwidth = width, bheight = height;
    if (entry->use_content_tree && entry->toplevel->geometry.width > 0) {
      bwidth = MIN(width, entry->toplevel->geometry.width);
      bheight = MIN(height, entry->toplevel->geometry.height);
    }

    struct wlr_box geo = {0, 0, bwidth, bheight};
    update_borders(entry->toplevel->border_tree, entry->toplevel->border_rects, geo, bw);

    if (entry->use_content_tree) {
      int cx = entry->toplevel->content_tree->node.x;
      int cy = entry->toplevel->content_tree->node.y;
      if (cx > 0 || cy > 0)
        wlr_scene_node_set_position(&entry->toplevel->border_tree->node,
          cx - (int)bw, cy - (int)bw);
    } else {
      wlr_scene_node_set_position(&entry->toplevel->border_tree->node,
        -(int)bw, entry->min_buf_y - (int)bw);
    }

    if (entry->toplevel->rounded && entry->toplevel->rounded->border_shader_node && bw > 0) {
      int new_fw = bwidth + 2 * (int)bw;
      int new_fh = bheight + 2 * (int)bw;
      if (new_fw > 0 && new_fh > 0)
        wlr_scene_buffer_set_dest_size(entry->toplevel->rounded->border_shader_node, new_fw, new_fh);
    }
  }
}

bool animation_update_output(output_t *output, struct timespec now) {
  bool active = false;
  animation_entry_t *entry, *tmp;

  wl_list_for_each_safe(entry, tmp, &animations, link) {
    if (!entry->node) {
      if (entry->output != output) {
        wlr_log(WLR_DEBUG, "animation: skip entry=%p output mismatch", (void*)entry);
        continue;
      }

      if (!entry->scene_tree) {
        wl_list_remove(&entry->link);
        free(entry);
        continue;
      }

      tick_entry(entry, now);
      float opacity = (float)(entry->from_opacity + (entry->to_opacity - entry->from_opacity) * entry->eased);
      wlr_scene_node_for_each_buffer(&entry->scene_tree->node, set_opacity_iterator, &opacity);

      if (is_entry_done(entry)) {
        float full = 1.0f;
        wlr_scene_node_for_each_buffer(&entry->scene_tree->node, set_opacity_iterator, &full);

        if (entry->to_opacity < entry->from_opacity)
          wlr_scene_node_set_enabled(&entry->scene_tree->node, false);

        if (entry->saved_tree)
          wlr_scene_node_destroy(&entry->saved_tree->node);

        wl_list_remove(&entry->link);
        free(entry);
      } else {
        active = true;
      }
      continue;
    }

    if (!entry->scene_tree || !entry->node->client) {
      wl_list_remove(&entry->link);
      free(entry);
      continue;
    }

    if (entry->node->output != output) continue;

    if (entry->snapshot_resize) {
      tick_entry(entry, now);
      update_snapshot_entry(entry, now);
      if (is_entry_done(entry)) {
        if (entry->toplevel) {
          toplevel_remove_saved_buffer(entry->toplevel);
          if (entry->toplevel->scene_tree)
            wlr_scene_node_set_enabled(&entry->toplevel->scene_tree->node, true);
        }
        wlr_log(WLR_DEBUG, "animation: finishing snapshot entry=%p node=%u", (void*)entry, entry->node ? entry->node->id : 0);
        destroy_snapshot_buffers(entry);
        wl_list_remove(&entry->link);
        free(entry);
      } else {
        active = true;
      }
      continue;
    }

    if (!entry->node->client->shown || !entry->scene_tree->node.enabled) {
      wl_list_remove(&entry->link);
      free(entry);
      continue;
    }

    tick_entry(entry, now);

    int x = (int)(entry->from.x + (entry->to.x - entry->from.x) * entry->eased);
    int y = (int)(entry->from.y + (entry->to.y - entry->from.y) * entry->eased);
    wlr_scene_node_set_position(&entry->scene_tree->node, x, y);

    float cur_opacity = (float)(entry->from_opacity + (entry->to_opacity - entry->from_opacity) * entry->eased);
    if (entry->from_opacity != entry->to_opacity)
      wlr_scene_node_for_each_buffer(&entry->scene_tree->node, set_opacity_iterator, &cur_opacity);

    if (is_entry_done(entry)) {
      if (entry->from_opacity != entry->to_opacity) {
        float full = 1.0f;
        wlr_scene_node_for_each_buffer(&entry->scene_tree->node, set_opacity_iterator, &full);
      }

      if (entry->workspace_switch && entry->slide_out && entry->node && entry->node->client) {
        entry->node->client->shown = false;
        wlr_scene_node_set_enabled(&entry->scene_tree->node, false);
        wlr_log(WLR_DEBUG, "animation: workspace slide-out complete, disabled node=%u", entry->node->id);
      }

      wl_list_remove(&entry->link);
      free(entry);
    } else {
      active = true;
    }
  }

  return active;
}
