#include <ruby/ruby.hpp>
#include <cassert>
#include <cstdio>

struct BlockingAudio : ruby::AudioDriver {
  BlockingAudio(ruby::Audio& audio) : AudioDriver(audio) {}
  std::mutex mutex;
  std::condition_variable condition;
  bool active = false, entered = false;
  unsigned calls = 0, changes = 0;

  auto hasLatencies() -> nall::vector<uint> override { return {0, 1}; }
  auto setLatency(uint) -> bool override {
    std::lock_guard<std::mutex> lock(mutex);
    assert(calls == 0);  // device reinitialization cannot race output
    changes++;
    return true;
  }
  auto setWorkerActive(bool value) -> void override {
    std::lock_guard<std::mutex> lock(mutex);
    active = value;
    condition.notify_all();
  }
  auto output(const double[]) -> void override {
    std::unique_lock<std::mutex> lock(mutex);
    calls++;
    entered = true;
    condition.notify_all();
    // Emulate a stalled endpoint which never produces another audio event.
    condition.wait(lock, [&] { return !active; });
    calls--;
  }
};

struct TestAudio : ruby::Audio {
  auto install() -> BlockingAudio* {
    auto driver = new BlockingAudio(*this);
    instance = driver;
    return driver;
  }
};

int main() {
  TestAudio audio;
  auto driver = audio.install();
  audio.startWorker();
  const double sample[] = {0.25, -0.25};
  for(unsigned i = 0; i < 1000; i++) audio.output(sample);
  {
    std::unique_lock<std::mutex> lock(driver->mutex);
    assert(driver->condition.wait_for(lock, std::chrono::seconds(2), [&] { return driver->entered; }));
  }
  assert(audio.setLatency(1));
  assert(driver->changes == 1);
  audio.stopWorker();
  std::puts("PASS: stalled worker cancellation; device change occurs after output exits; worker restart and shutdown");
}
