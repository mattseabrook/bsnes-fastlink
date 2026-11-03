#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

// Opt-in, bounded capture. No allocation or file I/O on the emulation path.
// Set FASTLINK_TIMING_LOG to a CSV filename; it is written on normal quit.
struct FastLinkTiming {
  struct Sample {
    uint64_t frames = 0, wall = 0, nominal = 0;
    uint64_t pacing = 0, prepare = 0, run = 0, trace = 0, other = 0;
    uint64_t maxRun = 0, maxPrepare = 0, discarded = 0;
    uint64_t video = 0, output = 0;
  };
  std::string path;
  std::array<Sample, 7200> samples{};
  Sample current;
  size_t count = 0;
  uint64_t previousEnd = 0;
  uint64_t frameVideo = 0, frameOutput = 0;

  auto initialize() -> void {
    if(auto value = std::getenv("FASTLINK_TIMING_LOG")) path = value;
  }
  auto enabled() const -> bool { return !path.empty() && count < samples.size(); }
  auto idle() -> void { previousEnd = 0; }
  auto beginFrame() -> void { frameVideo = frameOutput = 0; }
  auto record(uint64_t begin, uint64_t pace, uint64_t prepare, uint64_t run,
              uint64_t trace, uint64_t end, uint64_t nominal, uint64_t discarded) -> void {
    if(!enabled()) return;
    current.frames++;
    current.wall += end - (previousEnd ? previousEnd : begin);
    current.nominal += nominal;
    current.pacing += prepare - pace;
    current.prepare += run - prepare;
    current.run += trace - run;  // includes videoFrame/filter/submit/present callbacks
    current.video += frameVideo;
    current.output += frameOutput;
    current.trace += end - trace;
    current.other += pace - begin + (previousEnd ? begin - previousEnd : 0);
    if(trace - run > current.maxRun) current.maxRun = trace - run;
    if(run - prepare > current.maxPrepare) current.maxPrepare = run - prepare;
    current.discarded += discarded;
    previousEnd = end;
    if(current.frames == 60) {
      samples[count++] = current;
      current = {};
    }
  }
  auto save() -> void {
    if(path.empty()) return;
    if(current.frames && count < samples.size()) samples[count++] = current;
    auto file = std::fopen(path.c_str(), "w");
    if(!file) return;
    std::fprintf(file, "frames,wall_ms,nominal_ms,speed_percent,pacing_ms,prepare_ms,run_with_video_ms,ram_trace_ms,other_ms,max_run_ms,max_prepare_ms,discarded_ms,video_callback_ms,video_output_ms\n");
    for(size_t i = 0; i < count; i++) {
      const auto& s = samples[i];
      std::fprintf(file, "%llu,%.6f,%.6f,%.4f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n",
        (unsigned long long)s.frames, s.wall / 1e6, s.nominal / 1e6,
        s.wall ? 100.0 * s.nominal / s.wall : 0.0, s.pacing / 1e6,
        s.prepare / 1e6, s.run / 1e6, s.trace / 1e6, s.other / 1e6,
        s.maxRun / 1e6, s.maxPrepare / 1e6, s.discarded / 1e6,
        s.video / 1e6, s.output / 1e6);
    }
    std::fclose(file);
  }
};
