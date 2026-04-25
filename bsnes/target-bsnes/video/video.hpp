#pragma once

#if defined(PLATFORM_WINDOWS)

inline constexpr auto NativeVideoDriverName = "Win32 Vulkan";

struct NativeVideoImplementation;

struct NativeVideoManager {
  friend struct NativeVideoImplementation;

  struct Monitor {
    string name;
    bool primary = false;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
  };

  struct Size {
    uint width = 0;
    uint height = 0;
  };

  struct Acquire {
    explicit operator bool() const { return data; }
    uint32_t* data = nullptr;
    uint pitch = 0;
  };

  NativeVideoManager();
  ~NativeVideoManager();

  explicit operator bool() const;

  static auto optimalDriver() -> string { return NativeVideoDriverName; }
  static auto safestDriver() -> string { return NativeVideoDriverName; }
  static auto hasDrivers() -> vector<string> { return {NativeVideoDriverName, "None"}; }
  static auto hasMonitors() -> vector<Monitor>;
  static auto hasMonitor(string name) -> bool;

  auto reset() -> void;
  auto create(string driver = "") -> bool;
  auto driver() const -> string { return _driver; }
  auto ready() const -> bool { return _ready; }

  auto hasFullScreen() const -> bool { return false; }
  auto hasExclusive() const -> bool { return false; }
  auto hasBlocking() const -> bool { return false; }
  auto hasFlush() const -> bool { return true; }
  auto hasFormats() const -> vector<string> { return {"ARGB24"}; }
  auto hasFormat(string format) const -> bool { return format == "ARGB24"; }
  auto hasShader() const -> bool { return true; }

  auto fullScreen() const -> bool { return _fullScreen; }
  auto monitor() const -> string { return _monitor; }
  auto exclusive() const -> bool { return _exclusive; }
  auto context() const -> uintptr { return _context; }
  auto blocking() const -> bool { return _blocking; }
  auto flush() const -> bool { return _flush; }
  auto format() const -> string { return _format; }
  auto shader() const -> string { return _shader; }

  auto setFullScreen(bool fullScreen) -> bool;
  auto setMonitor(string monitor) -> bool;
  auto setExclusive(bool exclusive) -> bool;
  auto setContext(uintptr context) -> bool;
  auto setBlocking(bool blocking) -> bool;
  auto setFlush(bool flush) -> bool;
  auto setFormat(string format) -> bool;
  auto setShader(string shader) -> bool;

  auto focused() -> bool;
  auto clear() -> void;
  auto size() -> Size;
  auto acquire(uint width, uint height) -> Acquire;
  auto release() -> void;
  auto output(uint width = 0, uint height = 0) -> void;
  auto poll() -> void;

  auto onUpdate(const function<void (uint, uint)>& callback) -> void;
  auto doUpdate(uint width, uint height) -> void;

private:
  auto initialize() -> bool;
  auto defaultMonitorName() const -> string;
  auto currentClientSize() const -> Size;

  string _driver = "None";
  bool _ready = false;
  bool _fullScreen = false;
  string _monitor;
  bool _exclusive = false;
  uintptr _context = 0;
  bool _blocking = false;
  bool _flush = true;
  string _format = "ARGB24";
  string _shader = "None";
  Size _lastSize = {};
  function<void (uint, uint)> _update;
  unique_pointer<NativeVideoImplementation> _impl;
};

#endif
