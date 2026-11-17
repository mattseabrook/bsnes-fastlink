#include "../bsnes/target-bsnes/fastlink/frame-pacer.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>

int main() {
  constexpr uint64_t period = 16'639'264;
  FastLinkFramePacer pacer;
  uint64_t now = 1'000'000'000;
  const auto start = now;
  assert(pacer.next(now, period) == now);
  assert(pacer.next(now + 1'000'000, period) == start + period);
  pacer.reset();
  assert(pacer.next(now, period) == now);
  now += 50'000'000;
  assert(pacer.next(now, period) == start + period);
  assert(pacer.next(now, period) == start + 2 * period);
  assert(pacer.discardedNanoseconds == 0);

  now += 30'000'000'000ull;
  assert(pacer.next(now, period) == start + 3 * period);
  assert(pacer.discardedNanoseconds == 0);
  // A real pause or system suspend is an explicit reset, not a stall heuristic.
  pacer.reset();
  assert(pacer.next(now, period) == now);
  assert(pacer.next(now, period) == now + period);

  pacer = {};
  now = start;
  uint64_t oldNow = start, oldDeadline = 0;
  constexpr unsigned frames = 18'000;
  for(unsigned frame = 0; frame < frames; frame++) {
    now = std::max(now, pacer.next(now, period));
    if(!oldDeadline || oldNow > oldDeadline + period) oldDeadline = oldNow + period;
    else { oldNow = std::max(oldNow, oldDeadline); oldDeadline += period; }
    // A short host scheduling stall every two seconds, not guest/SNES lag.
    const auto work = frame % 600 == 599 ? 350'000'000ull
      : frame % 120 == 119 ? 100'000'000ull : 2'000'000ull;
    now += work;
    oldNow += work;
  }
  // End on a stall: the final debt is recovered by the following frames.
  assert(now - start <= frames * period + 350'000'000ull);
  assert(pacer.discardedNanoseconds == 0);
  assert(oldNow - now > 4'000'000'000ull);
  std::printf("PASS: 100/350ms stalls retain timeline, explicit pause/suspend reset; %u frames: old %.3fs, new %.3fs, nominal %.3fs\n",
    frames, (oldNow - start) / 1e9, (now - start) / 1e9, frames * period / 1e9);
}
