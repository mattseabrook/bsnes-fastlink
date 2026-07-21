#include "../bsnes.hpp"
#include "platform.cpp"
#include "game.cpp"
#include "game-pak.cpp"
#include "game-rom.cpp"
#include "paths.cpp"
#include "states.cpp"
#include "video.cpp"
#include "audio.cpp"
#include "input.cpp"
#include "utility.cpp"
#include "patch.cpp"
#include "hacks.cpp"
#include "filter.cpp"
#include "viewport.cpp"

#if defined(PLATFORM_WINDOWS)
extern "C" __declspec(dllimport) UINT WINAPI timeBeginPeriod(UINT period);
extern "C" __declspec(dllimport) UINT WINAPI timeEndPeriod(UINT period);
#include <csignal>
#include <exception>
#ifndef TIMERR_NOERROR
#define TIMERR_NOERROR 0
#endif

auto WINAPI fastLinkInvalidParameterHandler(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) -> void {
  fastLinkCrashWrite("invalid-parameter");
  std::abort();
}

auto fastLinkTerminateHandler() -> void {
  fastLinkCrashWrite("terminate");
  std::abort();
}

auto fastLinkSignalHandler(int signal) -> void {
  fastLinkCrashWrite(signal == SIGABRT ? "signal-sigabrt" : "signal");
  std::abort();
}
#endif

Program program;

#if defined(PLATFORM_WINDOWS)
namespace {
bool windowsLatencyModeActive = false;
bool ntTimerResolutionActive = false;
ULONG ntTimerResolutionCurrent = 0;
GUID* previousPowerScheme = nullptr;

using NtSetTimerResolutionProc = LONG (WINAPI*)(ULONG, BOOLEAN, PULONG);

auto applyThreadAffinity(int processor) -> void {
  if(processor == -1) return;

  DWORD_PTR processMask = 0;
  DWORD_PTR systemMask = 0;
  if(!GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) || !processMask) return;

  int target = processor;
  if(target < 0) {
    target = 0;
    for(uint index : range((uint)(sizeof(DWORD_PTR) * 8))) {
      if(processMask & ((DWORD_PTR)1 << index)) target = index;
    }
  }

  if(target < 0 || target >= (int)(sizeof(DWORD_PTR) * 8)) return;

  DWORD_PTR mask = (DWORD_PTR)1 << target;
  if(!(processMask & mask)) return;
  SetThreadAffinityMask(GetCurrentThread(), mask);
  SetThreadIdealProcessor(GetCurrentThread(), target);
}

auto enableNtTimerResolution() -> void {
  if(!settings.fastLink.windowsTimerResolution || ntTimerResolutionActive) return;
  auto ntdll = GetModuleHandleA("ntdll.dll");
  if(!ntdll) return;
  auto setTimerResolution = (NtSetTimerResolutionProc)GetProcAddress(ntdll, "NtSetTimerResolution");
  if(!setTimerResolution) return;
  if(setTimerResolution(5000, TRUE, &ntTimerResolutionCurrent) >= 0) {
    ntTimerResolutionActive = true;
  }
}

auto disableNtTimerResolution() -> void {
  if(!ntTimerResolutionActive) return;
  auto ntdll = GetModuleHandleA("ntdll.dll");
  auto setTimerResolution = ntdll ? (NtSetTimerResolutionProc)GetProcAddress(ntdll, "NtSetTimerResolution") : nullptr;
  if(setTimerResolution) setTimerResolution(5000, FALSE, &ntTimerResolutionCurrent);
  ntTimerResolutionActive = false;
}

auto enableHighPerformancePowerScheme() -> void {
  if(!settings.fastLink.windowsHighPerformancePower) return;
  auto powrprof = LoadLibraryW(L"powrprof.dll");
  if(!powrprof) return;
  using PowerGetActiveSchemeProc = DWORD (WINAPI*)(HKEY, GUID**);
  using PowerSetActiveSchemeProc = DWORD (WINAPI*)(HKEY, const GUID*);
  auto getActiveScheme = (PowerGetActiveSchemeProc)GetProcAddress(powrprof, "PowerGetActiveScheme");
  auto setActiveScheme = (PowerSetActiveSchemeProc)GetProcAddress(powrprof, "PowerSetActiveScheme");
  if(setActiveScheme) {
    if(!previousPowerScheme && getActiveScheme) getActiveScheme(nullptr, &previousPowerScheme);
    const GUID highPerformance = {0x8c5e7fda, 0xe8bf, 0x4a96, {0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c}};
    setActiveScheme(nullptr, &highPerformance);
  }
  FreeLibrary(powrprof);
}

auto restorePowerScheme() -> void {
  if(!previousPowerScheme) return;
  auto powrprof = LoadLibraryW(L"powrprof.dll");
  if(powrprof) {
    using PowerSetActiveSchemeProc = DWORD (WINAPI*)(HKEY, const GUID*);
    auto setActiveScheme = (PowerSetActiveSchemeProc)GetProcAddress(powrprof, "PowerSetActiveScheme");
    if(setActiveScheme) setActiveScheme(nullptr, previousPowerScheme);
    FreeLibrary(powrprof);
  }
  LocalFree(previousPowerScheme);
  previousPowerScheme = nullptr;
}

auto enableWindowsLatencyMode() -> void {
  SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
  SetProcessPriorityBoost(GetCurrentProcess(), FALSE);
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  SetThreadPriorityBoost(GetCurrentThread(), FALSE);

  if(!windowsLatencyModeActive && timeBeginPeriod(1) == TIMERR_NOERROR) {
    windowsLatencyModeActive = true;
  }
  enableNtTimerResolution();
  enableHighPerformancePowerScheme();
  applyThreadAffinity(settings.fastLink.mainThreadAffinity);
}

auto disableWindowsLatencyMode() -> void {
  restorePowerScheme();
  disableNtTimerResolution();
  if(windowsLatencyModeActive) {
    timeEndPeriod(1);
    windowsLatencyModeActive = false;
  }
}

auto lowLatencyInactiveYield() -> void {
  for(uint i : range(200)) {
    YieldProcessor();
    SwitchToThread();
  }
}

LONG WINAPI fastLinkCrashHandler(EXCEPTION_POINTERS* ep) {
  return EXCEPTION_CONTINUE_SEARCH;
}
}
#endif

namespace {
auto fastLinkFrameDurationNanoseconds(const string& region) -> uint64 {
  if(region == "PAL") {
    double frequency = Emulator::Constants::Colorburst::PAL * 4.8;
    double clocks = 1364.0 * 312.0;
    return (uint64)(1'000'000'000.0 * clocks / frequency + 0.5);
  }

  double frequency = Emulator::Constants::Colorburst::NTSC * 6.0;
  double clocks = 1364.0 * 262.0 - 2.0;  //average one short scanline every other field
  return (uint64)(1'000'000'000.0 * clocks / frequency + 0.5);
}
}

auto Program::create() -> void {
  #if defined(PLATFORM_WINDOWS)
  SetUnhandledExceptionFilter(fastLinkCrashHandler);
  _set_invalid_parameter_handler(fastLinkInvalidParameterHandler);
  std::set_terminate(fastLinkTerminateHandler);
  signal(SIGABRT, fastLinkSignalHandler);
  fastLinkDebug("Program::create SEH crash handler installed");
  fastLinkCrashStep("Program::create: crash handlers installed");
  #endif
  fastLinkDebug(
    "Program::create begin videoDriver=%s present=%s swap=%u audioDriver=%s audioExclusive=%d audioBlocking=%d runAhead=%u ramTrace=%d",
    (const char*)settings.video.driver,
    (const char*)settings.video.presentMode,
    settings.video.swapchainImages,
    (const char*)settings.audio.driver,
    settings.audio.exclusive,
    settings.audio.blocking,
    settings.fastLink.runAhead,
    settings.fastLink.ramTraceServer
  );
  fastLinkDebug("Program::create libco co_serializable=%d", co_serializable());
  #if defined(PLATFORM_WINDOWS)
  enableWindowsLatencyMode();
  fastLinkDebug("Program::create latency mode enabled");
  #endif

  Emulator::platform = this;

  fastLinkCrashStep("Program::create: presentation begin");
  fastLinkDebug("Program::create presentation begin");
  presentation.create();
  presentation.setVisible();
  presentation.viewport.setFocused();
  fastLinkDebug("Program::create presentation ready viewport=%p", (void*)presentation.viewport.handle());
  if(settings.fastLink.ramTraceServer) fastLinkRAMTraceService.start();

  fastLinkCrashStep("Program::create: settings/tools windows begin");
  fastLinkDebug("Program::create settings/tools windows begin");
  settingsWindow.create();
  videoSettings.create();
  audioSettings.create();
  #if !defined(PLATFORM_WINDOWS)
  inputSettings.create();
  #endif
  hotkeySettings.create();
  pathSettings.create();
  emulatorSettings.create();
  enhancementSettings.create();
  compatibilitySettings.create();
  driverSettings.create();

  toolsWindow.create();
  cheatFinder.create();
  cheatDatabase.create();
  cheatWindow.create();
  cheatEditor.create();
  stateWindow.create();
  stateManager.create();
  manifestViewer.create();
  fastLinkDebug("Program::create settings/tools windows ready");
  fastLinkCrashStep("Program::create: settings/tools windows ready");

  if(settings.general.crashed) {
    string crashComponent = settings.general.crashComponent;
    if(!crashComponent) crashComponent = "Video";

    MessageDialog(
      "Driver crash detected. Recovery settings have been applied.\n"
      "Please reconfigure drivers in the advanced settings panel if needed."
    ).setAlignment(*presentation).information();

    if(crashComponent == "Video") {
      #if defined(PLATFORM_WINDOWS)
      settings.video.driver = NativeVideoDriverName;
      #else
      settings.video.driver = "None";
      #endif
      if(settings.audio.driver == "None") settings.audio.driver = ruby::Audio::optimalDriver();
      #if defined(PLATFORM_WINDOWS)
      if(settings.input.driver == "None") settings.input.driver = NativeInputDriverName;
      #else
      if(settings.input.driver == "None") settings.input.driver = ruby::Input::safestDriver();
      #endif
    }
    if(crashComponent == "Audio") {
      settings.audio.driver = ruby::Audio::optimalDriver();
    }
    if(crashComponent == "Input") {
      #if defined(PLATFORM_WINDOWS)
      settings.input.driver = NativeInputDriverName;
      #else
      settings.input.driver = ruby::Input::safestDriver();
      #endif
    }
  }

  settings.general.crashed = true;
  settings.general.crashComponent = "Video";
  settings.save();
  fastLinkCrashStep("Program::create: updateVideoDriver");
  fastLinkDebug("Program::create updateVideoDriver begin");
  updateVideoDriver(presentation);
  fastLinkDebug("Program::create updateVideoDriver done ready=%d driver=%s", video.ready(), (const char*)video.driver());

  settings.general.crashComponent = "Audio";
  fastLinkCrashStep("Program::create: updateAudioDriver");
  fastLinkDebug("Program::create updateAudioDriver begin");
  updateAudioDriver(presentation);
  fastLinkDebug("Program::create updateAudioDriver done ready=%d driver=%s", audio.ready(), (const char*)audio.driver());

  settings.general.crashComponent = "Input";
  fastLinkCrashStep("Program::create: updateInputDriver");
  fastLinkDebug("Program::create updateInputDriver begin");
  updateInputDriver(presentation);
  fastLinkDebug("Program::create updateInputDriver done ready=%d driver=%s", input.ready(), (const char*)input.driver());

  settings.general.crashed = false;
  settings.general.crashComponent = "";
  settings.save();
  fastLinkDebug("Program::create crash guard cleared");
  fastLinkCrashStep("Program::create: crash guard cleared");

  if(gameQueue) load();
  if(startFullScreen && emulator->loaded()) {
    toggleVideoFullScreen();
  }
  #if !defined(PLATFORM_WINDOWS)
  inputManager.startWorker();
  #endif
  Application::onMain({&Program::main, this});
  fastLinkDebug("Program::create complete");
  fastLinkCrashStep("Program::create: complete");
}

auto Program::main() -> void {
  #if defined(BUILD_DEBUG)
  static uint64 mainTraceCounter = 0;
  mainTraceCounter++;
  bool traceFrame = mainTraceCounter <= 2000;
  #else
  constexpr uint64 mainTraceCounter = 0;
  constexpr bool traceFrame = false;
  #endif

  updateStatus();

  #if !defined(PLATFORM_WINDOWS)
  inputManager.poll();
  #endif
  inputManager.pollHotkeys();
  video.poll();

  static bool previouslyModal = false;
  bool currentlyModal = Application::modal();
  if(currentlyModal) {
    if(!previouslyModal) audio.clear();
    previouslyModal = true;
    if(traceFrame) fastLinkDebug("Program::main[%llu] modal return", (unsigned long long)mainTraceCounter);
    resetFramePacer();
    return;
  }
  previouslyModal = false;

  static bool previouslyInactive = true;
  bool currentlyInactive = inactive();
  if(traceFrame) {
    fastLinkDebug(
      "Program::main[%llu] loaded=%d inactive=%d warmup=%u runAheadSetting=%u frameAdvance=%d fastForward=%d",
      (unsigned long long)mainTraceCounter,
      emulator && emulator->loaded(),
      currentlyInactive,
      runAheadWarmupFrames,
      settings.fastLink.runAhead,
      presentation.frameAdvance.checked(),
      fastForwarding
    );
  }

  // check if emulator has transitioned from active to inactive or vice versa
  if(previouslyInactive != currentlyInactive) {
    previouslyInactive = currentlyInactive;
    if(currentlyInactive) audio.clear();
    else resetFramePacer();
    Application::setScreenSaver(currentlyInactive || settings.general.screenSaver);
  }

  if(currentlyInactive) {
    if(traceFrame) fastLinkDebug("Program::main[%llu] inactive return", (unsigned long long)mainTraceCounter);
    resetFramePacer();
    #if defined(PLATFORM_WINDOWS)
    if(settings.fastLink.lowLatencyInactiveYield) {
      lowLatencyInactiveYield();
    } else
    #endif
    {
      usleep(20 * 1000);
    }
    viewportRefresh();
    return;
  }

  // Wait until the frame deadline before entering the core so Raw Input can
  // continue publishing newer controller state for the core's just-in-time
  // input polls. Pacing after emulation makes the completed frame sit idle and
  // increases the age of the input represented by the next presented frame.
  paceFrame();
  // Retire the previous GPU upload now, before entering the core. Waiting for
  // upload ownership from videoFrame() would add a predictable stall after the
  // core has already sampled input.
  #if defined(PLATFORM_WINDOWS)
  if(emulator && emulator->loaded() && !video.prepare()) {
    // Do not advance emulation without presenting the completed frame.  The
    // Vulkan backend waits for ordinary GPU/swapchain pressure; reaching this
    // path indicates a real surface/device failure.
    resetFramePacer();
    return;
  }
  #endif

  auto frames = runAheadFrames();
  if(traceFrame) fastLinkDebug("Program::main[%llu] runAheadFrames=%u", (unsigned long long)mainTraceCounter, frames);
  if(frames == 0) {
    if(traceFrame) fastLinkDebug("Program::main[%llu] normal frame begin", (unsigned long long)mainTraceCounter);
    runEmulationFrame(true, true, true);
    if(traceFrame) fastLinkDebug("Program::main[%llu] normal frame done", (unsigned long long)mainTraceCounter);
  } else {
    auto serializeRunAhead = [&](const char* label) -> serializer {
      if(traceFrame) {
        fastLinkDebug(
          "Program::main[%llu] serialize %s deterministic begin co_serializable=%d",
          (unsigned long long)mainTraceCounter,
          label,
          co_serializable()
        );
      }
      auto state = emulator->serialize(false);
      if(traceFrame) fastLinkDebug("Program::main[%llu] serialize %s deterministic size=%u", (unsigned long long)mainTraceCounter, label, (uint)state.size());
      return state;
    };

    if(traceFrame) fastLinkDebug("Program::main[%llu] real frame begin", (unsigned long long)mainTraceCounter);
    runEmulationFrame(false, true, true);
    if(traceFrame) fastLinkDebug("Program::main[%llu] serialize real begin", (unsigned long long)mainTraceCounter);
    auto realFrame = serializeRunAhead("real");
    if(traceFrame) fastLinkDebug("Program::main[%llu] serialize real size=%u", (unsigned long long)mainTraceCounter, (uint)realFrame.size());
    if(!realFrame.size()) {
      resetRunAheadWarmup();
      if(traceFrame) fastLinkDebug("Program::main[%llu] real serialize empty; rendering recovery frame", (unsigned long long)mainTraceCounter);
      runEmulationFrame(true, true, true);
    } else {
      // The core state does not own Emulator::Audio's stream resamplers. Save
      // them separately so speculative frames cannot leave interpolation
      // history one frame ahead of the restored DSP state (an audible click at
      // every run-ahead boundary).
      serializer audioStateSize;
      Emulator::audio.serialize(audioStateSize);
      serializer realAudio{audioStateSize.size()};
      Emulator::audio.serialize(realAudio);

      auto realTurboCounter = inputManager.turboCounter;
      for(uint frame : range(frames)) {
        if(traceFrame) fastLinkDebug(
          "Program::main[%llu] run-ahead speculative frame %u/%u begin",
          (unsigned long long)mainTraceCounter,
          frame + 1,
          frames
        );
        runEmulationFrame(frame + 1 == frames, false, false);
        if(traceFrame) fastLinkDebug(
          "Program::main[%llu] run-ahead speculative frame %u/%u done",
          (unsigned long long)mainTraceCounter,
          frame + 1,
          frames
        );
      }
      serializer restore{realFrame.data(), (uint)realFrame.size()};
      if(traceFrame) fastLinkDebug("Program::main[%llu] restoring real frame", (unsigned long long)mainTraceCounter);
      bool restored = emulator->unserialize(restore);
      if(realAudio.size()) {
        serializer audioRestore{realAudio.data(), realAudio.size()};
        Emulator::audio.serialize(audioRestore);
      }
      inputManager.turboCounter = realTurboCounter;
      if(!restored) resetRunAheadWarmup();
      if(traceFrame) fastLinkDebug("Program::main[%llu] restored real frame=%d", (unsigned long long)mainTraceCounter, restored);
    }
  }
  if(runAheadWarmupFrames) runAheadWarmupFrames--;

  fastLinkRAMTraceService.publishFrame();
  if(traceFrame) fastLinkDebug("Program::main[%llu] published RAM trace if requested", (unsigned long long)mainTraceCounter);

  if(emulatorSettings.autoSaveMemory.checked()) {
    auto currentTime = chrono::timestamp();
    if(currentTime - autoSaveTime >= settings.emulator.autoSaveMemory.interval) {
      autoSaveTime = currentTime;
      emulator->save();
    }
  }

  if(traceFrame) fastLinkDebug("Program::main[%llu] done", (unsigned long long)mainTraceCounter);
}

auto Program::runEmulationFrame(bool videoEnabled, bool audioEnabled, bool accountFrame) -> void {
  #if defined(BUILD_DEBUG)
  static uint64 runFrameTraceCounter = 0;
  runFrameTraceCounter++;
  bool traceFrame = runFrameTraceCounter <= 240;
  #else
  constexpr uint64 runFrameTraceCounter = 0;
  constexpr bool traceFrame = false;
  #endif
  if(traceFrame) {
    fastLinkDebug(
      "Program::runEmulationFrame[%llu] begin video=%d audio=%d account=%d",
      (unsigned long long)runFrameTraceCounter,
      videoEnabled,
      audioEnabled,
      accountFrame
    );
  }
  auto previousVideo = suppressVideoFrame;
  auto previousAudio = suppressAudioFrame;
  auto previousAccounting = suppressFrameAccounting;
  suppressVideoFrame = !videoEnabled;
  suppressAudioFrame = !audioEnabled;
  suppressFrameAccounting = !accountFrame;

  // Fast PPU rendering is by far the largest cost of discarded run-ahead
  // frames (especially at HD Mode 7 scales).  Frame-skip suppresses that work
  // inside the core, rather than waiting until platform->videoFrame() to throw
  // the completed pixels away.  Run-ahead is disabled during fast-forward, so
  // the normal value here is zero; preserve it for callers that use another
  // frame-skip policy.
  auto previousFrameSkip = emulator->frameSkip();
  bool suppressCoreRendering = !videoEnabled && previousFrameSkip == 0;
  if(suppressCoreRendering) emulator->setFrameSkip(1);
  emulator->run();
  if(suppressCoreRendering) emulator->setFrameSkip(previousFrameSkip);
  suppressVideoFrame = previousVideo;
  suppressAudioFrame = previousAudio;
  suppressFrameAccounting = previousAccounting;
  if(traceFrame) fastLinkDebug("Program::runEmulationFrame[%llu] done", (unsigned long long)runFrameTraceCounter);
}

auto Program::runAheadFrames() const -> uint {
  if(!emulator || !emulator->loaded()) return 0;
  auto requestedFrames = settings.fastLink.runAhead;
  #if defined(PLATFORM_WINDOWS)
  // Keep the Windows fast path enabled even if configuration is mutated after
  // startup; Settings::process() also persists this normalization on load.
  requestedFrames = max(1u, requestedFrames);
  #endif
  if(!requestedFrames) return 0;
  if(!co_serializable()) {
    static bool logged = false;
    if(!logged) {
      logged = true;
      fastLinkDebug(
        "Program::runAheadFrames disabled for this process: libco backend is not stack-serializable; requested=%u",
        requestedFrames
      );
    }
    return 0;
  }
  if(runAheadWarmupFrames) return 0;
  if(frameAdvanceLock || presentation.frameAdvance.checked()) return 0;
  if(fastForwarding) return 0;
  return min(4u, requestedFrames);
}

auto Program::resetRunAheadWarmup() -> void {
  runAheadWarmupFrames = 8;
}

auto Program::paceFrame() -> void {
  if(!settings.fastLink.softwareFramePacing || fastForwarding) return resetFramePacer();
  if(settings.video.blocking || settings.audio.blocking) return resetFramePacer();

  uint64 frameDuration = fastLinkFrameDurationNanoseconds(superFamicom.region);
  auto now = chrono::nanosecond();
  if(!framePaceDeadline || now > framePaceDeadline + frameDuration) {
    framePaceDeadline = now + frameDuration;
    return;
  }

  while((now = chrono::nanosecond()) + 2'000'000ull < framePaceDeadline) {
    #if defined(PLATFORM_WINDOWS)
    SwitchToThread();
    #else
    ::std::this_thread::yield();
    #endif
  }
  while((now = chrono::nanosecond()) < framePaceDeadline) {
    #if defined(PLATFORM_WINDOWS)
    YieldProcessor();
    #elif defined(__SSE2__)
    __builtin_ia32_pause();
    #else
    ::std::this_thread::yield();
    #endif
  }
  framePaceDeadline += frameDuration;
}

auto Program::resetFramePacer() -> void {
  framePaceDeadline = 0;
}

auto Program::quit() -> void {
  presentation.saveWindowGeometry();

  //make closing the program feel more responsive
  presentation.setVisible(false);
  Application::processEvents();

  //in case the emulator was closed prior to initialization completing:
  settings.general.crashed = false;
  settings.general.crashComponent = "";

  fastLinkRAMTraceService.stop();
  unload();
  #if defined(PLATFORM_WINDOWS)
  nativeInputManager.terminate();
  #else
  inputManager.stopWorker();
  #endif
  settings.save();
  video.reset();
  audio.reset();
  input.reset();
  #if defined(PLATFORM_WINDOWS)
  disableWindowsLatencyMode();
  #endif

  #if defined(PLATFORM_WINDOWS)
  //in rare cases, when Application::exit() calls exit(0), a crash will occur.
  //this seems to be due to the internal state of certain ruby drivers.
  auto processID = GetCurrentProcessId();
  auto handle = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, true, processID);
  if(handle) {
    TerminateProcess(handle, 0);
    CloseHandle(handle);
  }
  #endif

  Application::exit();
}
