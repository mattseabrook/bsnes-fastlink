#include "../bsnes.hpp"
#include <mmsystem.h>
#include <sfc/sfc.hpp>
#undef platform
#include <nall/encode/bmp.hpp>
#include "latency.cpp"
#include "platform.cpp"
#include "game.cpp"
#include "game-rom.cpp"
#include "game-pak.cpp"
#include "paths.cpp"
#include "patch.cpp"
#include "states.cpp"
#include "palette.cpp"
#include "screenshot.cpp"
#include "cheat-codes.cpp"

Program program;

auto Program::create() -> bool {
  Emulator::platform = this;
  timing.initialize();
  enableWindowsLatencyMode();
  updateVideoPalette();
  emulator->configure("Video/HDMode7", settings.video.hdMode7);
  emulator->configure("Video/BlurEmulation", settings.video.blur);
  if(!updateVideoDriver(mainWindow)) return false;
  updateAudioDriver(mainWindow);
  if(!nativeInputManager.initialize()) { nativeError("Could not initialize Raw Input."); return false; }
  if(settings.fastLink.ramTraceServer) fastLinkRAMTraceService.start();
  if(gameQueue) load();
  if(startFullScreen) toggleVideoFullScreen();
  return true;
}

auto Program::focused() const -> bool { return video.focused(); }
auto Program::inactive() const -> bool {
  return loading || modalDepth || hostSuspended || !emulator->loaded() || paused
    || (!video.fullScreen() && IsIconic(mainWindow))
    || (frameAdvance && frameAdvanceLock) || (!focused() && settings.input.defocus == "Pause");
}

auto Program::paceFrame() -> void {
  double multiplier = fastForwarding ? settings.fastForward.limiter : speed;
  if(!settings.fastLink.softwareFramePacing || multiplier == 0
    || (audio.hasBlocking() && audio.blocking())) { resetFramePacer(); return; }
  auto duration = (uint64)(fastLinkFrameDurationNanoseconds() / multiplier);
  auto deadline = framePacer.next(chrono::nanosecond(), duration);
  waitForFrameDeadline(deadline);
}

auto Program::main() -> void {
  if(inFrame || quitting) return;
  inFrame = true;
  struct FrameGuard { bool& flag; ~FrameGuard() { flag = false; } } guard{inFrame};
  if(inactive()) { timing.idle(); resetFramePacer(); return; }
  bool capture = timing.enabled();
  auto begin = capture ? chrono::nanosecond() : 0;
  if(capture) timing.beginFrame();
  auto discarded = framePacer.discardedNanoseconds;
  video.poll();
  auto pace = capture ? chrono::nanosecond() : 0;
  paceFrame();
  auto prepare = capture ? chrono::nanosecond() : 0;
  bool presentReady=video.prepare();
  auto run = capture ? chrono::nanosecond() : 0;
  frameFocused = focused();
  emulator->run();
  presentVideo();
  auto trace = capture ? chrono::nanosecond() : 0;
  fastLinkRAMTraceService.publishFrame();
  if(capture)timing.frameDriverPresent=video.takePresentTiming();
  if(capture) timing.record(begin, pace, prepare, run, trace, chrono::nanosecond(),
    fastLinkFrameDurationNanoseconds(), framePacer.discardedNanoseconds - discarded,presentReady);
}

auto Program::quit() -> void { quitting = true; PostQuitMessage(0); }
auto Program::save() -> void { if(emulator->loaded()) emulator->save(); }
auto Program::reset() -> void {
  if(!emulator->loaded()) return;
  emulator->reset(); audio.clear(); resetFramePacer(); showMessage("Game reset");
}
auto Program::power() -> void {
  if(!emulator->loaded()) return;
  emulator->power(); audio.clear(); resetFramePacer(); showMessage("Power cycle");
}
auto Program::unload() -> void {
  if(!emulator->loaded()) return;
  nativeCloseTools();
  if(settings.emulator.autoSaveStateOnUnload) saveUndoState();
  emulator->unload();
  screenshot = {};
  superFamicom = {}; gameBoy = {}; bsMemory = {}; sufamiTurboA = {}; sufamiTurboB = {};
  fastLinkRAMTraceService.clearSnapshot();
  audio.clear(); video.clear(); resetFramePacer();
  nativeTitle(); nativeMenus();
  showMessage("Game unloaded");
}

auto Program::load() -> void {
  if(loading || inFrame) return;
  if(!gameQueue) {
    auto location = nativeFile(false, "Load SNES ROM", settings.path.games ? settings.path.games : settings.path.recent.superFamicom);
    if(!location) return;
    gameQueue.append({"Auto;", location});
  }
  loading = true;
  struct LoadGuard { bool& flag; ~LoadGuard() { flag = false; } } guard{loading};
  auto requested = gameQueue;
  unload(); gameQueue = requested;
  superFamicom = {}; gameBoy = {}; bsMemory = {}; sufamiTurboA = {}; sufamiTurboB = {};
  if(!emulator->load()) { gameQueue = {}; nativeError("Game loading failed. Check the ROM and required firmware."); return; }
  gameQueue = {};
  if(settings.emulator.warnOnUnverifiedGames && !verified()
    && !nativeConfirm("This ROM was not found in the game database. Load it?", "Unverified ROM")) { unload(); return; }
  emulator->power();
  if(settings.emulator.autoLoadStateOnLoad && hasState("Quick/Undo")) loadState("Quick/Undo");
  updateCheats();
  paused = frameAdvance = frameAdvanceLock = false;
  speed = 1.0; updateAudioFrequency();
  resetFramePacer();
  nativeTitle(); nativeMenus();
  string entry = {superFamicom.option, ";", superFamicom.location};
  if(gameBoy) entry.append("|", gameBoy.option, ";", gameBoy.location);
  if(bsMemory) entry.append("|", bsMemory.option, ";", bsMemory.location);
  if(sufamiTurboA) entry.append("|", sufamiTurboA.option, ";", sufamiTurboA.location);
  if(sufamiTurboB) entry.append("|", sufamiTurboB.option, ";", sufamiTurboB.location);
  vector<string> recent; recent.append(entry);
  for(uint i = 1; i <= 9 && recent.size() < 9; i++) {
    auto old = settings[string{"Game/Recent/", i}].text();
    if(old && old != entry) recent.append(old);
  }
  for(uint i = 0; i < 9; i++) settings(string{"Game/Recent/", i + 1}).setValue(i < recent.size() ? recent[i] : string{});
  settings.save(); nativeMenus();
  showMessage(verified() ? "Verified game loaded" : "Game loaded");
}

auto Program::load(uint id, string name, string type, vector<string> options) -> Emulator::Platform::Load {
  string location, option = "Auto";
  if(gameQueue) {
    auto entry = gameQueue.takeLeft().split(";", 1L);
    option = entry(0, "Auto"); location = entry(1);
  } else location = nativeFile(false, {"Load ", name});
  if(!location) return {};
  bool loaded = false;
  if(id == 1) { superFamicom.option = option; loaded = loadSuperFamicom(location); settings.path.recent.superFamicom = Location::dir(location); }
  if(id == 2) { gameBoy.option = option; loaded = loadGameBoy(location); settings.path.recent.gameBoy = Location::dir(location); }
  if(id == 3) { bsMemory.option = option; loaded = loadBSMemory(location); settings.path.recent.bsMemory = Location::dir(location); }
  if(id == 4) { sufamiTurboA.option = option; loaded = loadSufamiTurboA(location); settings.path.recent.sufamiTurboA = Location::dir(location); }
  if(id == 5) { sufamiTurboB.option = option; loaded = loadSufamiTurboB(location); settings.path.recent.sufamiTurboB = Location::dir(location); }
  return loaded ? Emulator::Platform::Load{id, option} : Emulator::Platform::Load{};
}

auto Program::videoFrame(const uint16* data, uint pitch, uint width, uint height, uint scale) -> void {
  screenshot = {data, pitch, width, height, scale};
  if(frameAdvance) frameAdvanceLock = true;
}

auto Program::presentVideo() -> void {
  if(!screenshot.data)return;
  auto data=screenshot.data;
  uint pitch=screenshot.pitch,width=screenshot.width,height=screenshot.height;
  bool capture = timing.enabled();
  auto begin = capture ? chrono::nanosecond() : 0;
  if(!settings.video.overscan) { uint rows = 8 * (height / 240); data += rows * (pitch / 2); height -= rows * 2; }
  auto size = video.size();
  // Windowed output follows the native pixel ratio, including while resizing.
  uint outWidth = 0, outHeight = 0;
  if(video.fullScreen() && settings.video.output != "Stretch" && size.width && size.height) {
    double baseWidth = settings.video.aspectCorrection ? 256.0 * 8.0 / 7.0 : 256.0;
    double baseHeight = settings.video.overscan ? 240.0 : 224.0;
    double factor = min(size.width / baseWidth, size.height / baseHeight);
    if(settings.video.output == "Center" && factor >= 1) factor = floor(factor);
    outWidth = (uint)(baseWidth * factor); outHeight = (uint)(baseHeight * factor);
  }
  if(auto target = video.acquire(width, height); target) {
    Filter::None::render(paused && settings.video.dimming ? paletteDimmed : palette,
      target.data, target.pitch, data, pitch, width, height);
    auto output = capture ? chrono::nanosecond() : 0;
    video.output(outWidth, outHeight);
    if(capture) timing.frameOutput += chrono::nanosecond() - output;
  }
  if(capture) timing.frameVideo += chrono::nanosecond() - begin;
  static uint count = 0; static uint64 previous = 0;
  count++;
  auto now = chrono::second();
  if(now != previous) {
    previous = now; statusFrameRate = {count, " FPS"}; count = 0;
    if(chrono::millisecond() - statusTime > 2000) nativeStatus(statusFrameRate);
  }
}

auto Program::audioFrame(const double* samples, uint) -> void {
  bool silent = settings.audio.mute || (settings.audio.muteUnfocused && !frameFocused) || (fastForwarding && settings.fastForward.mute);
  if(silent) { const double silence[2]{}; audio.output(silence); }
  else audio.output(samples);
}
auto Program::inputPoll(uint port, uint device, uint input) -> int16 {
  if(device != 1 || (!frameFocused && settings.input.defocus != "Allow")) return 0;
  return nativeInputManager.pollGamepad(port, input);
}
auto Program::showMessage(string text) -> void { statusTime = chrono::millisecond(); statusMessage = text; nativeStatus(text); }
auto Program::refresh() -> bool {
  if(inFrame) return false;
  // Vulkan's Win32 WSI may dispatch window messages while presenting. A
  // paused redraw needs the same protection against reentry as a core frame.
  inFrame=true;
  struct RefreshGuard { bool& flag; ~RefreshGuard() { flag=false; } } guard{inFrame};
  video.poll();
  if(!screenshot.data)return true;
  if(!video.ready())return false;
  presentVideo();return true;
}

auto Program::updateVideoDriver(HWND) -> bool {
  video.setContext((uintptr_t)videoWindow);
  video.setPresentMode(settings.video.presentMode);
  video.setSwapchainImages(settings.video.swapchainImages);
  video.setMonitor(settings.video.monitor);
  settings.video.flush=false;
  video.setFlush(false);
  video.setShader(settings.video.shader == "Blur" ? "Blur" : "None");
  video.create(NativeVideoDriverName);
  video.captureTiming(timing.enabled());
  if(!video.ready()) { nativeError("Could not initialize the native Vulkan backend."); return false; }
  return true;
}
auto Program::updateAudioDriver(HWND) -> bool {
  audio.reset(); audio.create(settings.audio.driver); audio.setContext((uintptr_t)mainWindow);
  if(!audio.ready()) { nativeError("Could not initialize audio output."); return false; }
  if(audio.hasDevice(settings.audio.device)) audio.setDevice(settings.audio.device);
  else settings.audio.device = audio.device();
  audio.setExclusive(settings.audio.exclusive);
  // Device backpressure belongs to the audio worker, never to SNES execution.
  settings.audio.blocking=false;
  audio.setBlocking(false);
  audio.setDynamic(settings.audio.dynamic);
  if(audio.hasFrequency(settings.audio.frequency)) audio.setFrequency(settings.audio.frequency);
  else settings.audio.frequency = audio.frequency();
  if(audio.hasLatency(settings.audio.latency)) audio.setLatency(settings.audio.latency);
  else settings.audio.latency = audio.latency();
  updateAudioFrequency(); updateAudioEffects(); return true;
}
auto Program::updateAudioFrequency() -> void {
  double multiplier = fastForwarding ? max(1u, settings.fastForward.limiter) : speed;
  Emulator::audio.setFrequency((settings.audio.frequency + settings.audio.skew) / multiplier);
}
auto Program::updateAudioEffects() -> void {
  Emulator::audio.setVolume(settings.audio.volume * 0.01);
  Emulator::audio.setBalance(max(-1.0, min(1.0, ((int)settings.audio.balance - 50) / 50.0)));
}
auto Program::toggleVideoFullScreen() -> void {
  video.setFullScreen(!video.fullScreen()); nativeMenus();
  refresh();
}
auto Program::updateCheats() -> void {
  vector<string> codes;
  if(settings.emulator.cheats.enable) {
    auto document = BML::unserialize(string::read(cheatPath()));
    for(auto entry : document.find("cheat")) if(entry["enable"]) {
      for(auto code : entry["code"].text().split("+")) {
        code.strip().downcase();
        if(gameBoy ? decodeGB(code) : decodeSNES(code)) codes.append(code);
      }
    }
  }
  emulator->cheats(codes);
}

auto Program::shutdown() -> void { disableWindowsLatencyMode(); }
