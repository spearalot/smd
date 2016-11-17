#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/ip.h>

#define QUIT   0
#define QUEUE  1
#define LIST   2
#define NEXT   3
#define CLEAR  4
#define STATUS 5

const char *commands[] = {
  "quit", "queue", "list", "next", "clear", "status", NULL
};

static char socket_buf[1024];

static int server_recv(int fd, char **payload, int *len)
{
  int l;

  l = recv(fd, &socket_buf, 1024, 0);
  if (l <= 0)
    return -1;

  *len = socket_buf[1] << 8 | socket_buf[2];
  if (*len > l || *len >= 1020)
    return -1;

  socket_buf[*len + 3] = '\0';
  *payload = socket_buf + 3;

  return socket_buf[0];
}

static int server_send(int fd, char type, char *payload, int len)
{
  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_port = htons(1025);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  socket_buf[0] = type;
  socket_buf[1] = (char) ((len >> 8) & 0xFF00);
  socket_buf[2] = (char) (len & 0xFF);

  if (len)
    strncpy(socket_buf + 3, payload, len);

  return sendto(fd, socket_buf, len + 3, 0, (struct sockaddr *) &addr, sizeof(struct sockaddr_in));
}

static int parse_command(char *cmd)
{
  int i;

  for (i = 0; commands[i]; ++i)
    if (strcmp(cmd, commands[i]) == 0)
      return i;

  return -1;
}

int main(int argc, char **argv)
{
  char *payload;
  int len;

  if (argc < 1)
    return EXIT_FAILURE;

  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    perror("socket");
    exit(EXIT_FAILURE);
  }

  switch (parse_command(argv[1])) {
  case QUIT:
    server_send(fd, QUIT, NULL, 0);
    break;

  case QUEUE:
    if (argc >= 3)
      server_send(fd, QUEUE, argv[2], strlen(argv[2]));
    break;

  case LIST:
    if (argc >= 3)
      server_send(fd, LIST, argv[2], strlen(argv[2]));
    else
      server_send(fd, LIST, NULL, 0);
    break;

  case NEXT:
    server_send(fd, NEXT, NULL, 0);
    break;

  case CLEAR:
    server_send(fd, CLEAR, NULL, 0);
    break;

  case STATUS:
    server_send(fd, STATUS, NULL, 0);
    if (server_recv(fd, &payload, &len) == 0) {
      printf("Status: %s\n", payload);
    }
    break;

  default:
    break;
  }

  close(fd);
  return 0;
}
