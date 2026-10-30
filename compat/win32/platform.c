#include "platform.h"
#include <windows.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

int ds4_win32_temp_path(char *path, size_t capacity, const char *name) {
    if (!path || !capacity || capacity > INT_MAX || !name ||
        strchr(name, '/') || strchr(name, '\\')) { errno = EINVAL; return -1; }
    DWORD needed = GetTempPathW(0, NULL);
    if (!needed) { errno = EIO; return -1; }
    wchar_t *wide = malloc((size_t)needed * sizeof(*wide));
    if (!wide) { errno = ENOMEM; return -1; }
    DWORD length = GetTempPathW(needed, wide);
    if (!length || length >= needed) { free(wide); errno = EIO; return -1; }
    int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1,
                                    path, (int)capacity, NULL, NULL);
    free(wide);
    if (!bytes) { errno = ENAMETOOLONG; return -1; }
    size_t n = strlen(name);
    if (n > capacity - (size_t)bytes) { errno = ENAMETOOLONG; return -1; }
    memcpy(path + bytes - 1, name, n + 1);
    return 0;
}
