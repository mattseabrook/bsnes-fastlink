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
  int processor=settings.fastLink.inputThreadAffinity;
  // Automatic mode leaves scheduling to Windows/MMCSS rather than forcing
  // input onto the last logical CPU. Explicit processor selection is retained.
  if(processor<0 || processor>=(int)(sizeof(DWORD_PTR)*8))return;
  DWORD_PTR processMask=0,systemMask=0;
  if(!GetProcessAffinityMask(GetCurrentProcess(),&processMask,&systemMask))return;
  DWORD_PTR mask=(DWORD_PTR)1<<processor;
  if(!(processMask&mask))return;
  SetThreadAffinityMask(GetCurrentThread(),mask);
  SetThreadIdealProcessor(GetCurrentThread(),processor);
}
}

NativeInputManager nativeInputManager;

auto NativeInputManager::initialize() -> bool {
  terminate();

  if(_rawInputBuffer.empty()) _rawInputBuffer.resize(32);

  _ready = false;
  _readySignal = false;
  _thread = ::std::thread([this] { threadMain(); });

  ::std::unique_lock<::std::mutex> lock(_readyMutex);
  _readyCondition.wait(lock, [&] { return _readySignal; });
  return _ready;
}

auto NativeInputManager::terminate() -> void {
  if(!_thread.joinable()) return;
  if(auto window = _window.load(::std::memory_order_acquire)) PostMessage(window, WM_CLOSE, 0, 0);
  _thread.join();

  _window.store(nullptr, ::std::memory_order_release);
  for(auto& mask : _padMasks) mask.store(0, ::std::memory_order_release);
  _pads.clear();
  _ignoredDevices.clear();

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
    L"",
    0,
    0, 0, 0, 0,
    HWND_MESSAGE,
    nullptr,
    GetModuleHandle(nullptr),
    this
  );

  if(!window) {
    if(taskHandle) AvRevertMmThreadCharacteristics(taskHandle);
    signalReady(false);
    return;
  }

  _window.store(window, ::std::memory_order_release);

  if(!registerRawInput(window)) {
    _window.store(nullptr, ::std::memory_order_release);
    DestroyWindow(window);
    if(taskHandle) AvRevertMmThreadCharacteristics(taskHandle);
    signalReady(false);
    return;
  }

  signalReady(true);

  MSG message = {};
  while(GetMessage(&message, nullptr, 0, 0) > 0) {
    DispatchMessage(&message);
  }

  unregisterRawInput();
  _window.store(nullptr, ::std::memory_order_release);
  for(auto& mask : _padMasks) mask.store(0, ::std::memory_order_release);
  if(taskHandle) AvRevertMmThreadCharacteristics(taskHandle);
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
  const RAWINPUTDEVICE devices[] = {
    {0x01, 0x04, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, window},
    {0x01, 0x05, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, window},
  };
  return RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE));
}

auto NativeInputManager::unregisterRawInput() -> void {
  const RAWINPUTDEVICE devices[] = {
    {0x01, 0x04, RIDEV_REMOVE, nullptr},
    {0x01, 0x05, RIDEV_REMOVE, nullptr},
  };
  RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE));
}

auto NativeInputManager::removePad(HANDLE device) -> void {
  _pads.erase(::std::remove_if(_pads.begin(), _pads.end(), [&](auto& pad) {
    return pad.device == device;
  }), _pads.end());
  _ignoredDevices.erase(::std::remove(_ignoredDevices.begin(), _ignoredDevices.end(), device), _ignoredDevices.end());
  refreshPadMasks();
}

auto NativeInputManager::handleRawInput(HRAWINPUT rawInput) -> void {
  // DWORD-aligned storage is retained by the input thread. Normal packets need
  // one Win32 call; query/grow only when an unusually large report arrives.
  UINT size = (UINT)(_rawInputBuffer.size() * sizeof(RAWINPUT));
  UINT received = GetRawInputData(rawInput, RID_INPUT, _rawInputBuffer.data(), &size, sizeof(RAWINPUTHEADER));
  if(received == (UINT)-1) {
    if(GetLastError() != ERROR_INSUFFICIENT_BUFFER) return;
    size = 0;
    if(GetRawInputData(rawInput, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 || !size) return;
    _rawInputBuffer.resize((size + sizeof(RAWINPUT) - 1) / sizeof(RAWINPUT));
    received = GetRawInputData(rawInput, RID_INPUT, _rawInputBuffer.data(), &size, sizeof(RAWINPUTHEADER));
  }
  if(received == (UINT)-1 || received < sizeof(RAWINPUTHEADER)) return;
  const auto* input = _rawInputBuffer.data();

  if(input->header.dwType != RIM_TYPEHID) return;
  if(!input->header.hDevice) return;

  handleRawHid(input->header.hDevice, input->data.hid);
}

auto NativeInputManager::handleRawHid(HANDLE device, const RAWHID& hid) -> void {
  const auto reportLength = hid.dwSizeHid;
  if(reportLength == 0 || hid.dwCount == 0) return;
  const auto* report = hid.bRawData + (hid.dwCount - 1) * reportLength;

  for(uint port = 0; port < _pads.size(); port++) {
    auto& pad = _pads[port];
    if(pad.device != device) continue;
    updateRawHidPadReport(pad, report, reportLength);
    // Connected pads keep their port until insertion/removal compacts the list.
    if(port < _padMasks.size()) {
      const auto mask = padToMask(pad);
      if(_padMasks[port].load(::std::memory_order_relaxed) != mask)
        _padMasks[port].store(mask, ::std::memory_order_release);
    }
    return;
  }
  if(::std::find(_ignoredDevices.begin(), _ignoredDevices.end(), device) != _ignoredDevices.end()) return;

  if(isXInputDevice(device)) {
    _ignoredDevices.push_back(device);
    return;
  }

  HidPad pad;
  if(!initializeRawHidPad(device, pad)) return;
  updateRawHidPadReport(pad, report, reportLength);
  _pads.push_back(::std::move(pad));
  refreshPadMasks();
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
  pad.dataList.resize(HidP_MaxDataListLength(HidP_Input, preparsed));

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

  ULONG dataLength = (ULONG)pad.dataList.size();
  if(dataLength > 0 && !pad.buttonDataIndices.empty()) {
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

auto NativeInputManager::refreshPadMasks() -> void {
  uint activePort = 0;
  for(auto& pad : _pads) {
    if(!pad.state.attached) continue;
    if(activePort >= _padMasks.size()) break;
    _padMasks[activePort++].store(padToMask(pad), ::std::memory_order_release);
  }
  // Publish each port once: clearing attached pads first exposes a false
  // button release to the emulation thread between the two atomic stores.
  while(activePort < _padMasks.size()) _padMasks[activePort++].store(0, ::std::memory_order_release);
}

auto NativeInputManager::padToMask(const HidPad& pad) const -> uint32_t {
  // Known RetroUSB devices keep their physical ordering; unknown devices can
  // opt into conventional HID ordering through the configuration.
  const auto& mapping = isRetroUSBController(pad.vendorID, pad.productID)
    || settings.fastLink.genericHIDMapping == "SNES Adapter"
    ? RetroUSBButtonMap : StandardHIDButtonMap;
  const auto& state = pad.state;
  return uint32_t(state.up) << GamepadUp
    | uint32_t(state.down) << GamepadDown
    | uint32_t(state.left) << GamepadLeft
    | uint32_t(state.right) << GamepadRight
    | uint32_t(buttonPressed(state.buttons, mapping.b)) << GamepadB
    | uint32_t(buttonPressed(state.buttons, mapping.a)) << GamepadA
    | uint32_t(buttonPressed(state.buttons, mapping.y)) << GamepadY
    | uint32_t(buttonPressed(state.buttons, mapping.x)) << GamepadX
    | uint32_t(buttonPressed(state.buttons, mapping.l)) << GamepadL
    | uint32_t(buttonPressed(state.buttons, mapping.r)) << GamepadR
    | uint32_t(buttonPressed(state.buttons, mapping.select)) << GamepadSelect
    | uint32_t(buttonPressed(state.buttons, mapping.start)) << GamepadStart;
}

auto NativeInputManager::pollGamepad(uint port, uint input) -> int16 {
  if(port >= _padMasks.size() || input > GamepadStart) return 0;
  return (_padMasks[port].load(::std::memory_order_acquire) >> input) & 1u;
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

#endif
