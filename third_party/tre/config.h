/* ds4 Windows ARM64 / UCRT configuration for the pinned TRE sources.
 * Force-include this file for the TRE translation units only. */
#ifndef DS4_TRE_CONFIG_H
#define DS4_TRE_CONFIG_H

#include "include/tre/tre-config.h"

#define HAVE_MALLOC_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_WCTYPE_H 1
#define HAVE_WCHAR_T 1
#define HAVE_MBRTOWC 1
#define HAVE_MBSTATE_T 1
#define HAVE_ISWCTYPE 1
#define HAVE_ISWLOWER 1
#define HAVE_ISWUPPER 1
#define HAVE_ISWBLANK 1
#define HAVE_TOWLOWER 1
#define HAVE_TOWUPPER 1
#define HAVE_WCTYPE 1
#define HAVE_WCSLEN 1
#define HAVE_WCSRTOMBS 1
#define STDC_HEADERS 1
#define TRE_REGEX_T_FIELD value
#define WCHAR_T_UNSIGNED 1

/* Approximate matching, gettext, debug allocation, and alloca are disabled. */
#endif
