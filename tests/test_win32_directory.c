/* Model-independent native Windows tests; fixtures never depend on cwd. */
#include <dirent.h>
#include <windows.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "%s:%d: %s (errno=%d, Win32=%lu)\n", \
                __FILE__, __LINE__, #expression, errno, GetLastError()); \
        abort(); \
    } \
} while (0)

enum { PATH_UNITS = 1024, UTF8_BYTES = PATH_UNITS * 3 };

static void to_utf8(const wchar_t *wide, char *utf8) {
    CHECK(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1,
                             utf8, UTF8_BYTES, NULL, NULL) > 0);
}

static void join_path(wchar_t *out, const wchar_t *base, const wchar_t *name) {
    const size_t base_length = wcslen(base);
    const size_t name_length = wcslen(name);
    CHECK(base_length + name_length + 2 <= PATH_UNITS);
    memcpy(out, base, base_length * sizeof(*out));
    out[base_length] = L'\\';
    memcpy(out + base_length + 1, name, (name_length + 1) * sizeof(*out));
}

static void extended_path(wchar_t *out, const wchar_t *path) {
    CHECK(wcslen(path) + 9 < PATH_UNITS);
    if (!wcsncmp(path, L"\\\\?\\", 4)) {
        wcscpy(out, path);
    } else if (path[0] == L'\\' && path[1] == L'\\') {
        wcscpy(out, L"\\\\?\\UNC\\");
        wcscat(out, path + 2);
    } else {
        wcscpy(out, L"\\\\?\\");
        wcscat(out, path);
    }
}

static void create_file(const wchar_t *path) {
    wchar_t native[PATH_UNITS];
    extended_path(native, path);
    HANDLE file = CreateFileW(native, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(file != INVALID_HANDLE_VALUE);
    CHECK(CloseHandle(file));
}

static int dot_entry(const char *name) {
    return !strcmp(name, ".") || !strcmp(name, "..");
}

static void check_eof(DIR *directory) {
    errno = 0;
    CHECK(readdir(directory) == NULL);
    CHECK(errno == 0);
    errno = EDOM;
    CHECK(readdir(directory) == NULL);
    CHECK(errno == EDOM);
}

static void check_entries(const char *path, const char *const *names, size_t count) {
    CHECK(count <= 8);
    unsigned seen = 0;
    errno = EDOM;
    DIR *directory = opendir(path);
    CHECK(directory != NULL);
    CHECK(errno == EDOM);
    struct dirent *storage = NULL;
    for (;;) {
        errno = EDOM;
        struct dirent *entry = readdir(directory);
        CHECK(errno == EDOM);
        if (!entry) break;
        if (storage) CHECK(entry == storage);
        storage = entry;
        if (dot_entry(entry->d_name)) continue;
        size_t i;
        for (i = 0; i < count; i++)
            if (!strcmp(entry->d_name, names[i])) break;
        CHECK(i < count);
        CHECK(!(seen & (1u << i)));
        seen |= 1u << i;
    }
    CHECK(seen == (1u << count) - 1);
    check_eof(directory);
    CHECK(closedir(directory) == 0);
    CHECK(errno == EDOM);
}

static void test_iteration(const wchar_t *root) {
    /* Include non-BMP characters in both the directory and an entry name. */
    const wchar_t directory_name[] = L"iteration-\x76ee\x5f55-\xD83D\xDE80";
    const wchar_t unicode_name[] = L"\x6587\x4ef6-\xD83D\xDE80.kv";
    wchar_t directory[PATH_UNITS], path[PATH_UNITS], longest[256];
    char utf8[UTF8_BYTES], unicode[UTF8_BYTES], long_utf8[UTF8_BYTES];
    join_path(directory, root, directory_name);
    CHECK(CreateDirectoryW(directory, NULL));
    join_path(path, directory, L"plain.kv");
    create_file(path);
    join_path(path, directory, unicode_name);
    create_file(path);
    for (size_t i = 0; i < 255; i++) longest[i] = L'\x754C';
    longest[255] = L'\0';
    join_path(path, directory, longest);
    create_file(path);
    to_utf8(unicode_name, unicode);
    to_utf8(longest, long_utf8);
    CHECK(strlen(long_utf8) == 765);
    CHECK(sizeof(((struct dirent *)0)->d_name) >= strlen(long_utf8) + 1);
    const char *names[] = {"plain.kv", unicode, long_utf8};
    to_utf8(directory, utf8);
    check_entries(utf8, names, 3);

    const size_t length = strlen(utf8);
    memcpy(utf8 + length, "\\\\", 3);
    check_entries(utf8, names, 3);
    utf8[length] = '/';
    utf8[length + 1] = '\0';
    for (size_t i = 0; i < length; i++)
        if (utf8[i] == '\\') utf8[i] = '/';
    check_entries(utf8, names, 3);

    /* Streams own all their state, not the caller's path or a global result. */
    DIR *first = opendir(utf8);
    DIR *second = opendir(utf8);
    CHECK(first && second);
    memset(utf8, 'x', strlen(utf8));
    struct dirent *first_entry = readdir(first);
    CHECK(first_entry != NULL);
    char saved[sizeof(first_entry->d_name)];
    memcpy(saved, first_entry->d_name, strlen(first_entry->d_name) + 1);
    struct dirent *second_entry = readdir(second);
    CHECK(second_entry != NULL && second_entry != first_entry);
    CHECK(!strcmp(first_entry->d_name, saved));
    CHECK(closedir(second) == 0);
    CHECK(!strcmp(first_entry->d_name, saved));
    CHECK(closedir(first) == 0);

    join_path(path, directory, L"plain.kv");
    CHECK(DeleteFileW(path));
    join_path(path, directory, unicode_name);
    CHECK(DeleteFileW(path));
    join_path(path, directory, longest);
    wchar_t native[PATH_UNITS];
    extended_path(native, path);
    CHECK(DeleteFileW(native));
    CHECK(RemoveDirectoryW(directory));
}

static void test_empty_and_errors(const wchar_t *root) {
    wchar_t directory[PATH_UNITS], path[PATH_UNITS];
    char utf8[UTF8_BYTES];
    join_path(directory, root, L"empty");
    CHECK(CreateDirectoryW(directory, NULL));
    to_utf8(directory, utf8);
    check_entries(utf8, NULL, 0);
    strcat(utf8, "/");
    check_entries(utf8, NULL, 0);

    join_path(path, directory, L"missing");
    to_utf8(path, utf8);
    errno = 0;
    CHECK(opendir(utf8) == NULL && errno == ENOENT);
    strcat(utf8, "\\");
    errno = 0;
    CHECK(opendir(utf8) == NULL && errno == ENOENT);
    join_path(path, directory, L"file");
    create_file(path);
    to_utf8(path, utf8);
    errno = 0;
    CHECK(opendir(utf8) == NULL && errno == ENOTDIR);
    CHECK(DeleteFileW(path));
    CHECK(RemoveDirectoryW(directory));

    errno = 0;
    CHECK(opendir("") == NULL && errno == ENOENT);
    errno = 0;
    CHECK(opendir(NULL) == NULL && errno == EINVAL);
    errno = 0;
    CHECK(opendir("\xC0\xAF") == NULL && errno == EILSEQ);
    errno = 0;
    CHECK(readdir(NULL) == NULL && errno == EBADF);
    errno = 0;
    CHECK(closedir(NULL) == -1 && errno == EBADF);
}

static void test_invalid_utf16_entry(const wchar_t *root) {
    /* Win32 filenames can contain an unpaired surrogate. Such a name cannot
     * be faithfully represented in UTF-8, and must cause an error, not EOF. */
    wchar_t directory[PATH_UNITS], path[PATH_UNITS];
    char utf8[UTF8_BYTES];
    join_path(directory, root, L"invalid-utf16");
    CHECK(CreateDirectoryW(directory, NULL));
    const wchar_t invalid_name[] = {0xD800, 0};
    join_path(path, directory, invalid_name);
    create_file(path);
    to_utf8(directory, utf8);
    DIR *stream = opendir(utf8);
    CHECK(stream != NULL);
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(stream);
        if (!entry) break;
        CHECK(errno == 0);
        CHECK(dot_entry(entry->d_name));
    }
    CHECK(errno == EILSEQ);
    errno = 0;
    CHECK(readdir(stream) == NULL && errno == EILSEQ);
    CHECK(closedir(stream) == 0);
    CHECK(DeleteFileW(path));
    CHECK(RemoveDirectoryW(directory));
}

static DWORD handle_count(void) {
    DWORD count;
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &count));
    return count;
}

static void test_handle_ownership(const wchar_t *root) {
    char utf8[UTF8_BYTES];
    to_utf8(root, utf8);
    check_entries(utf8, NULL, 0); /* Warm up native/CRT state before counting. */
    const DWORD baseline = handle_count();
    for (int i = 0; i < 64; i++) {
        DIR *directory = opendir(utf8);
        CHECK(directory != NULL);
        if (i & 1) {
            while (readdir(directory)) {}
            check_eof(directory);
        }
        CHECK(closedir(directory) == 0);
    }
    CHECK(handle_count() == baseline);
}

static void test_extended_path(const wchar_t *root) {
    wchar_t extended[PATH_UNITS], paths[4][PATH_UNITS], component[71];
    extended_path(extended, root);
    for (size_t i = 0; i < 70; i++) component[i] = L'x';
    component[70] = L'\0';
    for (int i = 0; i < 4; i++) {
        join_path(paths[i], i ? paths[i - 1] : extended, component);
        CHECK(CreateDirectoryW(paths[i], NULL));
    }
    CHECK(wcslen(paths[3]) > MAX_PATH);
    char utf8[UTF8_BYTES];
    to_utf8(paths[3], utf8);
    check_entries(utf8, NULL, 0);
    strcat(utf8, "\\");
    check_entries(utf8, NULL, 0);
    for (int i = 3; i >= 0; i--) CHECK(RemoveDirectoryW(paths[i]));
}

int main(void) {
    wchar_t temporary[MAX_PATH], root[PATH_UNITS];
    const DWORD length = GetTempPathW(MAX_PATH, temporary);
    CHECK(length > 0 && length < MAX_PATH);
    CHECK(GetTempFileNameW(temporary, L"ds4", 0, root));
    CHECK(DeleteFileW(root));
    CHECK(CreateDirectoryW(root, NULL));
    test_iteration(root);
    test_empty_and_errors(root);
    test_invalid_utf16_entry(root);
    test_extended_path(root);
    test_handle_ownership(root);
    CHECK(RemoveDirectoryW(root));
    puts("win32 directory tests passed");
    return 0;
}
