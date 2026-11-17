struct Settings : Markup::Node {
  using string = nall::string;

  auto load() -> void;
  auto save() -> void;
  auto process(bool load) -> void;

  string location;

  struct Video {
    string driver;
    bool exclusive = false;
    bool blocking = false;
    bool flush = false;
    string presentMode = "Mailbox";
    uint swapchainImages = 2;
    string monitor = "Primary";
    string format = "Default";
    string shader = "None";

    uint luminance = 100;
    uint saturation = 100;
    uint gamma = 150;
    bool dimming = true;
    bool snow = false;

    string output = "Scale";
    uint multiplier = 2;
    bool aspectCorrection = true;
    bool overscan = false;
    bool blur = false;
    bool hdMode7 = true;
    string filter = "None";
  } video;

  struct Audio {
    string driver;
    bool exclusive = false;
    string device;
    bool blocking = false;
    bool dynamic = false;
    uint frequency = 48000;
    uint latency = 0;

    bool mute = false;
    bool muteUnfocused = false;
    int skew = 0;
    uint volume = 100;
    uint balance = 50;
  } audio;

  struct Input {
    string driver;
    uint frequency = 5;
    string defocus = "Pause";
    struct Turbo {
      uint frequency = 4;
    } turbo;
    struct Hotkey {
      string logic = "or";
    } hotkey;
  } input;

  struct Path {
    string games;
    string patches;
    string saves;
    string cheats;
    string states;
    string screenshots;
    struct Recent {
      string superFamicom;
      string gameBoy;
      string bsMemory;
      string sufamiTurboA;
      string sufamiTurboB;
    } recent;
  } path;

  struct FastForward {
    uint limiter = 0;
    bool mute = false;
  } fastForward;

  struct FastLink {
    bool ramTraceServer = false;
    bool disableSoftwareFilters = true;
    bool lowLatencyInactiveYield = true;
    bool softwareFramePacing = true;
    uint inputPollMicroseconds = 500;
    string genericHIDMapping = "SNES Adapter";
    int mainThreadAffinity = -2;
    int inputThreadAffinity = -2;
    bool windowsTimerResolution = true;
    bool windowsHighPerformancePower = false;
  } fastLink;

  struct Emulator {
    bool warnOnUnverifiedGames = false;
    struct AutoSaveMemory {
      bool enable = false;
      uint interval = 30;
    } autoSaveMemory;
    bool autoSaveStateOnUnload = false;
    bool autoLoadStateOnLoad = false;
    struct Cheats {
      bool enable = true;
    } cheats;
  } emulator;

  struct General {
    struct Window {
      int x = 128;
      int y = 128;
      uint width = 0;
      uint height = 0;
      bool maximized = false;
      string monitor = "Primary";
    } window;

    bool statusBar = true;
    bool screenSaver = false;
    bool toolTips = true;
    bool crashed = false;
    string crashComponent;
    bool nativeFileDialogs = false;
  } general;
};


extern Settings settings;
