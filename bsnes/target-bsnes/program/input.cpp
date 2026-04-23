auto Program::updateInputDriver(Window parent) -> void {
  #if defined(PLATFORM_WINDOWS)
  settings.input.driver = NativeInputDriverName;
  if(nativeInputManager.initialize()) return;

  MessageDialog({
    "Error: failed to initialize [", NativeInputDriverName, "] input backend."
  }).setAlignment(parent).error();
  #else
  auto workerRunning = inputManager._pollRunning.load();
  if(workerRunning) inputManager.stopWorker();

  auto changed = (bool)input;
  input.create(settings.input.driver);
  input.setContext(presentation.viewport.handle());
  if(changed) {
  }

  inputManager.initialize();
  inputSettings.reloadPorts();
  hotkeySettings.reloadMappings();

  if(!input.ready()) {
    MessageDialog({
      "Error: failed to initialize [", settings.input.driver, "] input driver."
    }).setAlignment(parent).error();
    settings.input.driver = "None";
    updateInputDriver(parent);
    if(workerRunning && !inputManager._pollRunning.load()) inputManager.startWorker();
    return;
  }

  if(workerRunning && !inputManager._pollRunning.load()) inputManager.startWorker();
  #endif
}
