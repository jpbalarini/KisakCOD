// io.h shim (MSVC CRT) -- directory enumeration (_findfirst64i32 etc.) is implemented in win32_shim.cpp
#pragma once
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define _A_SUBDIR 0x10
#define _A_RDONLY 0x01
struct _finddata64i32_t { unsigned attrib; long long time_create, time_access, time_write; unsigned long size; char name[260]; };
typedef struct _finddata64i32_t _finddata_t;
intptr_t _findfirst64i32(const char *filespec, struct _finddata64i32_t *fileinfo);
int _findnext64i32(intptr_t handle, struct _finddata64i32_t *fileinfo);
int _findclose(intptr_t handle);
#define _findfirst _findfirst64i32
#define _findnext _findnext64i32
#ifdef __cplusplus
}
#endif
