#if defined(PLATFORM_WINDOWS)
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <hidsdi.h>
#include <avrt.h>

namespace {
constexpr LONG RawInputNeutralThreshold = 60;
constexpr uint MaximumRawButtons = 32;
constexpr uint16_t RetroUSBVendorID = 0x0f00;
constexpr uint16_t RetroUSBRetroPadProductID = 0x0003;
constexpr uint16_t RetroUSBSuperRetroPortProductID = 0x00f1;
enum : uint {
  GamepadUp,
  GamepadDown,
  GamepadLeft,
  GamepadRight,
  GamepadB,
  GamepadA,
  GamepadY,
  GamepadX,
  GamepadL,
  GamepadR,
  GamepadSelect,
  GamepadStart,
};

struct SnesButtonMap {
  int b;
  int a;
  int y;
  int x;
  int l;
  int r;
  int select;
  int start;
};

constexpr auto RetroUSBButtonMap = SnesButtonMap{1, 5, 0, 4, 6, 7, 2, 3};
constexpr auto StandardHIDButtonMap = SnesButtonMap{0, 1, 2, 3, 4, 5, 6, 7};

auto isRetroUSBController(uint16_t vendorID, uint16_t productID) -> bool {
  if(vendorID != RetroUSBVendorID) return false;
  return productID == RetroUSBRetroPadProductID || productID == RetroUSBSuperRetroPortProductID;
}

auto buttonPressed(const bool (&buttons)[MaximumRawButtons], int index) -> bool {
  return index >= 0 && index < (int)MaximumRawButtons ? buttons[index] : false;
}

auto applyNativeInputThreadAffinity() -> void {
  int processor = settings.fastLink.inputThreadAffinity;
  if(processor == -1) return;

  DWORD_PTR processMask = 0;
  DWORD_PTR systemMask = 0;
  if(!GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) || !processMask) return;

  int target = processor;
  if(target < 0) {
    target = 0;
    for(uint index : range((uint)(sizeof(DWORD_PTR) * 8))) {
      if(processMask & ((DWORD_PTR)1 << index)) target = index;
    }
  }

  if(target < 0 || target >= (int)(sizeof(DWORD_PTR) * 8)) return;

  DWORD_PTR mask = (DWORD_PTR)1 << target;
  if(!(processMask & mask)) return;
  SetThreadAffinityMask(GetCurrentThread(), mask);
  SetThreadIdealProcessor(GetCurrentThread(), target);
}
}

NativeInputManager nativeInputManager;

auto NativeInputManager::initialize() -> bool {
  terminate();

  {
    ::std::lock_guard<::std::mutex> lock(_stateMutex);
    _keyboardState.fill(0);
    _keyboardMask.store(0, ::std::memory_order_release);
    for(auto& mask : _padMasks) mask.store(0, ::std::memory_order_release);
    _pads.clear();
    _ignoredDevices.clear();
  }

  _ready = false;
  _readySignal = false;
  _running = true;
  _thread = ::std::thread([this] { threadMain(); });

  ::std::unique_lock<::std::mutex> lock(_readyMutex);
  _readyCondition.wait(lock, [&] { return _readySignal; });
  return _ready;
}

auto NativeInputManager::terminate() -> void {
  if(!_running && !_thread.joinable()) return;

  _running = false;
  if(_window) PostMessage(_window, WM_CLOSE, 0, 0);
  if(_thread.joinable()) _thread.join();

  {
    ::std::lock_guard<::std::mutex> lock(_stateMutex);
    _window = nullptr;
    _keyboardState.fill(0);
    _keyboardMask.store(0, ::std::memory_order_release);
    for(auto& mask : _padMasks) mask.store(0, ::std::memory_order_release);
    _pads.clear();
    _ignoredDevices.clear();
  }

  _ready = false;
  _readySignal = false;
}

auto NativeInputManager::threadMain() -> void {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  SetThreadPriorityBoost(GetCurrentThread(), FALSE);
  applyNativeInputThreadAffinity();
  DWORD taskIndex = 0;
  HANDLE taskHandle = AvSetMmThreadCharacteristicsW(L"Games", &taskIndex);
  if(taskHandle) AvSetMmThreadPriority(taskHandle, AVRT_PRIORITY_HIGH);

  static bool classRegistered = false;
  if(!classRegistered) {
    WNDCLASSW windowClass = {};
    windowClass.hInstance = GetModuleHandle(nullptr);
    windowClass.lpfnWndProc = &NativeInputManager::windowProc;
    windowClass.lpszClassName = L"BsnesFastlinkRawInput";
    classRegistered = RegisterClassW(&windowClass) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
  }

  if(!classRegistered) {
    if(taskHandle) AvRevertMmThreadCharacteristics(taskHandle);
    signalReady(false);
    return;
  }

  auto window = CreateWindowW(
    L"BsnesFastlinkRawInput",
    L"BsnesFastlinkRawInput",
    WS_POPUP,
    0, 0, 64, 64,
    nullptr,
    nullptr,
    GetModuleHandle(nullptr),
    this
  );

  if(!window) {
    if(taskHandle) AvRevertMmThreadCharacteristics(taskHandle);
    signalReady(false);
    return;
  }

  {
    ::std::lock_guard<::std::mutex> lock(_stateMutex);
    _window = window;
  }

  if(!registerRawInput(window)) {
    {
      ::std::lock_guard<::std::mutex> lock(_stateMutex);
      _window = nullptr;
    }
    DestroyWindow(window);
    if(taskHandle) AvRevertMmThreadCharacteristics(taskHandle);
    signalReady(false);
    return;
  }

  signalReady(true);

  MSG message = {};
  while(GetMessage(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessage(&message);
  }

  unregisterRawInput();
  {
    ::std::lock_guard<::std::mutex> lock(_stateMutex);
    _window = nullptr;
    _keyboardState.fill(0);
    _keyboardMask.store(0, ::std::memory_order_release);
    for(auto& mask : _padMasks) mask.store(0, ::std::memory_order_release);
    _pads.clear();
    _ignoredDevices.clear();
  }
  if(taskHandle) AvRevertMmThreadCharacteristics(taskHandle);
  _running = false;
}

auto NativeInputManager::signalReady(bool ready) -> void {
  {
    ::std::lock_guard<::std::mutex> lock(_readyMutex);
    _ready = ready;
    _readySignal = true;
  }
  _readyCondition.notify_all();
}

auto NativeInputManager::registerRawInput(HWND window) -> bool {
  RAWINPUTDEVICE devices[3] = {};

  devices[0].usUsagePage = 0x01;
  devices[0].usUsage = 0x06;
  devices[0].dwFlags = RIDEV_INPUTSINK;
  devices[0].hwndTarget = window;

  devices[1].usUsagePage = 0x01;
  devices[1].usUsage = 0x04;
  devices[1].dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
  devices[1].hwndTarget = window;

  devices[2].usUsagePage = 0x01;
  devices[2].usUsage = 0x05;
  devices[2].dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
  devices[2].hwndTarget = window;

  return RegisterRawInputDevices(devices, 3, sizeof(RAWINPUTDEVICE));
}

auto NativeInputManager::unregisterRawInput() -> void {
  RAWINPUTDEVICE devices[3] = {};

  devices[0].usUsagePage = 0x01;
  devices[0].usUsage = 0x06;
  devices[0].dwFlags = RIDEV_REMOVE;

  devices[1].usUsagePage = 0x01;
  devices[1].usUsage = 0x04;
  devices[1].dwFlags = RIDEV_REMOVE;

  devices[2].usUsagePage = 0x01;
  devices[2].usUsage = 0x05;
  devices[2].dwFlags = RIDEV_REMOVE;

  RegisterRawInputDevices(devices, 3, sizeof(RAWINPUTDEVICE));
}

auto NativeInputManager::removePad(HANDLE device) -> void {
  ::std::lock_guard<::std::mutex> lock(_stateMutex);
  _pads.erase(::std::remove_if(_pads.begin(), _pads.end(), [&](auto& pad) {
    return pad.device == device;
  }), _pads.end());
  _ignoredDevices.erase(::std::remove(_ignoredDevices.begin(), _ignoredDevices.end(), device), _ignoredDevices.end());
  refreshPadMasksLocked();
}

auto NativeInputManager::handleRawInput(HRAWINPUT rawInput) -> void {
  UINT size = 0;
  if(GetRawInputData(rawInput, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 || size == 0) return;

  ::std::array<BYTE, 1024> stackBuffer;
  ::std::vector<BYTE> heapBuffer;
  BYTE* buffer = stackBuffer.data();
  if(size > stackBuffer.size()) {
    heapBuffer.resize(size);
    buffer = heapBuffer.data();
  }

  if(GetRawInputData(rawInput, RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER)) != size) return;

  auto* input = reinterpret_cast<RAWINPUT*>(buffer);

  if(input->header.dwType == RIM_TYPEKEYBOARD) {
    handleRawKeyboard(input->data.keyboard);
    return;
  }

  if(input->header.dwType != RIM_TYPEHID) return;
  if(!input->header.hDevice) return;

  handleRawHid(input->header.hDevice, input->data.hid);
}

auto NativeInputManager::handleRawKeyboard(const RAWKEYBOARD& keyboard) -> void {
  auto virtualKey = resolveVirtualKey(keyboard);
  if(virtualKey == 0 || virtualKey >= _keyboardState.size()) return;

  auto bit = keyboardMaskForKey(virtualKey);
  if(bit) {
    if(keyboard.Flags & RI_KEY_BREAK) {
      _keyboardMask.fetch_and(~bit, ::std::memory_order_release);
    } else {
      _keyboardMask.fetch_or(bit, ::std::memory_order_release);
    }
  }

  ::std::lock_guard<::std::mutex> lock(_stateMutex);
  _keyboardState[virtualKey] = (keyboard.Flags & RI_KEY_BREAK) ? 0 : 1;
}

auto NativeInputManager::handleRawHid(HANDLE device, const RAWHID& hid) -> void {
  const auto reportLength = hid.dwSizeHid;
  if(reportLength == 0 || hid.dwCount == 0) return;
  const auto* report = hid.bRawData + (hid.dwCount - 1) * reportLength;

  ::std::lock_guard<::std::mutex> lock(_stateMutex);
  if(::std::find(_ignoredDevices.begin(), _ignoredDevices.end(), device) != _ignoredDevices.end()) return;

  for(auto& pad : _pads) {
    if(pad.device != device) continue;
    updateRawHidPadReport(pad, report, reportLength);
    refreshPadMasksLocked();
    return;
  }

  if(isXInputDevice(device)) {
    _ignoredDevices.push_back(device);
    return;
  }

  HidPad pad;
  if(!initializeRawHidPad(device, pad)) return;
  updateRawHidPadReport(pad, report, reportLength);
  _pads.push_back(::std::move(pad));
  refreshPadMasksLocked();
}

auto NativeInputManager::initializeRawHidPad(HANDLE device, HidPad& pad) -> bool {
  RID_DEVICE_INFO info = {};
  info.cbSize = sizeof(info);
  UINT infoSize = sizeof(info);
  if(GetRawInputDeviceInfo(device, RIDI_DEVICEINFO, &info, &infoSize) == (UINT)-1) return false;
  if(info.dwType != RIM_TYPEHID) return false;
  if(info.hid.usUsagePage != 0x01) return false;
  if(info.hid.usUsage != 0x04 && info.hid.usUsage != 0x05) return false;

  UINT preparsedSize = 0;
  if(GetRawInputDeviceInfo(device, RIDI_PREPARSEDDATA, nullptr, &preparsedSize) == (UINT)-1 || preparsedSize == 0) return false;

  pad.preparsed.resize(preparsedSize);
  if(GetRawInputDeviceInfo(device, RIDI_PREPARSEDDATA, pad.preparsed.data(), &preparsedSize) == (UINT)-1) return false;

  auto* preparsed = reinterpret_cast<PHIDP_PREPARSED_DATA>(pad.preparsed.data());
  if(HidP_GetCaps(preparsed, &pad.caps) != HIDP_STATUS_SUCCESS) return false;

  pad.device = device;
  pad.vendorID = (USHORT)info.hid.dwVendorId;
  pad.productID = (USHORT)info.hid.dwProductId;
  pad.state = {};
  pad.xAxis = getUsageInfo(pad, 0x30);
  pad.yAxis = getUsageInfo(pad, 0x31);
  pad.hat = getUsageInfo(pad, 0x39);

  USHORT buttonCapsLength = pad.caps.NumberInputButtonCaps;
  if(buttonCapsLength == 0) return true;

  ::std::vector<HIDP_BUTTON_CAPS> buttonCaps(buttonCapsLength);
  if(HidP_GetButtonCaps(HidP_Input, buttonCaps.data(), &buttonCapsLength, preparsed) != HIDP_STATUS_SUCCESS) return true;

  for(USHORT index = 0; index < buttonCapsLength; index++) {
    auto& caps = buttonCaps[index];
    if(caps.UsagePage != 0x09) continue;

    if(caps.IsRange) {
      for(USHORT dataIndex = caps.Range.DataIndexMin; dataIndex <= caps.Range.DataIndexMax; dataIndex++) {
        pad.buttonDataIndices.push_back(dataIndex);
      }
      continue;
    }

    pad.buttonDataIndices.push_back(caps.NotRange.DataIndex);
  }

  ::std::sort(pad.buttonDataIndices.begin(), pad.buttonDataIndices.end());
  pad.buttonDataIndices.erase(::std::unique(pad.buttonDataIndices.begin(), pad.buttonDataIndices.end()), pad.buttonDataIndices.end());
  return true;
}

auto NativeInputManager::getUsageInfo(const HidPad& pad, USAGE usage) const -> HidUsageValue {
  HidUsageValue result;
  auto valueCapsLength = pad.caps.NumberInputValueCaps;
  if(valueCapsLength == 0) return result;

  ::std::vector<HIDP_VALUE_CAPS> valueCaps(valueCapsLength);
  auto* preparsed = reinterpret_cast<PHIDP_PREPARSED_DATA>(const_cast<BYTE*>(pad.preparsed.data()));
  if(HidP_GetValueCaps(HidP_Input, valueCaps.data(), &valueCapsLength, preparsed) != HIDP_STATUS_SUCCESS) return result;

  for(USHORT index = 0; index < valueCapsLength; index++) {
    auto& caps = valueCaps[index];
    if(caps.UsagePage != 0x01) continue;

    auto minimumUsage = caps.IsRange ? caps.Range.UsageMin : caps.NotRange.Usage;
    auto maximumUsage = caps.IsRange ? caps.Range.UsageMax : caps.NotRange.Usage;
    if(usage < minimumUsage || usage > maximumUsage) continue;

    result.logicalMin = caps.LogicalMin;
    result.logicalMax = caps.LogicalMax;
    result.bitSize = caps.BitSize;
    result.valid = result.logicalMax > result.logicalMin;
    return result;
  }

  return result;
}

auto NativeInputManager::updateRawHidDPad(HidPadState& state, ULONG hat) -> void {
  switch(hat) {
  case 0: state.up = true; break;
  case 1: state.up = true, state.right = true; break;
  case 2: state.right = true; break;
  case 3: state.down = true, state.right = true; break;
  case 4: state.down = true; break;
  case 5: state.down = true, state.left = true; break;
  case 6: state.left = true; break;
  case 7: state.up = true, state.left = true; break;
  default: break;
  }
}

auto NativeInputManager::updateRawHidPadReport(HidPad& pad, const BYTE* report, ULONG reportLength) -> void {
  if(!report || reportLength == 0) return;

  auto* preparsed = reinterpret_cast<PHIDP_PREPARSED_DATA>(pad.preparsed.data());
  HidPadState state = {};
  state.attached = true;

  auto dataLength = HidP_MaxDataListLength(HidP_Input, preparsed);
  if(dataLength > 0 && !pad.buttonDataIndices.empty()) {
    pad.dataList.resize(dataLength);
    if(HidP_GetData(HidP_Input, pad.dataList.data(), &dataLength, preparsed, (PCHAR)report, reportLength) == HIDP_STATUS_SUCCESS) {
      for(ULONG index = 0; index < dataLength; index++) {
        auto button = ::std::lower_bound(pad.buttonDataIndices.begin(), pad.buttonDataIndices.end(), pad.dataList[index].DataIndex);
        if(button == pad.buttonDataIndices.end() || *button != pad.dataList[index].DataIndex) continue;

        auto buttonIndex = (uint)(button - pad.buttonDataIndices.begin());
        if(buttonIndex >= MaximumRawButtons || !pad.dataList[index].On) continue;
        state.buttons[buttonIndex] = true;
      }
    }
  }

  ULONG value = 0;

  auto readAxis = [&](USAGE usage, const HidUsageValue& info, bool& negative, bool& positive) {
    if(!info.valid) return;
    if(HidP_GetUsageValue(HidP_Input, 0x01, 0, usage, &value, preparsed, (PCHAR)report, reportLength) != HIDP_STATUS_SUCCESS) return;

    LONG signedValue = value;
    if(info.logicalMin < 0 && info.bitSize > 0 && info.bitSize < 32 && (value & (1ul << (info.bitSize - 1)))) {
      signedValue = value | (~0ul << info.bitSize);
    }

    auto normalized = ((int)(signedValue - info.logicalMin) * 200 / (info.logicalMax - info.logicalMin)) - 100;
    negative = normalized < -RawInputNeutralThreshold;
    positive = normalized > RawInputNeutralThreshold;
  };

  readAxis(0x30, pad.xAxis, state.left, state.right);
  readAxis(0x31, pad.yAxis, state.up, state.down);

  if(pad.hat.valid
  && HidP_GetUsageValue(HidP_Input, 0x01, 0, 0x39, &value, preparsed, (PCHAR)report, reportLength) == HIDP_STATUS_SUCCESS) {
    LONG signedValue = value;
    if(pad.hat.logicalMin < 0 && pad.hat.bitSize > 0 && pad.hat.bitSize < 32 && (value & (1ul << (pad.hat.bitSize - 1)))) {
      signedValue = value | (~0ul << pad.hat.bitSize);
    }

    if(signedValue >= pad.hat.logicalMin && signedValue <= pad.hat.logicalMax) {
      updateRawHidDPad(state, signedValue - pad.hat.logicalMin);
    }
  }

  pad.state = state;
}

auto NativeInputManager::refreshPadMasksLocked() -> void {
  for(auto& mask : _padMasks) mask.store(0, ::std::memory_order_release);

  uint activePort = 0;
  for(auto& pad : _pads) {
    if(!pad.state.attached) continue;
    if(activePort >= _padMasks.size()) break;
    _padMasks[activePort++].store(padToMask(pad), ::std::memory_order_release);
  }
}

auto NativeInputManager::padToMask(const HidPad& pad) const -> uint32_t {
  uint32_t mask = 0;
  for(uint input = GamepadUp; input <= GamepadStart; input++) {
    if(mapPad(pad, input)) mask |= 1u << input;
  }
  return mask;
}

auto NativeInputManager::keyboardMaskForKey(WORD key) const -> uint32_t {
  switch(key) {
  case VK_UP: return 1u << GamepadUp;
  case VK_DOWN: return 1u << GamepadDown;
  case VK_LEFT: return 1u << GamepadLeft;
  case VK_RIGHT: return 1u << GamepadRight;
  case 'Z': return 1u << GamepadB;
  case 'X': return 1u << GamepadA;
  case 'A': return 1u << GamepadY;
  case 'S': return 1u << GamepadX;
  case 'Q': return 1u << GamepadL;
  case 'W': return 1u << GamepadR;
  case VK_LSHIFT:
  case VK_RSHIFT: return 1u << GamepadSelect;
  case VK_RETURN: return 1u << GamepadStart;
  }
  return 0;
}

auto NativeInputManager::mapKeyboard(uint port, uint input) const -> bool {
  if(port != 0) return false;
  if(input > GamepadStart) return false;
  return _keyboardMask.load(::std::memory_order_acquire) & (1u << input);
}

auto NativeInputManager::mapPad(const HidPad& pad, uint input) const -> bool {
  // Known RetroUSB devices always use their established physical ordering.
  // Unknown HID devices default to that compatibility map for existing
  // FastLink users, but can opt into conventional HID ordering in the config.
  auto mapping = isRetroUSBController(pad.vendorID, pad.productID)
    || settings.fastLink.genericHIDMapping == "SNES Adapter"
    ? RetroUSBButtonMap : StandardHIDButtonMap;

  switch(input) {
  case GamepadUp: return pad.state.up;
  case GamepadDown: return pad.state.down;
  case GamepadLeft: return pad.state.left;
  case GamepadRight: return pad.state.right;
  case GamepadB: return buttonPressed(pad.state.buttons, mapping.b);
  case GamepadA: return buttonPressed(pad.state.buttons, mapping.a);
  case GamepadY: return buttonPressed(pad.state.buttons, mapping.y);
  case GamepadX: return buttonPressed(pad.state.buttons, mapping.x);
  case GamepadL: return buttonPressed(pad.state.buttons, mapping.l);
  case GamepadR: return buttonPressed(pad.state.buttons, mapping.r);
  case GamepadSelect: return buttonPressed(pad.state.buttons, mapping.select);
  case GamepadStart: return buttonPressed(pad.state.buttons, mapping.start);
  }

  return false;
}

auto NativeInputManager::pollGamepad(uint port, uint input) -> int16 {
  if(input > GamepadStart) return 0;
  uint32_t mask = port == 0 ? _keyboardMask.load(::std::memory_order_acquire) : 0;
  if(port < _padMasks.size()) mask |= _padMasks[port].load(::std::memory_order_acquire);
  bool pressed = mask & (1u << input);
  return pressed ? 1 : 0;
}

auto NativeInputManager::rumbleGamepad(uint port, uint input, bool enable) -> void {
}

auto NativeInputManager::resolveVirtualKey(const RAWKEYBOARD& keyboard) -> WORD {
  auto virtualKey = keyboard.VKey;
  if(virtualKey == 255) return 0;

  if(virtualKey == VK_SHIFT) {
    virtualKey = (WORD)MapVirtualKey(keyboard.MakeCode, MAPVK_VSC_TO_VK_EX);
  } else if(virtualKey == VK_CONTROL) {
    virtualKey = (keyboard.Flags & RI_KEY_E0) ? VK_RCONTROL : VK_LCONTROL;
  } else if(virtualKey == VK_MENU) {
    virtualKey = (keyboard.Flags & RI_KEY_E0) ? VK_RMENU : VK_LMENU;
  }

  return virtualKey;
}

auto NativeInputManager::isXInputDevice(HANDLE device) -> bool {
  UINT nameSize = 0;
  GetRawInputDeviceInfoA(device, RIDI_DEVICENAME, nullptr, &nameSize);
  if(nameSize == 0) return false;

  ::std::vector<char> name(nameSize + 1, '\0');
  if(GetRawInputDeviceInfoA(device, RIDI_DEVICENAME, name.data(), &nameSize) == (UINT)-1) return false;

  for(auto& ch : name) ch = (char)toupper((unsigned char)ch);
  return strstr(name.data(), "IG_") != nullptr;
}

auto CALLBACK NativeInputManager::windowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) -> LRESULT {
  if(msg == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCT*>(lparam);
    SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)create->lpCreateParams);
    return DefWindowProc(hwnd, msg, wparam, lparam);
  }

  auto* self = reinterpret_cast<NativeInputManager*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
  if(!self) return DefWindowProc(hwnd, msg, wparam, lparam);

  if(msg == WM_INPUT_DEVICE_CHANGE) {
    if(wparam == GIDC_REMOVAL) self->removePad((HANDLE)lparam);
    return 0;
  }

  if(msg == WM_INPUT) {
    self->handleRawInput((HRAWINPUT)lparam);
    return DefWindowProc(hwnd, msg, wparam, lparam);
  }

  if(msg == WM_CLOSE) {
    DestroyWindow(hwnd);
    return 0;
  }

  if(msg == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }

  return DefWindowProc(hwnd, msg, wparam, lparam);
}

#else
NativeInputManager nativeInputManager;

auto NativeInputManager::initialize() -> bool {
  _ready = false;
  return false;
}

auto NativeInputManager::terminate() -> void {
}

auto NativeInputManager::pollGamepad(uint port, uint input) -> int16 {
  return 0;
}

auto NativeInputManager::rumbleGamepad(uint port, uint input, bool enable) -> void {
}
#endif
