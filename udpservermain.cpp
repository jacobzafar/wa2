#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
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

#define OP_ADD 1
#define OP_SUB 2
#define OP_MUL 3
#define OP_DIV 4

/* A client that has been handed a task but has not answered is dropped after
   this many seconds; a later answer is then rejected. */
#define TASK_TIMEOUT 10

#define MAX_CLIENTS 1024

/* "TEXT UDP 1.1" with an optional trailing newline. */
#define TEXT_HANDSHAKE "TEXT UDP 1.1"

struct client_entry {
  int in_use;
  struct sockaddr_storage addr;
  socklen_t addrlen;
  int is_binary;
  uint32_t id;          /* binary only */
  int arith;
  int32_t v1, v2;
  int32_t expected;
  time_t created;
};

static struct client_entry g_clients[MAX_CLIENTS];

static int addr_equal(const struct sockaddr_storage *a, socklen_t alen,
                      const struct sockaddr_storage *b, socklen_t blen)
{
  if (a->ss_family != b->ss_family)
    return 0;
  if (a->ss_family == AF_INET) {
    const struct sockaddr_in *x = (const struct sockaddr_in *)a;
    const struct sockaddr_in *y = (const struct sockaddr_in *)b;
    return x->sin_port == y->sin_port &&
           memcmp(&x->sin_addr, &y->sin_addr, sizeof(x->sin_addr)) == 0;
  }
  if (a->ss_family == AF_INET6) {
    const struct sockaddr_in6 *x = (const struct sockaddr_in6 *)a;
    const struct sockaddr_in6 *y = (const struct sockaddr_in6 *)b;
    return x->sin6_port == y->sin6_port &&
           memcmp(&x->sin6_addr, &y->sin6_addr, sizeof(x->sin6_addr)) == 0;
  }
  return alen == blen && memcmp(a, b, alen) == 0;
}

static struct client_entry *find_client(const struct sockaddr_storage *addr,
                                        socklen_t addrlen)
{
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (g_clients[i].in_use &&
        addr_equal(&g_clients[i].addr, g_clients[i].addrlen, addr, addrlen))
      return &g_clients[i];
  }
  return NULL;
}

static struct client_entry *alloc_client(const struct sockaddr_storage *addr,
                                         socklen_t addrlen)
{
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (!g_clients[i].in_use) {
      memset(&g_clients[i], 0, sizeof(g_clients[i]));
      g_clients[i].in_use = 1;
      memcpy(&g_clients[i].addr, addr, addrlen);
      g_clients[i].addrlen = addrlen;
      g_clients[i].created = time(NULL);
      return &g_clients[i];
    }
  }
  return NULL;
}

static void free_client(struct client_entry *c)
{
  if (c)
    c->in_use = 0;
}

static void sweep_clients(time_t now)
{
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (g_clients[i].in_use && now - g_clients[i].created >= TASK_TIMEOUT) {
      DBG("[udp] client timed out, removed\n");
      g_clients[i].in_use = 0;
    }
  }
}

static int next_timeout_ms(time_t now)
{
  int best = -1;
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (!g_clients[i].in_use)
      continue;
    int remain = (int)(g_clients[i].created + TASK_TIMEOUT - now);
    if (remain < 0)
      remain = 0;
    if (best < 0 || remain * 1000 < best)
      best = remain * 1000;
  }
  return best; /* -1 => block until a datagram arrives */
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

static void send_text(int sock, const struct sockaddr_storage *addr,
                      socklen_t addrlen, const char *s)
{
  sendto(sock, s, strlen(s), 0, (const struct sockaddr *)addr, addrlen);
}

static void send_calcmessage(int sock, const struct sockaddr_storage *addr,
                             socklen_t addrlen, uint32_t message)
{
  struct calcMessage m;
  memset(&m, 0, sizeof(m));
  m.type = htons(2);          /* server to client, binary */
  m.message = htonl(message); /* 1 = OK, 2 = NOT OK */
  m.protocol = htons(17);     /* UDP */
  m.major_version = htons(1);
  m.minor_version = htons(1);
  sendto(sock, &m, sizeof(m), 0, (const struct sockaddr *)addr, addrlen);
}

static int looks_numeric(const char *s, ssize_t n)
{
  ssize_t i = 0;
  while (i < n && isspace((unsigned char)s[i]))
    i++;
  if (i < n && (s[i] == '-' || s[i] == '+'))
    i++;
  int digits = 0;
  while (i < n && isdigit((unsigned char)s[i])) {
    i++;
    digits++;
  }
  return digits > 0;
}

/* Hand a fresh task to a newly seen client. */
static void start_client(int sock, const struct sockaddr_storage *addr,
                         socklen_t addrlen, int is_binary)
{
  struct client_entry *c = alloc_client(addr, addrlen);
  if (!c) {
    fprintf(stderr, "client table full\n");
    return;
  }
  gen_task(&c->arith, &c->v1, &c->v2);
  c->expected = compute(c->arith, c->v1, c->v2);
  c->is_binary = is_binary;

  if (is_binary) {
    c->id = (uint32_t)rand() ^ ((uint32_t)time(NULL) << 8);
    struct calcProtocol p;
    memset(&p, 0, sizeof(p));
    p.type = htons(1);
    p.major_version = htons(1);
    p.minor_version = htons(1);
    p.id = htonl(c->id);
    p.arith = htonl((uint32_t)c->arith);
    p.inValue1 = (int32_t)htonl((uint32_t)c->v1);
    p.inValue2 = (int32_t)htonl((uint32_t)c->v2);
    p.inResult = 0;
    sendto(sock, &p, sizeof(p), 0, (const struct sockaddr *)addr, addrlen);
    DBG("[udp] bin task id=%u arith=%d %d %d\n", c->id, c->arith, c->v1, c->v2);
  } else {
    char task[128];
    int len = snprintf(task, sizeof(task), "%s %d %d\n",
                       arith_name(c->arith), c->v1, c->v2);
    sendto(sock, task, (size_t)len, 0, (const struct sockaddr *)addr, addrlen);
    DBG("[udp] text task: %s %d %d\n", arith_name(c->arith), c->v1, c->v2);
  }
}

static void handle_second_message(int sock, struct client_entry *c,
                                  const unsigned char *buf, ssize_t n,
                                  const struct sockaddr_storage *addr,
                                  socklen_t addrlen)
{
  if (c->is_binary) {
    if (n != (ssize_t)sizeof(struct calcProtocol))
      return; /* malformed, let it retry until timeout */
    struct calcProtocol in;
    memcpy(&in, buf, sizeof(in));
    uint32_t rid = ntohl(in.id);
    int32_t rresult = (int32_t)ntohl((uint32_t)in.inResult);
    uint32_t verdict = (rid == c->id && rresult == c->expected) ? 1 : 2;
    DBG("[udp] bin answer=%d expected=%d -> %s\n", rresult, c->expected,
        verdict == 1 ? "OK" : "NOT OK");
    send_calcmessage(sock, addr, addrlen, verdict);
    free_client(c);
  } else {
    if (!looks_numeric((const char *)buf, n))
      return; /* not an answer, let it retry until timeout */
    long answer = strtol((const char *)buf, NULL, 10);
    const char *reply = (answer == (long)c->expected) ? "OK\n" : "ERROR\n";
    DBG("[udp] text answer=%ld expected=%d -> %s", answer, c->expected, reply);
    send_text(sock, addr, addrlen, reply);
    free_client(c);
  }
}

static void handle_first_message(int sock, const unsigned char *buf, ssize_t n,
                                 const struct sockaddr_storage *addr,
                                 socklen_t addrlen)
{
  /* A binary message starts with a 16-bit type in network byte order, so its
     first byte is always 0x00. A text message starts with a printable char. */
  int printable = (n > 0 && buf[0] >= 0x20 && buf[0] < 0x7f);

  if (!printable && n == (ssize_t)sizeof(struct calcMessage)) {
    struct calcMessage m;
    memcpy(&m, buf, sizeof(m));
    uint16_t type = ntohs(m.type);
    uint16_t major = ntohs(m.major_version);
    uint16_t minor = ntohs(m.minor_version);
    if (type < 21 || type > 23 || major != 1 || minor != 1) {
      DBG("[udp] bad binary handshake type=%u v=%u.%u\n", type, major, minor);
      send_calcmessage(sock, addr, addrlen, 2);
      return;
    }
    start_client(sock, addr, addrlen, 1);
    return;
  }

  if (printable && n <= 32 &&
      strncmp((const char *)buf, TEXT_HANDSHAKE, strlen(TEXT_HANDSHAKE)) == 0) {
    start_client(sock, addr, addrlen, 0);
    return;
  }

  /* Anything else: an unknown or stale datagram (for instance an answer that
     arrives after its task was dropped). Reject it explicitly. */
  if (!printable && n == (ssize_t)sizeof(struct calcProtocol)) {
    send_calcmessage(sock, addr, addrlen, 2);
  } else if (printable && looks_numeric((const char *)buf, n)) {
    send_text(sock, addr, addrlen, "ERROR\n");
  }
  DBG("[udp] datagram rejected (size=%zd)\n", n);
}

static int make_udp_socket(const char *host, const char *port)
{
  struct addrinfo hints, *res, *rp;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
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
    if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0)
      break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);

  if (fd < 0)
    fprintf(stderr, "Failed to bind %s:%s\n", host, port);
  return fd;
}

int main(int argc, char *argv[])
{
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <host>:<port>\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  char *arg = argv[1];
  char *colon = strrchr(arg, ':');
  if (!colon || colon == arg || colon[1] == '\0') {
    fprintf(stderr, "Error: argument must be in <host>:<port> format\n");
    exit(EXIT_FAILURE);
  }
  *colon = '\0';
  char *host = arg;
  char *port = colon + 1;

  initCalcLib();

  int sock = make_udp_socket(host, port);
  if (sock < 0)
    exit(EXIT_FAILURE);

  printf("UDP server on: %s:%s\n", host, port);
  fflush(stdout);

  for (;;) {
    struct pollfd pfd;
    pfd.fd = sock;
    pfd.events = POLLIN;

    int pr = poll(&pfd, 1, next_timeout_ms(time(NULL)));
    if (pr < 0) {
      if (errno == EINTR)
        continue;
      perror("poll");
      break;
    }

    if (pr > 0 && (pfd.revents & POLLIN)) {
      unsigned char buf[512];
      struct sockaddr_storage src;
      socklen_t srclen = sizeof(src);
      ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                           (struct sockaddr *)&src, &srclen);
      if (n < 0) {
        if (errno != EINTR)
          perror("recvfrom");
        continue;
      }
      buf[n] = '\0';

      struct client_entry *c = find_client(&src, srclen);
      if (c)
        handle_second_message(sock, c, buf, n, &src, srclen);
      else
        handle_first_message(sock, buf, n, &src, srclen);
    }

    sweep_clients(time(NULL));
  }

  close(sock);
  return 0;
}
