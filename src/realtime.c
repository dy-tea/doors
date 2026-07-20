#include "realtime.h"

#include <pthread.h>
#include <sched.h>
#include <wlr/util/log.h>

static void child_fork_callback(void) {
	struct sched_param param = {.sched_priority = 0};
	pthread_setschedparam(pthread_self(), SCHED_OTHER, &param);
}

void set_rr_scheduling(void) {
	int prio = sched_get_priority_min(SCHED_RR);
	struct sched_param param = {.sched_priority = prio};
	if (pthread_setschedparam(pthread_self(), SCHED_RR, &param) != 0) {
		wlr_log(WLR_INFO, "Failed to set real-time scheduling to %d", prio);
		return;
	}
	pthread_atfork(NULL, NULL, child_fork_callback);
	wlr_log(WLR_DEBUG, "Real-time scheduling enabled (SCHED_RR, priority=%d)", prio);
}
