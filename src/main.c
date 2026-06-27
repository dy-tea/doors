#include "config.h"
#include "log.h"
#include "server.h"
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <wlr/util/log.h>

struct server_t server = {0};

static void usage(const char *argv0) {
  printf("Usage: %s [-c <config-dir>]\n", argv0);
  printf("  -c, --config <dir>  Use specified config directory instead of default\n");
}

static void child_fork_callback(void) {
  struct sched_param param = {.sched_priority = 0};
  pthread_setschedparam(pthread_self(), SCHED_OTHER, &param);
}

static void set_rr_scheduling(void) {
  int prio = sched_get_priority_min(SCHED_RR);
  struct sched_param param = {.sched_priority = prio};
  if (pthread_setschedparam(pthread_self(), SCHED_RR, &param) != 0) {
    wlr_log(WLR_INFO, "Failed to set real-time scheduling to %d", prio);
    return;
  }
  pthread_atfork(NULL, NULL, child_fork_callback);
  wlr_log(WLR_DEBUG, "Real-time scheduling enabled (SCHED_RR, priority=%d)", prio);
}

int main(int argc, char *argv[]) {
  const char *config_dir = NULL;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "Error: %s requires an argument\n", argv[i]);
        usage(argv[0]);
        return 1;
      }
      config_dir = argv[++i];
    } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      usage(argv[0]);
      return 0;
    } else {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      usage(argv[0]);
      return 1;
    }
  }

  if (log_init(NULL) != 0) {
    fprintf(stderr, "Error: Failed to initialize logging\n");
    return 1;
  }

  if (log_setup_signals() != 0) {
    fprintf(stderr, "Error: Failed to setup signal handlers\n");
    return 1;
  }

  config_init_with_config_dir(config_dir);
  server_init();
  set_rr_scheduling();
  int ret = server_run();
  server_fini();
  log_fini();
  return ret;
}
