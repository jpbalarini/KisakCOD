// layout_guard.cpp -- compile-time guards for platform assumptions the decompiled engine makes silently.
//
// Part of the "does the port still match MSVC/x86?" audit (see web/README.md, "Platform assumptions"). Nothing here generates code: if the
// wasm/clang layout ever diverges from what the IDA-derived code expects, the build fails here instead of corrupting memory at runtime.
#include <universal/q_shared.h>
#include <Windows.h>
#include <stddef.h>
#include <ui_mp/ui_mp.h>
#include <server_mp/server_mp.h>

// MSVC's long double is 64-bit (the decompiled "fp2"/"st7" temporaries are just FPU registers). On wasm it would be a 128-bit soft-float
// type, 220 uses across the engine; build.py passes -Xclang -mlong-double-64 to keep it a double.
static_assert(sizeof(long double) == 8 && alignof(long double) == 8, "build.py must pass -Xclang -mlong-double-64");

// wasm32 must keep Win32's 4-byte pointers/long (the fastfile layout depends on it; also asserted in kisak_compat.h).
static_assert(sizeof(void*) == 4 && sizeof(long) == 4 && sizeof(int) == 4 && sizeof(short) == 2 && sizeof(wchar_t) == 4,
              "unexpected data model");

// ui_main_mp.cpp / ui_gameinfo_mp.cpp reach the map list through `sharedUiInfo.serverHardwareIconList[40 * n - 5119 + k]`, i.e. byte offset
// `4 * (40 * n - 5119 + k)` from the icon list = `160 * n + 4 + 4 * k` from mapList: mapList[n].mapLoadName at k = 0, levelShot at k = 37,
// active at k = 38. That only works if mapList sits immediately before the icon list and mapInfo is exactly 160 bytes.
static_assert(sizeof(mapInfo) == 160, "mapInfo layout");
static_assert(offsetof(sharedUiInfo_t, serverHardwareIconList) == offsetof(sharedUiInfo_t, mapList) + 128 * sizeof(mapInfo),
              "serverHardwareIconList must directly follow mapList");
static_assert(offsetof(mapInfo, mapLoadName) == 4 && offsetof(mapInfo, levelShot) == 152 && offsetof(mapInfo, active) == 156,
              "mapInfo member offsets");

// SV_Set/GetClientStat store 32-bit stats (index 2000..3497) at stats[4 * index - 5996]; the slots must stay inside client_t::stats.
static_assert(4 * 3497 - 5996 + 4 <= sizeof(client_t::stats), "client_t::stats too small for the 32-bit stat slots");
static_assert(4 * 2000 - 5996 == 4 + 2000, "32-bit stats must start right after the 2000 byte stats (stats[4 .. 2003])");
