#include "surface.h"
#include "output.h"
#include "server.h"
#include "toplevel.h"
#include <pixman.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

static bool corner_mask_no_input(struct wlr_scene_buffer *buffer, double *sx, double *sy) {
  (void)buffer;
  (void)sx;
  (void)sy;
  return false;
}

void surface_set_blur(struct wlr_scene_tree *scene_tree, node_t *node,
    surface_blur_t **blur, bool enabled) {
  if (!scene_tree) return;

  if (enabled) {
    if (!*blur) {
      *blur = calloc(1, sizeof(**blur));
      if (!*blur) return;
    }
    if (!(*blur)->blur_node) {
      (*blur)->blur_node = wlr_scene_buffer_create(scene_tree, NULL);
      if ((*blur)->blur_node) {
        wlr_scene_node_lower_to_bottom(&(*blur)->blur_node->node);
        if (node && node->output) {
          struct wlr_scene_output *so = wlr_scene_get_scene_output(server.scene,
            node->output->wlr_output);
          if (so) {
            pixman_region32_union_rect(&so->damage_ring.current, &so->damage_ring.current,
              0, 0, (unsigned int)node->output->width,
              (unsigned int)node->output->height);
            output_schedule_frame(node->output);
          }
        }
      }
    }
  } else if (*blur && (*blur)->blur_node) {
    wlr_scene_node_destroy(&(*blur)->blur_node->node);
    (*blur)->blur_node = NULL;
    if ((*blur)->blur_buf) {
      wlr_buffer_unlock((*blur)->blur_buf);
      (*blur)->blur_buf = NULL;
      (*blur)->blur_buf_fbo = 0;
    }
  }
}

void surface_set_mica(struct wlr_scene_tree *scene_tree, node_t *node,
    surface_blur_t **blur, bool enabled) {
  if (!scene_tree) return;

  if (enabled) {
    if (!*blur) {
      *blur = calloc(1, sizeof(**blur));
      if (!*blur) return;
    }

    if (!(*blur)->mica_node) {
      (*blur)->mica_node = wlr_scene_buffer_create(scene_tree, NULL);
      if ((*blur)->mica_node) {
        wlr_scene_node_lower_to_bottom(&(*blur)->mica_node->node);
        if (node && node->output) {
          struct wlr_scene_output *so = wlr_scene_get_scene_output(server.scene,
            node->output->wlr_output);
          if (so) {
            pixman_region32_union_rect(&so->damage_ring.current, &so->damage_ring.current,
              0, 0, (unsigned int)node->output->width,
              (unsigned int)node->output->height);
            output_schedule_frame(node->output);
          }
        }
      }
    }
  } else if (*blur && (*blur)->mica_node) {
    wlr_scene_node_destroy(&(*blur)->mica_node->node);
    (*blur)->mica_node = NULL;
  }
}

void surface_set_acrylic(struct wlr_scene_tree *scene_tree, node_t *node,
    surface_blur_t **blur, bool enabled) {
  if (!scene_tree) return;

  if (enabled) {
    if (!*blur) {
      *blur = calloc(1, sizeof(**blur));
      if (!*blur) return;
    }

    if (!(*blur)->acrylic_node) {
      (*blur)->acrylic_node = wlr_scene_buffer_create(scene_tree, NULL);
      if ((*blur)->acrylic_node) {
        wlr_scene_node_lower_to_bottom(&(*blur)->acrylic_node->node);
        if (node && node->output) {
          struct wlr_scene_output *so = wlr_scene_get_scene_output(server.scene,
            node->output->wlr_output);
          if (so) {
            pixman_region32_union_rect(&so->damage_ring.current, &so->damage_ring.current,
              0, 0, (unsigned int)node->output->width,
              (unsigned int)node->output->height);
            output_schedule_frame(node->output);
          }
        }
      }
    }
  } else if (*blur && (*blur)->acrylic_node) {
    wlr_scene_node_destroy(&(*blur)->acrylic_node->node);
    (*blur)->acrylic_node = NULL;

    if ((*blur)->acrylic_buf) {
      wlr_buffer_unlock((*blur)->acrylic_buf);
      (*blur)->acrylic_buf = NULL;
      (*blur)->acrylic_buf_fbo = 0;
    }
  }
}

void surface_set_border_radius(struct wlr_scene_tree *scene_tree,
    struct wlr_scene_tree *content_tree, struct wlr_scene_tree *border_tree,
    node_t *node, surface_rounded_t **rounded,
    surface_shadow_t **shadow, float radius) {
  if (!scene_tree || !content_tree) return;

  if (node && node->client)
    node->client->border_radius = radius;

  if (*shadow)
    (*shadow)->shadow_dirty = true;

  if (radius > 0.0f) {
    if (!*rounded) {
      *rounded = calloc(1, sizeof(**rounded));
      if (!*rounded) return;
    }

    if (!(*rounded)->corner_mask_node) {
      (*rounded)->corner_mask_node = wlr_scene_buffer_create(scene_tree, NULL);
      if ((*rounded)->corner_mask_node) {
        wlr_scene_node_place_above(&(*rounded)->corner_mask_node->node,
          &content_tree->node);

        if (border_tree)
          wlr_scene_node_place_below(&(*rounded)->corner_mask_node->node,
            &border_tree->node);

        (*rounded)->corner_mask_node->point_accepts_input = corner_mask_no_input;
      }
    }
    (*rounded)->border_dirty = true;
    (*rounded)->corner_mask_dirty = true;
  } else if (*rounded) {
    if ((*rounded)->corner_mask_node) {
      wlr_scene_node_destroy(&(*rounded)->corner_mask_node->node);
      (*rounded)->corner_mask_node = NULL;

      if ((*rounded)->corner_mask_buf) {
        wlr_buffer_unlock((*rounded)->corner_mask_buf);
        (*rounded)->corner_mask_buf = NULL;
        (*rounded)->corner_mask_buf_fbo = 0;
      }
    }

    bool has_gradient = ((*rounded)->gradient_count >= 2);
    if (!has_gradient) {
      if ((*rounded)->border_shader_node) {
        wlr_scene_node_destroy(&(*rounded)->border_shader_node->node);
        (*rounded)->border_shader_node = NULL;

        if ((*rounded)->border_shader_buf) {
          wlr_buffer_unlock((*rounded)->border_shader_buf);
          (*rounded)->border_shader_buf = NULL;
          (*rounded)->border_shader_buf_fbo = 0;
          (*rounded)->border_shader_buf_w = 0;
          (*rounded)->border_shader_buf_h = 0;
        }
      }
      (*rounded)->border_dirty = false;
      (*rounded)->corner_mask_dirty = false;
    } else {
      (*rounded)->border_dirty = true;
      (*rounded)->corner_mask_dirty = true;
    }
  }
}

void surface_set_shadow(struct wlr_scene_tree *scene_tree, node_t *node,
    surface_shadow_t **shadow, bool enabled) {
  if (!scene_tree) return;

  if (node && node->client)
    node->client->shadow = enabled;

  if (enabled) {
    if (!*shadow) {
      *shadow = calloc(1, sizeof(**shadow));
      if (!*shadow) return;
    }
    (*shadow)->shadow_dirty = true;
  } else if (*shadow) {
    if ((*shadow)->shadow_node) {
      wlr_scene_node_destroy(&(*shadow)->shadow_node->node);
      (*shadow)->shadow_node = NULL;
    }
    if ((*shadow)->shadow_buf) {
      wlr_buffer_unlock((*shadow)->shadow_buf);
      (*shadow)->shadow_buf = NULL;
      (*shadow)->shadow_buf_fbo = 0;
    }
    (*shadow)->shadow_dirty = false;
  }
}

void surface_update_rounded(surface_rounded_t **rounded, float color[4], border_theme_t *bt) {
  if (!*rounded) {
    *rounded = calloc(1, sizeof(**rounded));
    if (!*rounded) return;
  }

  surface_rounded_t *r = *rounded;
  r->border_color[0] = color[0];
  r->border_color[1] = color[1];
  r->border_color[2] = color[2];
  r->border_color[3] = color[3];

  memcpy(r->gradient_colors, bt->gradient, bt->gradient_count * 4 * sizeof(float));
  r->gradient_count = bt->gradient_count;
  r->gradient_angle = bt->gradient_angle;
  memcpy(r->gradient2_colors, bt->gradient2, bt->gradient2_count * 4 * sizeof(float));
  r->gradient2_count = bt->gradient2_count;
  r->gradient2_angle = bt->gradient2_angle;
  r->gradient_lerp = bt->gradient_lerp;
  r->border_dirty = true;
  r->corner_mask_dirty = true;
}

void surface_client_set_blur(client_t *client, bool enabled) {
  if (client->toplevel)
    toplevel_set_blur(client->toplevel, enabled);
  else if (client->xwayland_view)
    xwayland_set_blur(client->xwayland_view, enabled);
}

void surface_client_set_mica(client_t *client, bool enabled) {
  if (client->toplevel)
    toplevel_set_mica(client->toplevel, enabled);
  else if (client->xwayland_view)
    xwayland_set_mica(client->xwayland_view, enabled);
}

void surface_client_set_acrylic(client_t *client, bool enabled) {
  if (client->toplevel)
    toplevel_set_acrylic(client->toplevel, enabled);
  else if (client->xwayland_view)
    xwayland_set_acrylic(client->xwayland_view, enabled);
}

void surface_client_set_border_radius(client_t *client, float radius) {
  if (client->toplevel)
    toplevel_set_border_radius(client->toplevel, radius);
  else if (client->xwayland_view)
    xwayland_set_border_radius(client->xwayland_view, radius);
  else
    client->border_radius = radius;
}

void surface_client_set_shadow(client_t *client, bool enabled) {
  if (client->toplevel)
    toplevel_set_shadow(client->toplevel, enabled);
  else if (client->xwayland_view)
    xwayland_set_shadow(client->xwayland_view, enabled);
}
