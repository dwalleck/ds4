/* Win32 shadow of <time.h>; see gufo_posix.h. Leaf-only on purpose. */
#pragma once
#if defined(_MSC_VER) && !defined(__clang__)
#include <ds4_ucrt/time.h>
#else
#include_next <time.h>
#endif
#include "gufo_posix_time.h"
