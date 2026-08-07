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
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <malloc.h>
#include <poll.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/filio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <switch.h>

#include "paths.h"
#include "prefs.h"
#include "libc_shim.h"
#include "net_shim.h"

static int net_up = 0;
static int nifm_up = 0;
static int ca_ready = 0;
static int cached_available = -1;
static u64 availability_tick = 0;
static Mutex availability_lock;
static char user_id[17] = "0000000000000000";

static int file_nonempty(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && st.st_size > 0;
}

static int write_pem_certificate(FILE *f, const unsigned char *der, size_t len) {
  static const char b64[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  if (fputs("-----BEGIN CERTIFICATE-----\n", f) < 0)
    return -1;
  unsigned column = 0;
  for (size_t i = 0; i < len; i += 3) {
    const size_t left = len - i;
    const uint32_t v = (uint32_t)der[i] << 16 |
                       (left > 1 ? (uint32_t)der[i + 1] << 8 : 0) |
                       (left > 2 ? der[i + 2] : 0);
    char out[4] = {
      b64[(v >> 18) & 63], b64[(v >> 12) & 63],
      left > 1 ? b64[(v >> 6) & 63] : '=',
      left > 2 ? b64[v & 63] : '=',
    };
    if (fwrite(out, 1, sizeof(out), f) != sizeof(out))
      return -1;
    column += 4;
    if (column == 64) {
      if (fputc('\n', f) == EOF)
        return -1;
      column = 0;
    }
  }
  if (column && fputc('\n', f) == EOF)
    return -1;
  return fputs("-----END CERTIFICATE-----\n", f) < 0 ? -1 : 0;
}

// The Android OpenSSL bundled in Geometry Dash has a build-machine CA path.
// Export Horizon's maintained public trust store to a normal PEM file so the
// original client can verify Boomlings without shipping a soon-stale bundle.
static int export_firmware_ca_bundle(void) {
  const int had_bundle = file_nonempty(path_ca_bundle());
  Result rc = sslInitialize(1);
  if (R_FAILED(rc))
    return had_bundle;

  u32 ids[1] = { (u32)SslCaCertificateId_All };
  u32 size = 0, total = 0;
  rc = sslGetCertificateBufSize(ids, 1, &size);
  if (R_FAILED(rc) || size < sizeof(SslBuiltInCertificateInfo)) {
    sslExit();
    return had_bundle;
  }

  const size_t alloc_size = (size + 0xfff) & ~(size_t)0xfff;
  void *buffer = memalign(0x1000, alloc_size);
  if (!buffer) {
    sslExit();
    return had_bundle;
  }
  memset(buffer, 0, alloc_size);
  rc = sslGetCertificates(buffer, size, ids, 1, &total);
  if (R_FAILED(rc)) {
    free(buffer);
    sslExit();
    return had_bundle;
  }

  char tmp[384], old[384];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path_ca_bundle());
  snprintf(old, sizeof(old), "%s.old", path_ca_bundle());
  remove(tmp);
  FILE *f = fopen(tmp, "wb");
  unsigned written = 0;
  if (f) {
    SslBuiltInCertificateInfo *certs = buffer;
    for (u32 i = 0; i < total; i++) {
      // IDs below 1000 are Nintendo-private roots. The web PKI starts at 1000.
      if (certs[i].cert_id < 1000 ||
          certs[i].status != SslTrustedCertStatus_EnabledTrusted ||
          !certs[i].cert_data || certs[i].cert_size == 0)
        continue;
      if (write_pem_certificate(f, certs[i].cert_data,
                                (size_t)certs[i].cert_size) != 0)
        break;
      written++;
    }
    if (fflush(f) != 0 || ferror(f))
      written = 0;
    if (fclose(f) != 0)
      written = 0;
  }
  free(buffer);
  sslExit();

  if (!written) {
    remove(tmp);
    return had_bundle;
  }

  remove(old);
  const int moved_old = rename(path_ca_bundle(), old) == 0;
  if (rename(tmp, path_ca_bundle()) != 0) {
    if (moved_old)
      rename(old, path_ca_bundle());
    remove(tmp);
    return file_nonempty(path_ca_bundle());
  }
  remove(old);
  return 1;
}

static int valid_user_id(const char *id) {
  if (!id || strlen(id) != 16)
    return 0;
  int any_nonzero = 0;
  for (unsigned i = 0; i < 16; i++) {
    if (!isxdigit((unsigned char)id[i]))
      return 0;
    any_nonzero |= id[i] != '0';
  }
  return any_nonzero;
}

static void init_user_id(void) {
  const char *saved = prefs_get_string("__gdash_nx_network_id", "");
  if (valid_user_id(saved)) {
    snprintf(user_id, sizeof(user_id), "%s", saved);
    return;
  }
  unsigned char random[8];
  randomGet(random, sizeof(random));
  static const char hex[] = "0123456789abcdef";
  for (unsigned i = 0; i < sizeof(random); i++) {
    user_id[i * 2] = hex[random[i] >> 4];
    user_id[i * 2 + 1] = hex[random[i] & 15];
  }
  user_id[16] = '\0';
  if (!valid_user_id(user_id))
    snprintf(user_id, sizeof(user_id), "6e78506f72743031"); // "nxPort01"
  prefs_set_string("__gdash_nx_network_id", user_id);
}

int net_init(void) {
  init_user_id();
  ca_ready = export_firmware_ca_bundle();
  Result rc = socketInitializeDefault();
  if (R_FAILED(rc))
    return 0;
  net_up = 1;

  rc = nifmInitialize(NifmServiceType_User);
  if (R_SUCCEEDED(rc))
    nifm_up = 1;
  return 1;
}

void net_exit(void) {
  if (nifm_up) {
    nifmExit();
    nifm_up = 0;
  }
  if (net_up) {
    socketExit();
    net_up = 0;
  }
}

int net_is_available(void) {
  if (!net_up)
    return 0;
  if (!nifm_up)
    return 1;

  mutexLock(&availability_lock);
  const u64 now = armGetSystemTick();
  const u64 freq = armGetSystemTickFreq();
  if (cached_available < 0 || !availability_tick || now - availability_tick >= freq) {
    NifmInternetConnectionType type = 0;
    NifmInternetConnectionStatus status = 0;
    u32 strength = 0;
    Result rc = nifmGetInternetConnectionStatus(&type, &strength, &status);
    cached_available = R_FAILED(rc) || status == NifmInternetConnectionStatus_Connected;
    availability_tick = now;
  }
  const int available = cached_available;
  mutexUnlock(&availability_lock);
  return available;
}

int net_tls_ca_ready(void) {
  return ca_ready;
}

const char *net_user_id(void) {
  return user_id;
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

// The game is compiled against bionic's Linux errno numbers while libnx uses
// newlib/FreeBSD numbers for most socket errors. Curl checks these values
// directly, especially SO_ERROR after a non-blocking connect.
int net_errno_to_linux(int value) {
  switch (value) {
    case EAFNOSUPPORT:   return 97;
    case EPROTOTYPE:     return 91;
    case ENOTSOCK:       return 88;
    case ENOPROTOOPT:    return 92;
#ifdef ESHUTDOWN
    case ESHUTDOWN:      return 108;
#endif
    case EADDRINUSE:     return 98;
    case ECONNABORTED:   return 103;
    case ENETUNREACH:    return 101;
    case ENETDOWN:       return 100;
    case ETIMEDOUT:      return 110;
    case EHOSTDOWN:      return 112;
    case EHOSTUNREACH:   return 113;
    case EINPROGRESS:    return 115;
    case EALREADY:       return 114;
    case EDESTADDRREQ:   return 89;
    case EMSGSIZE:       return 90;
    case EPROTONOSUPPORT:return 93;
#ifdef ESOCKTNOSUPPORT
    case ESOCKTNOSUPPORT:return 94;
#endif
    case EADDRNOTAVAIL:  return 99;
    case ENETRESET:      return 102;
    case EISCONN:        return 106;
    case ENOTCONN:       return 107;
    case ETOOMANYREFS:   return 109;
    default:             return value; // common/POSIX values already match
  }
}

static void fixup_socket_errno(void) {
  errno = net_errno_to_linux(errno);
}

static int socket_result(int result) {
  if (result < 0)
    fixup_socket_errno();
  return result;
}

static long socket_result_long(long result) {
  if (result < 0)
    fixup_socket_errno();
  return result;
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
  if (fd < 0)
    fixup_socket_errno();
  return fd;
}

int connect_fake(int fd, const void *addr, unsigned addrlen) {
  if (!net_up) { errno = EBADF; return -1; }
  struct sockaddr_storage tmp;
  int r = connect(fd, sa_to_bsd(addr, addrlen, &tmp), addrlen);
  if (r < 0)
    fixup_socket_errno();
  return r;
}

int bind_fake(int fd, const void *addr, unsigned addrlen) {
  if (!net_up) { errno = EBADF; return -1; }
  struct sockaddr_storage tmp;
  return socket_result(bind(fd, sa_to_bsd(addr, addrlen, &tmp), addrlen));
}

int accept_fake(int fd, void *addr, unsigned *addrlen) {
  if (!net_up) { errno = EBADF; return -1; }
  int r = socket_result(accept(fd, (struct sockaddr *)addr, (socklen_t *)addrlen));
  if (r >= 0 && addr && addrlen)
    sa_to_linux_inplace(addr, *addrlen);
  return r;
}

int listen_fake(int fd, int backlog) {
  if (!net_up) { errno = EBADF; return -1; }
  return socket_result(listen(fd, backlog));
}

int shutdown_fake(int fd, int how) {
  if (!net_up) { errno = EBADF; return -1; }
  return socket_result(shutdown(fd, how));
}

long send_fake(int fd, const void *buf, size_t len, int flags) {
  if (!net_up) { errno = EBADF; return -1; }
  return socket_result_long(send(fd, buf, len, msg_flags_to_bsd(flags)));
}

long recv_fake(int fd, void *buf, size_t len, int flags) {
  if (!net_up) { errno = EBADF; return -1; }
  return socket_result_long(recv(fd, buf, len, msg_flags_to_bsd(flags)));
}

long sendto_fake(int fd, const void *buf, size_t len, int flags, const void *addr, unsigned addrlen) {
  if (!net_up) { errno = EBADF; return -1; }
  struct sockaddr_storage tmp;
  return socket_result_long(sendto(fd, buf, len, msg_flags_to_bsd(flags),
                           addr ? sa_to_bsd(addr, addrlen, &tmp) : NULL,
                           addrlen));
}

long recvfrom_fake(int fd, void *buf, size_t len, int flags, void *addr, unsigned *addrlen) {
  if (!net_up) { errno = EBADF; return -1; }
  long r = socket_result_long(recvfrom(fd, buf, len, msg_flags_to_bsd(flags),
                                      (struct sockaddr *)addr,
                                      (socklen_t *)addrlen));
  if (r >= 0 && addr && addrlen)
    sa_to_linux_inplace(addr, *addrlen);
  return r;
}

int getsockname_fake(int fd, void *addr, unsigned *addrlen) {
  if (!net_up) { errno = EBADF; return -1; }
  int r = socket_result(getsockname(fd, (struct sockaddr *)addr,
                                   (socklen_t *)addrlen));
  if (r == 0 && addr && addrlen)
    sa_to_linux_inplace(addr, *addrlen);
  return r;
}

int getpeername_fake(int fd, void *addr, unsigned *addrlen) {
  if (!net_up) { errno = EBADF; return -1; }
  int r = socket_result(getpeername(fd, (struct sockaddr *)addr,
                                   (socklen_t *)addrlen));
  if (r == 0 && addr && addrlen)
    sa_to_linux_inplace(addr, *addrlen);
  return r;
}

// Linux SOL_SOCKET=1, option numbers differ from BSD across the board
#define LINUX_SOL_SOCKET 1

static int sockopt_to_bsd(int level, int optname, int *out_level, int *out_name) {
  if (level == IPPROTO_TCP) { // 6 == 6
    *out_level = IPPROTO_TCP;
    switch (optname) {
      case 1: *out_name = TCP_NODELAY;  return 0;
      case 4: *out_name = TCP_KEEPIDLE; return 0;
      case 5: *out_name = TCP_KEEPINTVL;return 0;
      case 6: *out_name = TCP_KEEPCNT;  return 0;
      default: return -1;
    }
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
  int r = getsockopt(fd, l, n, optval, (socklen_t *)optlen);
  if (r < 0) {
    fixup_socket_errno();
    return r;
  }
  if (l == SOL_SOCKET && n == SO_ERROR && optval && optlen &&
      *optlen >= sizeof(int)) {
    int *error = optval;
    *error = net_errno_to_linux(*error);
  }
  return r;
}

int setsockopt_fake(int fd, int level, int optname, const void *optval, unsigned optlen) {
  if (!net_up) { errno = EBADF; return -1; }
  int l, n;
  if (sockopt_to_bsd(level, optname, &l, &n) != 0)
    return 0; // ignore unmapped options
  return socket_result(setsockopt(fd, l, n, optval, optlen));
}

// struct addrinfo and AI_* values match Android's BSD-derived bionic ABI; only
// socket type flags, address families and the embedded sockaddr need changes.
int getaddrinfo_fake(const char *node, const char *service, const void *hints, void **res) {
  if (!net_up)
    return EAI_FAIL;
  struct addrinfo h, *bsd_hints = NULL;
  if (hints) {
    memcpy(&h, hints, sizeof(h));
    h.ai_flags &= AI_MASK;
    h.ai_family = family_to_bsd((unsigned short)h.ai_family);
    h.ai_socktype &= LINUX_SOCK_TYPE_MASK;
    h.ai_addrlen = 0;
    h.ai_canonname = NULL;
    h.ai_addr = NULL;
    h.ai_next = NULL;
    bsd_hints = &h;
  }
  int r = getaddrinfo(node, service, bsd_hints, (struct addrinfo **)res);
  if (r == EAI_SYSTEM)
    fixup_socket_errno();
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
  return getnameinfo(sa_to_bsd(sa, salen, &tmp), salen, host, hostlen, serv,
                     servlen, flags);
}

const char *inet_ntop_fake(int af, const void *src, char *dst, unsigned size) {
  return inet_ntop(family_to_bsd((unsigned short)af), src, dst, size);
}

int inet_pton_fake(int af, const char *src, void *dst) {
  int r = inet_pton(family_to_bsd((unsigned short)af), src, dst);
  if (r < 0)
    fixup_socket_errno();
  return r;
}

// struct pollfd and the POLLIN/OUT/ERR/HUP/NVAL bits match between ABIs
int poll_fake(void *fds, unsigned nfds, int timeout) {
  if (!net_up) { errno = EINVAL; return -1; }
  struct pollfd *pollfds = fds;
  int result;
  if (nfds == 0) {
    // POSIX uses poll(NULL, 0, timeout) as a millisecond sleep. The BSD
    // service rejects the null array with EFAULT, so perform that sleep here.
    if (timeout > 0)
      svcSleepThread((s64)timeout * 1000000ll);
    result = 0;
  } else if (nfds == 1 && pollfds && is_urandom_fd_fake(pollfds[0].fd)) {
    // OpenSSL polls /dev/urandom once before its first entropy read. Its fd is
    // virtual in this port, so forwarding it to BSD produces EBADF and aborts
    // TLS before certificate setup. Horizon's randomGet is always readable.
    pollfds[0].revents = pollfds[0].events & (POLLIN | POLLRDNORM);
    result = pollfds[0].revents ? 1 : 0;
  } else {
    result = socket_result(poll(pollfds, nfds, timeout));
  }
  return result;
}

int select_fake(int nfds, void *rd, void *wr, void *ex, void *tv) {
  if (!net_up) { errno = EINVAL; return -1; }
  return socket_result(select(nfds, (fd_set *)rd, (fd_set *)wr,
                              (fd_set *)ex, (struct timeval *)tv));
}

// fcntl: F_GETFL/F_SETFL match (3/4); O_NONBLOCK is 0x800 on bionic vs
// newlib's 0x4000. F_SETFD/F_GETFD are absorbed.
#define LINUX_O_NONBLOCK 0x800

int fcntl_fake(int fd, int cmd, ...) {
  long arg = 0;
  if (cmd == F_SETFL || cmd == 2) {
    va_list va;
    va_start(va, cmd);
    arg = va_arg(va, long);
    va_end(va);
  }
  switch (cmd) {
    case F_GETFL: {
      int fl = socket_result(fcntl(fd, F_GETFL, 0));
      if (fl < 0) return fl;
      int out = fl & 3;
      if (fl & O_NONBLOCK) out |= LINUX_O_NONBLOCK;
      return out;
    }
    case F_SETFL: {
      int fl = 0;
      if (arg & LINUX_O_NONBLOCK) fl |= O_NONBLOCK;
      return socket_result(fcntl(fd, F_SETFL, fl));
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
      return socket_result(ioctl(fd, FIONBIO, arg));
    case LINUX_FIONREAD:
      return socket_result(ioctl(fd, FIONREAD, arg));
    default:
      return 0;
  }
}

int pipe_fake(int fds[2]) {
  (void)fds;
  errno = 38; // bionic/Linux ENOSYS (newlib uses 88)
  return -1;
}

int socketpair_fake(int domain, int type, int protocol, int sv[2]) {
  (void)domain; (void)type; (void)protocol; (void)sv;
  errno = 38;
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
  return gai_strerror(ecode);
}

int dup2_fake(int oldfd, int newfd) {
  (void)oldfd; (void)newfd;
  errno = 38;
  return -1;
}
