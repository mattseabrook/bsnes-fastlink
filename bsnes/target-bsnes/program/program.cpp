#include "../bsnes.hpp"
#include "platform.cpp"
#include "game.cpp"
#include "game-pak.cpp"
#include "game-rom.cpp"
#include "paths.cpp"
#include "states.cpp"
#include "movies.cpp"
#include "rewind.cpp"
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
#ifndef TIMERR_NOERROR
#define TIMERR_NOERROR 0
#endif
#endif

Program program;

#if defined(PLATFORM_WINDOWS)
namespace {
bool windowsLatencyModeActive = false;

auto enableWindowsLatencyMode() -> void {
  SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
  SetProcessPriorityBoost(GetCurrentProcess(), FALSE);
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  SetThreadPriorityBoost(GetCurrentThread(), FALSE);

  if(!windowsLatencyModeActive && timeBeginPeriod(1) == TIMERR_NOERROR) {
    windowsLatencyModeActive = true;
  }
}

auto disableWindowsLatencyMode() -> void {
  if(!windowsLatencyModeActive) return;
  timeEndPeriod(1);
  windowsLatencyModeActive = false;
}
}
#endif

auto Program::create() -> void {
  #if defined(PLATFORM_WINDOWS)
  enableWindowsLatencyMode();
  #endif

  Emulator::platform = this;

  presentation.create();
  presentation.setVisible();
  presentation.viewport.setFocused();
  fastLinkRAMTraceService.start();

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
  updateVideoDriver(presentation);

  settings.general.crashComponent = "Audio";
  settings.save();
  updateAudioDriver(presentation);

  settings.general.crashComponent = "Input";
  settings.save();
  updateInputDriver(presentation);

  settings.general.crashed = false;
  settings.general.crashComponent = "";
  settings.save();

  if(gameQueue) load();
  if(startFullScreen && emulator->loaded()) {
    toggleVideoFullScreen();
  }
  #if !defined(PLATFORM_WINDOWS)
  inputManager.startWorker();
  #endif
  Application::onMain({&Program::main, this});
}

auto Program::main() -> void {
  updateStatus();
  video.poll();

  if(Application::modal()) {
    audio.clear();
    return;
  }

  #if !defined(PLATFORM_WINDOWS)
  inputManager.poll();
  #endif
  inputManager.pollHotkeys();

  static bool previouslyInactive = true;
  bool currentlyInactive = inactive();

  // check if emulator has transitioned from active to inactive or vice versa
  if(previouslyInactive != currentlyInactive) {
    previouslyInactive = currentlyInactive;
    Application::setScreenSaver(currentlyInactive || settings.general.screenSaver);
  }

  if(currentlyInactive) {
    audio.clear();
    usleep(20 * 1000);
    viewportRefresh();
    return;
  }

  rewindRun();

  emulator->run();
  fastLinkRAMTraceService.publishFrame();

  if(emulatorSettings.autoSaveMemory.checked()) {
    auto currentTime = chrono::timestamp();
    if(currentTime - autoSaveTime >= settings.emulator.autoSaveMemory.interval) {
      autoSaveTime = currentTime;
      emulator->save();
    }
  }
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
  TerminateProcess(handle, 0);
  #endif

  Application::exit();
}
