auto EmulatorSettings::create() -> void {
  setCollapsible();
  setVisible(false);

  optionsLabel.setText("Options").setFont(Font().setBold());
  warnOnUnverifiedGames.setText("Warn when loading games that have not been verified").setChecked(settings.emulator.warnOnUnverifiedGames).onToggle([&] {
    settings.emulator.warnOnUnverifiedGames = warnOnUnverifiedGames.checked();
  });
  autoSaveMemory.setText("Auto-save memory periodically").setChecked(settings.emulator.autoSaveMemory.enable).onToggle([&] {
    settings.emulator.autoSaveMemory.enable = autoSaveMemory.checked();
  });
  autoSaveStateOnUnload.setText("Auto-save undo state when unloading games").setChecked(settings.emulator.autoSaveStateOnUnload).onToggle([&] {
    settings.emulator.autoSaveStateOnUnload = autoSaveStateOnUnload.checked();
    if(!autoSaveStateOnUnload.checked()) {
      autoLoadStateOnLoad.setEnabled(false).setChecked(false).doToggle();
    } else {
      autoLoadStateOnLoad.setEnabled(true);
    }
  }).doToggle();
  autoLoadStateOnLoad.setText("Auto-resume on load").setChecked(settings.emulator.autoLoadStateOnLoad).onToggle([&] {
    settings.emulator.autoLoadStateOnLoad = autoLoadStateOnLoad.checked();
  });
  nativeFileDialogs.setText("Use native file dialogs").setChecked(settings.general.nativeFileDialogs).onToggle([&] {
    settings.general.nativeFileDialogs = nativeFileDialogs.checked();
  });
  #if defined(PLATFORM_WINDOWS)
  nativeFileDialogs.setChecked(true).setEnabled(false).setToolTip("Windows builds always use the native Win32 file dialogs.");
  #endif
  screenSaver.setText("Allow screensaver during emulation").setChecked(settings.general.screenSaver).onToggle([&] {
    settings.general.screenSaver = screenSaver.checked();
    // setting can be toggled while emulation is active
    if(!program.inactive()) Application::setScreenSaver(settings.general.screenSaver);
  });
  optionsSpacer.setColor({192, 192, 192});

  fastForwardLabel.setText("Fast Forward").setFont(Font().setBold());

  limiterLabel.setText("Limiter:").setToolTip({
    "Set the maximum speed when fast forwarding."
  });

  limiterAmount.append(ComboButtonItem().setText("None"));
  limiterAmount.append(ComboButtonItem().setText("200%"));
  limiterAmount.append(ComboButtonItem().setText("300%"));
  limiterAmount.append(ComboButtonItem().setText("400%"));
  limiterAmount.append(ComboButtonItem().setText("500%"));
  limiterAmount.append(ComboButtonItem().setText("600%"));
  limiterAmount.append(ComboButtonItem().setText("700%"));
  limiterAmount.append(ComboButtonItem().setText("800%"));
  if(settings.fastForward.limiter == 0) limiterAmount.item(0).setSelected();
  if(settings.fastForward.limiter == 2) limiterAmount.item(1).setSelected();
  if(settings.fastForward.limiter == 3) limiterAmount.item(2).setSelected();
  if(settings.fastForward.limiter == 4) limiterAmount.item(3).setSelected();
  if(settings.fastForward.limiter == 5) limiterAmount.item(4).setSelected();
  if(settings.fastForward.limiter == 6) limiterAmount.item(5).setSelected();
  if(settings.fastForward.limiter == 7) limiterAmount.item(6).setSelected();
  if(settings.fastForward.limiter == 8) limiterAmount.item(7).setSelected();
  limiterAmount.onChange([&] {
    auto index = limiterAmount.selected().offset();
    if(index == 0) settings.fastForward.limiter = 0;
    if(index == 1) settings.fastForward.limiter = 2;
    if(index == 2) settings.fastForward.limiter = 3;
    if(index == 3) settings.fastForward.limiter = 4;
    if(index == 4) settings.fastForward.limiter = 5;
    if(index == 5) settings.fastForward.limiter = 6;
    if(index == 6) settings.fastForward.limiter = 7;
    if(index == 7) settings.fastForward.limiter = 8;
  });

  fastForwardMute.setText("Mute while fast forwarding").setChecked(settings.fastForward.mute).onToggle([&] {
    settings.fastForward.mute = fastForwardMute.checked();
  });

  fastForwardSpacer.setColor({192, 192, 192});
}
