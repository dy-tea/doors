#include "bezier.h"
#include "ipc.h"
#include "ipc_cmd.h"
#include "ipc_helpers.h"
#include "spring.h"
#include <stdlib.h>
#include <wlr/util/log.h>

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
    } else if (streq("seat", *args)) {
     ipc_cmd_seat(++args, --num, client_fd);
    } else if (streq("scroller", *args)) {
     ipc_cmd_scroller(++args, --num, client_fd);
    } else if (streq("master_stack", *args)) {
     ipc_cmd_master_stack(++args, --num, client_fd);
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
