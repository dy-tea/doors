#include "ipc.h"
#include "ipc_cmd.h"
#include "ipc_helpers.h"
#include "output.h"
#include "server.h"
#include "toplevel.h"
#include "tree.h"
#include "workspace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ipc_cmd_query(char **args, int num, int client_fd) {
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
