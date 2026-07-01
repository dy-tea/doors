#include "ipc.h"
#include "ipc_cmd.h"
#include "ipc_helpers.h"
#include "rule.h"
#include <stdlib.h>
#include <string.h>

void ipc_cmd_rule(char **args, int num, int client_fd) {
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

      if (strncmp(arg, "title=", 6) == 0) {
        title = arg + 6;
        strncpy(r->match.title, title, MAXLEN - 1);
        r->match.title[MAXLEN - 1] = '\0';
      } else if (strncmp(arg, "tag=", 4) == 0) {
        strncpy(r->match.tag, arg + 4, MAXLEN - 1);
        r->match.tag[MAXLEN - 1] = '\0';
      } else if (strncmp(arg, "app_id=", 7) == 0) {
        app_id = arg + 7;
        strncpy(r->match.app_id, app_id, MAXLEN - 1);
        r->match.app_id[MAXLEN - 1] = '\0';
      } else if (arg[0] != '-' && app_id == NULL && strchr(arg, '=') == NULL) {
        app_id = arg;
        strncpy(r->match.app_id, app_id, MAXLEN - 1);
        r->match.app_id[MAXLEN - 1] = '\0';
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
      } else if (streq("shadow=on", arg)) {
        r->consequence.shadow = true;
        r->consequence.has_shadow = true;
      } else if (streq("shadow=off", arg)) {
        r->consequence.shadow = false;
        r->consequence.has_shadow = true;
      } else if (streq("block_out_from_screenshare=on", arg)) {
        r->consequence.block_out_from_screenshare = true;
        r->consequence.has_block_out_from_screenshare = true;
      } else if (streq("block_out_from_screenshare=off", arg)) {
        r->consequence.block_out_from_screenshare = false;
        r->consequence.has_block_out_from_screenshare = true;
      } else if (streq("allow_tearing=on", arg)) {
        r->consequence.allow_tearing = true;
        r->consequence.has_allow_tearing = true;
      } else if (streq("allow_tearing=off", arg)) {
        r->consequence.allow_tearing = false;
        r->consequence.has_allow_tearing = true;
      } else if (streq("shortcuts_inhibitor=on", arg)) {
        r->consequence.shortcuts_inhibitor = true;
        r->consequence.has_shortcuts_inhibitor = true;
      } else if (streq("shortcuts_inhibitor=off", arg)) {
        r->consequence.shortcuts_inhibitor = false;
        r->consequence.has_shortcuts_inhibitor = true;
      }

      args++;
      num--;
    }

    if (!app_id && !title && r->match.tag[0] == '\0') {
      free(r);
      send_failure(client_fd, "rule -a: must specify app_id, title, or tag\n");
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
