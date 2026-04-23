#if defined(PLATFORM_WINDOWS)
#include <windows.h>
#include <cstdarg>
#include <cstdio>
namespace {
auto ProgramVideoTrace(const char* format, ...) -> void {
  char buffer[2048];
  int offset = snprintf(buffer, sizeof(buffer), "[bsnes-fastlink][video] ");
  if(offset < 0) offset = 0;
  va_list args;
  va_start(args, format);
  vsnprintf(buffer + offset, sizeof(buffer) - offset, format, args);
  va_end(args);
  OutputDebugStringA(buffer);
}
}
#else
namespace {
auto ProgramVideoTrace(const char* format, ...) -> void {
}
}
#endif

auto Program::updateVideoDriver(Window parent) -> void {
  auto changed = (bool)video;
  ProgramVideoTrace("updateVideoDriver: requested=%s active=%s changed=%d viewport=%p\n", (const char*)settings.video.driver, (const char*)video.driver(), changed, (void*)presentation.viewport.handle());

  video.setContext(presentation.viewport.handle());
  ProgramVideoTrace("updateVideoDriver: context cached, driver=%s ready=%d\n", (const char*)video.driver(), video.ready());

  video.create(settings.video.driver);
  ProgramVideoTrace("updateVideoDriver: after create active=%s ready=%d format=%s\n", (const char*)video.driver(), video.ready(), (const char*)video.format());

  if(changed) {
    settings.video.format = video.format();
    ProgramVideoTrace("updateVideoDriver: preserved format=%s from previous driver change\n", (const char*)settings.video.format);
  }

  ProgramVideoTrace("updateVideoDriver: update exclusive\n");
  updateVideoExclusive();
  ProgramVideoTrace("updateVideoDriver: update blocking\n");
  updateVideoBlocking();
  ProgramVideoTrace("updateVideoDriver: update flush\n");
  updateVideoFlush();
  ProgramVideoTrace("updateVideoDriver: update monitor\n");
  updateVideoMonitor();
  ProgramVideoTrace("updateVideoDriver: update format\n");
  updateVideoFormat();
  ProgramVideoTrace("updateVideoDriver: update shader\n");
  updateVideoShader();

  if(video.ready()) {
    ProgramVideoTrace("updateVideoDriver: clearing video surface\n");
    video.clear();
    updateVideoShader();
  }

  video.onUpdate([&](uint width, uint height) {
    if(!emulator->loaded()) video.clear();
  });

  if(!video.ready()) {
    ProgramVideoTrace("updateVideoDriver: FAILED requested=%s active=%s -> falling back to None\n", (const char*)settings.video.driver, (const char*)video.driver());
    MessageDialog({
      "Error: failed to initialize [", settings.video.driver, "] video driver."
    }).setAlignment(parent).error();
    settings.video.driver = "None";
    return updateVideoDriver(parent);
  }

  ProgramVideoTrace("updateVideoDriver: success active=%s ready=%d\n", (const char*)video.driver(), video.ready());
  presentation.updateShaders();
}

auto Program::updateVideoExclusive() -> void {
  video.setExclusive(settings.video.exclusive);
}

auto Program::updateVideoBlocking() -> void {
  video.setBlocking(settings.video.blocking);
}

auto Program::updateVideoFlush() -> void {
  video.setFlush(settings.video.flush);
}

auto Program::updateVideoMonitor() -> void {
  if(!video.hasMonitor(settings.video.monitor)) {
    settings.video.monitor = video.monitor();
  }
  video.setMonitor(settings.video.monitor);
}

auto Program::updateVideoFormat() -> void {
  if(!video.hasFormat(settings.video.format)) {
    settings.video.format = video.format();
  }
  video.setFormat(settings.video.format);
}

auto Program::updateVideoShader() -> void {
  video.setShader(settings.video.shader);
}

auto Program::updateVideoPalette() -> void {
  double luminance = settings.video.luminance / 100.0;
  double saturation = settings.video.saturation / 100.0;
  double gamma = settings.video.gamma / 100.0;

  uint depth = 24;
  if(video.format() == "RGB30") depth = 30;

  for(uint color : range(32768)) {
    uint16 r = (color >> 10) & 31;
    uint16 g = (color >>  5) & 31;
    uint16 b = (color >>  0) & 31;

    r = r << 3 | r >> 2; r = r << 8 | r << 0;
    g = g << 3 | g >> 2; g = g << 8 | g << 0;
    b = b << 3 | b >> 2; b = b << 8 | b << 0;

    if(saturation != 1.0) {
      uint16 grayscale = uclamp<16>((r + g + b) / 3);
      double inverse = max(0.0, 1.0 - saturation);
      r = uclamp<16>(r * saturation + grayscale * inverse);
      g = uclamp<16>(g * saturation + grayscale * inverse);
      b = uclamp<16>(b * saturation + grayscale * inverse);
    }

    if(gamma != 1.0) {
      double reciprocal = 1.0 / 32767.0;
      r = r > 32767 ? r : uint16(32767 * pow(r * reciprocal, gamma));
      g = g > 32767 ? g : uint16(32767 * pow(g * reciprocal, gamma));
      b = b > 32767 ? b : uint16(32767 * pow(b * reciprocal, gamma));
    }

    if(luminance != 1.0) {
      r = uclamp<16>(r * luminance);
      g = uclamp<16>(g * luminance);
      b = uclamp<16>(b * luminance);
    }

    switch(depth) {
    case 24: palette[color] = r >> 8 << 16 | g >> 8 <<  8 | b >> 8 << 0; break;
    case 30: palette[color] = r >> 6 << 20 | g >> 6 << 10 | b >> 6 << 0; break;
    }

    r >>= 1;
    g >>= 1;
    b >>= 1;

    switch(depth) {
    case 24: paletteDimmed[color] = r >> 8 << 16 | g >> 8 <<  8 | b >> 8 << 0; break;
    case 30: paletteDimmed[color] = r >> 6 << 20 | g >> 6 << 10 | b >> 6 << 0; break;
    }
  }

  emulator->configure("Video/ColorEmulation", false);
}

auto Program::updateVideoEffects() -> void {
  emulator->configure("Video/BlurEmulation", settings.video.blur);
}

auto Program::toggleVideoFullScreen() -> void {
  if(!video.hasFullScreen()) return;
  if(presentation.fullScreen()) return;

  video.clear();

  if(!video.fullScreen()) {
    video.setFullScreen(true);
    if(!input.acquired() && (video.exclusive() || video.hasMonitors().size() == 1)) input.acquire();
  } else {
    if(input.acquired()) input.release();
    video.setFullScreen(false);
    presentation.viewport.setFocused();
  }
}

auto Program::toggleVideoPseudoFullScreen() -> void {
  if(video.fullScreen()) return;

  if(!presentation.fullScreen()) {
    presentation.setFullScreen(true);
    presentation.menuBar.setVisible(false);
    if(!input.acquired() && video.hasMonitors().size() == 1) input.acquire();
  } else {
    if(input.acquired()) input.release();
    presentation.menuBar.setVisible(true);
    presentation.setFullScreen(false);
  }
}
