auto Program::updateAudioDriver(Window parent) -> void {
  fastLinkDebug(
    "Program::updateAudioDriver begin requested=%s exclusive=%d blocking=%d latency=%u frequency=%u",
    (const char*)settings.audio.driver,
    settings.audio.exclusive,
    settings.audio.blocking,
    settings.audio.latency,
    settings.audio.frequency
  );
  auto changed = (bool)audio;
  audio.create(settings.audio.driver);
  fastLinkDebug("Program::updateAudioDriver create done active=%s ready=%d changed=%d", (const char*)audio.driver(), audio.ready(), changed);
  audio.setContext(presentation.viewport.handle());
  audio.setChannels(2);
  if(changed) {
    settings.audio.device = audio.device();
    settings.audio.frequency = audio.frequency();
    settings.audio.latency = audio.latency();
  }
  fastLinkDebug("Program::updateAudioDriver update exclusive");
  updateAudioExclusive();
  fastLinkDebug("Program::updateAudioDriver update device");
  updateAudioDevice();
  fastLinkDebug("Program::updateAudioDriver update blocking");
  updateAudioBlocking();
  fastLinkDebug("Program::updateAudioDriver update dynamic");
  updateAudioDynamic();

  if(!audio.ready()) {
    fastLinkDebug("Program::updateAudioDriver failed active=%s; falling back to None", (const char*)audio.driver());
    MessageDialog({
      "Error: failed to initialize [", settings.audio.driver, "] audio driver."
    }).setAlignment(parent).error();
    settings.audio.driver = "None";
    return updateAudioDriver(parent);
  }
  fastLinkDebug(
    "Program::updateAudioDriver complete active=%s ready=%d device=%s frequency=%u latency=%u exclusive=%d blocking=%d",
    (const char*)audio.driver(),
    audio.ready(),
    (const char*)settings.audio.device,
    settings.audio.frequency,
    settings.audio.latency,
    settings.audio.exclusive,
    settings.audio.blocking
  );
}

auto Program::updateAudioExclusive() -> void {
  fastLinkDebug("Program::updateAudioExclusive exclusive=%d", settings.audio.exclusive);
  audio.setExclusive(settings.audio.exclusive);
  updateAudioFrequency();
  updateAudioLatency();
}

auto Program::updateAudioDevice() -> void {
  fastLinkDebug("Program::updateAudioDevice begin device=%s", (const char*)settings.audio.device);
  audio.clear();
  if(!audio.hasDevice(settings.audio.device)) {
    settings.audio.device = audio.device();
  }
  audio.setDevice(settings.audio.device);
  updateAudioFrequency();
  updateAudioLatency();
  fastLinkDebug("Program::updateAudioDevice done device=%s", (const char*)settings.audio.device);
}

auto Program::updateAudioBlocking() -> void {
  fastLinkDebug("Program::updateAudioBlocking blocking=%d", settings.audio.blocking);
  audio.clear();
  audio.setBlocking(settings.audio.blocking);
  presentation.speedMenu.setEnabled(!settings.video.blocking && settings.audio.blocking);
}

auto Program::updateAudioDynamic() -> void {
  audio.setDynamic(settings.audio.dynamic);
}

auto Program::updateAudioFrequency() -> void {
  fastLinkDebug("Program::updateAudioFrequency begin frequency=%u skew=%d", settings.audio.frequency, settings.audio.skew);
  audio.clear();
  if(!audio.hasFrequency(settings.audio.frequency)) {
    settings.audio.frequency = audio.frequency();
  }
  audio.setFrequency(settings.audio.frequency);
  double frequency = settings.audio.frequency + settings.audio.skew;
  if(!settings.video.blocking && settings.audio.blocking) {
    for(auto item : presentation.speedGroup.objects<MenuRadioItem>()) {
      if(item.checked()) frequency *= item.attribute("multiplier").real();
    }
  }
  Emulator::audio.setFrequency(frequency);
  fastLinkDebug("Program::updateAudioFrequency done effective=%f", frequency);
}

auto Program::updateAudioLatency() -> void {
  fastLinkDebug("Program::updateAudioLatency begin latency=%u", settings.audio.latency);
  audio.clear();
  if(!audio.hasLatency(settings.audio.latency)) {
    settings.audio.latency = audio.latency();
  }
  audio.setLatency(settings.audio.latency);
  fastLinkDebug("Program::updateAudioLatency done latency=%u", settings.audio.latency);
}

auto Program::updateAudioEffects() -> void {
  double volume = settings.audio.volume * 0.01;
  Emulator::audio.setVolume(volume);

  double balance = max(-1.0, min(+1.0, ((int)settings.audio.balance - 50) / 50.0));
  Emulator::audio.setBalance(balance);
}
