/* Internal FILE-token dispatch. Caller-owned buffers are never copied to disk. */
#ifndef DS4_WIN32_MEMORY_STREAM_H
#define DS4_WIN32_MEMORY_STREAM_H
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif
int gufo_memory_close(FILE *stream, int *result);
#ifdef __cplusplus
}
#endif
#endif
