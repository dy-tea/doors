#pragma once

#include <stdarg.h>

int log_init(const char *log_file);
void log_fini(void);
const char *log_get_path(void);
int log_get_fd(void);
