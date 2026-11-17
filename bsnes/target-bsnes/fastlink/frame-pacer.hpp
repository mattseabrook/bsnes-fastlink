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
    // Preserve the original timeline across host stalls of any duration.
    // Only explicit pause/resume/reset events may establish a new timeline.
    auto target = deadline;
    deadline += duration;
    return target;
  }
};
