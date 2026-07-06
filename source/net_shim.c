/* net_shim.c -- bionic(Linux)<->libnx(BSD) socket ABI conversion
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/filio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <switch.h>

#include "net_shim.h"

static int net_up = 0;

void net_init(void) {
  if (R_SUCCEEDED(socketInitializeDefault()))
    net_up = 1;
}

// ---------------------------------------------------------------------------
// sockaddr conversion: Linux {u16 family, ...} <-> BSD {u8 len, u8 family, ...}
// Payload past the first two bytes is identical for AF_INET/AF_INET6.
// ---------------------------------------------------------------------------

#define LINUX_AF_UNSPEC 0
#define LINUX_AF_UNIX   1
#define LINUX_AF_INET   2
#define LINUX_AF_INET6  10

static unsigned char family_to_bsd(unsigned short fam) {
  switch (fam) {
    case LINUX_AF_INET:  return AF_INET;
    case LINUX_AF_INET6: return AF_INET6;
    case LINUX_AF_UNIX:  return AF_UNIX;
    default:             return (unsigned char)fam;
  }
}

static unsigned short family_to_linux(unsigned char fam) {
  switch (fam) {
    case AF_INET:  return LINUX_AF_INET;
    case AF_INET6: return LINUX_AF_INET6;
    case AF_UNIX:  return LINUX_AF_UNIX;
    default:       return fam;
  }
}

// game sockaddr (linux) -> temp BSD copy
static struct sockaddr *sa_to_bsd(const void *in, unsigned len, struct sockaddr_storage *tmp) {
  if (!in || len < 2 || len > sizeof(*tmp))
    return (struct sockaddr *)in;
  memcpy(tmp, in, len);
  const unsigned short fam = *(const unsigned short *)in;
  ((struct sockaddr *)tmp)->sa_len = (unsigned char)len;
  ((struct sockaddr *)tmp)->sa_family = family_to_bsd(fam);
  return (struct sockaddr *)tmp;
}

// BSD sockaddr -> game (linux) layout, in place
static void sa_to_linux_inplace(void *sa, unsigned len) {
  if (!sa || len < 2)
    return;
  const unsigned char fam = ((struct sockaddr *)sa)->sa_family;
  *(unsigned short *)sa = family_to_linux(fam);
}

// ---------------------------------------------------------------------------
// msg flags: OOB/PEEK/DONTROUTE match; DONTWAIT is 0x40 on Linux but 0x80 on
// BSD (and Linux 0x40 == BSD MSG_WAITALL, which would BLOCK a non-blocking
// read); WAITALL is 0x100 -> 0x40. Everything else (NOSIGNAL &c.) is dropped.
// ---------------------------------------------------------------------------

static int msg_flags_to_bsd(int flags) {
  int out = flags & (MSG_OOB | MSG_PEEK | MSG_DONTROUTE); // 1|2|4 match
  if (flags & 0x40)  out |= MSG_DONTWAIT; // Linux MSG_DONTWAIT
  if (flags & 0x100) out |= MSG_WAITALL;  // Linux MSG_WAITALL
  return out;
}

// errno translation for curl's non-blocking connect state machine: the game
// is compiled against bionic's (Linux) errno values, newlib's differ for the
// in-progress family.
static void fixup_connect_errno(void) {
  switch (errno) {
    case EINPROGRESS: errno = 115; break; // Linux EINPROGRESS
    case EALREADY:    errno = 114; break; // Linux EALREADY
    case EISCONN:     errno = 106; break; // Linux EISCONN
    default: break;
  }
}

// ---------------------------------------------------------------------------
// entry points
// ---------------------------------------------------------------------------

#define LINUX_SOCK_TYPE_MASK 0xf // strip SOCK_NONBLOCK/SOCK_CLOEXEC

int socket_fake(int domain, int type, int protocol) {
  if (!net_up) { errno = EACCES; return -1; }
  const int nonblock = type & 0x800; // Linux SOCK_NONBLOCK
  int fd = socket(family_to_bsd((unsigned short)domain), type & LINUX_SOCK_TYPE_MASK, protocol);
  if (fd >= 0 && nonblock) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
  return fd;
}

int connect_fake(int fd, const void *addr, unsigned addrlen) {
  if (!net_up) { errno = EBADF; return -1; }
  struct sockaddr_storage tmp;
  int r = connect(fd, sa_to_bsd(addr, addrlen, &tmp), addrlen);
  if (r < 0)
    fixup_connect_errno();
  return r;
}

int bind_fake(int fd, const void *addr, unsigned addrlen) {
  if (!net_up) { errno = EBADF; return -1; }
  struct sockaddr_storage tmp;
  return bind(fd, sa_to_bsd(addr, addrlen, &tmp), addrlen);
}

int accept_fake(int fd, void *addr, unsigned *addrlen) {
  if (!net_up) { errno = EBADF; return -1; }
  int r = accept(fd, (struct sockaddr *)addr, (socklen_t *)addrlen);
  if (r >= 0 && addr && addrlen)
    sa_to_linux_inplace(addr, *addrlen);
  return r;
}

int listen_fake(int fd, int backlog) {
  if (!net_up) { errno = EBADF; return -1; }
  return listen(fd, backlog);
}

int shutdown_fake(int fd, int how) {
  if (!net_up) { errno = EBADF; return -1; }
  return shutdown(fd, how);
}

long send_fake(int fd, const void *buf, size_t len, int flags) {
  if (!net_up) { errno = EBADF; return -1; }
  return send(fd, buf, len, msg_flags_to_bsd(flags));
}

long recv_fake(int fd, void *buf, size_t len, int flags) {
  if (!net_up) { errno = EBADF; return -1; }
  return recv(fd, buf, len, msg_flags_to_bsd(flags));
}

long sendto_fake(int fd, const void *buf, size_t len, int flags, const void *addr, unsigned addrlen) {
  if (!net_up) { errno = EBADF; return -1; }
  struct sockaddr_storage tmp;
  return sendto(fd, buf, len, msg_flags_to_bsd(flags),
                addr ? sa_to_bsd(addr, addrlen, &tmp) : NULL, addrlen);
}

long recvfrom_fake(int fd, void *buf, size_t len, int flags, void *addr, unsigned *addrlen) {
  if (!net_up) { errno = EBADF; return -1; }
  long r = recvfrom(fd, buf, len, msg_flags_to_bsd(flags), (struct sockaddr *)addr, (socklen_t *)addrlen);
  if (r >= 0 && addr && addrlen)
    sa_to_linux_inplace(addr, *addrlen);
  return r;
}

int getsockname_fake(int fd, void *addr, unsigned *addrlen) {
  if (!net_up) { errno = EBADF; return -1; }
  int r = getsockname(fd, (struct sockaddr *)addr, (socklen_t *)addrlen);
  if (r == 0 && addr && addrlen)
    sa_to_linux_inplace(addr, *addrlen);
  return r;
}

int getpeername_fake(int fd, void *addr, unsigned *addrlen) {
  if (!net_up) { errno = EBADF; return -1; }
  int r = getpeername(fd, (struct sockaddr *)addr, (socklen_t *)addrlen);
  if (r == 0 && addr && addrlen)
    sa_to_linux_inplace(addr, *addrlen);
  return r;
}

// Linux SOL_SOCKET=1, option numbers differ from BSD across the board
#define LINUX_SOL_SOCKET 1

static int sockopt_to_bsd(int level, int optname, int *out_level, int *out_name) {
  if (level == IPPROTO_TCP) { // 6 == 6
    *out_level = IPPROTO_TCP;
    *out_name = optname; // TCP_NODELAY 1 == 1
    return 0;
  }
  if (level != LINUX_SOL_SOCKET)
    return -1;
  *out_level = SOL_SOCKET;
  switch (optname) {
    case 2:  *out_name = SO_REUSEADDR; return 0;
    case 4:  *out_name = SO_ERROR;     return 0;
    case 6:  *out_name = SO_BROADCAST; return 0;
    case 7:  *out_name = SO_SNDBUF;    return 0;
    case 8:  *out_name = SO_RCVBUF;    return 0;
    case 9:  *out_name = SO_KEEPALIVE; return 0;
    case 10: *out_name = SO_OOBINLINE; return 0;
    case 13: *out_name = SO_LINGER;    return 0;
    case 20: *out_name = SO_RCVTIMEO;  return 0;
    case 21: *out_name = SO_SNDTIMEO;  return 0;
    default: return -1;
  }
}

int getsockopt_fake(int fd, int level, int optname, void *optval, unsigned *optlen) {
  if (!net_up) { errno = EBADF; return -1; }
  int l, n;
  if (sockopt_to_bsd(level, optname, &l, &n) != 0) {
    // unknown option: report "no error/zero" rather than failing the caller
    if (optval && optlen && *optlen >= 4)
      memset(optval, 0, 4);
    return 0;
  }
  return getsockopt(fd, l, n, optval, (socklen_t *)optlen);
}

int setsockopt_fake(int fd, int level, int optname, const void *optval, unsigned optlen) {
  if (!net_up) { errno = EBADF; return -1; }
  int l, n;
  if (sockopt_to_bsd(level, optname, &l, &n) != 0)
    return 0; // ignore unmapped options
  return setsockopt(fd, l, n, optval, optlen);
}

// struct addrinfo field order matches bionic (BSD-derived); only ai_flags
// values and the embedded sockaddrs need attention. curl passes AI_NUMERICHOST
// (Linux 4 == BSD 4) and AI_PASSIVE (1 == 1); AI_ADDRCONFIG (0x20 vs 0x400)
// is dropped -- it is a hint only.
int getaddrinfo_fake(const char *node, const char *service, const void *hints, void **res) {
  if (!net_up)
    return EAI_FAIL;
  struct addrinfo h, *bsd_hints = NULL;
  if (hints) {
    memcpy(&h, hints, sizeof(h));
    h.ai_flags &= (AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST);
    h.ai_family = family_to_bsd((unsigned short)h.ai_family);
    h.ai_socktype &= LINUX_SOCK_TYPE_MASK;
    h.ai_addrlen = 0;
    h.ai_canonname = NULL;
    h.ai_addr = NULL;
    h.ai_next = NULL;
    bsd_hints = &h;
  }
  int r = getaddrinfo(node, service, bsd_hints, (struct addrinfo **)res);
  if (r == 0 && res) {
    for (struct addrinfo *ai = *res; ai; ai = ai->ai_next) {
      ai->ai_family = family_to_linux((unsigned char)ai->ai_family);
      if (ai->ai_addr)
        sa_to_linux_inplace(ai->ai_addr, ai->ai_addrlen);
    }
  }
  return r;
}

void freeaddrinfo_fake(void *res) {
  if (!res)
    return;
  // undo the in-place family rewrite before handing the chain back
  for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
    if (ai->ai_addr) {
      unsigned short fam = *(unsigned short *)ai->ai_addr;
      ((struct sockaddr *)ai->ai_addr)->sa_len = (unsigned char)ai->ai_addrlen;
      ((struct sockaddr *)ai->ai_addr)->sa_family = family_to_bsd(fam);
    }
  }
  freeaddrinfo(res);
}

void *gethostbyname_fake(const char *name) {
  if (!net_up)
    return NULL;
  return gethostbyname(name); // struct hostent layout matches
}

int getnameinfo_fake(const void *sa, unsigned salen, char *host, unsigned hostlen,
                     char *serv, unsigned servlen, int flags) {
  if (!net_up)
    return EAI_FAIL;
  struct sockaddr_storage tmp;
  return getnameinfo(sa_to_bsd(sa, salen, &tmp), salen, host, hostlen, serv, servlen,
                     flags & (NI_NUMERICHOST | NI_NUMERICSERV));
}

// struct pollfd and the POLLIN/OUT/ERR/HUP/NVAL bits match between ABIs
int poll_fake(void *fds, unsigned nfds, int timeout) {
  if (!net_up) { errno = EINVAL; return -1; }
  return poll((struct pollfd *)fds, nfds, timeout);
}

int select_fake(int nfds, void *rd, void *wr, void *ex, void *tv) {
  if (!net_up) { errno = EINVAL; return -1; }
  return select(nfds, (fd_set *)rd, (fd_set *)wr, (fd_set *)ex, (struct timeval *)tv);
}

// fcntl: F_GETFL/F_SETFL match (3/4); O_NONBLOCK is 0x800 on bionic vs
// newlib's 0x4000. F_SETFD/F_GETFD are absorbed.
#define LINUX_O_NONBLOCK 0x800

int fcntl_fake(int fd, int cmd, ...) {
  va_list va;
  va_start(va, cmd);
  long arg = va_arg(va, long);
  va_end(va);
  switch (cmd) {
    case F_GETFL: {
      int fl = fcntl(fd, F_GETFL, 0);
      if (fl < 0) return fl;
      int out = fl & 3;
      if (fl & O_NONBLOCK) out |= LINUX_O_NONBLOCK;
      return out;
    }
    case F_SETFL: {
      int fl = 0;
      if (arg & LINUX_O_NONBLOCK) fl |= O_NONBLOCK;
      return fcntl(fd, F_SETFL, fl);
    }
    case 1: // F_GETFD
      return 0;
    case 2: // F_SETFD (FD_CLOEXEC)
      return 0;
    default:
      return 0;
  }
}

#define LINUX_FIONBIO 0x5421
#define LINUX_FIONREAD 0x541B

int ioctl_fake(int fd, unsigned long request, ...) {
  va_list va;
  va_start(va, request);
  void *arg = va_arg(va, void *);
  va_end(va);
  switch (request) {
    case LINUX_FIONBIO:
      return ioctl(fd, FIONBIO, arg);
    case LINUX_FIONREAD:
      return ioctl(fd, FIONREAD, arg);
    default:
      return 0;
  }
}

int pipe_fake(int fds[2]) {
  (void)fds;
  errno = ENOSYS;
  return -1;
}

int socketpair_fake(int domain, int type, int protocol, int sv[2]) {
  (void)domain; (void)type; (void)protocol; (void)sv;
  errno = ENOSYS;
  return -1;
}

int gethostname_fake(char *name, size_t len) {
  snprintf(name, len, "switch");
  return 0;
}

unsigned if_nametoindex_fake(const char *ifname) {
  (void)ifname;
  return 0;
}

const char *gai_strerror_fake(int ecode) {
  (void)ecode;
  return "getaddrinfo error";
}

int dup2_fake(int oldfd, int newfd) {
  (void)oldfd; (void)newfd;
  errno = ENOSYS;
  return -1;
}
