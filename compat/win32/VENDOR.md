# Windows compatibility provenance

## Imported source

Primary source: [pixmaate/gufo](https://github.com/pixmaate/gufo), exact commit
[`2eb0778ac15d9e1c18c5b2f9e5dbbecc19e20ec2`](https://github.com/pixmaate/gufo/tree/2eb0778ac15d9e1c18c5b2f9e5dbbecc19e20ec2/compat/win32).
The GitHub recursive Git tree API identified the `compat/win32` tree as
`61d8b178f9b4592389c12b8ad72b3df9b3b2e222`. Every blob under that directory was
fetched from the commit-pinned raw URL, except its deliberately unusable
`include/pthread.h` stub. The separate native C pthread implementation replaces
that stub; it is not imported from bubo.

`LICENSE` is the full upstream MIT license, Copyright (c) 2026 gufo
contributors (upstream Git blob `a39b172b11fc6a6f34cd57d02f5f622491c0b4eb`).
`NOTICE` is the unmodified upstream root notice (Git blob
`2ccc00d52ce6f88310b898e8ee7e63b61c55a2da`). Its descriptions of other gufo
components do not mean those components were imported here. The unused
HIP-specific `hip_include/cmath` was removed after native C/CUDA verification;
neither Windows build uses that include directory.

The user approved a temporary exception for the vendored C++ implementation.
DS4-owned engine code, platform helpers, pthread implementation, and tests remain
C. Public compatibility interfaces use the C ABI; wide-path overloads from the
upstream vendor remain C++-only.

## Downstream comparison

Reference checkout: `C:/Users/dwall/repos/bubo`, actual HEAD
`1d744add40a424bb1e31445e0f8e06798d05ae6e`, recorded using `git rev-parse HEAD`.
A content comparison was performed between every imported pinned blob and the
corresponding downstream file, not a whole-tree copy.

Most downstream headers, all `spawn.cpp`, and almost all `posix.cpp` match the
pinned implementation. The substantive downstream additions are a SIGBUS
constant/ignored-handler acceptance, descriptor/executable path helpers, and
directory locking via a `.gufo-dir-lock` sidecar. These additions were reviewed,
not blindly imported:

- The pinned CRT include ordering and leaf type/time headers are retained; bubo
  has no additional C-compatibility fix to copy in these files.
- Descriptor/executable path helpers are unnecessary for positional I/O:
  `gufo_reopen` uses `ReOpenFile` on the existing descriptor, avoiding a mutable
  path reopen and a separate path-helper convention.
- The directory sidecar implementation keys ownership by fd, does not integrate
  with close/dup ownership, and holds its global mutex across a blocking lock.
  It is not selected. Directory flock fails explicitly with `ENOTSUP`.
- Silently accepting an undeliverable SIGBUS handler is not selected. Unsupported
  signal handling fails rather than claiming a handler was installed.
- The downstream pthread stub and HIP integration are not used.

## Local semantic fixes

### Files, descriptors, and streams

- Ordinary stream handles remain synchronous. `pread`/`pwrite` lazily reopen a
  separate, cached overlapped description, with an independent event per
  calling thread and proper pending-I/O completion. They never read, seek, or
  restore the stream's current position; concurrent calls therefore do not race
  with sequential `read`/`write`/`lseek`. Duplicates share the cached description.
- Transfer offsets and overflow are checked, offsets remain signed 64-bit on
  ARM64, EOF and short reads are returned accurately, and write errors are not
  mistaken for EOF. Each call can return a short transfer of at most 1 GiB;
  callers must already loop, as on POSIX. Positioned writes on append-only
  descriptors fail with `ENOTSUP` instead of silently appending.
- `gufo_reopen(fd, flags)` exposes independent-description reopening without
  `/proc`. `O_DIRECT` preserves native buffer/sector/length alignment requirements;
  no buffered retry hides invalid direct-I/O requests.
- `dup` preserves access mode and clears CLOEXEC; `dup2` shares description
  ownership. `F_DUPFD`/`F_DUPFD_CLOEXEC` honor their minimum fd. Descriptor
  flags are validated, close-on-exec controls actual spawn inheritance, and
  unsupported status changes fail explicitly. Regular disk files accept and
  retain `O_NONBLOCK`, which has no asynchronous-I/O effect on those files.
  Anonymous pipe read ends support nonblocking reads; nonblocking pipe writes
  and pipe write-readiness probing do not claim support.
- File locks use a separate byte-range lock handle at a reserved high offset,
  shared by duplicates and released only when the final duplicate closes.
  Shared/exclusive contention, conversion, unlock, and nonblocking errno are
  real operations; conversions are non-atomic. Directory locks are unsupported.
- Invalid CRT descriptors produce `EBADF` through a scoped thread-local invalid
  parameter handler. Importing this layer no longer installs global crash
  handlers, parses x64 C++ exception metadata, changes console modes, or silently
  initializes Winsock.
- The `stdio.h` shadow provides UTF-8 `fopen` through `_wfopen`, binary default
  payload I/O, CRT `fdopen`/64-bit seek aliases, and `fclose` registration cleanup.
  `fclose` must be reached through that shadow when a stream owns a tracked fd.
  `fsync` performs `FlushFileBuffers` and does not turn invalid handles into
  success. `/dev/null` maps to `NUL`.
- Writable/readable `fmemopen` uses native C `memory_stream.c` with direct
  caller-buffer access, not temporary-file staging. The stdio shadow dispatches
  block/character/string I/O, seek/tell/position, flush, EOF/error, rewind,
  buffering requests, descriptor query, and close. Null-buffer streams own
  their allocation; supplied buffers are never freed. Per-stream locks protect
  position/data while a registry lock protects lifetime. Binary reads retain
  embedded NULs; flush/close terminate writable buffers, requiring the snapshot
  caller's existing extra byte to preserve the final payload byte. Capacity,
  read/write permissions, partial items, invalid seeks, and arithmetic overflow
  produce real errors. Full/line buffering requests on memory tokens return
  ENOTSUP (memory streams are direct/unbuffered). The supplied dispatch is the
  byte-oriented subset listed above, not formatted scanning/output or
  wide-character stdio; DS4's snapshot serializer uses block I/O only.
- `fclose` unregisters its fd before CRT close but retains shared description/
  lock ownership through the actual CRT flush, avoiding both premature unlock
  and fd-reuse races. Mutable nonblocking pipe status belongs to the shared
  description, so setting or clearing it through either duplicate affects both.
- Descriptor inheritance enumerates the UCRT low-level 8192-entry table, not
  `_getmaxstdio`'s FILE-stream limit. A bounded `DS4FDV1` metadata trailer after
  the standard startup fd block transfers access/append/nonblocking flags without
  adding variables to explicit child environments. Child lazy initialization
  precedes descriptor lookup, mutation, and re-spawn, preserving inherited flags.
- `posix_fallocate` returns `ENOTSUP`: extending EOF alone does not reserve space.
  POSIX permission/ownership or resource-limit guarantees are not synthesized:
  chmod/fchmod and setrlimit fail explicitly; getrlimit only reports CRT fd limits.
  Stat mode/uid/gid remain synthetic, not a Windows ACL/security check.

### Mapping and memory

- Mapping offsets must be page-aligned; file views align their native base down
  to allocation granularity and return the page-aligned skewed address. Negative
  offsets, overflow, invalid protections/flags, and unsupported fixed/shared
  anonymous mappings are rejected.
- A mapping registry owns each complete view/allocation. `munmap` requires the
  original address and exact requested length. Partial unmap returns `ENOTSUP`
  without freeing any part of the live mapping. DS4 callers must not assume
  POSIX suffix-unmap support.
- The model loader handles disk-only Qwen/Engram tails explicitly: map the
  retained read-only prefix while the old view is still valid, rebase borrowed
  metadata keys and tensor names, then release the complete old view. Values
  remain offset-based; no tensor payload is copied and disk-tail descriptors
  stay valid. The native regression checks page boundaries that are not
  allocation-granularity boundaries, disk reads, metadata, and view cleanup.
- Shared writable mappings retain a backing handle for `MS_SYNC` flushes in
  addition to `FlushViewOfFile`; the view survives closing its original fd.
- `mlock`/`munlock` perform `VirtualLock`/`VirtualUnlock` and propagate errors and
  working-set quota denial. `mincore` uses the working-set query and validates
  page alignment.
- Prefetch uses `PrefetchVirtualMemory`; read population actually faults pages
  and reports access faults. Nonbinding access-pattern hints may be ignored.
  DONTNEED/FREE, write population, dump exclusion, and huge-page guarantees return
  `ENOTSUP`. Neither memset nor VirtualUnlock is advertised as cache eviction.

### Sockets and polling

- New `socket.cpp` wraps Winsock in owned synthetic int descriptors starting at
  `0x40000000`; native SOCKET values are never truncated on ARM64. `close`,
  `read`, `write`, `fcntl`, and `poll` route these descriptors correctly.
- Startup is checked and lazy. Standard address/listen/accept/connect/data/options
  operations translate Winsock errors to errno, including nonblocking connect
  and SO_ERROR. Timeval timeouts round positive microseconds upward to native
  milliseconds and reject overflow.
- Resolver-first `getaddrinfo` also performs checked startup (parent integration
  fix), so it does not depend on a prior socket() call.
- SOCK_NONBLOCK and SOCK_CLOEXEC are meaningful flags. Socketpair supports
  AF_UNIX/SOCK_STREAM/protocol zero using loopback TCP; it does not imply a full
  AF_UNIX pathname-socket implementation. Socket descriptor duplication and
  inheritance through spawn return `ENOTSUP`.
- MSG_DONTWAIT requires an already nonblocking socket, otherwise `ENOTSUP`;
  changing shared socket mode transiently would race another caller.
- The POSIX pollfd uses signed int fd, ignores negative fds, preserves events,
  supports more than 64 sockets, and reports closed descriptors as POLLNVAL.
  The mixed poll loop probes sockets, pipe read ends, regular files, and console
  input with a 1 ms wait quantum. Positive deadlines use QPC-backed monotonic
  elapsed time; the coarse `GetTickCount64` tick could expire a short wait early.
  Console readiness can include non-key events. Pipe write readiness is
  explicitly unsupported.

### Processes, timing, and paths

- Spawn honors explicit UTF-8 environments (including empty environments),
  preserves argv[0], fixes Windows CRT quoting, applies open/dup2/close actions
  in order, and does not resurrect closed descriptors from the parent table.
  All inherited descriptors are copied through a bounded CRT fd block and an
  explicit Win32 handle list; CLOEXEC and append flags are honored.
- Unsupported attributes/groups fail. SETPGROUP with pgroup zero creates a
  kill-on-close job, assigning the suspended child before it runs. Group kill
  terminates descendants even after waitpid reaps the original shell.
  `ds4_win32_spawn_forget` releases retained process/group handles; successful
  persistent browsers can call `ds4_win32_spawn_detach` to disable kill-on-close
  before release. Callers must release retained group jobs.
- Wait checks OS failures/options, protects process handles against concurrent
  wait/kill/forget, and distinguishes explicit kill from normal exit codes such
  as 143. Only a positive child pid is supported by waitpid. kill supports tracked
  child/group handles with signal zero or termination signals; it is not full
  Unix signal delivery. Literal quotes in argv[0] are unrepresentable by the
  Windows CRT first-token parser and return EINVAL. ARM64 CRT inheritance allows
  descriptor numbers below 7281 before accounting for the flags trailer; the
  complete block must fit 65535 bytes, otherwise spawn explicitly returns E2BIG.
- `sigaction` maintains process-wide persistent handlers for SIGINT, SIGBREAK,
  SIGTERM, and SIGABRT, including the legacy CRT abort alias. The dispatcher
  rearms the UCRT's one-shot handler before invoking user code; registration
  and rearming are serialized, and the previous user action is returned.
  Thread-local hardware-exception actions are not POSIX-equivalent and fail
  with `ENOTSUP`, as do unsupported masks/flags.
- Imported `environ == NULL` remains an inheritance sentinel, not a mutable
  environment array. setenv rejects empty values (`ENOTSUP`) because UCRT
  getenv cannot distinguish them from removal; nonempty values/unsetenv propagate
  CRT errors. Explicit spawn envp can still contain empty values.
- Clock IDs are validated; QPC supplies monotonic time, FILETIME realtime, and
  native process/thread CPU timing. nanosleep uses a waitable timer, rounded up
  to 100 ns units; Unix asynchronous signal interruption is not emulated.
  gettimeofday uses the Winsock timeval layout and reports EOVERFLOW past its
  signed 32-bit seconds range. uname reports aarch64 on native ARM64.
- chdir/getcwd/realpath use UTF-8 paths; chdir updates the native process working
  directory. getcwd/realpath return owned malloc results when requested.
  realpath resolves an existing native handle and requires PATH_MAX bytes for
  caller-owned buffers.
  strndup is available to C. Standard free cannot release `_aligned_malloc`;
  callers needing native aligned allocation must use a paired allocator/free.

## Build and test requirements

Place `compat/win32/include` before CRT headers. Clang uses include_next.
Native MSVC/NVCC host preprocessing uses generated `ds4_ucrt/` bridges to the
actual CRT headers for fcntl.h, limits.h, signal.h, stdio.h, string.h, time.h,
sys/stat.h, and sys/types.h; the build supplies that generated include root.
Require Windows SDK/UCRT, `_CRT_DECLARE_NONSTDC_NAMES=0`, `WIN32_LEAN_AND_MEAN`, `NOMINMAX`,
and C++17 with exceptions for `posix.cpp`, `socket.cpp`, and `spawn.cpp`. Compile
DS4 C sources and `tests/test_win32_posix.c` as C, not C++. Link the ARM64 C++
runtime plus `Kernel32.lib`, `Ws2_32.lib`, and `Psapi.lib`; other native C helpers
may have their own library requirements.

Embed `tools/windows/ds4.manifest` to select UTF-8 for native narrow argv and
environment conversion. Wide process creation alone cannot preserve Unicode
when the child UCRT narrows its command line through legacy ACP 1252.
The spawn regression checks the actual child's BMP/supplementary UTF-8 argv,
not merely the parent's wide command-line construction.

The frontends explicitly initialize console output before installing their
application signal handlers. `platform.c` captures both output modes before
changing either (stdout/stderr may share a buffer), enables UTF-8/VT output,
and restores the original code page/modes after flushing buffered output at
normal exit. A native termination fallback restores them on unhandled console
control events; application-handled cancellation keeps the live console
configured. Redirected streams are untouched. Native console regressions verify
actual Unicode glyphs, ANSI color attributes, and restoration from CP437 with
VT initially disabled.

`tests/test_win32_posix.c` includes the socket/spawn C cases. It exercises
concurrent positional reads/writes and a live sequential stream, short reads and
EOF, sparse offsets beyond 4 GiB, write errors, fd duplication/CLOEXEC/stream
ownership, flock contention/conversion/final-close release, mapping alignment and
whole-view ownership, explicit unsupported advice, locking, timing, and path
helpers. Socket/process cases cover real TCP and socketpair, polling >64 fds,
nonblocking errno/timeouts, ordered inheritance/actions, argv/environment, and
job-group lifecycle. Sparse-file tests require a filesystem supporting
FSCTL_SET_SPARSE (the intended NTFS target). No model or GPU is required.

`tests/test_win32_memory_stream.c` separately checks direct snapshot last-byte
preservation, read-only embedded NULs, truncation/append/update modes, seek/tell,
positions, EOF/error clearing, capacity failures, borrowed/owned buffers, and
guard bytes. POSIX tests also check shared duplicate nonblocking state and
buffered close retaining file locks through flush; spawn tests exercise inherited
read/write descriptors above 512 and nonblocking flags under an empty environment.

The native ARM64 clean compatibility build and runtime tests passed, including
repeated short poll deadlines across timer ticks. Toolchain versions and
integration results are recorded in [the Windows guide](../../docs/WINDOWS.md).
