#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <calcLib.h>
#include "protocol.h"

// Enable if you want debugging to be printed.
// Alternative, pass CFLAGS=-DDEBUG to make, make CFLAGS=-DDEBUG
#define DEBUG

#ifdef DEBUG
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
#define DBG(...) do {} while (0)
#endif

using namespace std;

/* Arithmetic codes, see protocol.h mapping. */
#define OP_ADD 1
#define OP_SUB 2
#define OP_MUL 3
#define OP_DIV 4

/* Each operation the client performs has to complete within this many seconds. */
#define OP_TIMEOUT 5

/* Set for the child process while it owns a client socket, so the SIGALRM
   handler knows where to send "ERROR TO\n". */
static volatile sig_atomic_t g_client_fd = -1;

static void on_timeout(int sig)
{
  (void)sig;
  if (g_client_fd >= 0) {
    ssize_t w = write(g_client_fd, "ERROR TO\n", 9);
    (void)w;
    close(g_client_fd);
  }
  _exit(EXIT_SUCCESS);
}

/* Split "<host>:<port>" in place. The assignment guarantees an IPv4 dotted
   address or a DNS name, so the last ':' separates host from port. */
static int split_hostport(char *arg, char **host, char **port)
{
  char *colon = strrchr(arg, ':');
  if (!colon || colon == arg || colon[1] == '\0')
    return -1;
  *colon = '\0';
  *host = arg;
  *port = colon + 1;
  return 0;
}

/* Create a listening socket for the given host and port. */
static int make_listen_socket(const char *host, const char *port)
{
  struct addrinfo hints, *res, *rp;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  int rc = getaddrinfo(host, port, &hints, &res);
  if (rc != 0) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
    return -1;
  }

  int fd = -1;
  for (rp = res; rp != NULL; rp = rp->ai_next) {
    fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd < 0)
      continue;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0 && listen(fd, 16) == 0)
      break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);

  if (fd < 0)
    fprintf(stderr, "Failed to bind %s:%s\n", host, port);
  return fd;
}

static int send_all(int fd, const void *buf, size_t len)
{
  const unsigned char *p = (const unsigned char *)buf;
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = send(fd, p + sent, len - sent, 0);
    if (n <= 0)
      return -1;
    sent += (size_t)n;
  }
  return 0;
}

static int recv_all(int fd, void *buf, size_t len)
{
  unsigned char *p = (unsigned char *)buf;
  size_t got = 0;
  while (got < len) {
    ssize_t n = recv(fd, p + got, len - got, 0);
    if (n <= 0)
      return -1;
    got += (size_t)n;
  }
  return 0;
}

/* Read one '\n' terminated line. Reads a byte at a time so we never consume
   bytes that belong to a following binary message. */
static ssize_t recv_line(int fd, char *buf, size_t max)
{
  size_t i = 0;
  while (i + 1 < max) {
    char c;
    ssize_t n = recv(fd, &c, 1, 0);
    if (n <= 0)
      return n;
    buf[i++] = c;
    if (c == '\n')
      break;
  }
  buf[i] = '\0';
  return (ssize_t)i;
}

static const char *arith_name(int a)
{
  switch (a) {
    case OP_ADD: return "add";
    case OP_SUB: return "sub";
    case OP_MUL: return "mul";
    case OP_DIV: return "div";
  }
  return "add";
}

static int32_t compute(int arith, int32_t a, int32_t b)
{
  switch (arith) {
    case OP_ADD: return a + b;
    case OP_SUB: return a - b;
    case OP_MUL: return a * b;
    case OP_DIV: return a / b;
  }
  return 0;
}

/* Build a random integer task. Guarantees a non-zero divisor for division so
   the client never hits a floating point / divide-by-zero exception. */
static void gen_task(int *arith, int32_t *v1, int32_t *v2)
{
  char *t = randomType();
  int a;
  if (strcmp(t, "add") == 0)
    a = OP_ADD;
  else if (strcmp(t, "sub") == 0)
    a = OP_SUB;
  else if (strcmp(t, "mul") == 0)
    a = OP_MUL;
  else
    a = OP_DIV;

  int x = randomInt();
  int y = randomInt();
  if (a == OP_DIV) {
    while (y == 0)
      y = randomInt();
  }
  *arith = a;
  *v1 = x;
  *v2 = y;
}

static void serve_text(int fd)
{
  int arith;
  int32_t v1, v2;
  gen_task(&arith, &v1, &v2);

  char task[128];
  int len = snprintf(task, sizeof(task), "%s %d %d\n", arith_name(arith), v1, v2);
  DBG("[tcp] text task: %s %d %d\n", arith_name(arith), v1, v2);

  alarm(OP_TIMEOUT);
  if (send_all(fd, task, (size_t)len) < 0)
    return;
  alarm(0);

  char line[256];
  alarm(OP_TIMEOUT);
  ssize_t n = recv_line(fd, line, sizeof(line));
  alarm(0);
  if (n <= 0)
    return;

  long answer = strtol(line, NULL, 10);
  int32_t correct = compute(arith, v1, v2);
  const char *reply = (answer == (long)correct) ? "OK\n" : "ERROR\n";
  DBG("[tcp] text answer=%ld correct=%d -> %s", answer, correct, reply);
  send_all(fd, reply, strlen(reply));
}

static void serve_binary(int fd)
{
  int arith;
  int32_t v1, v2;
  gen_task(&arith, &v1, &v2);
  uint32_t id = (uint32_t)rand() ^ ((uint32_t)getpid() << 16);

  struct calcProtocol out;
  memset(&out, 0, sizeof(out));
  out.type = htons(1);              /* server to client */
  out.major_version = htons(1);
  out.minor_version = htons(1);
  out.id = htonl(id);
  out.arith = htonl((uint32_t)arith);
  out.inValue1 = (int32_t)htonl((uint32_t)v1);
  out.inValue2 = (int32_t)htonl((uint32_t)v2);
  out.inResult = 0;
  DBG("[tcp] bin task id=%u arith=%d %d %d\n", id, arith, v1, v2);

  alarm(OP_TIMEOUT);
  if (send_all(fd, &out, sizeof(out)) < 0)
    return;
  alarm(0);

  struct calcProtocol in;
  alarm(OP_TIMEOUT);
  int rc = recv_all(fd, &in, sizeof(in));
  alarm(0);
  if (rc < 0)
    return;

  uint32_t rid = ntohl(in.id);
  int32_t rresult = (int32_t)ntohl((uint32_t)in.inResult);
  int32_t correct = compute(arith, v1, v2);

  struct calcMessage msg;
  memset(&msg, 0, sizeof(msg));
  msg.type = htons(2);             /* server to client, binary */
  msg.protocol = htons(6);         /* TCP */
  msg.major_version = htons(1);
  msg.minor_version = htons(1);
  uint32_t verdict = (rid == id && rresult == correct) ? 1 : 2;
  msg.message = htonl(verdict);
  DBG("[tcp] bin result=%d correct=%d -> %s\n", rresult, correct,
      verdict == 1 ? "OK" : "NOT OK");
  send_all(fd, &msg, sizeof(msg));
}

static void serve(int fd)
{
  g_client_fd = fd;
  signal(SIGALRM, on_timeout);
  initCalcLib_seed((unsigned int)(time(NULL) ^ getpid()));

  const char *greeting = "TEXT TCP 1.1\nBINARY TCP 1.1\n\n";
  alarm(OP_TIMEOUT);
  if (send_all(fd, greeting, strlen(greeting)) < 0)
    return;
  alarm(0);

  char line[256];
  alarm(OP_TIMEOUT);
  ssize_t n = recv_line(fd, line, sizeof(line));
  alarm(0);
  if (n <= 0)
    return;

  if (strncmp(line, "TEXT TCP 1.1", 12) == 0) {
    serve_text(fd);
  } else if (strncmp(line, "BINARY TCP 1.1", 14) == 0) {
    serve_binary(fd);
  } else {
    const char *err = "ERROR\n";
    send_all(fd, err, strlen(err));
  }
}

int main(int argc, char *argv[])
{
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <host>:<port>\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  char *host = NULL, *port = NULL;
  if (split_hostport(argv[1], &host, &port) != 0) {
    fprintf(stderr, "Error: argument must be in <host>:<port> format\n");
    exit(EXIT_FAILURE);
  }

  initCalcLib();
  signal(SIGCHLD, SIG_IGN);   /* auto-reap children, no zombies */
  signal(SIGPIPE, SIG_IGN);

  int listenfd = make_listen_socket(host, port);
  if (listenfd < 0)
    exit(EXIT_FAILURE);

  printf("TCP server on: %s:%s\n", host, port);
  fflush(stdout);

  for (;;) {
    struct sockaddr_storage peer;
    socklen_t plen = sizeof(peer);
    int clientfd = accept(listenfd, (struct sockaddr *)&peer, &plen);
    if (clientfd < 0) {
      if (errno == EINTR)
        continue;
      perror("accept");
      continue;
    }

    pid_t pid = fork();
    if (pid < 0) {
      perror("fork");
      close(clientfd);
      continue;
    }
    if (pid == 0) {
      close(listenfd);
      serve(clientfd);
      close(clientfd);
      _exit(EXIT_SUCCESS);
    }
    close(clientfd);
  }

  close(listenfd);
  return 0;
}
