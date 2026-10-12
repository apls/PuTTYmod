/*
 * stdint.h shim for Visual Studio .NET 2003 (MSVC 7.1), which
 * predates the C99 integer types header.
 */
#ifndef PUTTY_COMPAT_STDINT_H
#define PUTTY_COMPAT_STDINT_H

#include <stddef.h>

typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef short int16_t;
typedef unsigned short uint16_t;
typedef int int32_t;
typedef unsigned int uint32_t;
typedef __int64 int64_t;
typedef unsigned __int64 uint64_t;

typedef int intptr_t;
typedef unsigned int uintptr_t;
typedef __int64 intmax_t;
typedef unsigned __int64 uintmax_t;

#define INT8_MIN   (-128)
#define INT8_MAX   127
#define UINT8_MAX  0xff

#define INT16_MIN  (-32768)
#define INT16_MAX  32767
#define UINT16_MAX 0xffff

#define INT32_MIN  (-2147483647-1)
#define INT32_MAX  2147483647
#define UINT32_MAX 0xffffffffu

#define INT64_MIN  (-9223372036854775807i64-1)
#define INT64_MAX  9223372036854775807i64
#define UINT64_MAX 0xffffffffffffffffui64

#define INTPTR_MIN  INT32_MIN
#define INTPTR_MAX  INT32_MAX
#define UINTPTR_MAX UINT32_MAX

#define INTMAX_MIN  INT64_MIN
#define INTMAX_MAX  INT64_MAX
#define UINTMAX_MAX UINT64_MAX

#define SIZE_MAX    (~((size_t)0))
#define PTRDIFF_MIN INT32_MIN
#define PTRDIFF_MAX INT32_MAX

#endif /* PUTTY_COMPAT_STDINT_H */
