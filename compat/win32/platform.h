#ifndef DS4_WIN32_PLATFORM_H
#define DS4_WIN32_PLATFORM_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* UTF-8 path under the current user's Windows temporary directory. */
int ds4_win32_temp_path(char *path, size_t capacity, const char *name);
#ifdef __cplusplus
}
#endif
#endif
