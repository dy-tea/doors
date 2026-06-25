#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>

#define DOORS_SOCKET_ENV "DOORS_SOCKET"
#define DOORS_BUFSIZ 4096

static void err(const char *msg) {
  fprintf(stderr, "%s", msg);
  exit(EXIT_FAILURE);
}

int main(int argc, char *argv[]) {
  int sock_fd;
  struct sockaddr_un sock_address;
  char msg[DOORS_BUFSIZ], rsp[DOORS_BUFSIZ];

  if (argc < 2)
    err("No arguments given.\n");

  sock_address.sun_family = AF_UNIX;

  char *sp = getenv(DOORS_SOCKET_ENV);
  if (sp == NULL) {
    fprintf(stderr, "Error: DOORS_SOCKET environment variable not set.\n");
    fprintf(stderr, "This variable should be set by doors when it starts.\n");
    err("Make sure you are running doors and executing doorsctl from the same session.\n");
  }

  snprintf(sock_address.sun_path, sizeof(sock_address.sun_path), "%s", sp);

  if (strcmp(argv[1], "--print-socket-path") == 0) {
    printf("%s\n", sock_address.sun_path);
    return EXIT_SUCCESS;
  }

  sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock_fd == -1)
    err("Failed to create the socket.\n");

  if (connect(sock_fd, (struct sockaddr *)&sock_address, sizeof(sock_address)) == -1) {
    close(sock_fd);
    fprintf(stderr, "Error: Failed to connect to socket at %s\n", sock_address.sun_path);
    err("Is doors running?\n");
  }

  argc--;
  argv++;
  int msg_len = 0;

  for (int offset = 0, rem = sizeof(msg), n = 0; argc > 0 && rem > 0; offset += n, rem -= n, argc--, argv++) {
    n = snprintf(msg + offset, rem, "%s%c", *argv, 0);
    msg_len += n;
  }

  if (send(sock_fd, msg, msg_len, 0) == -1) {
    close(sock_fd);
    err("Failed to send the data.\n");
  }

  int ret = EXIT_SUCCESS, nb;

  struct pollfd fds[] = {
    {sock_fd, POLLIN | POLLHUP, 0},
  };

  while (poll(fds, 1, -1) > 0) {
    if (fds[0].revents & (POLLHUP | POLLIN)) {
      if ((nb = recv(sock_fd, rsp, sizeof(rsp) - 1, 0)) > 0) {
        rsp[nb] = '\0';
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
              close(sock_fd);
              err("subscribe: failed to open fifo for reading\n");
            }
            close(sock_fd);
            char buf[DOORS_BUFSIZ];
            while ((nb = read(fifo_fd, buf, sizeof(buf))) > 0) {
              fwrite(buf, 1, nb, stdout);
              fflush(stdout);
            }
            close(fifo_fd);
            return ret;
          }
        }
      }
      break;
    }
  }

  close(sock_fd);
  return ret;
}
