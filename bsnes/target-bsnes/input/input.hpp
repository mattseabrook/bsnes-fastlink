#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#if defined(PLATFORM_WINDOWS)
#include <hidsdi.h>
#endif

inline constexpr auto NativeInputDriverName = "Win32 Raw Input";

struct NativeInputManager {
  auto initialize() -> bool;
  auto terminate() -> void;
  auto driver() const -> string { return NativeInputDriverName; }
  auto ready() const -> bool { return _ready; }
  auto pollGamepad(uint port, uint input) -> int16;

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

  struct HidUsageValue {
    bool valid = false;
    LONG logicalMin = 0;
    LONG logicalMax = 0;
    USHORT bitSize = 0;
  };

  struct HidPad {
    HANDLE device = nullptr;
    USHORT vendorID = 0;
    USHORT productID = 0;
    ::std::vector<BYTE> preparsed;
    HIDP_CAPS caps = {};
    HidPadState state;
    ::std::vector<USHORT> buttonDataIndices;
    ::std::vector<HIDP_DATA> dataList;
    HidUsageValue xAxis;
    HidUsageValue yAxis;
    HidUsageValue hat;
  };

  auto threadMain() -> void;
  auto signalReady(bool ready) -> void;
  auto registerRawInput(HWND window) -> bool;
  auto unregisterRawInput() -> void;
  auto removePad(HANDLE device) -> void;
  auto handleRawInput(HRAWINPUT rawInput) -> void;
  auto handleRawHid(HANDLE device, const RAWHID& hid) -> void;
  auto initializeRawHidPad(HANDLE device, HidPad& pad) -> bool;
  auto getUsageInfo(const HidPad& pad, USAGE usage) const -> HidUsageValue;
  auto updateRawHidDPad(HidPadState& state, ULONG hat) -> void;
  auto updateRawHidPadReport(HidPad& pad, const BYTE* report, ULONG reportLength) -> void;
  auto refreshPadMasks() -> void;
  auto padToMask(const HidPad& pad) const -> uint32_t;

  static auto isXInputDevice(HANDLE device) -> bool;
  static auto CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) -> LRESULT;

  ::std::thread _thread;
  ::std::mutex _readyMutex;
  ::std::condition_variable _readyCondition;
  ::std::vector<HidPad> _pads;
  ::std::vector<HANDLE> _ignoredDevices;
  ::std::vector<RAWINPUT> _rawInputBuffer;
  ::std::array<::std::atomic<uint32_t>, 4> _padMasks = {};
  ::std::atomic<HWND> _window{nullptr};
  bool _readySignal = false;
  #endif
};

extern NativeInputManager nativeInputManager;
