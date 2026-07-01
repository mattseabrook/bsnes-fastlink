#pragma once

namespace nall {

#if defined(__AVX2__) || defined(_M_AVX2)
  #define SIMD 256
  #define SIMD_AVX2
  #include <immintrin.h>
#elif defined(__AVX__) || defined(_M_AVX)
  #define SIMD 256
  #define SIMD_AVX
  #include <immintrin.h>
#elif defined(__SSE4_2__)
  #define SIMD 128
  #define SIMD_SSE4_2
  #include <nmmintrin.h>
#elif defined(__SSE4_1__)
  #define SIMD 128
  #define SIMD_SSE4_1
  #include <smmintrin.h>
#elif defined(__SSSE3__)
  #define SIMD 128
  #define SIMD_SSSE3
  #include <tmmintrin.h>
#elif defined(__SSE3__)
  #define SIMD 128
  #define SIMD_SSE3
  #include <pmmintrin.h>
#elif defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
  #define SIMD 128
  #define SIMD_SSE2
  #include <emmintrin.h>
#elif defined(__SSE__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
  #define SIMD 128
  #define SIMD_SSE
  #include <xmmintrin.h>
#endif

}
