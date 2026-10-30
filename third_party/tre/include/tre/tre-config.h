/* ds4 configuration for TRE's public ABI; BSD-2-Clause, see ../../LICENSE.
 * Character conversion/classes/case folding use the current UCRT locale.
 * In the initial C locale, narrow input is matched byte-for-byte. */
#ifndef DS4_TRE_PUBLIC_CONFIG_H
#define DS4_TRE_PUBLIC_CONFIG_H

#include <stddef.h>
#define HAVE_SYS_TYPES_H 1
#define HAVE_WCHAR_H 1
#define TRE_WCHAR 1
#define TRE_MULTIBYTE 1
#define TRE_VERSION "0.9.0"
#define TRE_VERSION_1 0
#define TRE_VERSION_2 9
#define TRE_VERSION_3 0

#endif
