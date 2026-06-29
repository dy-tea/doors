#include "master_stack.h"
#include "toplevel.h"
#include "tree.h"
#include "output.h"
#include <stdbool.h>
#include <stdlib.h>
#include <wlr/util/log.h>

int master_stack_count = 1;
float master_stack_ratio = 0.5f;
master_area_orientation_t master_stack_orientation = MASTER_LEFT;
stack_layout_t master_stack_layout = STACK_VERTICAL;

static int collect_tiled_nodes(desktop_t *d, node_t ***out_nodes) {
  if (!d || !d->root) return 0;

  int count = 0;
  for (node_t *n = first_extrema(d->root); n != NULL; n = next_leaf(n, d->root))
    if (n->client && IS_TILED(n->client))
      count++;

  if (count == 0) return 0;

  node_t **nodes = calloc(count, sizeof(node_t *));
  if (!nodes) return 0;

  int idx = 0;
  for (node_t *n = first_extrema(d->root); n != NULL; n = next_leaf(n, d->root))
    if (n->client && IS_TILED(n->client))
      nodes[idx++] = n;

  *out_nodes = nodes;
  return count;
}

static int master_count_clamped(int total_nodes) {
  int mc = master_stack_count;
  if (mc < 1) mc = 1;
  if (mc > total_nodes) mc = total_nodes;
  return mc;
}

static void set_node_geom(node_t *n, struct wlr_box geom, output_t *m, desktop_t *d) {
  if (!n || !n->client) return;

  unsigned int bw = effective_border_width(d);
  struct wlr_box r = geom;
  r.x += bw;
  r.y += bw;
  r.width = (r.width > (int)(2 * bw)) ? r.width - 2 * bw : 0;
  r.height = (r.height > (int)(2 * bw)) ? r.height - 2 * bw : 0;

  if (n->client->state == STATE_PSEUDO_TILED) {
    if (r.width > n->client->floating_rectangle.width)
      r.width = n->client->floating_rectangle.width;
    if (r.height > n->client->floating_rectangle.height)
      r.height = n->client->floating_rectangle.height;
  }

  n->client->tiled_rectangle = r;
  node_set_pending_rectangle(n, geom);
  n->output = m;
  node_set_dirty(n);
}

static void distribute_area(struct wlr_box area, struct wlr_box *out, int count, int gap, bool vertical) {
  int *total_size = vertical ? &area.height : &area.width;
  int total_gap = (count - 1) * gap;
  int unit = (*total_size - total_gap) / count;

  for (int i = 0; i < count; i++) {
    out[i] = area;
    if (vertical) {
      out[i].y = area.y + i * (unit + gap);
      out[i].height = (i == count - 1) ? area.y + area.height - out[i].y : unit;
    } else {
      out[i].x = area.x + i * (unit + gap);
      out[i].width = (i == count - 1) ? area.x + area.width - out[i].x : unit;
    }
  }
}

static bool is_master(int idx, int mc) { return idx >= 0 && idx < mc; }

void master_stack_arrange(output_t *m, desktop_t *d, struct wlr_box available) {
  if (!d || !d->root) return;

  node_t **nodes = NULL;
  int n = collect_tiled_nodes(d, &nodes);
  if (n == 0) return;

  int wg = smart_gaps && visible_tiled_count(d) <= 1 ? 0 : d->window_gap;

  struct wlr_box rect = available;
  rect.x += wg;
  rect.y += wg;
  rect.width -= 2 * wg;
  rect.height -= 2 * wg;

  if (rect.width < 1 || rect.height < 1)
    rect = (struct wlr_box){0, 0, 0, 0};

  int mc = master_count_clamped(n);
  int sc = n - mc;
  bool horiz = master_stack_orientation == MASTER_LEFT || master_stack_orientation == MASTER_RIGHT;
  bool master_first = master_stack_orientation == MASTER_LEFT || master_stack_orientation == MASTER_TOP;

  int avail_master = (horiz ? rect.width : rect.height) - (sc > 0 ? wg : 0);
  int ms;
  if (sc == 0) {
    ms = avail_master;
  } else {
    ms = (int)((horiz ? rect.width : rect.height) * master_stack_ratio);
    if (ms < 1) ms = 1;
    if (ms > avail_master) ms = avail_master;
  }

  struct wlr_box master_area = {0}, stack_area = {0};

  if (horiz) {
    if (master_first) {
      master_area = (struct wlr_box){
      	.x = rect.x,
       	.y = rect.y,
        .width = ms,
        .height = rect.height
      };

      if (sc > 0)
        stack_area = (struct wlr_box){
        	.x = rect.x + ms + wg,
         	.y = rect.y,
          .width = rect.width - ms - wg,
          .height = rect.height
        };
    } else {
      if (sc > 0)
        stack_area = (struct wlr_box){
        	.x = rect.x,
         	.y = rect.y,
          .width = rect.width - ms - wg,
          .height = rect.height
        };

      master_area = (struct wlr_box){
      	.x = rect.x + rect.width - ms,
       	.y = rect.y,
        .width = ms,
        .height = rect.height
      };
    }
  } else {
    if (master_first) {
      master_area = (struct wlr_box){
      	.x = rect.x,
       	.y = rect.y,
        .width = rect.width,
        .height = ms
      };

      if (sc > 0)
        stack_area = (struct wlr_box){
        	.x = rect.x,
         	.y = rect.y + ms + wg,
          .width = rect.width,
          .height = rect.height - ms - wg
        };
    } else {
      if (sc > 0)
        stack_area = (struct wlr_box){
        	.x = rect.x,
         	.y = rect.y,
          .width = rect.width,
          .height = rect.height - ms - wg
        };

      master_area = (struct wlr_box){
      	.x = rect.x,
       	.y = rect.y + rect.height - ms,
        .width = rect.width,
        .height = ms
      };
    }
  }

  if (master_area.width < 1) master_area.width = 1;
  if (master_area.height < 1) master_area.height = 1;
  if (stack_area.width < 1) stack_area.width = 1;
  if (stack_area.height < 1) stack_area.height = 1;

  bool master_vertical = horiz;
  struct wlr_box *geoms = calloc(mc > sc ? mc : sc, sizeof(struct wlr_box));
  distribute_area(master_area, geoms, mc, wg, master_vertical);
  for (int i = 0; i < mc; i++)
    set_node_geom(nodes[i], geoms[i], m, d);

  distribute_area(stack_area, geoms, sc, wg, master_stack_layout == STACK_VERTICAL);
  for (int i = 0; i < sc; i++)
    set_node_geom(nodes[mc + i], geoms[i], m, d);

  free(geoms);
  free(nodes);
}

void master_stack_increment(void) {
  if (master_stack_count < 10)
    master_stack_count++;
}

void master_stack_decrement(void) {
  if (master_stack_count > 1)
    master_stack_count--;
}

void master_stack_set_count(int count) {
  if (count < 1) count = 1;
  if (count > 10) count = 10;
  master_stack_count = count;
}

void master_stack_flip_orientation(void) {
  switch (master_stack_orientation) {
  case MASTER_LEFT:    master_stack_orientation = MASTER_RIGHT; break;
  case MASTER_RIGHT:   master_stack_orientation = MASTER_LEFT; break;
  case MASTER_TOP:     master_stack_orientation = MASTER_BOTTOM; break;
  case MASTER_BOTTOM:  master_stack_orientation = MASTER_TOP; break;
  }
}

void master_stack_cycle_orientation(void) {
  master_stack_orientation = (master_stack_orientation + 1) % 4;
}

void master_stack_cycle_stack_layout(void) {
  master_stack_layout = (master_stack_layout + 1) % 2;
}

int master_stack_collect(desktop_t *d, node_t ***out_nodes) {
  return collect_tiled_nodes(d, out_nodes);
}

int master_stack_find_index(desktop_t *d, node_t *n) {
  if (!d || !n) return -1;

  node_t **nodes = NULL;
  int count = collect_tiled_nodes(d, &nodes);
  if (count == 0) return -1;

  for (int i = 0; i < count; i++) {
    if (nodes[i] == n) {
      free(nodes);
      return i;
    }
  }

  free(nodes);
  return -1;
}

static node_t **gather_focused(desktop_t *d, int *out_total, int *out_idx) {
  node_t **nodes = NULL;
  int count = collect_tiled_nodes(d, &nodes);
  if (count == 0) return NULL;

  for (int i = 0; i < count; i++)
    if (nodes[i] == d->focus) {
      *out_total = count;
      *out_idx = i;
      return nodes;
    }

  free(nodes);
  return NULL;
}

static bool focus_nth(desktop_t *d, int target) {
  if (target < 0) return false;
  node_t **nodes = NULL;
  int count = 0, idx = -1;
  nodes = gather_focused(d, &count, &idx);
  if (!nodes) return false;
  if (target >= count) { free(nodes); return false; }
  d->focus = nodes[target];
  free(nodes);
  return true;
}

static bool swap_with_nth(output_t *m, desktop_t *d, int target) {
  if (target < 0) return false;
  node_t **nodes = NULL;
  int count = 0, idx = -1;
  nodes = gather_focused(d, &count, &idx);
  if (!nodes || idx == target) { free(nodes); return false; }
  swap_nodes(m, d, nodes[idx], m, d, nodes[target]);
  free(nodes);
  return true;
}

bool master_stack_focus_south(desktop_t *d) {
  node_t **nodes = NULL;
  int count = 0, idx = -1;
  nodes = gather_focused(d, &count, &idx);
  if (!nodes) return false;

  int mc = master_count_clamped(count);
  int target = -1;

  if ((is_master(idx, mc) && idx + 1 < mc) ||
      (is_master(idx, mc) && idx + 1 >= mc && focus_wrapping))
    target = (idx + 1 < mc) ? idx + 1 : 0;
  else if (!is_master(idx, mc) && idx + 1 < count)
    target = idx + 1;
  else if (!is_master(idx, mc) && focus_wrapping)
    target = mc;

  free(nodes);
  return target >= 0 ? focus_nth(d, target) : false;
}

bool master_stack_focus_north(desktop_t *d) {
  node_t **nodes = NULL;
  int count = 0, idx = -1;
  nodes = gather_focused(d, &count, &idx);
  if (!nodes) return false;

  int mc = master_count_clamped(count);
  int target = -1;

  if (is_master(idx, mc) && idx - 1 >= 0)
    target = idx - 1;
  else if (is_master(idx, mc) && focus_wrapping)
    target = mc - 1;
  else if (!is_master(idx, mc) && idx - 1 >= mc)
    target = idx - 1;
  else if (!is_master(idx, mc) && focus_wrapping)
    target = count - 1;

  free(nodes);
  return target >= 0 ? focus_nth(d, target) : false;
}

bool master_stack_focus_east(desktop_t *d) {
  node_t **nodes = NULL;
  int count = 0, idx = -1;
  nodes = gather_focused(d, &count, &idx);
  if (!nodes) return false;

  int mc = master_count_clamped(count);
  int target = -1;

  if (is_master(idx, mc) && mc < count)
    target = mc;
  else if (is_master(idx, mc) && focus_wrapping)
    target = 0;
  else if (!is_master(idx, mc) && mc >= 1)
    target = 0;
  else if (!is_master(idx, mc) && focus_wrapping)
    target = count - 1;

  free(nodes);
  return target >= 0 ? focus_nth(d, target) : false;
}

bool master_stack_focus_west(desktop_t *d) {
  node_t **nodes = NULL;
  int count = 0, idx = -1;
  nodes = gather_focused(d, &count, &idx);
  if (!nodes) return false;

  int mc = master_count_clamped(count);
  int target = -1;

  if (is_master(idx, mc) && mc < count)
    target = count - 1;
  else if (is_master(idx, mc) && focus_wrapping)
    target = 0;
  else if (!is_master(idx, mc) && mc >= 1)
    target = mc - 1;
  else if (!is_master(idx, mc) && focus_wrapping)
    target = count - 1;

  free(nodes);
  return target >= 0 ? focus_nth(d, target) : false;
}

bool master_stack_swap_south(output_t *m, desktop_t *d) {
  node_t **nodes = NULL;
  int count = 0, idx = -1;
  nodes = gather_focused(d, &count, &idx);
  if (!nodes) return false;

  int target = (idx + 1 < count) ? idx + 1 : 0;
  free(nodes);
  return swap_with_nth(m, d, target);
}

bool master_stack_swap_north(output_t *m, desktop_t *d) {
  node_t **nodes = NULL;
  int count = 0, idx = -1;
  nodes = gather_focused(d, &count, &idx);
  if (!nodes) return false;

  int target = (idx - 1 >= 0) ? idx - 1 : count - 1;
  free(nodes);
  return swap_with_nth(m, d, target);
}

bool master_stack_swap_east(output_t *m, desktop_t *d) {
  node_t **nodes = NULL;
  int count = 0, idx = -1;
  nodes = gather_focused(d, &count, &idx);
  if (!nodes) return false;

  int mc = master_count_clamped(count);
  int target = -1;
  if (is_master(idx, mc) && mc < count)
    target = mc;
  else if (!is_master(idx, mc) && mc >= 1)
    target = 0;
  else
    target = (idx + 1 < count) ? idx + 1 : 0;

  free(nodes);
  return swap_with_nth(m, d, target);
}

bool master_stack_swap_west(output_t *m, desktop_t *d) {
  node_t **nodes = NULL;
  int count = 0, idx = -1;
  nodes = gather_focused(d, &count, &idx);
  if (!nodes) return false;

  int mc = master_count_clamped(count);
  int target = -1;
  if (is_master(idx, mc) && mc < count)
    target = count - 1;
  else if (!is_master(idx, mc) && mc >= 1)
    target = mc - 1;
  else
    target = (idx - 1 >= 0) ? idx - 1 : count - 1;

  free(nodes);
  return swap_with_nth(m, d, target);
}
