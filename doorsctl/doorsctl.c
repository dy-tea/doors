#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define DOORS_SOCKET_ENV "DOORS_SOCKET"
#define DOORS_BUFSIZ 4096

static void err(const char *msg) {
	fprintf(stderr, "%s", msg);
	exit(EXIT_FAILURE);
}

static void doors_socket_not_set_err(void) {
	fprintf(stderr, "Error: DOORS_SOCKET environment variable not set.\n");
	fprintf(stderr, "This variable should be set by doors when it starts.\n");
	err("Make sure you are running doors and executing doorsctl from the same session.\n");
}

static char *read_reply(int sock_fd, size_t *out_len) {
	size_t cap = DOORS_BUFSIZ, len = 0;
	char *buf = malloc(cap);

	if (!buf)
		err("Failed to allocate memory.\n");

	for (; ; ) {
		// keep room for the terminator
		if (len + 1 >= cap) {
			char *grown = realloc(buf, cap * 2);

			if (!grown) {
				free(buf);
				err("Failed to allocate memory.\n");
			}

			buf = grown;
			cap *= 2;
		}

		ssize_t n = recv(sock_fd, buf + len, cap - len - 1, 0);

		if (n > 0) {
			len += (size_t)n;

			if (len > 1 && buf[1] == '/')
				break;

			continue;
		}

		if (n < 0 && errno == EINTR)
			continue;

		break;
	}

	buf[len] = '\0';
	*out_len = len;
	return buf;
}

static int open_socket(bool quiet) {
	int sock_fd;
	struct sockaddr_un sock_address;

	sock_address.sun_family = AF_UNIX;

	char *sp = getenv(DOORS_SOCKET_ENV);
	if (sp == NULL) {
		if (!quiet)
			doors_socket_not_set_err();
		return -1;
	}

	snprintf(sock_address.sun_path, sizeof(sock_address.sun_path), "%s", sp);

	sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock_fd == -1) {
		if (!quiet)
			err("Failed to create the socket.\n");
		return -1;
	}

	if (connect(sock_fd, (struct sockaddr *)&sock_address, sizeof(sock_address)) == -1) {
		close(sock_fd);
		if (!quiet) {
			fprintf(stderr, "Error: Failed to connect to socket at %s\n", sock_address.sun_path);
			err("Is doors running?\n");
		}
		return -1;
	}

	return sock_fd;
}

static bool send_args(int sock_fd, char **argv, int argc) {
	char msg[DOORS_BUFSIZ];
	int msg_len = 0;

	for (int offset = 0, rem = sizeof(msg), n = 0; argc > 0 && rem > 0; offset += n, rem -= n, argc--,
			argv++) {
		n = snprintf(msg + offset, rem, "%s%c", *argv, 0);
		msg_len += n;
	}

	if (send(sock_fd, msg, msg_len, 0) == -1)
		return false;

	return true;
}

static int run_command(int sock_fd, char **argv, int argc) {
	if (!send_args(sock_fd, argv, argc))
		err("Failed to send the data.\n");

	int ret = EXIT_SUCCESS;
	struct pollfd fds[] = {
		{sock_fd, POLLIN | POLLHUP, 0},
	};

	while (poll(fds, 1, -1) > 0) {
		if (fds[0].revents & (POLLHUP | POLLIN)) {
			size_t rsp_len;
			char *rsp = read_reply(sock_fd, &rsp_len);

			if (rsp_len == 0) {
				free(rsp);
				break;
			}

			if (rsp[0] == '\x01') {
				ret = EXIT_FAILURE;
				fprintf(stderr, "%s", rsp + 1);
				fflush(stderr);
			} else {
				fprintf(stdout, "%s", rsp + 1);
				fflush(stdout);
				if (rsp[1] == '/') {
					size_t len = strlen(rsp + 1);
					if (len > 0 && rsp[1 + len - 1] == '\n')
						rsp[1 + len - 1] = '\0';
					int fifo_fd = open(rsp + 1, O_RDONLY);
					if (fifo_fd < 0) {
						free(rsp);
						close(sock_fd);
						err("subscribe: failed to open fifo for reading\n");
					}
					free(rsp);
					close(sock_fd);
					char buf[DOORS_BUFSIZ];
					ssize_t nb;
					while ((nb = read(fifo_fd, buf, sizeof(buf))) > 0) {
						fwrite(buf, 1, nb, stdout);
						fflush(stdout);
					}
					close(fifo_fd);
					return ret;
				}
			}

			free(rsp);
			break;
		}
	}

	return ret;
}

static void hoist_dry_run(char **argv, int *argc) {
	int flags = 0, kept = 0;

	for (int i = 0; i < *argc; i++) {
		if (strcmp(argv[i], "--dry-run") == 0)
			flags++;
		else
			argv[kept++] = argv[i];
	}

	if (flags == 0)
		return;

	memmove(argv + flags, argv, (size_t)kept * sizeof(*argv));
	for (int i = 0; i < flags; i++)
		argv[i] = "--dry-run";
}

int main(int argc, char *argv[]) {
	if (argc < 2)
		err("No arguments given.\n");

	char *sp = getenv(DOORS_SOCKET_ENV);
	if (sp == NULL)
		doors_socket_not_set_err();

	if (strcmp(argv[1], "--print-socket-path") == 0) {
		printf("%s\n", sp);
		return EXIT_SUCCESS;
	}

	int sock_fd = open_socket(false);
	if (sock_fd < 0)
		return EXIT_FAILURE;

	argc--;
	argv++;
	hoist_dry_run(argv, &argc);
	int ret = run_command(sock_fd, argv, argc);

	close(sock_fd);
	return ret;
}
