#include "../bsnes/target-bsnes/fastlink/video-frames.hpp"
#include "../bsnes/target-bsnes/fastlink/video-size.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <memory>
#include <thread>

int main() {
  for(bool overscan:{false,true}) {
    int nativeHeight=overscan?240:224;
    for(int width=1;width<=8192;width++) {
      auto size=fastLinkResizeVideo(width,1,false,overscan);
      assert(size.width>=256 && !(size.width&1) && size.height>=nativeHeight);
      int64_t error=int64_t(size.height)*256-int64_t(size.width)*nativeHeight;
      assert(error>=-128 && error<=128);
    }
    for(int scale=1;scale<=16;scale++)for(int delta=-10;delta<=10;delta++) {
      auto size=fastLinkResizeVideo(scale*256+delta,1,false,overscan);
      assert(size.width==scale*256 && size.height==scale*nativeHeight);
    }
    for(int scale=2;scale<=16;scale++)for(int delta:{-11,11})
      assert(fastLinkResizeVideo(scale*256+delta,1,false,overscan).width!=scale*256);
    for(int height=1;height<=4096;height++) {
      auto size=fastLinkResizeVideo(1,height,true,overscan);
      assert(size.width>=256 && !(size.width&1) && size.height>=nativeHeight);
    }
  }
  {
    auto frames=std::make_unique<FastLinkVideoFrames>();
    auto pixels=frames->acquire(128,64);assert(pixels);
    std::fill_n(pixels,128*64,1);frames->publish(0,0);
    auto held=frames->read();assert(held<frames->Slots);
    // A renderer can retain a frame throughout a long stall. The producer
    // still publishes continuously and never modifies the retained image.
    for(unsigned sequence=2;sequence<=2000;sequence++) {
      pixels=frames->acquire(128,64);assert(pixels);
      std::fill_n(pixels,128*64,sequence);frames->publish(0,0);
    }
    assert(frames->frames[held].sequence==1);
    for(unsigned i=0;i<128*64;i++)assert(frames->frames[held].pixels[i]==1);
    frames->release(held);
    auto latest=frames->read();assert(latest<frames->Slots);
    assert(frames->frames[latest].sequence==2000);
    frames->release(latest);
  }
  {
    auto frames=std::make_unique<FastLinkVideoFrames>();
    std::atomic<bool> done{false};
    std::thread producer([&] {
      for(unsigned sequence=1;sequence<=10000;sequence++) {
        auto pixels=frames->acquire(128,64);assert(pixels);
        std::fill_n(pixels,128*64,sequence);frames->publish(0,0);
        if(!(sequence%32))std::this_thread::yield();
      }
      done=true;
    });
    uint64_t observed=0;
    while(!done || observed<10000) {
      auto slot=frames->read();if(slot==frames->Slots)continue;
      const auto& frame=frames->frames[slot];
      assert(frame.width==128 && frame.height==64 && frame.sequence>=observed);
      for(unsigned i=0;i<128*64;i++)assert(frame.pixels[i]==frame.sequence);
      observed=frame.sequence;frames->release(slot);
    }
    producer.join();
  }
  std::puts("PASS: even native-aspect widths, 10px snapping, minimum size; stalled renderer and 10000 concurrent frame publications stay coherent");
}
