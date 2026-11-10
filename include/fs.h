#pragma once

#include <sys/stat.h>
#include <sys/types.h>
#include <stdbool.h>
#include <stddef.h>

bool mkdir_p(const char *path, mode_t mode);

// largest file fs_read_text() will load
#define FS_READ_MAX (1024 * 1024)

// read whole file into a heap buffer that is NUL terminated
char *fs_read_text(const char *path, char *err, size_t errsz);
