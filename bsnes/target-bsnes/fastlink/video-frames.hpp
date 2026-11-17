#pragma once
#include <array>
#include <atomic>
#include <cstdint>

// One producer, one renderer. Three fixed buffers retain only the latest
// completed image; a slow driver never makes the producer wait or allocate.
struct FastLinkVideoFrames {
  static constexpr uint32_t Slots=3,MaxWidth=512,MaxHeight=480;
  struct Frame {
    std::array<uint32_t,MaxWidth*MaxHeight> pixels;
    uint32_t width=0,height=0,outputWidth=0,outputHeight=0;
    uint64_t sequence=0;
  };
  struct alignas(64) Readers { std::atomic<uint32_t> count{0}; };
  std::array<Frame,Slots> frames;
  std::array<Readers,Slots> readers;
  alignas(64) std::atomic<uint32_t> active{Slots};
  uint32_t writing=Slots;
  uint64_t sequence=0;

  auto acquire(uint32_t width,uint32_t height) -> uint32_t* {
    writing=Slots;
    if(!width || !height || width>MaxWidth || height>MaxHeight)return nullptr;
    uint32_t published=active.load();
    for(uint32_t slot=0;slot<Slots;slot++)if(slot!=published && !readers[slot].count.load()) {
      writing=slot;frames[slot].width=width;frames[slot].height=height;
      return frames[slot].pixels.data();
    }
    return nullptr;
  }

  auto publish(uint32_t width,uint32_t height) -> void {
    if(writing==Slots)return;
    auto& frame=frames[writing];frame.outputWidth=width;frame.outputHeight=height;frame.sequence=++sequence;
    active.store(writing);writing=Slots;
  }

  auto read() -> uint32_t {
    for(uint32_t attempt=0;attempt<8;attempt++) {
      uint32_t slot=active.load();if(slot==Slots)return Slots;
      readers[slot].count.fetch_add(1);
      if(active.load()==slot)return slot;
      readers[slot].count.fetch_sub(1);
    }
    return Slots;
  }

  auto release(uint32_t slot) -> void { readers[slot].count.fetch_sub(1); }
};
