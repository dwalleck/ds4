/* ds4-owned C implementation; no third-party implementation is copied. */
#include <dirent.h>
#include <windows.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

struct ds4_win32_directory {
    HANDLE search;
    WIN32_FIND_DATAW data;
    struct dirent entry;
    int pending;
    int finished;
    int error;
};

static int directory_error(DWORD error) {
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
    case ERROR_INVALID_NAME: return ENOENT;
    case ERROR_DIRECTORY: return ENOTDIR;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION: return EACCES;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY: return ENOMEM;
    case ERROR_FILENAME_EXCED_RANGE:
    case ERROR_BUFFER_OVERFLOW:
    case ERROR_INSUFFICIENT_BUFFER: return ENAMETOOLONG;
    case ERROR_NO_UNICODE_TRANSLATION: return EILSEQ;
    case ERROR_INVALID_HANDLE: return EBADF;
    case ERROR_INVALID_PARAMETER: return EINVAL;
    default: return EIO;
    }
}

/* Resolve before adding the extended-path prefix: that namespace does not
 * normalize relative components or forward slashes. Leave room for "\\*". */
static wchar_t *directory_path(const char *path) {
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         path, -1, NULL, 0);
    if (!count) {
        errno = directory_error(GetLastError());
        return NULL;
    }
    if (count > 32767) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    wchar_t *wide = malloc((size_t)count * sizeof(*wide));
    if (!wide) {
        errno = ENOMEM;
        return NULL;
    }
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                             path, -1, wide, count)) {
        const DWORD error = GetLastError();
        free(wide);
        errno = directory_error(error);
        return NULL;
    }
    for (int i = 0; i < count; i++)
        if (wide[i] == L'/') wide[i] = L'\\';
    const DWORD required = GetFullPathNameW(wide, 0, NULL, NULL);
    if (!required || required > 32767) {
        const int error = required ? ENAMETOOLONG : directory_error(GetLastError());
        free(wide);
        errno = error;
        return NULL;
    }
    wchar_t *result = malloc(((size_t)required + 11) * sizeof(*result));
    if (!result) {
        free(wide);
        errno = ENOMEM;
        return NULL;
    }
    /* Eight reserved units accommodate the longest prefix, "\\\\?\\UNC\\". */
    wchar_t *absolute = result + 8;
    const DWORD copied = GetFullPathNameW(wide, required, absolute, NULL);
    const DWORD error = GetLastError();
    free(wide);
    if (!copied || copied >= required) {
        free(result);
        errno = copied ? ENAMETOOLONG : directory_error(error);
        return NULL;
    }
    size_t length = copied;
    const size_t root_length = length >= 7 && absolute[0] == L'\\' &&
        absolute[1] == L'\\' && absolute[3] == L'\\' &&
        absolute[5] == L':' && absolute[6] == L'\\' ? 7 : 3;
    while (length > root_length && absolute[length - 1] == L'\\')
        absolute[--length] = L'\0';
    if (length >= 4 && absolute[0] == L'\\' && absolute[1] == L'\\' &&
        (absolute[2] == L'?' || absolute[2] == L'.') && absolute[3] == L'\\') {
        memmove(result, absolute, (length + 1) * sizeof(*result));
    } else if (absolute[0] == L'\\' && absolute[1] == L'\\') {
        memmove(result + 8, absolute + 2, (length - 1) * sizeof(*result));
        memcpy(result, L"\\\\?\\UNC\\", 8 * sizeof(*result));
    } else {
        memmove(result + 4, absolute, (length + 1) * sizeof(*result));
        memcpy(result, L"\\\\?\\", 4 * sizeof(*result));
    }
    if (wcslen(result) + 3 > 32767) {
        free(result);
        errno = ENAMETOOLONG;
        return NULL;
    }
    return result;
}

DIR *opendir(const char *path) {
    if (!path || !path[0]) {
        errno = path ? ENOENT : EINVAL;
        return NULL;
    }
    const int saved_errno = errno;
    wchar_t *pattern = directory_path(path);
    if (!pattern) return NULL;
    const DWORD attributes = GetFileAttributesW(pattern);
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        const int error = attributes == INVALID_FILE_ATTRIBUTES
                        ? directory_error(GetLastError()) : ENOTDIR;
        free(pattern);
        errno = error;
        return NULL;
    }
    const size_t length = wcslen(pattern);
    size_t end = length;
    if (pattern[end - 1] != L'\\') pattern[end++] = L'\\';
    pattern[end++] = L'*';
    pattern[end] = L'\0';
    DIR *directory = calloc(1, sizeof(*directory));
    if (!directory) {
        free(pattern);
        errno = ENOMEM;
        return NULL;
    }
    directory->search = FindFirstFileW(pattern, &directory->data);
    const DWORD error = GetLastError();
    free(pattern);
    if (directory->search == INVALID_HANDLE_VALUE) {
        /* FindFirstFile may report no entries for a valid empty directory. */
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_NO_MORE_FILES) {
            free(directory);
            errno = directory_error(error);
            return NULL;
        }
        directory->finished = 1;
    } else {
        directory->pending = 1;
    }
    errno = saved_errno;
    return directory;
}

struct dirent *readdir(DIR *directory) {
    if (!directory) {
        errno = EBADF;
        return NULL;
    }
    if (directory->error) {
        errno = directory->error;
        return NULL;
    }
    if (directory->finished) return NULL;
    const int saved_errno = errno;
    if (!directory->pending &&
        !FindNextFileW(directory->search, &directory->data)) {
        const DWORD error = GetLastError();
        directory->finished = 1;
        if (error != ERROR_NO_MORE_FILES) {
            directory->error = directory_error(error);
            errno = directory->error;
        } else {
            errno = saved_errno;
        }
        return NULL;
    }
    directory->pending = 0;
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            directory->data.cFileName, -1, directory->entry.d_name,
            (int)sizeof(directory->entry.d_name), NULL, NULL)) {
        /* Never return a truncated name or silently skip invalid UTF-16. */
        directory->error = directory_error(GetLastError());
        errno = directory->error;
        return NULL;
    }
    errno = saved_errno;
    return &directory->entry;
}

int closedir(DIR *directory) {
    if (!directory) {
        errno = EBADF;
        return -1;
    }
    const int saved_errno = errno;
    const BOOL closed = directory->search == INVALID_HANDLE_VALUE ||
                        FindClose(directory->search);
    const DWORD error = GetLastError();
    free(directory);
    if (!closed) {
        errno = directory_error(error);
        return -1;
    }
    errno = saved_errno;
    return 0;
}
