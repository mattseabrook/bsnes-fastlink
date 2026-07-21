#include <dsound.h>

struct AudioDirectSound : AudioDriver {
  AudioDirectSound& self = *this;
  AudioDirectSound(Audio& super) : AudioDriver(super) {}
  ~AudioDirectSound() { terminate(); }

  auto create() -> bool override {
    super.setChannels(2);
    super.setFrequency(48000);
    super.setLatency(40);
    return initialize();
  }

  auto driver() -> string override { return "DirectSound 7.0"; }
  auto ready() -> bool override { return _ready; }

  auto hasBlocking() -> bool override { return true; }

  auto hasFrequencies() -> vector<uint> override {
    return {44100, 48000, 96000};
  }

  auto hasLatencies() -> vector<uint> override {
    return {40, 60, 80, 100};
  }

  auto setBlocking(bool blocking) -> bool override { return true; }
  auto setFrequency(uint frequency) -> bool override { return initialize(); }
  auto setLatency(uint latency) -> bool override { return initialize(); }

  auto clear() -> void override {
    if(!ready()) return;

    _ringRead = 0;
    _ringWrite = _rings - 2;
    _ringDistance = _rings - 2;

    if(_buffer) memory::fill<uint32_t>(_buffer, _period * _rings);
    _offset = 0;

    if(!_secondary) return;
    _secondary->Stop();
    _secondary->SetCurrentPosition(0);

    void* output;
    DWORD size;
    auto result = _secondary->Lock(0, _period * _rings * 4, &output, &size, 0, 0, 0);
    if(result == DSERR_BUFFERLOST) {
      _secondary->Restore();
      result = _secondary->Lock(0, _period * _rings * 4, &output, &size, 0, 0, 0);
    }
    if(result == DS_OK) {
      memory::fill<uint8_t>(output, size);
      _secondary->Unlock(output, size, 0, 0);
    }

    _secondary->Play(0, 0, DSBPLAY_LOOPING);
  }

  auto output(const double samples[]) -> void override {
    if(!ready()) return;

    _buffer[_offset]  = (uint16_t)sclamp<16>(samples[0] * 32767.0) <<  0;
    _buffer[_offset] |= (uint16_t)sclamp<16>(samples[1] * 32767.0) << 16;
    if(++_offset < _period) return;
    _offset = 0;

    if(!ensureWritableRing()) return;

    _ringWrite = (_ringWrite + 1) % _rings;
    _ringDistance++;

    void* output;
    DWORD size;
    auto result = _secondary->Lock(_ringWrite * _period * 4, _period * 4, &output, &size, 0, 0, 0);
    if(result == DSERR_BUFFERLOST) {
      _secondary->Restore();
      result = _secondary->Lock(_ringWrite * _period * 4, _period * 4, &output, &size, 0, 0, 0);
    }
    if(result == DS_OK) {
      memory::copy<uint32_t>(output, _buffer, _period);
      _secondary->Unlock(output, size, 0, 0);
    }
  }

private:
  auto synchronizePlaybackRing() -> void {
    if(!_secondary || !_period || !_rings) return;

    DWORD position = 0;
    if(_secondary->GetCurrentPosition(&position, 0) != DS_OK) return;

    uint ringActive = position / (_period * 4);
    if(ringActive == _ringRead) return;

    uint ringsPlayed = (_rings + ringActive - _ringRead) % _rings;
    if(ringsPlayed > (uint)_ringDistance) {
      _ringDistance = 0;
    } else {
      _ringDistance -= ringsPlayed;
    }
    _ringRead = ringActive;

    if(_ringDistance < 2) {
      _ringDistance = _rings - 2;
      _ringWrite = (_rings + _ringRead - 2) % _rings;
    }
  }

  auto ensureWritableRing() -> bool {
    synchronizePlaybackRing();
    if(_ringDistance < (int)_rings - 1) return true;
    if(!self.blocking) return false;

    while(_ringDistance >= (int)_rings - 1) {
      Sleep(1);
      synchronizePlaybackRing();
    }
    return true;
  }

  auto initialize() -> bool {
    terminate();

    _rings = 8;
    _period = self.frequency * self.latency / _rings / 1000.0 + 0.5;
    _buffer = new uint32_t[_period * _rings];
    _offset = 0;

    if(DirectSoundCreate(0, &_interface, 0) != DS_OK) return terminate(), false;
    if(_interface->SetCooperativeLevel(GetDesktopWindow(), DSSCL_PRIORITY) != DS_OK) return terminate(), false;

    DSBUFFERDESC primaryDescription = {};
    primaryDescription.dwSize = sizeof(DSBUFFERDESC);
    primaryDescription.dwFlags = DSBCAPS_PRIMARYBUFFER;
    primaryDescription.dwBufferBytes = 0;
    primaryDescription.lpwfxFormat = 0;
    if(_interface->CreateSoundBuffer(&primaryDescription, &_primary, 0) != DS_OK) return terminate(), false;

    WAVEFORMATEX waveFormat = {};
    waveFormat.wFormatTag = WAVE_FORMAT_PCM;
    waveFormat.nChannels = self.channels;
    waveFormat.nSamplesPerSec = self.frequency;
    waveFormat.wBitsPerSample = 16;
    waveFormat.nBlockAlign = waveFormat.nChannels * waveFormat.wBitsPerSample / 8;
    waveFormat.nAvgBytesPerSec = waveFormat.nSamplesPerSec * waveFormat.nBlockAlign;
    if(_primary->SetFormat(&waveFormat) != DS_OK) return terminate(), false;

    DSBUFFERDESC secondaryDescription = {};
    secondaryDescription.dwSize = sizeof(DSBUFFERDESC);
    secondaryDescription.dwFlags = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_CTRLFREQUENCY | DSBCAPS_GLOBALFOCUS | DSBCAPS_LOCSOFTWARE;
    secondaryDescription.dwBufferBytes = _period * _rings * 4;
    secondaryDescription.guid3DAlgorithm = GUID_NULL;
    secondaryDescription.lpwfxFormat = &waveFormat;
    if(_interface->CreateSoundBuffer(&secondaryDescription, &_secondary, 0) != DS_OK) return terminate(), false;
    if(_secondary->SetFrequency(self.frequency) != DS_OK) return terminate(), false;
    if(_secondary->SetCurrentPosition(0) != DS_OK) return terminate(), false;

    _ready = true;
    clear();
    return true;
  }

  auto terminate() -> void {
    _ready = false;
    if(_buffer) { delete[] _buffer; _buffer = nullptr; }
    if(_secondary) { _secondary->Stop(); _secondary->Release(); _secondary = nullptr; }
    if(_primary) { _primary->Stop(); _primary->Release(); _primary = nullptr; }
    if(_interface) { _interface->Release(); _interface = nullptr; }
  }

  bool _ready = false;

  LPDIRECTSOUND _interface = nullptr;
  LPDIRECTSOUNDBUFFER _primary = nullptr;
  LPDIRECTSOUNDBUFFER _secondary = nullptr;

  uint32_t* _buffer = nullptr;
  uint _offset = 0;

  uint _period = 0;
  uint _rings = 0;
  uint _ringRead = 0;
  uint _ringWrite = 0;
  int _ringDistance = 0;
};
