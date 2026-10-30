/* UTF-8 path capacity used by the Win32 realpath compatibility entrypoint. */
#pragma once
#if defined(_MSC_VER) && !defined(__clang__)
#include <ds4_ucrt/limits.h>
#else
#include_next <limits.h>
#endif
#ifndef PATH_MAX
#define PATH_MAX 32768
#endif
#ifndef SSIZE_MAX
/* gufo_posix_types.h defines ssize_t as signed long long on Windows. */
#define SSIZE_MAX LLONG_MAX
#endif
