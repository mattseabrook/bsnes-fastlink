#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

struct FastLinkRAMTraceService {
  static constexpr uint32_t Port = 9000;
  static constexpr uint32_t WRAMSize = 128 * 1024;

  FastLinkRAMTraceService();
  ~FastLinkRAMTraceService();

  auto start() -> void;
  auto stop() -> void;
  auto clearSnapshot() -> void;
  auto publishFrame() -> void;

private:
  static constexpr uint32_t SnapshotSlots = 3;

  auto threadMain() -> void;
  auto requestSnapshot(uint64_t previousSequence) -> void;
  auto publish(const uint8_t* data, uint32_t size) -> void;
  auto copyActiveSnapshot(uint8_t* output, uint32_t capacity) -> uint32_t;

  std::array<std::array<uint8_t, WRAMSize>, SnapshotSlots> _snapshots = {};
  std::array<std::atomic<uint32_t>, SnapshotSlots> _readers = {};
  std::atomic<uint32_t> _activeSlot{0};
  std::atomic<uint64_t> _sequence{0};
  std::atomic<uint32_t> _publishedSize{WRAMSize};
  std::atomic<bool> _running{false};
  std::atomic<bool> _snapshotRequested{false};
  std::thread _thread;
  std::mutex _conditionMutex;
  std::condition_variable _condition;
};

extern FastLinkRAMTraceService fastLinkRAMTraceService;
