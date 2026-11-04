// smmintrin.h wrapper: emscripten's SIMD compat headers declare `struct __unaligned`, which collides with
// the MS-extension keyword __unaligned that -fms-extensions turns on.
#pragma once
#define __unaligned __kisak_unaligned_tag
#include_next <smmintrin.h>
#undef __unaligned
