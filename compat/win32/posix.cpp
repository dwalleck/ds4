// Win32 implementation of the POSIX subset declared in include/gufo_posix.h.
//
// File descriptors are UCRT descriptors wrapping Win32 handles, so `int fd`
// code keeps working, but every read, write and seek goes straight to the
// handle rather than through the CRT's text-mode machinery.
//
// Local semantic corrections and supported boundaries are recorded in VENDOR.md.
// Stream handles stay synchronous. Positional I/O uses a separate cached
// overlapped file description, never the stream's shared current position.

#define GUFO_STDIO_IMPLEMENTATION
#include "memory_stream.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>
#include <winternl.h>

#include <crtdbg.h>
#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>

#include <atomic>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "gufo_socket.h"
// Only this implementation calls the CRT's actual fclose after unregistering.
#undef fclose
#undef fopen

namespace {

// UCRT raises its invalid-parameter handler (which terminates by default) for
// a bad descriptor. POSIX code expects EBADF instead.
void IgnoreInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*,
                            unsigned int, uintptr_t) {}


int ErrnoFromWin32(DWORD error) {
  switch (error) {
    case ERROR_SUCCESS:
      return 0;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_PATHNAME:
    case ERROR_INVALID_NAME:
      return ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_WRITE_PROTECT:
    case ERROR_PRIVILEGE_NOT_HELD:
      return EACCES;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
      return EEXIST;
    case ERROR_DIR_NOT_EMPTY:
      return ENOTEMPTY;
    case ERROR_DIRECTORY:
      return ENOTDIR;
    case ERROR_INVALID_HANDLE:
      return EBADF;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
    case ERROR_COMMITMENT_LIMIT:
      return ENOMEM;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
      return ENOSPC;
    case ERROR_TOO_MANY_OPEN_FILES:
      return EMFILE;
    case ERROR_BROKEN_PIPE:
    case ERROR_NO_DATA:
      return EPIPE;
    case ERROR_NOT_SUPPORTED:
    case ERROR_CALL_NOT_IMPLEMENTED:
      return ENOTSUP;
    case ERROR_INVALID_PARAMETER:
    case ERROR_NEGATIVE_SEEK:
      return EINVAL;
    case ERROR_NOT_LOCKED:
      return EINVAL;
    case ERROR_WORKING_SET_QUOTA:
      return ENOMEM;
    case ERROR_OPERATION_ABORTED:
      return EINTR;
    case ERROR_CANT_RESOLVE_FILENAME:
      return ELOOP;
    case ERROR_FILENAME_EXCED_RANGE:
      return ENAMETOOLONG;
    default:
      return EIO;
  }
}

int Fail(DWORD error) {
  errno = ErrnoFromWin32(error);
  return -1;
}

std::wstring Widen(const char* utf8) {
  if (utf8 == nullptr || *utf8 == '\0') {
    return {};
  }
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
  std::wstring out(n > 0 ? n - 1 : 0, L'\0');
  if (n > 0) {
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), n);
  }
  for (auto& c : out) {
    if (c == L'/') {
      c = L'\\';
    }
  }
  return out;
}

std::wstring Backslashed(const wchar_t* wide) {
  std::wstring out = wide != nullptr ? wide : L"";
  for (auto& c : out) {
    if (c == L'/') {
      c = L'\\';
    }
  }
  return out;
}

std::string Narrow(const wchar_t* wide) {
  const int n =
      WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
  std::string out(n > 0 ? n - 1 : 0, '\0');
  if (n > 0) {
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), n, nullptr,
                        nullptr);
  }
  return out;
}

HANDLE HandleOf(int fd) {
  if (fd < 0) {
    return INVALID_HANDLE_VALUE;
  }
  const auto previous =
      _set_thread_local_invalid_parameter_handler(IgnoreInvalidParameter);
  const intptr_t handle = _get_osfhandle(fd);
  _set_thread_local_invalid_parameter_handler(previous);
  return reinterpret_cast<HANDLE>(handle);
}

constexpr DWORD kLockOffsetLow = 0xFFFFFFFEu;
constexpr DWORD kLockOffsetHigh = 0x7FFFFFFFu;

struct Description {
  std::mutex mutex;
  HANDLE positional = INVALID_HANDLE_VALUE;
  HANDLE lock_handle = INVALID_HANDLE_VALUE;
  int lock_mode = 0;
  std::atomic<int> status{0};  // Mutable open-description flags shared by dup.
  ~Description() {
    if (positional != INVALID_HANDLE_VALUE) CloseHandle(positional);
    if (lock_handle != INVALID_HANDLE_VALUE) {
      if (lock_mode != 0) {
        OVERLAPPED ov{};
        ov.Offset = kLockOffsetLow;
        ov.OffsetHigh = kLockOffsetHigh;
        UnlockFileEx(lock_handle, 0, 1, 0, &ov);
      }
      CloseHandle(lock_handle);
    }
  }
};

std::mutex g_fd_mutex;
std::unordered_map<int, int> g_fd_flags;
std::unordered_map<int, std::shared_ptr<Description>> g_descriptions;
// UCRT low-level descriptors have a separate 8192-entry table; _getmaxstdio
// only limits FILE streams and must not bound open/dup/spawn descriptors.
constexpr int kMaxDescriptors = 8192;
int g_initial_flags[kMaxDescriptors]{};
INIT_ONCE g_inherited_once = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK ReadInheritedFlags(PINIT_ONCE, PVOID, PVOID*) {
  STARTUPINFOW startup{};
  GetStartupInfoW(&startup);
  const unsigned char* bytes = startup.lpReserved2;
  const size_t size = startup.cbReserved2;
  if (!bytes || size < sizeof(int)) return TRUE;
  int count;
  memcpy(&count, bytes, sizeof(count));
  if (count < 0 || count > kMaxDescriptors) return TRUE;
  const size_t prefix = sizeof(int) + static_cast<size_t>(count) * (1 + sizeof(intptr_t));
  constexpr unsigned char magic[8] = {'D', 'S', '4', 'F', 'D', 'V', '1', 0};
  if (prefix > size || size - prefix < 12 ||
      memcmp(bytes + prefix, magic, sizeof(magic))) return TRUE;
  uint32_t entries;
  memcpy(&entries, bytes + prefix + 8, sizeof(entries));
  if (entries > static_cast<uint32_t>(count) ||
      static_cast<size_t>(entries) * 8 != size - prefix - 12) return TRUE;
  for (uint32_t i = 0; i < entries; ++i) {
    int32_t fd;
    uint32_t flags;
    memcpy(&fd, bytes + prefix + 12 + i * 8, sizeof(fd));
    memcpy(&flags, bytes + prefix + 16 + i * 8, sizeof(flags));
    if (fd >= 0 && fd < count && HandleOf(fd) != INVALID_HANDLE_VALUE)
      g_initial_flags[fd] = static_cast<int>(flags);
  }
  return TRUE;
}

void EnsureInheritedFlags() {
  InitOnceExecuteOnce(&g_inherited_once, ReadInheritedFlags, nullptr, nullptr);
}

int InitialFlags(int fd) {
  return fd >= 0 && fd < kMaxDescriptors ? g_initial_flags[fd] : 0;
}

void RememberFlags(int fd, int flags) {
  EnsureInheritedFlags();
  std::lock_guard lock(g_fd_mutex);
  g_fd_flags[fd] = flags;
  if (fd >= 0 && fd < kMaxDescriptors) g_initial_flags[fd] = 0;
}

int FlagsOf(int fd) {
  EnsureInheritedFlags();
  std::lock_guard lock(g_fd_mutex);
  const auto it = g_fd_flags.find(fd);
  const auto description = g_descriptions.find(fd);
  const int flags = it == g_fd_flags.end() ? InitialFlags(fd) : it->second;
  const int status = description == g_descriptions.end() ? flags & O_NONBLOCK
      : description->second->status.load(std::memory_order_relaxed);
  return (flags & ~O_NONBLOCK) | status;
}

std::shared_ptr<Description> DescriptionOf(int fd) {
  EnsureInheritedFlags();
  std::lock_guard lock(g_fd_mutex);
  auto& state = g_descriptions[fd];
  if (!state) {
    state = std::make_shared<Description>();
    const auto flags = g_fd_flags.find(fd);
    state->status.store((flags == g_fd_flags.end() ? InitialFlags(fd) : flags->second) & O_NONBLOCK);
  }
  return state;
}

std::shared_ptr<Description> ForgetFlags(int fd) {
  EnsureInheritedFlags();
  std::lock_guard lock(g_fd_mutex);
  std::shared_ptr<Description> retained;
  const auto found = g_descriptions.find(fd);
  if (found != g_descriptions.end()) {
    retained = std::move(found->second);
    g_descriptions.erase(found);
  }
  g_fd_flags.erase(fd);
  if (fd >= 0 && fd < kMaxDescriptors) g_initial_flags[fd] = 0;
  return retained;
}

int WrapHandle(HANDLE handle, int flags) {
  int crt = _O_BINARY | _O_NOINHERIT;
  if ((flags & O_ACCMODE) == O_RDONLY) {
    crt |= _O_RDONLY;
  } else if ((flags & O_ACCMODE) == O_WRONLY) {
    crt |= _O_WRONLY;
  } else {
    crt |= _O_RDWR;
  }
  if (flags & O_APPEND) {
    crt |= _O_APPEND;
  }
  const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), crt);
  if (fd < 0) {
    CloseHandle(handle);
    errno = EMFILE;
    return -1;
  }
  try {
    RememberFlags(fd, flags);
  } catch (const std::bad_alloc&) {
    _close(fd);
    errno = ENOMEM;
    return -1;
  }
  return fd;
}

DWORD AccessFor(int flags) {
  DWORD access = 0;
  switch (flags & O_ACCMODE) {
    case O_RDONLY:
      access = GENERIC_READ;
      break;
    case O_WRONLY:
      access = GENERIC_WRITE;
      break;
    default:
      access = GENERIC_READ | GENERIC_WRITE;
      break;
  }
  if (flags & O_APPEND) {
    // Stream writes append atomically; positioned writes reject append handles.
    access &= ~static_cast<DWORD>(GENERIC_WRITE);
    access |= FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE;
  }
  return access | FILE_READ_ATTRIBUTES;
}

DWORD AttributesFor(int flags) {
  DWORD attributes = FILE_ATTRIBUTE_NORMAL;
  if (flags & O_DIRECTORY) {
    attributes |= FILE_FLAG_BACKUP_SEMANTICS;
  }
  if (flags & O_NOFOLLOW) {
    attributes |= FILE_FLAG_OPEN_REPARSE_POINT;
  }
  if (flags & O_DIRECT) {
    attributes |= FILE_FLAG_NO_BUFFERING;
  }
  if (flags & O_CONCURRENT_RANDOM) {
    attributes |= FILE_FLAG_RANDOM_ACCESS;
  }
  if (flags & O_SYNC) {
    attributes |= FILE_FLAG_WRITE_THROUGH;
  }
  return attributes;
}

int ReopenDescriptor(int source, int flags) {
  const HANDLE handle = HandleOf(source);
  if (handle == INVALID_HANDLE_VALUE) {
    errno = EBADF;
    return -1;
  }
  const HANDLE reopened =
      ReOpenFile(handle, AccessFor(flags) & ~FILE_READ_ATTRIBUTES,
                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                 AttributesFor(flags) & ~FILE_ATTRIBUTE_NORMAL);
  if (reopened == INVALID_HANDLE_VALUE) {
    return Fail(GetLastError());
  }
  return WrapHandle(reopened, flags);
}

bool IsDirectoryHandle(HANDLE handle) {
  BY_HANDLE_FILE_INFORMATION info{};
  return GetFileInformationByHandle(handle, &info) &&
         (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

int OpenPath(const std::wstring& path, int flags, mode_t mode) {
  constexpr int supported = O_ACCMODE | O_APPEND | O_CREAT | O_TRUNC | O_EXCL |
      O_BINARY | O_CLOEXEC | O_DIRECT | O_NOFOLLOW | O_DIRECTORY | O_SYNC |
      O_CONCURRENT_RANDOM | O_NONBLOCK;
  if ((flags & O_ACCMODE) == O_ACCMODE) { errno = EINVAL; return -1; }
  if (flags & ~supported) { errno = ENOTSUP; return -1; }
  DWORD disposition = OPEN_EXISTING;
  if (flags & O_CREAT) {
    if (flags & O_EXCL) {
      disposition = CREATE_NEW;
    } else if (flags & O_TRUNC) {
      disposition = CREATE_ALWAYS;
    } else {
      disposition = OPEN_ALWAYS;
    }
  } else if (flags & O_TRUNC) {
    disposition = TRUNCATE_EXISTING;
  }
  DWORD attributes = AttributesFor(flags);
  if ((flags & O_CREAT) && (mode & S_IWUSR) == 0 && mode != 0) {
    attributes |= FILE_ATTRIBUTE_READONLY;
  }
  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, FALSE};
  const HANDLE handle = CreateFileW(
      path.c_str(), AccessFor(flags),
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &security,
      disposition, attributes, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    DWORD error = GetLastError();
    if (error == ERROR_ACCESS_DENIED && !(flags & O_DIRECTORY)) {
      // Opening a directory without O_DIRECTORY is EISDIR on Linux.
      const DWORD found = GetFileAttributesW(path.c_str());
      if (found != INVALID_FILE_ATTRIBUTES &&
          (found & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        errno = EISDIR;
        return -1;
      }
    }
    return Fail(error);
  }
  if (flags & O_NONBLOCK) {
    const DWORD type = GetFileType(handle);
    /* Regular-file O_NONBLOCK is a status flag, not an asynchronous-I/O
     * promise. Pipes support it only on the read end via PeekNamedPipe. */
    if (type != FILE_TYPE_DISK &&
        !(type == FILE_TYPE_PIPE && (flags & O_ACCMODE) == O_RDONLY)) {
      CloseHandle(handle);
      errno = ENOTSUP;
      return -1;
    }
  }
  if (flags & O_DIRECTORY) {
    if (!IsDirectoryHandle(handle)) {
      CloseHandle(handle);
      errno = ENOTDIR;
      return -1;
    }
  }
  if (flags & O_NOFOLLOW) {
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag,
                                     sizeof(tag)) &&
        (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      CloseHandle(handle);
      errno = ELOOP;
      return -1;
    }
  }
  return WrapHandle(handle, flags);
}

std::wstring DirectoryPathOf(int dirfd) {
  if (dirfd == AT_FDCWD) {
    return {};
  }
  const HANDLE handle = HandleOf(dirfd);
  wchar_t buffer[32768];
  const DWORD n = GetFinalPathNameByHandleW(handle, buffer, 32768,
                                            FILE_NAME_NORMALIZED);
  if (n == 0 || n >= 32768) {
    return {};
  }
  return std::wstring(buffer, n);
}

std::wstring ResolveAt(int dirfd, const char* path) {
  std::wstring relative = Widen(path);
  const bool absolute =
      relative.size() >= 2 && (relative[1] == L':' || relative[0] == L'\\');
  if (dirfd == AT_FDCWD || absolute) {
    return relative;
  }
  std::wstring base = DirectoryPathOf(dirfd);
  if (base.empty()) {
    return {};
  }
  if (base.back() != L'\\') {
    base += L'\\';
  }
  return base + relative;
}

struct timespec TimespecFromFiletime(const FILETIME& ft) {
  ULARGE_INTEGER value;
  value.LowPart = ft.dwLowDateTime;
  value.HighPart = ft.dwHighDateTime;
  constexpr unsigned long long kEpochDelta = 116444736000000000ULL;
  const unsigned long long ticks =
      value.QuadPart > kEpochDelta ? value.QuadPart - kEpochDelta : 0;
  struct timespec ts;
  ts.tv_sec = static_cast<time_t>(ticks / 10000000ULL);
  ts.tv_nsec = static_cast<long>((ticks % 10000000ULL) * 100ULL);
  return ts;
}

int StatHandle(HANDLE handle, struct stat* st, bool no_follow) {
  memset(st, 0, sizeof(*st));
  const DWORD type = GetFileType(handle);
  if (type == FILE_TYPE_CHAR) {
    st->st_mode = S_IFCHR | 0600;
    return 0;
  }
  if (type == FILE_TYPE_PIPE) {
    st->st_mode = S_IFIFO | 0600;
    return 0;
  }
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(handle, &info)) {
    return Fail(GetLastError());
  }
  const bool directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
  bool link = false;
  if (no_follow && (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
    FILE_ATTRIBUTE_TAG_INFO tag{};
    link = GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag,
                                        sizeof(tag)) &&
           (tag.ReparseTag == IO_REPARSE_TAG_SYMLINK ||
            tag.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT);
  }
  mode_t permissions = directory ? 0700 : 0600;
  if (info.dwFileAttributes & FILE_ATTRIBUTE_READONLY) {
    permissions &= ~static_cast<mode_t>(0200);
  }
  st->st_mode =
      (link ? S_IFLNK : (directory ? S_IFDIR : S_IFREG)) | permissions;
  st->st_dev = info.dwVolumeSerialNumber;
  st->st_ino = (static_cast<unsigned long long>(info.nFileIndexHigh) << 32) |
               info.nFileIndexLow;
  st->st_nlink = info.nNumberOfLinks;
  st->st_size = static_cast<off_t>(
      (static_cast<unsigned long long>(info.nFileSizeHigh) << 32) |
      info.nFileSizeLow);
  st->st_blksize = 4096;
  st->st_blocks = (st->st_size + 511) / 512;
  st->st_atim = TimespecFromFiletime(info.ftLastAccessTime);
  st->st_mtim = TimespecFromFiletime(info.ftLastWriteTime);
  // st_ctime is the inode change time, which NTFS tracks as ChangeTime; it
  // moves on every write even when the modification time is restored.
  FILE_BASIC_INFO basic{};
  if (GetFileInformationByHandleEx(handle, FileBasicInfo, &basic,
                                   sizeof(basic))) {
    FILETIME change;
    change.dwLowDateTime = static_cast<DWORD>(basic.ChangeTime.QuadPart);
    change.dwHighDateTime = static_cast<DWORD>(basic.ChangeTime.QuadPart >> 32);
    st->st_ctim = TimespecFromFiletime(change);
  } else {
    st->st_ctim = st->st_mtim;
  }
  return 0;
}

int StatPath(const std::wstring& path, struct stat* st, bool no_follow) {
  DWORD attributes = FILE_FLAG_BACKUP_SEMANTICS;
  if (no_follow) {
    attributes |= FILE_FLAG_OPEN_REPARSE_POINT;
  }
  const HANDLE handle =
      CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, attributes, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Fail(GetLastError());
  }
  const int result = StatHandle(handle, st, no_follow);
  CloseHandle(handle);
  return result;
}

HANDLE ThreadEvent() {
  struct Holder {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~Holder() {
      if (event != nullptr) {
        CloseHandle(event);
      }
    }
  };
  thread_local Holder holder;
  return holder.event;
}


// The separate overlapped description prevents pread/pwrite from changing the
// stream position even on ordinary (non-O_DIRECT) open() descriptors.
ssize_t Transfer(int fd, void* buffer, size_t count, off_t offset,
                 bool writing) {
  const HANDLE stream = HandleOf(fd);
  if (stream == INVALID_HANDLE_VALUE) {
    errno = EBADF;
    return -1;
  }
  if (offset < 0 || count > static_cast<size_t>(LLONG_MAX) ||
      count > static_cast<unsigned long long>(LLONG_MAX - offset)) {
    errno = EINVAL;
    return -1;
  }
  const int flags = FlagsOf(fd);
  if ((writing && (flags & O_ACCMODE) == O_RDONLY) ||
      (!writing && (flags & O_ACCMODE) == O_WRONLY)) {
    errno = EBADF;
    return -1;
  }
  if (writing && (flags & O_APPEND)) {
    errno = ENOTSUP;
    return -1;
  }
  if (GetFileType(stream) != FILE_TYPE_DISK || IsDirectoryHandle(stream)) {
    errno = ESPIPE;
    return -1;
  }
  std::shared_ptr<Description> state;
  try {
    state = DescriptionOf(fd);
  } catch (const std::bad_alloc&) {
    errno = ENOMEM;
    return -1;
  }
  HANDLE handle;
  {
    std::lock_guard lock(state->mutex);
    if (state->positional == INVALID_HANDLE_VALUE) {
      state->positional = ReOpenFile(
          stream, AccessFor(flags) & ~FILE_READ_ATTRIBUTES,
          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
          (AttributesFor(flags) & ~FILE_ATTRIBUTE_NORMAL) | FILE_FLAG_OVERLAPPED);
      if (state->positional == INVALID_HANDLE_VALUE) return Fail(GetLastError());
    }
    handle = state->positional;
  }
  OVERLAPPED overlapped{};
  overlapped.Offset = static_cast<DWORD>(static_cast<unsigned long long>(offset));
  overlapped.OffsetHigh =
      static_cast<DWORD>(static_cast<unsigned long long>(offset) >> 32);
  overlapped.hEvent = ThreadEvent();
  if (!overlapped.hEvent) return Fail(GetLastError());
  ResetEvent(overlapped.hEvent);
  const DWORD chunk = static_cast<DWORD>(count > (1U << 30) ? (1U << 30) : count);
  DWORD done = 0;
  const BOOL ok = writing ? WriteFile(handle, buffer, chunk, &done, &overlapped)
                          : ReadFile(handle, buffer, chunk, &done, &overlapped);
  if (!ok) {
    DWORD error = GetLastError();
    if (error == ERROR_IO_PENDING) {
      if (GetOverlappedResult(handle, &overlapped, &done, TRUE))
        return static_cast<ssize_t>(done);
      error = GetLastError();
    }
    if (!writing && error == ERROR_HANDLE_EOF) return 0;
    return Fail(error);
  }
  return static_cast<ssize_t>(done);
}

// Current-position transfer for synchronous handles (files, pipes, console).
ssize_t Stream(int fd, void* buffer, size_t count, bool writing) {
  const HANDLE handle = HandleOf(fd);
  if (handle == INVALID_HANDLE_VALUE) {
    errno = EBADF;
    return -1;
  }
  const DWORD chunk =
      static_cast<DWORD>(count > (1U << 30) ? (1U << 30) : count);
  DWORD done = 0;
  const BOOL ok = writing ? WriteFile(handle, buffer, chunk, &done, nullptr)
                          : ReadFile(handle, buffer, chunk, &done, nullptr);
  if (!ok) {
    const DWORD error = GetLastError();
    if (!writing &&
        (error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF)) {
      return 0;
    }
    return Fail(error);
  }
  return static_cast<ssize_t>(done);
}

SYSTEM_INFO SystemInfo() {
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  return info;
}
struct Mapping {
  void* base;
  size_t length;
  bool anonymous;
  HANDLE flush_file;
};
std::mutex g_mapping_mutex;
std::unordered_map<void*, Mapping> g_mappings;

bool RememberMapping(void* address, void* base, size_t length, bool anonymous,
                     HANDLE flush_file = INVALID_HANDLE_VALUE) {
  try {
    std::lock_guard lock(g_mapping_mutex);
    g_mappings.emplace(address, Mapping{base, length, anonymous, flush_file});
    return true;
  } catch (const std::bad_alloc&) {
    if (anonymous) VirtualFree(base, 0, MEM_RELEASE);
    else UnmapViewOfFile(base);
    if (flush_file != INVALID_HANDLE_VALUE) CloseHandle(flush_file);
    errno = ENOMEM;
    return false;
  }
}

// Child processes started by the posix_spawn shim, so waitpid() can find a
// process that has already exited.
std::mutex g_child_mutex;
struct Child {
  HANDLE process;
  HANDLE job;
  int signal;
};
std::unordered_map<pid_t, Child> g_children;

}  // namespace

extern "C" {

// Used by compat/win32/spawn.cpp.
int gufo_register_child(pid_t pid, void* process, void* job) {
  try {
    std::lock_guard lock(g_child_mutex);
    const bool inserted = g_children.emplace(
        pid, Child{static_cast<HANDLE>(process), static_cast<HANDLE>(job), 0}).second;
    return inserted ? 0 : EAGAIN;
  } catch (const std::bad_alloc&) {
    return ENOMEM;
  }
}

int ds4_win32_spawn_forget(pid_t pid) {
  std::lock_guard lock(g_child_mutex);
  const auto found = g_children.find(pid);
  if (found == g_children.end()) { errno = ECHILD; return -1; }
  if (found->second.process) CloseHandle(found->second.process);
  if (found->second.job) CloseHandle(found->second.job);
  g_children.erase(found);
  return 0;
}
int ds4_win32_spawn_detach(pid_t pid) {
  std::lock_guard lock(g_child_mutex);
  const auto found = g_children.find(pid);
  if (found == g_children.end()) { errno = ECHILD; return -1; }
  if (found->second.job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    if (!QueryInformationJobObject(found->second.job, JobObjectExtendedLimitInformation,
                                   &limits, sizeof(limits), nullptr))
      return Fail(GetLastError());
    limits.BasicLimitInformation.LimitFlags &= ~JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(found->second.job, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits)))
      return Fail(GetLastError());
  }
  if (found->second.process) CloseHandle(found->second.process);
  if (found->second.job) CloseHandle(found->second.job);
  g_children.erase(found);
  return 0;
}

int gufo_spawn_fd_snapshot(
    int (*visit)(int, intptr_t, int, void*), void* context) {
  const int socket_error = gufo_socket_spawn_check();
  if (socket_error) return socket_error;
  EnsureInheritedFlags();
  std::lock_guard lock(g_fd_mutex);
  for (int fd = 0; fd < kMaxDescriptors; ++fd) {
    const HANDLE handle = HandleOf(fd);
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr) continue;
    const auto found = g_fd_flags.find(fd);
    int flags = found == g_fd_flags.end() ? InitialFlags(fd) : found->second;
    const auto description = g_descriptions.find(fd);
    if (description != g_descriptions.end())
      flags = (flags & ~O_NONBLOCK) | description->second->status.load(std::memory_order_relaxed);
    const int error = visit(fd, reinterpret_cast<intptr_t>(handle), flags, context);
    if (error) return error;
  }
  return 0;
}
int gufo_reopen(int fd, int flags) {
  try { return ReopenDescriptor(fd, flags); }
  catch (const std::bad_alloc&) { errno = ENOMEM; return -1; }
}

int gufo_fclose(FILE* stream) {
  if (!stream) { errno = EINVAL; return EOF; }
  int memory_result;
  if (gufo_memory_close(stream, &memory_result)) return memory_result;
  const int fd = _fileno(stream);
  // Remove the fd registration before CRT close (the number can be reused),
  // but keep its lock/description alive until the CRT has flushed its buffers.
  const auto retained = fd >= 0 ? ForgetFlags(fd) : nullptr;
  return fclose(stream);
}

int nanosleep(const struct timespec* request, struct timespec* remaining) {
  if (!request || request->tv_sec < 0 || request->tv_nsec < 0 ||
      request->tv_nsec >= 1000000000L || request->tv_sec > (LLONG_MAX - 9999999) / 10000000) {
    errno = EINVAL; return -1;
  }
  LARGE_INTEGER due;
  due.QuadPart = -(request->tv_sec * 10000000LL + (request->tv_nsec + 99) / 100);
  if (!due.QuadPart) due.QuadPart = -1;
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                      TIMER_MODIFY_STATE | SYNCHRONIZE);
  if (!timer) timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
  if (!timer) return Fail(GetLastError());
  if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
    const DWORD error = GetLastError(); CloseHandle(timer); return Fail(error);
  }
  const DWORD waited = WaitForSingleObject(timer, INFINITE);
  const DWORD error = GetLastError();
  CloseHandle(timer);
  if (waited != WAIT_OBJECT_0) return Fail(error);
  if (remaining) memset(remaining, 0, sizeof(*remaining));
  return 0;
}

int gettimeofday(struct timeval* value, void* timezone) {
  if (!value) { errno = EFAULT; return -1; }
  if (timezone) { errno = ENOTSUP; return -1; }
  FILETIME now;
  GetSystemTimePreciseAsFileTime(&now);
  const struct timespec ts = TimespecFromFiletime(now);
  if (ts.tv_sec > LONG_MAX) { errno = EOVERFLOW; return -1; }
  value->tv_sec = static_cast<long>(ts.tv_sec);
  value->tv_usec = ts.tv_nsec / 1000;
  return 0;
}
int getpagesize(void) { return static_cast<int>(SystemInfo().dwPageSize); }
FILE* gufo_fopen(const char* path, const char* mode) {
  if (!path || !mode || !*mode) { errno = EINVAL; return nullptr; }
  try {
    std::string binary_mode(mode);
    if (binary_mode.find_first_not_of("rwa+bx") != std::string::npos) {
      errno = ENOTSUP; return nullptr;
    }
    if (binary_mode.find('b') == std::string::npos) binary_mode += 'b';
    const std::wstring name = Widen(path);
    const std::wstring wide_mode = Widen(binary_mode.c_str());
    FILE* stream = _wfopen(name.c_str(), wide_mode.c_str());
    if (!stream) return nullptr;
    const int flags = (strchr(mode, '+') ? O_RDWR : mode[0] == 'r' ? O_RDONLY : O_WRONLY)
        | (mode[0] == 'a' ? O_APPEND : 0);
    try { RememberFlags(_fileno(stream), flags); }
    catch (...) { fclose(stream); throw; }
    return stream;
  } catch (const std::bad_alloc&) { errno = ENOMEM; return nullptr; }
}

int open(const char* path, int flags, ...) {
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list args;
    va_start(args, flags);
    mode = va_arg(args, int);
    va_end(args);
  }
  if (path == nullptr) {
    errno = EFAULT;
    return -1;
  }
  if (strcmp(path, "/dev/null") == 0) return OpenPath(L"NUL", flags, mode);
  constexpr char kProcFd[] = "/proc/self/fd/";
  if (strncmp(path, kProcFd, sizeof(kProcFd) - 1) == 0) {
    return ReopenDescriptor(atoi(path + sizeof(kProcFd) - 1), flags);
  }
  return OpenPath(Widen(path), flags, mode);
}

int openat(int dirfd, const char* path, int flags, ...) {
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list args;
    va_start(args, flags);
    mode = va_arg(args, int);
    va_end(args);
  }
  const std::wstring resolved = ResolveAt(dirfd, path);
  if (resolved.empty()) {
    errno = EBADF;
    return -1;
  }
  return OpenPath(resolved, flags, mode);
}

int close(int fd) {
  if (gufo_socket_is_fd(fd)) return gufo_socket_close(fd);
  if (HandleOf(fd) == INVALID_HANDLE_VALUE) {
    errno = EBADF;
    return -1;
  }
  const auto retained = ForgetFlags(fd);
  return _close(fd);
}

ssize_t read(int fd, void* buf, size_t count) {
  if (gufo_socket_is_fd(fd)) return gufo_socket_read(fd, buf, count);
  const HANDLE handle = HandleOf(fd);
  if (GetFileType(handle) == FILE_TYPE_PIPE && (FlagsOf(fd) & O_NONBLOCK)) {
    DWORD available = 0;
    if (!PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr)) {
      if (GetLastError() == ERROR_BROKEN_PIPE) return 0;
      return Fail(GetLastError());
    }
    if (!available && count) { errno = EAGAIN; return -1; }
    if (count > available) count = available;
  }
  return Stream(fd, buf, count, false);
}

ssize_t write(int fd, const void* buf, size_t count) {
  if (gufo_socket_is_fd(fd)) return gufo_socket_write(fd, buf, count);
  return Stream(fd, const_cast<void*>(buf), count, true);
}

ssize_t pread(int fd, void* buf, size_t count, off_t offset) {
  if (gufo_socket_is_fd(fd)) { errno = ESPIPE; return -1; }
  return Transfer(fd, buf, count, offset, false);
}

ssize_t pwrite(int fd, const void* buf, size_t count, off_t offset) {
  if (gufo_socket_is_fd(fd)) { errno = ESPIPE; return -1; }
  return Transfer(fd, const_cast<void*>(buf), count, offset, true);
}

off_t lseek(int fd, off_t offset, int whence) {
  if (gufo_socket_is_fd(fd)) { errno = ESPIPE; return -1; }
  if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
    errno = EINVAL;
    return -1;
  }
  const HANDLE handle = HandleOf(fd);
  LARGE_INTEGER distance;
  distance.QuadPart = offset;
  LARGE_INTEGER position;
  const DWORD method = whence == SEEK_SET   ? FILE_BEGIN
                       : whence == SEEK_CUR ? FILE_CURRENT
                                            : FILE_END;
  if (!SetFilePointerEx(handle, distance, &position, method)) {
    return Fail(GetLastError());
  }
  return position.QuadPart;
}

int fsync(int fd) {
  const HANDLE handle = HandleOf(fd);
  if (handle == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
  if (!FlushFileBuffers(handle)) return Fail(GetLastError());
  return 0;
}

int fdatasync(int fd) { return fsync(fd); }

int ftruncate(int fd, off_t length) {
  if (length < 0) { errno = EINVAL; return -1; }
  FILE_END_OF_FILE_INFO info{};
  info.EndOfFile.QuadPart = length;
  if (!SetFileInformationByHandle(HandleOf(fd), FileEndOfFileInfo, &info,
                                  sizeof(info))) {
    return Fail(GetLastError());
  }
  return 0;
}

int posix_fallocate(int fd, off_t offset, off_t length) {
  if (offset < 0 || length <= 0 || offset > LLONG_MAX - length) return EINVAL;
  if (HandleOf(fd) == INVALID_HANDLE_VALUE) return EBADF;
  // Extending EOF is not a reservation guarantee (sparse files/quota).
  return ENOTSUP;
}

int posix_fadvise(int fd, off_t offset, off_t length, int advice) {
  if (HandleOf(fd) == INVALID_HANDLE_VALUE) return EBADF;
  if (offset < 0 || length < 0 || advice < POSIX_FADV_NORMAL ||
      advice > POSIX_FADV_NOREUSE) return EINVAL;
  // Advisory hints may be ignored; unlike allocation/locking no guarantee.
  return 0;
}

int fcntl(int fd, int cmd, ...) {
  int argument = 0;
  if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC || cmd == F_SETFD || cmd == F_SETFL) {
    va_list args;
    va_start(args, cmd);
    argument = va_arg(args, int);
    va_end(args);
  }
  if (gufo_socket_is_fd(fd)) return gufo_socket_fcntl(fd, cmd, argument);
  if (HandleOf(fd) == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
  const int flags = FlagsOf(fd);
  switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
      if (argument < 0 || argument >= kMaxDescriptors) { errno = EINVAL; return -1; }
      std::vector<int> reserved;
      int result = -1;
      try {
        reserved.reserve(static_cast<size_t>(argument));
        do {
          result = dup(fd);
          if (result < 0 || result >= argument) break;
          reserved.push_back(result);
        } while (true);
      } catch (const std::bad_alloc&) {
        errno = ENOMEM;
      }
      const int error = errno;
      for (int held : reserved) close(held);
      errno = error;
      if (result >= argument && cmd == F_DUPFD_CLOEXEC)
        RememberFlags(result, FlagsOf(result) | O_CLOEXEC);
      return result >= argument ? result : -1;
    }
    case F_GETFD: return (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
    case F_SETFD:
      if (argument & ~FD_CLOEXEC) { errno = EINVAL; return -1; }
      RememberFlags(fd, (flags & ~O_CLOEXEC) | ((argument & FD_CLOEXEC) ? O_CLOEXEC : 0));
      return 0;
    case F_GETFL: return flags & ~O_CLOEXEC;
    case F_SETFL:
      if ((argument ^ flags) & ~(O_NONBLOCK | O_CLOEXEC)) { errno = ENOTSUP; return -1; }
      if (argument & O_NONBLOCK) {
        const DWORD type = GetFileType(HandleOf(fd));
        if (type != FILE_TYPE_DISK &&
            !(type == FILE_TYPE_PIPE && (flags & O_ACCMODE) == O_RDONLY)) {
          errno = ENOTSUP; return -1;
        }
      }
      try {
        DescriptionOf(fd)->status.store(argument & O_NONBLOCK, std::memory_order_relaxed);
      } catch (const std::bad_alloc&) { errno = ENOMEM; return -1; }
      return 0;
    default: errno = EINVAL; return -1;
  }
}

int dup(int fd) {
  if (gufo_socket_is_fd(fd)) { errno = ENOTSUP; return -1; }
  if (HandleOf(fd) == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
  try {
    const auto state = DescriptionOf(fd);
    const int flags = FlagsOf(fd) & ~O_CLOEXEC;
    const int copy = _dup(fd);
    if (copy < 0) return -1;
    try {
      std::lock_guard lock(g_fd_mutex);
      g_fd_flags[copy] = flags;
      g_descriptions[copy] = state;
    } catch (...) { _close(copy); throw; }
    return copy;
  } catch (const std::bad_alloc&) { errno = ENOMEM; return -1; }
}

int dup2(int fd, int fd2) {
  if (gufo_socket_is_fd(fd) || gufo_socket_is_fd(fd2)) { errno = ENOTSUP; return -1; }
  if (HandleOf(fd) == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
  if (fd == fd2) return fd2;
  if (fd2 < 0 || fd2 >= kMaxDescriptors) { errno = EBADF; return -1; }
  try {
    const auto state = DescriptionOf(fd);
    const int flags = FlagsOf(fd) & ~O_CLOEXEC;
    if (_dup2(fd, fd2) != 0) return -1;
    ForgetFlags(fd2);
    std::lock_guard lock(g_fd_mutex);
    g_fd_flags[fd2] = flags;
    g_descriptions[fd2] = state;
    return fd2;
  } catch (const std::bad_alloc&) { errno = ENOMEM; return -1; }
}

int pipe(int fds[2]) {
  HANDLE read_end = nullptr;
  HANDLE write_end = nullptr;
  if (!CreatePipe(&read_end, &write_end, nullptr, 1 << 20)) {
    return Fail(GetLastError());
  }
  fds[0] = WrapHandle(read_end, O_RDONLY);
  fds[1] = WrapHandle(write_end, O_WRONLY);
  if (fds[0] < 0 || fds[1] < 0) {
    const int error = errno;
    if (fds[0] >= 0) close(fds[0]);
    if (fds[1] >= 0) close(fds[1]);
    errno = error;
    return -1;
  }
  return 0;
}

int unlink(const char* path) {
  const std::wstring wide = Widen(path);
  if (DeleteFileW(wide.c_str())) {
    return 0;
  }
  const DWORD error = GetLastError();
  if (error == ERROR_ACCESS_DENIED) {
    const DWORD attributes = GetFileAttributesW(wide.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
      errno = EISDIR;
      return -1;
    }
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_READONLY)) {
      SetFileAttributesW(wide.c_str(),
                         attributes & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));
      if (DeleteFileW(wide.c_str())) {
        return 0;
      }
    }
  }
  return Fail(error);
}

int unlinkat(int dirfd, const char* path, int flags) {
  const std::wstring resolved = ResolveAt(dirfd, path);
  const std::string narrow = Narrow(resolved.c_str());
  return (flags & 0x200 /* AT_REMOVEDIR */) ? rmdir(narrow.c_str())
                                            : unlink(narrow.c_str());
}

int rmdir(const char* path) {
  if (!RemoveDirectoryW(Widen(path).c_str())) {
    return Fail(GetLastError());
  }
  return 0;
}

int mkdir(const char* path, mode_t) {
  if (!CreateDirectoryW(Widen(path).c_str(), nullptr)) {
    return Fail(GetLastError());
  }
  return 0;
}

int access(const char* path, int mode) {
  const DWORD attributes = GetFileAttributesW(Widen(path).c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    return Fail(GetLastError());
  }
  if ((mode & W_OK) && (attributes & FILE_ATTRIBUTE_READONLY) &&
      !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
    errno = EACCES;
    return -1;
  }
  return 0;
}

int isatty(int fd) { return _isatty(fd); }

pid_t getpid(void) { return static_cast<pid_t>(GetCurrentProcessId()); }
uid_t getuid(void) { return 0; }
uid_t geteuid(void) { return 0; }
gid_t getgid(void) { return 0; }

long sysconf(int name) {
  switch (name) {
    case _SC_PAGESIZE:
      return static_cast<long>(SystemInfo().dwPageSize);
    case _SC_NPROCESSORS_CONF:
    case _SC_NPROCESSORS_ONLN:
      return static_cast<long>(
          GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    case _SC_PHYS_PAGES:
    case _SC_AVPHYS_PAGES: {
      MEMORYSTATUSEX status{};
      status.dwLength = sizeof(status);
      if (!GlobalMemoryStatusEx(&status)) {
        return -1;
      }
      const auto bytes = name == _SC_PHYS_PAGES ? status.ullTotalPhys
                                                : status.ullAvailPhys;
      return static_cast<long>(bytes / SystemInfo().dwPageSize);
    }
    default:
      errno = EINVAL;
      return -1;
  }
}

unsigned int sleep(unsigned int seconds) {
  Sleep(seconds * 1000U);
  return 0;
}

int usleep(unsigned long long microseconds) {
  Sleep(static_cast<DWORD>((microseconds + 999) / 1000));
  return 0;
}

int fstat(int fd, struct stat* st) {
  const HANDLE handle = HandleOf(fd);
  if (handle == INVALID_HANDLE_VALUE) {
    errno = EBADF;
    return -1;
  }
  return StatHandle(handle, st, (FlagsOf(fd) & O_NOFOLLOW) != 0);
}

int stat(const char* path, struct stat* st) {
  return StatPath(Widen(path), st, false);
}

int lstat(const char* path, struct stat* st) {
  return StatPath(Widen(path), st, true);
}

int fstatat(int dirfd, const char* path, struct stat* st, int flags) {
  const std::wstring resolved = ResolveAt(dirfd, path);
  return StatPath(resolved, st, (flags & 0x100 /* AT_SYMLINK_NOFOLLOW */) != 0);
}

int fchmod(int fd, mode_t) {
  if (HandleOf(fd) == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
  errno = ENOTSUP; return -1;
}
int chmod(const char*, mode_t) { errno = ENOTSUP; return -1; }

void* mmap(void* addr, size_t length, int prot, int flags, int fd,
           off_t offset) {
  const size_t page = SystemInfo().dwPageSize;
  if (!length || offset < 0 || (static_cast<unsigned long long>(offset) % page) ||
      (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) ||
      ((flags & (MAP_SHARED | MAP_PRIVATE)) != MAP_SHARED &&
       (flags & (MAP_SHARED | MAP_PRIVATE)) != MAP_PRIVATE)) {
    errno = EINVAL; return MAP_FAILED;
  }
  if (addr || (flags & ~(MAP_SHARED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE)) ||
      ((flags & MAP_ANONYMOUS) && ((flags & MAP_SHARED) || offset))) {
    errno = ENOTSUP; return MAP_FAILED;
  }
  if (length > SIZE_MAX - page || static_cast<unsigned long long>(offset) >
      static_cast<unsigned long long>(LLONG_MAX) - length) {
    errno = EOVERFLOW; return MAP_FAILED;
  }
  const bool writable = (prot & PROT_WRITE) != 0;
  const bool executable = (prot & PROT_EXEC) != 0;
  if (flags & MAP_ANONYMOUS) {
    const DWORD protect = executable ? (writable ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ)
        : writable ? PAGE_READWRITE : (prot & PROT_READ) ? PAGE_READONLY : PAGE_NOACCESS;
    void* memory = VirtualAlloc(nullptr, length, MEM_RESERVE | MEM_COMMIT, protect);
    if (!memory) { Fail(GetLastError()); return MAP_FAILED; }
    if (!RememberMapping(memory, memory, length, true)) return MAP_FAILED;
    return memory;
  }
  const HANDLE file = HandleOf(fd);
  if (file == INVALID_HANDLE_VALUE) { errno = EBADF; return MAP_FAILED; }
  const bool copy_on_write = writable && (flags & MAP_PRIVATE);
  const DWORD protect = executable
      ? (copy_on_write ? PAGE_EXECUTE_WRITECOPY : writable ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ)
      : (copy_on_write ? PAGE_WRITECOPY : writable ? PAGE_READWRITE : PAGE_READONLY);
  const DWORD access = (copy_on_write ? FILE_MAP_COPY : writable ? FILE_MAP_WRITE : FILE_MAP_READ)
      | (executable ? FILE_MAP_EXECUTE : 0);
  const HANDLE mapping = CreateFileMappingW(file, nullptr, protect, 0, 0, nullptr);
  if (!mapping) { Fail(GetLastError()); return MAP_FAILED; }
  const unsigned long long granularity = SystemInfo().dwAllocationGranularity;
  const unsigned long long aligned =
      static_cast<unsigned long long>(offset) / granularity * granularity;
  const size_t skew = static_cast<size_t>(offset - aligned);
  if (length > SIZE_MAX - skew) {
    CloseHandle(mapping); errno = EOVERFLOW; return MAP_FAILED;
  }
  void* view = MapViewOfFile(mapping, access, static_cast<DWORD>(aligned >> 32),
                             static_cast<DWORD>(aligned), length + skew);
  const DWORD error = GetLastError();
  CloseHandle(mapping);
  if (!view) { Fail(error); return MAP_FAILED; }
  void* result = static_cast<char*>(view) + skew;
  if (prot == PROT_NONE) {
    DWORD old;
    if (!VirtualProtect(result, length, PAGE_NOACCESS, &old)) {
      const DWORD protection_error = GetLastError();
      UnmapViewOfFile(view); Fail(protection_error); return MAP_FAILED;
    }
  }
  HANDLE flush_file = INVALID_HANDLE_VALUE;
  if (writable && (flags & MAP_SHARED) &&
      !DuplicateHandle(GetCurrentProcess(), file, GetCurrentProcess(),
                       &flush_file, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
    const DWORD duplicate_error = GetLastError();
    UnmapViewOfFile(view); Fail(duplicate_error); return MAP_FAILED;
  }
  if (!RememberMapping(result, view, length, false, flush_file)) return MAP_FAILED;
  if ((flags & MAP_POPULATE) && madvise(result, length, MADV_POPULATE_READ) != 0) {
    const int saved = errno;
    munmap(result, length); errno = saved; return MAP_FAILED;
  }
  return result;
}

int munmap(void* addr, size_t length) {
  if (!addr || !length) { errno = EINVAL; return -1; }
  std::lock_guard lock(g_mapping_mutex);
  const auto found = g_mappings.find(addr);
  if (found == g_mappings.end() || found->second.length != length) {
    errno = ENOTSUP; return -1;  // Never free a whole view for a partial request.
  }
  const Mapping& mapping = found->second;
  const BOOL ok = mapping.anonymous ? VirtualFree(mapping.base, 0, MEM_RELEASE)
                                    : UnmapViewOfFile(mapping.base);
  if (!ok) return Fail(GetLastError());
  if (mapping.flush_file != INVALID_HANDLE_VALUE) CloseHandle(mapping.flush_file);
  g_mappings.erase(found);
  return 0;
}

int madvise(void* addr, size_t length, int advice) {
  if (length == 0) {
    return 0;
  }
  switch (advice) {
    case MADV_WILLNEED: {
      WIN32_MEMORY_RANGE_ENTRY range{addr, length};
      if (!PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0))
        return Fail(GetLastError());
      return 0;
    }
    case MADV_POPULATE_READ: {
      // Queue one large read, then fault every page so the call, like Linux,
      // returns only once the range is resident.
      WIN32_MEMORY_RANGE_ENTRY range{addr, length};
      (void)PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0);
      const size_t page = SystemInfo().dwPageSize;
      const auto* bytes = static_cast<const volatile char*>(addr);
      __try {
        for (size_t i = 0; i < length; i += page) {
          (void)bytes[i];
        }
        (void)bytes[length - 1];
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        errno = EFAULT;
        return -1;
      }
      return 0;
    }
    case MADV_DONTNEED:
    case MADV_FREE:
    case MADV_POPULATE_WRITE:
    case MADV_DONTDUMP:
    case MADV_HUGEPAGE:
    case MADV_NOHUGEPAGE:
      errno = ENOTSUP;
      return -1;
    case MADV_NORMAL:
    case MADV_RANDOM:
    case MADV_SEQUENTIAL:
      return 0;  // These are nonbinding access-pattern hints.
    default:
      errno = EINVAL;
      return -1;
  }
}

int posix_madvise(void* addr, size_t length, int advice) {
  return madvise(addr, length, advice) == 0 ? 0 : errno;
}

int mincore(void* addr, size_t length, unsigned char* vec) {
  const size_t page = SystemInfo().dwPageSize;
  if ((reinterpret_cast<uintptr_t>(addr) % page) || length > SIZE_MAX - page ||
      (!vec && length)) { errno = EINVAL; return -1; }
  const auto base = reinterpret_cast<uintptr_t>(addr) & ~(page - 1);
  const size_t pages =
      (reinterpret_cast<uintptr_t>(addr) - base + length + page - 1) / page;
  constexpr size_t kBatch = 4096;
  PSAPI_WORKING_SET_EX_INFORMATION info[kBatch];
  for (size_t done = 0; done < pages; done += kBatch) {
    const size_t count = pages - done < kBatch ? pages - done : kBatch;
    for (size_t i = 0; i < count; ++i) {
      info[i].VirtualAddress = reinterpret_cast<void*>(base + (done + i) * page);
    }
    if (!QueryWorkingSetEx(GetCurrentProcess(), info,
                           static_cast<DWORD>(count * sizeof(info[0])))) {
      return Fail(GetLastError());
    }
    for (size_t i = 0; i < count; ++i) {
      vec[done + i] = info[i].VirtualAttributes.Valid ? 1 : 0;
    }
  }
  return 0;
}

int mlock(const void* addr, size_t length) {
  return VirtualLock(const_cast<void*>(addr), length) ? 0 : Fail(GetLastError());
}
int munlock(const void* addr, size_t length) {
  return VirtualUnlock(const_cast<void*>(addr), length) ? 0 : Fail(GetLastError());
}

int msync(void* addr, size_t length, int flags) {
  if ((flags & ~(MS_SYNC | MS_ASYNC | MS_INVALIDATE)) ||
      ((flags & MS_SYNC) && (flags & MS_ASYNC))) { errno = EINVAL; return -1; }
  if (flags & MS_INVALIDATE) { errno = ENOTSUP; return -1; }
  std::lock_guard lock(g_mapping_mutex);
  const uintptr_t start = reinterpret_cast<uintptr_t>(addr);
  for (const auto& entry : g_mappings) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(entry.first);
    const Mapping& mapping = entry.second;
    if (start < base || start - base > mapping.length ||
        length > mapping.length - (start - base)) continue;
    if (mapping.anonymous) return 0;  // No backing file to synchronize.
    if (!FlushViewOfFile(addr, length)) return Fail(GetLastError());
    if ((flags & MS_SYNC) && mapping.flush_file != INVALID_HANDLE_VALUE &&
        !FlushFileBuffers(mapping.flush_file)) return Fail(GetLastError());
    return 0;
  }
  errno = ENOMEM;
  return -1;
}

int flock(int fd, int operation) {
  const HANDLE handle = HandleOf(fd);
  if (handle == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
  const int mode = operation & ~LOCK_NB;
  if ((operation & ~(LOCK_SH | LOCK_EX | LOCK_UN | LOCK_NB)) ||
      (mode != LOCK_SH && mode != LOCK_EX && mode != LOCK_UN)) {
    errno = EINVAL; return -1;
  }
  if (IsDirectoryHandle(handle) || GetFileType(handle) != FILE_TYPE_DISK) {
    errno = ENOTSUP; return -1;
  }
  try {
    const auto state = DescriptionOf(fd);
    std::lock_guard lock(state->mutex);
    if (mode == state->lock_mode) return 0;
    OVERLAPPED ov{};
    ov.Offset = kLockOffsetLow;
    ov.OffsetHigh = kLockOffsetHigh;
    if (state->lock_handle == INVALID_HANDLE_VALUE) {
      state->lock_handle = ReOpenFile(handle, AccessFor(FlagsOf(fd)),
          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_FLAG_OVERLAPPED);
      if (state->lock_handle == INVALID_HANDLE_VALUE) return Fail(GetLastError());
    }
    if (state->lock_mode) {
      if (!UnlockFileEx(state->lock_handle, 0, 1, 0, &ov)) return Fail(GetLastError());
      state->lock_mode = 0;  // Conversions are non-atomic, like flock on some POSIX systems.
    }
    if (mode == LOCK_UN) return 0;
    ov.hEvent = ThreadEvent();
    if (!ov.hEvent) return Fail(GetLastError());
    ResetEvent(ov.hEvent);
    const DWORD flags = (mode == LOCK_EX ? LOCKFILE_EXCLUSIVE_LOCK : 0) |
                        (operation & LOCK_NB ? LOCKFILE_FAIL_IMMEDIATELY : 0);
    if (!LockFileEx(state->lock_handle, flags, 0, 1, 0, &ov)) {
      DWORD error = GetLastError();
      if (error == ERROR_IO_PENDING) {
        DWORD done;
        if (GetOverlappedResult(state->lock_handle, &ov, &done, TRUE)) {
          state->lock_mode = mode; return 0;
        }
        error = GetLastError();
      }
      if (error == ERROR_LOCK_VIOLATION) { errno = EWOULDBLOCK; return -1; }
      return Fail(error);
    }
    state->lock_mode = mode;
    return 0;
  } catch (const std::bad_alloc&) { errno = ENOMEM; return -1; }
}

static bool FillTemplate(char* path_template) {
  const size_t n = strlen(path_template);
  if (n < 6 || strcmp(path_template + n - 6, "XXXXXX") != 0) {
    errno = EINVAL;
    return false;
  }
  static constexpr char kAlphabet[] =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  thread_local std::mt19937_64 random{std::random_device{}() ^
                                      GetCurrentThreadId()};
  for (size_t i = n - 6; i < n; ++i) {
    path_template[i] = kAlphabet[random() % (sizeof(kAlphabet) - 1)];
  }
  return true;
}

int mkstemp(char* path_template) {
  const size_t n = strlen(path_template);
  std::string original(path_template, n);
  for (int attempt = 0; attempt < 100; ++attempt) {
    memcpy(path_template, original.data(), n);
    if (!FillTemplate(path_template)) {
      return -1;
    }
    const int fd = open(path_template, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0 || errno != EEXIST) {
      return fd;
    }
  }
  errno = EEXIST;
  return -1;
}

char* mkdtemp(char* path_template) {
  const size_t n = strlen(path_template);
  std::string original(path_template, n);
  for (int attempt = 0; attempt < 100; ++attempt) {
    memcpy(path_template, original.data(), n);
    if (!FillTemplate(path_template)) {
      return nullptr;
    }
    if (mkdir(path_template, 0700) == 0) {
      return path_template;
    }
    if (errno != EEXIST) {
      return nullptr;
    }
  }
  errno = EEXIST;
  return nullptr;
}

int setenv(const char* name, const char* value, int overwrite) {
  if (!name || !*name || strchr(name, '=') || !value) {
    errno = EINVAL; return -1;
  }
  if (!overwrite && getenv(name)) return 0;
  // UCRT cannot represent an empty value in getenv(); fail instead of deleting.
  if (!*value) { errno = ENOTSUP; return -1; }
  const int error = _putenv_s(name, value);
  if (error) { errno = error; return -1; }
  return 0;
}

int unsetenv(const char* name) {
  if (!name || !*name || strchr(name, '=')) { errno = EINVAL; return -1; }
  const int error = _putenv_s(name, "");
  if (error) { errno = error; return -1; }
  return 0;
}

int chdir(const char* path) {
  if (!path) { errno = EINVAL; return -1; }
  try {
    return SetCurrentDirectoryW(Widen(path).c_str()) ? 0 : Fail(GetLastError());
  } catch (const std::bad_alloc&) { errno = ENOMEM; return -1; }
}

char* getcwd(char* buffer, size_t size) {
  try {
    const DWORD count = GetCurrentDirectoryW(0, nullptr);
    if (!count) { Fail(GetLastError()); return nullptr; }
    std::wstring wide(count, L'\0');
    if (!GetCurrentDirectoryW(count, wide.data())) { Fail(GetLastError()); return nullptr; }
    const std::string path = Narrow(wide.c_str());
    if (!buffer) {
      if (size && size <= path.size()) { errno = ERANGE; return nullptr; }
      buffer = static_cast<char*>(malloc(size ? size : path.size() + 1));
      if (!buffer) { errno = ENOMEM; return nullptr; }
    } else if (size <= path.size()) { errno = ERANGE; return nullptr; }
    memcpy(buffer, path.c_str(), path.size() + 1);
    return buffer;
  } catch (const std::bad_alloc&) { errno = ENOMEM; return nullptr; }
}

char* realpath(const char* path, char* resolved) {
  if (!path) { errno = EINVAL; return nullptr; }
  try {
    const std::wstring wide = Widen(path);
    const HANDLE file = CreateFileW(wide.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) { Fail(GetLastError()); return nullptr; }
    const DWORD count = GetFinalPathNameByHandleW(file, nullptr, 0, FILE_NAME_NORMALIZED);
    if (!count) { const DWORD error = GetLastError(); CloseHandle(file); Fail(error); return nullptr; }
    std::vector<wchar_t> name;
    try { name.resize(static_cast<size_t>(count) + 1); }
    catch (...) { CloseHandle(file); throw; }
    const DWORD done = GetFinalPathNameByHandleW(file, name.data(),
        static_cast<DWORD>(name.size()), FILE_NAME_NORMALIZED);
    const DWORD error = GetLastError();
    CloseHandle(file);
    if (!done || done >= name.size()) { Fail(done ? ERROR_INSUFFICIENT_BUFFER : error); return nullptr; }
    std::string result = Narrow(name.data());
    if (result.compare(0, 8, "\\\\?\\UNC\\") == 0) result = "\\\\" + result.substr(8);
    else if (result.compare(0, 4, "\\\\?\\") == 0) result.erase(0, 4);
    if (result.size() >= 32768) { errno = ENAMETOOLONG; return nullptr; }
    if (!resolved) resolved = static_cast<char*>(malloc(result.size() + 1));
    if (!resolved) { errno = ENOMEM; return nullptr; }
    memcpy(resolved, result.c_str(), result.size() + 1);
    return resolved;
  } catch (const std::bad_alloc&) { errno = ENOMEM; return nullptr; }
}

char* strndup(const char* text, size_t limit) {
  const size_t size = strnlen(text, limit);
  char* result = static_cast<char*>(malloc(size + 1));
  if (!result) { errno = ENOMEM; return nullptr; }
  memcpy(result, text, size);
  result[size] = '\0';
  return result;
}

struct tm* gmtime_r(const time_t* timer, struct tm* result) {
  return gmtime_s(result, timer) == 0 ? result : nullptr;
}

struct tm* localtime_r(const time_t* timer, struct tm* result) {
  return localtime_s(result, timer) == 0 ? result : nullptr;
}

time_t timegm(struct tm* tm) { return _mkgmtime(tm); }

int clock_gettime(clockid_t clock, struct timespec* ts) {
  if (!ts) { errno = EFAULT; return -1; }
  switch (clock) {
    case CLOCK_REALTIME: {
      FILETIME ft;
      GetSystemTimePreciseAsFileTime(&ft);
      *ts = TimespecFromFiletime(ft);
      return 0;
    }
    case CLOCK_PROCESS_CPUTIME_ID:
    case CLOCK_THREAD_CPUTIME_ID: {
      FILETIME creation, exit, kernel, user;
      const BOOL ok =
          clock == CLOCK_PROCESS_CPUTIME_ID
              ? GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel,
                                &user)
              : GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel,
                               &user);
      if (!ok) {
        return Fail(GetLastError());
      }
      ULARGE_INTEGER k, u;
      k.LowPart = kernel.dwLowDateTime;
      k.HighPart = kernel.dwHighDateTime;
      u.LowPart = user.dwLowDateTime;
      u.HighPart = user.dwHighDateTime;
      const unsigned long long ticks = k.QuadPart + u.QuadPart;
      ts->tv_sec = static_cast<time_t>(ticks / 10000000ULL);
      ts->tv_nsec = static_cast<long>((ticks % 10000000ULL) * 100ULL);
      return 0;
    }
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_BOOTTIME: {
      static const long long frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
      }();
      LARGE_INTEGER counter;
      QueryPerformanceCounter(&counter);
      ts->tv_sec = static_cast<time_t>(counter.QuadPart / frequency);
      ts->tv_nsec = static_cast<long>((counter.QuadPart % frequency) *
                                      1000000000LL / frequency);
      return 0;
    }
    default: errno = EINVAL; return -1;
  }
}

int getrusage(int who, struct rusage* usage) {
  if (who != RUSAGE_SELF && who != RUSAGE_THREAD) { errno = ENOTSUP; return -1; }
  memset(usage, 0, sizeof(*usage));
  FILETIME creation, exit, kernel, user;
  const BOOL ok = who == RUSAGE_THREAD
                      ? GetThreadTimes(GetCurrentThread(), &creation, &exit,
                                       &kernel, &user)
                      : GetProcessTimes(GetCurrentProcess(), &creation, &exit,
                                        &kernel, &user);
  if (!ok) return Fail(GetLastError());
  if (ok) {
    auto convert = [](const FILETIME& ft, gufo_rusage_time* out) {
      ULARGE_INTEGER value;
      value.LowPart = ft.dwLowDateTime;
      value.HighPart = ft.dwHighDateTime;
      out->tv_sec = static_cast<long long>(value.QuadPart / 10000000ULL);
      out->tv_usec = static_cast<long long>((value.QuadPart % 10000000ULL) / 10);
    };
    convert(user, &usage->ru_utime);
    convert(kernel, &usage->ru_stime);
  }
  PROCESS_MEMORY_COUNTERS counters{};
  counters.cb = sizeof(counters);
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
    usage->ru_maxrss = static_cast<long>(counters.PeakWorkingSetSize / 1024);
    // Windows does not separate minor from major faults.
    usage->ru_minflt = static_cast<long>(counters.PageFaultCount);
  }
  return 0;
}

int getrlimit(int resource, struct rlimit* limit) {
  if (resource != RLIMIT_NOFILE) { errno = ENOTSUP; return -1; }
  limit->rlim_cur = limit->rlim_max = static_cast<rlim_t>(kMaxDescriptors);
  return 0;
}

int setrlimit(int, const struct rlimit*) { errno = ENOTSUP; return -1; }

int uname(struct utsname* name) {
  memset(name, 0, sizeof(*name));
  strcpy_s(name->sysname, sizeof(name->sysname), "Windows");
#if defined(_M_ARM64) || defined(__aarch64__)
  strcpy_s(name->machine, sizeof(name->machine), "aarch64");
#elif defined(_M_X64) || defined(__x86_64__)
  strcpy_s(name->machine, sizeof(name->machine), "x86_64");
#else
  strcpy_s(name->machine, sizeof(name->machine), "unknown");
#endif
  DWORD size = sizeof(name->nodename);
  (void)GetComputerNameA(name->nodename, &size);
  using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  const auto get_version = ntdll ? reinterpret_cast<RtlGetVersionFn>(
                                       GetProcAddress(ntdll, "RtlGetVersion"))
                                 : nullptr;
  RTL_OSVERSIONINFOW info{};
  info.dwOSVersionInfoSize = sizeof(info);
  if (get_version != nullptr && get_version(&info) == 0) {
    snprintf(name->release, sizeof(name->release), "%lu.%lu.%lu",
             info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber);
    snprintf(name->version, sizeof(name->version), "Windows %lu.%lu build %lu",
             info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber);
  }
  return 0;
}

unsigned long long gufo_resident_bytes(void) {
  PROCESS_MEMORY_COUNTERS counters{};
  counters.cb = sizeof(counters);
  return GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))
             ? counters.WorkingSetSize
             : 0;
}

unsigned long long gufo_peak_resident_bytes(void) {
  PROCESS_MEMORY_COUNTERS counters{};
  counters.cb = sizeof(counters);
  return GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))
             ? counters.PeakWorkingSetSize
             : 0;
}

unsigned long long gufo_available_physical_bytes(void) {
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  return GlobalMemoryStatusEx(&status) ? status.ullAvailPhys : 0;
}

unsigned long long gufo_total_physical_bytes(void) {
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  return GlobalMemoryStatusEx(&status) ? status.ullTotalPhys : 0;
}

int renameat(int olddirfd, const char* oldpath, int newdirfd,
             const char* newpath) {
  const std::wstring from = ResolveAt(olddirfd, oldpath);
  const std::wstring to = ResolveAt(newdirfd, newpath);
  if (from.empty() || to.empty()) {
    errno = EBADF;
    return -1;
  }
  // POSIX-semantics rename replaces the target even while another handle
  // has it open, like rename(2). Fall back to MoveFileEx on older systems.
  const HANDLE source =
      CreateFileW(from.c_str(), 0x00010000L /* DELETE */ | SYNCHRONIZE,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING,
                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                  nullptr);
  if (source == INVALID_HANDLE_VALUE) {
    return Fail(GetLastError());
  }
  const size_t bytes = sizeof(FILE_RENAME_INFO) + to.size() * sizeof(wchar_t);
  std::string storage(bytes, '\0');
  auto* info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
  info->Flags = 0x1 /* FILE_RENAME_FLAG_REPLACE_IF_EXISTS */ |
                0x2 /* FILE_RENAME_FLAG_POSIX_SEMANTICS */;
  info->RootDirectory = nullptr;
  info->FileNameLength = static_cast<DWORD>(to.size() * sizeof(wchar_t));
  memcpy(info->FileName, to.c_str(), to.size() * sizeof(wchar_t));
  const BOOL renamed = SetFileInformationByHandle(
      source, static_cast<FILE_INFO_BY_HANDLE_CLASS>(22 /* FileRenameInfoEx */),
      info, static_cast<DWORD>(bytes));
  CloseHandle(source);
  if (renamed) {
    return 0;
  }
  if (!MoveFileExW(from.c_str(), to.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return Fail(GetLastError());
  }
  return 0;
}

static FILETIME FiletimeFromTimespec(const struct timespec& ts) {
  const unsigned long long ticks =
      static_cast<unsigned long long>(ts.tv_sec) * 10000000ULL +
      static_cast<unsigned long long>(ts.tv_nsec) / 100ULL +
      116444736000000000ULL;
  FILETIME ft;
  ft.dwLowDateTime = static_cast<DWORD>(ticks);
  ft.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
  return ft;
}

static int TouchHandle(HANDLE handle, const struct timespec times[2]) {
  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  FILETIME access = now;
  FILETIME modify = now;
  FILETIME* access_ptr = &access;
  FILETIME* modify_ptr = &modify;
  if (times != nullptr) {
    if (times[0].tv_nsec == UTIME_OMIT) {
      access_ptr = nullptr;
    } else if (times[0].tv_nsec != UTIME_NOW) {
      access = FiletimeFromTimespec(times[0]);
    }
    if (times[1].tv_nsec == UTIME_OMIT) {
      modify_ptr = nullptr;
    } else if (times[1].tv_nsec != UTIME_NOW) {
      modify = FiletimeFromTimespec(times[1]);
    }
  }
  if (!SetFileTime(handle, nullptr, access_ptr, modify_ptr)) {
    return Fail(GetLastError());
  }
  return 0;
}

int utimensat(int dirfd, const char* path, const struct timespec times[2],
              int flags) {
  const std::wstring resolved = ResolveAt(dirfd, path);
  DWORD attributes = FILE_FLAG_BACKUP_SEMANTICS;
  if (flags & AT_SYMLINK_NOFOLLOW) {
    attributes |= FILE_FLAG_OPEN_REPARSE_POINT;
  }
  const HANDLE handle =
      CreateFileW(resolved.c_str(), FILE_WRITE_ATTRIBUTES,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, attributes, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Fail(GetLastError());
  }
  const int result = TouchHandle(handle, times);
  CloseHandle(handle);
  return result;
}

int futimens(int fd, const struct timespec times[2]) {
  return TouchHandle(HandleOf(fd), times);
}

FILE* gufo_tmpfile(void) {
  wchar_t directory[MAX_PATH + 1];
  const DWORD n = GetTempPathW(MAX_PATH + 1, directory);
  if (n == 0 || n > MAX_PATH) {
    errno = ENOENT;
    return nullptr;
  }
  wchar_t name[MAX_PATH + 1];
  if (GetTempFileNameW(directory, L"gfo", 0, name) == 0) {
    Fail(GetLastError());
    return nullptr;
  }
  const HANDLE handle = CreateFileW(
      name, GENERIC_READ | GENERIC_WRITE | 0x00010000L /* DELETE */, 0,
      nullptr, CREATE_ALWAYS,
      FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    Fail(GetLastError());
    DeleteFileW(name);
    return nullptr;
  }
  const int fd =
      _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDWR | _O_BINARY);
  if (fd < 0) {
    CloseHandle(handle);
    errno = EMFILE;
    return nullptr;
  }
  FILE* stream = _fdopen(fd, "w+b");
  if (stream == nullptr) {
    _close(fd);
  }
  return stream;
}


int fileno(FILE* stream) { return gufo_fileno(stream); }

int dprintf(int fd, const char* format, ...) {
  va_list args;
  va_start(args, format);
  va_list copy;
  va_copy(copy, args);
  const int needed = vsnprintf(nullptr, 0, format, copy);
  va_end(copy);
  if (needed < 0) {
    va_end(args);
    return -1;
  }
  std::string text(static_cast<size_t>(needed) + 1, '\0');
  vsnprintf(text.data(), text.size(), format, args);
  va_end(args);
  size_t done = 0;
  while (done < static_cast<size_t>(needed)) {
    const ssize_t n = write(fd, text.data() + done, needed - done);
    if (n <= 0) {
      return -1;
    }
    done += static_cast<size_t>(n);
  }
  return needed;
}

int sched_yield(void) {
  SwitchToThread();
  return 0;
}

long syscall(long, ...) {
  errno = ENOSYS;
  return -1;
}

pid_t waitpid(pid_t pid, int* status, int options) {
  if (pid <= 0) { errno = ENOTSUP; return -1; }
  if (options & ~WNOHANG) { errno = EINVAL; return -1; }
  HANDLE process = nullptr;
  {
    std::lock_guard lock(g_child_mutex);
    const auto found = g_children.find(pid);
    if (found == g_children.end() || !found->second.process) { errno = ECHILD; return -1; }
    if (!DuplicateHandle(GetCurrentProcess(), found->second.process,
                         GetCurrentProcess(), &process, 0, FALSE, DUPLICATE_SAME_ACCESS))
      return Fail(GetLastError());
  }
  const DWORD waited = WaitForSingleObject(process, (options & WNOHANG) ? 0 : INFINITE);
  if (waited == WAIT_TIMEOUT) { CloseHandle(process); return 0; }
  if (waited != WAIT_OBJECT_0) {
    const DWORD error = GetLastError(); CloseHandle(process); return Fail(error);
  }
  DWORD code = 0;
  if (!GetExitCodeProcess(process, &code)) {
    const DWORD error = GetLastError(); CloseHandle(process); return Fail(error);
  }
  CloseHandle(process);
  std::lock_guard lock(g_child_mutex);
  const auto found = g_children.find(pid);
  if (found == g_children.end() || !found->second.process) { errno = ECHILD; return -1; }
  if (status) *status = found->second.signal ? found->second.signal
                                            : static_cast<int>((code & 0xff) << 8);
  CloseHandle(found->second.process);
  found->second.process = nullptr;
  // Group jobs outlive the shell: descendants may still hold its stdout pipe.
  if (!found->second.job) g_children.erase(found);
  return pid;
}

int kill(pid_t pid, int sig) {
  if (pid == 0 || pid == -1 || pid == INT_MIN) { errno = ENOTSUP; return -1; }
  if (sig != 0 && sig != SIGTERM && sig != SIGKILL && sig != SIGINT) {
    errno = ENOTSUP; return -1;
  }
  std::lock_guard lock(g_child_mutex);
  const auto found = g_children.find(pid < 0 ? -pid : pid);
  if (found == g_children.end()) { errno = ESRCH; return -1; }
  Child& child = found->second;
  if ((pid < 0 && !child.job) || (pid > 0 && !child.process)) {
    errno = ESRCH; return -1;
  }
  if (!sig) return 0;
  const BOOL ok = pid < 0 ? TerminateJobObject(child.job, 128U + sig)
                         : TerminateProcess(child.process, 128U + sig);
  if (!ok) return Fail(GetLastError());
  child.signal = sig;
  return 0;
}

typedef void (*Win32SignalHandler)(int);
static SRWLOCK signal_action_lock = SRWLOCK_INIT;
static Win32SignalHandler signal_actions[NSIG] = {};

static void DeliverPersistentSignal(int sig) {
  AcquireSRWLockExclusive(&signal_action_lock);
  const Win32SignalHandler handler =
      signal_actions[sig == SIGABRT_COMPAT ? SIGABRT : sig];
  /* UCRT clears a custom signal() action before invoking it. Serialize
   * rearming with sigaction changes so cancellation remains persistent and
   * cannot overwrite a concurrently installed ignore/default disposition. */
  if (handler != SIG_DFL && handler != SIG_IGN)
    signal(sig, DeliverPersistentSignal);
  ReleaseSRWLockExclusive(&signal_action_lock);
  if (handler == SIG_DFL) {
    raise(sig);
  } else if (handler != SIG_IGN) {
    handler(sig);
  }
}

int sigaction(int sig, const struct sigaction* action,
              struct sigaction* previous) {
  if (previous != nullptr) {
    memset(previous, 0, sizeof(*previous));
  }
  if (sig == SIGPIPE) {
    if (action && action->sa_handler != SIG_IGN && action->sa_handler != SIG_DFL) {
      errno = ENOTSUP; return -1;
    }
    return 0;  // Windows socket writes never deliver SIGPIPE.
  }
  if (!action || action->sa_flags || action->sa_mask ||
      sig == SIGHUP || sig == SIGCHLD || sig == SIGUSR1) {
    errno = ENOTSUP; return -1;
  }
  if (sig == SIGABRT_COMPAT) sig = SIGABRT;
  /* These CRT actions are process-wide. Hardware exception actions are
   * thread-local in UCRT and cannot provide the POSIX sigaction contract. */
  if (sig != SIGINT && sig != SIGBREAK && sig != SIGTERM && sig != SIGABRT) {
    errno = ENOTSUP; return -1;
  }
  const Win32SignalHandler handler = action->sa_handler;
  const Win32SignalHandler native_handler =
      handler == SIG_DFL || handler == SIG_IGN ? handler : DeliverPersistentSignal;
  AcquireSRWLockExclusive(&signal_action_lock);
  const Win32SignalHandler old_handler = signal_actions[sig];
  const auto old = signal(sig, native_handler);
  if (old != SIG_ERR) signal_actions[sig] = handler;
  ReleaseSRWLockExclusive(&signal_action_lock);
  if (old == SIG_ERR) {
    errno = EINVAL;
    return -1;
  }
  if (previous != nullptr) {
    previous->sa_handler = old == DeliverPersistentSignal ? old_handler : old;
  }
  return 0;
}

int sigemptyset(sigset_t* set) {
  *set = 0;
  return 0;
}

int sigfillset(sigset_t* set) {
  *set = ~0UL;
  return 0;
}

int sigaddset(sigset_t* set, int sig) {
  *set |= 1UL << (sig & 31);
  return 0;
}

int poll(struct pollfd* fds, unsigned long count, int timeout_ms) {
  if (count && !fds) { errno = EFAULT; return -1; }
  struct timespec start = {};
  if (timeout_ms > 0 && clock_gettime(CLOCK_MONOTONIC, &start)) return -1;
  for (;;) {
    for (unsigned long i = 0; i < count; ++i) fds[i].revents = 0;
    int ready = gufo_socket_poll(fds, count);
    if (ready < 0) return -1;
    for (unsigned long i = 0; i < count; ++i) {
      if (fds[i].fd < 0 || gufo_socket_is_fd(fds[i].fd)) continue;
      const HANDLE handle = HandleOf(fds[i].fd);
      if (handle == INVALID_HANDLE_VALUE) {
        fds[i].revents = POLLNVAL;
      } else {
        const DWORD type = GetFileType(handle);
        if (type == FILE_TYPE_PIPE) {
          if (fds[i].events & POLLIN) {
            DWORD available = 0;
            if (PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr))
              fds[i].revents = available ? POLLIN : 0;
            else if (GetLastError() == ERROR_BROKEN_PIPE)
              fds[i].revents = POLLHUP;
            else if ((FlagsOf(fds[i].fd) & O_ACCMODE) != O_WRONLY)
              return Fail(GetLastError());
          }
          if (fds[i].events & POLLOUT) { errno = ENOTSUP; return -1; }
        } else if (type == FILE_TYPE_CHAR) {
          DWORD mode = 0;
          if (GetConsoleMode(handle, &mode)) {
            DWORD events = 0;
            if ((fds[i].events & POLLIN) &&
                GetNumberOfConsoleInputEvents(handle, &events) && events)
              fds[i].revents |= POLLIN;
            if (fds[i].events & POLLOUT) fds[i].revents |= POLLOUT;
          } else fds[i].revents = fds[i].events & (POLLIN | POLLOUT);
        } else if (type == FILE_TYPE_DISK) {
          fds[i].revents = fds[i].events & (POLLIN | POLLOUT);
        } else fds[i].revents = POLLNVAL;
      }
      if (fds[i].revents) ++ready;
    }
    if (ready || !timeout_ms) return ready;
    if (timeout_ms > 0) {
      struct timespec now;
      if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
      const long long elapsed =
          (now.tv_sec - start.tv_sec) * 1000000000LL + now.tv_nsec - start.tv_nsec;
      if (elapsed >= static_cast<long long>(timeout_ms) * 1000000LL) return 0;
    }
    Sleep(1);
  }
}

}  // extern "C"

int open(const wchar_t* path, int flags, int mode) {
  if (path == nullptr) {
    errno = EFAULT;
    return -1;
  }
  return OpenPath(Backslashed(path), flags, static_cast<mode_t>(mode));
}

int openat(int dirfd, const wchar_t* path, int flags, int mode) {
  return openat(dirfd, Narrow(path).c_str(), flags, mode);
}

int stat(const wchar_t* path, struct stat* st) {
  return StatPath(Backslashed(path), st, false);
}

int lstat(const wchar_t* path, struct stat* st) {
  return StatPath(Backslashed(path), st, true);
}

int mkdir(const wchar_t* path, mode_t mode) {
  return mkdir(Narrow(path).c_str(), mode);
}

int rmdir(const wchar_t* path) { return rmdir(Narrow(path).c_str()); }

int unlink(const wchar_t* path) { return unlink(Narrow(path).c_str()); }

int chmod(const wchar_t*, mode_t) { errno = ENOTSUP; return -1; }

int utimensat(int dirfd, const wchar_t* path, const struct timespec times[2],
              int flags) {
  return utimensat(dirfd, Narrow(path).c_str(), times, flags);
}

int access(const wchar_t* path, int mode) {
  return access(Narrow(path).c_str(), mode);
}
