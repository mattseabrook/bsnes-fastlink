#include <nall/encode/bmp.hpp>
#include <heuristics/heuristics.hpp>
#include <heuristics/heuristics.cpp>
#include <heuristics/super-famicom.cpp>
#include <heuristics/game-boy.cpp>
#include <heuristics/bs-memory.cpp>
#include <heuristics/sufami-turbo.cpp>

namespace {
constexpr uint NativeGamepadDeviceID = 1;
}

//ROM data is held in memory to support compressed archives, soft-patching, and game hacks
auto Program::open(uint id, string name, vfs::file::mode mode, bool required) -> shared_pointer<vfs::file> {
  static uint openTraceCounter = 0;
  bool traceOpen = openTraceCounter++ < 120;
  fastLinkCrashStep("Program::open: begin");
  if(traceOpen) {
    fastLinkDebug(
      "Program::open begin id=%u name=%s mode=%u required=%d",
      id,
      (const char*)name,
      (uint)mode,
      required
    );
  }
  shared_pointer<vfs::file> result;

  if(id == 0) {  //System
    if(name == "boards.bml" && mode == vfs::file::mode::read) {
      result = vfs::memory::file::open(Resource::System::Boards, sizeof(Resource::System::Boards));
    }

    if(name == "ipl.rom" && mode == vfs::file::mode::read) {
      result = vfs::memory::file::open(Resource::System::IPLROM, sizeof(Resource::System::IPLROM));
    }
  }

  if(id == 1) {  //Super Famicom
    if(name == "manifest.bml" && mode == vfs::file::mode::read) {
      result = vfs::memory::file::open(superFamicom.manifest.data<uint8_t>(), superFamicom.manifest.size());
    } else if(name == "program.rom" && mode == vfs::file::mode::read) {
      result = vfs::memory::file::open(superFamicom.program.data(), superFamicom.program.size());
    } else if(name == "data.rom" && mode == vfs::file::mode::read) {
      result = vfs::memory::file::open(superFamicom.data.data(), superFamicom.data.size());
    } else if(name == "expansion.rom" && mode == vfs::file::mode::read) {
      result = vfs::memory::file::open(superFamicom.expansion.data(), superFamicom.expansion.size());
    } else if(superFamicom.location.endsWith("/")) {
      result = openPakSuperFamicom(name, mode);
    } else {
      result = openRomSuperFamicom(name, mode);
    }
  }

  if(id == 2) {  //Game Boy
    if(name == "manifest.bml" && mode == vfs::file::mode::read) {
      result = vfs::memory::file::open(gameBoy.manifest.data<uint8_t>(), gameBoy.manifest.size());
    } else if(name == "program.rom" && mode == vfs::file::mode::read) {
      result = vfs::memory::file::open(gameBoy.program.data(), gameBoy.program.size());
    } else if(gameBoy.location.endsWith("/")) {
      result = openPakGameBoy(name, mode);
    } else {
      result = openRomGameBoy(name, mode);
    }
  }

  if(id == 3) {  //BS Memory
    if(name == "manifest.bml" && mode == vfs::file::mode::read) {
      result = vfs::memory::file::open(bsMemory.manifest.data<uint8_t>(), bsMemory.manifest.size());
    } else if(name == "program.rom" && mode == vfs::file::mode::read) {
      result = vfs::memory::file::open(bsMemory.program.data(), bsMemory.program.size());
    } else if(name == "program.flash") {
      //writes are not flushed to disk in bsnes
      result = vfs::memory::file::open(bsMemory.program.data(), bsMemory.program.size());
    } else if(bsMemory.location.endsWith("/")) {
      result = openPakBSMemory(name, mode);
    } else {
      result = openRomBSMemory(name, mode);
    }
  }

  if(id == 4) {  //Sufami Turbo - Slot A
    if(name == "manifest.bml" && mode == vfs::file::mode::read) {
      result = vfs::memory::file::open(sufamiTurboA.manifest.data<uint8_t>(), sufamiTurboA.manifest.size());
    } else if(name == "program.rom" && mode == vfs::file::mode::read) {
      result = vfs::memory::file::open(sufamiTurboA.program.data(), sufamiTurboA.program.size());
    } else if(sufamiTurboA.location.endsWith("/")) {
      result = openPakSufamiTurboA(name, mode);
    } else {
      result = openRomSufamiTurboA(name, mode);
    }
  }

  if(id == 5) {  //Sufami Turbo - Slot B
    if(name == "manifest.bml" && mode == vfs::file::mode::read) {
      result = vfs::memory::file::open(sufamiTurboB.manifest.data<uint8_t>(), sufamiTurboB.manifest.size());
    } else if(name == "program.rom" && mode == vfs::file::mode::read) {
      result = vfs::memory::file::open(sufamiTurboB.program.data(), sufamiTurboB.program.size());
    } else if(sufamiTurboB.location.endsWith("/")) {
      result = openPakSufamiTurboB(name, mode);
    } else {
      result = openRomSufamiTurboB(name, mode);
    }
  }

  if(!result && required) {
    if(traceOpen) fastLinkDebug("Program::open missing required id=%u name=%s", id, (const char*)name);
    MessageDialog({"Error: missing required data: ", name}).setAlignment(*presentation).error();
  }

  if(traceOpen) fastLinkDebug("Program::open done id=%u name=%s result=%d", id, (const char*)name, (bool)result);
    fastLinkCrashStep("Program::open: done");
  return result;
}

auto Program::load(uint id, string name, string type, vector<string> options) -> Emulator::Platform::Load {
    fastLinkCrashStep("Program::platformLoad: begin");
  fastLinkDebug(
    "Program::platformLoad begin id=%u name=%s type=%s queued=%d",
    id,
    (const char*)name,
    (const char*)type,
    (bool)gameQueue
  );
  BrowserDialog dialog;
  dialog.setAlignment(*presentation);
  dialog.setOptions(options);

  if(id == 1 && name == "Super Famicom" && type == "sfc") {
    if(gameQueue) {
      auto game = gameQueue.takeLeft().split(";", 1L);
      superFamicom.option = game(0);
      superFamicom.location = game(1);
    } else {
      dialog.setTitle("Load SNES ROM");
      dialog.setPath(path("Games", settings.path.recent.superFamicom));
      dialog.setFilters({string{"SNES ROMs|*.sfc:*.smc:*.zip:*.7z:*.SFC:*.SMC:*.ZIP:*.7Z:*.Sfc:*.Smc:*.Zip"}, string{"All Files|*"}});
      superFamicom.location = openGame(dialog);
      superFamicom.option = dialog.option();
    }
    if(inode::exists(superFamicom.location)) {
      fastLinkCrashStep("Program::platformLoad: super famicom load");
      settings.path.recent.superFamicom = Location::dir(superFamicom.location);
      if(loadSuperFamicom(superFamicom.location)) {
        fastLinkDebug("Program::platformLoad Super Famicom done option=%s", (const char*)superFamicom.option);
        return {id, superFamicom.option};
      }
      fastLinkDebug("Program::platformLoad Super Famicom load failed");
    }
  }

  if(id == 2 && name == "Game Boy" && type == "gb") {
    if(gameQueue) {
      auto game = gameQueue.takeLeft().split(";", 1L);
      gameBoy.option = game(0);
      gameBoy.location = game(1);
    } else {
      dialog.setTitle("Load Game Boy ROM");
      dialog.setPath(path("Games", settings.path.recent.gameBoy));
      dialog.setFilters({string{"Game Boy ROMs|*.gb:*.gbc:*.zip:*.7z:*.GB:*.GBC:*.ZIP:*.7Z:*.Gb:*.Gbc:*.Zip"}, string{"All Files|*"}});
      gameBoy.location = openGame(dialog);
      gameBoy.option = dialog.option();
    }
    if(inode::exists(gameBoy.location)) {
      fastLinkCrashStep("Program::platformLoad: game boy load");
      settings.path.recent.gameBoy = Location::dir(gameBoy.location);
      if(loadGameBoy(gameBoy.location)) {
        fastLinkDebug("Program::platformLoad Game Boy done option=%s", (const char*)gameBoy.option);
        return {id, gameBoy.option};
      }
      fastLinkDebug("Program::platformLoad Game Boy load failed");
    }
  }

  if(id == 3 && name == "BS Memory" && type == "bs") {
    if(gameQueue) {
      auto game = gameQueue.takeLeft().split(";", 1L);
      bsMemory.option = game(0);
      bsMemory.location = game(1);
    } else {
      dialog.setTitle("Load BS Memory ROM");
      dialog.setPath(path("Games", settings.path.recent.bsMemory));
      dialog.setFilters({string{"BS Memory ROMs|*.bs:*.zip:*.7z:*.BS:*.ZIP:*.7Z:*.Bs:*.Zip"}, string{"All Files|*"}});
      bsMemory.location = openGame(dialog);
      bsMemory.option = dialog.option();
    }
    if(inode::exists(bsMemory.location)) {
      fastLinkCrashStep("Program::platformLoad: bs memory load");
      settings.path.recent.bsMemory = Location::dir(bsMemory.location);
      if(loadBSMemory(bsMemory.location)) {
        fastLinkDebug("Program::platformLoad BS Memory done option=%s", (const char*)bsMemory.option);
        return {id, bsMemory.option};
      }
      fastLinkDebug("Program::platformLoad BS Memory load failed");
    }
  }

  if(id == 4 && name == "Sufami Turbo" && type == "st") {
    if(gameQueue) {
      auto game = gameQueue.takeLeft().split(";", 1L);
      sufamiTurboA.option = game(0);
      sufamiTurboA.location = game(1);
    } else {
      dialog.setTitle("Load Sufami Turbo ROM - Slot A");
      dialog.setPath(path("Games", settings.path.recent.sufamiTurboA));
      dialog.setFilters({string{"Sufami Turbo ROMs|*.st:*.zip:*.7z:*.ST:*.ZIP:*.7Z:*.St:*.Zip"}, string{"All Files|*"}});
      sufamiTurboA.location = openGame(dialog);
      sufamiTurboA.option = dialog.option();
    }
    if(inode::exists(sufamiTurboA.location)) {
      fastLinkCrashStep("Program::platformLoad: sufami turbo A load");
      settings.path.recent.sufamiTurboA = Location::dir(sufamiTurboA.location);
      if(loadSufamiTurboA(sufamiTurboA.location)) {
        fastLinkDebug("Program::platformLoad Sufami Turbo A done option=%s", (const char*)sufamiTurboA.option);
        return {id, sufamiTurboA.option};
      }
      fastLinkDebug("Program::platformLoad Sufami Turbo A load failed");
    }
  }

  if(id == 5 && name == "Sufami Turbo" && type == "st") {
    if(gameQueue) {
      auto game = gameQueue.takeLeft().split(";", 1L);
      sufamiTurboB.option = game(0);
      sufamiTurboB.location = game(1);
    } else {
      dialog.setTitle("Load Sufami Turbo ROM - Slot B");
      dialog.setPath(path("Games", settings.path.recent.sufamiTurboB));
      dialog.setFilters({string{"Sufami Turbo ROMs|*.st:*.zip:*.7z:*.ST:*.ZIP:*.7Z:*.St:*.Zip"}, string{"All Files|*"}});
      sufamiTurboB.location = openGame(dialog);
      sufamiTurboB.option = dialog.option();
    }
    if(inode::exists(sufamiTurboB.location)) {
      fastLinkCrashStep("Program::platformLoad: sufami turbo B load");
      settings.path.recent.sufamiTurboB = Location::dir(sufamiTurboB.location);
      if(loadSufamiTurboB(sufamiTurboB.location)) {
        fastLinkDebug("Program::platformLoad Sufami Turbo B done option=%s", (const char*)sufamiTurboB.option);
        return {id, sufamiTurboB.option};
      }
      fastLinkDebug("Program::platformLoad Sufami Turbo B load failed");
    }
  }

  fastLinkDebug("Program::platformLoad returning empty id=%u name=%s", id, (const char*)name);
    fastLinkCrashStep("Program::platformLoad: empty return");
  return {};
}

auto Program::videoFrame(const uint16* data, uint pitch, uint width, uint height, uint scale) -> void {
  #if defined(BUILD_DEBUG)
  static uint64 videoFrameTraceCounter = 0;
  videoFrameTraceCounter++;
  bool traceFrame = videoFrameTraceCounter <= 120;
  #else
  constexpr uint64 videoFrameTraceCounter = 0;
  constexpr bool traceFrame = false;
  #endif
    fastLinkCrashStep("Program::videoFrame: begin");
  if(traceFrame) {
    fastLinkDebug(
      "Program::videoFrame[%llu] begin data=%p pitch=%u width=%u height=%u scale=%u suppressVideo=%d suppressAccounting=%d",
      (unsigned long long)videoFrameTraceCounter,
      (const void*)data,
      pitch,
      width,
      height,
      scale,
      suppressVideoFrame,
      suppressFrameAccounting
    );
  }
  if(!suppressFrameAccounting) inputManager.frame();
  if(suppressVideoFrame) {
    if(traceFrame) fastLinkDebug("Program::videoFrame[%llu] suppressed", (unsigned long long)videoFrameTraceCounter);
    return;
  }

  //this relies on the UI only running between Emulator::Scheduler::Event::Frame events
  //this will always be the case; so we can avoid an unnecessary copy or one-frame delay here
  //if the core were to exit between a frame event, the next frame might've been only partially rendered
  screenshot.data   = data;
  screenshot.pitch  = pitch;
  screenshot.width  = width;
  screenshot.height = height;
  screenshot.scale  = scale;

  pitch >>= 1;
  if(!settings.video.overscan) {
    uint multiplier = height / 240;
    data += 8 * multiplier * pitch;
    height -= 16 * multiplier;
  }

  uint outputWidth, outputHeight;
  viewportSize(outputWidth, outputHeight, scale);

  uint filterWidth = width, filterHeight = height;
  auto filterRender = filterSelect(filterWidth, filterHeight, scale);
  if(traceFrame) {
    fastLinkDebug(
      "Program::videoFrame[%llu] viewport=%ux%u filter=%ux%u",
      (unsigned long long)videoFrameTraceCounter,
      outputWidth,
      outputHeight,
      filterWidth,
      filterHeight
    );
  }
  if(auto [output, length] = video.acquire(filterWidth, filterHeight); output) {
    fastLinkCrashStep("Program::videoFrame: acquired");
    if(traceFrame) fastLinkDebug("Program::videoFrame[%llu] video.acquire ok output=%p pitch=%u", (unsigned long long)videoFrameTraceCounter, (void*)output, length);
    filterRender(palette, output, length, (const uint16_t*)data, pitch << 1, width, height);
    fastLinkCrashStep("Program::videoFrame: filtered");
    if(traceFrame) {
      uint32_t checksum = 2166136261u;
      uint32_t alpha = 0;
      uint32_t nonBlack = 0;
      uint stride = max(1u, (filterWidth * filterHeight) / 64u);
      uint pitchPixels = length >> 2;
      for(uint index = 0; index < filterWidth * filterHeight; index += stride) {
        uint32_t pixel = output[(index / filterWidth) * pitchPixels + (index % filterWidth)];
        checksum = (checksum ^ pixel) * 16777619u;
        alpha |= pixel >> 24;
        if(pixel & 0x00ffffff) nonBlack++;
      }
      fastLinkDebug(
        "Program::videoFrame[%llu] pixels sample0=%08x checksum=%08x alphaOr=%02x nonBlackSamples=%u",
        (unsigned long long)videoFrameTraceCounter,
        output[0],
        checksum,
        alpha,
        nonBlack
      );
    }
    video.release();
    fastLinkCrashStep("Program::videoFrame: released");
    if(traceFrame) fastLinkDebug("Program::videoFrame[%llu] video.output begin", (unsigned long long)videoFrameTraceCounter);
    fastLinkCrashStep("Program::videoFrame: output begin");
    video.output(outputWidth, outputHeight);
    fastLinkCrashStep("Program::videoFrame: output done");
    if(traceFrame) fastLinkDebug("Program::videoFrame[%llu] video.output done", (unsigned long long)videoFrameTraceCounter);
  } else {
    if(traceFrame) fastLinkDebug("Program::videoFrame[%llu] video.acquire returned null ready=%d", (unsigned long long)videoFrameTraceCounter, video.ready());
  }

  if(presentation.frameAdvance.checked()) {
    frameAdvanceLock = true;
  }

  static uint frameCounter = 0;
  static uint64 previous, current;
  frameCounter++;

  current = chrono::timestamp();
  if(current != previous) {
    previous = current;
    showFrameRate({frameCounter * (1 + emulator->frameSkip()), " FPS"});
    frameCounter = 0;
  }
}

auto Program::audioFrame(const double* samples, uint channels) -> void {
  #if defined(BUILD_DEBUG)
  static uint64 audioFrameTraceCounter = 0;
  audioFrameTraceCounter++;
  bool traceFrame = audioFrameTraceCounter <= 80;
  #else
  constexpr uint64 audioFrameTraceCounter = 0;
  constexpr bool traceFrame = false;
  #endif
  if(traceFrame) {
    fastLinkDebug(
      "Program::audioFrame[%llu] begin samples=%p channels=%u suppressAudio=%d mute=%u ready=%d",
      (unsigned long long)audioFrameTraceCounter,
      (const void*)samples,
      channels,
      suppressAudioFrame,
      mute,
      audio.ready()
    );
  }
  if(suppressAudioFrame) {
    if(traceFrame) fastLinkDebug("Program::audioFrame[%llu] suppressed", (unsigned long long)audioFrameTraceCounter);
    return;
  }

  if(mute) {
    double silence[] = {0.0, 0.0};
    audio.output(silence);
  } else {
    audio.output(samples);
  }
  if(traceFrame) fastLinkDebug("Program::audioFrame[%llu] done", (unsigned long long)audioFrameTraceCounter);
}

auto Program::inputPoll(uint port, uint device, uint input) -> int16 {
  int16 value = 0;
  if(focused() || settings.input.defocus == "Allow") {
    #if defined(PLATFORM_WINDOWS)
    if(device == NativeGamepadDeviceID) {
      value = nativeInputManager.pollGamepad(port, input);
    } else if(auto mapping = inputManager.cachedMapping(port, device, input)) {
      value = mapping->poll();
    }
    #else
    inputManager.poll();
    if(auto mapping = inputManager.cachedMapping(port, device, input)) {
      value = mapping->poll();
    }
    #endif
  }
  return value;
}

auto Program::inputRumble(uint port, uint device, uint input, bool enable) -> void {
  if(focused() || settings.input.defocus == "Allow" || !enable) {
    #if defined(PLATFORM_WINDOWS)
    if(device == NativeGamepadDeviceID) {
      return nativeInputManager.rumbleGamepad(port, input, enable);
    }
    #endif
    if(auto mapping = inputManager.cachedMapping(port, device, input)) {
      return mapping->rumble(enable);
    }
  }
}
