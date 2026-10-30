/* Win32 shadow of <sys/types.h>; see gufo_posix.h. Leaf-only on purpose. */
#pragma once
#if defined(_MSC_VER) && !defined(__clang__)
#include <ds4_ucrt/sys/types.h>
#else
#include_next <sys/types.h>
#endif
#include "gufo_posix_types.h"
