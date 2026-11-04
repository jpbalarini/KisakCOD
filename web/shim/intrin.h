// intrin.h shim -- MSVC intrinsics used by the engine, emulated for wasm32.
#pragma once
#include <xmmintrin.h>
#include <stdint.h>
#include <emscripten/emscripten.h>

static inline unsigned long long __rdtsc(void) {
    // No TSC on wasm: use a monotonic ~1GHz tick so the engine's cycle->ms calibration still works.
    return (unsigned long long)(emscripten_get_now() * 1000000.0);
}
static inline unsigned char _BitScanReverse(unsigned long *idx, unsigned long mask) {
    if (!mask) return 0; *idx = 31 - __builtin_clz(mask); return 1;
}
static inline unsigned char _BitScanForward(unsigned long *idx, unsigned long mask) {
    if (!mask) return 0; *idx = __builtin_ctz(mask); return 1;
}
static inline void __cpuid(int info[4], int leaf) { (void)leaf; info[0] = info[1] = info[2] = info[3] = 0; }
static inline void __cpuidex(int info[4], int leaf, int sub) { (void)leaf; (void)sub; info[0] = info[1] = info[2] = info[3] = 0; }
#define InterlockedExchangePointer(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
static inline void _mm_pause_(void) {}
static inline void _mm_empty(void) {}   // MMX/x87 state switch: no-op on wasm
static inline void _m_empty(void) {}
