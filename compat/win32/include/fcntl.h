/* Win32 shadow of <fcntl.h>; see gufo_posix.h. */
#pragma once
#if defined(_MSC_VER) && !defined(__clang__)
#include <ds4_ucrt/fcntl.h>
#else
#include_next <fcntl.h>
#endif
#include "gufo_posix.h"
