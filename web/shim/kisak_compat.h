// kisak_compat.h -- force-included (-include) into every translation unit of the web build.
// Maps MSVC-isms used by the decompiled engine onto clang/emscripten equivalents.
// NOTE: wasm32 has 4-byte pointers and 4-byte `long`, exactly like the Win32 x86 target the
// engine (and the fastfile on-disk layout) was written for. Keep it that way.
#pragma once

#ifndef __EMSCRIPTEN__
#error "kisak_compat.h is only meant for the emscripten build"
#endif

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <alloca.h>
#include <limits.h>
#include <float.h>

_Static_assert(sizeof(void*) == 4, "web build must be wasm32 so pointers match the fastfile layout");
_Static_assert(sizeof(long) == 4, "DWORD/LONG assumptions");

#ifdef __cplusplus

#define __forceinline inline __attribute__((always_inline))
#define __debugbreak() __builtin_trap()
#define _alloca alloca
#define _countof(a) (sizeof(a) / sizeof((a)[0]))

// CRT name mapping (MSVC -> POSIX)
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define stricmp strcasecmp
#define strnicmp strncasecmp
#define _strdup strdup
#define _snprintf snprintf
#define _vsnprintf vsnprintf
#define _isnan(x) isnan(x)
#define _finite(x) isfinite(x)
#define _unlink unlink
#define _fileno fileno
// The engine takes its base path from the current directory (Sys_Cwd): report the install root instead of the process cwd.
#ifdef __cplusplus
extern "C" char *kisak_getcwd(char *buf, size_t size);
#else
char *kisak_getcwd(char *buf, size_t size);
#endif
#define _getcwd kisak_getcwd
#define _chdir chdir
#define _access access

// emscripten's libc already declares strlwr/strupr (compat/string.h); implemented in win32_shim.cpp
#define _strlwr strlwr
#define _strupr strupr
extern "C" char* _itoa(int value, char* buffer, int radix);

// Engine file paths are Windows-style (backslashes, any letter case). Route stdio through a path-normalising layer
// (implemented in win32_shim.cpp) so the unmodified engine can open "zone\\english\\common_mp.ff" on a POSIX/web FS.
#include <sys/stat.h>
extern "C" FILE* kisak_fopen(const char* path, const char* mode);
extern "C" int kisak_stat(const char* path, struct stat* st);
#define fopen kisak_fopen
#define stat(path, st) kisak_stat((path), (st))

// The engine declares `float random()` at global scope, which clashes with libc's `long random(void)`.
// stdlib.h is already included above, so renaming from here on only affects engine code.
#define random kisak_random


#define CPUSTRING "web-wasm32"

// ---- MSVC CRT extras ----
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
typedef long long __time64_t;
static inline __time64_t _time64(__time64_t* t) { time_t r = time(nullptr); if (t) *t = r; return r; }
static inline char* _ctime64(const __time64_t* t) { time_t tt = (time_t)*t; return ctime(&tt); }
static inline struct tm* _localtime64(const __time64_t* t) { time_t tt = (time_t)*t; return localtime(&tt); }
#define _mkdir(p) mkdir((p), 0777)
#define _rmdir rmdir
#define _putenv putenv
typedef int errno_t;
static inline errno_t fopen_s(FILE** f, const char* name, const char* mode) { *f = fopen(name, mode); return *f ? 0 : errno; }
template <size_t N> static inline errno_t strcpy_s(char (&d)[N], const char* s) { strncpy(d, s, N - 1); d[N - 1] = 0; return 0; }
static inline errno_t strcpy_s(char* d, size_t n, const char* s) { strncpy(d, s, n - 1); d[n - 1] = 0; return 0; }
template <size_t N> static inline int sprintf_s(char (&d)[N], const char* fmt, ...) { va_list a; va_start(a, fmt); int r = vsnprintf(d, N, fmt, a); va_end(a); return r; }
static inline int sprintf_s(char* d, size_t n, const char* fmt, ...) { va_list a; va_start(a, fmt); int r = vsnprintf(d, n, fmt, a); va_end(a); return r; }
#define sscanf_s sscanf
typedef FILE _iobuf;
static inline int _vsnprintf_s(char* d, size_t n, size_t count, const char* fmt, va_list a) { (void)count; return vsnprintf(d, n, fmt, a); }
#define PF_NON_TEMPORAL_LEVEL_ALL 0
#define PreFetchCacheLine(level, addr) ((void)(level), (void)(addr))

// MSVC <windows.h> defines min/max macros that accept mixed types; emulate with templates (not macros,
// so libc++'s std::min/std::max keep working).
#include <type_traits>
template <class A, class B> static inline auto max(A a, B b) -> typename std::common_type<A, B>::type { return a > b ? a : b; }
template <class A, class B> static inline auto min(A a, B b) -> typename std::common_type<A, B>::type { return a < b ? a : b; }

#endif // __cplusplus

