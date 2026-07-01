#include "../bsnes.hpp"
#include "hotkeys.cpp"
InputManager inputManager;

auto InputMapping::bind() -> void {
  for(auto& binding : bindings) binding = {};

  for(uint index : range(BindingLimit)) {
    auto& assignment = assignments[index];
    auto& binding = bindings[index];

    auto token = assignment.split("/");
    if(token.size() < 3) continue;  //skip invalid mappings

    uint64 id = token[0].natural();
    uint group = token[1].natural();
    uint input = token[2].natural();
    string qualifier = token(3, "None");

    for(auto& device : inputManager.devices) {
      if(id != device->id()) continue;
      if(group >= device->size()) continue;
      if(input >= device->group(group).size()) continue;

      binding.device = device;
      binding.group = group;
      binding.input = input;
      binding.value = device->group(group).input(input).valueAddress();
      binding.qualifier = Qualifier::None;
      if(qualifier == "Lo") binding.qualifier = Qualifier::Lo;
      if(qualifier == "Hi") binding.qualifier = Qualifier::Hi;
      if(qualifier == "Rumble") binding.qualifier = Qualifier::Rumble;
      break;
    }
  }

  string text;
  for(auto& assignment : assignments) text.append(assignment, ";");
  text.trimRight(";");
  settings[path].setValue(text);
}

auto InputMapping::bind(string mapping, uint binding) -> void {
  if(binding >= BindingLimit) return;
  assignments[binding] = mapping;
  bind();
}

auto InputMapping::bind(shared_pointer<HID::Device> device, uint group, uint input, int16 oldValue, int16 newValue, uint binding) -> bool {
  if(device->isNull() || (device->isKeyboard() && device->group(group).input(input).name() == "Escape")) {
    return unbind(binding), true;
  }

  string encoding = {"0x", hex(device->id()), "/", group, "/", input};

  if(isDigital()) {
    if((device->isKeyboard() && group == HID::Keyboard::GroupID::Button)
    || (device->isMouse() && group == HID::Mouse::GroupID::Button)
    || (device->isJoypad() && group == HID::Joypad::GroupID::Button)) {
      if(newValue) {
        return bind(encoding, binding), true;
      }
    }

    if((device->isJoypad() && group == HID::Joypad::GroupID::Axis)
    || (device->isJoypad() && group == HID::Joypad::GroupID::Hat)
    || (device->isJoypad() && group == HID::Joypad::GroupID::Trigger)) {
      if(newValue < -16384 && group != HID::Joypad::GroupID::Trigger) {  //triggers are always hi
        return bind({encoding, "/Lo"}, binding), true;
      }

      if(newValue > +16384) {
        return bind({encoding, "/Hi"}, binding), true;
      }
    }
  }

  if(isAnalog()) {
    if((device->isMouse() && group == HID::Mouse::GroupID::Axis)
    || (device->isJoypad() && group == HID::Joypad::GroupID::Axis)
    || (device->isJoypad() && group == HID::Joypad::GroupID::Hat)) {
      if(newValue < -16384 || newValue > +16384) {
        return bind(encoding, binding), true;
      }
    }
  }

  if(isRumble()) {
    if(device->isJoypad() && group == HID::Joypad::GroupID::Button) {
      if(newValue) {
        return bind({encoding, "/Rumble"}, binding), true;
      }
    }
  }

  return false;
}

auto InputMapping::unbind(uint binding) -> void {
  bindings[binding] = {};
  assignments[binding] = {};
  string text;
  for(auto& assignment : assignments) text.append(assignment, ";");
  text.trimRight(";");
  settings[path].setValue(text);
}

auto InputMapping::poll() -> int16 {
  if(turboID) {
    auto& mapping = inputManager.ports[portID].devices[deviceID].mappings[turboID()];
    auto result = mapping.poll();
    if(result) return inputManager.turboCounter >= inputManager.turboFrequency;
  }

  uint validBindings = 0;
  int16 result = 0;

  for(auto& binding : bindings) {
    if(!binding.device) continue;  //unbound
    validBindings++;

    auto& device = binding.device;
    auto& group = binding.group;
    auto& input = binding.input;
    auto& qualifier = binding.qualifier;
    auto value = binding.value ? *binding.value : device->group(group).input(input).value();

    if(isDigital()) {
      boolean output;

      if((device->isKeyboard() && group == HID::Keyboard::GroupID::Button)
      || (device->isMouse() && group == HID::Mouse::GroupID::Button)
      || (device->isJoypad() && group == HID::Joypad::GroupID::Button)) {
        output = value != 0;
      }
      if((device->isJoypad() && group == HID::Joypad::GroupID::Axis)
      || (device->isJoypad() && group == HID::Joypad::GroupID::Hat)
      || (device->isJoypad() && group == HID::Joypad::GroupID::Trigger)) {
        if(qualifier == Qualifier::Lo) output = value < -16384;
        if(qualifier == Qualifier::Hi) output = value > +16384;
      }

      if(logic() == Logic::AND && output == 0) return 0;
      if(logic() == Logic::OR  && output == 1) return 1;
    }

    if(isAnalog()) {
      //logic does not apply to analog inputs ... always combinatorial
      if(device->isMouse() && group == HID::Mouse::GroupID::Axis) result += value;
      if(device->isJoypad() && group == HID::Joypad::GroupID::Axis) result += value >> 8;
      if(device->isJoypad() && group == HID::Joypad::GroupID::Hat) result += value < 0 ? -1 : value > 0 ? +1 : 0;
    }
  }

  if(isDigital() && logic() == Logic::AND && validBindings > 0) return 1;
  return result;
}

auto InputMapping::rumble(bool enable) -> void {
  for(auto& binding : bindings) {
    if(!binding.device) continue;
    input.rumble(binding.device->id(), enable);
  }
}

//

auto InputMapping::Binding::icon() -> image {
  if(device && device->isKeyboard()) return Icon::Device::Keyboard;
  if(device && device->isMouse()) return Icon::Device::Mouse;
  if(device && device->isJoypad()) return Icon::Device::Joypad;
  return {};
}

auto InputMapping::Binding::name() -> string {
  //if ruby drivers cannot report accurate vendor/product IDs (eg SDL),
  //and the user closes the emulator, changes gamepads, and restarts it,
  //it is possible the wrong device will now be mapped to the input IDs.
  //that could potentially make the group/input IDs go out of bounds.
  if(!device) return {};
  if(group >= device->size()) return {};
  if(input >= device->group(group).size()) return {};

  if(device->isKeyboard()) {
    return device->group(group).input(input).name();
  }

  if(device->isMouse()) {
    return device->group(group).input(input).name();
  }

  if(device->isJoypad()) {
    string name{Hash::CRC16(string{device->id()}).digest().upcase()};
    name.append(" ", device->group(group).name());
    name.append(" ", device->group(group).input(input).name());
    if(qualifier == Qualifier::Lo) name.append(" Lo");
    if(qualifier == Qualifier::Hi) name.append(" Hi");
    if(qualifier == Qualifier::Rumble) name.append(" Rumble");
    return name;
  }

  return {};
}

//

auto InputManager::initialize() -> void {
  devices.reset();
  ports.reset();
  hotkeys.reset();
  _pendingDevices.reset();
  _devicesChanged = false;
  {
    ::std::lock_guard<::std::mutex> lock(_changeMutex);
    _pendingChanges.clear();
  }
  _mainThreadID = ::std::this_thread::get_id();

  input.onChange({&InputManager::onChange, this});

  lastPoll = 0;  //force a poll event immediately
  frequency = max(1u, settings.input.frequency);

  turboCounter = 0;
  turboFrequency = max(1, settings.input.turbo.frequency);

  auto information = emulator->information();
  auto ports = emulator->ports();
  for(uint portID : range(ports.size())) {
    auto& port = ports[portID];
    InputPort inputPort{port.id, port.name};
    auto devices = emulator->devices(port.id);
    for(uint deviceID : range(devices.size())) {
      auto& device = devices[deviceID];
      InputDevice inputDevice{device.id, device.name};
      auto inputs = emulator->inputs(device.id);
      for(uint inputID : range(inputs.size())) {
        auto& input = inputs[inputID];
        InputMapping inputMapping;
        inputMapping.portID = portID;
        inputMapping.deviceID = deviceID;
        inputMapping.inputID = inputID;
        inputMapping.name = input.name;
        inputMapping.type = input.type;
        inputMapping.path = string{information.name, "/", inputPort.name, "/", inputDevice.name, "/", inputMapping.name}.replace(" ", "");
        auto assignments = settings(inputMapping.path).text().split(";");
        for(uint index : range(BindingLimit)) inputMapping.assignments[index] = assignments(index);
        inputDevice.mappings.append(inputMapping);
      }
      for(uint inputID : range(inputs.size())) {
        auto& input = inputs[inputID];
        if(input.type != InputMapping::Type::Button && input.type != InputMapping::Type::Trigger) continue;
        uint turboID = inputDevice.mappings.size();
        InputMapping inputMapping;
        inputMapping.portID = portID;
        inputMapping.deviceID = deviceID;
        inputMapping.inputID = turboID;
        inputMapping.name = string{"Turbo ", input.name};
        inputMapping.type = input.type;
        inputMapping.path = string{information.name, "/", inputPort.name, "/", inputDevice.name, "/", inputMapping.name}.replace(" ", "");
        auto assignments = settings(inputMapping.path).text().split(";");
        for(uint index : range(BindingLimit)) inputMapping.assignments[index] = assignments(index);
        inputDevice.mappings.append(inputMapping);
        inputDevice.mappings[inputID].turboID = turboID;
      }
      inputPort.devices.append(inputDevice);
    }
    this->ports.append(inputPort);
  }

  bindHotkeys();
  rebuildMappingCache();
  poll();
}

auto InputManager::bind() -> void {
  for(auto& port : ports) {
    for(auto& device : port.devices) {
      for(auto& mapping : device.mappings) {
        mapping.bind();
      }
    }
  }

  for(auto& hotkey : hotkeys) {
    hotkey.bind();
  }
}

auto InputManager::poll() -> void {
  if(Application::modal()) return;

  //fastlink: when worker is running, devices are updated continuously
  //just check if device list changed and rebind if needed
  if(_pollRunning) {
    bool changed = false;
    {
      ::std::lock_guard<::std::mutex> lock(_deviceMutex);
      if(_devicesChanged.exchange(false)) {
        devices = _pendingDevices;
        changed = true;
      }
    }
    if(changed) bind();
    flushWorkerEvents();
    return;
  }

  //fallback: poll directly (worker not started yet)
  auto thisPoll = chrono::millisecond();
  if(thisPoll - lastPoll < frequency) return;
  lastPoll = thisPoll;

  auto devices = input.poll();
  bool changed = devices.size() != this->devices.size();
  if(!changed) {
    for(auto n : range(devices.size())) {
      changed = devices[n] != this->devices[n];
      if(changed) break;
    }
  }
  if(changed) {
    this->devices = devices;
    bind();
  }
}

auto InputManager::frame() -> void {
  if(++turboCounter >= turboFrequency * 2) turboCounter = 0;
}

auto InputManager::onChange(shared_pointer<HID::Device> device, uint group, uint input, int16_t oldValue, int16_t newValue) -> void {
  if(!settingsWindow.focused()) return;

  if(_pollRunning && ::std::this_thread::get_id() != _mainThreadID) {
    ::std::lock_guard<::std::mutex> lock(_changeMutex);
    _pendingChanges.push_back({device, group, input, oldValue, newValue});
    return;
  }

  inputSettings.inputEvent(device, group, input, oldValue, newValue);
  hotkeySettings.inputEvent(device, group, input, oldValue, newValue);
}

auto InputManager::flushWorkerEvents() -> void {
  ::std::vector<ChangeEvent> pendingChanges;
  {
    ::std::lock_guard<::std::mutex> lock(_changeMutex);
    if(_pendingChanges.empty()) return;
    pendingChanges.swap(_pendingChanges);
  }

  for(auto& event : pendingChanges) {
    onChange(event.device, event.group, event.input, event.oldValue, event.newValue);
  }
}

auto InputManager::mapping(uint port, uint device, uint input) -> maybe<InputMapping&> {
  if(!emulator) return nothing;
  for(auto& inputPort : ports) {
    if(inputPort.id != port) continue;
    for(auto& inputDevice : inputPort.devices) {
      if(inputDevice.id != device) continue;
      if(input >= inputDevice.mappings.size()) continue;
      return inputDevice.mappings[input];
    }
  }
  return nothing;
}

auto InputManager::cachedMapping(uint port, uint device, uint input) -> maybe<InputMapping&> {
  if(port < MappingCachePorts && device < MappingCacheDevices && input < MappingCacheInputs) {
    if(auto mapping = _mappingCache[port][device][input]) return *mapping;
  }
  return mapping(port, device, input);
}

auto InputManager::rebuildMappingCache() -> void {
  for(uint port : range(MappingCachePorts)) {
    for(uint device : range(MappingCacheDevices)) {
      for(uint input : range(MappingCacheInputs)) {
        _mappingCache[port][device][input] = nullptr;
      }
    }
  }

  for(auto& inputPort : ports) {
    if(inputPort.id >= MappingCachePorts) continue;
    for(auto& inputDevice : inputPort.devices) {
      if(inputDevice.id >= MappingCacheDevices) continue;
      for(uint inputID : range(inputDevice.mappings.size())) {
        if(inputID >= MappingCacheInputs) break;
        _mappingCache[inputPort.id][inputDevice.id][inputID] = &inputDevice.mappings[inputID];
      }
    }
  }
}

auto InputManager::findMouse() -> shared_pointer<HID::Device> {
  for(auto& device : devices) {
    if(device->isMouse()) return device;
  }
  return {};
}

auto InputManager::startWorker() -> void {
  if(_pollRunning) return;
  _pollRunning = true;
  _pollThread = ::std::thread([this] {
    auto knownDevices = this->devices;
    while(_pollRunning) {
      if(!Application::modal()) {
        auto polledDevices = input.poll();
        bool changed = polledDevices.size() != knownDevices.size();
        if(!changed) {
          for(auto n : range(polledDevices.size())) {
            changed = polledDevices[n] != knownDevices[n];
            if(changed) break;
          }
        }
        if(changed) {
          ::std::lock_guard<::std::mutex> lock(_deviceMutex);
          _pendingDevices = polledDevices;
          _devicesChanged = true;
          knownDevices = polledDevices;
        }
      }
      auto target = chrono::microsecond() + max(100u, settings.fastLink.inputPollMicroseconds);
      while(_pollRunning && chrono::microsecond() < target) {
        #if defined(PLATFORM_WINDOWS)
        YieldProcessor();
        #elif defined(__SSE2__)
        __builtin_ia32_pause();
        #else
        ::std::this_thread::yield();
        #endif
      }
    }
  });
}

auto InputManager::stopWorker() -> void {
  if(!_pollRunning) return;
  _pollRunning = false;
  if(_pollThread.joinable()) _pollThread.join();
  flushWorkerEvents();
}

#include "native-win32.cpp"
