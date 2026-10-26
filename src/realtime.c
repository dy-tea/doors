#define _GNU_SOURCE

#include "realtime.h"
#include <pthread.h>
#include <sched.h>
#include <wlr/util/log.h>

#ifndef SCHED_RESET_ON_FORK
static void child_fork_callback(void) {
	struct sched_param param = {
		.sched_priority = 0
	};
	pthread_setschedparam(pthread_self(), SCHED_OTHER, &param);
}

#endif

void set_rr_scheduling(void) {
	int prio = sched_get_priority_min(SCHED_RR);
	struct sched_param param = {
		.sched_priority = prio
	};
#ifdef SCHED_RESET_ON_FORK
	int sched_policy = SCHED_RR | SCHED_RESET_ON_FORK;
#else
	int sched_policy = SCHED_RR;
#endif
	if (sched_setscheduler(0, sched_policy, &param) != 0) {
		wlr_log(WLR_INFO, "Failed to set real-time scheduling to %d", prio);
		return;
	}
#ifndef SCHED_RESET_ON_FORK
	// without kernel support, demote kids back to SCHED_OTHER
	pthread_atfork(NULL, NULL, child_fork_callback);
#endif
	wlr_log(WLR_DEBUG, "Real-time scheduling enabled (SCHED_RR, priority=%d)", prio);
}
