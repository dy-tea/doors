#include "animation.h"
#include "bezier.h"
#include "blur.h"
#include "config.h"
#include "input.h"
#include "ipc.h"
#include "ipc_helpers.h"
#include "keyboard.h"
#include "output.h"
#include "output_config.h"
#include "rule.h"
#include "server.h"
#include "scroller.h"
#include "scratchpad.h"
#include "spring.h"
#include "tabs.h"
#include "text.h"
#include "toplevel.h"
#include "transaction.h"
#include "tree.h"
#include "workspace.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>

void send_success(int client_fd, const char *msg);
void send_failure(int client_fd, const char *msg);

void toplevel_map(struct wl_listener *listener, void *data);

static void ipc_cmd_quit(char **args, int num, int client_fd) {
  (void)args;
  (void)num;
  wlr_log(WLR_INFO, "Quit requested via IPC");
  wl_display_terminate(server.wl_display);
  send_success(client_fd, "quit\n");
}

static void ipc_cmd_output(char **args, int num, int client_fd) {
  if (num < 1) {
    send_failure(client_fd, "output: missing arguments\n");
    return;
  }

  if (streq("list", *args) || streq("--list", *args) || streq("-l", *args)) {
    char buf[DOORS_BUFSIZ];
    int offset = 0;
    offset += snprintf(buf + offset, sizeof(buf) - offset, "[\n");
    bool first = true;
    for (output_t *output = mon_head; output != NULL; output = output->next) {
      struct wlr_output *wo = output->wlr_output;
      if (!first)
        offset += snprintf(buf + offset, sizeof(buf) - offset, ",\n");
      first = false;
      offset += snprintf(buf + offset, sizeof(buf) - offset,
        "  {\n"
        "    \"name\": \"%s\",\n"
        "    \"description\": \"%s\",\n"
        "    \"make\": \"%s\",\n"
        "    \"model\": \"%s\",\n"
        "    \"serial\": \"%s\",\n"
        "    \"width\": %d,\n"
        "    \"height\": %d,\n"
        "    \"refresh\": %.3f,\n"
        "    \"scale\": %.6g,\n"
        "    \"phys_width\": %d,\n"
        "    \"phys_height\": %d,\n"
        "    \"enabled\": %s\n"
        "  }",
        wo->name ? wo->name : "",
        wo->description ? wo->description : "",
        wo->make ? wo->make : "",
        wo->model ? wo->model : "",
        wo->serial ? wo->serial : "",
        wo->width, wo->height, wo->refresh / 1000.0f,
        wo->scale,
        wo->phys_width, wo->phys_height,
        wo->enabled ? "true" : "false");
    }
    offset += snprintf(buf + offset, sizeof(buf) - offset, "\n]\n");
    send_success(client_fd, buf);
    return;
  }

  char *output_name = *args;
  args++;
  num--;

  if (num < 1) {
    send_failure(client_fd, "output: missing subcommand\n");
    return;
  }

  struct output_config *oc = output_config_find(output_name);
  bool oc_new = false;
  if (!oc) {
    oc = output_config_create(output_name);
    if (!oc) {
      send_failure(client_fd, "output: failed to create config\n");
      return;
    }
    oc_new = true;
  }

  if (oc_new)
    output_config_add(oc);

  output_t *mon = find_output_by_name(output_name);
  char *subcmd = *args;

  if (streq("enable", subcmd)) {
    oc->enable = OUTPUT_CONFIG_ENABLE;
    output_config_apply(oc);
    send_success(client_fd, "output enabled\n");
  } else if (streq("disable", subcmd)) {
    oc->enable = OUTPUT_CONFIG_DISABLE;
    output_config_apply(oc);
    send_success(client_fd, "output disabled\n");
  } else if (streq("mode", subcmd) || streq("resolution", subcmd) || streq("res", subcmd)) {
    if (num < 2) {
      send_failure(client_fd, "output mode: missing resolution\n");
      return;
    }
    args++;
    num--;

    char *res = *args;
    int width, height;
    float refresh_rate = -1;

    char *at = strchr(res, '@');
    if (at) {
      *at = '\0';
      if (sscanf(at + 1, "%f", &refresh_rate) != 1) {
        send_failure(client_fd, "output mode: invalid refresh rate\n");
        return;
      }
    }

    if (sscanf(res, "%dx%d", &width, &height) != 2) {
      send_failure(client_fd, "output mode: invalid resolution format\n");
      return;
    }

    oc->width = width;
    oc->height = height;
    oc->refresh_rate = refresh_rate;
    output_config_apply(oc);
    send_success(client_fd, "output mode set\n");
  } else if (streq("modes", subcmd)) {
    output_t *target = NULL;
    for (output_t *o = mon_head; o != NULL; o = o->next) {
      if (streq(o->name, output_name) || streq(o->wlr_output->name, output_name)) {
        target = o;
        break;
      }
    }

    if (!target) {
      send_failure(client_fd, "output modes: no such output\n");
      return;
    }

    struct wlr_output *wo = target->wlr_output;
    char buf[DOORS_BUFSIZ];
    int offset = 0;
    offset += snprintf(buf + offset, sizeof(buf) - offset, "[\n");
    bool first = true;

    struct wlr_output_mode *mode;
    wl_list_for_each(mode, &wo->modes, link) {
      if (!first) offset += snprintf(buf + offset, sizeof(buf) - offset, ",\n");

      first = false;
      offset += snprintf(buf + offset, sizeof(buf) - offset,
        "  {\n"
        "    \"width\": %d,\n"
        "    \"height\": %d,\n"
        "    \"refresh\": %.3f\n"
        "  }",
        mode->width, mode->height, mode->refresh / 1000.0f);
    }

    offset += snprintf(buf + offset, sizeof(buf) - offset, "\n]\n");
    send_success(client_fd, buf);
  } else if (streq("position", subcmd) || streq("pos", subcmd)) {
    if (num < 3) {
      send_failure(client_fd, "output position: missing coordinates\n");
      return;
    }
    args++;
    num--;

    int x, y;
    if (sscanf(*args, "%d", &x) != 1) {
      send_failure(client_fd, "output position: invalid x\n");
      return;
    }
    args++;
    num--;

    if (sscanf(*args, "%d", &y) != 1) {
      send_failure(client_fd, "output position: invalid y\n");
      return;
    }

    oc->x = x;
    oc->y = y;
    output_config_apply(oc);
    send_success(client_fd, "output position set\n");
  } else if (streq("scale", subcmd)) {
    if (num < 2) {
      send_failure(client_fd, "output scale: missing scale factor\n");
      return;
    }
    args++;
    num--;

    float scale;
    if (sscanf(*args, "%f", &scale) != 1) {
      send_failure(client_fd, "output scale: invalid scale\n");
      return;
    }

    oc->scale = scale;
    output_config_apply(oc);
    send_success(client_fd, "output scale set\n");
  } else if (streq("transform", subcmd)) {
    if (num < 2) {
      send_failure(client_fd, "output transform: missing transform\n");
      return;
    }
    args++;
    num--;

    int transform = -1;
    if (streq("normal", *args) || streq("0", *args)) {
      transform = WL_OUTPUT_TRANSFORM_NORMAL;
    } else if (streq("90", *args)) {
      transform = WL_OUTPUT_TRANSFORM_90;
    } else if (streq("180", *args)) {
      transform = WL_OUTPUT_TRANSFORM_180;
    } else if (streq("270", *args)) {
      transform = WL_OUTPUT_TRANSFORM_270;
    } else if (streq("flipped", *args) || streq("flipped-180", *args)) {
      transform = WL_OUTPUT_TRANSFORM_FLIPPED_180;
    } else if (streq("flipped-90", *args)) {
      transform = WL_OUTPUT_TRANSFORM_FLIPPED_90;
    } else if (streq("flipped-270", *args)) {
      transform = WL_OUTPUT_TRANSFORM_FLIPPED_270;
    }

    if (transform < 0) {
      send_failure(client_fd, "output transform: invalid transform\n");
      return;
    }

    oc->transform = transform;
    output_config_apply(oc);
    send_success(client_fd, "output transform set\n");
  } else if (streq("dpms", subcmd)) {
    if (num < 2) {
      send_failure(client_fd, "output dpms: missing state\n");
      return;
    }
    args++;
    num--;

    if (streq("on", *args)) {
      oc->dpms_state = OUTPUT_CONFIG_DPMS_ON;
    } else if (streq("off", *args)) {
      oc->dpms_state = OUTPUT_CONFIG_DPMS_OFF;
    } else {
      send_failure(client_fd, "output dpms: invalid state (on/off)\n");
      return;
    }

    output_config_apply(oc);
    send_success(client_fd, "output dpms set\n");
  } else if (streq("adaptive_sync", subcmd) || streq("vrr", subcmd)) {
    if (num < 2) {
      send_failure(client_fd, "output adaptive_sync: missing state\n");
      return;
    }
    args++;
    num--;

    if (streq("on", *args) || streq("enable", *args)) {
      oc->adaptive_sync = OUTPUT_CONFIG_ADAPTIVE_SYNC_ENABLED;
    } else if (streq("off", *args) || streq("disable", *args)) {
      oc->adaptive_sync = OUTPUT_CONFIG_ADAPTIVE_SYNC_DISABLED;
    } else {
      send_failure(client_fd, "output adaptive_sync: invalid state (on/off)\n");
      return;
    }

    output_config_apply(oc);
    send_success(client_fd, "output adaptive_sync set\n");
  } else if (streq("render_bit_depth", subcmd)) {
    if (num < 2) {
      send_failure(client_fd, "output render_bit_depth: missing value\n");
      return;
    }
    args++;
    num--;

    if (streq("8", *args) || streq("8-bit", *args)) {
      oc->render_bit_depth = OUTPUT_CONFIG_RENDER_BIT_DEPTH_8;
    } else if (streq("10", *args) || streq("10-bit", *args)) {
      oc->render_bit_depth = OUTPUT_CONFIG_RENDER_BIT_DEPTH_10;
    } else {
      send_failure(client_fd, "output render_bit_depth: invalid value (8/10)\n");
      return;
    }

    output_config_apply(oc);
    send_success(client_fd, "output render_bit_depth set\n");
  } else if (streq("color_profile", subcmd)) {
    if (num < 2) {
      send_failure(client_fd, "output color_profile: missing profile type (gamma22|srgb|icc <path>)\n");
      return;
    }
    args++;
    num--;

    struct wlr_color_transform *new_transform = NULL;
    if (streq("gamma22", *args)) {
    	// no transform
    } else if (streq("srgb", *args)) {
      new_transform = wlr_color_transform_init_linear_to_inverse_eotf(
          WLR_COLOR_TRANSFER_FUNCTION_SRGB);
      if (!new_transform) {
        send_failure(client_fd, "output color_profile: failed to create sRGB transform\n");
        return;
      }
    } else if (streq("icc", *args)) {
      if (num < 2) {
        send_failure(client_fd, "output color_profile icc: missing file path\n");
        return;
      }
      args++;
      num--;

      int fd = open(*args, O_RDONLY | O_NOCTTY | O_CLOEXEC);
      if (fd == -1) {
        send_failure(client_fd, "output color_profile icc: cannot open file\n");
        return;
      }
      struct stat info;
      if (fstat(fd, &info) == -1 || !S_ISREG(info.st_mode) || info.st_size <= 0) {
        close(fd);
        send_failure(client_fd, "output color_profile icc: invalid file\n");
        return;
      }
      void *icc_data = malloc(info.st_size);
      if (!icc_data) {
        close(fd);
        send_failure(client_fd, "output color_profile icc: out of memory\n");
        return;
      }
      size_t nread = 0;
      while (nread < (size_t)info.st_size) {
        ssize_t r = read(fd, (char *)icc_data + nread, (size_t)info.st_size - nread);
        if ((r == -1 && errno != EINTR) || r == 0) {
          free(icc_data);
          close(fd);
          send_failure(client_fd, "output color_profile icc: read error\n");
          return;
        }
        nread += (size_t)r;
      }
      close(fd);

      new_transform = wlr_color_transform_init_linear_to_icc(icc_data, (size_t)info.st_size);
      free(icc_data);
      if (!new_transform) {
        send_failure(client_fd, "output color_profile icc: failed to initialize ICC transform\n");
        return;
      }
    } else {
      send_failure(client_fd, "output color_profile: invalid type (gamma22|srgb|icc <path>)\n");
      return;
    }

    wlr_color_transform_unref(oc->color_transform);
    oc->color_transform = new_transform;
    output_config_apply(oc);
    send_success(client_fd, "output color_profile set\n");
  } else if (streq("tearing", subcmd)) {
    if (num < 2) {
      send_failure(client_fd, "output tearing: missing state\n");
      return;
    }
    args++;
    num--;

    if (streq("on", *args) || streq("enable", *args) || streq("true", *args)) {
      oc->allow_tearing = 1;
    } else if (streq("off", *args) || streq("disable", *args) || streq("false", *args)) {
      oc->allow_tearing = 0;
    } else {
      send_failure(client_fd, "output tearing: invalid state (on/off)\n");
      return;
    }

    output_config_apply(oc);
    send_success(client_fd, "output tearing set\n");
  } else if (streq("focus", subcmd) || streq("-f", subcmd) || streq("--focus", subcmd)) {
    if (!mon) {
      send_failure(client_fd, "output focus: no such output\n");
      return;
    }
    server.focused_output = mon;
    focus_node(mon, mon->desk, mon->desk ? mon->desk->focus : NULL);
    send_success(client_fd, "focused\n");
  } else if (streq("rename", subcmd) || streq("-n", subcmd) || streq("--rename", subcmd)) {
    if (num < 2) {
      send_failure(client_fd, "output rename: missing name argument\n");
      return;
    }
    args++;
    num--;
    strncpy(mon->name, *args, SMALEN - 1);
    mon->name[SMALEN - 1] = '\0';
    transaction_commit_dirty();
    send_success(client_fd, "renamed\n");
  } else if (streq("add-desktops", subcmd) || streq("-a", subcmd) || streq("--add-desktops", subcmd)) {
    if (num < 2) {
      send_failure(client_fd, "output add-desktops: missing desktop names\n");
      return;
    }
    args++;
    num--;
    while (num > 0) {
      desktop_t *d = (desktop_t *)calloc(1, sizeof(desktop_t));
      d->id = next_desktop_id++;
      strncpy(d->name, *args, SMALEN - 1);
      d->name[SMALEN - 1] = '\0';
      d->layout = LAYOUT_TILED;
      d->user_layout = LAYOUT_TILED;
      d->window_gap = window_gap;
      d->border_width = border_width;
      d->padding = (padding_t){0};
      d->root = NULL;
      d->focus = NULL;

      if (mon->desk_tail) {
        d->prev = mon->desk_tail;
        mon->desk_tail->next = d;
        mon->desk_tail = d;
      } else {
        mon->desk = d;
        mon->desk_head = d;
        mon->desk_tail = d;
      }
      d->output = mon;

      workspace_create_desktop(d->name);

      args++;
      num--;
    }
    transaction_commit_dirty();
    send_success(client_fd, "desktops added\n");
  } else if (streq("desktops", subcmd) || streq("-d", subcmd) || streq("--desktops", subcmd)) {
    if (!mon) {
      send_failure(client_fd, "output desktops: no such output\n");
      return;
    }
    if (num < 2) {
      char buf[DOORS_BUFSIZ];
      size_t offset = 0;
      for (desktop_t *d = mon->desk_head; d != NULL; d = d->next) {
        offset += snprintf(buf + offset, sizeof(buf) - offset, "%s\n", d->name);
      }
      send_success(client_fd, buf);
    } else {
      args++;
      num--;

      desktop_t *d = mon->desk_head;
      for (; num > 0 && d != NULL; d = d->next) {
        strncpy(d->name, *args, SMALEN - 1);
        d->name[SMALEN - 1] = '\0';
        workspace_create_desktop(d->name);
        args++;
        num--;
      }

      while (num > 0) {
        desktop_t *newd = (desktop_t *)calloc(1, sizeof(desktop_t));
        newd->id = next_desktop_id++;
        strncpy(newd->name, *args, SMALEN - 1);
        newd->name[SMALEN - 1] = '\0';
        newd->layout = LAYOUT_TILED;
        newd->user_layout = LAYOUT_TILED;
        newd->window_gap = window_gap;
        newd->border_width = border_width;
        newd->padding = (padding_t){0};
        newd->root = NULL;
        newd->focus = NULL;

        if (mon->desk_tail) {
          newd->prev = mon->desk_tail;
          mon->desk_tail->next = newd;
          mon->desk_tail = newd;
        } else {
          mon->desk = newd;
          mon->desk_head = newd;
          mon->desk_tail = newd;
        }
        newd->output = mon;

        workspace_create_desktop(newd->name);
        args++;
        num--;
      }

      while (d != NULL) {
        if (d == mon->desk) {
          mon->desk = d->next;
          if (mon->desk)
            mon->desk->prev = NULL;
          if (mon->desk_head == d)
            mon->desk_head = d->next;
          if (mon->desk_tail == d)
            mon->desk_tail = d->prev;
          if (mon->desk)
            focus_node(mon, mon->desk, mon->desk->focus);
        } else {
          if (d->prev)
            d->prev->next = d->next;
          if (d->next)
            d->next->prev = d->prev;
          if (mon->desk_tail == d)
            mon->desk_tail = d->prev;
        }
        desktop_t *next = d->next;
        free(d);
        d = next;
      }

      transaction_commit_dirty();
      workspace_sync();

      if (mon->desk) {
        focus_node(mon, mon->desk, mon->desk->focus);
      }

      send_success(client_fd, "desktops reset\n");
    }
  } else if (streq("swap-desktops", subcmd) || streq("-s", subcmd) || streq("--swap", subcmd)) {
    if (num < 2) {
      send_failure(client_fd, "output swap-desktops: missing target output\n");
      return;
    }
    args++;
    num--;

    output_t *target = find_output_by_name(*args);
    if (!target) {
      send_failure(client_fd, "output swap-desktops: target output not found\n");
      return;
    }

    if (target == mon) {
      send_failure(client_fd, "output swap-desktops: cannot swap with self\n");
      return;
    }

    output_t *m0 = mon;
    output_t *m1 = target;

    desktop_t *d0 = m0->desk;
    desktop_t *d1 = m1->desk;

    m0->desk = d1;
    m0->desk_head = d1 ? d1 : m0->desk_head;
    m0->desk_tail = d1 ? (d1->prev ? d1->prev : d1) : m0->desk_tail;

    m1->desk = d0;
    m1->desk_head = d0 ? d0 : m1->desk_head;
    m1->desk_tail = d0 ? (d0->prev ? d0->prev : d0) : m1->desk_tail;

    if (d0) {
      for (desktop_t *d = d0; d != NULL; d = d->next)
        d->output = m1;
      d0->prev = NULL;
      desktop_t *tail = d0;
      for (; tail->next; tail = tail->next)
        ;
      tail->next = d1;
      if (d1) d1->prev = tail;
    }

    if (d1)
      for (desktop_t *d = d1; d != NULL; d = d->next)
        d->output = m0;

    if (server.focused_output == m0)
      server.focused_output = m1;
    else if (server.focused_output == m1)
      server.focused_output = m0;

    transaction_commit_dirty();
    send_success(client_fd, "swapped\n");
  } else if (streq("remove", subcmd) || streq("-r", subcmd) || streq("--remove", subcmd)) {
    if (!mon->prev && !mon->next) {
      send_failure(client_fd, "output remove: cannot remove the only output\n");
      return;
    }

    if (mon->desk) {
      send_failure(client_fd, "output remove: cannot remove output with desktops\n");
      return;
    }

    output_t *prev = mon->prev;
    output_t *next = mon->next;

    if (prev)
      prev->next = next;
    else if (mon_head == mon)
      mon_head = next;

    if (next)
      next->prev = prev;

    if (server.focused_output == mon) {
      server.focused_output = next ? next : prev;
      if (server.focused_output) {
        focus_node(server.focused_output, server.focused_output->desk,
          server.focused_output->desk ? server.focused_output->desk->focus : NULL);
      }
    }

    free(mon);
    transaction_commit_dirty();
    send_success(client_fd, "removed\n");
  } else if (streq("rectangle", subcmd) || streq("-g", subcmd) || streq("--rectangle", subcmd)) {
    if (num < 2) {
      send_failure(client_fd, "output rectangle: missing rectangle\n");
      return;
    }
    args++;
    num--;

    int x, y, width, height;
    if (sscanf(*args, "%dx%d:%d,%d", &width, &height, &x, &y) != 4 &&
        sscanf(*args, "%dx%d", &width, &height) == 2) {
      x = mon->rectangle.x;
      y = mon->rectangle.y;
    } else if (sscanf(*args, "%d,%d,%d,%d", &x, &y, &width, &height) != 4) {
      send_failure(client_fd, "output rectangle: invalid rectangle format\n");
      return;
    }

    mon->rectangle.x = x;
    mon->rectangle.y = y;
    mon->rectangle.width = width;
    mon->rectangle.height = height;

    transaction_commit_dirty();
    send_success(client_fd, "rectangle set\n");
  } else if (streq("reorder-desktops", subcmd) || streq("-o", subcmd) || streq("--reorder-desktops", subcmd)) {
    if (num < 2) {
      send_failure(client_fd, "output reorder-desktops: missing desktop names\n");
      return;
    }
    args++;
    num--;

    desktop_t *d = mon->desk;
    while (d != NULL && num > 0) {
      desktop_t *next = d->next;
      for (int i = 0; i < num; i++) {
        if (strcmp(d->name, args[i]) == 0) {
          strncpy(d->name, args[i], SMALEN - 1);
          d->name[SMALEN - 1] = '\0';
          break;
        }
      }
      d = next;
    }

    transaction_commit_dirty();
    send_success(client_fd, "desktops reordered\n");
  } else {
    send_failure(client_fd, "output: unknown subcommand\n");
  }
}

static void ipc_cmd_input(char **args, int num, int client_fd) {
  if (num < 1) {
    send_failure(client_fd, "input: missing arguments\n");
    return;
  }

  char *identifier = NULL;
  enum input_config_type type = INPUT_CONFIG_TYPE_ANY;
  if (num > 0 && strncmp(*args, "type:", 5) != 0) {
    identifier = *args;
    args++;
    num--;
  }

  if (num > 0 && strncmp(*args, "type:", 5) == 0) {
    char *type_str = *args + 5;
    while (*type_str == ' ') type_str++;

    if (streq(type_str, "keyboard")) {
      type = INPUT_CONFIG_TYPE_KEYBOARD;
    } else if (streq(type_str, "pointer")) {
      type = INPUT_CONFIG_TYPE_POINTER;
    } else if (streq(type_str, "touchpad")) {
      type = INPUT_CONFIG_TYPE_TOUCHPAD;
    } else if (streq(type_str, "touchscreen")) {
      type = INPUT_CONFIG_TYPE_TOUCH;
    } else if (streq(type_str, "tablet")) {
      type = INPUT_CONFIG_TYPE_TABLET;
    } else if (streq(type_str, "tablet_pad")) {
      type = INPUT_CONFIG_TYPE_TABLET_PAD;
    } else if (streq(type_str, "switch")) {
      type = INPUT_CONFIG_TYPE_SWITCH;
    } else if (streq(type_str, "any")) {
      type = INPUT_CONFIG_TYPE_ANY;
    } else {
      send_failure(client_fd, "input: unknown type\n");
      return;
    }

    args++;
    num--;
  }

  if (num < 1) {
    send_failure(client_fd, "input: missing property\n");
    return;
  }

  char *property = *args;
  args++;
  num--;

  char *value = "";
  if (num > 0)
    value = *args;

  input_config_t *config = NULL;
  for (size_t i = 0; i < num_input_configs; i++) {
    input_config_t *cfg = input_configs[i];
    if (identifier) {
      if (cfg->identifier && strcmp(cfg->identifier, identifier) == 0) {
        config = cfg;
        break;
      }
    } else if (cfg->type == type && cfg->identifier == NULL) {
      config = cfg;
      break;
    }
  }

  if (!config) {
    config = input_config_create(identifier);
    if (!config) {
      send_failure(client_fd, "input: failed to create config\n");
      return;
    }
    config->type = type;
    input_config_add(config);
  }

  if (!input_config_set_value(config, property, value)) {
    send_failure(client_fd, "input: unknown property\n");
    return;
  }

  if (!config) {
    send_failure(client_fd, "input: failed to create config\n");
    return;
  }
  config->type = type;

  if (!input_config_set_value(config, property, value)) {
    input_config_destroy(config);
    send_failure(client_fd, "input: unknown property\n");
    return;
  }

  input_apply_config_all_pointers();
  input_apply_config_all_keyboards();
}

static void ipc_cmd_node(char **args, int num, int client_fd) {
  if (num < 1) {
    send_failure(client_fd, "node: Missing arguments\n");
    return;
  }

  if (streq("-f", *args) || streq("--focus", *args)) {
    output_t *m = server.focused_output;
    if (m && m->desk && m->desk->focus && m->desk->focus->client) {
      focus_node(m, m->desk, m->desk->focus);
      send_success(client_fd, "focused\n");
    } else {
      send_failure(client_fd, "no focused node\n");
    }
  } else if (streq("-c", *args) || streq("--close", *args)) {
    output_t *m = server.focused_output;
    if (m && m->desk && m->desk->focus && m->desk->focus->client) {
      kill_node(m->desk, m->desk->focus);
      send_success(client_fd, "closed\n");
    } else {
      send_failure(client_fd, "no focused node to close\n");
    }
  } else if (streq("-t", *args) || streq("--state", *args)) {
    if (num < 2) {
      send_failure(client_fd, "node -t: missing state argument\n");
      return;
    }
    args++;
    num--;
    if (streq("tiled", *args)) {
      tile_focused();
    } else if (streq("floating", *args)) {
      toggle_floating();
    } else if (streq("fullscreen", *args)) {
      toggle_fullscreen();
    } else {
      send_failure(client_fd, "node -t: unknown state\n");
      return;
    }

    output_t *m = server.focused_output;
    if (m && m->desk && m->desk->focus && m->desk->focus->client) {
      send_success(client_fd, "state changed\n");
    } else {
      send_failure(client_fd, "no focused node\n");
    }
  } else if (streq("-d", *args) || streq("--to-desktop", *args)) {
    if (num < 2) {
      send_failure(client_fd, "node -d: missing desktop name\n");
      return;
    }
    args++;
    num--;
    char *desk_name = *args;
    desktop_t *target = find_desktop_by_name(desk_name);
    if (!target) {
      send_failure(client_fd, "node -d: desktop not found\n");
      return;
    }

    output_t *m = server.focused_output;
    if (!m || !m->desk || !m->desk->focus) {
      send_failure(client_fd, "node -d: no focused node\n");
      return;
    }

    if (m->desk == target) {
      send_failure(client_fd, "node -d: already on target desktop\n");
      return;
    }

    node_t *n = m->desk->focus;
    if (n == NULL || n->client == NULL) {
      send_failure(client_fd, "node -d: no client\n");
      return;
    }

    desktop_t *src_desk = m->desk;

    n->destroying = false;
    n->ntxnrefs = 0;

    n->client->shown = false;
    struct wlr_scene_tree *scene_tree = client_get_scene_tree(n->client);
    if (scene_tree)
      wlr_scene_node_set_enabled(&scene_tree->node, false);

    remove_node(src_desk, n);

    if (src_desk != target && src_desk->root != NULL) {
      node_t *new_focus = first_extrema(src_desk->root);
      if (new_focus != NULL) {
        src_desk->focus = new_focus;
        focus_node(m, src_desk, new_focus);
      } else {
        src_desk->focus = NULL;
      }
    } else {
      src_desk->focus = NULL;
    }

    n->destroying = false;
    n->ntxnrefs = 0;

    insert_node(target, n, find_public(target));

    target->focus = n;
    if (target == m->desk && target->focus == n) {
      focus_node(m, target, n);
    }

    if (target == m->desk) {
      for (node_t *n_iter = first_extrema(target->root); n_iter != NULL; n_iter = next_leaf(n_iter, target->root)) {
        if (n_iter->client) {
          n_iter->client->shown = true;
          bool already_configured = true;
          if (n_iter->client->toplevel)
            already_configured = n_iter->client->toplevel->configured;
          if (already_configured) {
            struct wlr_scene_tree *scene_tree = client_get_scene_tree(n_iter->client);
            if (scene_tree)
              wlr_scene_node_set_enabled(&scene_tree->node, true);
          }
        }
      }
      arrange(m, target, true);
    } else {
      for (node_t *n_iter = first_extrema(target->root); n_iter != NULL; n_iter = next_leaf(n_iter, target->root)) {
        if (n_iter->client) {
          n_iter->client->shown = false;
          struct wlr_scene_tree *scene_tree = client_get_scene_tree(n_iter->client);
          if (scene_tree)
            wlr_scene_node_set_enabled(&scene_tree->node, false);
        }
      }
      arrange(m, target, false);
    }

    if (src_desk == m->desk) {
      for (node_t *n_iter = first_extrema(src_desk->root); n_iter != NULL; n_iter = next_leaf(n_iter, src_desk->root)) {
        if (n_iter->client) {
          n_iter->client->shown = true;
          bool already_configured = true;
          if (n_iter->client->toplevel)
            already_configured = n_iter->client->toplevel->configured;
          if (already_configured) {
            struct wlr_scene_tree *scene_tree = client_get_scene_tree(n_iter->client);
            if (scene_tree)
              wlr_scene_node_set_enabled(&scene_tree->node, true);
          }
        }
      }
      arrange(m, src_desk, true);
    } else if (src_desk->root) {
      for (node_t *n_iter = first_extrema(src_desk->root); n_iter != NULL; n_iter = next_leaf(n_iter, src_desk->root)) {
        if (n_iter->client) {
          n_iter->client->shown = false;
          struct wlr_scene_tree *scene_tree = client_get_scene_tree(n_iter->client);
          if (scene_tree)
            wlr_scene_node_set_enabled(&scene_tree->node, false);
        }
      }
    }

    send_success(client_fd, "node sent to desktop\n");
  } else if (streq("-g", *args) || streq("--flag", *args)) {
    if (num < 2) {
      send_failure(client_fd, "node -g: missing flag argument\n");
      return;
    }
    args++;
    num--;

    output_t *m = server.focused_output;
    if (!m || !m->desk) {
      send_failure(client_fd, "node -g: no focused desktop\n");
      return;
    }

    node_t *n = m->desk->focus;
    if (!n) {
      send_failure(client_fd, "node -g: no focused node\n");
      return;
    }

    char *key = strtok(*args, "=");
    char *val = strtok(NULL, "=");

    bool set_value = false;
    bool has_value = false;

    if (val == NULL) {
      has_value = false;
    } else {
   		has_value = true;
      if (strcmp(val, "true") == 0 || strcmp(val, "on") == 0 || strcmp(val, "1") == 0) {
        set_value = true;
      } else if (strcmp(val, "false") == 0 || strcmp(val, "off") == 0 || strcmp(val, "0") == 0) {
        set_value = false;
      }
    }

    if (strcmp(key, "hidden") == 0) {
      n->hidden = has_value ? set_value : !n->hidden;
      transaction_commit_dirty();
      send_success(client_fd, "flag changed\n");
    } else if (strcmp(key, "sticky") == 0) {
      n->sticky = has_value ? set_value : !n->sticky;
      transaction_commit_dirty();
      send_success(client_fd, "flag changed\n");
    } else if (strcmp(key, "private") == 0) {
      n->private_node = has_value ? set_value : !n->private_node;
      transaction_commit_dirty();
      send_success(client_fd, "flag changed\n");
    } else if (strcmp(key, "locked") == 0) {
      n->locked = has_value ? set_value : !n->locked;
      transaction_commit_dirty();
      send_success(client_fd, "flag changed\n");
    } else if (strcmp(key, "marked") == 0) {
      n->marked = has_value ? set_value : !n->marked;
      transaction_commit_dirty();
      send_success(client_fd, "flag changed\n");
    } else if (strcmp(key, "blur") == 0) {
      if (!n->client) {
        send_failure(client_fd, "node -g: no client\n");
        return;
      }
      bool new_blur = has_value ? set_value : !n->client->blur;
      n->client->blur = new_blur;
      if (n->client->toplevel)
        toplevel_set_blur(n->client->toplevel, new_blur);
      send_success(client_fd, "flag changed\n");
    } else if (strcmp(key, "mica") == 0) {
      if (!n->client) {
        send_failure(client_fd, "node -g: no client\n");
        return;
      }
      bool new_val = has_value ? set_value : !n->client->mica;
      n->client->mica = new_val;
      if (n->client->toplevel)
        toplevel_set_mica(n->client->toplevel, new_val);
      send_success(client_fd, "flag changed\n");
    } else if (strcmp(key, "acrylic") == 0) {
      if (!n->client) {
        send_failure(client_fd, "node -g: no client\n");
        return;
      }
      bool new_val = has_value ? set_value : !n->client->acrylic;
      n->client->acrylic = new_val;
      if (n->client->toplevel)
        toplevel_set_acrylic(n->client->toplevel, new_val);
      send_success(client_fd, "flag changed\n");
    } else if (strncmp(key, "border_radius", 13) == 0) {
      if (!n->client) {
        send_failure(client_fd, "node -g: no client\n");
        return;
      }
      float r = atof(key + 14);
      if (n->client->toplevel)
        toplevel_set_border_radius(n->client->toplevel, r);
      else if (n->client)
        n->client->border_radius = r;
      send_success(client_fd, "border_radius set\n");
    } else {
      send_failure(client_fd, "node -g: unknown flag\n");
      return;
    }
  } else if (streq("-S", *args) || streq("--scratchpad", *args)) {
    output_t *m = server.focused_output;
    if (!m || !m->desk) {
      send_failure(client_fd, "node -S: no focused desktop\n");
      return;
    }
    node_t *n = m->desk->focus;
    if (!n || !n->client) {
      send_failure(client_fd, "node -S: no focused client\n");
      return;
    }
    scratchpad_add(n);
    send_success(client_fd, "sent to scratchpad\n");
  } else if (streq("-v", *args) || streq("--move", *args)) {
    if (num < 3) {
      send_failure(client_fd, "node -v: missing delta arguments\n");
      return;
    }
    args++;
    num--;

    output_t *m = server.focused_output;
    if (!m || !m->desk) {
      send_failure(client_fd, "node -v: no focused desktop\n");
      return;
    }

    node_t *n = m->desk->focus;
    if (!n || !n->client) {
      send_failure(client_fd, "node -v: no focused client\n");
      return;
    }

    int dx = 0, dy = 0;
    if (sscanf(*args, "%d", &dx) != 1) {
      send_failure(client_fd, "node -v: invalid dx\n");
      return;
    }
    args++;
    num--;

    if (sscanf(*args, "%d", &dy) != 1) {
      send_failure(client_fd, "node -v: invalid dy\n");
      return;
    }

    n->client->state = STATE_FLOATING;
    n->client->floating_rectangle.x += dx;
    n->client->floating_rectangle.y += dy;

    if (n->client->toplevel && n->client->toplevel->scene_tree) {
      wlr_scene_node_set_position(&n->client->toplevel->scene_tree->node,
        n->client->floating_rectangle.x, n->client->floating_rectangle.y);
    } else if (n->client->xwayland_view && n->client->xwayland_view->scene_tree) {
      wlr_scene_node_set_position(&n->client->xwayland_view->scene_tree->node,
        n->client->floating_rectangle.x, n->client->floating_rectangle.y);
    }

    transaction_commit_dirty();
    send_success(client_fd, "moved\n");
  } else if (streq("-z", *args) || streq("--resize", *args)) {
    if (num < 4) {
      send_failure(client_fd, "node -z: missing arguments\n");
      return;
    }
    args++;
    num--;

    output_t *m = server.focused_output;
    if (!m || !m->desk) {
      send_failure(client_fd, "node -z: no focused desktop\n");
      return;
    }

    node_t *n = m->desk->focus;
    if (!n || !n->client) {
      send_failure(client_fd, "node -z: no focused client\n");
      return;
    }

    char *handle = *args;
    int dx = 0, dy = 0;

    args++;
    num--;

    if (sscanf(*args, "%d", &dx) != 1) {
      send_failure(client_fd, "node -z: invalid dx\n");
      return;
    }
    args++;
    num--;

    if (sscanf(*args, "%d", &dy) != 1) {
      send_failure(client_fd, "node -z: invalid dy\n");
      return;
    }

    n->client->state = STATE_FLOATING;

    if (strcmp(handle, "northwest") == 0 || strcmp(handle, "nw") == 0 || strcmp(handle, "left") == 0) {
      n->client->floating_rectangle.x += dx;
      n->client->floating_rectangle.y += dy;
      n->client->floating_rectangle.width -= dx;
      n->client->floating_rectangle.height -= dy;
    } else if (strcmp(handle, "north") == 0 || strcmp(handle, "n") == 0) {
      n->client->floating_rectangle.y += dy;
      n->client->floating_rectangle.height -= dy;
    } else if (strcmp(handle, "northeast") == 0 || strcmp(handle, "ne") == 0) {
      n->client->floating_rectangle.y += dy;
      n->client->floating_rectangle.width += dx;
      n->client->floating_rectangle.height -= dy;
    } else if (strcmp(handle, "east") == 0 || strcmp(handle, "e") == 0 || strcmp(handle, "right") == 0) {
      n->client->floating_rectangle.width += dx;
    } else if (strcmp(handle, "southeast") == 0 || strcmp(handle, "se") == 0) {
      n->client->floating_rectangle.width += dx;
      n->client->floating_rectangle.height += dy;
    } else if (strcmp(handle, "south") == 0 || strcmp(handle, "s") == 0) {
      n->client->floating_rectangle.height += dy;
    } else if (strcmp(handle, "southwest") == 0 || strcmp(handle, "sw") == 0) {
      n->client->floating_rectangle.x += dx;
      n->client->floating_rectangle.width -= dx;
      n->client->floating_rectangle.height += dy;
    } else if (strcmp(handle, "west") == 0 || strcmp(handle, "w") == 0) {
      n->client->floating_rectangle.x += dx;
      n->client->floating_rectangle.width -= dx;
    } else if (strcmp(handle, "center") == 0 || strcmp(handle, "c") == 0) {
      n->client->floating_rectangle.x += dx;
      n->client->floating_rectangle.y += dy;
      n->client->floating_rectangle.width += dx;
      n->client->floating_rectangle.height += dy;
    } else {
      send_failure(client_fd, "node -z: invalid resize handle\n");
      return;
    }

    if (n->client->floating_rectangle.width < 50)
      n->client->floating_rectangle.width = 50;
    if (n->client->floating_rectangle.height < 50)
      n->client->floating_rectangle.height = 50;

    if (n->client->toplevel && n->client->toplevel->scene_tree) {
      wlr_scene_node_set_position(&n->client->toplevel->scene_tree->node,
        n->client->floating_rectangle.x, n->client->floating_rectangle.y);
      wlr_xdg_toplevel_set_size(n->client->toplevel->xdg_toplevel,
        n->client->floating_rectangle.width, n->client->floating_rectangle.height);
    } else if (n->client->xwayland_view && n->client->xwayland_view->scene_tree) {
      wlr_scene_node_set_position(&n->client->xwayland_view->scene_tree->node,
        n->client->floating_rectangle.x, n->client->floating_rectangle.y);
      wlr_xwayland_surface_configure(n->client->xwayland_view->xwayland_surface,
        n->client->floating_rectangle.x, n->client->floating_rectangle.y,
        n->client->floating_rectangle.width, n->client->floating_rectangle.height);
    }

    transaction_commit_dirty();
    send_success(client_fd, "resized\n");
  } else if (streq("-a", *args) || streq("--activate", *args)) {
    output_t *m = server.focused_output;
    if (!m || !m->desk) {
      send_failure(client_fd, "node -a: no focused desktop\n");
      return;
    }
    node_t *n = m->desk->focus;
    if (!n) {
      send_failure(client_fd, "node -a: no focused node\n");
      return;
    }
    activate_node(m, m->desk, n);
    send_success(client_fd, "activated\n");
  } else if (streq("-k", *args) || streq("--kill", *args)) {
    output_t *m = server.focused_output;
    if (!m || !m->desk) {
      send_failure(client_fd, "node -k: no focused desktop\n");
      return;
    }
    node_t *n = m->desk->focus;
    if (!n) {
      send_failure(client_fd, "node -k: no focused node\n");
      return;
    }
    kill_node(m->desk, n);
    transaction_commit_dirty();
    send_success(client_fd, "killed\n");
  } else if (streq("-m", *args) || streq("--to-monitor", *args)) {
    if (num < 2) {
      send_failure(client_fd, "node -m: missing monitor name\n");
      return;
    }
    args++;
    num--;

    bool set_focus = false;
    char *target_name = NULL;
    while (num > 0) {
      if (streq("--follow", *args)) {
        set_focus = true;
      } else if (target_name == NULL) {
        target_name = *args;
      } else {
        send_failure(client_fd, "node -m: unexpected argument\n");
        return;
      }
      args++;
      num--;
    }

    if (target_name == NULL) {
      send_failure(client_fd, "node -m: missing monitor name\n");
      return;
    }

    output_t *target = find_output_by_name(target_name);
    if (!target) {
      send_failure(client_fd, "node -m: monitor not found\n");
      return;
    }

    output_t *m = server.focused_output;
    if (!m || !m->desk) {
      send_failure(client_fd, "node -m: no focused desktop\n");
      return;
    }

    if (m == target) {
      send_failure(client_fd, "node -m: already on target monitor\n");
      return;
    }

    node_t *n = m->desk->focus;
    if (!n || !n->client) {
      send_failure(client_fd, "node -m: no client\n");
      return;
    }

    desktop_t *src_desk = m->desk;
    desktop_t *target_desk = target->desk ? target->desk : target->desk_head;

		if (!target_desk){
			send_failure(client_fd, "node -m: target monitor has no desktops");
			return;
		}

    n->destroying = false;
    n->ntxnrefs = 0;
    n->client->shown = false;
    struct wlr_scene_tree *scene_tree = client_get_scene_tree(n->client);
    if (scene_tree)
      wlr_scene_node_set_enabled(&scene_tree->node, false);

    remove_node(src_desk, n);

    if (src_desk->root) {
      node_t *new_focus = first_extrema(src_desk->root);
      if (new_focus) {
        src_desk->focus = new_focus;
        focus_node(m, src_desk, new_focus);
      } else {
        src_desk->focus = NULL;
      }
    } else {
      src_desk->focus = NULL;
    }

    n->destroying = false;
    n->ntxnrefs = 0;

    insert_node(target_desk, n, find_public(target_desk));
    target_desk->focus = n;

    for (node_t *n_iter = first_extrema(target_desk->root); n_iter != NULL; n_iter = next_leaf(n_iter, target_desk->root)) {
      if (n_iter->client) {
        n_iter->client->shown = true;
        bool already_configured = true;
        if (n_iter->client->toplevel)
          already_configured = n_iter->client->toplevel->configured;
        if (already_configured) {
          struct wlr_scene_tree *scene_tree = client_get_scene_tree(n_iter->client);
          if (scene_tree)
            wlr_scene_node_set_enabled(&scene_tree->node, true);
        }
      }
    }

    arrange(target, target_desk, true);
    arrange(m, src_desk, src_desk->root != NULL);
    if (set_focus)
      focus_node(target, target_desk, n);

    send_success(client_fd, "node sent to monitor\n");
  } else if (streq("-n", *args) || streq("--to-node", *args)) {
    if (num < 2) {
      send_failure(client_fd, "node -n: missing target node\n");
      return;
    }
    args++;
    num--;

    output_t *m = server.focused_output;
    if (!m || !m->desk) {
      send_failure(client_fd, "node -n: no focused desktop\n");
      return;
    }

    node_t *n1 = m->desk->focus;
    if (!n1 || !n1->client) {
      send_failure(client_fd, "node -n: no focused client\n");
      return;
    }

    node_t *n2 = NULL;
    int target_id = atoi(*args);
    if (target_id > 0) {
      for (node_t *n = first_extrema(m->desk->root); n != NULL; n = next_leaf(n, m->desk->root)) {
        if (n->id == (uint32_t)target_id) {
          n2 = n;
          break;
        }
      }
    }

    if (!n2) {
      send_failure(client_fd, "node -n: target node not found\n");
      return;
    }

    if (n1 == n2) {
      send_failure(client_fd, "node -n: cannot transfer to self\n");
      return;
    }

    desktop_t *src_desk = m->desk;
    desktop_t *target_desk = src_desk;

    n1->destroying = false;
    n1->ntxnrefs = 0;
    n1->client->shown = false;
    struct wlr_scene_tree *scene_tree = client_get_scene_tree(n1->client);
    if (scene_tree)
      wlr_scene_node_set_enabled(&scene_tree->node, false);

    remove_node(src_desk, n1);

    if (src_desk->root) {
      node_t *new_focus = first_extrema(src_desk->root);
      if (new_focus) {
        src_desk->focus = new_focus;
        focus_node(m, src_desk, new_focus);
      } else {
        src_desk->focus = NULL;
      }
    } else {
      src_desk->focus = NULL;
    }

    n1->destroying = false;
    n1->ntxnrefs = 0;

    if (n2->first_child) {
      n1->parent = n2;
      n2->second_child = n1;
    } else {
      n1->parent = n2;
      n2->first_child = n1;
    }

    target_desk->focus = n1;
    if (target_desk == m->desk)
      focus_node(m, target_desk, n1);

    for (node_t *n_iter = first_extrema(target_desk->root); n_iter != NULL; n_iter = next_leaf(n_iter, target_desk->root)) {
      if (n_iter->client) {
        n_iter->client->shown = true;
        bool already_configured = true;
        if (n_iter->client->toplevel)
          already_configured = n_iter->client->toplevel->configured;
        if (already_configured) {
          struct wlr_scene_tree *scene_tree = client_get_scene_tree(n_iter->client);
          if (scene_tree)
            wlr_scene_node_set_enabled(&scene_tree->node, true);
        }
      }
    }

    arrange(m, target_desk, true);
    if (src_desk != target_desk)
      arrange(m, src_desk, src_desk->root != NULL);

    send_success(client_fd, "node sent to node\n");
  } else if (streq("-l", *args) || streq("--layer", *args)) {
    if (num < 2) {
      send_failure(client_fd, "node -l: missing layer argument\n");
      return;
    }
    args++;
    num--;

    output_t *m = server.focused_output;
    if (!m || !m->desk) {
      send_failure(client_fd, "node -l: no focused desktop\n");
      return;
    }
    node_t *n = m->desk->focus;
    if (!n || !n->client) {
      send_failure(client_fd, "node -l: no client\n");
      return;
    }

    stack_layer_t layer;
    if (streq("below", *args)) {
      layer = LAYER_BELOW;
    } else if (streq("normal", *args)) {
      layer = LAYER_NORMAL;
    } else if (streq("above", *args)) {
      layer = LAYER_ABOVE;
    } else {
      send_failure(client_fd, "node -l: unknown layer (use below, normal, or above)\n");
      return;
    }

    n->client->layer = layer;
    transaction_commit_dirty();
    send_success(client_fd, "layer changed\n");
  } else if (streq("-y", *args) || streq("--type", *args)) {
    if (num < 2) {
      send_failure(client_fd, "node -y: missing type argument\n");
      return;
    }
    args++;
    num--;

    output_t *m = server.focused_output;
    if (!m || !m->desk) {
      send_failure(client_fd, "node -y: no focused desktop\n");
      return;
    }
    node_t *n = m->desk->focus;
    if (!n) {
      send_failure(client_fd, "node -y: no focused node\n");
      return;
    }

    if (streq("next_tab", *args) || streq("next.tab", *args)) {
      node_t *t = tabbed_ancestor(n);
      if (t == NULL) {
        send_failure(client_fd, "node -y: focused node not in tab group\n");
        return;
      }
      node_t *next = tab_next_leaf(t, n);
      if (next != NULL) {
        focus_node(m, m->desk, next);
        arrange(m, m->desk, true);
      }
      send_success(client_fd, "next tab\n");
      return;
    }
    if (streq("prev_tab", *args) || streq("prev.tab", *args)) {
      node_t *t = tabbed_ancestor(n);
      if (t == NULL) {
        send_failure(client_fd, "node -y: focused node not in tab group\n");
        return;
      }
      node_t *prev = tab_prev_leaf(t, n);
      if (prev != NULL) {
        focus_node(m, m->desk, prev);
        arrange(m, m->desk, true);
      }
      send_success(client_fd, "prev tab\n");
      return;
    }
    if (streq("tabbed", *args) || streq("horizontal", *args) ||
        streq("vertical", *args)) {
      node_t *target = n->parent;
      if (target == NULL) {
        send_failure(client_fd, "node -y: focused node has no parent\n");
        return;
      }
      split_type_t prev_st = target->split_type;
      split_type_t st = TYPE_HORIZONTAL;
      if (streq("tabbed", *args))
        st = TYPE_TABBED;
      else if (streq("vertical", *args))
        st = TYPE_VERTICAL;
      target->split_type = st;
      target->pending.split_type = st;
      target->current.split_type = st;

      if (prev_st == TYPE_TABBED && st != TYPE_TABBED) {
        tabs_destroy(target);
        for (node_t *leaf = first_extrema(target);
             leaf != NULL && leaf != target;
             leaf = next_leaf(leaf, target)) {
          if (leaf->client == NULL)
            continue;
          if (leaf->client->state == STATE_FLOATING)
            continue;
          leaf->client->shown = true;
          struct wlr_scene_tree *stree = client_get_scene_tree(leaf->client);
          if (stree)
            wlr_scene_node_set_enabled(&stree->node, true);
        }
      }

      arrange(m, m->desk, true);

      // reapply decoration mode for all leaves
      for (node_t *leaf = first_extrema(target);
           leaf != NULL && leaf != target;
           leaf = next_leaf(leaf, target)) {
        if (leaf->client && leaf->client->toplevel)
          toplevel_apply_decoration_mode(leaf->client->toplevel);
      }

      if (m->desk->focus != NULL)
        focus_node(m, m->desk, m->desk->focus);
      send_success(client_fd, "type changed\n");
      return;
    }
    n->split_type = (n->split_type + 1) % 2;

    transaction_commit_dirty();
    send_success(client_fd, "type changed\n");
  } else if (streq("-r", *args) || streq("--ratio", *args)) {
    if (num < 2) {
      send_failure(client_fd, "node -r: missing ratio argument\n");
      return;
    }
    args++;
    num--;

    output_t *m = server.focused_output;
    if (!m || !m->desk) {
      send_failure(client_fd, "node -r: no focused desktop\n");
      return;
    }
    node_t *n = m->desk->focus;
    if (!n) {
      send_failure(client_fd, "node -r: no focused node\n");
      return;
    }

    double rat;
    if ((*args)[0] == '+' || (*args)[0] == '-') {
      float delta;
      if (sscanf(*args, "%f", &delta) == 1) {
        if (delta > -1 && delta < 1) {
          rat = n->split_ratio + delta;
        } else {
          int max = (n->split_type == TYPE_HORIZONTAL) ? n->rectangle.height : n->rectangle.width;
          rat = ((max * n->split_ratio) + delta) / max;
        }
      } else {
        send_failure(client_fd, "node -r: invalid argument\n");
        return;
      }
    } else {
      if (sscanf(*args, "%lf", &rat) != 1) {
        send_failure(client_fd, "node -r: invalid argument\n");
        return;
      }
    }

    if (rat > 0 && rat < 1) {
      n->split_ratio = rat;
      transaction_commit_dirty();
      send_success(client_fd, "ratio changed\n");
    } else {
      send_failure(client_fd, "node -r: ratio out of range\n");
      return;
    }
  } else if (streq("-C", *args) || streq("--circulate", *args)) {
    if (num < 2) {
      send_failure(client_fd, "node -C: missing direction\n");
      return;
    }
    args++;
    num--;

    output_t *m = server.focused_output;
    if (!m || !m->desk) {
      send_failure(client_fd, "node -C: no focused desktop\n");
      return;
    }
    node_t *n = m->desk->focus;
    if (!n) {
      send_failure(client_fd, "node -C: no focused node\n");
      return;
    }

    if (streq("forward", *args) || streq("f", *args)) {
      node_t *next = next_leaf(n, m->desk->root);
      if (next) {
        m->desk->focus = next;
        focus_node(m, m->desk, next);
      }
    } else if (streq("backward", *args) || streq("b", *args)) {
      node_t *prev = prev_leaf(n, m->desk->root);
      if (prev) {
        m->desk->focus = prev;
        focus_node(m, m->desk, prev);
      }
    } else {
      send_failure(client_fd, "node -C: unknown direction\n");
      return;
    }

    send_success(client_fd, "circulated\n");
  } else if (streq("-i", *args) || streq("--insert-receptacle", *args)) {
    output_t *m = server.focused_output;
    if (!m || !m->desk) {
      send_failure(client_fd, "node -i: no focused desktop\n");
      return;
    }
    node_t *n = m->desk->focus;

    node_t *receptacle = make_node(0);
    receptacle->vacant = true;
    receptacle->split_type = TYPE_VERTICAL;
    receptacle->split_ratio = 0.5;

    if (n && !is_leaf(n)) {
      if (n->first_child) {
        receptacle->parent = n;
        n->second_child->parent = receptacle;
        receptacle->first_child = n->second_child;
        n->second_child = receptacle;
      } else {
        n->first_child = receptacle;
        receptacle->parent = n;
      }
    } else if (n) {
      node_t *parent = n->parent;
      if (parent) {
        if (parent->first_child == n) {
          parent->first_child = receptacle;
        } else {
          parent->second_child = receptacle;
        }
        receptacle->parent = parent;

        if (n->split_type == TYPE_VERTICAL) {
          receptacle->first_child = n;
          n->parent = receptacle;
        } else {
          receptacle->second_child = n;
          n->parent = receptacle;
        }
      } else {
        m->desk->root = receptacle;
        receptacle->first_child = n;
        n->parent = receptacle;
      }
      receptacle->split_type = TYPE_VERTICAL;
    } else {
      m->desk->root = receptacle;
    }

    transaction_commit_dirty();
    send_success(client_fd, "receptacle inserted\n");
  } else if (streq("-p", *args) || streq("--presel-dir", *args)) {
    if (num < 2) {
      send_failure(client_fd, "node -p: missing direction\n");
      return;
    }
    args++;
    num--;

    output_t *m = server.focused_output;
    if (!m || !m->desk) {
      send_failure(client_fd, "node -p: no focused desktop\n");
      return;
    }
    node_t *n = m->desk->focus;
    if (!n || n->vacant) {
      send_failure(client_fd, "node -p: no valid node\n");
      return;
    }

    if (streq("cancel", *args)) {
      if (n->presel) {
        free(n->presel);
        n->presel = NULL;
      }
      send_success(client_fd, "presel cancelled\n");
      return;
    }

    direction_t dir;
    if (streq("west", *args) || streq("w", *args)) {
      dir = DIR_WEST;
    } else if (streq("east", *args) || streq("e", *args)) {
      dir = DIR_EAST;
    } else if (streq("north", *args) || streq("n", *args)) {
      dir = DIR_NORTH;
    } else if (streq("south", *args) || streq("s", *args)) {
      dir = DIR_SOUTH;
    } else {
      send_failure(client_fd, "node -p: unknown direction\n");
      return;
    }

    presel_dir(n, dir);
    transaction_commit_dirty();
    send_success(client_fd, "presel set\n");
  } else if (streq("-o", *args) || streq("--presel-ratio", *args)) {
    if (num < 2) {
      send_failure(client_fd, "node -o: missing ratio\n");
      return;
    }
    args++;
    num--;

    output_t *m = server.focused_output;
    if (!m || !m->desk) {
      send_failure(client_fd, "node -o: no focused desktop\n");
      return;
    }
    node_t *n = m->desk->focus;
    if (!n || n->vacant) {
      send_failure(client_fd, "node -o: no valid node\n");
      return;
    }

    double rat;
    if (sscanf(*args, "%lf", &rat) != 1 || rat <= 0 || rat >= 1) {
      send_failure(client_fd, "node -o: invalid ratio\n");
      return;
    }

    if (!n->presel) {
      n->presel = make_presel();
    }
    n->presel->split_ratio = rat;

    transaction_commit_dirty();
    send_success(client_fd, "presel ratio set\n");
  } else if (streq("-s", *args) || streq("--swap", *args)) {
    if (num < 2) {
      send_failure(client_fd, "node -s: missing target node\n");
      return;
    }
    args++;
    num--;

    output_t *m = server.focused_output;
    if (!m || !m->desk) {
      send_failure(client_fd, "node -s: no focused desktop\n");
      return;
    }

    node_t *n1 = m->desk->focus;
    if (!n1) {
      send_failure(client_fd, "node -s: no focused node\n");
      return;
    }

    node_t *n2 = NULL;
    int target_id = atoi(*args);
    if (target_id > 0) {
      for (node_t *n = first_extrema(m->desk->root); n != NULL; n = next_leaf(n, m->desk->root)) {
        if (n->id == (uint32_t)target_id) {
          n2 = n;
          break;
        }
      }
    }

    if (!n2) {
      send_failure(client_fd, "node -s: target node not found\n");
      return;
    }

    if (n1 == n2) {
      send_failure(client_fd, "node -s: cannot swap with self\n");
      return;
    }

    swap_nodes(m, m->desk, n1, m, m->desk, n2);
    m->desk->focus = n2;
    transaction_commit_dirty();
    send_success(client_fd, "swapped\n");
  } else {
    send_failure(client_fd, "node: unknown command\n");
  }
}

static void ipc_cmd_desktop(char **args, int num, int client_fd) {
  if (num < 1) {
    send_failure(client_fd, "desktop: Missing arguments\n");
    return;
  }

  output_t *mon = server.focused_output;
  if (!mon || !mon->desk) {
    send_failure(client_fd, "no desktop\n");
    return;
  }

  if (streq("next", *args)) {
    focus_next_desktop();
    send_success(client_fd, "focused\n");
    return;
  } else if (streq("last", *args)) {
    focus_last_desktop();
    send_success(client_fd, "focused\n");
    return;
  } else if (streq("prev", *args) || streq("previous", *args)) {
    focus_prev_desktop();
    send_success(client_fd, "focused\n");
    return;
  }

  desktop_t *desk = mon->desk;
  if ((*args)[0] != '-') {
    desk = find_desktop_by_name_in_monitor(mon, *args);
    if (!desk) {
      char *end;
      long idx = strtol(*args, &end, 10);
      if (*end == '\0' && idx >= 1 && idx <= 10) {
        workspace_switch_to_desktop_by_index(idx - 1);
        send_success(client_fd, "focused\n");
        return;
      }
      send_failure(client_fd, "desktop: unknown desktop\n");
      return;
    }
    args++;
    num--;
  }

  if (num < 1) {
    workspace_switch_to_desktop(desk->name);
    send_success(client_fd, "focused\n");
    return;
  }

  if (streq("-f", *args) || streq("--focus", *args)) {
    args++;
    num--;
    if (num >= 1 && (streq("next", *args) || streq("next.local", *args))) {
      focus_next_desktop();
      send_success(client_fd, "focused\n");
    } else if (num >= 1 && streq("last", *args)) {
      focus_last_desktop();
      send_success(client_fd, "focused\n");
    } else if (num >= 1 && (streq("prev", *args) || streq("prev.local", *args) || streq("previous", *args))) {
      focus_prev_desktop();
      send_success(client_fd, "focused\n");
    } else {
      workspace_switch_to_desktop(desk->name);
      send_success(client_fd, "focused\n");
    }
  } else if (streq("-l", *args) || streq("--layout", *args)) {
    if (num < 2) {
      send_failure(client_fd, "desktop -l: missing layout argument\n");
      return;
    }
    args++;
    if (streq("tiled", *args)) {
      desk->layout = LAYOUT_TILED;
    } else if (streq("monocle", *args)) {
      desk->layout = LAYOUT_MONOCLE;
    } else if (streq("scroller", *args)) {
      desk->layout = LAYOUT_SCROLLER;
    } else {
      send_failure(client_fd, "desktop -l: unknown layout\n");
      return;
    }
    if (desk->root != NULL) {
      arrange(mon, desk, true);
      if (desk->focus != NULL)
        focus_node(mon, desk, desk->focus);
    } else {
      transaction_commit_dirty();
    }
    send_success(client_fd, "layout changed\n");
  } else if (streq("-n", *args) || streq("--rename", *args)) {
    if (num < 2) {
      send_failure(client_fd, "desktop -n: missing name argument\n");
      return;
    }
    args++;
    strncpy(desk->name, *args, SMALEN - 1);
    desk->name[SMALEN - 1] = '\0';
    transaction_commit_dirty();
    send_success(client_fd, "renamed\n");
  } else if (streq("-s", *args) || streq("--swap", *args)) {
    if (num < 2) {
      send_failure(client_fd, "desktop -s: missing target desktop\n");
      return;
    }
    args++;
    num--;

    desktop_t *target = find_desktop_by_name_in_monitor(mon, *args);
    if (!target) {
      send_failure(client_fd, "desktop -s: target desktop not found\n");
      return;
    }

    if (target == desk) {
      send_failure(client_fd, "desktop -s: cannot swap with self\n");
      return;
    }

    desktop_t *d0 = desk;
    desktop_t *d1 = target;
    output_t *m0 = d0->output;
    output_t *m1 = d1->output;

    if (m0 == m1) {
      desktop_t *prev0 = d0->prev;
      desktop_t *next0 = d0->next;
      desktop_t *prev1 = d1->prev;
      desktop_t *next1 = d1->next;

      if (next0 == d1) {
        d0->prev = d1;
        d0->next = next1;
        d1->prev = prev0;
        d1->next = d0;
        if (prev0) prev0->next = d1;
        if (next1) next1->next = d0;
      } else {
        d0->prev = prev1;
        d0->next = next1;
        d1->prev = prev0;
        d1->next = next0;
        if (prev0) prev0->next = d1;
        if (next0) next0->next = d1;
        if (prev1) prev1->next = d0;
        if (next1) next1->next = d0;
      }

      if (m0->desk == d0) m0->desk = d1;
      else if (m0->desk == d1) m0->desk = d0;
    } else {
      desktop_t *prev0 = d0->prev;
      desktop_t *next0 = d0->next;
      desktop_t *prev1 = d1->prev;
      desktop_t *next1 = d1->next;

      d0->prev = prev1;
      d0->next = next1;
      d1->prev = prev0;
      d1->next = next0;

      if (prev0) prev0->next = d1; else m0->desk = d1;
      if (next0) next0->next = d1;
      if (prev1) prev1->next = d0; else m1->desk = d0;
      if (next1) next1->next = d0;

      d0->output = m1;
      d1->output = m0;
    }

    if (mon->desk == d0) mon->desk = d1;
    else if (mon->desk == d1) mon->desk = d0;

    transaction_commit_dirty();
    send_success(client_fd, "swapped\n");
  } else if (streq("-r", *args) || streq("--remove", *args)) {
    if (!desk->prev && !desk->next) {
      send_failure(client_fd, "desktop -r: cannot remove the only desktop\n");
      return;
    }

    desktop_t *prev = desk->prev;
    desktop_t *next = desk->next;

    if (prev)
      prev->next = next;
    else if (mon->desk)
      mon->desk = next;

    if (next)
      next->prev = prev;

    if (mon->desk == desk) {
      mon->desk = next ? next : prev;
      if (mon->desk)
        focus_node(mon, mon->desk, mon->desk->focus);
    }
    if (mon->last_desk == desk)
      mon->last_desk = next ? next : prev;

    free(desk);
    transaction_commit_dirty();
    send_success(client_fd, "removed\n");
  } else if (streq("-b", *args) || streq("--bubble", *args)) {
    if (num < 2) {
      send_failure(client_fd, "desktop -b: missing direction\n");
      return;
    }
    args++;
    num--;

    if (streq("up", *args) || streq("prev", *args)) {
      if (desk->prev) {
        desktop_t *prev = desk->prev;
        desktop_t *prev_prev = prev->prev;

        desk->prev = prev_prev;
        desk->next = prev;
        prev->prev = desk;
        prev->next = desk;

        if (prev_prev)
          prev_prev->next = desk;
        else
          mon->desk = desk;
      }
    } else if (streq("down", *args) || streq("next", *args)) {
      if (desk->next) {
        desktop_t *next = desk->next;
        desktop_t *next_next = next->next;

        desk->prev = next;
        desk->next = next_next;
        next->prev = desk;
        next->next = desk;

        if (next_next)
          next_next->prev = desk;
      }
    } else {
      send_failure(client_fd, "desktop -b: unknown direction\n");
      return;
    }

    transaction_commit_dirty();
    send_success(client_fd, "bubbled\n");
  } else if (streq("-m", *args) || streq("--to-monitor", *args)) {
    if (num < 2) {
      send_failure(client_fd, "desktop -m: missing monitor name\n");
      return;
    }
    args++;
    num--;

    desktop_t *desk = mon->desk;
    output_t *target = find_output_by_name(*args);
    if (!target) {
      send_failure(client_fd, "desktop -m: monitor not found\n");
      return;
    }

    if (desk->output == target) {
      send_failure(client_fd, "desktop -m: already on target monitor\n");
      return;
    }

    if (!target->desk && !target->desk_head) {
      send_failure(client_fd, "desktop -m: target monitor has no desktop\n");
      return;
    }

    output_t *src_mon = desk->output;

    if (desk->prev) {
      desk->prev->next = desk->next;
    } else {
      src_mon->desk = desk->next;
      if (src_mon->desk_head == desk) src_mon->desk_head = desk->next;
    }

    if (desk->next) {
      desk->next->prev = desk->prev;
    } else {
      if (src_mon->desk_tail == desk) src_mon->desk_tail = desk->prev;
    }

    desk->prev = target->desk_tail;
    desk->next = NULL;
    desk->output = target;

    if (target->desk_tail) {
      target->desk_tail->next = desk;
      target->desk_tail = desk;
    } else {
      target->desk = desk;
      target->desk_head = desk;
      target->desk_tail = desk;
    }

    if (src_mon->desk == desk) {
      src_mon->desk = src_mon->desk_head;
      if (src_mon->desk)
        focus_node(src_mon, src_mon->desk, src_mon->desk->focus);
    }

    transaction_commit_dirty();
    send_success(client_fd, "desktop moved to monitor\n");
  } else {
    send_failure(client_fd, "desktop: unknown command\n");
  }
}

static void ipc_cmd_query(char **args, int num, int client_fd) {
  char buf[DOORS_BUFSIZ];
  size_t offset = 0;

  if (num < 1) {
    send_failure(client_fd, "query: Missing arguments\n");
    return;
  }

  // parse optional selectors
  output_t *filter_mon = NULL;
  desktop_t *filter_desk = NULL;
  node_t *filter_node = NULL;
  bool use_names = false;

  while (num > 0 && (streq("-m", *args) || streq("--monitor", *args) ||
	  streq("-d", *args) || streq("--desktop", *args) || streq("-n", *args) ||
	  streq("--node", *args) || streq("--names", *args))) {
    if (streq("-m", *args) || streq("--monitor", *args)) {
      if (num < 2) {
        send_failure(client_fd, "query -m: missing monitor\n");
        return;
      }
      args++;
      num--;
      filter_mon = find_output_by_name(*args);
      if (!filter_mon) {
        send_failure(client_fd, "query -m: monitor not found\n");
        return;
      }
    } else if (streq("-d", *args) || streq("--desktop", *args)) {
      if (num < 2) {
        send_failure(client_fd, "query -d: missing desktop\n");
        return;
      }
      args++;
      num--;
      filter_desk = find_desktop_by_name(*args);
      if (!filter_desk) {
        send_failure(client_fd, "query -d: desktop not found\n");
        return;
      }
    } else if (streq("-n", *args) || streq("--node", *args)) {
      if (num < 2) {
        send_failure(client_fd, "query -n: missing node\n");
        return;
      }
      args++;
      num--;
      int node_id = atoi(*args);
      if (node_id <= 0) {
        send_failure(client_fd, "query -n: invalid node id\n");
        return;
      }
      struct toplevel_t *toplevel;
      wl_list_for_each(toplevel, &server.toplevels, link) {
        if (toplevel->node && toplevel->node->id == (uint32_t)node_id) {
          filter_node = toplevel->node;
          break;
        }
      }
      if (!filter_node) {
        send_failure(client_fd, "query -n: node not found\n");
        return;
      }
    } else if (streq("--names", *args)) {
      use_names = true;
    }
    args++;
    num--;
  }

  if (num < 1) {
    send_failure(client_fd, "query: missing query type\n");
    return;
  }

  if (streq("-T", *args) || streq("--tree", *args)) {
    offset += snprintf(buf + offset, sizeof(buf) - offset, "{\n");

    output_t *m_start = filter_mon ? filter_mon : mon_head;
    output_t *m_end = filter_mon ? filter_mon->next : NULL;

    for (output_t *m = m_start; m != m_end; ) {
      offset += snprintf(buf + offset, sizeof(buf) - offset,
        "  \"monitor\": {\"name\": \"%s\", \"id\": %u},\n",
        m->name, m->id);

      desktop_t *d_start = filter_desk ? filter_desk : m->desk;
      desktop_t *d_end = filter_desk ? filter_desk->next : NULL;

      for (desktop_t *d = d_start; d != d_end; ) {
        offset += snprintf(buf + offset, sizeof(buf) - offset,
          "  \"desktop\": {\"name\": \"%s\", \"id\": %u, \"layout\": %d},\n",
          d->name, d->id, d->layout);
        if (filter_desk) break;
        d = d->next;
      }

      if (filter_mon) break;
      m = m->next;
    }

    toplevel_t *toplevel;
    wl_list_for_each(toplevel, &server.toplevels, link) {
      bool include = true;
      if (filter_node && toplevel->node != filter_node)
        include = false;
      if (filter_desk && toplevel->node && toplevel->node->output &&
          toplevel->node->output->desk != filter_desk)
        include = false;
      if (filter_mon && toplevel->node && toplevel->node->output != filter_mon)
        include = false;

      if (include)
        offset += snprintf(buf + offset, sizeof(buf) - offset,
          "  \"toplevel\": {\"app_id\": \"%s\", \"title\": \"%s\", \"identifier\": \"%s\"}\n",
          toplevel->node && toplevel->node->client ? toplevel->node->client->app_id : "?",
          toplevel->node && toplevel->node->client ? toplevel->node->client->title : "?",
          toplevel->foreign_identifier ? toplevel->foreign_identifier : "?");
    }

    offset += snprintf(buf + offset, sizeof(buf) - offset, "}\n");
    send_success(client_fd, buf);
  } else if (streq("-M", *args) || streq("--monitors", *args)) {
    for (output_t *m = filter_mon ? filter_mon : mon_head;
         m != NULL; m = filter_mon ? NULL : m->next) {
      if (use_names)
        offset += snprintf(buf + offset, sizeof(buf) - offset, "%s\n", m->name);
      else
        offset += snprintf(buf + offset, sizeof(buf) - offset, "%u %s\n", m->id, m->name);
      if (filter_mon) break;
    }
    send_success(client_fd, buf);
  } else if (streq("-D", *args) || streq("--desktops", *args)) {
    output_t *m_start = filter_mon ? filter_mon : mon_head;
    for (output_t *m = m_start; m != NULL; m = filter_mon ? NULL : m->next) {
      desktop_t *d_start = filter_desk ? filter_desk : m->desk;
      for (desktop_t *d = d_start; d != NULL; d = filter_desk ? NULL : d->next) {
        if (use_names)
          offset += snprintf(buf + offset, sizeof(buf) - offset, "%s\n", d->name);
        else
          offset += snprintf(buf + offset, sizeof(buf) - offset, "%u %s\n", d->id, d->name);
        if (filter_desk) break;
      }
      if (filter_mon) break;
    }
    send_success(client_fd, buf);
  } else if (streq("-N", *args) || streq("--nodes", *args)) {
    toplevel_t *toplevel;
    wl_list_for_each(toplevel, &server.toplevels, link) {
      bool include = true;
      if (filter_node && toplevel->node != filter_node)
        include = false;
      if (filter_desk && toplevel->node && toplevel->node->output &&
          toplevel->node->output->desk != filter_desk)
        include = false;
      if (filter_mon && toplevel->node && toplevel->node->output != filter_mon)
        include = false;

      if (include) {
        if (use_names) {
          const char *name = "?";
          if (toplevel->node && toplevel->node->client && toplevel->node->client->title[0])
            name = toplevel->node->client->title;
          else if (toplevel->node && toplevel->node->client && toplevel->node->client->app_id[0])
            name = toplevel->node->client->app_id;
          offset += snprintf(buf + offset, sizeof(buf) - offset, "%s\n", name);
        } else {
          offset += snprintf(buf + offset, sizeof(buf) - offset, "%u %s\n",
            toplevel->node ? toplevel->node->id : 0,
            toplevel->foreign_identifier ? toplevel->foreign_identifier : "?");
        }
      }
    }
    send_success(client_fd, buf);
  } else if (streq("-f", *args) || streq("--focused", *args)) {
    output_t *m = server.focused_output;
    if (!m || !m->desk) {
      send_failure(client_fd, "no focused desktop\n");
      return;
    }
    node_t *n = m->desk->focus;
    if (!n) {
      send_failure(client_fd, "no focused node\n");
      return;
    }
    char *foreign_id = "?";
    toplevel_t *toplevel;
    wl_list_for_each(toplevel, &server.toplevels, link)
      if (toplevel->node == n) {
        foreign_id = toplevel->foreign_identifier ? toplevel->foreign_identifier : "?";
        break;
      }

    if (use_names) {
      offset += snprintf(buf + offset, sizeof(buf) - offset,
        "{\"monitor\": \"%s\", \"desktop\": \"%s\", \"node\": \"%s\", \"title\": \"%s\", \"type\": %d, "
        "\"rect\": {\"x\": %d, \"y\": %d, \"width\": %d, \"height\": %d}, "
        "\"client\": \"%s\", \"identifier\": \"%s\"}\n",
        m->name,
        m->desk->name,
        n->client && n->client->title[0] ? n->client->title :
        (n->client && n->client->app_id[0] ? n->client->app_id : "?"),
        n->client && n->client->title[0] ? n->client->title : "",
        n->split_type,
        n->rectangle.x,
        n->rectangle.y,
        n->rectangle.width,
        n->rectangle.height,
        n->client && n->client->app_id[0] ? n->client->app_id : "?",
        foreign_id);
    } else {
      offset += snprintf(buf + offset, sizeof(buf) - offset,
        "{\"monitor\": \"%s\", \"desktop\": \"%s\", \"id\": %u, \"title\": \"%s\", \"type\": %d, "
        "\"rect\": {\"x\": %d, \"y\": %d, \"width\": %d, \"height\": %d}, "
        "\"client\": \"%s\", \"identifier\": \"%s\"}\n",
        m->name,
        m->desk->name,
        n->id,
        n->client && n->client->title[0] ? n->client->title : "",
        n->split_type,
        n->rectangle.x,
        n->rectangle.y,
        n->rectangle.width,
        n->rectangle.height,
        n->client && n->client->app_id[0] ? n->client->app_id : "?",
        foreign_id);
    }
    send_success(client_fd, buf);
  } else {
    send_failure(client_fd, "query: unknown command\n");
  }
}

static void ipc_cmd_wm(char **args, int num, int client_fd) {
  char buf[DOORS_BUFSIZ];
  size_t offset = 0;

  if (num < 1) {
    send_failure(client_fd, "wm: missing command\n");
    return;
  }

  if (streq("-d", *args) || streq("--dump-state", *args)) {
    // dump current state as JSON
    offset += snprintf(buf + offset, sizeof(buf) - offset, "{\n");
    offset += snprintf(buf + offset, sizeof(buf) - offset, "  \"monitors\": [\n");

    bool first_mon = true;
    for (output_t *m = mon_head; m != NULL; m = m->next) {
      if (!first_mon) offset += snprintf(buf + offset, sizeof(buf) - offset, ",\n");
      first_mon = false;
      offset += snprintf(buf + offset, sizeof(buf) - offset,
        "    {\"name\": \"%s\", \"id\": %u, \"rect\": {\"x\": %d, \"y\": %d, \"width\": %d, \"height\": %d}}",
        m->name, m->id, m->rectangle.x, m->rectangle.y, m->rectangle.width, m->rectangle.height);
    }
    offset += snprintf(buf + offset, sizeof(buf) - offset, "\n  ],\n");

    offset += snprintf(buf + offset, sizeof(buf) - offset, "  \"settings\": {\n");
    offset += snprintf(buf + offset, sizeof(buf) - offset,
      "    \"border_width\": %d,\n", border_width);
    offset += snprintf(buf + offset, sizeof(buf) - offset,
      "    \"window_gap\": %d,\n", window_gap);
    offset += snprintf(buf + offset, sizeof(buf) - offset,
      "    \"split_ratio\": %.2f,\n", split_ratio);
    offset += snprintf(buf + offset, sizeof(buf) - offset,
      "    \"single_monocle\": %s,\n", single_monocle ? "true" : "false");
    offset += snprintf(buf + offset, sizeof(buf) - offset,
      "    \"automatic_scheme\": %d,\n", automatic_scheme);
    offset += snprintf(buf + offset, sizeof(buf) - offset,
      "    \"record_history\": %s\n", record_history ? "true" : "false");
    offset += snprintf(buf + offset, sizeof(buf) - offset, "  }\n");
    offset += snprintf(buf + offset, sizeof(buf) - offset, "}\n");

    send_success(client_fd, buf);
  } else if (streq("-l", *args) || streq("--load-state", *args)) {
    send_success(client_fd, "load-state: not implemented\n");
  } else if (streq("-a", *args) || streq("--add-monitor", *args)) {
    if (num < 2) {
      send_failure(client_fd, "wm -a: missing monitor name\n");
      return;
    }
    args++;
    num--;

    if (find_output_by_name(*args)) {
      send_failure(client_fd, "wm -a: monitor already exists\n");
      return;
    }

    struct output_config *oc = output_config_create(*args);
    if (oc) {
      send_success(client_fd, "monitor config added\n");
    } else {
      send_failure(client_fd, "wm -a: failed to add monitor\n");
    }
  } else if (streq("-O", *args) || streq("--reorder-monitors", *args)) {
    if (num < 2) {
      send_failure(client_fd, "wm -O: missing monitor list\n");
      return;
    }
    args++;
    num--;

    send_success(client_fd, "unimplemented\n");
  } else if (streq("-o", *args) || streq("--adopt-orphans", *args)) {
    struct toplevel_t *toplevel, *tmp;
    int adopted = 0;
    wl_list_for_each_safe(toplevel, tmp, &server.toplevels, link) {
      if (!toplevel->node && toplevel->xdg_toplevel && toplevel->mapped) {
        toplevel_map(NULL, toplevel);
        adopted++;
      }
    }
    offset += snprintf(buf + offset, sizeof(buf) - offset, "adopted %d orphans\n", adopted);
    send_success(client_fd, buf);
  } else if (streq("-g", *args) || streq("--get-status", *args)) {
    int output_count = 0;
    for (output_t *m = mon_head; m != NULL; m = m->next)
      output_count++;

    offset += snprintf(buf + offset, sizeof(buf) - offset,
      "status: running\n"
      "monitors: %d\n", output_count);

    output_t *m = server.focused_output;
    if (m && m->desk) {
      offset += snprintf(buf + offset, sizeof(buf) - offset,
        "focused_monitor: %s\n"
        "focused_desktop: %s\n",
        m->name, m->desk->name);
      if (m->desk->focus) {
        offset += snprintf(buf + offset, sizeof(buf) - offset,
          "focused_node: %u\n", m->desk->focus->id);
      }
    }

    send_success(client_fd, buf);
  } else if (streq("-h", *args) || streq("--record-history", *args)) {
    if (num >= 2) {
      if (streq("true", args[1]) || streq("on", args[1]) || streq("1", args[1])) {
        record_history = true;
        send_success(client_fd, "record-history enabled\n");
      } else if (streq("false", args[1]) || streq("off", args[1]) || streq("0", args[1])) {
        record_history = false;
        send_success(client_fd, "record-history disabled\n");
      } else {
        send_failure(client_fd, "wm -h: invalid value (use true/false)\n");
      }
    } else {
      send_success(client_fd, record_history ? "true\n" : "false\n");
    }
  } else if (streq("-r", *args) || streq("--restart", *args)) {
    server_restart();
    send_success(client_fd, "restarting\n");
  } else {
    send_failure(client_fd, "wm: unknown command\n");
  }
}

static void ipc_cmd_config(char **args, int num, int client_fd) {
  if (num < 1) {
    send_failure(client_fd, "config: Missing arguments\n");
    return;
  }

  if (streq("border_width", *args)) {
    if (num >= 2) {
      int val = atoi(args[1]);
      border_width = val;
      for (output_t *m = mon_head; m != NULL; m = m->next) {
        m->border_width = border_width;
        for (desktop_t *d = m->desk; d != NULL; d = d->next)
          d->border_width = border_width;
      }
      transaction_commit_dirty();
      send_success(client_fd, "border_width set\n");
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "%d\n", border_width);
      send_success(client_fd, buf);
    }
  } else if (streq("window_gap", *args)) {
    if (num >= 2) {
      int val = atoi(args[1]);
      window_gap = val;
      for (output_t *m = mon_head; m != NULL; m = m->next) {
        m->window_gap = window_gap;
        for (desktop_t *d = m->desk; d != NULL; d = d->next)
          d->window_gap = window_gap;
      }
      transaction_commit_dirty();
      send_success(client_fd, "window_gap set\n");
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "%d\n", window_gap);
      send_success(client_fd, buf);
    }
  } else if (streq("single_monocle", *args)) {
    ipc_handle_bool(args, num, client_fd, &single_monocle, IPC_FLAG_COMMIT);
  } else if (streq("borderless_monocle", *args)) {
    ipc_handle_bool(args, num, client_fd, &borderless_monocle, IPC_FLAG_COMMIT);
  } else if (streq("borderless_singleton", *args)) {
    ipc_handle_bool(args, num, client_fd, &borderless_singleton, IPC_FLAG_COMMIT);
  } else if (streq("gapless_monocle", *args)) {
    ipc_handle_bool(args, num, client_fd, &gapless_monocle, IPC_FLAG_COMMIT);
  } else if (streq("decoration_mode", *args)) {
    if (num >= 2) {
      if (strcmp(args[1], "none") == 0)
        decoration_mode = DECORATION_NONE;
      else if (strcmp(args[1], "tabs") == 0)
        decoration_mode = DECORATION_TABS;
      else if (strcmp(args[1], "always") == 0)
        decoration_mode = DECORATION_ALWAYS;
      else if (strcmp(args[1], "csd") == 0)
        decoration_mode = DECORATION_CSD;
      else {
        send_failure(client_fd, "config decoration_mode: expected \"none\", \"tabs\", \"always\", or \"csd\"\n");
        return;
      }
      for (output_t *m = mon_head; m != NULL; m = m->next)
        for (desktop_t *d = m->desk; d != NULL; d = d->next)
          if (d->root != NULL)
            tabs_rebuild(d->root);
      transaction_commit_dirty();
      send_success(client_fd, "decoration_mode set\n");
    } else {
      const char *mode_str = "";
      switch (decoration_mode) {
      case DECORATION_NONE:
        mode_str = "none\n";
        break;
      case DECORATION_TABS:
        mode_str = "tabs\n";
        break;
      case DECORATION_ALWAYS:
        mode_str = "always\n";
        break;
      case DECORATION_CSD:
        mode_str = "csd\n";
        break;
      }
      send_success(client_fd, mode_str);
    }
  } else if (streq("enable_animations", *args)) {
    ipc_handle_bool(args, num, client_fd, &enable_animations, IPC_FLAG_NONE);
  } else if (streq("workspace_anim_direction", *args)) {
    if (num >= 2) {
      if (streq(args[1], "vertical")) {
        workspace_anim_direction = WORKSPACE_ANIM_VERTICAL;
        send_success(client_fd, "workspace_anim_direction set to vertical\n");
      } else if (streq(args[1], "horizontal")) {
        workspace_anim_direction = WORKSPACE_ANIM_HORIZONTAL;
        send_success(client_fd, "workspace_anim_direction set to horizontal\n");
      } else {
        send_failure(client_fd, "workspace_anim_direction: must be 'vertical' or 'horizontal'\n");
      }
    } else {
      send_success(client_fd, workspace_anim_direction == WORKSPACE_ANIM_VERTICAL ? "vertical\n" : "horizontal\n");
    }
  } else if (streq("workspace_anim_slide_up", *args)) {
    ipc_handle_bool(args, num, client_fd, &workspace_anim_slide_up, IPC_FLAG_NONE);
  } else if (streq("edge_scroller_pointer_focus", *args)) {
    ipc_handle_bool(args, num, client_fd, &edge_scroller_pointer_focus, IPC_FLAG_NONE);
  } else if (streq("tab_color_bar_bg", *args)) {
    if (num >= 2) {
      float r, g, b, a = 1.0f;
      int n = sscanf(args[1], "%f %f %f %f", &r, &g, &b, &a);
      if (n >= 3) {
        color_bar_bg[0] = r; color_bar_bg[1] = g;
        color_bar_bg[2] = b; color_bar_bg[3] = a;
        for (output_t *m = mon_head; m != NULL; m = m->next)
          for (desktop_t *d = m->desk; d != NULL; d = d->next)
            if (d->root != NULL)
              tabs_rebuild(d->root);
        send_success(client_fd, "tab_color_bar_bg set\n");
      } else {
        send_failure(client_fd, "config tab_color_bar_bg: expected \"R G B [A]\"\n");
      }
    } else {
      char buf[128];
      snprintf(buf, sizeof(buf), "%.3f %.3f %.3f %.3f\n",
        color_bar_bg[0], color_bar_bg[1], color_bar_bg[2], color_bar_bg[3]);
      send_success(client_fd, buf);
    }
  } else if (streq("tab_color_bg", *args)) {
    if (num >= 2) {
      float r, g, b, a = 1.0f;
      int n = sscanf(args[1], "%f %f %f %f", &r, &g, &b, &a);
      if (n >= 3) {
        color_tab_bg[0] = r; color_tab_bg[1] = g;
        color_tab_bg[2] = b; color_tab_bg[3] = a;
        for (output_t *m = mon_head; m != NULL; m = m->next)
          for (desktop_t *d = m->desk; d != NULL; d = d->next)
            if (d->root != NULL)
              tabs_rebuild(d->root);
        send_success(client_fd, "tab_color_bg set\n");
      } else {
        send_failure(client_fd, "config tab_color_bg: expected \"R G B [A]\"\n");
      }
    } else {
      char buf[128];
      snprintf(buf, sizeof(buf), "%.3f %.3f %.3f %.3f\n",
        color_tab_bg[0], color_tab_bg[1], color_tab_bg[2], color_tab_bg[3]);
      send_success(client_fd, buf);
    }
  } else if (streq("tab_color_bg_active", *args)) {
    if (num >= 2) {
      float r, g, b, a = 1.0f;
      int n = sscanf(args[1], "%f %f %f %f", &r, &g, &b, &a);
      if (n >= 3) {
        color_tab_bg_active[0] = r; color_tab_bg_active[1] = g;
        color_tab_bg_active[2] = b; color_tab_bg_active[3] = a;
        for (output_t *m = mon_head; m != NULL; m = m->next)
          for (desktop_t *d = m->desk; d != NULL; d = d->next)
            if (d->root != NULL)
              tabs_rebuild(d->root);
        send_success(client_fd, "tab_color_bg_active set\n");
      } else {
        send_failure(client_fd, "config tab_color_bg_active: expected \"R G B [A]\"\n");
      }
    } else {
      char buf[128];
      snprintf(buf, sizeof(buf), "%.3f %.3f %.3f %.3f\n",
        color_tab_bg_active[0], color_tab_bg_active[1], color_tab_bg_active[2], color_tab_bg_active[3]);
      send_success(client_fd, buf);
    }
  } else if (streq("tab_color_text", *args)) {
    if (num >= 2) {
      float r, g, b, a = 1.0f;
      int n = sscanf(args[1], "%f %f %f %f", &r, &g, &b, &a);
      if (n >= 3) {
        color_tab_text[0] = r; color_tab_text[1] = g;
        color_tab_text[2] = b; color_tab_text[3] = a;
        for (output_t *m = mon_head; m != NULL; m = m->next)
          for (desktop_t *d = m->desk; d != NULL; d = d->next)
            if (d->root != NULL)
              tabs_rebuild(d->root);
        send_success(client_fd, "tab_color_text set\n");
      } else {
        send_failure(client_fd, "config tab_color_text: expected \"R G B [A]\"\n");
      }
    } else {
      char buf[128];
      snprintf(buf, sizeof(buf), "%.3f %.3f %.3f %.3f\n",
        color_tab_text[0], color_tab_text[1], color_tab_text[2], color_tab_text[3]);
      send_success(client_fd, buf);
    }
  } else if (streq("tab_color_text_active", *args)) {
    if (num >= 2) {
      float r, g, b, a = 1.0f;
      int n = sscanf(args[1], "%f %f %f %f", &r, &g, &b, &a);
      if (n >= 3) {
        color_tab_text_active[0] = r; color_tab_text_active[1] = g;
        color_tab_text_active[2] = b; color_tab_text_active[3] = a;
        for (output_t *m = mon_head; m != NULL; m = m->next)
          for (desktop_t *d = m->desk; d != NULL; d = d->next)
            if (d->root != NULL)
              tabs_rebuild(d->root);
        send_success(client_fd, "tab_color_text_active set\n");
      } else {
        send_failure(client_fd, "config tab_color_text_active: expected \"R G B [A]\"\n");
      }
    } else {
      char buf[128];
      snprintf(buf, sizeof(buf), "%.3f %.3f %.3f %.3f\n",
        color_tab_text_active[0], color_tab_text_active[1], color_tab_text_active[2], color_tab_text_active[3]);
      send_success(client_fd, buf);
    }
  } else if (streq("tab_color_sep", *args)) {
    if (num >= 2) {
      float r, g, b, a = 1.0f;
      int n = sscanf(args[1], "%f %f %f %f", &r, &g, &b, &a);
      if (n >= 3) {
        color_tab_sep[0] = r; color_tab_sep[1] = g;
        color_tab_sep[2] = b; color_tab_sep[3] = a;
        for (output_t *m = mon_head; m != NULL; m = m->next)
          for (desktop_t *d = m->desk; d != NULL; d = d->next)
            if (d->root != NULL)
              tabs_rebuild(d->root);
        send_success(client_fd, "tab_color_sep set\n");
      } else {
        send_failure(client_fd, "config tab_color_sep: expected \"R G B [A]\"\n");
      }
    } else {
      char buf[128];
      snprintf(buf, sizeof(buf), "%.3f %.3f %.3f %.3f\n",
        color_tab_sep[0], color_tab_sep[1], color_tab_sep[2], color_tab_sep[3]);
      send_success(client_fd, buf);
    }
  } else if (streq("text_font", *args)) {
    if (num >= 2) {
      snprintf(text_font, sizeof(text_font), "%s", args[1]);
      for (output_t *m = mon_head; m != NULL; m = m->next)
        for (desktop_t *d = m->desk; d != NULL; d = d->next)
          if (d->root != NULL)
            tabs_rebuild(d->root);
      send_success(client_fd, "text_font set\n");
    } else {
      char buf[256];
      snprintf(buf, sizeof(buf), "%s\n", text_font);
      send_success(client_fd, buf);
    }
  } else if (streq("text_height", *args)) {
    if (num >= 2) {
      int val = atoi(args[1]);
      if (val > 0) {
        text_height = val;
        for (output_t *m = mon_head; m != NULL; m = m->next)
          for (desktop_t *d = m->desk; d != NULL; d = d->next)
            if (d->root != NULL)
              tabs_rebuild(d->root);
        send_success(client_fd, "text_height set\n");
      } else {
        send_failure(client_fd, "config text_height: value must be > 0\n");
      }
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "%d\n", text_height);
      send_success(client_fd, buf);
    }
  } else if (streq("scroller_default_proportion", *args)) {
    ipc_handle_float(args, num, client_fd, &scroller_default_proportion, IPC_FLAG_NONE, 0.1f, 1.0f, "%.2f\n", NULL);
  } else if (streq("scroller_proportion_preset", *args)) {
    if (num >= 2) {
      // Parse comma-separated list of proportions
      char *value = args[1];
      int count = 1;
      for (char *p = value; *p; p++)
        if (*p == ',') count++;

      // Free old presets
      if (scroller_proportion_preset)
        free(scroller_proportion_preset);

      // Allocate new array
      scroller_proportion_preset = malloc(count * sizeof(float));
      if (!scroller_proportion_preset) {
        send_failure(client_fd, "memory allocation failed\n");
        return;
      }

      // Parse values
      char *token = strtok(value, ",");
      int i = 0;
      while (token && i < count) {
        float val = atof(token);
        if (val < 0.1f) val = 0.1f;
        if (val > 1.0f) val = 1.0f;
        scroller_proportion_preset[i++] = val;
        token = strtok(NULL, ",");
      }
      scroller_proportion_preset_count = i;

      send_success(client_fd, "scroller_proportion_preset set\n");
    } else {
      char buf[512];
      int offset = 0;
      for (int i = 0; i < scroller_proportion_preset_count && offset < 500; i++) {
        offset += snprintf(buf + offset, sizeof(buf) - offset, "%.2f%s",
                          scroller_proportion_preset[i],
                          i < scroller_proportion_preset_count - 1 ? "," : "\n");
      }
      send_success(client_fd, buf);
    }
  } else if (streq("scroller_default_proportion_single", *args)) {
    ipc_handle_float(args, num, client_fd, &scroller_default_proportion_single, IPC_FLAG_NONE, 0.1f, 1.0f, "%.2f\n", NULL);
  } else if (streq("scroller_focus_center", *args)) {
    ipc_handle_bool(args, num, client_fd, &scroller_focus_center, IPC_FLAG_NONE);
  } else if (streq("scroller_prefer_center", *args)) {
    ipc_handle_bool(args, num, client_fd, &scroller_prefer_center, IPC_FLAG_NONE);
  } else if (streq("scroller_prefer_overspread", *args)) {
    ipc_handle_bool(args, num, client_fd, &scroller_prefer_overspread, IPC_FLAG_NONE);
  } else if (streq("scroller_ignore_proportion_single", *args)) {
    ipc_handle_bool(args, num, client_fd, &scroller_ignore_proportion_single, IPC_FLAG_NONE);
  } else if (streq("scroller_structs", *args)) {
    ipc_handle_int(args, num, client_fd, &scroller_structs, IPC_FLAG_NONE, 0, 1000000, NULL);
  } else if (streq("focus_follows_pointer", *args)) {
    ipc_handle_bool(args, num, client_fd, &focus_follows_pointer, IPC_FLAG_COMMIT);
  } else if (streq("pointer_follows_focus", *args)) {
    ipc_handle_bool(args, num, client_fd, &pointer_follows_focus, IPC_FLAG_COMMIT);
  } else if (streq("split_ratio", *args)) {
    if (num >= 2) {
      double val = atof(args[1]);
      if (val > 0 && val < 1) {
        split_ratio = val;
        transaction_commit_dirty();
        send_success(client_fd, "split_ratio set\n");
      } else {
        send_failure(client_fd, "config split_ratio: invalid value\n");
      }
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "%f\n", split_ratio);
      send_success(client_fd, buf);
    }
  } else if (streq("top_padding", *args)) {
    ipc_handle_int(args, num, client_fd, &padding.top, IPC_FLAG_COMMIT, INT_MIN, INT_MAX, NULL);
  } else if (streq("right_padding", *args)) {
    ipc_handle_int(args, num, client_fd, &padding.right, IPC_FLAG_COMMIT, INT_MIN, INT_MAX, NULL);
  } else if (streq("bottom_padding", *args)) {
    ipc_handle_int(args, num, client_fd, &padding.bottom, IPC_FLAG_COMMIT, INT_MIN, INT_MAX, NULL);
  } else if (streq("left_padding", *args)) {
    ipc_handle_int(args, num, client_fd, &padding.left, IPC_FLAG_COMMIT, INT_MIN, INT_MAX, NULL);
  } else if (streq("normal_border_color", *args)) {
    if (num >= 2) {
      strncpy(normal_border_color, args[1], sizeof(normal_border_color) - 1);
      normal_border_color[sizeof(normal_border_color) - 1] = '\0';
      refresh_border_colors();
      transaction_commit_dirty();
      send_success(client_fd, "normal_border_color set\n");
    } else {
      send_success(client_fd, normal_border_color);
      send_success(client_fd, "\n");
    }
  } else if (streq("active_border_color", *args)) {
    if (num >= 2) {
      strncpy(active_border_color, args[1], sizeof(active_border_color) - 1);
      active_border_color[sizeof(active_border_color) - 1] = '\0';
      refresh_border_colors();
      transaction_commit_dirty();
      send_success(client_fd, "active_border_color set\n");
    } else {
      send_success(client_fd, active_border_color);
      send_success(client_fd, "\n");
    }
  } else if (streq("focused_border_color", *args)) {
    if (num >= 2) {
      strncpy(focused_border_color, args[1], sizeof(focused_border_color) - 1);
      focused_border_color[sizeof(focused_border_color) - 1] = '\0';
      refresh_border_colors();
      transaction_commit_dirty();
      send_success(client_fd, "focused_border_color set\n");
    } else {
      send_success(client_fd, focused_border_color);
      send_success(client_fd, "\n");
    }
  } else if (streq("presel_feedback_color", *args)) {
    if (num >= 2) {
      strncpy(presel_feedback_color, args[1], sizeof(presel_feedback_color) - 1);
      presel_feedback_color[sizeof(presel_feedback_color) - 1] = '\0';
      transaction_commit_dirty();
      send_success(client_fd, "presel_feedback_color set\n");
    } else {
      send_success(client_fd, presel_feedback_color);
      send_success(client_fd, "\n");
    }
  } else if (streq("normal_border_gradient", *args) || streq("active_border_gradient", *args) ||
      streq("focused_border_gradient", *args)) {
    float *grad;
    int *gcount;
    float *gangle;
    if (args[0][0] == 'n') {
      grad = normal_border_gradient;
      gcount = &normal_border_gradient_count;
      gangle = &normal_border_gradient_angle;
    } else if (args[0][0] == 'a') {
      grad = active_border_gradient;
      gcount = &active_border_gradient_count;
      gangle = &active_border_gradient_angle;
    } else {
      grad = focused_border_gradient;
      gcount = &focused_border_gradient_count;
      gangle = &focused_border_gradient_angle;
    }

    if (num >= 2) {
      char joined[512] = "";
      for (int i = 1; i < num; i++) {
        if (i > 1) strncat(joined, " ", sizeof(joined) - strlen(joined) - 1);
        strncat(joined, args[i], sizeof(joined) - strlen(joined) - 1);
      }

      if (streq(joined, "clear")) {
        *gcount = 0;
        *gangle = 0.0f;
      } else {
        ipc_parse_gradient(joined, grad, gcount, gangle);
      }

      refresh_border_colors();
      transaction_commit_dirty();
      send_success(client_fd, "border gradient set\n");
    } else {
      char buf[512];
      ipc_format_gradient(buf, sizeof(buf), grad, *gcount, *gangle);
      send_success(client_fd, buf);
      send_success(client_fd, "\n");
    }
  } else if (streq("normal_border_gradient2", *args) || streq("active_border_gradient2", *args) ||
      streq("focused_border_gradient2", *args)) {
    float *grad;
    int *gcount;
    float *gangle;
    if (args[0][0] == 'n') {
      grad = normal_border_gradient2;
      gcount = &normal_border_gradient2_count;
      gangle = &normal_border_gradient2_angle;
    } else if (args[0][0] == 'a') {
      grad = active_border_gradient2;
      gcount = &active_border_gradient2_count;
      gangle = &active_border_gradient2_angle;
    } else {
      grad = focused_border_gradient2;
      gcount = &focused_border_gradient2_count;
      gangle = &focused_border_gradient2_angle;
    }

    if (num >= 2) {
      char joined[512] = "";
      for (int i = 1; i < num; i++) {
        if (i > 1) strncat(joined, " ", sizeof(joined) - strlen(joined) - 1);
        strncat(joined, args[i], sizeof(joined) - strlen(joined) - 1);
      }

      if (streq(joined, "clear")) {
        *gcount = 0;
        *gangle = 0.0f;
      } else {
        ipc_parse_gradient(joined, grad, gcount, gangle);
      }

      refresh_border_colors();
      transaction_commit_dirty();
      send_success(client_fd, "border gradient2 set\n");
    } else {
      char buf[512];
      ipc_format_gradient(buf, sizeof(buf), grad, *gcount, *gangle);
      send_success(client_fd, buf);
      send_success(client_fd, "\n");
    }
  } else if (streq("normal_border_gradient_lerp", *args) || streq("active_border_gradient_lerp", *args) ||
      streq("focused_border_gradient_lerp", *args)) {
    float *lerp;
    if (args[0][0] == 'n') lerp = &normal_border_gradient_lerp;
    else if (args[0][0] == 'a') lerp = &active_border_gradient_lerp;
    else lerp = &focused_border_gradient_lerp;

    if (num >= 2) {
      *lerp = (float)atof(args[1]);
      if (*lerp < 0.0f) *lerp = 0.0f;
      if (*lerp > 1.0f) *lerp = 1.0f;
      refresh_border_colors();
      transaction_commit_dirty();
      send_success(client_fd, "border gradient lerp set\n");
    } else {
      char buf[32];
      snprintf(buf, sizeof(buf), "%f\n", *lerp);
      send_success(client_fd, buf);
    }
  } else if (streq("automatic_scheme", *args)) {
    if (num >= 2) {
      if (streq("longest_side", args[1]) || streq("longest-side", args[1])) {
        automatic_scheme = SCHEME_LONGEST_SIDE;
      } else if (streq("alternate", args[1])) {
        automatic_scheme = SCHEME_ALTERNATE;
      } else if (streq("spiral", args[1])) {
        automatic_scheme = SCHEME_SPIRAL;
      } else {
        send_failure(client_fd, "config automatic_scheme: invalid value\n");
        return;
      }
      transaction_commit_dirty();
      send_success(client_fd, "automatic_scheme set\n");
    } else {
      char scheme_buf[64];
      const char *scheme_str = "spiral";
      if (automatic_scheme == SCHEME_LONGEST_SIDE) scheme_str = "longest_side";
      else if (automatic_scheme == SCHEME_ALTERNATE) scheme_str = "alternate";
      snprintf(scheme_buf, sizeof(scheme_buf), "%s\n", scheme_str);
      send_success(client_fd, scheme_buf);
    }
  } else if (streq("initial_polarity", *args)) {
    if (num >= 2) {
      if (streq("first_child", args[1]) || streq("first-child", args[1])) {
        initial_polarity = FIRST_CHILD;
      } else if (streq("second_child", args[1]) || streq("second-child", args[1])) {
        initial_polarity = SECOND_CHILD;
      } else {
        send_failure(client_fd, "config initial_polarity: invalid value\n");
        return;
      }
      send_success(client_fd, "initial_polarity set\n");
    } else {
      send_success(client_fd, initial_polarity == FIRST_CHILD ? "first_child\n" : "second_child\n");
    }
  } else if (streq("directional_focus_tightness", *args)) {
    ipc_handle_int(args, num, client_fd, &directional_focus_tightness, IPC_FLAG_NONE, 0, 100, "invalid value");
  } else if (streq("mapping_events_count", *args)) {
    ipc_handle_int(args, num, client_fd, &mapping_events_count, IPC_FLAG_NONE, 0, 1000000, "invalid value");
  } else if (streq("minimize_to_scratchpad", *args)) {
    ipc_handle_bool(args, num, client_fd, &minimize_to_scratchpad, IPC_FLAG_NONE);
  } else if (streq("ignore_ewmh_fullscreen", *args)) {
    ipc_handle_int(args, num, client_fd, &ignore_ewmh_fullscreen, IPC_FLAG_NONE, 0, 2, "invalid value (0-2)");
  } else if (streq("click_to_focus", *args)) {
    ipc_handle_bool(args, num, client_fd, &click_to_focus, IPC_FLAG_NONE);
  } else if (streq("record_history", *args)) {
    ipc_handle_bool(args, num, client_fd, &record_history, IPC_FLAG_NONE);
  } else if (streq("blur_enabled", *args)) {
    ipc_handle_bool(args, num, client_fd, &blur_enabled, IPC_FLAG_NONE);
  } else if (streq("blur_algorithm", *args)) {
    if (num >= 2) {
      enum blur_algorithm algo = blur_algorithm_from_str(args[1]);
      if (algo == BLUR_ALGORITHM_NONE && strcmp(args[1], "none") != 0) {
        send_failure(client_fd, "config blur_algorithm: unknown algorithm\n");
      } else {
        blur_algorithm = algo;
        send_success(client_fd, "blur_algorithm set\n");
      }
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "%s\n", blur_algorithm_to_str(blur_algorithm));
      send_success(client_fd, buf);
    }
  } else if (streq("blur_passes", *args)) {
    ipc_handle_int(args, num, client_fd, &blur_passes, IPC_FLAG_NONE, 1, 10, "value must be 1-10");
  } else if (streq("blur_radius", *args)) {
    if (num >= 2) {
      float val = atof(args[1]);
      if (val > 0.0f) {
        blur_radius = val;
        send_success(client_fd, "blur_radius set\n");
      } else {
        send_failure(client_fd, "config blur_radius: value must be > 0\n");
      }
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "%.2f\n", blur_radius);
      send_success(client_fd, buf);
    }
  } else if (streq("refraction_strength", *args)) {
    ipc_handle_float(args, num, client_fd, &refraction_strength, IPC_FLAG_NONE, 0.0f, 30.0f, "%.3f\n", "value must be 0.0-30.0");
  } else if (streq("refraction_edge_size_px", *args)) {
    ipc_handle_float(args, num, client_fd, &refraction_edge_size_px, IPC_FLAG_NONE, 0.0f, 400.0f, "%.3f\n", "value must be 0.0-400.0");
  } else if (streq("refraction_corner_radius_px", *args)) {
    ipc_handle_float(args, num, client_fd, &refraction_corner_radius_px, IPC_FLAG_NONE, 0.0f, 400.0f, "%.3f\n", "value must be 0.0-400.0");
  } else if (streq("refraction_normal_pow", *args)) {
    ipc_handle_float(args, num, client_fd, &refraction_normal_pow, IPC_FLAG_NONE, 0.0f, 8.0f, "%.3f\n", "value must be 0.0-8.0");
  } else if (streq("refraction_rgb_fringing", *args)) {
    ipc_handle_float(args, num, client_fd, &refraction_rgb_fringing, IPC_FLAG_NONE, 0.0f, 1.0f, "%.6f\n", "value must be 0.0-1.0");
  } else if (streq("refraction_texture_repeat_mode", *args)) {
    if (num >= 2) {
      int val = atoi(args[1]);
      if (val == 0 || val == 1) {
        refraction_texture_repeat_mode = val;
        send_success(client_fd, "refraction_texture_repeat_mode set\n");
      } else {
        send_failure(client_fd, "config refraction_texture_repeat_mode: value must be 0 (clamp) or 1 (mirror)\n");
      }
    } else {
      char buf[32];
      snprintf(buf, sizeof(buf), "%d\n", refraction_texture_repeat_mode);
      send_success(client_fd, buf);
    }
  } else if (streq("refraction_offset", *args)) {
    ipc_handle_float(args, num, client_fd, &refraction_offset, IPC_FLAG_NONE, 0.0f, 8.0f, "%.3f\n", "value must be 0.0-8.0");
  } else if (streq("blur_downsample", *args)) {
    if (num >= 2) {
      int val = atoi(args[1]);
      if (val >= 1 && val <= 8) {
        blur_downsample = val;
        for (output_t *m = mon_head; m; m = m->next) {
          if (m && m->blur_ctx)
            blur_output_resize(m->blur_ctx, m->width, m->height, m);
        }
        send_success(client_fd, "blur_downsample set\n");
      } else {
        send_failure(client_fd, "config blur_downsample: value must be 1-8\n");
      }
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "%d\n", blur_downsample);
      send_success(client_fd, buf);
    }
  } else if (streq("blur_vibrancy", *args)) {
    ipc_handle_float(args, num, client_fd, &blur_vibrancy, IPC_FLAG_NONE, 0.0f, 1.0f, "%.3f\n", "value must be 0.0-1.0");
  } else if (streq("blur_vibrancy_darkness", *args)) {
    ipc_handle_float(args, num, client_fd, &blur_vibrancy_darkness, IPC_FLAG_NONE, 0.0f, 1.0f, "%.3f\n", "value must be 0.0-1.0");
  } else if (streq("blur_noise_strength", *args)) {
    ipc_handle_float(args, num, client_fd, &blur_noise_strength, IPC_FLAG_NONE, 0.0f, 1.0f, "%.3f\n", "value must be 0.0-1.0");
  } else if (streq("blur_brightness", *args)) {
    ipc_handle_float(args, num, client_fd, &blur_brightness, IPC_FLAG_NONE, 0.5f, 2.0f, "%.3f\n", "value must be 0.5-2.0");
  } else if (streq("blur_contrast", *args)) {
    ipc_handle_float(args, num, client_fd, &blur_contrast, IPC_FLAG_NONE, 0.5f, 2.0f, "%.3f\n", "value must be 0.5-2.0");
  } else if (streq("mica_enabled", *args)) {
    if (num >= 2) {
      mica_enabled = (strcmp(args[1], "true") == 0);
      for (output_t *output = mon_head; output; output = output->next)
        blur_invalidate_mica(output->blur_ctx);
      send_success(client_fd, "mica_enabled set\n");
    } else {
      send_success(client_fd, mica_enabled ? "true\n" : "false\n");
    }
  } else if (streq("mica_tint_strength", *args)) {
    if (num >= 2) {
      float val = (float)atof(args[1]);
      if (val >= 0.0f && val <= 1.0f) {
        mica_tint_strength = val;
        for (output_t *output = mon_head; output; output = output->next)
          blur_invalidate_mica(output->blur_ctx);
        send_success(client_fd, "mica_tint_strength set\n");
      } else {
        send_failure(client_fd, "config mica_tint_strength: value must be 0.0-1.0\n");
      }
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "%.3f\n", mica_tint_strength);
      send_success(client_fd, buf);
    }
  } else if (streq("mica_tint", *args)) {
    if (num >= 2) {
      float r, g, b, a = 1.0f;
      int n = sscanf(args[1], "%f %f %f %f", &r, &g, &b, &a);
      if (n >= 3) {
        mica_tint[0] = r; mica_tint[1] = g;
        mica_tint[2] = b; mica_tint[3] = a;
        for (output_t *output = mon_head; output; output = output->next)
          blur_invalidate_mica(output->blur_ctx);
        send_success(client_fd, "mica_tint set\n");
      } else {
        send_failure(client_fd, "config mica_tint: expected \"R G B [A]\"\n");
      }
    } else {
      char buf[128];
      snprintf(buf, sizeof(buf), "%.3f %.3f %.3f %.3f\n",
      	mica_tint[0], mica_tint[1], mica_tint[2], mica_tint[3]);
    send_success(client_fd, buf);
  }
  } else if (streq("acrylic_tint", *args)) {
    if (num >= 2) {
      float r, g, b, a = 1.0f;
      int n = sscanf(args[1], "%f %f %f %f", &r, &g, &b, &a);
      if (n >= 3) {
        acrylic_tint[0] = r; acrylic_tint[1] = g;
        acrylic_tint[2] = b; acrylic_tint[3] = a;
        send_success(client_fd, "acrylic_tint set\n");
      } else {
        send_failure(client_fd, "config acrylic_tint: expected \"R G B [A]\"\n");
      }
    } else {
      char buf[128];
      snprintf(buf, sizeof(buf), "%.3f %.3f %.3f %.3f\n",
        acrylic_tint[0], acrylic_tint[1], acrylic_tint[2], acrylic_tint[3]);
      send_success(client_fd, buf);
    }
  } else if (streq("acrylic_tint_strength", *args)) {
    ipc_handle_float(args, num, client_fd, &acrylic_tint_strength, IPC_FLAG_NONE, 0.0f, 1.0f, "%.3f\n", "value must be 0.0-1.0");
  } else if (streq("acrylic_noise_strength", *args)) {
    ipc_handle_float(args, num, client_fd, &acrylic_noise_strength, IPC_FLAG_NONE, 0.0f, 1.0f, "%.3f\n", "value must be 0.0-1.0");
  } else if (streq("acrylic_light_anchor", *args)) {
    if (num >= 2) {
    	float a, b;
     	int n = sscanf(args[1], "%f %f", &a, &b);
      if (n == 2 && a >= -1.0f && a <= 1.0f && b >= -1.0f && b <= 1.0f) {
        acrylic_light_anchor[0] = a;
        acrylic_light_anchor[1] = b;
        send_success(client_fd, "acrylic_light_anchor set\n");
      } else {
        send_failure(client_fd, "config acrylic_light_anchor: values must be -1.0 to 1.0\n");
      }
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "%.3f\n", acrylic_light_anchor[0]);
      send_success(client_fd, buf);
    }
  } else if (streq("acrylic_blur_passes", *args)) {
    ipc_handle_int(args, num, client_fd, &acrylic_blur_passes, IPC_FLAG_NONE, 0, 10, "value must be 0-10");
  } else if (streq("screen_shader", *args)) {
    if (num >= 2) {
      if (!screen_shader_set(args[1])) {
        send_failure(client_fd,
          "config screen_shader: unknown shader (builtin: none grayscale invert sepia nightlight)\n");
      } else {
        send_success(client_fd, "screen_shader set\n");
      }
    } else {
      char buf[256];
      snprintf(buf, sizeof(buf), "%s\n", screen_shader_get_name());
      send_success(client_fd, buf);
    }
  } else if (streq("screen_shader_file", *args)) {
    if (num >= 2) {
      if (!screen_shader_load_file(args[1])) {
        send_failure(client_fd, "config screen_shader_file: failed to load shader\n");
      } else {
        send_success(client_fd, "screen_shader_file loaded\n");
      }
    } else {
      send_failure(client_fd, "config screen_shader_file: missing path argument\n");
    }
  } else if (streq("screen_shader_enabled", *args)) {
    ipc_handle_bool(args, num, client_fd, &screen_shader_enabled, IPC_FLAG_NONE);
  } else if (streq("animation_bezier", *args)) {
    if (num >= 2) {
      if (bezier_exists(args[1])) {
        animation_set_bezier(args[1]);
        send_success(client_fd, "animation_bezier set\n");
      } else {
        send_failure(client_fd, "config animation_bezier: no such bezier curve\n");
      }
    } else {
      char buf[96];
      snprintf(buf, sizeof(buf), "%s\n", animation_get_bezier());
      send_success(client_fd, buf);
    }
  } else if (streq("animation_duration", *args)) {
    if (num >= 2) {
      int val = atoi(args[1]);
      if (val > 0) {
        animation_set_duration((uint32_t)val);
        send_success(client_fd, "animation_duration set\n");
      } else {
        send_failure(client_fd, "config animation_duration: must be > 0\n");
      }
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "%u\n", animation_get_duration());
      send_success(client_fd, buf);
    }
  } else if (streq("animation", *args)) {
    if (num < 2) {
      send_failure(client_fd, "config animation: expected <type> [bezier|duration|spring] [value]\n");
    } else if (num >= 3 && streq("spring", args[2])) {
      const char *sname = num >= 4 ? args[3] : "";
      if (sname[0] != '\0' && !spring_exists(sname)) {
        send_failure(client_fd, "config animation: no such spring curve\n");
      } else if (animation_set_type_spring(args[1], sname)) {
        send_success(client_fd, "animation type spring set\n");
      } else {
        send_failure(client_fd, "config animation: unknown type\n");
      }
    } else if (num >= 3 && streq("bezier", args[2])) {
      const char *bname = num >= 4 ? args[3] : "";
      if (bname[0] != '\0' && !bezier_exists(bname)) {
        send_failure(client_fd, "config animation: no such bezier curve\n");
      } else if (animation_set_type_config(args[1], bname, 0)) {
        send_success(client_fd, "animation type bezier set\n");
      } else {
        send_failure(client_fd, "config animation: unknown type\n");
      }
    } else if (num >= 3 && streq("duration", args[2])) {
      if (num < 4) {
        send_failure(client_fd, "config animation <type> duration: expected value\n");
      } else {
        int val = atoi(args[3]);
        if (val <= 0) {
          send_failure(client_fd, "config animation <type> duration: must be > 0\n");
        } else if (animation_set_type_config(args[1], NULL, (uint32_t)val)) {
          send_success(client_fd, "animation type duration set\n");
        } else {
          send_failure(client_fd, "config animation: unknown type\n");
        }
      }
    } else {
      // query type config
      int idx = animation_type_from_name(args[1]);
      if (idx < 0) {
        send_failure(client_fd, "config animation: unknown type\n");
      } else {
        const char *bname = animation_type_get_bezier(args[1]);
        uint32_t dur = animation_type_get_duration(args[1]);
        const char *sname = animation_type_get_spring(args[1]);
        char buf[256];
        int off = 0;
        if (sname) {
          off = snprintf(buf, sizeof(buf), "spring: %s\n", sname);
        } else {
          off = snprintf(buf, sizeof(buf), "bezier: %s\n", bname ? bname : "(global default)");
        }
        snprintf(buf + off, sizeof(buf) - off, "duration: %u\n",
          dur > 0 ? dur : animation_get_duration());
        send_success(client_fd, buf);
      }
    }
  } else {
    send_failure(client_fd, "config: unknown setting\n");
  }
}

static void ipc_cmd_focus(char **args, int num, int client_fd) {
  if (num < 1) {
    send_failure(client_fd, "focus: missing direction\n");
    return;
  }

  bind_action_t action = bind_action_from_name(*args);
  if (action != BIND_NONE) {
    execute_bind_action(action);
    send_success(client_fd, "focused\n");
  } else {
    send_failure(client_fd, "focus: unknown direction\n");
  }
}

static void ipc_cmd_swap(char **args, int num, int client_fd) {
  if (num < 1) {
    send_failure(client_fd, "swap: missing direction\n");
    return;
  }

  bind_action_t action = bind_action_from_name(*args);
  if (action != BIND_NONE) {
    execute_bind_action(action);
    send_success(client_fd, "swapped\n");
  } else {
    send_failure(client_fd, "swap: unknown direction\n");
  }
}

static void ipc_cmd_presel(char **args, int num, int client_fd) {
  if (num < 1) {
    send_failure(client_fd, "presel: missing direction\n");
    return;
  }

  bind_action_t action = bind_action_from_name(*args);
  if (action != BIND_NONE) {
    execute_bind_action(action);
    const char *name = *args;
    if (strcmp(name, "cancel") == 0) {
      send_success(client_fd, "presel cancelled\n");
    } else {
      send_success(client_fd, "presel set\n");
    }
  } else {
    send_failure(client_fd, "presel: unknown direction\n");
  }
}

static void ipc_cmd_resize(char **args, int num, int client_fd) {
  if (num < 1) {
    send_failure(client_fd, "resize: missing direction\n");
    return;
  }

  bind_action_t action = bind_action_from_name(*args);
  if (action != BIND_NONE) {
    execute_bind_action(action);
    send_success(client_fd, "resized\n");
  } else {
    send_failure(client_fd, "resize: unknown direction\n");
  }
}

static void ipc_cmd_toggle(char **args, int num, int client_fd) {
  if (num < 1) {
    send_failure(client_fd, "toggle: missing property\n");
    return;
  }

  if (streq("floating", *args)) {
    toggle_floating();
    send_success(client_fd, "toggled\n");
  } else if (streq("fullscreen", *args)) {
    toggle_fullscreen();
    send_success(client_fd, "toggled\n");
  } else if (streq("pseudo_tiled", *args)) {
    toggle_pseudo_tiled();
    send_success(client_fd, "toggled\n");
  } else if (streq("monocle", *args)) {
    toggle_monocle();
    send_success(client_fd, "toggled\n");
  } else if (streq("block_out_from_screenshare", *args)) {
    toggle_block_out_from_screenshare();
    send_success(client_fd, "toggled\n");
  } else {
    send_failure(client_fd, "toggle: unknown property\n");
  }
}

static void ipc_cmd_rotate(char **args, int num, int client_fd) {
  if (num < 1) {
    send_failure(client_fd, "rotate: missing direction\n");
    return;
  }

  bind_action_t action = bind_action_from_name(*args);
  if (action == BIND_ROTATE_CW || action == BIND_ROTATE_CCW) {
    execute_bind_action(action);
    send_success(client_fd, "rotated\n");
  } else {
    send_failure(client_fd, "rotate: unknown direction\n");
  }
}

static void ipc_cmd_flip(char **args, int num, int client_fd) {
  if (num < 1) {
    send_failure(client_fd, "flip: missing direction\n");
    return;
  }

  bind_action_t action = bind_action_from_name(*args);
  if (action == BIND_FLIP_HORIZONTAL || action == BIND_FLIP_VERTICAL) {
    execute_bind_action(action);
    send_success(client_fd, "flipped\n");
  } else {
    send_failure(client_fd, "flip: unknown direction\n");
  }
}

static void ipc_cmd_equalize(char **args, int num, int client_fd) {
  (void)args;
  (void)num;

  output_t *m = server.focused_output;
  if (!m || !m->desk) {
    send_failure(client_fd, "equalize: no focused desktop\n");
    return;
  }

  if (!m->desk->root) {
    send_failure(client_fd, "equalize: no tree\n");
    return;
  }

  equalize_tree(m->desk->root);
  transaction_commit_dirty();
  send_success(client_fd, "equalized\n");
}

static void ipc_cmd_balance(char **args, int num, int client_fd) {
  (void)args;
  (void)num;

  output_t *m = server.focused_output;
  if (!m || !m->desk) {
    send_failure(client_fd, "balance: no focused desktop\n");
    return;
  }

  if (!m->desk->root) {
    send_failure(client_fd, "balance: no tree\n");
    return;
  }

  balance_tree(m->desk->root);
  transaction_commit_dirty();
  send_success(client_fd, "balanced\n");
}

static void ipc_cmd_send(char **args, int num, int client_fd) {
  if (num < 1) {
    send_failure(client_fd, "send: missing direction\n");
    return;
  }

  const char *arg = *args;
  if (strcmp(arg, "next") == 0) {
    send_to_next_desktop();
    send_success(client_fd, "sent\n");
  } else if (strcmp(arg, "prev") == 0 || strcmp(arg, "previous") == 0) {
    send_to_prev_desktop();
    send_success(client_fd, "sent\n");
  } else {
    send_failure(client_fd, "send: unknown direction\n");
  }
}

static void ipc_cmd_rule(char **args, int num, int client_fd) {
  if (num < 1) {
    send_failure(client_fd, "rule: missing arguments\n");
    return;
  }

  char *subcmd = *args;

  if (streq("-a", subcmd) || streq("--add", subcmd)) {
    if (num < 2) {
      send_failure(client_fd, "rule -a: missing app_id\n");
      return;
    }

    args++;
    num--;

    rule_t *r = make_rule();
    if (!r) {
      send_failure(client_fd, "rule: failed to create rule\n");
      return;
    }

    char *app_id = NULL;
    char *title = NULL;
    r->consequence.follow = true;
    r->consequence.has_follow = true;
    r->consequence.focus = true;
    r->consequence.has_focus = true;
    r->consequence.manage = true;
    r->consequence.has_manage = true;

    while (num > 0) {
      char *arg = *args;

      if (arg[0] != '-' && app_id == NULL) {
        app_id = arg;
        strncpy(r->match.app_id, app_id, MAXLEN - 1);
        r->match.app_id[MAXLEN - 1] = '\0';
      } else if (streq("title=", arg) || strncmp(arg, "title=", 6) == 0) {
        title = arg + 6;
        strncpy(r->match.title, title, MAXLEN - 1);
        r->match.title[MAXLEN - 1] = '\0';
      } else if (streq("state=tiled", arg)) {
        r->consequence.state = STATE_TILED;
        r->consequence.has_state = true;
      } else if (streq("state=floating", arg)) {
        r->consequence.state = STATE_FLOATING;
        r->consequence.has_state = true;
      } else if (streq("state=fullscreen", arg)) {
        r->consequence.state = STATE_FULLSCREEN;
        r->consequence.has_state = true;
      } else if (streq("state=pseudo_tiled", arg)) {
        r->consequence.state = STATE_PSEUDO_TILED;
        r->consequence.has_state = true;
      } else if (streq("desktop=^", arg) || (strlen(arg) > 8 && strncmp(arg, "desktop=", 8) == 0)) {
        char *desk = arg + 8;
        strncpy(r->consequence.desktop, desk, SMALEN - 1);
        r->consequence.desktop[SMALEN - 1] = '\0';
        r->consequence.has_desktop = true;
      } else if (streq("follow=on", arg)) {
        r->consequence.follow = true;
        r->consequence.has_follow = true;
      } else if (streq("follow=off", arg)) {
        r->consequence.follow = false;
        r->consequence.has_follow = true;
      } else if (streq("focus=on", arg)) {
        r->consequence.focus = true;
        r->consequence.has_focus = true;
      } else if (streq("focus=off", arg)) {
        r->consequence.focus = false;
        r->consequence.has_focus = true;
      } else if (streq("manage=on", arg)) {
        r->consequence.manage = true;
        r->consequence.has_manage = true;
      } else if (streq("manage=off", arg)) {
        r->consequence.manage = false;
        r->consequence.has_manage = true;
      } else if (streq("locked=on", arg)) {
        r->consequence.locked = true;
        r->consequence.has_locked = true;
      } else if (streq("locked=off", arg)) {
        r->consequence.locked = false;
        r->consequence.has_locked = true;
      } else if (streq("hidden=on", arg)) {
        r->consequence.hidden = true;
        r->consequence.has_hidden = true;
      } else if (streq("hidden=off", arg)) {
        r->consequence.hidden = false;
        r->consequence.has_hidden = true;
      } else if (streq("sticky=on", arg)) {
        r->consequence.sticky = true;
        r->consequence.has_sticky = true;
      } else if (streq("sticky=off", arg)) {
        r->consequence.sticky = false;
        r->consequence.has_sticky = true;
      } else if (streq("one_shot", arg)) {
        r->match.one_shot = true;
      } else if (strncmp(arg, "scroller_proportion=", 20) == 0) {
        float val = atof(arg + 20);
        if (val >= 0.1f && val <= 1.0f) {
          r->consequence.scroller_proportion = val;
          r->consequence.has_scroller_proportion = true;
        }
      } else if (strncmp(arg, "scroller_proportion_single=", 27) == 0) {
        float val = atof(arg + 27);
        if (val >= 0.1f && val <= 1.0f) {
          r->consequence.scroller_proportion_single = val;
          r->consequence.has_scroller_proportion_single = true;
        }
      } else if (streq("blur=on", arg)) {
        r->consequence.blur = true;
        r->consequence.has_blur = true;
      } else if (streq("blur=off", arg)) {
        r->consequence.blur = false;
        r->consequence.has_blur = true;
      } else if (streq("mica=on", arg)) {
        r->consequence.mica = true;
        r->consequence.has_mica = true;
      } else if (streq("mica=off", arg)) {
        r->consequence.mica = false;
        r->consequence.has_mica = true;
      } else if (streq("acrylic=on", arg)) {
        r->consequence.acrylic = true;
        r->consequence.has_acrylic = true;
      } else if (streq("acrylic=off", arg)) {
        r->consequence.acrylic = false;
        r->consequence.has_acrylic = true;
      } else if (strncmp("border_radius=", arg, 14) == 0) {
        r->consequence.border_radius = atof(arg + 14);
        r->consequence.has_border_radius = true;
      } else if (streq("block_out_from_screenshare=on", arg)) {
        r->consequence.block_out_from_screenshare = true;
        r->consequence.has_block_out_from_screenshare = true;
      } else if (streq("block_out_from_screenshare=off", arg)) {
        r->consequence.block_out_from_screenshare = false;
        r->consequence.has_block_out_from_screenshare = true;
      }

      args++;
      num--;
    }

    if (!app_id && !title) {
      free(r);
      send_failure(client_fd, "rule -a: must specify app_id or title\n");
      return;
    }

    add_rule(r);
    send_success(client_fd, "rule added\n");

  } else if (streq("-r", subcmd) || streq("--remove", subcmd)) {
    if (num < 2) {
      send_failure(client_fd, "rule -r: missing index\n");
      return;
    }
    args++;
    int idx = atoi(*args);
    if (remove_rule_by_index(idx))
      send_success(client_fd, "rule removed\n");
    else
   		send_failure(client_fd, "rule -r: invalid index\n");

  } else if (streq("-l", subcmd) || streq("--list", subcmd)) {
    char buf[DOORS_BUFSIZ];
    list_rules(buf, sizeof(buf));
    send_success(client_fd, buf);
  } else {
    send_failure(client_fd, "rule: unknown subcommand (use -a, -r, or -l)\n");
  }
}

static void ipc_cmd_keyboard_grouping(char **args, int num, int client_fd) {
  if (num < 1) {
    send_failure(client_fd, "keyboard_grouping: missing argument\n");
    return;
  }

  char *mode = *args;
  keyboard_grouping_t grouping;
  if (streq("none", mode)) {
    grouping = KEYBOARD_GROUP_NONE;
  } else if (streq("smart", mode)) {
    grouping = KEYBOARD_GROUP_SMART;
  } else if (streq("default", mode)) {
    grouping = KEYBOARD_GROUP_DEFAULT;
  } else {
    send_failure(client_fd, "keyboard_grouping: invalid mode (use none, smart, default)\n");
    return;
  }

  set_keyboard_grouping(grouping);
  keyboard_reapply_grouping();
  send_success(client_fd, "keyboard_grouping set\n");
}

static void ipc_cmd_scroller(char **args, int num, int client_fd) {
  if (num < 1) {
    send_failure(client_fd, "scroller: missing arguments\n");
    return;
  }

  output_t *mon = server.focused_output;
  if (!mon || !mon->desk) {
    send_failure(client_fd, "no desktop\n");
    return;
  }

  desktop_t *desk = mon->desk;

  if (streq("proportion", *args)) {
    if (num < 2) {
      send_failure(client_fd, "scroller proportion: missing value\n");
      return;
    }

    if (!desk->focus || !desk->focus->client) {
      send_failure(client_fd, "scroller proportion: no focused client\n");
      return;
    }

    float value = atof(args[1]);
    if (value < 0.1f || value > 1.0f) {
      send_failure(client_fd, "scroller proportion: value must be between 0.1 and 1.0\n");
      return;
    }

    client_t *head = scroller_get_stack_head(desk->focus->client);
    head->scroller_proportion = value;
    arrange(mon, desk, true);
    send_success(client_fd, "proportion set\n");

  } else if (streq("stack", *args)) {
    if (!desk->focus || !desk->focus->client) {
      send_failure(client_fd, "scroller stack: no focused client\n");
      return;
    }

    // Find another client to stack with (for now, just stack with previous leaf)
    // TODO: something better?
    node_t *target = prev_leaf(desk->focus, desk->root);
    if (!target || !target->client) {
      send_failure(client_fd, "scroller stack: no target to stack with\n");
      return;
    }

    client_t *head = scroller_get_stack_head(target->client);
    scroller_stack_push(head, desk->focus->client);
    arrange(mon, desk, true);
    send_success(client_fd, "stacked\n");

  } else if (streq("unstack", *args)) {
    if (!desk->focus || !desk->focus->client) {
      send_failure(client_fd, "scroller unstack: no focused client\n");
      return;
    }

    scroller_stack_remove(desk->focus->client);
    arrange(mon, desk, true);
    send_success(client_fd, "unstacked\n");

  } else if (streq("resize", *args)) {
    if (num < 2) {
      send_failure(client_fd, "scroller resize: missing delta\n");
      return;
    }

    if (!desk->focus || !desk->focus->client) {
      send_failure(client_fd, "scroller resize: no focused client\n");
      return;
    }

    float delta = atof(args[1]);
    scroller_resize_width(desk->focus->client, delta);
    arrange(mon, desk, true);
    send_success(client_fd, "resized\n");

  } else if (streq("set_proportion", *args)) {
    if (num < 2) {
      send_failure(client_fd, "scroller set_proportion: missing value\n");
      return;
    }

    if (!desk->focus || !desk->focus->client) {
      send_failure(client_fd, "scroller set_proportion: no focused client\n");
      return;
    }

    float value = atof(args[1]);
    scroller_set_proportion(desk->focus->client, value);
    arrange(mon, desk, true);
    send_success(client_fd, "proportion set\n");

  } else if (streq("cycle_preset", *args)) {
    if (!desk->focus || !desk->focus->client) {
      send_failure(client_fd, "scroller cycle_preset: no focused client\n");
      return;
    }

    if (scroller_proportion_preset_count == 0) {
      send_failure(client_fd, "scroller cycle_preset: no presets configured\n");
      return;
    }

    scroller_cycle_proportion_preset(desk->focus->client);
    arrange(mon, desk, true);
    send_success(client_fd, "cycled to next preset\n");

  } else if (streq("center", *args)) {
    if (!desk->focus || !desk->focus->client) {
      send_failure(client_fd, "scroller center: no focused client\n");
      return;
    }

    scroller_center_window(desk, desk->focus->client);
    send_success(client_fd, "window centered\n");

  } else {
    send_failure(client_fd, "scroller: unknown subcommand\n");
  }
}

static void ipc_cmd_hotkey(char **args, int num, int client_fd) {
  if (num < 1) {
    send_failure(client_fd, "hotkey: usage: list | <modifiers+key> <command> [args...]\n");
    return;
  }

  if (streq("list", args[0])) {
    char buf[DOORS_BUFSIZ];
    int offset = 0;
    for (size_t i = 0; i < num_keybinds; i++) {
      keybind_t *kb = &keybinds[i];

      char mods[64] = {0};
      if (kb->modifiers & WLR_MODIFIER_LOGO) strcat(mods, "super+");
      if (kb->modifiers & WLR_MODIFIER_ALT) strcat(mods, "alt+");
      if (kb->modifiers & WLR_MODIFIER_CTRL) strcat(mods, "ctrl+");
      if (kb->modifiers & WLR_MODIFIER_SHIFT) strcat(mods, "shift+");
      if (mods[0]) mods[strlen(mods) - 1] = '\0';

      char key_name[64] = {0};
      if (kb->use_keycode)
        snprintf(key_name, sizeof(key_name), "keycode_%u", kb->keycode);
      else
        xkb_keysym_get_name(kb->keysym, key_name, sizeof(key_name));

      const char *action_label;
      char action_buf[MAXLEN * 2 + 32];
      if (kb->action == BIND_EXTERNAL)
        action_label = kb->external_cmd;
      else
        action_label = bind_action_name(kb->action);

      if (kb->submap_name[0])
        snprintf(action_buf, sizeof(action_buf), "%s [submap: %s]", action_label, kb->submap_name);
      else
        snprintf(action_buf, sizeof(action_buf), "%s", action_label);

      offset += snprintf(buf + offset, sizeof(buf) - offset, "%zu\t%s\t%s\t%s\n",
        i, mods[0] ? mods : "(none)", key_name, action_buf);
    }
    offset += snprintf(buf + offset, sizeof(buf) - offset, "total: %zu keybinds\n", num_keybinds);
    send_success(client_fd, buf);
    return;
  }

  if (num < 2) {
    send_failure(client_fd, "hotkey: usage: list | <modifiers+key> <command> [args...]\n");
    return;
  }

  char hotkey_str[MAXLEN];
  snprintf(hotkey_str, sizeof(hotkey_str), "%s", args[0]);

  uint32_t modifiers = 0;
  xkb_keysym_t keysym = XKB_KEY_NoSymbol;
  uint32_t keycode = 0;
  bool use_keycode = false;

  char *plus = strrchr(hotkey_str, '+');
  if (plus) {
    *plus = '\0';
    modifiers = parse_modifiers(hotkey_str);
    char *key_part = plus + 1;
    while (*key_part == ' ') key_part++;
    keysym = parse_keysym(key_part);
    keycode = parse_keycode(key_part);
    if (keycode > 0)
      use_keycode = true;
  } else {
    keysym = parse_keysym(args[0]);
    keycode = parse_keycode(args[0]);
    if (keycode > 0)
      use_keycode = true;
  }

  if (keysym == XKB_KEY_NoSymbol && keycode == 0) {
    send_failure(client_fd, "hotkey: unknown keysym\n");
    return;
  }

  char cmd[MAXLEN] = {0};
  int offset = 0;
  for (int i = 1; i < num; i++) {
    if (i > 1)
      offset += snprintf(cmd + offset, sizeof(cmd) - offset, " ");
    offset += snprintf(cmd + offset, sizeof(cmd) - offset, "%s", args[i]);
  }

  int desktop_index = 0;
  char submap_name[MAXLEN];
  submap_name[0] = '\0';
  bind_action_t action = parse_action(cmd, &desktop_index, submap_name);

  add_keybind(modifiers, keysym, keycode, use_keycode, action, desktop_index,
    action == BIND_EXTERNAL ? cmd : NULL,
    submap_name[0] ? submap_name : NULL);

  send_success(client_fd, "hotkey added\n");
}

static void ipc_cmd_scratchpad(char **args, int num, int client_fd) {
  if (num < 1) {
    send_failure(client_fd, "scratchpad: missing arguments\n");
    return;
  }

  if (streq("show", *args)) {
    if (num >= 2) {
      node_t *n = NULL;
      if (strncmp(args[1], "app_id:", 7) == 0)
        n = scratchpad_find_by_app_id(args[1] + 7);
      else if (strncmp(args[1], "title:", 6) == 0)
        n = scratchpad_find_by_title(args[1] + 6);

      if (n) {
        scratchpad_show(n);
        transaction_commit_dirty();
        send_success(client_fd, "scratchpad shown\n");
        return;
      }
      send_failure(client_fd, "scratchpad show: matching entry not found\n");
      return;
    }

    scratchpad_toggle_auto();
    transaction_commit_dirty();
    send_success(client_fd, "scratchpad toggled\n");
    return;
  } else {
    send_failure(client_fd, "scratchpad: unknown subcommand (use show)\n");
  }
}

bool ipc_cmd_subscribe(char **args, int num, int client_fd);

bool process_ipc_message(char *msg, int msg_len, int client_fd) {
  wlr_log(WLR_DEBUG, "IPC: processing message: %.*s", msg_len, msg);
  bool owns_client_fd = false;
  int cap = 16;
  int num = 0;
  char **args = calloc(cap, sizeof(char *));

  if (!args) {
    send_failure(client_fd, "memory error\n");
    return false;
  }

  for (int i = 0, j = 0; i < msg_len; i++) {
    if (num >= cap) {
      cap *= 2;
      char **new = realloc(args, cap * sizeof(char *));
      if (!new) {
        free(args);
        send_failure(client_fd, "memory error\n");
        return false;
      }
      args = new;
    }
    if (msg[i] == '\0') {
      args[num++] = msg + j;
      j = i + 1;
    }
  }

  if (num < 1) {
    free(args);
    send_failure(client_fd, "no arguments\n");
    return false;
  }

  char **args_orig = args;

  if (streq("node", *args)) {
    ipc_cmd_node(++args, --num, client_fd);
  } else if (streq("desktop", *args)) {
    ipc_cmd_desktop(++args, --num, client_fd);
  } else if (streq("query", *args)) {
    ipc_cmd_query(++args, --num, client_fd);
  } else if (streq("wm", *args)) {
    ipc_cmd_wm(++args, --num, client_fd);
  } else if (streq("config", *args)) {
    ipc_cmd_config(++args, --num, client_fd);
  } else if (streq("quit", *args)) {
    ipc_cmd_quit(++args, --num, client_fd);
  } else if (streq("subscribe", *args)) {
    owns_client_fd = ipc_cmd_subscribe(++args, --num, client_fd);
  } else if (streq("output", *args)) {
    ipc_cmd_output(++args, --num, client_fd);
  } else if (streq("input", *args)) {
    ipc_cmd_input(++args, --num, client_fd);
  } else if (streq("focus", *args)) {
    ipc_cmd_focus(++args, --num, client_fd);
  } else if (streq("swap", *args)) {
    ipc_cmd_swap(++args, --num, client_fd);
  } else if (streq("presel", *args)) {
    ipc_cmd_presel(++args, --num, client_fd);
  } else if (streq("resize", *args)) {
    ipc_cmd_resize(++args, --num, client_fd);
  } else if (streq("toggle", *args)) {
    ipc_cmd_toggle(++args, --num, client_fd);
  } else if (streq("rotate", *args)) {
    ipc_cmd_rotate(++args, --num, client_fd);
  } else if (streq("flip", *args)) {
    ipc_cmd_flip(++args, --num, client_fd);
  } else if (streq("equalize", *args)) {
    ipc_cmd_equalize(++args, --num, client_fd);
  } else if (streq("balance", *args)) {
    ipc_cmd_balance(++args, --num, client_fd);
   } else if (streq("scratchpad", *args)) {
     ipc_cmd_scratchpad(++args, --num, client_fd);
   } else if (streq("send", *args)) {
     ipc_cmd_send(++args, --num, client_fd);
   } else if (streq("rule", *args)) {
     ipc_cmd_rule(++args, --num, client_fd);
   } else if (streq("keyboard_grouping", *args)) {
     ipc_cmd_keyboard_grouping(++args, --num, client_fd);
   } else if (streq("scroller", *args)) {
     ipc_cmd_scroller(++args, --num, client_fd);
   } else if (streq("hotkey", *args)) {
     ipc_cmd_hotkey(++args, --num, client_fd);
   } else if (streq("bezier", *args)) {
     ++args; --num;
     if (num < 5) {
       send_failure(client_fd, "usage: bezier <name> <p1x> <p1y> <p2x> <p2y>\n");
     } else {
       double p1x = atof(args[1]), p1y = atof(args[2]);
       double p2x = atof(args[3]), p2y = atof(args[4]);
       if (bezier_add(args[0], p1x, p1y, p2x, p2y))
         send_success(client_fd, "bezier curve added\n");
       else
         send_failure(client_fd, "failed to add bezier curve\n");
     }
   } else if (streq("spring", *args)) {
     ++args; --num;
     if (num < 3) {
       send_failure(client_fd, "usage: spring <name> <stiffness> <damping> [mass] [value_eps] [velocity_eps]\n");
     } else {
       double stiffness = atof(args[1]);
       double damping = atof(args[2]);
       double mass = num >= 4 ? atof(args[3]) : 1.0;
       double value_eps = num >= 5 ? atof(args[4]) : SPRING_EPSILON_DEFAULT;
       double velocity_eps = num >= 6 ? atof(args[5]) : SPRING_EPSILON_DEFAULT;
       if (spring_add(args[0], stiffness, damping, mass, value_eps, velocity_eps))
         send_success(client_fd, "spring curve added\n");
       else
         send_failure(client_fd, "failed to add spring curve\n");
     }
   } else {
     send_failure(client_fd, "unknown command\n");
   }

  free(args_orig);
  return owns_client_fd;
}
