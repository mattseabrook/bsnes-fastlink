namespace Filter::None {

auto size(uint& width, uint& height) -> void {
  width  = width;
  height = height;
}

auto render(
  uint32_t* colortable, uint32_t* output, uint outpitch,
  const uint16_t* input, uint pitch, uint width, uint height
) -> void {
  pitch    >>= 1;
  outpitch >>= 2;

  for(uint y = 0; y < height; y++) {
    const uint16_t* in = input + y * pitch;
    uint32_t* out = output + y * outpitch;
    #if defined(SIMD_SSE) || defined(SIMD_SSE2) || defined(SIMD_SSE3) || defined(SIMD_SSSE3) || defined(SIMD_SSE4_1) || defined(SIMD_SSE4_2) || defined(SIMD_AVX) || defined(SIMD_AVX2)
    if(y + 2 < height) _mm_prefetch((const char*)(input + (y + 2) * pitch), _MM_HINT_T0);
    #endif
    for(uint x = 0; x < width; x++) {
      *out++ = colortable[*in++];
    }
  }
}

}
