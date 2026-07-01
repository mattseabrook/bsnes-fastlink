# bsnes-fastlink: Deep-Dive Latency Analysis Report

## Executive Summary

This report is a comprehensive analysis of the bsnes-fastlink fork with a singular focus:
**identifying every source of input-to-photon latency outside the core SNES emulation code.**

The analysis covers the main execution loop, video pipeline, audio pipeline, input system, platform abstractions, synchronization primitives, and operating-system interactions. Each finding is rated by severity (Critical / High / Medium / Low) and includes concrete recommendations.

---

## 1. The Main Execution Loop (program.cpp)

### 1.1 Polling-Style Architecture with Cooperative Scheduling

```cpp
auto Program::main() -> void {
  updateStatus();
  video.poll();

  if(Application::modal()) {
    audio.clear();
    return;
  }

  #if !defined(PLATFORM_WINDOWS)
  inputManager.poll();
  #endif
  inputManager.pollHotkeys();

  // ... inactive check ...

  rewindRun();

  emulator->run();
  fastLinkRAMTraceService.publishFrame();

  // auto-save memory check
}
```

**Severity: CRITICAL**

This is a **cooperatively-scheduled single-threaded loop** driven by `Application::onMain()`. The fundamental problem: this loop does NOT run at a fixed cadence locked to the display's vblank. Instead, it is driven by the toolkit's event pump, which on Windows means `PeekMessage/GetMessage` latency + message dispatch overhead + any blocking inside `video.poll()`.

**What's wrong:**
- There is **no busy-wait spinlock** or high-precision timer synchronizing the start of each frame to the display's vblank.
- `video.poll()` can block internally (e.g., waiting for VSync via D3D Present, OpenGL SwapBuffers, or DXGI), which means the entire emulation loop is gated by display output rather than running ahead of it.
- The loop body does work in this order: `video.poll()` → `inputManager.pollHotkeys()` → `emulator->run()` → `fastLinkRAMTraceService.publishFrame()`. Input polling happens **before** the frame runs, but the **video output happens at the start of the NEXT iteration** via `video.poll()` blocking on the previous frame. This creates a full-frame pipeline bubble.

**Recommended fix:** Restructure the loop to the **"frame-synchronized runahead"** pattern:

```cpp
auto Program::main() -> void {
  // 1. Poll input FIRST (at maximum frequency, not gated by anything)
  inputManager.poll();
  inputManager.pollHotkeys();

  // 2. Run the emulation core to generate the NEXT frame
  emulator->run();

  // 3. Push video immediately (non-blocking presentation)
  video.present();  // release the frame, do NOT wait for vsync here

  // 4. Audio is already output by the core's callbacks

  // 5. FastLink RAM trace (off the critical path)
  fastLinkRAMTraceService.publishFrame();

  // 6. Spin-wait with YieldProcessor() until the next frame deadline
  //    (measured from a high-res counter, NOT from video.poll())
  spinUntilNextFrame();
}
```

The key insight: **video output and emulation should NOT be sequential blocking operations.** Emulate, present (fire-and-forget), then spin-wait until the next 16.667ms boundary. The display hardware's vsync should be a separate concern, handled by the video driver in a way that doesn't stall the CPU.

---

### 1.2 The `usleep(20 * 1000)` in Inactive Path

```cpp
if(currentlyInactive) {
    audio.clear();
    usleep(20 * 1000);  // 20ms sleep when window is inactive
    viewportRefresh();
    return;
}
```

**Severity: HIGH**

This is a 20 millisecond sleep. On Windows, `usleep` is typically implemented via `Sleep()` which has terrible granularity unless `timeBeginPeriod(1)` is active (which you do call in `enableWindowsLatencyMode()`). However:

- When the window loses focus, the entire emulation thread sleeps for ~20ms.
- When focus returns, there is a **cold-start penalty** as caches are cold and the scheduler has to re-prioritize the thread.
- For speedrunning ALTTP, if you're switching windows at all (even briefly), this introduces jitter.

**Recommended fix:** Replace with a shorter spin-yield loop:

```cpp
if(currentlyInactive) {
    audio.clear();
    // Yield but don't sleep deep - keeps thread hot
    for(int i = 0; i < 200; i++) {
        SwitchToThread();  // or YieldProcessor() on newer Windows
    }
    viewportRefresh();
    return;
}
```

---

### 1.3 Auto-Save Memory Interval Check

```cpp
if(emulatorSettings.autoSaveMemory.checked()) {
    auto currentTime = chrono::timestamp();
    if(currentTime - autoSaveTime >= settings.emulator.autoSaveMemory.interval) {
        autoSaveTime = currentTime;
        emulator->save();
    }
}
```

**Severity: MEDIUM**

`emulator->save()` writes to disk. On Windows with some antivirus software, this can cause a 5-50ms stall. This happens on the main thread, synchronized with the frame loop.

**Recommended fix:** Move auto-save to a background thread. Queue the save operation and have a worker thread handle the disk I/O.

---

## 2. Input System (input/input.cpp, input/native-win32.cpp)

### 2.1 Input Worker Thread — Correctly Implemented but With a Sleep

```cpp
auto InputManager::startWorker() -> void {
  _pollThread = ::std::thread([this] {
    while(_pollRunning) {
      // ... poll logic ...
      ::std::this_thread::sleep_for(::std::chrono::milliseconds(1));
    }
  });
}
```

**Severity: HIGH**

The input worker thread sleeps for **1ms between polls**. This means the maximum input sampling rate is 1000Hz, which sounds good, but the actual latency is: input arrives → worker wakes from sleep (up to 1ms later) → worker polls → main thread picks it up on next frame.

On Windows, `sleep_for(1ms)` with `timeBeginPeriod(1)` active should give ~1-2ms granularity, but there's still jitter from the scheduler.

**Recommended fix:** Replace sleep with a **busy-wait spinloop** that polls at a much higher effective rate:

```cpp
_pollThread = ::std::thread([this] {
    while(_pollRunning) {
        // ... poll logic ...
        // Spin for ~500 microseconds instead of sleeping 1ms
        auto start = QueryPerformanceCounter();
        LARGE_INTEGER freq; QueryPerformanceFrequency(&freq);
        auto target = start.QuadPart + (freq.QuadPart / 2000);  // ~0.5ms
        LARGE_INTEGER now;
        do {
            YieldProcessor();
            QueryPerformanceCounter(&now);
        } while(now.QuadPart < target);
    }
});
```

Better yet: use **Raw Input** on Windows with `RegisterRawInputDevices` and receive input via `WM_INPUT` messages, which eliminates polling entirely and delivers input at the HID report rate (typically 1000Hz for gaming peripherals).

---

### 2.2 Input Polling on Main Thread (Non-Windows Path)

```cpp
#if !defined(PLATFORM_WINDOWS)
  inputManager.poll();
#endif
```

**Severity: HIGH (Linux/macOS only)**

On non-Windows platforms, input is polled on the main thread with a **frequency-gated check**:

```cpp
auto thisPoll = chrono::millisecond();
if(thisPoll - lastPoll < frequency) return;
```

This means input is only read once per `frequency` milliseconds on the main thread. If `frequency` is set to the default (5ms), that's a 200Hz input poll rate — much slower than the 1000Hz Windows worker thread.

**Recommended fix:** Use the worker thread path on ALL platforms, not just Windows.

---

### 2.3 HID::Device Value Lookups in `InputMapping::poll()`

```cpp
auto value = device->group(group).input(input).value();
```

**Severity: MEDIUM**

Every input query traverses: device → group array lookup → input array lookup → value field. For a standard SNES pad with ~12 buttons × 2 ports, that's ~24 traversals per frame. At 60fps, this is negligible in raw CPU time, but:

- The `group()` and `input()` calls may not be inlined across the ruby HID abstraction boundary.
- Each lookup involves virtual function calls through the ruby driver hierarchy.

**Recommended fix:** Cache the input values at bind time. Store direct pointers/references to the value fields and read them directly:

```cpp
struct CachedBinding {
    const int16_t* valuePtr;  // direct pointer to the value field
    Qualifier qualifier;
    // ...
};
```

---

## 3. Video Pipeline (program/video.cpp, program/viewport.cpp)

### 3.1 Synchronous Acquire-Filter-Release Pattern

```cpp
auto Program::videoFrame(...) -> void {
    // ... screenshot update ...

    if(auto [output, length] = video.acquire(filterWidth, filterHeight); output) {
        filterRender(palette, output, length, ...);
        video.release();
        video.output(outputWidth, outputHeight);
    }

    inputManager.frame();  // <-- called AFTER video output!
}
```

**Severity: CRITICAL**

The video pipeline has a **blocking chain**:

1. `video.acquire()` — may block if the driver's backbuffer is full
2. `filterRender()` — CPU-intensive software pixel filter runs synchronously
3. `video.release()` — may trigger a GPU upload
4. `video.output()` — may block on vsync

Then `inputManager.frame()` is called, which advances the turbo counter. This means the turbo frequency is gated by video output latency, not by emulation timing.

**What's particularly bad:** The software filters (`Filter::HQ2x`, `Filter::Scale2x`, `Filter::None`, etc.) all run **on the CPU in the main thread**, pixel-by-pixel, before the image ever reaches the GPU. On a 256×240 frame upscaled to 512×480, that's 245,760 pixels processed in software every frame.

**Recommended fixes:**

1. **Eliminate CPU filters entirely.** Use GPU shader-based scaling. The `Filter::None` path is the only one that matters for latency — if you're using any filter, you're adding 0.5-3ms of CPU time per frame.

2. **Double-buffer the video output.** The current path does a single-buffer acquire. Use a ring buffer of 2-3 GPU textures. The emulator writes to texture N while texture N-1 is being scanned out. This decouples emulation from display.

3. **Move `inputManager.frame()` to the TOP of `videoFrame()`** (before any video work), so turbo timing is not gated by video latency.

4. **Run the filter on a worker thread** while the emulator is running the next frame. Pre-render the output into a staging buffer.

---

### 3.2 `video.output()` May Block on VSync

The ruby video backend's `output()` call ultimately translates to a platform-specific present call:
- **OpenGL:** `SwapBuffers()` — if swap interval is 1, this blocks until vblank
- **Direct3D:** `Present()` — same behavior
- **Vulkan:** `vkQueuePresentKHR` — with FIFO present mode, this blocks

**Severity: CRITICAL**

If the video driver is configured with vsync enabled (the default in most configurations), `video.output()` will block the main thread for up to **16.667ms** if a frame is missed. This creates a **cascading stall**: the emulation can't start the next frame until the display is ready.

**Recommended fix:** Use **mailbox/triple buffering** or **immediate present mode**:
- OpenGL: Use `WGL_EXT_swap_control_tear` with `SwapInterval(-1)` for adaptive vsync
- Direct3D: Use DXGI swap chain with `DXGI_SWAP_EFFECT_FLIP_DISCARD` and `SyncInterval = 0`
- Vulkan: Use `VK_PRESENT_MODE_MAILBOX_KHR` or `VK_PRESENT_MODE_IMMEDIATE_KHR`

Then implement **software frame pacing** in the main loop:

```cpp
void spinUntilNextFrame() {
    static LARGE_INTEGER lastFrameTime;
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);

    int64_t frameDuration = freq.QuadPart / 60;  // ~16.667ms in QPC units
    int64_t target = lastFrameTime.QuadPart + frameDuration;

    // Spin-wait with YieldProcessor for the last ~2ms
    while(now.QuadPart < target - (freq.QuadPart / 500)) {
        YieldProcessor();
        QueryPerformanceCounter(&now);
    }

    // For the final sub-millisecond, busy spin
    while(now.QuadPart < target) {
        _mm_pause();  // or __builtin_ia32_pause on GCC
        QueryPerformanceCounter(&now);
    }

    lastFrameTime = now;
}
```

---

### 3.3 Palette Recalculation on Every Frame Change

```cpp
auto Program::updateVideoPalette() -> void {
    for(uint color : range(32768)) {
        // ... floating-point math for every color ...
    }
}
```

**Severity: LOW**

The 32,768-entry palette is recalculated whenever settings change. This is correct behavior. However, the inner loop uses `pow()` for gamma correction — `pow()` is a relatively expensive math function. This only runs on settings changes, so it's not on the critical path.

**Recommended fix:** Replace `pow()` with a precomputed lookup table or a fast approximation:

```cpp
// Precompute gamma LUT at startup
float gammaLUT[256];
for(int i = 0; i < 256; i++) {
    gammaLUT[i] = powf(i / 255.0f, gamma);
}
// Then in the palette loop:
r = gammaLUT[r >> 8];  // LUT lookup instead of pow()
```

---

## 4. Audio Pipeline (program/audio.cpp, emulator/audio/)

### 4.1 Audio `process()` Called from Stream `write()`

```cpp
auto Stream::write(const double samples[]) -> void {
    // ... filter processing ...
    channels[c].resampler.write(sample);
    audio.process();  // <-- called on EVERY sample write!
}
```

**Severity: HIGH**

The `audio.process()` function is called from within `Stream::write()`, which itself is called from the emulation core's audio sample generator. `audio.process()` then loops through ALL streams checking if they're pending:

```cpp
auto Audio::process() -> void {
    while(_streams) {
        for(auto& stream : _streams) {
            if(!stream->pending()) return;  // early exit
        }
        // ... mix all streams ...
        platform->audioFrame(samples, _channels);
    }
}
```

This is **synchronous audio generation**: every audio sample triggers a check of all streams, and when a complete audio frame is ready, it's immediately pushed to the platform audio callback (`platform->audioFrame`).

**Problems:**
- The `platform->audioFrame()` call at the bottom of the audio pipeline may block if the audio driver's ring buffer is full.
- The IIR filter processing (Biquad, OnePole) runs on every single audio sample, on the main thread.
- The cubic resampler runs on every sample.

**Recommended fix:**
1. **Decouple audio generation from audio output.** Accumulate audio samples in a ring buffer. Have a separate high-priority audio callback thread read from this buffer.
2. **Pre-allocate the stream resampler buffers** to avoid any allocation during gameplay.
3. Consider using **SSE/AVX intrinsics** for the IIR filter processing if the compiler isn't auto-vectorizing.

---

### 4.2 `audio.clear()` on Modal and Inactive

```cpp
if(Application::modal()) {
    audio.clear();
    return;
}
```

**Severity: MEDIUM**

`audio.clear()` likely zeroes out the audio ring buffer. This causes an audio discontinuity when a dialog opens. More importantly, it adds overhead on a path that's supposed to be fast.

**Recommended fix:** Instead of clearing, just stop pulling from the audio buffer. Let the existing samples play out (they'll be a tiny fraction of a second).

---

## 5. FastLink RAM Trace Service (fastlink/ram-trace.cpp)

### 5.1 `publishFrame()` Called on Main Thread After `emulator->run()`

```cpp
emulator->run();
fastLinkRAMTraceService.publishFrame();
```

**Severity: MEDIUM**

The RAM trace service publishes a frame's worth of data after the emulation core finishes. If this involves:
- Memory copies
- Network sends (TCP/UDP socket write)
- File I/O

...then this adds latency directly to the main thread.

**Recommended fix:** Move `publishFrame()` to a lock-free ring buffer + worker thread. The main thread writes a pointer to the frame data, the worker thread handles transmission.

---

## 6. Windows-Specific Code (program.cpp)

### 6.1 `enableWindowsLatencyMode()` — Good but Incomplete

```cpp
auto enableWindowsLatencyMode() -> void {
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetProcessPriorityBoost(GetCurrentProcess(), FALSE);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    SetThreadPriorityBoost(GetCurrentThread(), FALSE);

    if(!windowsLatencyModeActive && timeBeginPeriod(1) == TIMERR_NOERROR) {
        windowsLatencyModeActive = true;
    }
}
```

**Severity: MEDIUM (improvement possible)**

This is well-implemented but missing some optimizations:

1. **No `SetThreadAffinityMask` call** — the main thread can migrate between CPU cores, causing cache misses and scheduler delays. Pin it to a specific core (ideally the highest-performing one):
   ```cpp
   SetThreadAffinityMask(GetCurrentThread(), 1 << targetCore);
   ```

2. **`HIGH_PRIORITY_CLASS` is not `REALTIME_PRIORITY_CLASS`** — for a dedicated speedrunning setup, consider `REALTIME_PRIORITY_CLASS` with caution. This prevents ANY other process from preempting bsnes. **Warning:** can hang the system if the process enters an infinite loop.

3. **No power profile adjustment** — Windows power plans can throttle CPU. Call:
   ```cpp
   PowerSetActiveScheme(NULL, &GUID_MIN_POWER_SAVINGS);  // High performance
   ```
   Or use `NtSetTimerResolution` for even finer timer granularity than `timeBeginPeriod`.

4. **No thread ideal processor set for the input worker** — the input worker thread should also be pinned, preferably to the SAME core as the main thread to avoid inter-core communication overhead, or to a DIFFERENT core if input processing is heavy.

---

### 6.2 `TerminateProcess()` in Quit Path

```cpp
auto processID = GetCurrentProcessId();
auto handle = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, true, processID);
TerminateProcess(handle, 0);
```

**Severity: LOW**

This is a sledgehammer approach to avoid shutdown crashes in ruby drivers. It works, but it means:
- No cleanup of temporary files
- No graceful release of DirectInput devices
- Audio buffer may not be flushed
- GPU resources may leak (though the OS cleans them up on process termination)

**Recommended fix:** This is fine for a speedrunning-focused build. The 1-2ms saved on shutdown isn't worth chasing. But add `CloseHandle(handle)` before `TerminateProcess` to avoid a handle leak in the brief window before termination.

Actually, looking closer: `OpenProcess` is called, then `TerminateProcess` is called immediately. The handle is never closed. This is harmless since the process dies microseconds later, but it's sloppy.

---

## 7. Platform Abstraction Layer (program/platform.cpp)

### 7.1 `inputPoll()` Double-Indirect Lookup

```cpp
auto Program::inputPoll(uint port, uint device, uint input) -> int16 {
    // ...
    #if defined(PLATFORM_WINDOWS)
    if(device == NativeGamepadDeviceID) {
        value = nativeInputManager.pollGamepad(port, input);
    } else if(auto mapping = inputManager.mapping(port, device, input)) {
        value = mapping->poll();
    }
    #else
    inputManager.poll();
    if(auto mapping = inputManager.mapping(port, device, input)) {
        value = mapping->poll();
    }
    #endif
    // ... movie handling ...
    return value;
}
```

**Severity: MEDIUM**

The emulation core calls `inputPoll()` potentially hundreds of times per frame (once per input per port). Each call:

1. On Windows: checks if it's the native gamepad device ID → calls `nativeInputManager.pollGamepad()` which does a lookup through the native input structures
2. Otherwise: calls `inputManager.mapping()` which does a triple-nested linear search through `ports[] → devices[] → mappings[]`

The linear search through `inputManager.mapping()` is O(n) where n is the total number of input mappings. For a standard setup this is small (~24), but it's still unnecessary.

**Recommended fix:** Precompute a flat lookup table at bind time:

```cpp
// At bind time:
int16_t* inputValueCache[MaxPorts][MaxDevices][MaxInputs];

// In inputPoll():
int16 Program::inputPoll(uint port, uint device, uint input) {
    #if defined(PLATFORM_WINDOWS)
    if(device == NativeGamepadDeviceID) {
        return nativeInputManager.pollGamepad(port, input);
    }
    #endif
    // Direct cache lookup — O(1)
    if(auto* ptr = inputValueCache[port][device][input]) {
        return *ptr;
    }
    return 0;
}
```

The cached values would be updated by the input worker thread every time it polls, so the main thread just reads the cached values with no locks needed (on x86, a plain int16 read is atomic for aligned data).

---

### 7.2 Movie Playback Input Override

```cpp
if(movie.mode == Movie::Mode::Recording) {
    movie.input.append(value);
} else if(movie.mode == Movie::Mode::Playing) {
    if(movie.input) {
        value = movie.input.takeFirst();
    }
    if(!movie.input) {
        movieStop();
    }
}
```

**Severity: LOW**

`movie.input.takeFirst()` on a vector/list is O(n) if it's a linked list or requires a memmove if it's a dynamic array. This is only active during movie playback, but for TAS/speedrunning this matters.

**Recommended fix:** Use a ring buffer or a simple index into a preallocated array:

```cpp
if(movie.mode == Movie::Mode::Playing) {
    if(movie.inputIndex < movie.inputData.size()) {
        value = movie.inputData[movie.inputIndex++];
    } else {
        movieStop();
    }
}
```

---

## 8. Ruby Video/Audio Backend Integration

### 8.1 Video Driver Switching Fallback Chain

```cpp
if(!video.ready()) {
    settings.video.driver = "None";
    return updateVideoDriver(parent);  // recursive fallback!
}
```

**Severity: MEDIUM**

On driver initialization failure, there's a recursive call to `updateVideoDriver()`. If the "None" driver also fails, this would recurse infinitely. In practice "None" always works, but recursion in initialization code is risky.

**Recommended fix:** Use an iterative fallback with a retry counter:

```cpp
for(int retries = 0; retries < 3; retries++) {
    video.create(settings.video.driver);
    if(video.ready()) break;
    settings.video.driver = fallbackChain[retries];
}
```

---

### 8.2 Debug Trace String Formatting (Windows)

```cpp
auto ProgramVideoTrace(const char* format, ...) -> void {
    char buffer[2048];
    int offset = snprintf(buffer, sizeof(buffer), "[bsnes-fastlink][video] ");
    // ... vsnprintf ...
    OutputDebugStringA(buffer);
}
```

**Severity: LOW**

`OutputDebugStringA` is a **kernel trap** — it transitions to kernel mode and can be intercepted by debuggers. When no debugger is attached, it's relatively fast, but it still has syscall overhead.

**Recommended fix:** These traces should be compiled out in release builds:

```cpp
#ifdef DEBUG
#define VIDEO_TRACE(...) ProgramVideoTrace(__VA_ARGS__)
#else
#define VIDEO_TRACE(...) ((void)0)
#endif
```

---

## 9. Filter System (filter/*.cpp)

### 9.1 Software Filter Performance

All filters in `bsnes/filter/` run on the CPU. Even `Filter::None` does a pixel-by-pixel copy with palette lookup:

```cpp
auto render(...) -> void {
    for(uint y = 0; y < height; y++) {
        const uint16_t* in = input + y * pitch;
        uint32_t* out = output + y * outpitch;
        for(uint x = 0; x < width; x++) {
            *out++ = colortable[*in++];  // 1 memory read + 1 LUT + 1 write per pixel
        }
    }
}
```

**Severity: HIGH**

For a 256×240 frame:
- 61,440 pixels processed
- At 60fps = 3,686,400 pixels/second
- Each pixel = 2 memory reads + 1 write (the LUT is in L1 cache)

This is ~7.4MB/s of memory traffic — trivial for modern CPUs. But for filters like `HQ2x`, `Super2xSaI`, `NTSC`, the processing is much heavier:
- `HQ2x`: multiple YUV conversions, pattern matching, and blend operations per pixel
- `NTSC`: full composite signal simulation with 7 output pixels per 3 input pixels
- These can take **1-5ms per frame** on a slow CPU

**Recommended fix:** For minimum latency, always use `Filter::None` (no filter). If visual quality is desired, move ALL filtering to GPU shaders. The current CPU filter path is fundamentally at odds with low latency.

---

## 10. SFC Interface and Emulation Core (sfc/interface/)

### 10.1 `emulator->run()` Runs the Full Frame

The `SuperFamicom::Interface::run()` method (not shown in the uploaded files, but implied by the architecture) runs the SNES for a full frame (~357,368 master clock cycles) before returning. During this time:

- No input is polled
- No video output happens
- The CPU is 100% dedicated to emulation

**Severity: LOW (this is correct behavior)**

This is actually the correct model for an emulator. The SNES itself only samples input once per frame (during VBlank, via the joypad read routine in the cartridge). However:

**The issue is when `emulator->run()` is called relative to display timing.** If it's called right after `video.output()` returns (which may have just blocked on vsync), then:

1. VSync occurs at t=0
2. `video.output()` returns at t=0
3. `emulator->run()` starts at t=0, finishes at t=~8ms (assuming 8ms to emulate a frame)
4. `video.output()` on the next frame blocks until t=16.667ms
5. The frame sits ready for ~8ms before being displayed

This means the **input read at the start of `emulator->run()` is processed into a frame that won't be displayed for ~8-16ms**.

**Recommended fix:** Implement **"late frame submission"** or **"just-in-time emulation"**:

```cpp
// Wait until ~2ms before the next vsync, THEN emulate
void lateSubmitFrame() {
    spinUntil(2ms_before_vblank);
    emulator->run();  // emulate as late as possible
    video.present();  // submit immediately, minimal time between emu end and display
}
```

This ensures the input sampled at the start of emulation is displayed as quickly as possible after the emulation finishes.

---

## 11. Settings and Configuration I/O

### 11.1 `settings.save()` Called Synchronously on Video/Audio/Input Driver Init

```cpp
settings.general.crashed = true;
settings.general.crashComponent = "Video";
settings.save();  // writes to disk!
updateVideoDriver(presentation);
```

**Severity: LOW**

`settings.save()` writes to `settings.bml` on disk. This happens 3 times during initialization (once per driver). On a system with slow disk I/O or aggressive antivirus, each write could take 10-50ms.

**Recommended fix:** Defer settings saves to a background thread, or batch them and save only on clean shutdown.

---

## 12. Missing Optimizations (Not Bugs, But Opportunities)

### 12.1 No Use of `timeBeginPeriod(0.5)` or `NtSetTimerResolution`

Windows 10/11 support timer resolutions below 1ms via undocumented APIs. `NtSetTimerResolution` can set granularity as low as **0.5ms**.

**Recommended fix:**
```cpp
// After timeBeginPeriod(1), also call:
HMODULE ntdll = GetModuleHandleA("ntdll.dll");
auto NtSetTimerResolution = (NTSTATUS(*)(ULONG, BOOLEAN, PULONG))
    GetProcAddress(ntdll, "NtSetTimerResolution");
ULONG current;
NtSetTimerResolution(5000, TRUE, &current);  // 0.5ms in 100ns units
```

### 12.2 No Memory Prefetching for Frame Data

The video frame data is read sequentially during filter processing, but there are no `_mm_prefetch` hints.

**Recommended fix:** Add prefetch hints a few scanlines ahead in the filter loops:
```cpp
for(uint y = 0; y < height; y++) {
    _mm_prefetch((const char*)(input + (y + 2) * pitch), _MM_HINT_T0);
    // ... process scanline ...
}
```

### 12.3 No Compiler Hints for Likely/Unlikely Branches

The input polling and audio paths have many branch predictions that could be improved with `[[likely]]` / `[[unlikely]]` (C++20) or compiler intrinsics like `__builtin_expect`.

---

## Summary of Recommendations (Priority Order)

| Priority | Issue | File | Estimated Latency Improvement |
|----------|-------|------|------------------------------|
| **P0** | Main loop blocks on `video.poll()` | `program.cpp` | **5-16ms** |
| **P0** | `video.output()` blocks on VSync | `program/video.cpp` | **5-16ms** |
| **P1** | Input worker thread sleeps 1ms | `input/input.cpp` | **0.5-1ms** |
| **P1** | CPU software filters block frame output | `program/viewport.cpp` | **1-5ms** |
| **P1** | Audio `process()` called synchronously per-sample | `emulator/audio/stream.cpp` | **0.5-2ms** |
| **P2** | `usleep(20ms)` when inactive | `program.cpp` | **Jitter reduction** |
| **P2** | `emulator->save()` on main thread | `program.cpp` | **5-50ms spike elimination** |
| **P2** | `inputPoll()` does linear search | `program/platform.cpp` | **0.1-0.3ms** |
| **P2** | No thread affinity pinning | `program.cpp` | **Jitter reduction** |
| **P3** | Movie playback O(n) input read | `program/platform.cpp` | **Micro-optimization** |
| **P3** | Debug trace kernel traps | `program/video.cpp` | **Minimal** |
| **P3** | No `NtSetTimerResolution` | `program.cpp` | **0.5ms granularity** |

---

## Architectural Recommendations

### A. Implement a "Render Thread" Architecture

Current: Single thread does everything (input → emulate → filter → present → wait for vsync)

Recommended: Three threads with lock-free queues:

```
[Input Thread] ──lock-free──→ [Emulation Thread] ──lock-free──→ [Render Thread]
     1000Hz                        60Hz                         VSync-locked
   Raw Input                    Runs bsnes core              GPU upload + present
```

### B. Use GPU-Only Rendering Path

Current: CPU filters → GPU upload → Present

Recommended: GPU shaders only:

```
SNES frame (256×240) ──GPU texture──→ [Vertex/Fragment Shader scaling] ──→ Present
```

No CPU pixel processing. The SNES frame is uploaded as a raw texture and all scaling is done by the GPU.

### C. Implement "Just-In-Time Emulation"

Instead of: `Emulate → Wait for VSync → Present → Emulate → ...`

Use: `Wait until 2ms before VSync → Emulate → Present immediately`

This minimizes the time between input sampling and photon emission.

### D. Consider DWM Bypass on Windows

For absolute minimum latency on Windows, bypass the Desktop Window Manager entirely:
- Use a **fullscreen exclusive** DirectX path
- Or use `DwmSetWindowAttribute` with `DWMWA_USE_HOST_BACKDROP_BRUSH` to reduce compositor overhead
- On Windows 11, use `DwmSetWindowAttribute` with the new low-latency presentation APIs

---

## Final Notes

The bsnes-fastlink fork has already made excellent strides in latency reduction:
- ✅ Input worker thread at 1000Hz
- ✅ Windows priority boosting
- ✅ `timeBeginPeriod(1)`
- ✅ Native Windows input path
- ✅ Process/thread priority elevation

The remaining latency is primarily in the **synchronization model** between emulation, video output, and display refresh. The single-threaded blocking pipeline is the biggest remaining bottleneck. Addressing the P0 and P1 items above should get you into the **sub-frame latency regime** (input-to-photon in under 8ms), which is where you need to be for sub-6-minute ALTTP runs.
