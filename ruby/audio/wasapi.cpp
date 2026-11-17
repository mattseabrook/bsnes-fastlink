#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <devicetopology.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <cstdarg>
#include <cstdio>

// WASAPI GUIDs (not provided by import libs with clang-cl)
#ifndef __MINGW32__
#include <initguid.h>
DEFINE_GUID(CLSID_MMDeviceEnumerator, 0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
DEFINE_GUID(IID_IMMDeviceEnumerator,  0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
DEFINE_GUID(IID_IAudioClient,         0x1CB9AD4C, 0xDBFA, 0x4C32, 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
DEFINE_GUID(IID_IAudioRenderClient,   0xF294ACFC, 0x3146, 0x4483, 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2);
#endif

namespace {
auto WASAPITrace(const char* format, ...) -> void {
}
}

struct AudioWASAPI : AudioDriver {
  AudioWASAPI& self = *this;
  AudioWASAPI(Audio& super) : AudioDriver(super) { construct(); }
  ~AudioWASAPI() { destruct(); if(workerStopEvent) CloseHandle(workerStopEvent); }

  auto create() -> bool override {
    if(!workerStopEvent) return false;
    WASAPITrace("create begin devices=%u", (uint)self.devices.size());
    if(!self.devices) {
      WASAPITrace("create failed: no active render devices");
      return false;
    }
    self.exclusive = false;
    self.device = hasDevices().first();
    self.blocking = false;
    self.channels = 2;
    self.frequency = 48000;
    self.latency = 0;
    auto result = initialize();
    WASAPITrace(
      "create done result=%d ready=%d device=%s frequency=%u channels=%u latency=%u exclusive=%d blocking=%d",
      result,
      self.isReady,
      (const char*)self.device,
      self.frequency,
      self.channels,
      self.latency,
      self.exclusive,
      self.blocking
    );
    return result;
  }

  auto driver() -> string override { return "WASAPI"; }
  auto ready() -> bool override { return self.isReady; }

  auto hasExclusive() -> bool override { return false; }
  auto hasBlocking() -> bool override { return true; }

  auto hasDevices() -> vector<string> override {
    vector<string> devices;
    for(auto& device : self.devices) devices.append(device.name);
    return devices;
  }

  auto hasChannels() -> vector<uint> override {
    return {self.channels};
  }

  auto hasFrequencies() -> vector<uint> override {
    return {self.frequency};
  }

  auto hasLatencies() -> vector<uint> override {
    return {0, 20, 40, 60, 80, 100};
  }

  auto setExclusive(bool exclusive) -> bool override { return exclusive == false; }
  auto setDevice(string device) -> bool override { return initialize(); }
  auto setBlocking(bool blocking) -> bool override { return true; }
  auto setFrequency(uint frequency) -> bool override { return initialize(); }
  auto setLatency(uint latency) -> bool override { return initialize(); }

  auto setWorkerActive(bool active) -> void override {
    workerStopping.store(!active, ::std::memory_order_release);
    if(active) ResetEvent(workerStopEvent);
    else SetEvent(workerStopEvent);
  }

  auto clear() -> void override {
    static uint clearTraceCount = 0;
    if(clearTraceCount++ < 40) WASAPITrace("clear ready=%d audioClient=%p", self.isReady, (void*)self.audioClient);
    resetQueue();
    if(!self.audioClient) return;
    HRESULT hr = self.audioClient->Stop();
    if(hr != S_OK && recoverFromDeviceInterrupt(hr)) return;
    hr = self.audioClient->Reset();
    if(hr != S_OK && recoverFromDeviceInterrupt(hr)) return;
    hr = self.audioClient->Start();
    if(hr != S_OK) recoverFromDeviceInterrupt(hr);
  }

  auto output(const double samples[]) -> void override {
    if(workerStopping.load(::std::memory_order_acquire)) return;
    #if defined(BUILD_DEBUG)
    static uint outputTraceCount = 0;
    bool traceOutput = outputTraceCount++ < 80;
    #else
    constexpr bool traceOutput = false;
    #endif
    if(traceOutput) {
      WASAPITrace(
        "output begin ready=%d audioClient=%p renderClient=%p event=%p queue=%u",
        self.isReady,
        (void*)self.audioClient,
        (void*)self.renderClient,
        (void*)self.eventHandle,
        self.queue.count
      );
    }
    if(!self.isReady || !self.audioClient || !self.renderClient || !self.eventHandle) {
      if(traceOutput) WASAPITrace("output skipped: not ready");
      return;
    }

    const uint32_t quantum = self.outputQuantum;
    const uint32_t queueLimit = self.outputQueueLimit;

    // Pace against one device period worth of audio instead of the entire endpoint buffer.
    // Shared-mode WASAPI can expose a large buffer, and waiting to fill it first creates a fixed startup delay.
    if(self.queue.count >= queueLimit) {
      // Audio::output feeds WASAPI from its dedicated MMCSS worker.  Pace that
      // worker against the endpoint even when the public "blocking" option is
      // off; the emulation thread remains nonblocking because it only writes
      // to Audio's SPSC ring.  Dropping queued samples here caused discontinuous
      // waveforms (clicks) whenever the UI thread was briefly descheduled.
      while(self.queue.count >= queueLimit) {
        if(waitForAudio(INFINITE) != WAIT_OBJECT_0) return;
        if(!write()) break;
      }
    }

    for(uint n : range(self.channels)) {
      self.queue.samples[self.queue.write][n] = n < 2 ? samples[n] : 0.0;
    }
    self.queue.write++;
    self.queue.count++;

    if(self.queue.count >= quantum) {
      if(!self.blocking || waitForAudio(INFINITE) == WAIT_OBJECT_0) {
        write();
      }
    }
    if(traceOutput) WASAPITrace("output done queue=%u quantum=%u limit=%u", self.queue.count, quantum, queueLimit);
  }

private:
  // Independent of the endpoint handle, which recovery can recreate. Shutdown
  // can wake an endpoint wait even after the device stops generating events.
  HANDLE workerStopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  ::std::atomic<bool> workerStopping{false};
  auto waitForAudio(DWORD timeout) -> DWORD {
    HANDLE events[] = {workerStopEvent, self.eventHandle};
    auto result = WaitForMultipleObjects(2, events, FALSE, timeout);
    return result == WAIT_OBJECT_0 + 1 ? WAIT_OBJECT_0 : WAIT_FAILED;
  }

  struct Device {
    string id;
    string name;
  };
  vector<Device> devices;

  auto resetQueue() -> void {
    self.queue.read = 0;
    self.queue.write = 0;
    self.queue.count = 0;
  }

  auto recoverable(HRESULT hr) -> bool {
    return hr == AUDCLNT_E_DEVICE_INVALIDATED
        || hr == AUDCLNT_E_RESOURCES_INVALIDATED
        || hr == AUDCLNT_E_SERVICE_NOT_RUNNING;
  }

  auto recoverFromDeviceInterrupt(HRESULT hr) -> bool {
    if(!recoverable(hr) || self.recovering) return false;

    WASAPITrace("recover begin hr=0x%08lx", (unsigned long)hr);
    self.recovering = true;
    resetQueue();
    bool result = initialize();
    self.recovering = false;
    WASAPITrace("recover done result=%d ready=%d", result, self.isReady);
    return result;
  }

  auto construct() -> bool {
    WASAPITrace("construct begin");
    HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)&self.enumerator);
    if(hr != S_OK) {
      WASAPITrace("construct CoCreateInstance failed hr=0x%08lx", (unsigned long)hr);
      return false;
    }

    IMMDevice* defaultDeviceContext = nullptr;
    hr = self.enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDeviceContext);
    if(hr != S_OK) {
      WASAPITrace("construct GetDefaultAudioEndpoint failed hr=0x%08lx", (unsigned long)hr);
      return false;
    }

    Device defaultDevice;
    LPWSTR defaultDeviceString = nullptr;
    defaultDeviceContext->GetId(&defaultDeviceString);
    defaultDevice.id = (const char*)utf8_t(defaultDeviceString);
    CoTaskMemFree(defaultDeviceString);

    IMMDeviceCollection* deviceCollection = nullptr;
    hr = self.enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &deviceCollection);
    if(hr != S_OK) {
      WASAPITrace("construct EnumAudioEndpoints failed hr=0x%08lx", (unsigned long)hr);
      return false;
    }

    uint deviceCount = 0;
    hr = deviceCollection->GetCount(&deviceCount);
    if(hr != S_OK) {
      WASAPITrace("construct GetCount failed hr=0x%08lx", (unsigned long)hr);
      return false;
    }
    WASAPITrace("construct active device count=%u", deviceCount);

    for(uint deviceIndex : range(deviceCount)) {
      IMMDevice* deviceContext = nullptr;
      if(deviceCollection->Item(deviceIndex, &deviceContext) != S_OK) continue;

      Device device;
      LPWSTR deviceString = nullptr;
      deviceContext->GetId(&deviceString);
      device.id = (const char*)utf8_t(deviceString);
      CoTaskMemFree(deviceString);

      IPropertyStore* propertyStore = nullptr;
      deviceContext->OpenPropertyStore(STGM_READ, &propertyStore);
      PROPVARIANT propVariant;
      propertyStore->GetValue(PKEY_Device_FriendlyName, &propVariant);
      device.name = (const char*)utf8_t(propVariant.pwszVal);
      propertyStore->Release();

      if(device.id == defaultDevice.id) {
        self.devices.prepend(device);
      } else {
        self.devices.append(device);
      }
    }

    deviceCollection->Release();
    WASAPITrace("construct done devices=%u default=%s", (uint)self.devices.size(), self.devices ? (const char*)self.devices.first().name : "");
    return true;
  }

  auto destruct() -> void {
    WASAPITrace("destruct begin");
    terminate();

    if(self.enumerator) {
      self.enumerator->Release();
      self.enumerator = nullptr;
    }
    WASAPITrace("destruct done");
  }

  auto initialize() -> bool {
    WASAPITrace("initialize begin device=%s requestedFrequency=%u requestedLatency=%u", (const char*)self.device, self.frequency, self.latency);
    terminate();

    string deviceID;
    if(auto index = self.devices.find([&](auto& device) { return device.name == self.device; })) {
      deviceID = self.devices[*index].id;
    } else {
      WASAPITrace("initialize failed: device not found name=%s", (const char*)self.device);
      return false;
    }

    utf16_t deviceString(deviceID);
    HRESULT hr = self.enumerator->GetDevice(deviceString, &self.audioDevice);
    if(hr != S_OK) {
      WASAPITrace("initialize GetDevice failed hr=0x%08lx", (unsigned long)hr);
      return false;
    }

    hr = self.audioDevice->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr, (void**)&self.audioClient);
    if(hr != S_OK) {
      WASAPITrace("initialize Activate IAudioClient failed hr=0x%08lx", (unsigned long)hr);
      return false;
    }

    WAVEFORMATEX* waveFormat = nullptr;
    hr = self.audioClient->GetMixFormat(&waveFormat);
    if(hr != S_OK) {
      WASAPITrace("initialize GetMixFormat failed hr=0x%08lx", (unsigned long)hr);
      return false;
    }
    WASAPITrace("initialize mix format tag=%u channels=%u frequency=%lu bits=%u", waveFormat->wFormatTag, waveFormat->nChannels, (unsigned long)waveFormat->nSamplesPerSec, waveFormat->wBitsPerSample);
    hr = self.audioClient->GetDevicePeriod(&self.devicePeriod, nullptr);
    if(hr != S_OK) {
      WASAPITrace("initialize GetDevicePeriod failed hr=0x%08lx", (unsigned long)hr);
      CoTaskMemFree(waveFormat);
      return false;
    }
    auto latency = max(self.devicePeriod, (REFERENCE_TIME)self.latency * 10'000);  //1ms to 100ns units
    WASAPITrace("initialize shared-mode begin devicePeriod100ns=%lld latency100ns=%lld", (long long)self.devicePeriod, (long long)latency);
    hr = self.audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, latency, 0, waveFormat, nullptr);
    if(hr != S_OK) {
      WASAPITrace("initialize shared-mode failed hr=0x%08lx", (unsigned long)hr);
      CoTaskMemFree(waveFormat);
      return false;
    }
    WASAPITrace("initialize shared-mode ok");

    self.eventHandle = CreateEvent(nullptr, false, false, nullptr);
    if(!self.eventHandle) {
      WASAPITrace("initialize CreateEvent failed gle=%lu", (unsigned long)GetLastError());
      CoTaskMemFree(waveFormat);
      return false;
    }
    hr = self.audioClient->SetEventHandle(self.eventHandle);
    if(hr != S_OK) {
      WASAPITrace("initialize SetEventHandle failed hr=0x%08lx", (unsigned long)hr);
      CoTaskMemFree(waveFormat);
      return false;
    }
    hr = self.audioClient->GetService(IID_IAudioRenderClient, (void**)&self.renderClient);
    if(hr != S_OK) {
      WASAPITrace("initialize GetService render failed hr=0x%08lx", (unsigned long)hr);
      CoTaskMemFree(waveFormat);
      return false;
    }
    hr = self.audioClient->GetBufferSize(&self.bufferSize);
    if(hr != S_OK) {
      WASAPITrace("initialize GetBufferSize failed hr=0x%08lx", (unsigned long)hr);
      CoTaskMemFree(waveFormat);
      return false;
    }

    self.channels = waveFormat->nChannels;
    self.frequency = waveFormat->nSamplesPerSec;
    // Endpoint-dependent arithmetic belongs to device setup, not to each of
    // the tens of thousands of output samples processed every second.
    self.outputQuantum = (uint32_t)(((uint64_t)self.frequency * self.devicePeriod + 9'999'999) / 10'000'000);
    if(!self.outputQuantum) self.outputQuantum = self.bufferSize / 8;
    self.outputQuantum = max(16u, self.outputQuantum);
    if(self.bufferSize) self.outputQuantum = min(self.outputQuantum, self.bufferSize);
    self.outputQueueLimit = max(64u, self.outputQuantum * 2);
    if(self.bufferSize) self.outputQueueLimit = min(self.outputQueueLimit, self.bufferSize);
    self.outputQueueLimit = min(self.outputQueueLimit, 65535u);
    self.mode = waveFormat->wFormatTag;
    if(waveFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE && waveFormat->cbSize >= 22) {
      auto extensible = (WAVEFORMATEXTENSIBLE*)waveFormat;
      self.mode = extensible->SubFormat.Data1;
    }
    self.precision = waveFormat->wBitsPerSample;
    CoTaskMemFree(waveFormat);

    resetQueue();
    hr = self.audioClient->Start();
    if(hr != S_OK) {
      WASAPITrace("initialize Start failed hr=0x%08lx", (unsigned long)hr);
      return false;
    }
    self.isReady = true;
    WASAPITrace(
      "initialize done ready=%d channels=%u frequency=%u mode=%u precision=%u bufferSize=%lu devicePeriod100ns=%lld shared=1",
      self.isReady,
      self.channels,
      self.frequency,
      self.mode,
      self.precision,
      (unsigned long)self.bufferSize,
      (long long)self.devicePeriod
    );
    return true;
  }

  auto terminate() -> void {
    if(self.isReady || self.audioClient || self.renderClient || self.eventHandle) {
      WASAPITrace("terminate begin ready=%d audioClient=%p renderClient=%p event=%p", self.isReady, (void*)self.audioClient, (void*)self.renderClient, (void*)self.eventHandle);
    }
    self.isReady = false;
    if(self.audioClient) self.audioClient->Stop();
    if(self.renderClient) self.renderClient->Release(), self.renderClient = nullptr;
    if(self.audioClient) self.audioClient->Release(), self.audioClient = nullptr;
    if(self.audioDevice) self.audioDevice->Release(), self.audioDevice = nullptr;
    if(self.eventHandle) CloseHandle(self.eventHandle), self.eventHandle = nullptr;
    if(self.taskHandle) AvRevertMmThreadCharacteristics(self.taskHandle), self.taskHandle = nullptr;
    WASAPITrace("terminate done");
  }

  auto write() -> bool {
    if(!self.audioClient || !self.renderClient) return false;
    static uint writeTraceCount = 0;
    bool traceWrite = writeTraceCount++ < 80;
    if(traceWrite) WASAPITrace("write begin queue=%u bufferSize=%lu exclusive=%d", self.queue.count, (unsigned long)self.bufferSize, self.exclusive);

    uint32_t available = self.bufferSize;
    if(!self.exclusive) {
      uint32_t padding = 0;
      HRESULT hr = self.audioClient->GetCurrentPadding(&padding);
      if(hr != S_OK) {
        WASAPITrace("write GetCurrentPadding failed hr=0x%08lx", (unsigned long)hr);
        recoverFromDeviceInterrupt(hr);
        return false;
      }
      if(padding > self.bufferSize) padding = self.bufferSize;
      available = self.bufferSize - padding;
    }
    uint32_t length = min(available, self.queue.count);
    if(length == 0) return true;

    uint8_t* buffer = nullptr;
    HRESULT hr = self.renderClient->GetBuffer(length, &buffer);
    if(hr != S_OK) {
      WASAPITrace("write GetBuffer failed hr=0x%08lx length=%lu", (unsigned long)hr, (unsigned long)length);
      recoverFromDeviceInterrupt(hr);
      return false;
    }

    uint bufferFlags = 0;
    for(uint _ : range(length)) {
      double samples[8] = {};
      if(self.queue.count) {
        for(uint n : range(self.channels)) {
          samples[n] = self.queue.samples[self.queue.read][n];
        }
        self.queue.read++;
        self.queue.count--;
      }

      if(self.mode == 1 && self.precision == 16) {
        auto output = (uint16_t*)buffer;
        for(uint n : range(self.channels)) *output++ = (uint16_t)sclamp<16>(samples[n] * (32768.0 - 1.0));
        buffer = (uint8_t*)output;
      } else if(self.mode == 1 && self.precision == 32) {
        auto output = (uint32_t*)buffer;
        for(uint n : range(self.channels)) *output++ = (uint32_t)sclamp<32>(samples[n] * (65536.0 * 32768.0 - 1.0));
        buffer = (uint8_t*)output;
      } else if(self.mode == 3 && self.precision == 32) {
        auto output = (float*)buffer;
        for(uint n : range(self.channels)) *output++ = float(max(-1.0, min(+1.0, samples[n])));
        buffer = (uint8_t*)output;
      } else {
        //output silence for unsupported sample formats
        bufferFlags = AUDCLNT_BUFFERFLAGS_SILENT;
        break;
      }
    }

    hr = self.renderClient->ReleaseBuffer(length, bufferFlags);
    if(hr != S_OK) {
      WASAPITrace("write ReleaseBuffer failed hr=0x%08lx", (unsigned long)hr);
      recoverFromDeviceInterrupt(hr);
      return false;
    }

    if(traceWrite) WASAPITrace("write ReleaseBuffer length=%lu flags=%u remainingQueue=%u", (unsigned long)length, bufferFlags, self.queue.count);
    return true;
  }

  bool isReady = false;

  uint mode = 0;
  uint precision = 0;

  struct Queue {
    double samples[65536][8];
    uint16_t read;
    uint16_t write;
    uint16_t count;
  } queue;

  IMMDeviceEnumerator* enumerator = nullptr;
  IMMDevice* audioDevice = nullptr;
  IAudioClient* audioClient = nullptr;
  IAudioRenderClient* renderClient = nullptr;
  HANDLE eventHandle = nullptr;
  HANDLE taskHandle = nullptr;
  REFERENCE_TIME devicePeriod = 0;
  uint32_t bufferSize = 0;  //in frames
  uint32_t outputQuantum = 16;
  uint32_t outputQueueLimit = 64;
  bool recovering = false;
};
