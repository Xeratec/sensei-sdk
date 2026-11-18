/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/*
 * Host side of the simulated shield link (native_sim): TCP sockets, files. Built into the native simulator runner, so
 * it may use the host C library. Sockets are made non-blocking and close-on-exec: a shield that restarts itself (the
 * process executes itself again) must not keep its port or its connection.
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "sensei_lpwan/lpwan_sim.h"

static int nodelay(int fd) {
  int one = 1;

  return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

int lpwan_sim_listen(const char *bind_addr, int port) {
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
  int one = 1;
  int fd;

  if (inet_pton(AF_INET, bind_addr, &a.sin_addr) != 1) {
    return -1;
  }
  fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return -1;
  }
  (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(fd, 1) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

int lpwan_sim_accept(int listen_fd) {
  int fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);

  if (fd < 0) {
    return errno == EAGAIN || errno == EWOULDBLOCK ? -2 : -1;
  }
  (void)nodelay(fd);
  return fd;
}

int lpwan_sim_connect(const char *host, int port) {
  struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
  struct addrinfo *res = NULL;
  char service[12];
  int fd;

  snprintf(service, sizeof(service), "%d", port);
  if (getaddrinfo(host, service, &hints, &res) != 0 || res == NULL) {
    return -1;
  }
  fd = socket(res->ai_family, res->ai_socktype | SOCK_CLOEXEC, res->ai_protocol);
  if (fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen) != 0) { /* blocking: a refused port answers at once */
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if (fd < 0) {
    return -1;
  }
  (void)nodelay(fd);
  (void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  return fd;
}

int lpwan_sim_read(int fd, void *buf, size_t n) {
  ssize_t r = read(fd, buf, n);

  if (r < 0) {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? -2 : -1;
  }
  return (int)r;
}

int lpwan_sim_write(int fd, const void *buf, size_t n) {
  const char *p = buf;

  while (n > 0) {
    ssize_t w = send(fd, p, n, MSG_NOSIGNAL);

    if (w < 0) {
      struct pollfd pf = {.fd = fd, .events = POLLOUT};

      if ((errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) || poll(&pf, 1, 1000) <= 0) {
        return -1;
      }
      continue;
    }
    p += w;
    n -= (size_t)w;
  }
  return 0;
}

void lpwan_sim_close(int fd) {
  if (fd >= 0) {
    close(fd);
  }
}

int64_t lpwan_sim_utc_ms(void) {
  struct timespec ts;

  (void)clock_gettime(CLOCK_REALTIME, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

long lpwan_sim_file_read(const char *path, void *buf, size_t cap) {
  FILE *f = fopen(path, "rb");
  size_t n;

  if (f == NULL) {
    return -1;
  }
  n = fread(buf, 1, cap, f);
  fclose(f);
  return (long)n;
}

int lpwan_sim_file_write(const char *path, const void *buf, size_t n) {
  char tmp[512];
  FILE *f;
  int ok;

  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  f = fopen(tmp, "wb");
  if (f == NULL) {
    return -1;
  }
  ok = fwrite(buf, 1, n, f) == n;
  ok &= fclose(f) == 0;
  return ok && rename(tmp, path) == 0 ? 0 : -1;
}
