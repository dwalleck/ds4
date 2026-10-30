/* POSIX socket descriptors are synthetic ints, never truncated SOCKET handles.
 * Implementation: compat/win32/socket.cpp; link Ws2_32.lib. */
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include "gufo_posix.h"
#include "gufo_win32_unmacro.h"

#define SHUT_RD SD_RECEIVE
#define SHUT_WR SD_SEND
#define SHUT_RDWR SD_BOTH
#ifndef AF_UNIX
#define AF_UNIX 1
#endif
#ifndef MSG_NOSIGNAL
/* Winsock never raises SIGPIPE. Keep the bit distinct for flag validation. */
#define MSG_NOSIGNAL 0x40000000
#endif
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0x20000000
#endif
#define SOCK_CLOEXEC O_CLOEXEC
#define SOCK_NONBLOCK O_NONBLOCK
typedef int socklen_t;
typedef unsigned long nfds_t;

/* Winsock's pollfd has a pointer-sized unsigned fd, incompatible with POSIX. */
struct gufo_pollfd {
  int fd;
  short events;
  short revents;
};
#define pollfd gufo_pollfd

#ifdef __cplusplus
extern "C" {
#endif
int gufo_socket(int domain, int type, int protocol);
int gufo_getaddrinfo(const char* node, const char* service,
                     const struct addrinfo* hints, struct addrinfo** result);
int gufo_bind(int fd, const struct sockaddr* address, socklen_t length);
int gufo_connect(int fd, const struct sockaddr* address, socklen_t length);
int gufo_listen(int fd, int backlog);
int gufo_accept(int fd, struct sockaddr* address, socklen_t* length);
ssize_t gufo_send(int fd, const void* buffer, size_t length, int flags);
ssize_t gufo_recv(int fd, void* buffer, size_t length, int flags);
int gufo_setsockopt(int fd, int level, int option, const void* value,
                    socklen_t length);
int gufo_getsockopt(int fd, int level, int option, void* value,
                    socklen_t* length);
int gufo_getsockname(int fd, struct sockaddr* address, socklen_t* length);
int gufo_getpeername(int fd, struct sockaddr* address, socklen_t* length);
int gufo_shutdown(int fd, int how);
int gufo_socketpair(int domain, int type, int protocol, int descriptors[2]);

/* posix.cpp routes reserved descriptors through these helpers. is_fd tests
 * the reserved range, including closed descriptors (which must return EBADF).
 * fcntl supports GETFL/SETFL and GETFD/SETFD, not descriptor duplication.
 * MSG_DONTWAIT on a blocking socket fails with ENOTSUP rather than changing
 * a shared socket's mode and racing a concurrent reader/writer. */
int gufo_socket_is_fd(int fd);
int gufo_socket_close(int fd);
ssize_t gufo_socket_read(int fd, void* buffer, size_t length);
ssize_t gufo_socket_write(int fd, const void* buffer, size_t length);
int gufo_socket_fcntl(int fd, int command, int argument);
/* Spawn cannot reconstruct synthetic descriptors through the CRT fd table.
 * Returns ENOTSUP while an inheritable socket is open, otherwise zero. */
int gufo_socket_spawn_check(void);
/* Nonblocking sockets-only probe for the parent's mixed-descriptor poll loop.
 * Preserves events; clears negative fds; leaves CRT fd revents untouched.
 * Returns the number of socket entries with revents, or -1 with errno set. */
int gufo_socket_poll(struct pollfd* descriptors, nfds_t count);
int poll(struct pollfd* descriptors, nfds_t count, int timeout_ms);
#ifdef __cplusplus
}
#endif

#ifndef GUFO_SOCKET_IMPLEMENTATION
#define socket gufo_socket
#define getaddrinfo gufo_getaddrinfo
#define bind gufo_bind
#define connect gufo_connect
#define listen gufo_listen
#define accept gufo_accept
#define send gufo_send
#define recv gufo_recv
#define setsockopt gufo_setsockopt
#define getsockopt gufo_getsockopt
#define getsockname gufo_getsockname
#define getpeername gufo_getpeername
#define shutdown gufo_shutdown
#define socketpair gufo_socketpair
#endif
