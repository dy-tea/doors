#pragma once

#include <stdbool.h>

void idle_power_init(void);
void idle_power_fini(void);
void idle_power_notify_activity(void);
void idle_power_check_inhibitors(void);
void idle_power_reset_timer(void);
