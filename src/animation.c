#include "animation.h"
#include "layer.h"
#include "output.h"
#include "toplevel.h"
#include "tree.h"
#include "types.h"
#include <stdlib.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#define MIN(a, b) a < b ? a : b

struct bwm_snapshot_buffer {
  struct wl_list link;
  struct wlr_scene_buffer *buffer;
  int x;
  int y;
  int width;
  int height;
};

struct bwm_animation_entry {
  struct wl_list link;
  bool snapshot_resize;
  bool snapshot_buffers_initialized;
  bool updated_buffers_initialized;
  bool use_content_tree;
  bool workspace_switch;
  bool slide_out;
  struct node_t *node;
  struct bwm_toplevel *toplevel;
  struct wlr_scene_tree *scene_tree;
  struct wlr_scene_tree *saved_tree;
  struct wlr_box from;
  struct wlr_box to;
  struct timespec start;
  uint32_t duration_ms;
  struct wl_list snapshot_buffers;
  struct wl_list updated_buffers;
  struct bwm_output *output;
  float from_opacity;
  float to_opacity;
};

static struct wl_list animations;
static const uint32_t ANIMATION_DURATION_MS = 180;

static void snapshot_buffer_iterator(struct wlr_scene_buffer *buffer, int sx, int sy, void *data);
static void updated_buffer_iterator(struct wlr_scene_buffer *buffer, int sx, int sy, void *data);

static struct bwm_animation_entry *create_animation_entry(void) {
  struct bwm_animation_entry *entry = calloc(1, sizeof(*entry));
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

static struct bwm_animation_entry *find_animation(struct node_t *node) {
  struct bwm_animation_entry *entry;
  wl_list_for_each(entry, &animations, link)
    if (entry->node == node)
      return entry;

  return NULL;
}

static double elapsed_ms(struct timespec start, struct timespec now) {
  return (now.tv_sec - start.tv_sec) * 1000.0 +
    (now.tv_nsec - start.tv_nsec) / 1000000.0;
}

static double ease_out_cubic(double t) {
  double inv = 1.0 - t;
  return 1.0 - inv * inv * inv;
}

static void set_opacity_iterator(struct wlr_scene_buffer *buffer, int sx, int sy, void *data) {
  (void)sx; (void)sy;
  float *opacity = data;
  if (buffer)
    wlr_scene_buffer_set_opacity(buffer, *opacity);
}

static void schedule_output(struct bwm_output *output) {
  if (!output || !output->wlr_output || !output->enabled)
    return;

  wlr_output_schedule_frame(output->wlr_output);
}

static void snapshot_buffer_iterator(struct wlr_scene_buffer *buffer, int sx, int sy, void *data) {
  struct bwm_animation_entry *entry = data;

  if (!buffer || !buffer->buffer)
    return;

  struct bwm_snapshot_buffer *copy = calloc(1, sizeof(*copy));
  if (!copy)
    return;

  copy->buffer = buffer;
  copy->x = sx;
  copy->y = sy;
  copy->width = buffer->dst_width > 0 ? buffer->dst_width : buffer->WLR_PRIVATE.buffer_width;
  copy->height = buffer->dst_height > 0 ? buffer->dst_height : buffer->WLR_PRIVATE.buffer_height;
  wl_list_insert(&entry->snapshot_buffers, &copy->link);
}

static void updated_buffer_iterator(struct wlr_scene_buffer *buffer, int sx, int sy, void *data) {
  struct bwm_animation_entry *entry = data;

  if (!buffer || !buffer->buffer)
    return;
  if (!entry || !entry->scene_tree)
    return;

  struct wlr_scene_buffer *sbuf = wlr_scene_buffer_create(entry->scene_tree, NULL);
  if (!sbuf)
    return;

  wlr_scene_buffer_set_dest_size(sbuf, buffer->dst_width, buffer->dst_height);
  wlr_scene_buffer_set_opaque_region(sbuf, &buffer->opaque_region);
  wlr_scene_buffer_set_source_box(sbuf, &buffer->src_box);
  wlr_scene_node_set_position(&sbuf->node, sx, sy);
  wlr_scene_buffer_set_transform(sbuf, buffer->transform);
  wlr_scene_buffer_set_buffer(sbuf, buffer->buffer);

  struct bwm_snapshot_buffer *copy = calloc(1, sizeof(*copy));
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

struct clip_iter_data {
  struct bwm_animation_entry *entry;
  int anim_w;
  int anim_h;
};

static void content_buffer_clip_iterator(struct wlr_scene_buffer *buffer, int sx, int sy, void *data) {
  struct clip_iter_data *d = data;
  (void)d;
  if (!buffer || !buffer->buffer)
    return;

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

  wlr_scene_buffer_set_source_box(buffer, &src_fbox);
  wlr_scene_node_set_position(&buffer->node, vis_x1, vis_y1);
  wlr_scene_buffer_set_dest_size(buffer, (int)src_fbox.width, (int)src_fbox.height);
}

static void detect_buffer_iterator(struct wlr_scene_buffer *buffer, int sx, int sy, void *data) {
  (void)sx; (void)sy;
  bool *found = data;
  if (!found)
    return;
  if (buffer && buffer->buffer)
    *found = true;
}
static void destroy_snapshot_buffers(struct bwm_animation_entry *entry) {
  if (!entry || !entry->snapshot_buffers_initialized)
    return;

  struct bwm_snapshot_buffer *copy, *tmp;
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

void animation_init(void) {
  wl_list_init(&animations);
}

void animation_fini(void) {
  struct bwm_animation_entry *entry, *tmp;
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
  struct bwm_animation_entry *entry = find_animation(node);
  if (!entry)
    return;
  wlr_log(WLR_DEBUG, "animation: cancel node %u entry=%p", node ? node->id : 0, (void*)entry);
  if (entry->from_opacity != entry->to_opacity && entry->scene_tree) {
    float full = 1.0f;
    wlr_scene_node_for_each_buffer(&entry->scene_tree->node, set_opacity_iterator, &full);
  }
  destroy_snapshot_buffers(entry);
  wl_list_remove(&entry->link);
  free(entry);
}

void animation_cancel_scene_tree(struct wlr_scene_tree *scene_tree) {
  struct bwm_animation_entry *entry, *tmp;
  wl_list_for_each_safe(entry, tmp, &animations, link) {
    if (entry->scene_tree != scene_tree)
      continue;
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

bool animation_start_snapshot_resize(struct bwm_toplevel *toplevel, struct wlr_box from, struct wlr_box to) {
  if (!toplevel || !toplevel->saved_surface_tree || !toplevel->node || !enable_animations)
    return false;

  animation_cancel_node(toplevel->node);

  struct bwm_animation_entry *entry = find_animation(toplevel->node);
  if (!entry) {
    entry = create_animation_entry();
    if (!entry)
      return false;
  }

  entry->snapshot_resize = true;
  entry->node = toplevel->node;
  entry->toplevel = toplevel;
  entry->scene_tree = toplevel->saved_surface_tree;
  entry->from = from;
  entry->to = to;
  clock_gettime(CLOCK_MONOTONIC, &entry->start);
  entry->duration_ms = ANIMATION_DURATION_MS;
  entry->use_content_tree = false;
  entry->from_opacity = 1.0f;
  entry->to_opacity = 1.0f;

  destroy_snapshot_buffers(entry);
  wl_list_init(&entry->snapshot_buffers);
  wlr_scene_node_for_each_buffer(&toplevel->saved_surface_tree->node, snapshot_buffer_iterator, entry);

  wl_list_init(&entry->updated_buffers);
  if (toplevel->content_tree)
    wlr_scene_node_for_each_buffer(&toplevel->content_tree->node, updated_buffer_iterator, entry);

  if (toplevel->content_tree) {
    entry->use_content_tree = true;
    wlr_log(WLR_DEBUG, "animation: using content_tree for snapshot resize entry=%p node=%u", (void*)entry, entry->node ? entry->node->id : 0);
    wlr_scene_node_set_enabled(&toplevel->content_tree->node, true);
    if (toplevel->saved_surface_tree)
      wlr_scene_node_set_enabled(&toplevel->saved_surface_tree->node, false);
    entry->scene_tree = toplevel->content_tree;
  }

  if (wl_list_empty(&entry->snapshot_buffers)) {
    destroy_snapshot_buffers(entry);
    wl_list_remove(&entry->link);
    free(entry);
    return false;
  }

  wlr_scene_node_set_position(&toplevel->scene_tree->node, from.x, from.y);
  wlr_scene_node_set_position(&toplevel->saved_surface_tree->node, 0, 0);
  wlr_log(WLR_DEBUG, "animation: start snapshot resize entry=%p node=%u from=(%d,%d %dx%d) to=(%d,%d %dx%d)",
    (void*)entry, entry->node ? entry->node->id : 0,
    from.x, from.y, from.width, from.height,
    to.x, to.y, to.width, to.height);
  schedule_output(toplevel->node->output);
  return true;
}

bool animation_fade_in(struct bwm_toplevel *toplevel) {
  if (!toplevel || !toplevel->node || !toplevel->scene_tree || !enable_animations)
    return false;

  struct bwm_animation_entry *entry = find_animation(toplevel->node);
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
  }

  float zero = 0.0f;
  wlr_scene_node_for_each_buffer(&toplevel->scene_tree->node, set_opacity_iterator, &zero);

  wlr_log(WLR_DEBUG, "animation: fade_in entry=%p", (void*)entry);
  schedule_output(toplevel->node->output);
  return true;
}

bool animation_fade_in_layer(struct bwm_layer_surface *layer) {
  if (!layer || !layer->scene_tree || !layer->output || !enable_animations)
    return false;

  struct bwm_animation_entry *entry = create_animation_entry();
  if (!entry)
    return false;

  entry->node = NULL;
  entry->toplevel = NULL;
  entry->scene_tree = layer->scene_tree;
  entry->output = layer->output;
  entry->from_opacity = 0.0f;
  entry->to_opacity = 1.0f;
  clock_gettime(CLOCK_MONOTONIC, &entry->start);
  entry->duration_ms = ANIMATION_DURATION_MS;

  float zero = 0.0f;
  wlr_scene_node_for_each_buffer(&layer->scene_tree->node, set_opacity_iterator, &zero);

  wlr_log(WLR_DEBUG, "animation: fade_in_layer entry=%p", (void*)entry);
  schedule_output(entry->output);
  return true;
}

bool animation_fade_out(struct bwm_toplevel *toplevel) {
  if (!toplevel || !toplevel->scene_tree || !toplevel->node ||
      !toplevel->node->output || !enable_animations)
    return false;

  struct bwm_animation_entry *entry = create_animation_entry();
  if (!entry)
    return false;

  entry->node = NULL;
  entry->toplevel = toplevel;
  entry->scene_tree = toplevel->scene_tree;
  entry->output = toplevel->node->output;
  entry->from_opacity = 1.0f;
  entry->to_opacity = 0.0f;
  clock_gettime(CLOCK_MONOTONIC, &entry->start);
  entry->duration_ms = ANIMATION_DURATION_MS;

  wlr_log(WLR_DEBUG, "animation: fade_out entry=%p", (void*)entry);
  schedule_output(entry->output);
  return true;
}

bool animation_fade_out_layer(struct bwm_layer_surface *layer) {
  if (!layer || !layer->saved_tree || !layer->output || !enable_animations)
    return false;

  struct bwm_animation_entry *entry = create_animation_entry();
  if (!entry)
    return false;

  entry->node = NULL;
  entry->toplevel = NULL;
  entry->scene_tree = layer->saved_tree;
  entry->output = layer->output;
  entry->from_opacity = 1.0f;
  entry->to_opacity = 0.0f;
  entry->saved_tree = layer->saved_tree;
  clock_gettime(CLOCK_MONOTONIC, &entry->start);
  entry->duration_ms = ANIMATION_DURATION_MS;

  wlr_log(WLR_DEBUG, "animation: fade_out_layer entry=%p saved_tree=%p",
    (void*)entry, (void*)layer->saved_tree);
  schedule_output(entry->output);
  return true;
}

bool animation_start_workspace_slide(struct bwm_output *output,
    struct node_t *node, struct wlr_scene_tree *scene_tree,
    struct wlr_box from, struct wlr_box to, bool slide_out) {
  if (!node || !scene_tree || !output || !enable_animations)
    return false;

  animation_cancel_node(node);

  struct bwm_animation_entry *entry = find_animation(node);
  if (!entry) {
    entry = create_animation_entry();
    if (!entry)
      return false;
  }

  entry->node = node;
  entry->scene_tree = scene_tree;
  entry->from = from;
  entry->to = to;
  entry->workspace_switch = true;
  entry->slide_out = slide_out;
  clock_gettime(CLOCK_MONOTONIC, &entry->start);
  entry->duration_ms = ANIMATION_DURATION_MS;
  entry->from_opacity = 1.0f;
  entry->to_opacity = 1.0f;

  wlr_scene_node_set_position(&scene_tree->node, entry->from.x, entry->from.y);
  schedule_output(output);
  wlr_log(WLR_DEBUG, "animation: workspace_slide entry=%p node=%u from=(%d,%d) to=(%d,%d)", 
    (void*)entry, node->id, from.x, from.y, to.x, to.y);
  return true;
}

bool animation_has_fade_out(struct wlr_scene_tree *scene_tree) {
  if (!scene_tree)
    return false;

  struct bwm_animation_entry *entry;
  wl_list_for_each(entry, &animations, link)
    if (entry->scene_tree == scene_tree && entry->to_opacity < entry->from_opacity)
      return true;

  return false;
}

bool animation_apply_geometry(struct node_t *node, struct wlr_scene_tree *scene_tree, struct wlr_box target, bool animate) {
  struct wlr_box from;
  from.x = scene_tree->node.x;
  from.y = scene_tree->node.y;
  from.width = target.width;
  from.height = target.height;
  return animation_apply_geometry_from(node, scene_tree, from, target, animate);
}

bool animation_apply_geometry_from(struct node_t *node, struct wlr_scene_tree *scene_tree,
    struct wlr_box from, struct wlr_box target, bool animate) {
  if (!node || !scene_tree)
    return false;

  struct bwm_output *output = node->output;
  if (!animate || !enable_animations || !output || !output->enabled || !node->client || !node->client->shown) {
    animation_cancel_node(node);
    wlr_scene_node_set_position(&scene_tree->node, target.x, target.y);
    return false;
  }

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);

  struct bwm_animation_entry *entry = find_animation(node);
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

  if (entry->from.x == entry->to.x && entry->from.y == entry->to.y) {
    animation_cancel_node(node);
    wlr_scene_node_set_position(&scene_tree->node, target.x, target.y);
    return false;
  }

  wlr_scene_node_set_position(&scene_tree->node, entry->from.x, entry->from.y);
  schedule_output(output);
  return true;
}

static void update_snapshot_entry(struct bwm_animation_entry *entry,
                                  struct timespec now) {
  if (!entry->toplevel || !entry->toplevel->saved_surface_tree)
    return;

  double progress = elapsed_ms(entry->start, now) / (double)entry->duration_ms;
  if (progress < 0.0)
    progress = 0.0;
  if (progress > 1.0)
    progress = 1.0;

  double eased = ease_out_cubic(progress);
  int x = (int)(entry->from.x + (entry->to.x - entry->from.x) * eased);
  int y = (int)(entry->from.y + (entry->to.y - entry->from.y) * eased);
  int width = (int)(entry->from.width + (entry->to.width - entry->from.width) * eased);
  int height = (int)(entry->from.height + (entry->to.height - entry->from.height) * eased);

  if (entry->from.width <= 0 || entry->from.height <= 0)
    return;

  double scale_x = (double)width / (double)entry->from.width;
  double scale_y = (double)height / (double)entry->from.height;

  wlr_scene_node_set_position(&entry->toplevel->scene_tree->node, x, y);

  if (entry->use_content_tree && entry->toplevel->content_tree) {
    struct wlr_box clip = {
      .x = entry->toplevel->geometry.x,
      .y = entry->toplevel->geometry.y,
      .width = width,
      .height = height,
    };
    wlr_scene_subsurface_tree_set_clip(&entry->toplevel->content_tree->node, &clip);

    bool has_children = !wl_list_empty(&entry->toplevel->content_tree->children);
    bool any_buf = false;
    if (has_children)
      wlr_scene_node_for_each_buffer(&entry->toplevel->content_tree->node, detect_buffer_iterator, &any_buf);
    wlr_log(WLR_DEBUG, "animation: content_tree children=%d any_buf=%d entry=%p node=%u",
      has_children, any_buf, (void*)entry, entry->node ? entry->node->id : 0);
    goto borders_update;
  }

  if (!entry->use_content_tree && entry->toplevel && entry->toplevel->content_tree &&
      entry->to.width > entry->from.width) {
    bool has_buf = false;
    wlr_scene_node_for_each_buffer(&entry->toplevel->content_tree->node,
                                   detect_buffer_iterator, &has_buf);
    if (has_buf) {
      entry->use_content_tree = true;
      wlr_log(WLR_DEBUG, "animation: switching to content_tree mid-animation entry=%p node=%u", (void*)entry, entry->node ? entry->node->id : 0);
      wlr_scene_node_set_enabled(&entry->toplevel->content_tree->node, true);
      if (entry->toplevel->saved_surface_tree)
        wlr_scene_node_set_enabled(&entry->toplevel->saved_surface_tree->node, false);
      entry->scene_tree = entry->toplevel->content_tree;
    }
  }

  struct bwm_snapshot_buffer *copy;
  bool used_updated = false;

  if (entry->to.width > entry->from.width && entry->updated_buffers_initialized && !wl_list_empty(&entry->updated_buffers)) {
    used_updated = true;
    if (entry->use_content_tree && entry->toplevel && entry->toplevel->content_tree) {
      struct clip_iter_data d = { .entry = entry, .anim_w = width, .anim_h = height };
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
        if (dw <= 0) dw = 1;
        if (dh <= 0) dh = 1;
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
    }

    if (entry->toplevel->rounded && entry->toplevel->rounded->border_shader_node && bw > 0) {
      int new_fw = bwidth + 2 * (int)bw;
      int new_fh = bheight + 2 * (int)bw;
      if (new_fw > 0 && new_fh > 0)
        wlr_scene_buffer_set_dest_size(entry->toplevel->rounded->border_shader_node, new_fw, new_fh);
    }
  }
}

bool animation_update_output(struct bwm_output *output, struct timespec now) {
  bool active = false;
  struct bwm_animation_entry *entry, *tmp;

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

      double progress = elapsed_ms(entry->start, now) / (double)entry->duration_ms;
      if (progress < 0.0)
        progress = 0.0;
      if (progress > 1.0)
        progress = 1.0;

      double eased = ease_out_cubic(progress);
      float opacity = (float)(entry->from_opacity + (entry->to_opacity - entry->from_opacity) * eased);
      wlr_scene_node_for_each_buffer(&entry->scene_tree->node, set_opacity_iterator, &opacity);

      if (progress >= 1.0) {
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

    if (entry->node->output != output)
      continue;

    if (entry->snapshot_resize) {
      update_snapshot_entry(entry, now);
      double progress = elapsed_ms(entry->start, now) / (double)entry->duration_ms;
      if (progress >= 1.0) {
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

    double progress = elapsed_ms(entry->start, now) / (double)entry->duration_ms;
    if (progress < 0.0)
      progress = 0.0;
    if (progress > 1.0)
      progress = 1.0;

    double eased = ease_out_cubic(progress);

    int x = (int)(entry->from.x + (entry->to.x - entry->from.x) * eased);
    int y = (int)(entry->from.y + (entry->to.y - entry->from.y) * eased);
    wlr_scene_node_set_position(&entry->scene_tree->node, x, y);

    float cur_opacity = (float)(entry->from_opacity + (entry->to_opacity - entry->from_opacity) * eased);
    if (entry->from_opacity != entry->to_opacity)
      wlr_scene_node_for_each_buffer(&entry->scene_tree->node, set_opacity_iterator, &cur_opacity);

    if (progress >= 1.0) {
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
