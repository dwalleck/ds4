// POSIX socket subset over Winsock. SOCKET is pointer-sized on ARM64; only
// synthetic ints cross the C ABI. The reserved range never overlaps CRT fds.
#define GUFO_SOCKET_IMPLEMENTATION
#include "include/gufo_socket.h"
#include <windows.h>
#include <errno.h>
#include <limits.h>
#include <new>
#include <unordered_map>
#include <vector>
#include "include/gufo_win32_unmacro.h"

namespace {
constexpr int kSocketBase = 0x40000000;
struct Entry {
  SOCKET handle;
  int status;
  int descriptor_flags;
};
struct Registry {
  SRWLOCK lock = SRWLOCK_INIT;
  std::unordered_map<int, Entry> entries;
  unsigned int next = kSocketBase;
  ~Registry() {
    for (const auto& entry : entries) closesocket(entry.second.handle);
  }
};
Registry registry;
INIT_ONCE startup_once = INIT_ONCE_STATIC_INIT;
int startup_error;

int SocketErrno(int error) {
  switch (error) {
    case 0: return 0;
    case WSAEINTR: return EINTR;
    case WSAEBADF: return EBADF;
    case WSAEACCES: return EACCES;
    case WSAEFAULT: return EFAULT;
    case WSAEINVAL: return EINVAL;
    case WSAEMFILE: return EMFILE;
    case WSAEWOULDBLOCK: return EWOULDBLOCK;
    case WSAEINPROGRESS: return EINPROGRESS;
    case WSAEALREADY: return EALREADY;
    case WSAENOTSOCK: return ENOTSOCK;
    case WSAEDESTADDRREQ: return EDESTADDRREQ;
    case WSAEMSGSIZE: return EMSGSIZE;
    case WSAEPROTOTYPE: return EPROTOTYPE;
    case WSAENOPROTOOPT: return ENOPROTOOPT;
    case WSAEPROTONOSUPPORT: return EPROTONOSUPPORT;
    case WSAESOCKTNOSUPPORT: return EPROTONOSUPPORT;
    case WSAEOPNOTSUPP: return EOPNOTSUPP;
    case WSAEPFNOSUPPORT: return EAFNOSUPPORT;
    case WSAEAFNOSUPPORT: return EAFNOSUPPORT;
    case WSAEADDRINUSE: return EADDRINUSE;
    case WSAEADDRNOTAVAIL: return EADDRNOTAVAIL;
    case WSAENETDOWN: return ENETDOWN;
    case WSAENETUNREACH: return ENETUNREACH;
    case WSAENETRESET: return ENETRESET;
    case WSAECONNABORTED: return ECONNABORTED;
    case WSAECONNRESET: return ECONNRESET;
    case WSAENOBUFS: return ENOBUFS;
    case WSAEISCONN: return EISCONN;
    case WSAENOTCONN: return ENOTCONN;
    case WSAESHUTDOWN: return EPIPE;
    case WSAETIMEDOUT: return ETIMEDOUT;
    case WSAECONNREFUSED: return ECONNREFUSED;
    case WSAEHOSTDOWN: return EHOSTUNREACH;
    case WSAEHOSTUNREACH: return EHOSTUNREACH;
    case WSAEPROCLIM: return EAGAIN;
    case WSASYSNOTREADY: return ENETDOWN;
    case WSAVERNOTSUPPORTED: return EPROTONOSUPPORT;
    case WSANOTINITIALISED: return ENETDOWN;
    case WSA_NOT_ENOUGH_MEMORY: return ENOMEM;
    case WSA_OPERATION_ABORTED: return EINTR;
    default: return EIO;
  }
}
int Fail(int error) {
  errno = SocketErrno(error);
  return -1;
}
int Result(int result) {
  return result == SOCKET_ERROR ? Fail(WSAGetLastError()) : result;
}
BOOL CALLBACK Startup(PINIT_ONCE, PVOID, PVOID*) {
  WSADATA data;
  startup_error = WSAStartup(MAKEWORD(2, 2), &data);
  return TRUE;
}
bool Ready() {
  if (!InitOnceExecuteOnce(&startup_once, Startup, nullptr, nullptr)) {
    errno = EIO;
    return false;
  }
  if (startup_error) {
    Fail(startup_error);
    return false;
  }
  return true;
}
bool Lookup(int fd, Entry* entry) {
  AcquireSRWLockShared(&registry.lock);
  const auto found = registry.entries.find(fd);
  const bool present = found != registry.entries.end();
  if (present) *entry = found->second;
  ReleaseSRWLockShared(&registry.lock);
  if (!present) errno = gufo_socket_is_fd(fd) ? EBADF : ENOTSOCK;
  return present;
}
bool Configure(SOCKET handle, int flags) {
  if (!SetHandleInformation(reinterpret_cast<HANDLE>(handle), HANDLE_FLAG_INHERIT,
                            flags & SOCK_CLOEXEC ? 0 : HANDLE_FLAG_INHERIT)) {
    errno = EACCES;
    return false;
  }
  u_long mode = (flags & SOCK_NONBLOCK) != 0;
  return Result(ioctlsocket(handle, FIONBIO, &mode)) == 0;
}
// Takes ownership even on failure. All registry allocations are contained here.
int Adopt(SOCKET handle, int flags) {
  if (!Configure(handle, flags)) {
    const int saved = errno;
    closesocket(handle);
    errno = saved;
    return -1;
  }
  AcquireSRWLockExclusive(&registry.lock);
  int fd = -1;
  if (registry.next <= static_cast<unsigned int>(INT_MAX)) {
    try {
      fd = static_cast<int>(registry.next);
      registry.entries.emplace(fd, Entry{handle,
          O_RDWR | (flags & SOCK_NONBLOCK ? O_NONBLOCK : 0),
          flags & SOCK_CLOEXEC ? FD_CLOEXEC : 0});
      ++registry.next;
    } catch (const std::bad_alloc&) {
      fd = -1;
      errno = ENOMEM;
    }
  } else {
    errno = EMFILE;
  }
  ReleaseSRWLockExclusive(&registry.lock);
  if (fd < 0) {
    const int saved = errno;
    closesocket(handle);
    errno = saved;
  }
  return fd;
}
SOCKET NewSocket(int domain, int type, int protocol, bool cloexec) {
  const DWORD flags = WSA_FLAG_OVERLAPPED |
      (cloexec ? WSA_FLAG_NO_HANDLE_INHERIT : 0);
  const SOCKET handle = WSASocketW(domain, type, protocol, nullptr, 0, flags);
  if (handle == INVALID_SOCKET) Fail(WSAGetLastError());
  return handle;
}
bool MessageFlags(const Entry& entry, int* flags, bool writing) {
  const int supported = MSG_DONTWAIT | MSG_NOSIGNAL | MSG_OOB |
      (writing ? MSG_DONTROUTE : MSG_PEEK | MSG_WAITALL);
  if (*flags & ~supported) {
    errno = EOPNOTSUPP;
    return false;
  }
  if ((*flags & MSG_DONTWAIT) && !(entry.status & O_NONBLOCK)) {
    // Temporarily changing FIONBIO would affect other threads using this fd.
    errno = ENOTSUP;
    return false;
  }
  *flags &= ~(MSG_DONTWAIT | MSG_NOSIGNAL);
  return true;
}
// Keep a nonblocking call's mode stable against concurrent F_SETFL/close.
// Blocking calls must not hold this lock: close/shutdown may need to wake them.
struct MessageEntry {
  Entry socket;
  bool locked = false;
  ~MessageEntry() {
    if (locked) ReleaseSRWLockShared(&registry.lock);
  }
  bool Get(int fd, int* flags, bool writing) {
    if (*flags & MSG_DONTWAIT) {
      AcquireSRWLockShared(&registry.lock);
      locked = true;
      const auto found = registry.entries.find(fd);
      if (found == registry.entries.end()) {
        errno = gufo_socket_is_fd(fd) ? EBADF : ENOTSOCK;
        return false;
      }
      socket = found->second;
    } else if (!Lookup(fd, &socket)) {
      return false;
    }
    return MessageFlags(socket, flags, writing);
  }
};
struct PollScratch {
  std::vector<WSAPOLLFD> native;
  std::vector<nfds_t> indices;
};
thread_local PollScratch poll_scratch;
}  // namespace

extern "C" {
int gufo_getaddrinfo(const char* node, const char* service,
                     const struct addrinfo* hints, struct addrinfo** result) {
  if (!Ready()) return startup_error ? startup_error : EAI_FAIL;
  return getaddrinfo(node, service, hints, result);
}
int gufo_socket_is_fd(int fd) {
  return fd >= kSocketBase;
}
int gufo_socket(int domain, int type, int protocol) {
  if (!Ready()) return -1;
  const int flags = type & (SOCK_CLOEXEC | SOCK_NONBLOCK);
  const SOCKET handle = NewSocket(domain, type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK),
                                  protocol, (flags & SOCK_CLOEXEC) != 0);
  return handle == INVALID_SOCKET ? -1 : Adopt(handle, flags);
}
int gufo_bind(int fd, const struct sockaddr* address, socklen_t length) {
  Entry entry;
  if (!Lookup(fd, &entry)) return -1;
  return Result(bind(entry.handle, address, length));
}
int gufo_connect(int fd, const struct sockaddr* address, socklen_t length) {
  Entry entry;
  if (!Lookup(fd, &entry)) return -1;
  const int result = connect(entry.handle, address, length);
  if (result != SOCKET_ERROR) return result;
  const int error = WSAGetLastError();
  if (error == WSAEWOULDBLOCK) {
    errno = EINPROGRESS;
    return -1;
  }
  return Fail(error);
}
int gufo_listen(int fd, int backlog) {
  Entry entry;
  if (!Lookup(fd, &entry)) return -1;
  return Result(listen(entry.handle, backlog));
}
int gufo_accept(int fd, struct sockaddr* address, socklen_t* length) {
  Entry entry;
  if (!Lookup(fd, &entry)) return -1;
  if (address && !length) { errno = EFAULT; return -1; }
  const SOCKET accepted = accept(entry.handle, address, address ? length : nullptr);
  if (accepted == INVALID_SOCKET) return Fail(WSAGetLastError());
  // POSIX accept does not inherit O_NONBLOCK or FD_CLOEXEC from the listener.
  return Adopt(accepted, 0);
}
ssize_t gufo_send(int fd, const void* buffer, size_t length, int flags) {
  MessageEntry entry;
  if (!entry.Get(fd, &flags, true)) return -1;
  const int count = static_cast<int>(length > INT_MAX ? INT_MAX : length);
  const int result = send(entry.socket.handle, static_cast<const char*>(buffer), count, flags);
  if (result != SOCKET_ERROR) return result;
  const int error = WSAGetLastError();
  // POSIX socket send/receive timeouts are reported as EAGAIN, not ETIMEDOUT.
  if (error == WSAETIMEDOUT) { errno = EAGAIN; return -1; }
  return Fail(error);
}
ssize_t gufo_recv(int fd, void* buffer, size_t length, int flags) {
  MessageEntry entry;
  if (!entry.Get(fd, &flags, false)) return -1;
  const int count = static_cast<int>(length > INT_MAX ? INT_MAX : length);
  const int result = recv(entry.socket.handle, static_cast<char*>(buffer), count, flags);
  if (result != SOCKET_ERROR) return result;
  const int error = WSAGetLastError();
  if (error == WSAETIMEDOUT) { errno = EAGAIN; return -1; }
  return Fail(error);
}
int gufo_setsockopt(int fd, int level, int option, const void* value,
                    socklen_t length) {
  Entry entry;
  if (!Lookup(fd, &entry)) return -1;
  if (!value) { errno = EFAULT; return -1; }
  if (length < 0) { errno = EINVAL; return -1; }
  if (level == SOL_SOCKET && (option == SO_RCVTIMEO || option == SO_SNDTIMEO)) {
    if (length < static_cast<socklen_t>(sizeof(timeval))) {
      errno = EINVAL;
      return -1;
    }
    const timeval& timeout = *static_cast<const timeval*>(value);
    if (timeout.tv_sec < 0 || timeout.tv_usec < 0 || timeout.tv_usec >= 1000000) {
      errno = EINVAL;
      return -1;
    }
    const unsigned long long milliseconds =
        static_cast<unsigned long long>(timeout.tv_sec) * 1000 +
        (static_cast<unsigned long long>(timeout.tv_usec) + 999) / 1000;
    if (milliseconds > MAXDWORD) { errno = EINVAL; return -1; }
    const DWORD native = static_cast<DWORD>(milliseconds);
    return Result(setsockopt(entry.handle, level, option,
                             reinterpret_cast<const char*>(&native), sizeof(native)));
  }
  return Result(setsockopt(entry.handle, level, option,
                           static_cast<const char*>(value), length));
}
int gufo_getsockopt(int fd, int level, int option, void* value,
                    socklen_t* length) {
  Entry entry;
  if (!Lookup(fd, &entry)) return -1;
  if (!value || !length) { errno = EFAULT; return -1; }
  if (*length < 0) { errno = EINVAL; return -1; }
  if (level == SOL_SOCKET && (option == SO_RCVTIMEO || option == SO_SNDTIMEO)) {
    if (*length < static_cast<socklen_t>(sizeof(timeval))) {
      errno = EINVAL;
      return -1;
    }
    DWORD native = 0;
    int native_length = sizeof(native);
    if (Result(getsockopt(entry.handle, level, option,
                          reinterpret_cast<char*>(&native), &native_length)) < 0) return -1;
    timeval& timeout = *static_cast<timeval*>(value);
    timeout.tv_sec = static_cast<long>(native / 1000);
    timeout.tv_usec = static_cast<long>((native % 1000) * 1000);
    *length = sizeof(timeout);
    return 0;
  }
  if (level == SOL_SOCKET && option == SO_ERROR) {
    if (*length < static_cast<socklen_t>(sizeof(int))) { errno = EINVAL; return -1; }
    int error = 0;
    int native_length = sizeof(error);
    if (Result(getsockopt(entry.handle, level, option,
                          reinterpret_cast<char*>(&error), &native_length)) < 0) return -1;
    *static_cast<int*>(value) = SocketErrno(error);
    *length = sizeof(int);
    return 0;
  }
  return Result(getsockopt(entry.handle, level, option, static_cast<char*>(value), length));
}
int gufo_getsockname(int fd, struct sockaddr* address, socklen_t* length) {
  Entry entry;
  if (!Lookup(fd, &entry)) return -1;
  return Result(getsockname(entry.handle, address, length));
}
int gufo_getpeername(int fd, struct sockaddr* address, socklen_t* length) {
  Entry entry;
  if (!Lookup(fd, &entry)) return -1;
  return Result(getpeername(entry.handle, address, length));
}
int gufo_shutdown(int fd, int how) {
  Entry entry;
  if (!Lookup(fd, &entry)) return -1;
  return Result(shutdown(entry.handle, how));
}
int gufo_socketpair(int domain, int type, int protocol, int descriptors[2]) {
  if (!descriptors) { errno = EFAULT; return -1; }
  if (domain != AF_UNIX) { errno = EAFNOSUPPORT; return -1; }
  if ((type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK)) != SOCK_STREAM) {
    errno = EPROTOTYPE;
    return -1;
  }
  if (protocol != 0) { errno = EPROTONOSUPPORT; return -1; }
  if (!Ready()) return -1;
  SOCKET listener = NewSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, true);
  if (listener == INVALID_SOCKET) return -1;
  SOCKET client = INVALID_SOCKET;
  SOCKET peer = INVALID_SOCKET;
  int saved = 0;
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  int length = sizeof(address);
  if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
      listen(listener, 1) == SOCKET_ERROR ||
      getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == SOCKET_ERROR) {
    saved = SocketErrno(WSAGetLastError());
  } else {
    client = NewSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, true);
    if (client == INVALID_SOCKET) {
      saved = errno;
    } else if (connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
      saved = SocketErrno(WSAGetLastError());
    } else {
      sockaddr_in client_address = {};
      length = sizeof(client_address);
      if (getsockname(client, reinterpret_cast<sockaddr*>(&client_address), &length) == SOCKET_ERROR) {
        saved = SocketErrno(WSAGetLastError());
      } else {
        // Only publish the connection belonging to our client, not another
        // local process that happened to connect to the temporary listener.
        for (;;) {
          sockaddr_in remote = {};
          length = sizeof(remote);
          peer = accept(listener, reinterpret_cast<sockaddr*>(&remote), &length);
          if (peer == INVALID_SOCKET) { saved = SocketErrno(WSAGetLastError()); break; }
          if (remote.sin_addr.s_addr == client_address.sin_addr.s_addr &&
              remote.sin_port == client_address.sin_port) break;
          closesocket(peer);
          peer = INVALID_SOCKET;
        }
      }
    }
  }
  closesocket(listener);
  if (saved) {
    if (client != INVALID_SOCKET) closesocket(client);
    if (peer != INVALID_SOCKET) closesocket(peer);
    errno = saved;
    return -1;
  }
  const int flags = type & (SOCK_CLOEXEC | SOCK_NONBLOCK);
  const int first = Adopt(client, flags);
  if (first < 0) {
    saved = errno;
    closesocket(peer);
    errno = saved;
    return -1;
  }
  const int second = Adopt(peer, flags);
  if (second < 0) {
    saved = errno;
    gufo_socket_close(first);
    errno = saved;
    return -1;
  }
  descriptors[0] = first;
  descriptors[1] = second;
  return 0;
}
int gufo_socket_close(int fd) {
  AcquireSRWLockExclusive(&registry.lock);
  const auto found = registry.entries.find(fd);
  if (found == registry.entries.end()) {
    ReleaseSRWLockExclusive(&registry.lock);
    errno = EBADF;
    return -1;
  }
  const SOCKET handle = found->second.handle;
  const int result = closesocket(handle);
  const int error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
  // A nonblocking socket with SO_LINGER can refuse closure. Keep ownership
  // in that case so a caller can disable linger and retry without leaking it.
  if (result != SOCKET_ERROR) registry.entries.erase(found);
  ReleaseSRWLockExclusive(&registry.lock);
  return error ? Fail(error) : 0;
}
ssize_t gufo_socket_read(int fd, void* buffer, size_t length) {
  return gufo_recv(fd, buffer, length, 0);
}
ssize_t gufo_socket_write(int fd, const void* buffer, size_t length) {
  return gufo_send(fd, buffer, length, 0);
}
int gufo_socket_fcntl(int fd, int command, int argument) {
  AcquireSRWLockExclusive(&registry.lock);
  const auto found = registry.entries.find(fd);
  if (found == registry.entries.end()) {
    ReleaseSRWLockExclusive(&registry.lock);
    errno = EBADF;
    return -1;
  }
  Entry& entry = found->second;
  int result = 0;
  switch (command) {
    case F_GETFL: result = entry.status; break;
    case F_GETFD: result = entry.descriptor_flags; break;
    case F_SETFL: {
      if (argument & ~(O_ACCMODE | O_NONBLOCK)) { errno = EINVAL; result = -1; break; }
      u_long mode = (argument & O_NONBLOCK) != 0;
      result = Result(ioctlsocket(entry.handle, FIONBIO, &mode));
      if (result == 0) entry.status = O_RDWR | (argument & O_NONBLOCK);
      break;
    }
    case F_SETFD:
      if (argument & ~FD_CLOEXEC) { errno = EINVAL; result = -1; break; }
      if (!SetHandleInformation(reinterpret_cast<HANDLE>(entry.handle), HANDLE_FLAG_INHERIT,
                                argument & FD_CLOEXEC ? 0 : HANDLE_FLAG_INHERIT)) {
        errno = EACCES;
        result = -1;
      } else {
        entry.descriptor_flags = argument;
      }
      break;
    default: errno = ENOTSUP; result = -1; break;
  }
  ReleaseSRWLockExclusive(&registry.lock);
  return result;
}
int gufo_socket_spawn_check(void) {
  AcquireSRWLockShared(&registry.lock);
  int result = 0;
  for (const auto& item : registry.entries) {
    if (!(item.second.descriptor_flags & FD_CLOEXEC)) {
      errno = ENOTSUP;
      result = ENOTSUP;
      break;
    }
  }
  ReleaseSRWLockShared(&registry.lock);
  return result;
}
int gufo_socket_poll(struct pollfd* descriptors, nfds_t count) {
  if (count && !descriptors) { errno = EFAULT; return -1; }
  // Retain capacity, but do not allocate for pipe-only or invalid-fd polls.
  poll_scratch.native.clear();
  poll_scratch.indices.clear();
  AcquireSRWLockShared(&registry.lock);
  ULONG used = 0;
  int ready = 0;
  for (nfds_t i = 0; i < count; ++i) {
    if (descriptors[i].fd < 0) { descriptors[i].revents = 0; continue; }
    if (!gufo_socket_is_fd(descriptors[i].fd)) continue;
    descriptors[i].revents = 0;
    const auto found = registry.entries.find(descriptors[i].fd);
    if (found == registry.entries.end()) {
      descriptors[i].revents = POLLNVAL;
      ++ready;
      continue;
    }
    // Output-only bits are not legal WSAPoll input. Do not alter events.
    const short events = descriptors[i].events & (POLLIN | POLLOUT);
    try {
      poll_scratch.native.push_back(WSAPOLLFD{found->second.handle, events, 0});
      poll_scratch.indices.push_back(i);
    } catch (const std::bad_alloc&) {
      ReleaseSRWLockShared(&registry.lock);
      errno = ENOMEM;
      return -1;
    }
    ++used;
  }
  // Holding the registry lock during this zero-timeout call prevents close
  // from recycling a SOCKET between translation and the native probe.
  const int result = used ? WSAPoll(poll_scratch.native.data(), used, 0) : 0;
  int error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
  if (!error) {
    for (ULONG i = 0; i < used; ++i) {
      const WSAPOLLFD& native = poll_scratch.native[i];
      const nfds_t index = poll_scratch.indices[i];
      descriptors[index].revents = native.revents &
          (descriptors[index].events | POLLERR | POLLHUP | POLLNVAL);
      // WSAPoll rejects POLLPRI. Probe exceptional/OOB readiness separately;
      // one socket per select avoids Winsock's fixed FD_SETSIZE limit.
      if ((descriptors[index].events & POLLPRI) &&
          !(descriptors[index].revents & POLLERR)) {
        fd_set exceptional;
        FD_ZERO(&exceptional);
        FD_SET(native.fd, &exceptional);
        timeval immediate = {};
        const int exceptional_ready = select(0, nullptr, nullptr, &exceptional, &immediate);
        if (exceptional_ready == SOCKET_ERROR) {
          error = WSAGetLastError();
          break;
        }
        if (exceptional_ready > 0) descriptors[index].revents |= POLLPRI;
      }
      if (descriptors[index].revents) ++ready;
    }
  }
  ReleaseSRWLockShared(&registry.lock);
  return error ? Fail(error) : ready;
}
}  // extern "C"
