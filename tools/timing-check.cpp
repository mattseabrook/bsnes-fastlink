#include "../bsnes/target-bsnes/fastlink/timing.hpp"
#include <cassert>
#include <cstdio>

int main() {
  FastLinkTiming timing;
  assert(!timing.enabled());
  timing.path = "capture-enabled";  // no file is opened until save()
  for(uint64_t i = 0; i < 60; i++) {
    uint64_t start = 1000 + i * 1000;
    timing.beginFrame();
    assert(timing.frameVideo == 0 && timing.frameOutput == 0);
    timing.frameVideo = 200;
    timing.frameOutput = 100;
    timing.record(start, start + 100, start + 200, start + 300,
      start + 800, start + 900, 1000, 0);
  }
  assert(timing.count == 1);
  const auto& s = timing.samples[0];
  assert(s.frames == 60 && s.nominal == 60000 && s.wall == 59900);
  assert(s.pacing + s.prepare + s.run + s.trace + s.other == s.wall);
  assert(s.video == 12000 && s.output == 6000);
  assert(s.maxRun == 500 && s.maxPrepare == 100);
  timing.idle();
  timing.beginFrame();
  timing.record(1000000000, 1000000100, 1000000200, 1000000300,
    1000000800, 1000000900, 1000, 0);
  assert(timing.current.wall == 900);  // paused time is excluded
  std::puts("PASS: capture disabled by default; time accounting, video attribution, pause exclusion, sample rollover");
}
