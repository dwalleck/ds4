/* Native, model-independent compatibility contract tests. Compile as C. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <windows.h>
#include <winioctl.h>

#include "test_win32_socket_cases.inc"
#include "test_win32_spawn_cases.inc"

static void temp_template(char path[MAX_PATH]) {
    DWORD n = GetTempPathA(MAX_PATH, path);
    assert(n > 0 && n < MAX_PATH - 32);
    strcat(path, "ds4-posix-XXXXXX");
}

static unsigned char pattern(size_t position) {
    return (unsigned char)((position * 37 + 19) & 255);
}

typedef struct {
    int fd;
    unsigned index;
} io_worker;

static DWORD WINAPI concurrent_io(void *opaque) {
    io_worker *worker = opaque;
    unsigned char buffer[257];
    for (unsigned pass = 0; pass < 500; ++pass) {
        size_t start = (worker->index * 991 + pass * 47) % 60000;
        assert(pread(worker->fd, buffer, sizeof(buffer), (off_t)start) == sizeof(buffer));
        for (size_t i = 0; i < sizeof(buffer); ++i) assert(buffer[i] == pattern(start + i));
        memset(buffer, (int)(worker->index + 1), sizeof(buffer));
        assert(pwrite(worker->fd, buffer, sizeof(buffer), 65536 + worker->index * 4096) == sizeof(buffer));
    }
    return 0;
}

static void test_positioned_io(void) {
    char path[MAX_PATH];
    temp_template(path);
    int fd = mkstemp(path);
    assert(fd >= 0);
    unsigned char original[65536];
    for (size_t i = 0; i < sizeof(original); ++i) original[i] = pattern(i);
    assert(write(fd, original, sizeof(original)) == sizeof(original));
    assert(lseek(fd, 17, SEEK_SET) == 17);
    HANDLE threads[8];
    io_worker workers[8];
    for (unsigned i = 0; i < 8; ++i) {
        workers[i].fd = fd;
        workers[i].index = i;
        threads[i] = CreateThread(NULL, 0, concurrent_io, &workers[i], 0, NULL);
        assert(threads[i] != NULL);
    }
    for (unsigned pass = 0; pass < 500; ++pass) {
        unsigned char byte;
        assert(read(fd, &byte, 1) == 1);
        assert(byte == pattern(17 + pass));
    }
    assert(WaitForMultipleObjects(8, threads, TRUE, INFINITE) == WAIT_OBJECT_0);
    for (unsigned i = 0; i < 8; ++i) CloseHandle(threads[i]);
    assert(lseek(fd, 0, SEEK_CUR) == 517);
    unsigned char buffer[512];
    for (unsigned i = 0; i < 8; ++i) {
        assert(pread(fd, buffer, 257, 65536 + i * 4096) == 257);
        for (unsigned j = 0; j < 257; ++j) assert(buffer[j] == i + 1);
    }
    assert(pread(fd, buffer, 1, -1) == -1 && errno == EINVAL);
    assert(pwrite(fd, buffer, 1, -1) == -1 && errno == EINVAL);
    assert(lseek(fd, 0, 999) == -1 && errno == EINVAL);
    struct stat st;
    assert(fstat(fd, &st) == 0);
    assert(pread(fd, buffer, sizeof(buffer), st.st_size - 3) == 3);
    assert(pread(fd, buffer, sizeof(buffer), st.st_size) == 0);
    assert(pread(fd, buffer, sizeof(buffer), st.st_size + 100) == 0);
    assert(pread(fd, buffer, 0, 0) == 0);
    assert(ftruncate(fd, -1) == -1 && errno == EINVAL);
    assert(fsync(-1) == -1 && errno == EBADF);
    int readonly = open(path, O_RDONLY);
    assert(readonly >= 0);
    assert(pwrite(readonly, buffer, 1, 0) == -1 && errno == EBADF);
    assert(write(readonly, buffer, 1) == -1);
    assert(close(readonly) == 0);

    /* Sparse marking avoids reserving several GiB just to exercise high words. */
    DWORD done;
    assert(DeviceIoControl((HANDLE)_get_osfhandle(fd), FSCTL_SET_SPARSE,
                           NULL, 0, NULL, 0, &done, NULL));
    const off_t large = ((off_t)5 << 30) + 123;
    const char tail[] = "large-offset";
    assert(pwrite(fd, tail, sizeof(tail), large) == sizeof(tail));
    assert(pread(fd, buffer, sizeof(tail), large) == sizeof(tail));
    assert(memcmp(buffer, tail, sizeof(tail)) == 0);
    assert(lseek(fd, 0, SEEK_CUR) == 517);
    assert(fstat(fd, &st) == 0 && st.st_size == large + sizeof(tail));
    assert(pread(fd, buffer, sizeof(buffer), large + sizeof(tail) - 2) == 2);
    assert(pread(fd, buffer, 1, large + sizeof(tail)) == 0);
    assert(ftruncate(fd, 65536) == 0);
    int reopened = gufo_reopen(fd, O_RDONLY | O_CONCURRENT_RANDOM | O_CLOEXEC);
    assert(reopened >= 0 && (fcntl(reopened, F_GETFD) & FD_CLOEXEC));
    assert(lseek(reopened, 999, SEEK_SET) == 999);
    assert(lseek(fd, 0, SEEK_CUR) == 517);
    assert(close(reopened) == 0);
    assert(close(fd) == 0);
    assert(close(fd) == -1 && errno == EBADF);
    assert(unlink(path) == 0);
}

static void test_mappings(void) {
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    assert(page >= 4096 && (page & (page - 1)) == 0);
    unsigned char *anonymous = mmap(NULL, page * 3, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(anonymous != MAP_FAILED);
    assert(((uintptr_t)anonymous % page) == 0);
    for (size_t i = 0; i < page * 3; ++i) assert(anonymous[i] == 0);
    memset(anonymous, 0x5a, page * 3);
    assert(munmap(anonymous + page, page) == -1 && errno == ENOTSUP);
    assert(munmap(anonymous, page) == -1 && errno == ENOTSUP);
    assert(anonymous[0] == 0x5a && anonymous[page] == 0x5a);
    assert(madvise(anonymous, page, MADV_DONTNEED) == -1 && errno == ENOTSUP);
    assert(anonymous[0] == 0x5a);
    assert(madvise(anonymous, page, 99999) == -1 && errno == EINVAL);
    assert(madvise(anonymous, page, MADV_POPULATE_READ) == 0);
    unsigned char residency[3];
    assert(mincore(anonymous, page * 3, residency) == 0);
    for (unsigned i = 0; i < 3; ++i) assert(residency[i] & 1);
    int locked = mlock(anonymous, page);
    if (!locked) assert(munlock(anonymous, page) == 0);
    else assert(errno != 0); /* Working-set quota may deny locking. */
    assert(mlock((void *)(uintptr_t)1, page) == -1);
    assert(munmap(anonymous, page * 3) == 0);
    assert(mmap(NULL, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED);

    char path[MAX_PATH];
    temp_template(path);
    int fd = mkstemp(path);
    assert(fd >= 0 && ftruncate(fd, (off_t)(page * 5)) == 0);
    const char data[] = "page-aligned, not allocation-granularity-aligned";
    assert(pwrite(fd, data, sizeof(data), (off_t)page) == sizeof(data));
    unsigned char *view = mmap(NULL, page * 2, PROT_READ, MAP_PRIVATE, fd, (off_t)page);
    assert(view != MAP_FAILED && ((uintptr_t)view % page) == 0);
    assert(memcmp(view, data, sizeof(data)) == 0);
    MEMORY_BASIC_INFORMATION info;
    assert(VirtualQuery(view, &info, sizeof(info)) == sizeof(info));
    assert((info.Protect & 0xff) == PAGE_READONLY);
    assert(mmap(NULL, page, PROT_READ, MAP_PRIVATE, fd, 1) == MAP_FAILED && errno == EINVAL);
    assert(close(fd) == 0); /* The view owns its section independently. */
    assert(memcmp(view, data, sizeof(data)) == 0);
    assert(munmap(view, page * 2) == 0);
    assert(unlink(path) == 0);
}

static void test_descriptors_and_locks(void) {
    char path[MAX_PATH];
    temp_template(path);
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(fcntl(fd, F_SETFD, FD_CLOEXEC) == 0);
    assert(fcntl(fd, F_GETFD) == FD_CLOEXEC);
    int copy = dup(fd);
    assert(copy >= 0 && fcntl(copy, F_GETFD) == 0);
    int high = fcntl(fd, F_DUPFD_CLOEXEC, 128);
    assert(high >= 128 && fcntl(high, F_GETFD) == FD_CLOEXEC);
    assert(close(high) == 0);
    assert(write(fd, "abc", 3) == 3);
    assert(lseek(copy, 0, SEEK_CUR) == 3); /* dup shares stream position. */
    assert(flock(fd, LOCK_EX | LOCK_NB) == 0);
    int contender = open(path, O_RDWR);
    assert(contender >= 0);
    assert(flock(contender, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK);
    assert(close(fd) == 0);
    assert(flock(contender, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK);
    assert(close(copy) == 0); /* Only the final duplicate releases the lock. */
    assert(flock(contender, LOCK_EX | LOCK_NB) == 0);
    assert(flock(contender, LOCK_UN) == 0);
    assert(flock(contender, LOCK_SH | LOCK_NB) == 0);
    int shared = open(path, O_RDONLY);
    assert(shared >= 0 && flock(shared, LOCK_SH | LOCK_NB) == 0);
    assert(flock(contender, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK);
    assert(close(shared) == 0);
    assert(flock(contender, LOCK_EX | LOCK_NB) == 0);
    assert(flock(contender, 0) == -1 && errno == EINVAL);
    assert(close(contender) == 0);
    fd = open(path, O_RDWR);
    assert(fd >= 0);
    FILE *stream = fdopen(fd, "r+b");
    assert(stream != NULL);
    assert(fseeko(stream, 0, SEEK_SET) == 0 && ftello(stream) == 0);
    assert(fwrite("stream", 1, 6, stream) == 6 && fflush(stream) == 0);
    assert(fsync(fileno(stream)) == 0);
    assert(fclose(stream) == 0);
    assert(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
    assert(unlink(path) == 0);
}

typedef struct {
    int fd;
    HANDLE waiting;
} flush_lock_reader;

static DWORD WINAPI read_after_unlock(void *opaque) {
    flush_lock_reader *reader = opaque;
    SetEvent(reader->waiting);
    assert(flock(reader->fd, LOCK_EX) == 0);
    unsigned char payload[16384];
    assert(pread(reader->fd, payload, sizeof(payload), 0) == sizeof(payload));
    for (size_t i = 0; i < sizeof(payload); ++i) assert(payload[i] == 0x71);
    assert(close(reader->fd) == 0);
    return 0;
}

static void test_fclose_flush_lock(void) {
    char path[MAX_PATH];
    temp_template(path);
    int fd = mkstemp(path);
    assert(fd >= 0 && flock(fd, LOCK_EX) == 0);
    FILE *stream = fdopen(fd, "wb");
    assert(stream);
    char stream_buffer[65536];
    unsigned char payload[16384];
    memset(payload, 0x71, sizeof(payload));
    assert(setvbuf(stream, stream_buffer, _IOFBF, sizeof(stream_buffer)) == 0);
    assert(fwrite(payload, 1, sizeof(payload), stream) == sizeof(payload));
    flush_lock_reader reader;
    reader.fd = open(path, O_RDONLY);
    reader.waiting = CreateEventW(NULL, TRUE, FALSE, NULL);
    assert(reader.fd >= 0 && reader.waiting);
    HANDLE thread = CreateThread(NULL, 0, read_after_unlock, &reader, 0, NULL);
    assert(thread && WaitForSingleObject(reader.waiting, INFINITE) == WAIT_OBJECT_0);
    assert(fclose(stream) == 0); /* Flush must complete before the lock releases. */
    assert(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0);
    CloseHandle(thread);
    CloseHandle(reader.waiting);
    assert(unlink(path) == 0);
}

static void test_pipe_readiness(void) {
    int descriptors[2];
    char byte;
    assert(pipe(descriptors) == 0);
    int copy = dup(descriptors[0]);
    assert(copy >= 0);
    assert(fcntl(descriptors[0], F_SETFL,
                 fcntl(descriptors[0], F_GETFL) | O_NONBLOCK) == 0);
    assert(read(descriptors[0], &byte, 1) == -1 && errno == EAGAIN);
    assert(fcntl(copy, F_GETFL) & O_NONBLOCK);
    assert(read(copy, &byte, 1) == -1 && errno == EAGAIN);
    assert(fcntl(copy, F_SETFL, fcntl(copy, F_GETFL) & ~O_NONBLOCK) == 0);
    assert(!(fcntl(descriptors[0], F_GETFL) & O_NONBLOCK));
    assert(fcntl(copy, F_SETFL, fcntl(copy, F_GETFL) | O_NONBLOCK) == 0);
    assert(fcntl(descriptors[0], F_GETFL) & O_NONBLOCK);
    assert(close(copy) == 0);
    struct pollfd pending = {descriptors[0], POLLIN, 0};
    assert(poll(&pending, 1, 0) == 0);
    assert(write(descriptors[1], "r", 1) == 1);
    assert(poll(&pending, 1, 100) == 1 && (pending.revents & POLLIN));
    assert(read(descriptors[0], &byte, 1) == 1 && byte == 'r');
    assert(fcntl(descriptors[1], F_SETFL,
                 fcntl(descriptors[1], F_GETFL) | O_NONBLOCK) == -1 && errno == ENOTSUP);
    assert(close(descriptors[1]) == 0);
    assert(poll(&pending, 1, 100) == 1 && (pending.revents & POLLHUP));
    assert(read(descriptors[0], &byte, 1) == 0);
    assert(close(descriptors[0]) == 0);
}

static void test_utf8_stream_paths(void) {
    char path[MAX_PATH];
    temp_template(path);
    int fd = mkstemp(path);
    assert(fd >= 0 && close(fd) == 0 && unlink(path) == 0);
    strcat(path, "-\xce\xbb\xe9\x9b\xaa");
    const unsigned char payload[] = {'\n', '\r', '\n', 0, 0x1a, 0xff};
    FILE *stream = fopen(path, "w+");
    assert(stream != NULL);
    assert(fwrite(payload, 1, sizeof(payload), stream) == sizeof(payload));
    assert(fflush(stream) == 0 && fseeko(stream, 0, SEEK_END) == 0);
    assert(ftello(stream) == sizeof(payload));
    assert(fseeko(stream, 0, SEEK_SET) == 0);
    unsigned char result[sizeof(payload)];
    assert(fread(result, 1, sizeof(result), stream) == sizeof(result));
    assert(memcmp(result, payload, sizeof(result)) == 0);
    assert(fclose(stream) == 0);
    char *canonical = realpath(path, NULL);
    assert(canonical && strstr(canonical, "\xce\xbb\xe9\x9b\xaa"));
    free(canonical);
    assert(unlink(path) == 0);
}


static void test_time_and_paths(void) {
    struct timespec before, after;
    /* Cover tick boundaries: GetTickCount64 may advance 16 ms while the
     * caller has waited less than one millisecond. A timeout must not. */
    for (int i = 0; i < 16; ++i) {
        assert(clock_gettime(CLOCK_MONOTONIC, &before) == 0);
        assert(poll(NULL, 0, 15) == 0);
        assert(clock_gettime(CLOCK_MONOTONIC, &after) == 0);
        double elapsed = (double)(after.tv_sec - before.tv_sec) +
                         (after.tv_nsec - before.tv_nsec) / 1e9;
        assert(elapsed >= 0.015);
    }
    assert(clock_gettime(99999, &after) == -1 && errno == EINVAL);
    assert(clock_gettime(CLOCK_REALTIME, &after) == 0 && after.tv_sec > 1700000000);
    struct timespec invalid_sleep = {0, 1000000000};
    assert(nanosleep(&invalid_sleep, NULL) == -1 && errno == EINVAL);
    struct timespec delay = {0, 1000000};
    assert(nanosleep(&delay, NULL) == 0);
    char *cwd = getcwd(NULL, 0);
    char *resolved = realpath(".", NULL);
    assert(cwd && resolved);
    assert(strlen(cwd) > 0 && strlen(resolved) > 0);
    free(cwd); free(resolved);
    char tiny[1];
    assert(getcwd(tiny, sizeof(tiny)) == NULL && errno == ERANGE);
    char *prefix = strndup("abcdef", 3);
    assert(prefix && strcmp(prefix, "abc") == 0);
    free(prefix);
    assert(setenv("DS4_COMPAT_TEST", "value", 1) == 0);
    assert(strcmp(getenv("DS4_COMPAT_TEST"), "value") == 0);
    assert(setenv("DS4_COMPAT_TEST", "other", 0) == 0);
    assert(strcmp(getenv("DS4_COMPAT_TEST"), "value") == 0);
    assert(unsetenv("DS4_COMPAT_TEST") == 0 && getenv("DS4_COMPAT_TEST") == NULL);
    struct utsname name;
    assert(uname(&name) == 0);
#if defined(_M_ARM64) || defined(__aarch64__)
    assert(strcmp(name.machine, "aarch64") == 0);
#endif
}

static void test_change_directory(void) {
    char base[MAX_PATH], directory[MAX_PATH], marker[MAX_PATH];
    char *original = getcwd(NULL, 0);
    assert(original);
    temp_template(base);
    assert(mkdtemp(base));
    snprintf(directory, sizeof(directory), "%s/space \xe9\x9b\xaa", base);
    snprintf(marker, sizeof(marker), "%s/relative.bin", directory);
    assert(mkdir(directory, 0700) == 0);
    assert(chdir(directory) == 0);
    int fd = open("relative.bin", O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && write(fd, "ok", 2) == 2 && close(fd) == 0);
    struct stat info;
    assert(stat(marker, &info) == 0 && info.st_size == 2);
    assert(chdir("missing-child") == -1 && errno == ENOENT);
    assert(stat("relative.bin", &info) == 0 && info.st_size == 2);
    assert(chdir(original) == 0);
    char *restored = getcwd(NULL, 0);
    assert(restored && strcmp(original, restored) == 0);
    free(restored);
    free(original);
    assert(unlink(marker) == 0 && rmdir(directory) == 0 && rmdir(base) == 0);
}

static volatile sig_atomic_t interrupt_count;

static void count_interrupt(int signal_number) {
    if (signal_number == SIGINT || signal_number == SIGABRT ||
        signal_number == SIGABRT_COMPAT) ++interrupt_count;
}

static void test_persistent_signal_action(void) {
    struct sigaction action = {0}, original, previous;
    action.sa_handler = count_interrupt;
    assert(sigaction(SIGINT, &action, &original) == 0);
    interrupt_count = 0;
    assert(raise(SIGINT) == 0 && interrupt_count == 1);
    assert(raise(SIGINT) == 0 && interrupt_count == 2);
    action.sa_handler = SIG_IGN;
    assert(sigaction(SIGINT, &action, &previous) == 0);
    assert(previous.sa_handler == count_interrupt);
    assert(raise(SIGINT) == 0 && interrupt_count == 2);
    assert(sigaction(SIGINT, &original, &previous) == 0);
    assert(previous.sa_handler == SIG_IGN);
    /* UCRT shares one action between its legacy and modern abort numbers,
     * but raise preserves the delivered number. Both must stay persistent. */
    action.sa_handler = count_interrupt;
    assert(sigaction(SIGABRT_COMPAT, &action, &original) == 0);
    interrupt_count = 0;
    assert(raise(SIGABRT) == 0 && interrupt_count == 1);
    assert(raise(SIGABRT_COMPAT) == 0 && interrupt_count == 2);
    assert(sigaction(SIGABRT_COMPAT, &original, &previous) == 0);
    assert(previous.sa_handler == count_interrupt);
}

int main(int argc, char **argv) {
    int child = win32_spawn_child(argc, argv);
    if (child >= 0) return child;
    test_persistent_signal_action();
    test_positioned_io();
    test_mappings();
    test_descriptors_and_locks();
    test_fclose_flush_lock();
    test_time_and_paths();
    test_change_directory();
    test_pipe_readiness();
    test_utf8_stream_paths();
    test_win32_sockets();
    test_win32_spawn();
    puts("Win32 POSIX compatibility tests passed");
    return 0;
}
