#include "ipc/ipc.h"
#include "str.h"
#include "test.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef DOORS_DOCS_PATH
#define DOORS_DOCS_PATH "docs"
#endif

static size_t exchange(const char *msg, size_t len, char *reply, size_t replysz) {
	int fds[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
		CHECK(false);
		return 0;
	}

	process_ipc_message((char *)msg, (int)len, fds[0]);
	close(fds[0]);

	ssize_t n = recv(fds[1], reply, replysz - 1, 0);
	close(fds[1]);

	if (n < 0)
		n = 0;

	reply[n] = '\0';
	return (size_t)n;
}


#define MAX_LINES 4096
#define MAX_LINE 512

static char cmd_lines[MAX_LINES][MAX_LINE];
static size_t cmd_count;
static char doc_lines[MAX_LINES][MAX_LINE];
static size_t doc_count;

static void trim(char *s) {
	size_t len = strlen(s);
	while (len > 0 && isspace((unsigned char)s[len - 1]))
		s[--len] = '\0';
	size_t start = 0;
	while (start < len && isspace((unsigned char)s[start]))
		start++;
	if (start > 0)
		memmove(s, s + start, len - start + 1);
}

static void collapse(char *s) {
	char *out = s;
	int prev_space = 1;
	for (char *p = s; *p; p++) {
		if (isspace((unsigned char)*p)) {
			if (prev_space)
				continue;
			prev_space = 1;
			*out++ = ' ';
		} else {
			prev_space = 0;
			*out++ = *p;
		}
	}
	*out = '\0';
	if (out > s && *(out - 1) == ' ')
		*(out - 1) = '\0';
}

static void convert_i(char *s) {
	// convert I<...> to <...> in place
	char *out = s;
	char *p = s;
	while (*p) {
		if (p[0] == 'I' && p[1] == '<') {
			*out++ = '<';
			p += 2;
			while (*p && *p != '>')
				*out++ = *p++;
			if (*p == '>')
				p++;
			*out++ = '>';
		} else {
			*out++ = *p++;
		}
	}
	*out = '\0';
}

static bool has_bad_angle(const char *s) {
	for (const char *p = s; *p; p++) {
		if (*p != '<')
			continue;
		// allow C<, I<, L< prefixes
		if (p > s && (p[-1] == 'C' || p[-1] == 'I' || p[-1] == 'L'))
			continue;
		if (p > s && p[-1] == ' ')
			return true;
		// bare <
		return true;
	}
	return false;
}

int main(void) {
	char reply[1 << 20];
	size_t n = exchange("commands\0", 9, reply, sizeof(reply));
	CHECK(n > 1);
	CHECK(reply[0] == '\0');

	const char *text = reply + 1;
	char buf[1 << 20];
	str_copy(buf, sizeof(buf), text);

	char *line = strtok(buf, "\n");
	while (line != NULL) {
		char tmp[MAX_LINE];
		str_copy(tmp, sizeof(tmp), line);
		trim(tmp);
		if (strstr(tmp, "doorsctl ") != NULL) {
			collapse(tmp);
			if (cmd_count < MAX_LINES)
				str_copy(cmd_lines[cmd_count++], MAX_LINE, tmp);
		}
		line = strtok(NULL, "\n");
	}

	char path[4096];
	snprintf(path, sizeof(path), "%s/doorsctl.pod", DOORS_DOCS_PATH);
	FILE *f = fopen(path, "r");
	CHECK(f != NULL);
	if (f) {
		char filebuf[1 << 20];
		size_t flen = fread(filebuf, 1, sizeof(filebuf) - 1, f);
		filebuf[flen] = '\0';
		fclose(f);

		// parse multi-line C<doorsctl ...> usage blocks
		char *p = filebuf;
		while ((p = strstr(p, "C<doorsctl")) != NULL) {
			char tmp[MAX_LINE];
			char *start = p;
			char *end = strchr(start, '>');
			char *scan = start;
			char *last_gt = NULL;
			while (*scan) {
				if (*scan == '>') {
					last_gt = scan;
					// stop if next non-space is newline newline or EOF
					char *q = scan + 1;
					while (*q == ' ' || *q == '\t')
						q++;
					if (*q == '\n' || *q == '\0') {
						end = scan;
						break;
					}
				}
				scan++;
			}
			if (!end) {
				end = last_gt;
			}
			if (!end)
				break;

			size_t len = (size_t)(end - start + 1);
			if (len >= sizeof(tmp))
				len = sizeof(tmp) - 1;
			memcpy(tmp, start, len);
			tmp[len] = '\0';

			// remove the wrapping C< and trailing >
			char *inner = tmp + 2;
			// skip C<
			if (inner[0] == 'd') {
				// strip trailing >
				char *gt = strrchr(inner, '>');
				if (gt)
					*gt = '\0';
				collapse(inner);
				convert_i(inner);

				// validate formatting
				CHECK(!has_bad_angle(start));
				for (char *q = inner; *q; q++) {
					if (*q == ' ' && (q > inner && q[-1] == '|'))
						CHECK(false);
					if (*q == ' ' && q[1] == '|')
						CHECK(false);
				}

				if (doc_count < MAX_LINES)
					str_copy(doc_lines[doc_count++], MAX_LINE, inner);
			}

			p = end + 1;
		}
	}

	// every commands usage line must appear in the docs
	for (size_t i = 0; i < cmd_count; i++) {
		bool found = false;
		for (size_t j = 0; j < doc_count; j++) {
			if (strcmp(cmd_lines[i], doc_lines[j]) == 0) {
				found = true;
				break;
			}
		}
		if (!found) {
			fprintf(stderr, "commands usage not found in docs: %s\n", cmd_lines[i]);
			CHECK(false);
		}
	}

	// every command name in commands output must appear in docs
	{
		char seen[256][32];
		size_t nseen = 0;
		for (size_t i = 0; i < cmd_count; i++) {
			char tok[32];
			sscanf(cmd_lines[i], "%31s", tok);
			bool have = false;
			for (size_t k = 0; k < nseen; k++) {
				if (strcmp(seen[k], tok) == 0) {
					have = true;
					break;
				}
			}
			if (!have && nseen < 256)
				str_copy(seen[nseen++], 32, tok);
		}
		for (size_t k = 0; k < nseen; k++) {
			bool found = false;
			for (size_t j = 0; j < doc_count; j++) {
				char tok[32];
				sscanf(doc_lines[j], "%31s", tok);
				if (strcmp(tok, seen[k]) == 0) {
					found = true;
					break;
				}
			}
			if (!found) {
				fprintf(stderr, "command missing from docs: %s\n", seen[k]);
				CHECK(false);
			}
		}
	}

	if (test_failures) {
		fprintf(stderr, "%d of %d checks failed\n", test_failures, test_checks);
		return 1;
	}
	printf("%d checks passed\n", test_checks);
	return 0;
}
