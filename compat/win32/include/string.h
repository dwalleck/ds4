/* UCRT aliases are disabled; expose only the POSIX string subset used by C. */
#pragma once
#if defined(_MSC_VER) && !defined(__clang__)
#include <ds4_ucrt/string.h>
#else
#include_next <string.h>
#endif
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
char* strndup(const char* text, size_t limit);
#ifdef __cplusplus
}
#endif
#ifndef strdup
#define strdup _strdup
#endif
