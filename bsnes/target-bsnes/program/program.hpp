#pragma once
#include "../fastlink/frame-pacer.hpp"
#include "../fastlink/timing.hpp"

struct Program : Emulator::Platform {
  auto create() -> bool;
  auto main() -> void;
  auto quit() -> void;
  auto shutdown() -> void;
  auto paceFrame() -> void;
  auto resetFramePacer() -> void { framePacer.reset(); }
  auto focused() const -> bool;
  auto inactive() const -> bool;
  auto load() -> void;
  auto save() -> void;
  auto reset() -> void;
  auto power() -> void;
  auto unload() -> void;
  auto verified() const -> bool;
  auto loadFile(string location) -> vector<uint8_t>;
  auto loadSuperFamicom(string location) -> bool;
  auto loadGameBoy(string location) -> bool;
  auto loadBSMemory(string location) -> bool;
  auto loadSufamiTurboA(string location) -> bool;
  auto loadSufamiTurboB(string location) -> bool;
  auto open(uint id, string name, vfs::file::mode mode, bool required) -> shared_pointer<vfs::file> override;
  auto load(uint id, string name, string type, vector<string> options = {}) -> Emulator::Platform::Load override;
  auto videoFrame(const uint16* data, uint pitch, uint width, uint height, uint scale) -> void override;
  auto audioFrame(const double* samples, uint channels) -> void override;
  auto inputPoll(uint port, uint device, uint input) -> int16 override;
  auto inputRumble(uint, uint, uint, bool) -> void override {}
  auto openPakSuperFamicom(string name, vfs::file::mode mode) -> shared_pointer<vfs::file>;
  auto openRomSuperFamicom(string name, vfs::file::mode mode) -> shared_pointer<vfs::file>;
  auto openPakGameBoy(string name, vfs::file::mode mode) -> shared_pointer<vfs::file>;
  auto openRomGameBoy(string name, vfs::file::mode mode) -> shared_pointer<vfs::file>;
  auto openPakBSMemory(string name, vfs::file::mode mode) -> shared_pointer<vfs::file>;
  auto openRomBSMemory(string name, vfs::file::mode mode) -> shared_pointer<vfs::file>;
  auto openPakSufamiTurboA(string name, vfs::file::mode mode) -> shared_pointer<vfs::file>;
  auto openRomSufamiTurboA(string name, vfs::file::mode mode) -> shared_pointer<vfs::file>;
  auto openPakSufamiTurboB(string name, vfs::file::mode mode) -> shared_pointer<vfs::file>;
  auto openRomSufamiTurboB(string name, vfs::file::mode mode) -> shared_pointer<vfs::file>;
  auto path(string type, string location, string extension = "") -> string;
  auto gamePath() -> string;
  auto cheatPath() -> string;
  auto statePath() -> string;
  auto screenshotPath() -> string;
  auto appliedPatch() const -> bool;
  auto applyPatchIPS(vector<uint8_t>& data, string location) -> bool;
  auto applyPatchBPS(vector<uint8_t>& data, string location) -> bool;
  struct State { string name; uint64_t date; static const uint Signature; };
  auto availableStates(string type) -> vector<State>;
  auto hasState(string filename) -> bool;
  auto loadStateData(string filename) -> vector<uint8_t>;
  auto loadState(string filename) -> bool;
  auto saveState(string filename) -> bool;
  auto saveUndoState() -> bool;
  auto saveRedoState() -> bool;
  auto removeState(string filename) -> bool;
  auto renameState(string from, string to) -> bool;
  auto captureScreenshot() -> bool;
  auto updateVideoPalette() -> void;
  auto updateVideoDriver(HWND owner) -> bool;
  auto updateAudioDriver(HWND owner) -> bool;
  auto updateAudioFrequency() -> void;
  auto updateAudioEffects() -> void;
  auto toggleVideoFullScreen() -> void;
  auto refresh() -> bool;
  auto presentVideo() -> void;
  auto showMessage(string text) -> void;
  auto updateCheats() -> void;
  auto decodeSNES(string& code) -> bool;
  auto decodeGB(string& code) -> bool;
  struct Game {
    explicit operator bool() const { return (bool)location; }

    string option;
    string location;
    string manifest;
    Markup::Node document;
    boolean patched;
    boolean verified;
  };

  struct SuperFamicom : Game {
    string title;
    string region;
    vector<uint8_t> program;
    vector<uint8_t> data;
    vector<uint8_t> expansion;
    vector<uint8_t> firmware;
  } superFamicom;

  struct GameBoy : Game {
    vector<uint8_t> program;
  } gameBoy;

  struct BSMemory : Game {
    vector<uint8_t> program;
  } bsMemory;

  struct SufamiTurbo : Game {
    vector<uint8_t> program;
  } sufamiTurboA, sufamiTurboB;

  vector<string> gameQueue;

  uint32_t palette[32768];
  uint32_t paletteDimmed[32768];

  struct Screenshot {
    const uint16* data = nullptr;
    uint pitch  = 0;
    uint width  = 0;
    uint height = 0;
    uint scale  = 0;
  } screenshot;

  bool loading = false, inFrame = false, paused = false;
  bool hostSuspended = false;
  bool frameFocused = true;
  bool frameAdvance = false, frameAdvanceLock = false;
  bool fastForwarding = false, startFullScreen = false;
  double speed = 1.0;
  uint mute = 0;
  struct Mute { enum : uint { Always = 2, Unfocused = 4, FastForward = 8 }; };
  uint64 statusTime = 0;
  string statusMessage, statusFrameRate;
  FastLinkFramePacer framePacer;
  FastLinkTiming timing;
};
extern Program program;
