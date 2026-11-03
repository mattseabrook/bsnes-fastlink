// Presentation-only 2x affine sampling alongside the cycle-rendered PPU.
// No additional guest clocks, memory-mapped accesses, or scheduler transitions.
// The existing 512x480 buffer already has room for four samples per low-res
// pixel, so this requires no framebuffer allocation, copy, or larger upload.
auto PPU::Screen::runMode7HD(const Math& previousMath) -> void {
  const auto originalBG1 = ppu.bg1.output;
  const auto originalBG2 = ppu.bg2.output;
  const auto originalMath = math;
  const auto originalCgramAddress = ppu.latch.cgramAddress;

  const int x = (ppu.hcounter() - 58) >> 2;
  const int y = ppu.vcounter();
  const int a = (int16)ppu.io.m7a, b = (int16)ppu.io.m7b;
  const int c = (int16)ppu.io.m7c, d = (int16)ppu.io.m7d;
  const int centerX = (int13)ppu.io.m7x, centerY = (int13)ppu.io.m7y;
  const int offsetX = (int13)ppu.io.hoffsetMode7, offsetY = (int13)ppu.io.voffsetMode7;
  const int flipX = ppu.io.hflipMode7 ? -1 : 1;
  const int flipY = ppu.io.vflipMode7 ? -1 : 1;
  const int nativeX = flipX < 0 ? 255 - x : x;
  const int nativeY = flipY < 0 ? 255 - y : y;
  auto clip = [](int n) -> int { return n & 0x2000 ? n | ~1023 : n & 1023; };
  const int originX = (a * clip(offsetX - centerX) & ~63)
    + (b * clip(offsetY - centerY) & ~63) + (b * nativeY & ~63) + centerX * 256;
  const int originY = (c * clip(offsetX - centerX) & ~63)
    + (d * clip(offsetY - centerY) & ~63) + (d * nativeY & ~63) + centerY * 256;

  // Use the same native window decisions for each subpixel. Sprites, color
  // windows and non-Mode-7 layers keep their original pixel boundaries.
  auto windowAllows = [&](const auto& layer, bool above) -> bool {
    const auto& window = ppu.window.io;
    bool one = x >= window.oneLeft && x <= window.oneRight;
    bool two = x >= window.twoLeft && x <= window.twoRight;
    bool masked = ppu.window.test(layer.oneEnable, one ^ layer.oneInvert,
      layer.twoEnable, two ^ layer.twoInvert, layer.mask);
    return !masked || !(above ? layer.aboveEnable : layer.belowEnable);
  };
  const bool bg1Above = ppu.bg1.io.aboveEnable && windowAllows(ppu.window.io.bg1, true);
  const bool bg1Below = ppu.bg1.io.belowEnable && windowAllows(ppu.window.io.bg1, false);
  const bool bg2Above = ppu.bg2.io.aboveEnable && windowAllows(ppu.window.io.bg2, true);
  const bool bg2Below = ppu.bg2.io.belowEnable && windowAllows(ppu.window.io.bg2, false);

  for(int sy = 0; sy < 2; sy++) {
    auto target = sy ? lineB : lineA;
    for(int sx = 0; sx < 2; sx++) {
      if(sx == 0 && sy == 0) continue;  // reuse the hardware sample
      const int pixelX = ((originX + a * nativeX) * 2 + a * flipX * sx + b * flipY * sy) >> 9;
      const int pixelY = ((originY + c * nativeX) * 2 + c * flipX * sx + d * flipY * sy) >> 9;
      const bool outside = (pixelX | pixelY) & ~1023;
      const uint tileAddress = ((pixelY >> 3 & 127) << 7) | (pixelX >> 3 & 127);
      const uint paletteAddress = ((pixelY & 7) << 3) | (pixelX & 7);
      const uint tile = ppu.io.repeatMode7 == 3 && outside ? 0 : ppu.vram[tileAddress] & 255;
      const uint palette = ppu.io.repeatMode7 == 2 && outside ? 0 : ppu.vram[(tile << 6) | paletteAddress] >> 8;

      ppu.bg1.output = {};
      ppu.bg2.output = {};
      if(palette) {
        Background::Pixel pixel{};
        pixel.palette = palette;
        pixel.priority = ppu.bg1.io.priority[0];
        if(bg1Above) ppu.bg1.output.above = pixel;
        if(bg1Below) ppu.bg1.output.below = pixel;
      }
      if(ppu.io.extbg && (palette & 127)) {
        Background::Pixel pixel{};
        pixel.palette = palette & 127;
        pixel.priority = ppu.bg2.io.priority[palette >> 7];
        if(bg2Above) ppu.bg2.output.above = pixel;
        if(bg2Below) ppu.bg2.output.below = pixel;
      }

      math = previousMath;
      below(false);
      target[sx] = ppu.lightTable[ppu.io.displayBrightness][above()];
    }
  }

  ppu.bg1.output = originalBG1;
  ppu.bg2.output = originalBG2;
  math = originalMath;
  ppu.latch.cgramAddress = originalCgramAddress;
}
