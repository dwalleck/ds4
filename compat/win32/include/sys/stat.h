/* Win32 shadow of <sys/stat.h>; see gufo_posix.h. The UCRT header is parsed
 * completely before gufo_posix.h defines st_mtime and friends. */
#pragma once
#if defined(_MSC_VER) && !defined(__clang__)
#include <ds4_ucrt/sys/stat.h>
#else
#include_next <sys/stat.h>
#endif
#include "gufo_posix.h"
