#pragma once

#include <sys/stat.h>
#include <sys/types.h>
#include <stdbool.h>

bool mkdir_p(const char *path, mode_t mode);
