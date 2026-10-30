/* Win32 POSIX spawn over CreateProcessW. Ordered open/close/dup2 actions run
 * before CLOEXEC filtering; remaining CRT descriptors inherit by number.
 * An inheritable socket returns ENOTSUP (no socket inheritance handshake).
 * Only POSIX_SPAWN_SETPGROUP with pgroup=0 is supported (a new Win32 job). */
#pragma once
#include "gufo_posix.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int count;
  int capacity;
  struct gufo_spawn_action* actions;
} posix_spawn_file_actions_t;
typedef struct {
  int flags;
  pid_t pgroup;
} posix_spawnattr_t;
#define POSIX_SPAWN_SETPGROUP 0x02

int posix_spawn_file_actions_init(posix_spawn_file_actions_t* actions);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t* actions);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t* actions,
                                     int fd, int target);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t* actions,
                                      int fd);
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t* actions, int fd,
                                     const char* path, int flags, mode_t mode);
int posix_spawnattr_init(posix_spawnattr_t* attributes);
int posix_spawnattr_destroy(posix_spawnattr_t* attributes);
int posix_spawnattr_setflags(posix_spawnattr_t* attributes, short flags);
int posix_spawnattr_getflags(const posix_spawnattr_t* attributes, short* flags);
int posix_spawnattr_setpgroup(posix_spawnattr_t* attributes, pid_t pgroup);
int posix_spawnattr_getpgroup(const posix_spawnattr_t* attributes, pid_t* pgroup);
/* Strings are UTF-8. envp supplies the complete child environment; NULL
 * inherits the current environment. argv[0] is preserved independently of
 * the executable, but a literal double quote in argv[0] returns EINVAL
 * because the Windows CRT cannot represent it. Other arguments use CRT
 * backslash/quote escaping. spawnp searches the parent's Windows PATH
 * (.exe suffix supported); spawn resolves only the supplied path.
 * Nonzero unsupported attribute flags or pgroup values return ENOTSUP.
 * Descriptor numbers must fit the WORD-sized CRT inheritance block. */
int posix_spawnp(pid_t* pid, const char* file,
                 const posix_spawn_file_actions_t* actions,
                 const posix_spawnattr_t* attributes, char* const argv[],
                 char* const envp[]);
int posix_spawn(pid_t* pid, const char* path,
                const posix_spawn_file_actions_t* actions,
                const posix_spawnattr_t* attributes, char* const argv[],
                char* const envp[]);
/* Windows process-group lifecycle: forget closes a retained kill-on-close
 * job; detach preserves its descendants by removing that limit first.
 * Both return 0 on success or -1 with errno on failure. */
int ds4_win32_spawn_forget(pid_t pid);
int ds4_win32_spawn_detach(pid_t pid);

#ifdef __cplusplus
}
#endif
