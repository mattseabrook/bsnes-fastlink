#if defined(AUDIO_ALSA)
  #include <ruby/audio/alsa.cpp>
#endif

#if defined(AUDIO_AO)
  #include <ruby/audio/ao.cpp>
#endif

#if defined(AUDIO_ASIO)
  #include <ruby/audio/asio.cpp>
#endif

#if defined(AUDIO_DIRECTSOUND)
  #include <ruby/audio/directsound.cpp>
#endif

#if defined(AUDIO_OPENAL)
  #include <ruby/audio/openal.cpp>
#endif

#if defined(AUDIO_OSS)
  #include <ruby/audio/oss.cpp>
#endif

#if defined(AUDIO_PULSEAUDIO)
  #include <ruby/audio/pulseaudio.cpp>
#endif

#if defined(AUDIO_PULSEAUDIOSIMPLE)
  #include <ruby/audio/pulseaudio-simple.cpp>
#endif

#if defined(AUDIO_WASAPI)
  #include <ruby/audio/wasapi.cpp>
#endif

#if defined(AUDIO_WAVEOUT)
  #include <ruby/audio/waveout.cpp>
#endif

#if defined(AUDIO_XAUDIO2)
  #include <ruby/audio/xaudio2.cpp>
#endif

#if defined(PLATFORM_WINDOWS)
  #include <avrt.h>
#endif

namespace ruby {

auto Audio::setExclusive(bool exclusive) -> bool {
  if(instance->exclusive == exclusive) return true;
  if(!instance->hasExclusive()) return false;
  if(!instance->setExclusive(instance->exclusive = exclusive)) return false;
  return true;
}

auto Audio::setContext(uintptr context) -> bool {
  if(instance->context == context) return true;
  if(!instance->hasContext()) return false;
  if(!instance->setContext(instance->context = context)) return false;
  return true;
}

auto Audio::setDevice(string device) -> bool {
  if(instance->device == device) return true;
  if(!instance->hasDevice(device)) return false;
  if(!instance->setDevice(instance->device = device)) return false;
  return true;
}

auto Audio::setBlocking(bool blocking) -> bool {
  if(instance->blocking == blocking) return true;
  if(!instance->hasBlocking()) return false;
  if(!instance->setBlocking(instance->blocking = blocking)) return false;
  for(auto& resampler : resamplers) resampler.reset(instance->frequency);
  return true;
}

auto Audio::setDynamic(bool dynamic) -> bool {
  if(instance->dynamic == dynamic) return true;
  if(!instance->hasDynamic()) return false;
  if(!instance->setDynamic(instance->dynamic = dynamic)) return false;
  return true;
}

auto Audio::setChannels(uint channels) -> bool {
  if(resamplers.size() != channels) {
    resamplers.reset();
    resamplers.resize(channels);
    for(auto& resampler : resamplers) resampler.reset(instance->frequency);
  }
  if(instance->channels == channels) return true;
  if(!instance->hasChannels(channels)) return false;
  if(!instance->setChannels(instance->channels = channels)) return false;
  return true;
}

auto Audio::setFrequency(uint frequency) -> bool {
  if(instance->frequency == frequency) return true;
  if(!instance->hasFrequency(frequency)) return false;
  if(!instance->setFrequency(instance->frequency = frequency)) return false;
  for(auto& resampler : resamplers) resampler.reset(instance->frequency);
  return true;
}

auto Audio::setLatency(uint latency) -> bool {
  if(instance->latency == latency) return true;
  if(!instance->hasLatency(latency)) return false;
  if(!instance->setLatency(instance->latency = latency)) return false;
  return true;
}

//

auto Audio::clear() -> void {
  _smoothedMultiplier = 1.0;
  for(auto& resampler : resamplers) resampler.reset(instance->frequency);
  return instance->clear();
}

auto Audio::level() -> double {
  return instance->level();
}

auto Audio::output(const double samples[]) -> void {
  bool useWorker = _workerRunning && !instance->blocking;

  if(!instance->dynamic) {
    //fastlink: push to ring buffer for worker thread
    if(useWorker) {
      uint channels = instance->channels;
      uint write = _ringWrite.load(::std::memory_order_relaxed);
      uint next = (write + 1) % _ringCapacity;
      if(next != _ringRead.load(::std::memory_order_acquire)) {
        for(uint n = 0; n < channels && n < 2; n++) {
          _ringBuffer[write * 2 + n] = samples[n];
        }
        _ringWrite.store(next, ::std::memory_order_release);
        _workerCV.notify_one();
      }
      //if ring full, drop sample (non-blocking)
      return;
    }
    return instance->output(samples);
  }

  //fastlink: EMA-bounded dynamic rate control
  //alpha controls how fast the multiplier converges (lower = smoother)
  //maxStep caps the per-sample change to prevent audible pitch shifts
  constexpr double alpha = 0.16;
  constexpr double maxStep = 0.0035;

  double fillLevel = instance->level();
  double targetMultiplier = (1.0 - 0.005) + 2.0 * fillLevel * 0.005;

  //smooth the multiplier with EMA + clamped step
  double delta = alpha * (targetMultiplier - _smoothedMultiplier);
  if(delta > maxStep) delta = maxStep;
  if(delta < -maxStep) delta = -maxStep;
  _smoothedMultiplier += delta;

  double dynamicFrequency = _smoothedMultiplier * instance->frequency;
  for(auto& resampler : resamplers) {
    resampler.setInputFrequency(dynamicFrequency);
    resampler.write(*samples++);
  }

  while(resamplers.first().pending()) {
    double samples[instance->channels];
    for(uint n : range(instance->channels)) samples[n] = resamplers[n].read();
    //fastlink: push resampled output to ring buffer for worker thread
    if(useWorker) {
      uint write = _ringWrite.load(::std::memory_order_relaxed);
      uint next = (write + 1) % _ringCapacity;
      if(next != _ringRead.load(::std::memory_order_acquire)) {
        for(uint n = 0; n < instance->channels && n < 2; n++) {
          _ringBuffer[write * 2 + n] = samples[n];
        }
        _ringWrite.store(next, ::std::memory_order_release);
        _workerCV.notify_one();
      }
    } else {
      instance->output(samples);
    }
  }
}

auto Audio::startWorker() -> void {
  if(_workerRunning) return;
  _ringRead = 0;
  _ringWrite = 0;
  _workerRunning = true;
  _workerThread = ::std::thread([this] { _workerMain(); });
}

auto Audio::stopWorker() -> void {
  if(!_workerRunning) return;
  _workerRunning = false;
  _workerCV.notify_one();
  if(_workerThread.joinable()) _workerThread.join();
}

auto Audio::_workerMain() -> void {
  #if defined(PLATFORM_WINDOWS)
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  SetThreadPriorityBoost(GetCurrentThread(), FALSE);
  DWORD taskIndex = 0;
  HANDLE taskHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
  if(taskHandle) AvSetMmThreadPriority(taskHandle, AVRT_PRIORITY_HIGH);
  #endif

  while(_workerRunning) {
    uint read = _ringRead.load(::std::memory_order_relaxed);
    uint write = _ringWrite.load(::std::memory_order_acquire);

    if(read == write) {
      //ring empty, wait for signal
      ::std::unique_lock<::std::mutex> lock(_workerMutex);
      _workerCV.wait_for(lock, ::std::chrono::milliseconds(1), [&] {
        return _ringRead.load(::std::memory_order_relaxed) != _ringWrite.load(::std::memory_order_acquire)
            || !_workerRunning;
      });
      continue;
    }

    //drain available samples
    while(read != _ringWrite.load(::std::memory_order_acquire) && _workerRunning) {
      double samples[2];
      samples[0] = _ringBuffer[read * 2 + 0];
      samples[1] = _ringBuffer[read * 2 + 1];
      uint next = (read + 1) % _ringCapacity;
      _ringRead.store(next, ::std::memory_order_release);
      read = next;

      instance->output(samples);
    }
  }

  #if defined(PLATFORM_WINDOWS)
  if(taskHandle) AvRevertMmThreadCharacteristics(taskHandle);
  #endif
}

//

auto Audio::create(string driver) -> bool {
  self.stopWorker();
  self.instance.reset();
  if(!driver) driver = optimalDriver();

  #if defined(AUDIO_ALSA)
  if(driver == "ALSA") self.instance = new AudioALSA(*this);
  #endif

  #if defined(AUDIO_AO)
  if(driver == "libao") self.instance = new AudioAO(*this);
  #endif

  #if defined(AUDIO_ASIO)
  if(driver == "ASIO") self.instance = new AudioASIO(*this);
  #endif

  #if defined(AUDIO_DIRECTSOUND)
  if(driver == "DirectSound 7.0") self.instance = new AudioDirectSound(*this);
  #endif

  #if defined(AUDIO_OPENAL)
  if(driver == "OpenAL") self.instance = new AudioOpenAL(*this);
  #endif

  #if defined(AUDIO_OSS)
  if(driver == "OSS") self.instance = new AudioOSS(*this);
  #endif

  #if defined(AUDIO_PULSEAUDIO)
  if(driver == "PulseAudio") self.instance = new AudioPulseAudio(*this);
  #endif

  #if defined(AUDIO_PULSEAUDIOSIMPLE)
  if(driver == "PulseAudio Simple") self.instance = new AudioPulseAudioSimple(*this);
  #endif

  #if defined(AUDIO_WASAPI)
  if(driver == "WASAPI") self.instance = new AudioWASAPI(*this);
  #endif

  #if defined(AUDIO_WAVEOUT)
  if(driver == "waveOut") self.instance = new AudioWaveOut(*this);
  #endif

  #if defined(AUDIO_XAUDIO2)
  if(driver == "XAudio 2.1") self.instance = new AudioXAudio2(*this);
  #endif

  if(!self.instance) self.instance = new AudioDriver(*this);

  bool result = self.instance->create();
  if(result) self.startWorker();
  return result;
}

auto Audio::hasDrivers() -> vector<string> {
  return {

  #if defined(AUDIO_ASIO)
  "ASIO",
  #endif

  #if defined(AUDIO_WASAPI)
  "WASAPI",
  #endif

  #if defined(AUDIO_XAUDIO2)
  "XAudio 2.1",
  #endif

  #if defined(AUDIO_DIRECTSOUND)
  "DirectSound 7.0",
  #endif

  #if defined(AUDIO_WAVEOUT)
  "waveOut",
  #endif

  #if defined(AUDIO_ALSA)
  "ALSA",
  #endif

  #if defined(AUDIO_OSS)
  "OSS",
  #endif

  #if defined(AUDIO_OPENAL)
  "OpenAL",
  #endif

  #if defined(AUDIO_PULSEAUDIO)
  "PulseAudio",
  #endif

  #if defined(AUDIO_PULSEAUDIOSIMPLE)
  "PulseAudio Simple",
  #endif

  #if defined(AUDIO_AO)
  "libao",
  #endif

  "None"};
}

auto Audio::optimalDriver() -> string {
  #if defined(AUDIO_WASAPI)
  return "WASAPI";
  #elif defined(AUDIO_ASIO)
  return "ASIO";
  #elif defined(AUDIO_XAUDIO2)
  return "XAudio 2.1";
  #elif defined(AUDIO_DIRECTSOUND)
  return "DirectSound 7.0";
  #elif defined(AUDIO_WAVEOUT)
  return "waveOut";
  #elif defined(AUDIO_PULSEAUDIO)
  return "PulseAudio";
  #elif defined(AUDIO_PULSEAUDIOSIMPLE)
  return "PulseAudio Simple";
  #elif defined(AUDIO_OPENAL)
  return "OpenAL";
  #elif defined(AUDIO_AO)
  return "libao";
  #elif defined(AUDIO_ALSA)
  return "ALSA";
  #elif defined(AUDIO_OSS)
  return "OSS";
  #else
  return "None";
  #endif
}

auto Audio::safestDriver() -> string {
  #if defined(AUDIO_WAVEOUT)
  return "waveOut";
  #elif defined(AUDIO_DIRECTSOUND)
  return "DirectSound 7.0";
  #elif defined(AUDIO_WASAPI)
  return "WASAPI";
  #elif defined(AUDIO_XAUDIO2)
  return "XAudio 2.1";
  #elif defined(AUDIO_ALSA)
  return "ALSA";
  #elif defined(AUDIO_OSS)
  return "OSS";
  #elif defined(AUDIO_OPENAL)
  return "OpenAL";
  #elif defined(AUDIO_PULSEAUDIO)
  return "PulseAudio";
  #elif defined(AUDIO_PULSEAUDIOSIMPLE)
  return "PulseAudio Simple";
  #elif defined(AUDIO_AO)
  return "libao";
  #elif defined(AUDIO_ASIO)
  return "ASIO";
  #else
  return "None";
  #endif
}

}
