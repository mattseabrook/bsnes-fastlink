#pragma once

#include <cstdint>

// Host scheduling only: no core clocks or guest frames are changed or skipped.
struct FastLinkFramePacer {
  uint64_t deadline = 0;
  uint64_t discardedNanoseconds = 0;

  auto reset() -> void { deadline = 0; }

  auto next(uint64_t now, uint64_t duration) -> uint64_t {
    if(!deadline) {
      deadline = now + duration;
      return now;
    }
    // Recover ordinary scheduling stalls against the original timeline.
    // A debugger stop/suspend must not trigger seconds of catch-up gameplay.
    if(now > deadline && now - deadline > 250'000'000ull) {
      discardedNanoseconds += now - deadline;
      deadline = now;
    }
    auto target = deadline;
    deadline += duration;
    return target;
  }
};
