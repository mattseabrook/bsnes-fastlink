#if defined(PLATFORM_WINDOWS) && defined(BUILD_DEBUG)
#include <windows.h>
#include <cstdarg>
#include <cstdio>
namespace {
auto ProgramVideoTrace(const char* format, ...) -> void {
}
}
#else
namespace {
auto ProgramVideoTrace(const char* format, ...) -> void {
}
}
#endif

auto Program::updateVideoDriver(Window parent) -> void {
  #if defined(PLATFORM_WINDOWS)
  fastLinkCrashStep("Program::updateVideoDriver: begin");
  fastLinkDebug(
    "Program::updateVideoDriver begin requested=%s forced=%s viewport=%p present=%s swap=%u flush=%d blocking=%d",
    (const char*)settings.video.driver,
    NativeVideoDriverName,
    (void*)presentation.viewport.handle(),
    (const char*)settings.video.presentMode,
    settings.video.swapchainImages,
    settings.video.flush,
    settings.video.blocking
  );
  settings.video.driver = NativeVideoDriverName;

  video.setContext(presentation.viewport.handle());
  fastLinkCrashStep("Program::updateVideoDriver: context set");
  fastLinkDebug("Program::updateVideoDriver context set");
  video.create(settings.video.driver);
  fastLinkCrashStep("Program::updateVideoDriver: create done");
  fastLinkDebug("Program::updateVideoDriver create done active=%s ready=%d", (const char*)video.driver(), video.ready());

  fastLinkDebug("Program::updateVideoDriver update exclusive");
  updateVideoExclusive();
  fastLinkDebug("Program::updateVideoDriver update blocking");
  updateVideoBlocking();
  fastLinkDebug("Program::updateVideoDriver update flush");
  updateVideoFlush();
  fastLinkDebug("Program::updateVideoDriver update present mode");
  updateVideoPresentMode();
  fastLinkDebug("Program::updateVideoDriver update monitor");
  updateVideoMonitor();
  fastLinkDebug("Program::updateVideoDriver update format");
  updateVideoFormat();
  fastLinkDebug("Program::updateVideoDriver update shader");
  updateVideoShader();

  if(video.ready()) {
    fastLinkDebug("Program::updateVideoDriver clear surface");
    video.clear();
  }

  video.onUpdate([&](uint width, uint height) {
    fastLinkDebug("Program::updateVideoDriver onUpdate width=%u height=%u loaded=%d", width, height, emulator && emulator->loaded());
    if(!emulator->loaded()) video.clear();
  });

  if(!video.ready()) {
    fastLinkCrashStep("Program::updateVideoDriver: failed");
    fastLinkDebug("Program::updateVideoDriver failed active=%s", (const char*)video.driver());
    MessageDialog({
      "Error: failed to initialize [", NativeVideoDriverName, "] video backend."
    }).setAlignment(parent).error();
    return;
  }

  presentation.updateShaders();
  fastLinkCrashStep("Program::updateVideoDriver: complete");
  fastLinkDebug("Program::updateVideoDriver complete active=%s ready=%d format=%s", (const char*)video.driver(), video.ready(), (const char*)video.format());
  return;
  #endif

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
  updateVideoPresentMode();
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

  #if defined(PLATFORM_WINDOWS)
  if(audio.driver() == "WASAPI" && !settings.video.blocking && !settings.audio.blocking && !fastForwarding) {
    updateAudioBlocking();
    return;
  }
  #endif

  presentation.speedMenu.setEnabled(!settings.video.blocking && settings.audio.blocking);
}

auto Program::updateVideoFlush() -> void {
  video.setFlush(settings.video.flush);
}

auto Program::updateVideoPresentMode() -> void {
  video.setPresentMode(settings.video.presentMode);
  video.setSwapchainImages(settings.video.swapchainImages);
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
    case 24: palette[color] = 255u << 24 | r >> 8 << 16 | g >> 8 <<  8 | b >> 8 << 0; break;
    case 30: palette[color] =   3u << 30 | r >> 6 << 20 | g >> 6 << 10 | b >> 6 << 0; break;
    }

    r >>= 1;
    g >>= 1;
    b >>= 1;

    switch(depth) {
    case 24: paletteDimmed[color] = 255u << 24 | r >> 8 << 16 | g >> 8 <<  8 | b >> 8 << 0; break;
    case 30: paletteDimmed[color] =   3u << 30 | r >> 6 << 20 | g >> 6 << 10 | b >> 6 << 0; break;
    }
  }

  emulator->configure("Video/ColorEmulation", false);
}

auto Program::updateVideoEffects() -> void {
  emulator->configure("Video/BlurEmulation", settings.video.blur);
}

auto Program::toggleVideoFullScreen() -> void {
  #if defined(PLATFORM_WINDOWS)
  if(!video.hasFullScreen()) {
    toggleVideoPseudoFullScreen();
    return;
  }
  #endif

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
