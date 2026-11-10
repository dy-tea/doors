#include "fs.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PATH_MAX 4096

static void fs_err(char *err, size_t errsz, const char *fmt, ...) {
	if (err == NULL || errsz == 0)
		return;

	va_list ap;
	va_start(ap, fmt);
	vsnprintf(err, errsz, fmt, ap);
	va_end(ap);

	size_t len = strlen(err);
	if (len + 1 < errsz) {
		err[len] = '\n';
		err[len + 1] = '\0';
	}
}

char *fs_read_text(const char *path, char *err, size_t errsz) {
	if (path == NULL || path[0] == '\0') {
		fs_err(err, errsz, "missing path");
		return NULL;
	}

	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd == -1) {
		fs_err(err, errsz, "cannot open \"%s\": %s", path, strerror(errno));
		return NULL;
	}

	struct stat info;
	if (fstat(fd,
			&info) == -1 || !S_ISREG(info.st_mode) || info.st_size <= 0 || info.st_size > FS_READ_MAX) {
		fs_err(err, errsz, "\"%s\" is not a regular file of a sane size", path);
		close(fd);
		return NULL;
	}

	char *text = malloc((size_t)info.st_size + 1);
	if (text == NULL) {
		fs_err(err, errsz, "out of memory");
		close(fd);
		return NULL;
	}

	size_t len = 0;
	while (len < (size_t)info.st_size) {
		ssize_t n = read(fd, text + len, (size_t)info.st_size - len);
		if (n == -1 && errno == EINTR)
			continue;
		if (n <= 0) {
			fs_err(err, errsz, "cannot read \"%s\"", path);
			free(text);
			close(fd);
			return NULL;
		}
		len += (size_t)n;
	}
	close(fd);

	text[len] = '\0';
	return text;
}

static bool ensure_dir(const char *path, mode_t mode) {
	if (mkdir(path, mode) == 0 || errno == EEXIST)
		return true;
	return false;
}

// mkdir -p, only the last component is created with mode
bool mkdir_p(const char *path, const mode_t mode) {
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
