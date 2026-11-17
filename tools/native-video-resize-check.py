#!/usr/bin/env python3
"""Run actual manager reset/poll code without a Windows window or Vulkan device."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'bsnes/target-bsnes/video/native-vulkan.cpp').read_text()


def method(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


methods = [method(signature) for signature in (
    'NativeVideoManager::NativeVideoManager()',
    'NativeVideoManager::~NativeVideoManager()',
    'auto NativeVideoManager::reset()',
    'auto NativeVideoManager::poll()',
)]
# Include callback dispatch if it is reintroduced: the original null callback
# must make the regression fail rather than disappearing from the test harness.
for signature in ('auto NativeVideoManager::onUpdate(', 'auto NativeVideoManager::doUpdate('):
    if signature in source:
        methods.append(method(signature))

harness = r'''
#include <ruby/ruby.hpp>
#include <cassert>
#include <cstdio>
using namespace nall;
#define PLATFORM_WINDOWS
#include "bsnes/target-bsnes/video/video.hpp"

static unsigned wakeups=0;
static void SetEvent(void*) { ++wakeups; }
// No device is created. These fields only allow the inactive-device branch
// of the real poll method to compile; they are never accessed in this test.
struct NativeVideoImplementation {
  std::atomic<uint64_t> _resizeEpoch{0};
  std::atomic<bool> _surfaceSizeChanged{false},_presentPending{false};
  bool _framePrepared=false,_rebuildOnly=false;
  void* _presentWake=nullptr;
};
static NativeVideoManager::Size clientSize;
auto NativeVideoManager::currentClientSize() const -> Size { return clientSize; }
auto NativeVideoManager::defaultMonitorName() const -> string { return "Primary"; }
METHODS
int main() {
  const NativeVideoManager::Size sizes[]={{256,224},{511,337},{1024,896},{0,0},{256,240},{768,672}};
  NativeVideoManager video;
  for(unsigned cycle=0;cycle<128;cycle++) {
    video.reset();
    assert(video.size().width==0 && video.size().height==0);
    for(auto size:sizes) {
      clientSize=size;
      video.poll();video.poll();
      assert(video.size().width==size.width && video.size().height==size.height);
    }
  }
  assert(wakeups==0);
  std::puts("PASS: actual reset/poll code: 128 resets, 1536 size polls, native/arbitrary/minimized sizes; no device or callback needed");
}
'''.replace('METHODS', '\n'.join(methods))

with tempfile.TemporaryDirectory(prefix='fastlink-video-resize-') as directory:
    cpp = Path(directory) / 'check.cpp'
    exe = Path(directory) / 'check'
    cpp.write_text(harness)
    subprocess.run(['c++', '-std=c++17', '-O2', '-Wno-cast-user-defined', '-I', str(root), str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
