#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#if defined(PLATFORM_WINDOWS)
#include <hidsdi.h>
#endif

enum : uint { BindingLimit = 4 };
inline constexpr auto NativeInputDriverName = "Win32 Raw Input";

struct InputMapping {
  auto bind() -> void;
  auto bind(string mapping, uint binding) -> void;
  auto bind(shared_pointer<HID::Device> device, uint group, uint input, int16 oldValue, int16 newValue, uint binding) -> bool;
  auto unbind(uint binding) -> void;
  auto poll() -> int16;
  auto rumble(bool enable) -> void;

  using Type = Emulator::Interface::Input::Type;
  auto isDigital() const -> bool {
    return type == Type::Hat || type == Type::Button || type == Type::Trigger || type == Type::Control;
  }
  auto isAnalog() const -> bool {
    return type == Type::Axis;
  }
  auto isRumble() const -> bool {
    return type == Type::Rumble;
  }

  uint portID = 0;
  uint deviceID = 0;
  uint inputID = 0;
  maybe<uint> turboID;

  string path;  //configuration file key path
  string name;  //input name (human readable)
  uint type = 0;
  string assignments[BindingLimit];

  enum class Logic : uint { AND, OR };
  enum class Qualifier : uint { None, Lo, Hi, Rumble };
  virtual auto logic() const -> Logic { return Logic::OR; }

  struct Binding {
    auto icon() -> image;
    auto name() -> string;

    shared_pointer<HID::Device> device;
    uint group = 0;
    uint input = 0;
    Qualifier qualifier = Qualifier::None;
  };
  Binding bindings[BindingLimit];

  uint3 turboCounter = 0;
};

struct InputHotkey : InputMapping {
  InputHotkey(string name) { this->name = name; }
  auto& onPress(function<void ()> press) { return this->press = press, *this; }
  auto& onRelease(function<void ()> release) { return this->release = release, *this; }
  auto logic() const -> Logic override;

  function<void ()> press;
  function<void ()> release;
  int16 state = 0;
};

struct InputDevice {
  uint id;
  string name;
  vector<InputMapping> mappings;
};

struct InputPort {
  uint id;
  string name;
  vector<InputDevice> devices;
};

struct InputManager {
  InputMapping::Logic hotkeyLogic = InputMapping::Logic::OR;

  auto initialize() -> void;
  auto bind() -> void;
  auto poll() -> void;
  auto frame() -> void;
  auto onChange(shared_pointer<HID::Device> device, uint group, uint input, int16_t oldValue, int16_t newValue) -> void;
  auto mapping(uint port, uint device, uint input) -> maybe<InputMapping&>;
  auto findMouse() -> shared_pointer<HID::Device>;
  auto flushWorkerEvents() -> void;

  //hotkeys.cpp
  auto bindHotkeys() -> void;
  auto pollHotkeys() -> void;

  //fastlink: HID worker thread
  auto startWorker() -> void;
  auto stopWorker() -> void;

public:
  vector<shared_pointer<HID::Device>> devices;
  vector<InputPort> ports;
  vector<InputHotkey> hotkeys;

  uint64 lastPoll = 0;   //time in milliseconds since last call to poll()
  uint64 frequency = 0;  //minimum time in milliseconds before poll() can be called again

  uint turboCounter = 0;
  uint turboFrequency = 0;

  struct ChangeEvent {
    shared_pointer<HID::Device> device;
    uint group = 0;
    uint input = 0;
    int16_t oldValue = 0;
    int16_t newValue = 0;
  };

  //fastlink: worker thread state
  ::std::thread _pollThread;
  ::std::mutex _deviceMutex;
  ::std::mutex _changeMutex;
  ::std::atomic<bool> _pollRunning{false};
  ::std::atomic<bool> _devicesChanged{false};
  ::std::thread::id _mainThreadID;
  vector<shared_pointer<HID::Device>> _pendingDevices;
  ::std::vector<ChangeEvent> _pendingChanges;
};

struct NativeInputManager {
  auto initialize() -> bool;
  auto terminate() -> void;
  auto driver() const -> string { return NativeInputDriverName; }
  auto ready() const -> bool { return _ready; }
  auto pollGamepad(uint port, uint input) -> int16;
  auto rumbleGamepad(uint port, uint input, bool enable) -> void;

private:
  bool _ready = false;

  #if defined(PLATFORM_WINDOWS)
  struct HidPadState {
    bool attached = false;
    bool left = false;
    bool right = false;
    bool up = false;
    bool down = false;
    bool buttons[32] = {};
  };

  struct HidPad {
    HANDLE device = nullptr;
    USHORT vendorID = 0;
    USHORT productID = 0;
    ::std::vector<BYTE> preparsed;
    HIDP_CAPS caps = {};
    HidPadState state;
    ::std::vector<USHORT> buttonDataIndices;
  };

  auto threadMain() -> void;
  auto signalReady(bool ready) -> void;
  auto registerRawInput(HWND window) -> bool;
  auto unregisterRawInput() -> void;
  auto removePad(HANDLE device) -> void;
  auto handleRawInput(HRAWINPUT rawInput) -> void;
  auto handleRawKeyboard(const RAWKEYBOARD& keyboard) -> void;
  auto handleRawHid(HANDLE device, const RAWHID& hid) -> void;
  auto initializeRawHidPad(HANDLE device, HidPad& pad) -> bool;
  auto getUsageInfo(const HidPad& pad, USAGE usage, LONG& logicalMin, LONG& logicalMax, USHORT& bitSize) const -> bool;
  auto updateRawHidDPad(HidPadState& state, ULONG hat) -> void;
  auto updateRawHidPadReport(HidPad& pad, const BYTE* report, ULONG reportLength) -> void;
  auto mapKeyboard(uint port, uint input) const -> bool;
  auto mapPad(const HidPad& pad, uint input) const -> bool;
  auto keyPressed(WORD key) const -> bool;

  static auto resolveVirtualKey(const RAWKEYBOARD& keyboard) -> WORD;
  static auto isXInputDevice(HANDLE device) -> bool;
  static auto CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) -> LRESULT;

  ::std::thread _thread;
  ::std::mutex _stateMutex;
  ::std::mutex _readyMutex;
  ::std::condition_variable _readyCondition;
  ::std::vector<HidPad> _pads;
  ::std::array<uint8_t, 256> _keyboardState = {};
  HWND _window = nullptr;
  bool _running = false;
  bool _readySignal = false;
  #endif
};

extern InputManager inputManager;
extern NativeInputManager nativeInputManager;
