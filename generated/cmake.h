/*
 * cmake.h replacement for the VC2003 / nmake build.
 *
 * Upstream generates this file from cmake/cmake.h.in; these are the
 * values the CMake configuration would pick for a 32-bit x86 Windows
 * build with Visual Studio .NET 2003 (no hardware-accelerated crypto,
 * no modern SDK features).
 */
#ifndef PUTTY_CMAKE_H
#define PUTTY_CMAKE_H

#define HAVE_COUNTOF 0
#define HAVE_WINRESRC_H 1
#define HAVE_WINRES_H 0
#define HAVE_WIN_H 0
#define HAVE_NO_STDINT_H 0
#define HAVE_AFUNIX_H 0
#define HAVE_GCP_RESULTSW 1
#define HAVE_ADDDLLDIRECTORY 0
#define HAVE_SETDEFAULTDLLDIRECTORIES 0
#define HAVE_GETNAMEDPIPECLIENTPROCESSID 0
#define HAVE_STRTOUMAX 0
#define HAVE_DWMAPI_H 0

#define NOT_X_WINDOWS

#define HAVE_AES_NI 0
#define HAVE_SHA_NI 0
#define HAVE_SHAINTRIN_H 0
#define HAVE_CLMUL 0
#define HAVE_NEON_CRYPTO 0
#define HAVE_NEON_PMULL 0
#define HAVE_NEON_VADDQ_P128 0
#define HAVE_NEON_SHA512 0
#define HAVE_NEON_SHA512_INTRINSICS 0
#define USE_ARM64_NEON_H 0
#define HAVE_ARM_DIT 0

#endif /* PUTTY_CMAKE_H */
