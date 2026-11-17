#!/usr/bin/env python3
"""Exercise the actual Win32 mask methods on a host without Windows/HID devices."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'bsnes/target-bsnes/input/native-win32.cpp').read_text()
constants = source[source.index('constexpr uint MaximumRawButtons'):source.index('auto applyNativeInputThreadAffinity')]


def method(name):
    start = source.index('auto NativeInputManager::' + name + '(')
    return source[start:source.index('\nauto ', start + 1)]


harness = r'''
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
using uint = unsigned;
struct { struct { std::string genericHIDMapping; } fastLink; } settings;
CONSTANTS
struct NativeInputManager {
  struct HidPadState { bool attached = false, left = false, right = false, up = false, down = false; bool buttons[32] = {}; };
  struct HidPad { uint16_t vendorID = 0, productID = 0; HidPadState state; };
  struct Mask {
    uint32_t value = 0, expected = 0;
    uint stores = 0;
    void store(uint32_t next, std::memory_order) {
      // Check every publication, including intermediate states visible to a poll.
      assert(next == expected);
      value = next;
      stores++;
    }
  };
  std::array<Mask, 4> _padMasks;
  std::vector<HidPad> _pads;
  auto padToMask(const HidPad&) const -> uint32_t;
  auto refreshPadMasks() -> void;
};
METHODS
int main() {
  NativeInputManager manager;
  const int retro[] = {1,5,0,4,6,7,2,3}, standard[] = {0,1,2,3,4,5,6,7};
  for(bool compatibility : {false, true}) for(uint device = 0; device < 4; device++) {
    settings.fastLink.genericHIDMapping = compatibility ? "SNES Adapter" : "Standard HID";
    NativeInputManager::HidPad pad;
    pad.vendorID = device == 3 ? 0x1234 : 0x0f00;
    pad.productID = device == 0 ? 0x0003 : device == 1 ? 0x00f1 : 0x9999;
    const int* mapping = compatibility || device < 2 ? retro : standard;
    for(uint bits = 0; bits < 65536; bits++) {
      pad.state.up = bits & 1; pad.state.down = bits & 2;
      pad.state.left = bits & 4; pad.state.right = bits & 8;
      for(uint button = 0; button < 32; button++) pad.state.buttons[button] = bits & (1u << (4 + button % 12));
      uint32_t expected = bits & 15;
      for(uint button = 0; button < 8; button++) expected |= uint32_t(pad.state.buttons[mapping[button]]) << (4 + button);
      assert(manager.padToMask(pad) == expected);
    }
  }
  // Disconnected entries, port compaction, overflow, and removal of stale ports.
  for(uint count = 0; count <= 6; count++) for(uint attached = 0; attached < (1u << count); attached++) {
    manager._pads.assign(count, {});
    uint port = 0;
    for(auto& mask : manager._padMasks) { mask.value = 0xfff; mask.expected = 0; mask.stores = 0; }
    for(uint i = 0; i < count; i++) {
      auto& pad = manager._pads[i];
      pad.state.attached = attached & (1u << i);
      pad.state.up = true; pad.state.buttons[i] = true;
      if(pad.state.attached && port < 4) manager._padMasks[port++].expected = manager.padToMask(pad);
    }
    manager.refreshPadMasks();
    for(auto& mask : manager._padMasks) { assert(mask.stores == 1); assert(mask.value == mask.expected); }
  }
  std::puts("PASS: 524288 button/direction combinations, both mappings, known/unknown devices; one publication per port, disconnects, compaction, port limit");
}
'''.replace('CONSTANTS', constants).replace('METHODS', method('padToMask') + method('refreshPadMasks'))

with tempfile.TemporaryDirectory(prefix='fastlink-input-') as directory:
    cpp = Path(directory) / 'check.cpp'
    exe = Path(directory) / 'check'
    cpp.write_text(harness)
    subprocess.run(['c++', '-std=c++17', '-O2', '-Wall', '-Wextra', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
