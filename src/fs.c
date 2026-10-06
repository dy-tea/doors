#include "fs.h"
#include <errno.h>
#include <linux/limits.h>
#include <stdio.h>

static bool ensure_dir(const char *path, mode_t mode) {
	if (mkdir(path, mode) == 0 || errno == EEXIST)
		return true;
	return false;
}

// mkdir -p, only the last component is created with mode
bool mkdir_p(const char *path, mode_t mode) {
	if (path == NULL || path[0] != '/')
		return false;

	char buf[PATH_MAX];
	int len = snprintf(buf, sizeof(buf), "%s", path);
	if (len < 0 || (size_t)len >= sizeof(buf))
		return false;

	// ignore a trailing separator so the loop below never sees "//"
	if (len > 1 && buf[len - 1] == '/')
		buf[--len] = '\0';

	for (char *p = buf + 1; *p != '\0'; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (!ensure_dir(buf, 0700))
			return false;
		*p = '/';
	}

	return ensure_dir(buf, mode);
}
