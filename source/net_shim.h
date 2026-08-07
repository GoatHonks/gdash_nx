/* net_shim.h -- bionic(Linux)<->libnx(BSD) socket ABI conversion
 *
 * The game's statically-linked curl talks the Linux socket ABI: u16 family at
 * sockaddr offset 0, Linux SOL_SOCKET/SO_* numbers, Linux O_NONBLOCK/FIONBIO.
 * libnx's bsd service speaks FreeBSD: u8 len + u8 family, BSD option values.
 * These wrappers convert both ways. struct addrinfo matches bionic (both are
 * BSD-ordered), only the embedded sockaddrs need fixing up in place.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __NET_SHIM_H__
#define __NET_SHIM_H__

#include <stddef.h>

// Call once after paths/config/preferences are initialized. Besides bringing
// up BSD sockets, this exports Horizon's trusted public roots for the game's
// embedded OpenSSL and creates a stable anonymous Android-style device ID.
int net_init(void);
void net_exit(void);
int net_is_available(void);
int net_tls_ca_ready(void);
const char *net_user_id(void);
int net_errno_to_linux(int value);

int socket_fake(int domain, int type, int protocol);
int connect_fake(int fd, const void *addr, unsigned addrlen);
int bind_fake(int fd, const void *addr, unsigned addrlen);
int accept_fake(int fd, void *addr, unsigned *addrlen);
int listen_fake(int fd, int backlog);
int shutdown_fake(int fd, int how);
long send_fake(int fd, const void *buf, size_t len, int flags);
long recv_fake(int fd, void *buf, size_t len, int flags);
long sendto_fake(int fd, const void *buf, size_t len, int flags, const void *addr, unsigned addrlen);
long recvfrom_fake(int fd, void *buf, size_t len, int flags, void *addr, unsigned *addrlen);
int getsockname_fake(int fd, void *addr, unsigned *addrlen);
int getpeername_fake(int fd, void *addr, unsigned *addrlen);
int getsockopt_fake(int fd, int level, int optname, void *optval, unsigned *optlen);
int setsockopt_fake(int fd, int level, int optname, const void *optval, unsigned optlen);
int getaddrinfo_fake(const char *node, const char *service, const void *hints, void **res);
void freeaddrinfo_fake(void *res);
void *gethostbyname_fake(const char *name);
int getnameinfo_fake(const void *sa, unsigned salen, char *host, unsigned hostlen,
                     char *serv, unsigned servlen, int flags);
const char *inet_ntop_fake(int af, const void *src, char *dst, unsigned size);
int inet_pton_fake(int af, const char *src, void *dst);
int poll_fake(void *fds, unsigned nfds, int timeout);
int select_fake(int nfds, void *rd, void *wr, void *ex, void *tv);
int fcntl_fake(int fd, int cmd, ...);
int ioctl_fake(int fd, unsigned long request, ...);
int pipe_fake(int fds[2]);
int socketpair_fake(int domain, int type, int protocol, int sv[2]);
int gethostname_fake(char *name, size_t len);
unsigned if_nametoindex_fake(const char *ifname);
const char *gai_strerror_fake(int ecode);
int dup2_fake(int oldfd, int newfd);

#endif
