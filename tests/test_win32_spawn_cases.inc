/* Included by test_win32_posix.c; call the child dispatcher before tests. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wchar.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

static const char *const spawn_quote_arguments[] = {
    "", "two words", "tab\there", "line\nbreak", "literal\"quote",
    "\\", "trailing \\", "before\\\\\"after", "\\\\server\\share\\",
    "\xce\xbb\xe9\x9b\xaa \xf0\x90\x90\x80", NULL};

int win32_spawn_child(int argc, char **argv) {
    if (argc < 3 || strcmp(argv[1], "--win32-spawn-child") != 0) return -1;
    if (strcmp(argv[2], "quote") == 0) {
        int i;
        assert(strcmp(argv[0], "different program name\\") == 0);
        for (i = 0; spawn_quote_arguments[i] != NULL; ++i) {
            assert(i + 3 < argc);
            assert(strcmp(argv[i + 3], spawn_quote_arguments[i]) == 0);
        }
        assert(argc == i + 3);
        return 37;
    }
    if (strcmp(argv[2], "env") == 0) {
        wchar_t value[64];
        assert(GetEnvironmentVariableW(L"DS4_SPAWN_CUSTOM", value, 64) == 13);
        assert(wcscmp(value, L"value = space") == 0);
        SetLastError(ERROR_SUCCESS);
        assert(GetEnvironmentVariableW(L"DS4_SPAWN_PARENT_ONLY", value, 64) == 0);
        assert(GetLastError() == ERROR_ENVVAR_NOT_FOUND);
        assert(GetEnvironmentVariableW(L"DS4_SPAWN_UNICODE", value, 64) == 2);
        assert(value[0] == 0x03bb && value[1] == 0x96ea && value[2] == 0);
        return 0;
    }
    if (strcmp(argv[2], "inherit-env") == 0) {
        wchar_t value[16];
        assert(GetEnvironmentVariableW(L"DS4_SPAWN_PARENT_ONLY", value, 16) == 6);
        assert(wcscmp(value, L"parent") == 0);
        return 0;
    }
    if (strcmp(argv[2], "empty-env") == 0) {
        wchar_t value[16];
        SetLastError(ERROR_SUCCESS);
        assert(GetEnvironmentVariableW(L"DS4_SPAWN_PARENT_ONLY", value, 16) == 0);
        assert(GetLastError() == ERROR_ENVVAR_NOT_FOUND);
        return 0;
    }
    if (strcmp(argv[2], "write") == 0) {
        const int fd = argc > 3 ? atoi(argv[3]) : STDOUT_FILENO;
        assert(write(fd, "spawn output\n", 13) == 13);
        return 23;
    }
    if (strcmp(argv[2], "closed") == 0) {
        char byte;
        assert(argc == 4);
        errno = 0;
        assert(read(atoi(argv[3]), &byte, 1) == -1 && errno == EBADF);
        return 0;
    }
    if (strcmp(argv[2], "null-input") == 0) {
        char byte;
        assert(read(STDIN_FILENO, &byte, 1) == 0);
        return 0;
    }
    if (strcmp(argv[2], "sleep") == 0) {
        Sleep(100);
        return 41;
    }
    if (strcmp(argv[2], "exit143") == 0) return 143;
    if (strcmp(argv[2], "forever") == 0) {
        Sleep(INFINITE);
        return 0;
    }
    if (strcmp(argv[2], "descriptor-state") == 0) {
        const int file_fd = argc > 3 ? atoi(argv[3]) : -1;
        const int pipe_fd = argc > 4 ? atoi(argv[4]) : -1;
        const int append_fd = argc > 5 ? atoi(argv[5]) : -1;
        char data[3];
        assert(argc == 6);
        assert(file_fd >= 600);
        assert((fcntl(file_fd, F_GETFL) & O_ACCMODE) == O_RDWR);
        assert(lseek(file_fd, 0, SEEK_CUR) == 0);
        assert(pwrite(file_fd, "Z", 1, 1) == 1);
        assert(pread(file_fd, data, sizeof(data), 0) == sizeof(data));
        assert(memcmp(data, "aZc", sizeof(data)) == 0);
        assert(lseek(file_fd, 0, SEEK_CUR) == 0);
        assert((fcntl(pipe_fd, F_GETFL) & O_NONBLOCK) != 0);
        errno = 0;
        assert(read(pipe_fd, data, 1) == -1 && errno == EAGAIN);
        assert((fcntl(append_fd, F_GETFL) & (O_ACCMODE | O_APPEND)) ==
               (O_WRONLY | O_APPEND));
        return 0;
    }
    assert(!"unknown spawn child mode");
    return 127;
}

static void spawn_test_executable(char *path, size_t capacity) {
    wchar_t wide[32768];
    const DWORD length = GetModuleFileNameW(NULL, wide, 32768);
    assert(length != 0 && length < 32768);
    assert(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1,
                               path, (int)capacity, NULL, NULL) > 0);
}

static void spawn_test_wait(pid_t pid, int code) {
    int status = -1;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == code);
    errno = 0;
    assert(waitpid(pid, &status, 0) == -1 && errno == ECHILD);
}

static void spawn_test_pipe_output(int read_fd, pid_t pid) {
    char output[32];
    ssize_t length;
    spawn_test_wait(pid, 23);
    length = read(read_fd, output, sizeof(output));
    assert(length == 13 && memcmp(output, "spawn output\n", 13) == 0);
    assert(read(read_fd, output, sizeof(output)) == 0);
    assert(close(read_fd) == 0);
}

void test_win32_spawn(void) {
    char executable[32768 * 3];
    char *basic[] = {"child alias", "--win32-spawn-child", "sleep", NULL};
    char *environment[] = {"DS4_SPAWN_CUSTOM=value = space",
                           "DS4_SPAWN_UNICODE=\xce\xbb\xe9\x9b\xaa", NULL};
    char *empty_environment[] = {NULL};
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    pid_t pid = -1;
    int pipes[2];
    int status;
    int i;
    spawn_test_executable(executable, sizeof(executable));

    {
        char *arguments[32] = {"different program name\\",
                              "--win32-spawn-child", "quote"};
        for (i = 0; spawn_quote_arguments[i] != NULL; ++i)
            arguments[i + 3] = (char *)spawn_quote_arguments[i];
        arguments[i + 3] = NULL;
        assert(posix_spawn(&pid, executable, NULL, NULL, arguments, NULL) == 0);
        spawn_test_wait(pid, 37);
    }

    assert(SetEnvironmentVariableW(L"DS4_SPAWN_PARENT_ONLY", L"parent"));
    basic[2] = "env";
    assert(posix_spawn(&pid, executable, NULL, NULL, basic, environment) == 0);
    spawn_test_wait(pid, 0);
    basic[2] = "inherit-env";
    assert(posix_spawn(&pid, executable, NULL, NULL, basic, NULL) == 0);
    spawn_test_wait(pid, 0);
    basic[2] = "empty-env";
    assert(posix_spawn(&pid, executable, NULL, NULL, basic, empty_environment) == 0);
    spawn_test_wait(pid, 0);
    assert(SetEnvironmentVariableW(L"DS4_SPAWN_PARENT_ONLY", NULL));

    /* addopen owns its path and rebuilds stdin even without a parent console. */
    {
        char null_path[] = "/dev/null";
        assert(posix_spawn_file_actions_init(&actions) == 0);
        assert(posix_spawn_file_actions_addopen(&actions, STDIN_FILENO,
                                                null_path, O_RDONLY, 0) == 0);
        null_path[1] = 'x';
        basic[2] = "null-input";
        assert(posix_spawn(&pid, executable, &actions, NULL, basic, NULL) == 0);
        spawn_test_wait(pid, 0);
        assert(posix_spawn_file_actions_destroy(&actions) == 0);
    }

    /* Redirect stdout, then close the original pipe ends in the child. */
    assert(pipe(pipes) == 0);
    assert(posix_spawn_file_actions_init(&actions) == 0);
    assert(posix_spawn_file_actions_addclose(&actions, pipes[0]) == 0);
    assert(posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO) == 0);
    assert(posix_spawn_file_actions_addclose(&actions, pipes[1]) == 0);
    basic[2] = "write";
    assert(posix_spawn(&pid, executable, &actions, NULL, basic, NULL) == 0);
    assert(posix_spawn_file_actions_destroy(&actions) == 0);
    assert(close(pipes[1]) == 0);
    spawn_test_pipe_output(pipes[0], pid);

    /* A CLOEXEC source is still available to earlier file actions. */
    assert(pipe(pipes) == 0);
    assert(fcntl(pipes[1], F_SETFD, FD_CLOEXEC) == 0);
    assert(posix_spawn_file_actions_init(&actions) == 0);
    assert(posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO) == 0);
    assert(posix_spawn_file_actions_addclose(&actions, pipes[0]) == 0);
    assert(posix_spawn_file_actions_addclose(&actions, pipes[1]) == 0);
    assert(posix_spawn(&pid, executable, &actions, NULL, basic, NULL) == 0);
    assert(posix_spawn_file_actions_destroy(&actions) == 0);
    assert(close(pipes[1]) == 0);
    spawn_test_pipe_output(pipes[0], pid);

    /* Nonstandard fds inherit without an action; dup2(fd,fd) clears CLOEXEC. */
    for (i = 0; i < 2; ++i) {
        char fd_number[16];
        char *arguments[] = {"child alias", "--win32-spawn-child", "write", fd_number, NULL};
        assert(pipe(pipes) == 0);
        snprintf(fd_number, sizeof(fd_number), "%d", pipes[1]);
        assert(posix_spawn_file_actions_init(&actions) == 0);
        assert(posix_spawn_file_actions_addclose(&actions, pipes[0]) == 0);
        if (i != 0) {
            assert(fcntl(pipes[1], F_SETFD, FD_CLOEXEC) == 0);
            assert(posix_spawn_file_actions_adddup2(&actions, pipes[1], pipes[1]) == 0);
        }
        assert(posix_spawn(&pid, executable, &actions, NULL, arguments, NULL) == 0);
        assert(posix_spawn_file_actions_destroy(&actions) == 0);
        assert(close(pipes[1]) == 0);
        spawn_test_pipe_output(pipes[0], pid);
    }

    /* Closing a source must not resurrect it from the parent's table. */
    assert(pipe(pipes) == 0);
    assert(posix_spawn_file_actions_init(&actions) == 0);
    assert(posix_spawn_file_actions_addclose(&actions, pipes[1]) == 0);
    assert(posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO) == 0);
    assert(posix_spawn(&pid, executable, &actions, NULL, basic, NULL) == EBADF);
    assert(posix_spawn_file_actions_destroy(&actions) == 0);
    assert(close(pipes[0]) == 0 && close(pipes[1]) == 0);

    /* CLOEXEC descriptors disappear when no dup action clears the flag. */
    {
        char fd_number[16];
        char *arguments[] = {"child alias", "--win32-spawn-child", "closed", fd_number, NULL};
        assert(pipe(pipes) == 0);
        assert(fcntl(pipes[0], F_SETFD, FD_CLOEXEC) == 0);
        assert(fcntl(pipes[1], F_SETFD, FD_CLOEXEC) == 0);
        snprintf(fd_number, sizeof(fd_number), "%d", pipes[0]);
        assert(posix_spawn(&pid, executable, NULL, NULL, arguments, NULL) == 0);
        spawn_test_wait(pid, 0);
        assert(close(pipes[0]) == 0 && close(pipes[1]) == 0);
    }

    /* Private startup metadata preserves POSIX flags without contaminating
       even an explicitly empty environment, including CRT fds above 511. */
    {
        wchar_t directory[MAX_PATH], temporary[MAX_PATH];
        char path[MAX_PATH * 3], file_number[16], pipe_number[16], append_number[16];
        char *arguments[] = {"child alias", "--win32-spawn-child", "descriptor-state",
                             file_number, pipe_number, append_number, NULL};
        int file_fd, high_fd, append_fd;
        char data[3];
        assert(GetTempPathW(MAX_PATH, directory) > 0);
        assert(GetTempFileNameW(directory, L"ds4", 0, temporary) != 0);
        assert(WideCharToMultiByte(CP_UTF8, 0, temporary, -1, path,
                                   sizeof(path), NULL, NULL) > 0);
        file_fd = open(path, O_RDWR | O_TRUNC, 0600);
        assert(file_fd >= 0);
        assert(write(file_fd, "abc", 3) == 3);
        assert(lseek(file_fd, 0, SEEK_SET) == 0);
        high_fd = fcntl(file_fd, F_DUPFD, 600);
        assert(high_fd >= 600);
        assert(close(file_fd) == 0);
        append_fd = open(path, O_WRONLY | O_APPEND);
        assert(append_fd >= 0);
        assert(pipe(pipes) == 0);
        assert(fcntl(pipes[0], F_SETFL, O_NONBLOCK) == 0);
        snprintf(file_number, sizeof(file_number), "%d", high_fd);
        snprintf(pipe_number, sizeof(pipe_number), "%d", pipes[0]);
        snprintf(append_number, sizeof(append_number), "%d", append_fd);
        assert(posix_spawn(&pid, executable, NULL, NULL, arguments, empty_environment) == 0);
        spawn_test_wait(pid, 0);
        assert(pread(high_fd, data, sizeof(data), 0) == sizeof(data));
        assert(memcmp(data, "aZc", sizeof(data)) == 0);
        assert(lseek(high_fd, 0, SEEK_CUR) == 0);
        assert(close(high_fd) == 0 && close(append_fd) == 0);
        assert(close(pipes[0]) == 0 && close(pipes[1]) == 0);
        assert(DeleteFileW(temporary));
    }

    assert(posix_spawn_file_actions_init(&actions) == 0);
    assert(posix_spawn_file_actions_addclose(&actions, -1) == EBADF);
    assert(posix_spawn_file_actions_adddup2(&actions, 0, -1) == EBADF);
    assert(posix_spawn_file_actions_adddup2(&actions, INT_MAX, 1) == EBADF);
    assert(posix_spawn_file_actions_adddup2(&actions, 0, INT_MAX) == EBADF);
    assert(posix_spawn_file_actions_destroy(&actions) == 0);
    assert(posix_spawnattr_init(&attributes) == 0);
    assert(posix_spawnattr_setflags(&attributes, (short)0x4000) == ENOTSUP);
    assert(posix_spawnattr_setpgroup(&attributes, 1) == ENOTSUP);
    attributes.flags = 0x4000; /* Direct struct writes must not bypass validation. */
    assert(posix_spawn(&pid, executable, NULL, &attributes, basic, NULL) == ENOTSUP);
    assert(posix_spawnattr_destroy(&attributes) == 0);
    assert(posix_spawn(&pid, "?:\\ds4-invalid-executable.exe", NULL, NULL, basic, NULL) != 0);
    assert(posix_spawnp(&pid, "ds4-spawn-missing-executable-97f3.exe", NULL, NULL,
                        basic, NULL) == ENOENT);
    {
        char missing[32768 * 3];
        assert(strlen(executable) + 9 < sizeof(missing));
        strcpy(missing, executable);
        strcat(missing, ".missing");
        assert(posix_spawn(&pid, missing, NULL, NULL, basic, NULL) == ENOENT);
    }
    {
        wchar_t temporary_directory[MAX_PATH], temporary_file[MAX_PATH];
        char invalid_executable[MAX_PATH * 3];
        HANDLE file;
        DWORD written;
        assert(GetTempPathW(MAX_PATH, temporary_directory) > 0);
        assert(GetTempFileNameW(temporary_directory, L"ds4", 0, temporary_file) != 0);
        file = CreateFileW(temporary_file, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
        assert(file != INVALID_HANDLE_VALUE);
        assert(WriteFile(file, "not a PE executable", 19, &written, NULL) && written == 19);
        assert(CloseHandle(file));
        assert(WideCharToMultiByte(CP_UTF8, 0, temporary_file, -1, invalid_executable,
                                   sizeof(invalid_executable), NULL, NULL) > 0);
        assert(posix_spawn(&pid, invalid_executable, NULL, NULL, basic, NULL) == ENOEXEC);
        assert(DeleteFileW(temporary_file));
    }
    {
        char *bad_arguments[] = {"literal\"quote", NULL};
        char *bad_environment[] = {"not-an-assignment", NULL};
        assert(posix_spawn(&pid, executable, NULL, NULL, bad_arguments, NULL) == EINVAL);
        assert(posix_spawn(&pid, executable, NULL, NULL, basic, bad_environment) == EINVAL);
    }

    assert(posix_spawnattr_init(&attributes) == 0);
    assert(posix_spawnattr_setpgroup(&attributes, 0) == 0);
    assert(posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP) == 0);
    basic[2] = "sleep";
    assert(posix_spawn(&pid, executable, NULL, &attributes, basic, NULL) == 0);
    spawn_test_wait(pid, 41);
    assert(ds4_win32_spawn_forget(pid) == 0);
    assert(posix_spawnattr_destroy(&attributes) == 0);
    basic[2] = "exit143";
    assert(posix_spawn(&pid, executable, NULL, NULL, basic, NULL) == 0);
    spawn_test_wait(pid, 143); /* Normal high exit codes are not signals. */

    assert(posix_spawnattr_init(&attributes) == 0);
    assert(posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP) == 0);
    basic[2] = "forever";
    assert(posix_spawn(&pid, executable, NULL, &attributes, basic, NULL) == 0);
    assert(kill(-pid, SIGTERM) == 0);
    assert(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGTERM);
    assert(ds4_win32_spawn_forget(pid) == 0);
    assert(posix_spawnattr_destroy(&attributes) == 0);

    basic[2] = "sleep";
    assert(posix_spawn(&pid, executable, NULL, NULL, basic, NULL) == 0);
    errno = 0;
    assert(waitpid(pid, &status, 0x40000000) == -1 && errno == EINVAL);
    {
        const pid_t waited = waitpid(pid, &status, WNOHANG);
        assert(waited == 0 || waited == pid);
        if (waited == 0) spawn_test_wait(pid, 41);
        else assert(WIFEXITED(status) && WEXITSTATUS(status) == 41);
    }
}
