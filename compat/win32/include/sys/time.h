/* Win32 shadow of <sys/time.h>; see gufo_posix.h. */
#pragma once
#include "gufo_posix.h"
#include "gufo_socket.h" /* struct timeval */
#ifdef __cplusplus
extern "C" {
#endif
int gettimeofday(struct timeval* value, void* timezone);
#ifdef __cplusplus
}
#endif
