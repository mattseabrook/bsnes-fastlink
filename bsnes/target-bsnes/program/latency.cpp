namespace {
bool windowsLatencyModeActive = false;
bool ntTimerResolutionActive = false;
ULONG ntTimerResolutionCurrent = 0;
GUID* previousPowerScheme = nullptr;
HANDLE frameWaitTimer = nullptr;

using NtSetTimerResolutionProc = LONG (WINAPI*)(ULONG, BOOLEAN, PULONG);

auto applyThreadAffinity(int processor) -> void {
  // Automatic scheduling must not pin the emulator to the final logical CPU,
  // which may be busy or a lower-performance core on a hybrid processor.
  if(processor < 0) return;

  DWORD_PTR processMask = 0;
  DWORD_PTR systemMask = 0;
  if(!GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) || !processMask) return;

  int target = processor;
  if(target < 0 || target >= (int)(sizeof(DWORD_PTR) * 8)) return;

  DWORD_PTR mask = (DWORD_PTR)1 << target;
  if(!(processMask & mask)) return;
  SetThreadAffinityMask(GetCurrentThread(), mask);
  SetThreadIdealProcessor(GetCurrentThread(), target);
}

auto enableNtTimerResolution() -> void {
  if(!settings.fastLink.windowsTimerResolution || ntTimerResolutionActive) return;
  auto ntdll = GetModuleHandleA("ntdll.dll");
  if(!ntdll) return;
  auto setTimerResolution = (NtSetTimerResolutionProc)GetProcAddress(ntdll, "NtSetTimerResolution");
  if(!setTimerResolution) return;
  if(setTimerResolution(5000, TRUE, &ntTimerResolutionCurrent) >= 0) {
    ntTimerResolutionActive = true;
  }
}

auto disableNtTimerResolution() -> void {
  if(!ntTimerResolutionActive) return;
  auto ntdll = GetModuleHandleA("ntdll.dll");
  auto setTimerResolution = ntdll ? (NtSetTimerResolutionProc)GetProcAddress(ntdll, "NtSetTimerResolution") : nullptr;
  if(setTimerResolution) setTimerResolution(5000, FALSE, &ntTimerResolutionCurrent);
  ntTimerResolutionActive = false;
}

auto enableHighPerformancePowerScheme() -> void {
  if(!settings.fastLink.windowsHighPerformancePower) return;
  auto powrprof = LoadLibraryW(L"powrprof.dll");
  if(!powrprof) return;
  using PowerGetActiveSchemeProc = DWORD (WINAPI*)(HKEY, GUID**);
  using PowerSetActiveSchemeProc = DWORD (WINAPI*)(HKEY, const GUID*);
  auto getActiveScheme = (PowerGetActiveSchemeProc)GetProcAddress(powrprof, "PowerGetActiveScheme");
  auto setActiveScheme = (PowerSetActiveSchemeProc)GetProcAddress(powrprof, "PowerSetActiveScheme");
  if(setActiveScheme) {
    if(!previousPowerScheme && getActiveScheme) getActiveScheme(nullptr, &previousPowerScheme);
    const GUID highPerformance = {0x8c5e7fda, 0xe8bf, 0x4a96, {0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c}};
    setActiveScheme(nullptr, &highPerformance);
  }
  FreeLibrary(powrprof);
}

auto restorePowerScheme() -> void {
  if(!previousPowerScheme) return;
  auto powrprof = LoadLibraryW(L"powrprof.dll");
  if(powrprof) {
    using PowerSetActiveSchemeProc = DWORD (WINAPI*)(HKEY, const GUID*);
    auto setActiveScheme = (PowerSetActiveSchemeProc)GetProcAddress(powrprof, "PowerSetActiveScheme");
    if(setActiveScheme) setActiveScheme(nullptr, previousPowerScheme);
    FreeLibrary(powrprof);
  }
  LocalFree(previousPowerScheme);
  previousPowerScheme = nullptr;
}

auto enableWindowsLatencyMode() -> void {
  SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
  SetProcessPriorityBoost(GetCurrentProcess(), FALSE);
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  SetThreadPriorityBoost(GetCurrentThread(), FALSE);

  if(!windowsLatencyModeActive && timeBeginPeriod(1) == TIMERR_NOERROR) {
    windowsLatencyModeActive = true;
  }
  enableNtTimerResolution();
  enableHighPerformancePowerScheme();
  applyThreadAffinity(settings.fastLink.mainThreadAffinity);
  frameWaitTimer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
    TIMER_MODIFY_STATE|SYNCHRONIZE);
}

auto disableWindowsLatencyMode() -> void {
  if(frameWaitTimer){CloseHandle(frameWaitTimer);frameWaitTimer=nullptr;}
  restorePowerScheme();
  disableNtTimerResolution();
  if(windowsLatencyModeActive) {
    timeEndPeriod(1);
    windowsLatencyModeActive = false;
  }
}

auto waitForFrameDeadline(uint64 deadline) -> void {
  for(;;) {
    uint64 now=chrono::nanosecond();
    if(now>=deadline)return;
    uint64 remaining=deadline-now;
    // Sleep on a one-shot high-resolution timer, leaving only a short final
    // spin. The former 2ms spin occupied a CPU for ~12% of every second.
    if(frameWaitTimer && remaining>200'000) {
      LARGE_INTEGER due{};due.QuadPart=-(LONGLONG)((remaining-100'000)/100);
      if(SetWaitableTimer(frameWaitTimer,&due,0,nullptr,nullptr,FALSE)) {
        auto result=MsgWaitForMultipleObjectsEx(1,&frameWaitTimer,INFINITE,QS_SENDMESSAGE,0);
        // Let a presentation worker's synchronous WSI messages complete while
        // waiting. Posted user actions are handled later by the normal loop.
        if(result==WAIT_OBJECT_0+1){MSG message{};PeekMessageW(&message,nullptr,0,0,PM_NOREMOVE);continue;}
        if(result==WAIT_OBJECT_0)continue;
      }
    }
    if(remaining>2'000'000)Sleep(1);else YieldProcessor();
  }
}

}

namespace {
auto fastLinkFrameDurationNanoseconds() -> uint64 {
  bool interlace = SuperFamicom::cpu.interlace();
  double frequency = SuperFamicom::system.cpuFrequency();
  if(SuperFamicom::Region::PAL()) {
    double clocks = interlace ? 1364.0 * 312.5 + 2.0 : 1364.0 * 312.0;
    return (uint64)(1'000'000'000.0 * clocks / frequency + 0.5);
  }

  double clocks = interlace ? 1364.0 * 262.5 : 1364.0 * 262.0 - 2.0;
  return (uint64)(1'000'000'000.0 * clocks / frequency + 0.5);
}
}

