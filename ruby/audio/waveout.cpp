#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>

auto CALLBACK waveOutCallback(HWAVEOUT handle, UINT message, DWORD_PTR userData, DWORD_PTR, DWORD_PTR) -> void;

struct AudioWaveOut : AudioDriver {
  AudioWaveOut& self = *this;
  AudioWaveOut(Audio& super) : AudioDriver(super) {}
  ~AudioWaveOut() { terminate(); }

  auto create() -> bool override {
    super.setChannels(2);
    super.setFrequency(44100);
    super.setLatency(512);
    return initialize();
  }

  auto driver() -> string override { return "waveOut"; }
  auto ready() -> bool override { return handle != nullptr; }

  auto hasDevices() -> vector<string> override {
    vector<string> devices{"Default"};
    for(uint index : range(waveOutGetNumDevs())) {
      WAVEOUTCAPS caps{};
      if(waveOutGetDevCaps(index, &caps, sizeof(WAVEOUTCAPS)) == MMSYSERR_NOERROR) {
        devices.append((const char*)utf8_t(caps.szPname));
      }
    }
    return devices;
  }

  auto hasBlocking() -> bool override { return true; }
  auto hasDynamic() -> bool override { return true; }
  auto hasFrequencies() -> vector<uint> override { return {44100}; }
  auto hasLatencies() -> vector<uint> override { return {512, 384, 320, 256, 192, 160, 128, 96, 80, 64, 48, 40, 32}; }

  auto setBlocking(bool blocking) -> bool override { return true; }
  auto setDynamic(bool dynamic) -> bool override { return initialize(); }
  auto setLatency(uint latency) -> bool override { return initialize(); }

  auto clear() -> void override {
    if(!handle) return;
    waveOutReset(handle);
    frameIndex = 0;
    blockIndex = 0;
    InterlockedExchange(&blockQueue, 0);
    for(auto& header : headers) {
      memory::fill(header.lpData, frameCount * 4);
    }
    waveOutRestart(handle);
  }

  auto level() -> double override {
    if(!blockCount || !frameCount) return 0.0;
    LONG queued = blockQueue;
    if(queued < 0) queued = 0;
    if(queued > (LONG)blockCount) queued = blockCount;
    return (double)((queued * frameCount) + frameIndex) / (blockCount * frameCount);
  }

  auto output(const double samples[]) -> void override {
    if(!handle || !ensureWritableBlock()) return;

    uint16_t lsample = sclamp<16>(samples[0] * 32767.0);
    uint16_t rsample = sclamp<16>(samples[1] * 32767.0);

    auto block = (uint32_t*)headers[blockIndex].lpData;
    block[frameIndex] = lsample << 0 | rsample << 16;

    if(++frameIndex >= frameCount) {
      frameIndex = 0;
      submitBlock();
    }
  }

private:
  auto blockWritable(uint index) -> bool {
    return !(headers[index].dwFlags & WHDR_INQUEUE);
  }

  auto ensureWritableBlock() -> bool {
    bool waitForBlock = self.blocking || self.dynamic;

    for(uint attempt : range(blockCount)) {
      if(blockWritable(blockIndex)) return true;
      if(waitForBlock) break;
      if(++blockIndex >= blockCount) blockIndex = 0;
      frameIndex = 0;
    }

    if(!waitForBlock) return false;
    while(handle && !blockWritable(blockIndex)) Sleep(1);
    return handle && blockWritable(blockIndex);
  }

  auto submitBlock() -> void {
    bool waitForBlock = self.blocking || self.dynamic;

    while(handle) {
      auto result = waveOutWrite(handle, &headers[blockIndex], sizeof(WAVEHDR));
      if(result == MMSYSERR_NOERROR) {
        InterlockedIncrement(&blockQueue);
        if(++blockIndex >= blockCount) blockIndex = 0;
        return;
      }

      if(result != WAVERR_STILLPLAYING || !waitForBlock) {
        return;
      }

      Sleep(1);
    }
  }

  auto initialize() -> bool {
    terminate();

    auto deviceIndex = hasDevices().find(self.device);
    if(!deviceIndex) deviceIndex = 0;

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = 44100;
    format.nBlockAlign = 4;
    format.wBitsPerSample = 16;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    format.cbSize = 0;  //not sizeof(WAVEFORMAT); size of extra information after WAVEFORMATEX
    //-1 = default; 0+ = specific device; subtract -1 as hasDevices() includes "Default" entry
    if(waveOutOpen(&handle, (int)*deviceIndex - 1, &format, (DWORD_PTR)waveOutCallback, (DWORD_PTR)this, CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
      handle = nullptr;
      return false;
    }

    frameCount = self.latency;
    blockCount = 32;
    frameIndex = 0;
    blockIndex = 0;
    blockQueue = 0;

    headers.resize(blockCount);
    for(auto& header : headers) {
      memory::fill(&header, sizeof(WAVEHDR));
      header.lpData = (LPSTR)LocalAlloc(LMEM_FIXED, frameCount * 4);
      header.dwBufferLength = frameCount * 4;
      waveOutPrepareHeader(handle, &header, sizeof(WAVEHDR));
    }

    waveOutSetVolume(handle, 0xffff'ffff);  //100% volume (65535 left, 65535 right)
    waveOutRestart(handle);
    return true;
  }

  auto terminate() -> void {
    if(!handle) return;
    waveOutPause(handle);
    waveOutReset(handle);
    for(auto& header : headers) {
      waveOutUnprepareHeader(handle, &header, sizeof(WAVEHDR));
      LocalFree(header.lpData);
    }
    waveOutClose(handle);
    handle = nullptr;
    headers.reset();
  }

  HWAVEOUT handle = nullptr;
  vector<WAVEHDR> headers;
  uint frameCount = 0;
  uint blockCount = 0;
  uint frameIndex = 0;
  uint blockIndex = 0;

public:
  LONG blockQueue = 0;
};

auto CALLBACK waveOutCallback(HWAVEOUT handle, UINT message, DWORD_PTR userData, DWORD_PTR, DWORD_PTR) -> void {
  auto instance = (AudioWaveOut*)userData;
  if(message == MM_WOM_DONE && instance && instance->blockQueue > 0) InterlockedDecrement(&instance->blockQueue);
}
