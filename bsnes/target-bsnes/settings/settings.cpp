#include "../bsnes.hpp"
Settings settings;

auto Settings::load() -> void {
  Markup::Node::operator=(BML::unserialize(string::read(location), " "));
  process(true);
  save();
}

auto Settings::save() -> void {
  // Active gameplay only changes live settings. Persist once gameplay stops;
  // do not serialize, query paths, create events, or touch disk during a run.
  if(::emulator && ::emulator->loaded() && !program.paused && !quitting)return;
  process(false);
  file::write(location ? location : locate("settings.bml"),BML::serialize(*this, " "));
}

auto Settings::process(bool load) -> void {
  if(load) {
    //initialize non-static default settings
    video.driver = NativeVideoDriverName;
    audio.driver = ruby::Audio::optimalDriver();
    input.driver = NativeInputDriverName;
  }

  #define bind(type, path, name) \
    if(load) { \
      if(auto node = operator[](path)) name = node.type(); \
    } else { \
      operator()(path).setValue(name); \
    } \

  bind(text,    "Video/Driver",    video.driver);
  bind(boolean, "Video/Exclusive", video.exclusive);
  bind(boolean, "Video/Blocking",  video.blocking);
  bind(boolean, "Video/Flush",     video.flush);
  bind(text,    "Video/PresentMode", video.presentMode);
  bind(natural, "Video/SwapchainImages", video.swapchainImages);
  bind(text,    "Video/Monitor",   video.monitor);
  bind(text,    "Video/Format",    video.format);
  bind(text,    "Video/Shader",    video.shader);

  bind(natural, "Video/Luminance",  video.luminance);
  bind(natural, "Video/Saturation", video.saturation);
  bind(natural, "Video/Gamma",      video.gamma);
  bind(boolean, "Video/Dimming",    video.dimming);
  bind(boolean, "Video/Snow",       video.snow);

  bind(text,    "Video/Output",           video.output);
  bind(natural, "Video/Multiplier",       video.multiplier);
  bind(boolean, "Video/AspectCorrection", video.aspectCorrection);
  bind(boolean, "Video/Overscan",         video.overscan);
  bind(boolean, "Video/Blur",             video.blur);
  bind(boolean, "Video/HDMode7",           video.hdMode7);
  bind(text,    "Video/Filter",           video.filter);
  if(load) video.filter = "None";

  bind(text,    "Audio/Driver",    audio.driver);
  bind(boolean, "Audio/Exclusive", audio.exclusive);
  bind(text,    "Audio/Device",    audio.device);
  bind(boolean, "Audio/Blocking",  audio.blocking);
  bind(boolean, "Audio/Dynamic",   audio.dynamic);
  bind(natural, "Audio/Frequency", audio.frequency);
  bind(natural, "Audio/Latency",   audio.latency);

  bind(boolean, "Audio/Mute",          audio.mute);
  bind(boolean, "Audio/MuteUnfocused", audio.muteUnfocused);
  bind(integer, "Audio/Skew",          audio.skew);
  bind(natural, "Audio/Volume",        audio.volume);
  bind(natural, "Audio/Balance",       audio.balance);

  bind(text,    "Input/Driver",          input.driver);
  bind(natural, "Input/Frequency",       input.frequency);
  bind(text,    "Input/Defocus",         input.defocus);
  bind(natural, "Input/Turbo/Frequency", input.turbo.frequency);
  bind(text,    "Input/Hotkey/Logic",    input.hotkey.logic);

  bind(text,    "Path/Games",       path.games);
  bind(text,    "Path/Patches",     path.patches);
  bind(text,    "Path/Saves",       path.saves);
  bind(text,    "Path/Cheats",      path.cheats);
  bind(text,    "Path/States",      path.states);
  bind(text,    "Path/Screenshots", path.screenshots);

  bind(text,    "Path/Recent/SuperFamicom", path.recent.superFamicom);
  bind(text,    "Path/Recent/GameBoy",      path.recent.gameBoy);
  bind(text,    "Path/Recent/BSMemory",     path.recent.bsMemory);
  bind(text,    "Path/Recent/SufamiTurboA", path.recent.sufamiTurboA);
  bind(text,    "Path/Recent/SufamiTurboB", path.recent.sufamiTurboB);

  bind(natural, "FastForward/Limiter",   fastForward.limiter);
  bind(boolean, "FastForward/Mute",      fastForward.mute);

  bind(boolean, "FastLink/RAMTraceServer", fastLink.ramTraceServer);
  bind(boolean, "FastLink/DisableSoftwareFilters", fastLink.disableSoftwareFilters);
  bind(boolean, "FastLink/LowLatencyInactiveYield", fastLink.lowLatencyInactiveYield);
  bind(boolean, "FastLink/SoftwareFramePacing", fastLink.softwareFramePacing);
  bind(text,    "FastLink/GenericHIDMapping", fastLink.genericHIDMapping);
  bind(integer, "FastLink/MainThreadAffinity", fastLink.mainThreadAffinity);
  bind(integer, "FastLink/InputThreadAffinity", fastLink.inputThreadAffinity);
  bind(boolean, "FastLink/WindowsTimerResolution", fastLink.windowsTimerResolution);
  bind(boolean, "FastLink/WindowsHighPerformancePower", fastLink.windowsHighPerformancePower);

  bind(boolean, "Emulator/WarnOnUnverifiedGames",        emulator.warnOnUnverifiedGames);
  bind(boolean, "Emulator/AutoSaveMemory/Enable",        emulator.autoSaveMemory.enable);
  bind(natural, "Emulator/AutoSaveMemory/Interval",      emulator.autoSaveMemory.interval);
  bind(boolean, "Emulator/AutoSaveStateOnUnload",        emulator.autoSaveStateOnUnload);
  bind(boolean, "Emulator/AutoLoadStateOnLoad",          emulator.autoLoadStateOnLoad);
  bind(boolean, "Emulator/Cheats/Enable",                emulator.cheats.enable);

  bind(boolean, "General/StatusBar",         general.statusBar);
  bind(boolean, "General/ScreenSaver",       general.screenSaver);
  bind(boolean, "General/ToolTips",          general.toolTips);
  bind(boolean, "General/Crashed",           general.crashed);
  bind(text,    "General/CrashComponent",    general.crashComponent);
  bind(boolean, "General/NativeFileDialogs", general.nativeFileDialogs);
  bind(integer, "General/Window/X",          general.window.x);
  bind(integer, "General/Window/Y",          general.window.y);
  bind(natural, "General/Window/Width",      general.window.width);
  bind(natural, "General/Window/Height",     general.window.height);
  bind(boolean, "General/Window/Maximized", general.window.maximized);
  bind(text,    "General/Window/Monitor",    general.window.monitor);

  if(load) {
    if(video.driver != NativeVideoDriverName) {
      video.driver = NativeVideoDriverName;
    }
    if(video.presentMode != "Immediate" && video.presentMode != "Mailbox" && video.presentMode != "FIFO") {
      video.presentMode = "Mailbox";
    }
    if(video.swapchainImages < 2) video.swapchainImages = 2;
    if(video.swapchainImages > 4) video.swapchainImages = 4;
    if(fastLink.genericHIDMapping != "SNES Adapter" && fastLink.genericHIDMapping != "Standard HID") {
      fastLink.genericHIDMapping = "SNES Adapter";
    }
    if(fastLink.disableSoftwareFilters) video.filter = "None";
    if(audio.driver == "WASAPI") {
      audio.exclusive = false;
      audio.blocking = false;
    }
    if(input.defocus == "Pause") {
      input.defocus = "Block";
    }
    if(video.shader != "None") {
      video.shader = "None";
    }
    if(!ruby::Audio::hasDriver(audio.driver)) {
      audio.driver = ruby::Audio::optimalDriver();
    }
    if(audio.driver == "waveOut" || audio.driver == "DirectSound 7.0") {
      audio.driver = ruby::Audio::optimalDriver();
    }

    //Older crash recovery builds disabled every driver after a video failure.
    //Recover audio/input automatically when that exact all-None state is encountered.
    if(video.driver == "None" && audio.driver == "None" && input.driver == "None" && !general.crashComponent) {
      video.driver = NativeVideoDriverName;
      audio.driver = ruby::Audio::optimalDriver();
      input.driver = NativeInputDriverName;
      general.crashComponent = "Video";
    }
    if(!video.driver || video.driver == "None") video.driver = NativeVideoDriverName;
    if(!input.driver || input.driver == "None") input.driver = NativeInputDriverName;
    general.nativeFileDialogs = true;
  }

  #undef bind
}
