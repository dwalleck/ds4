// POSIX spawning over CreateProcessW. See include/spawn.h.
// The UCRT lpReserved2 block contains an int descriptor count, flag bytes,
// and unaligned intptr_t handles. Only handles in our explicit list inherit.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <errno.h>
#include <io.h>
#include <limits.h>
#include <spawn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// A null envp requests the current Win32 environment, including later changes.
extern "C" { char** environ = nullptr; }

struct gufo_spawn_action {
  int kind;  // 0 = dup2, 1 = close, 2 = open
  int fd;
  int target;
  char* path;
  int flags;
  mode_t mode;
};

// The snapshot holds the parent's descriptor lock while visiting every open
// descriptor, including CLOEXEC sources needed by ordered file actions.
extern "C" int gufo_spawn_fd_snapshot(
    int (*visit)(int, intptr_t, int, void*), void* context);
// Ownership transfers only on success.
extern "C" int gufo_register_child(pid_t pid, void* process, void* job);

namespace {
constexpr unsigned char kFopen = 0x01;
constexpr unsigned char kFpipe = 0x08;
constexpr unsigned char kFappend = 0x20;
constexpr unsigned char kFdev = 0x40;
constexpr int kMaxDescriptors =
    (USHRT_MAX - sizeof(int)) / (1 + sizeof(intptr_t));

int WindowsError(DWORD error) {
  switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND: return ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_ELEVATION_REQUIRED: return EACCES;
    case ERROR_BAD_EXE_FORMAT:
    case ERROR_EXE_MACHINE_TYPE_MISMATCH: return ENOEXEC;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY: return ENOMEM;
    case ERROR_TOO_MANY_OPEN_FILES: return EMFILE;
    case ERROR_INVALID_HANDLE: return EBADF;
    case ERROR_FILENAME_EXCED_RANGE: return ENAMETOOLONG;
    case ERROR_NO_UNICODE_TRANSLATION: return EILSEQ;
    default: return EINVAL;
  }
}

struct Handle {
  HANDLE value = nullptr;
  Handle() = default;
  explicit Handle(HANDLE handle) : value(handle) {}
  ~Handle() { if (value != nullptr) CloseHandle(value); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& other) noexcept : value(other.value) { other.value = nullptr; }
  Handle& operator=(Handle&& other) noexcept {
    if (value != nullptr) CloseHandle(value);
    value = other.value;
    other.value = nullptr;
    return *this;
  }
};

struct Descriptor {
  Handle handle;
  int open_flags;
};
using DescriptorTable = std::map<int, Descriptor>;

int SnapshotDescriptor(int fd, intptr_t handle, int open_flags, void* context) {
  HANDLE copy = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(handle),
                       GetCurrentProcess(), &copy, 0, FALSE,
                       DUPLICATE_SAME_ACCESS)) {
    return WindowsError(GetLastError());
  }
  Handle owned(copy);
  try {
    static_cast<DescriptorTable*>(context)->emplace(
        fd, Descriptor{std::move(owned), open_flags});
    return 0;
  } catch (const std::bad_alloc&) {
    return ENOMEM;
  }
}

bool ValidActionFd(int fd) { return fd >= 0 && fd < kMaxDescriptors; }

int Append(posix_spawn_file_actions_t* actions, gufo_spawn_action action) {
  if (actions == nullptr) return EINVAL;
  if (!ValidActionFd(action.fd) ||
      (action.kind == 0 && !ValidActionFd(action.target))) return EBADF;
  if (actions->count == actions->capacity) {
    if (actions->capacity > INT_MAX / 2) return ENOMEM;
    const int capacity = actions->capacity == 0 ? 8 : actions->capacity * 2;
    if (static_cast<size_t>(capacity) > SIZE_MAX / sizeof(gufo_spawn_action))
      return ENOMEM;
    auto* grown = static_cast<gufo_spawn_action*>(
        realloc(actions->actions, sizeof(gufo_spawn_action) * capacity));
    if (grown == nullptr) return ENOMEM;
    actions->actions = grown;
    actions->capacity = capacity;
  }
  actions->actions[actions->count++] = action;
  return 0;
}

int Widen(const char* utf8, std::wstring& out) {
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                       utf8, -1, nullptr, 0);
  if (count == 0) return WindowsError(GetLastError());
  out.resize(count);  // Include space for the terminator written by Win32.
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1,
                          out.data(), count) == 0)
    return WindowsError(GetLastError());
  out.resize(count - 1);
  return 0;
}

// The CRT parses argv[0] specially: quotes group spaces but backslashes are
// literal. A literal quote in argv[0] cannot be represented by this protocol.
int AppendProgramName(std::wstring& command, const std::wstring& name) {
  if (name.find(L'"') != std::wstring::npos) return EINVAL;
  command += L'"';
  command += name;
  command += L'"';
  return 0;
}

void AppendQuoted(std::wstring& command, const std::wstring& argument) {
  command += L" \"";
  size_t backslashes = 0;
  for (wchar_t ch : argument) {
    if (ch == L'\\') {
      ++backslashes;
      continue;
    }
    command.append(backslashes * (ch == L'"' ? 2 : 1), L'\\');
    if (ch == L'"') command += L'\\';
    command += ch;
    backslashes = 0;
  }
  command.append(backslashes * 2, L'\\');
  command += L'"';
}

int Environment(char* const envp[], std::vector<wchar_t>& block) {
  if (envp == nullptr) return 0;
  std::vector<std::wstring> entries;
  for (size_t i = 0; envp[i] != nullptr; ++i) {
    std::wstring entry;
    const int error = Widen(envp[i], entry);
    if (error != 0) return error;
    if (entry.empty()) return EINVAL;
    // Also accept Win32's hidden per-drive variables, e.g. =C:=C:\\work.
    const size_t separator = entry.find(L'=', entry[0] == L'=' ? 1 : 0);
    if (separator == std::wstring::npos || separator == 0) return EINVAL;
    entries.push_back(std::move(entry));
  }
  std::sort(entries.begin(), entries.end(),
            [](const std::wstring& a, const std::wstring& b) {
    const size_t a_name = a.find(L'=', a[0] == L'=' ? 1 : 0);
    const size_t b_name = b.find(L'=', b[0] == L'=' ? 1 : 0);
    return CompareStringOrdinal(a.data(), static_cast<int>(a_name),
                                b.data(), static_cast<int>(b_name), TRUE)
           == CSTR_LESS_THAN;
  });
  for (const auto& entry : entries) {
    block.insert(block.end(), entry.begin(), entry.end());
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  if (entries.empty()) block.push_back(L'\0');
  return 0;
}

int ResolveProgram(const std::wstring& file, bool search, std::wstring& program) {
  if (search && file.find_first_of(L"/\\:") == std::wstring::npos) {
    // POSIX spawnp searches the parent's PATH, not the supplied child envp.
    DWORD size = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    if (size == 0) return ENOENT;
    std::vector<wchar_t> path(size);
    DWORD length = GetEnvironmentVariableW(L"PATH", path.data(), size);
    if (length == 0 || length >= size) return ENOENT;
    size = SearchPathW(path.data(), file.c_str(), L".exe", 0, nullptr, nullptr);
    if (size == 0) return WindowsError(GetLastError());
    std::vector<wchar_t> resolved(size + 1);
    length = SearchPathW(path.data(), file.c_str(), L".exe",
                         static_cast<DWORD>(resolved.size()), resolved.data(),
                         nullptr);
    if (length == 0) return WindowsError(GetLastError());
    if (length >= resolved.size()) return ENAMETOOLONG;
    program.assign(resolved.data(), length);
    return 0;
  }
  // An explicit application name prevents argv[0] from selecting the program.
  // Make relative paths absolute so posix_spawn does not search Windows PATH.
  DWORD size = GetFullPathNameW(file.c_str(), 0, nullptr, nullptr);
  if (size == 0) return WindowsError(GetLastError());
  std::vector<wchar_t> resolved(size);
  const DWORD length = GetFullPathNameW(file.c_str(), size, resolved.data(), nullptr);
  if (length == 0) return WindowsError(GetLastError());
  if (length >= size) return ENAMETOOLONG;
  program.assign(resolved.data(), length);
  return 0;
}

struct AttributeList {
  LPPROC_THREAD_ATTRIBUTE_LIST value = nullptr;
  ~AttributeList() { if (value != nullptr) DeleteProcThreadAttributeList(value); }
};

int Spawn(pid_t* pid, const char* file,
          const posix_spawn_file_actions_t* actions,
          const posix_spawnattr_t* attributes, char* const argv[],
          char* const envp[], bool search) {
  if (file == nullptr || argv == nullptr || argv[0] == nullptr) return EINVAL;
  if (*file == '\0') return ENOENT;
  const bool grouped = attributes != nullptr &&
                       (attributes->flags & POSIX_SPAWN_SETPGROUP) != 0;
  if (attributes != nullptr &&
      ((attributes->flags & ~POSIX_SPAWN_SETPGROUP) != 0 ||
       attributes->pgroup != 0)) return ENOTSUP;

  std::wstring program, argument, command;
  int error = Widen(file, program);
  if (error != 0) return error;
  error = ResolveProgram(program, search, argument);
  if (error != 0) return error;
  program.swap(argument);
  error = Widen(argv[0], argument);
  if (error != 0) return error;
  error = AppendProgramName(command, argument);
  if (error != 0) return error;
  for (size_t i = 1; argv[i] != nullptr; ++i) {
    error = Widen(argv[i], argument);
    if (error != 0) return error;
    AppendQuoted(command, argument);
  }
  if (command.size() >= 32767) return E2BIG;
  std::vector<wchar_t> environment;
  error = Environment(envp, environment);
  if (error != 0) return error;

  DescriptorTable table;
  error = gufo_spawn_fd_snapshot(SnapshotDescriptor, &table);
  if (error != 0) return error;
  if (actions != nullptr) {
    if (actions->count < 0 || actions->count > actions->capacity ||
        (actions->count != 0 && actions->actions == nullptr)) return EINVAL;
    for (int i = 0; i < actions->count; ++i) {
      const gufo_spawn_action& action = actions->actions[i];
      if (!ValidActionFd(action.fd)) return EBADF;
      auto source = table.find(action.fd);
      if (action.kind == 1) {
        if (source == table.end()) return EBADF;
        table.erase(source);
      } else if (action.kind == 0) {
        if (!ValidActionFd(action.target) || source == table.end()) return EBADF;
        if (action.fd == action.target) {
          source->second.open_flags &= ~O_CLOEXEC;
          continue;
        }
        HANDLE copy = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), source->second.handle.value,
                             GetCurrentProcess(), &copy, 0, FALSE,
                             DUPLICATE_SAME_ACCESS))
          return WindowsError(GetLastError());
        Descriptor replacement{Handle(copy), source->second.open_flags & ~O_CLOEXEC};
        table.insert_or_assign(action.target, std::move(replacement));
      } else if (action.kind == 2) {
        // Use the shared POSIX layer for path and flag translation; only our
        // duplicate becomes a child descriptor.
        table.erase(action.fd);
        const int opened = open(action.path, action.flags, action.mode);
        if (opened < 0) return errno;
        HANDLE copy = nullptr;
        const BOOL duplicated = DuplicateHandle(
            GetCurrentProcess(), reinterpret_cast<HANDLE>(_get_osfhandle(opened)),
            GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS);
        const DWORD duplicate_error = GetLastError();
        close(opened);
        if (!duplicated) return WindowsError(duplicate_error);
        Descriptor replacement{Handle(copy), action.flags};
        table.insert_or_assign(action.fd, std::move(replacement));
      } else {
        return EINVAL;
      }
    }
  }
  for (auto it = table.begin(); it != table.end();) {
    if (it->second.open_flags & O_CLOEXEC) it = table.erase(it);
    else ++it;
  }
  const int count = table.empty() ? 0 : table.rbegin()->first + 1;
  if (count < 0 || count > kMaxDescriptors) return E2BIG;
  const size_t crt_size =
      sizeof(int) + static_cast<size_t>(count) * (1 + sizeof(intptr_t));
  const uint32_t metadata_count = static_cast<uint32_t>(table.size());
  const size_t reserved_size = crt_size + 12 + metadata_count * size_t{8};
  if (reserved_size > USHRT_MAX) return E2BIG;
  std::vector<unsigned char> reserved(reserved_size, 0);
  memcpy(reserved.data(), &count, sizeof(count));
  auto* flags = reserved.data() + sizeof(int);
  auto* handles = flags + count;
  std::vector<HANDLE> handle_list;
  handle_list.reserve(table.size());
  for (int fd = 0; fd < count; ++fd) {
    intptr_t value = -1;
    const auto it = table.find(fd);
    if (it != table.end()) {
      HANDLE handle = it->second.handle.value;
      if (!SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
        return WindowsError(GetLastError());
      handle_list.push_back(handle);
      value = reinterpret_cast<intptr_t>(handle);
      const DWORD type = GetFileType(handle);
      flags[fd] = kFopen | ((it->second.open_flags & O_APPEND) ? kFappend : 0) |
                  (type == FILE_TYPE_PIPE ? kFpipe
                   : type == FILE_TYPE_CHAR ? kFdev : 0);
    }
    memcpy(handles + static_cast<size_t>(fd) * sizeof(value), &value, sizeof(value));
  }
  // UCRT reads only the descriptor prefix. The ds4 POSIX layer recovers
  // access/status flags from this private trailer via GetStartupInfoW;
  // no environment variables or extra inherited handles are needed.
  // Layout: magic[8], uint32 entry_count, {int32 fd,uint32 open_flags}[].
  static constexpr unsigned char magic[8] = {'D', 'S', '4', 'F', 'D', 'V', '1', 0};
  unsigned char* metadata = reserved.data() + crt_size;
  memcpy(metadata, magic, sizeof(magic));
  metadata += sizeof(magic);
  memcpy(metadata, &metadata_count, sizeof(metadata_count));
  metadata += sizeof(metadata_count);
  for (const auto& entry : table) {
    const int32_t fd = entry.first;
    const uint32_t open_flags = static_cast<uint32_t>(entry.second.open_flags);
    memcpy(metadata, &fd, sizeof(fd));
    memcpy(metadata + sizeof(fd), &open_flags, sizeof(open_flags));
    metadata += sizeof(fd) + sizeof(open_flags);
  }

  std::vector<unsigned char> attribute_storage;
  AttributeList attribute_list;
  if (!handle_list.empty()) {
    SIZE_T size = 0;
    if (InitializeProcThreadAttributeList(nullptr, 1, 0, &size) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0)
      return EINVAL;
    attribute_storage.resize(size);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &size))
      return WindowsError(GetLastError());
    attribute_list.value = list;
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   handle_list.data(), handle_list.size() * sizeof(HANDLE),
                                   nullptr, nullptr))
      return WindowsError(GetLastError());
  }
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  auto std_handle = [&](int fd) {
    const auto it = table.find(fd);
    return it != table.end() ? it->second.handle.value : nullptr;
  };
  startup.StartupInfo.hStdInput = std_handle(0);
  startup.StartupInfo.hStdOutput = std_handle(1);
  startup.StartupInfo.hStdError = std_handle(2);
  startup.StartupInfo.cbReserved2 = static_cast<WORD>(reserved.size());
  startup.StartupInfo.lpReserved2 = reserved.data();
  startup.lpAttributeList = attribute_list.value;
  Handle job;
  if (grouped) {
    job.value = CreateJobObjectW(nullptr, nullptr);
    if (job.value == nullptr) return WindowsError(GetLastError());
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits)))
      return WindowsError(GetLastError());
  }
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr,
                       !handle_list.empty(),
                       EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT |
                           (grouped ? CREATE_SUSPENDED : 0),
                       envp != nullptr ? environment.data() : nullptr,
                       nullptr, &startup.StartupInfo, &process))
    return WindowsError(GetLastError());
  Handle thread(process.hThread);
  Handle child_process(process.hProcess);
  const pid_t child = static_cast<pid_t>(process.dwProcessId);
  auto abort_child = [&] {
    if (job.value != nullptr) TerminateJobObject(job.value, 127);
    TerminateProcess(process.hProcess, 127);
    WaitForSingleObject(process.hProcess, INFINITE);
  };
  if (grouped) {
    if (!AssignProcessToJobObject(job.value, process.hProcess)) {
      error = WindowsError(GetLastError());
      abort_child();
      return error;
    }
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
      error = WindowsError(GetLastError());
      abort_child();
      return error;
    }
  }
  error = gufo_register_child(child, process.hProcess, job.value);
  if (error != 0) {
    abort_child();
    return error;
  }
  child_process.value = nullptr;  // The registry now owns both handles.
  job.value = nullptr;
  if (pid != nullptr) *pid = child;
  return 0;
}

int SpawnNoThrow(pid_t* pid, const char* file,
                 const posix_spawn_file_actions_t* actions,
                 const posix_spawnattr_t* attributes, char* const argv[],
                 char* const envp[], bool search) noexcept {
  try {
    return Spawn(pid, file, actions, attributes, argv, envp, search);
  } catch (const std::bad_alloc&) {
    return ENOMEM;
  } catch (const std::length_error&) {
    return E2BIG;
  }
}
}  // namespace

extern "C" {
int posix_spawn_file_actions_init(posix_spawn_file_actions_t* actions) {
  if (actions == nullptr) return EINVAL;
  actions->count = 0;
  actions->capacity = 0;
  actions->actions = nullptr;
  return 0;
}

int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t* actions) {
  if (actions == nullptr) return EINVAL;
  for (int i = 0; i < actions->count; ++i) free(actions->actions[i].path);
  free(actions->actions);
  actions->actions = nullptr;
  actions->count = 0;
  actions->capacity = 0;
  return 0;
}

int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t* actions,
                                    int fd, int target) {
  return Append(actions, {0, fd, target, nullptr, 0, 0});
}

int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t* actions, int fd) {
  return Append(actions, {1, fd, -1, nullptr, 0, 0});
}

int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t* actions, int fd,
                                    const char* path, int flags, mode_t mode) {
  if (actions == nullptr || path == nullptr) return EINVAL;
  if (!ValidActionFd(fd)) return EBADF;
  const size_t size = strlen(path) + 1;
  auto* copy = static_cast<char*>(malloc(size));
  if (copy == nullptr) return ENOMEM;
  memcpy(copy, path, size);
  const int error = Append(actions, {2, fd, -1, copy, flags, mode});
  if (error != 0) free(copy);
  return error;
}

int posix_spawnattr_init(posix_spawnattr_t* attributes) {
  if (attributes == nullptr) return EINVAL;
  attributes->flags = 0;
  attributes->pgroup = 0;
  return 0;
}

int posix_spawnattr_destroy(posix_spawnattr_t* attributes) {
  return attributes != nullptr ? 0 : EINVAL;
}

int posix_spawnattr_setflags(posix_spawnattr_t* attributes, short flags) {
  if (attributes == nullptr) return EINVAL;
  if ((flags & ~POSIX_SPAWN_SETPGROUP) != 0) return ENOTSUP;
  attributes->flags = flags;
  return 0;
}

int posix_spawnattr_getflags(const posix_spawnattr_t* attributes, short* flags) {
  if (attributes == nullptr || flags == nullptr) return EINVAL;
  *flags = static_cast<short>(attributes->flags);
  return 0;
}

int posix_spawnattr_setpgroup(posix_spawnattr_t* attributes, pid_t pgroup) {
  if (attributes == nullptr) return EINVAL;
  if (pgroup != 0) return ENOTSUP;
  attributes->pgroup = pgroup;
  return 0;
}

int posix_spawnattr_getpgroup(const posix_spawnattr_t* attributes, pid_t* pgroup) {
  if (attributes == nullptr || pgroup == nullptr) return EINVAL;
  *pgroup = attributes->pgroup;
  return 0;
}

int posix_spawnp(pid_t* pid, const char* file,
                 const posix_spawn_file_actions_t* actions,
                 const posix_spawnattr_t* attributes, char* const argv[],
                 char* const envp[]) {
  return SpawnNoThrow(pid, file, actions, attributes, argv, envp, true);
}

int posix_spawn(pid_t* pid, const char* path,
                const posix_spawn_file_actions_t* actions,
                const posix_spawnattr_t* attributes, char* const argv[],
                char* const envp[]) {
  return SpawnNoThrow(pid, path, actions, attributes, argv, envp, false);
}
}  // extern "C"
