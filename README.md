bsnes-fastlink
==============

![bsnes logo](bsnes/target-bsnes/resource/logo.png)

bsnes-fastlink is a Windows/Vulkan-focused, accuracy-only fork of bsnes for
speedrun practice. Frontend latency is reduced without speculative execution,
overclocking, compatibility hacks, or alternate emulation engines.

This fork is based on bsnes, originally developed by Near.

Latest FastLink Changes
-----------------------

The `experimental-core` branch has one emulation path: stock CPU and
coprocessor clocks, timed 5A22 math, cycle-rendered PPU with hardware limits,
Gaussian DSP, continuous coprocessor synchronization, and low-level
enhancement-chip emulation. The former fast/HD PPU, run-ahead, compatibility
hotfixes, inaccurate DSP/math modes, overclocking, and HLE fallback engines
have been removed from the build and source tree. See the
[core accuracy audit](docs/core-accuracy-audit.md) for the source comparison,
OpenSFC hardware-boundary review, known gaps, changes, and validation results.

The latest play-testing changes from `Play-testing v1` center the Windows build
around the low-latency path:

  - Added the native `Win32 Vulkan` video backend and made it the default on
    Windows.
  - Added selectable Vulkan present behavior (`Immediate`, `Mailbox`, `FIFO`)
    and configurable swapchain image count for queue-depth control.
  - Added the native `Win32 Raw Input` backend with a dedicated high-priority
    input thread, atomic controller state, and RetroUSB controller
    mapping.
  - Added the FastLink RAM Trace localhost HTTP service on `127.0.0.1:9000`,
    serving 128 KiB WRAM snapshots with CORS headers for the Spectra prototype.
  - Updated WASAPI defaults for shared-mode, nonblocking, 48000 Hz output so
    other programs can keep using the sound device.
  - Added FastLink configuration keys for RAM Trace, non-Windows input polling,
    thread affinity, timer resolution, software frame pacing, and low-latency
    defaults.
  - Disabled shader/software-filter defaults on the Windows latency path to avoid
    extra frame work.
  - Removed the old Direct3D 9 runtime path from this fork's Windows build.

Accuracy-Only Core
------------------

Core timing and behavior are not user-selectable. Obsolete hack keys remaining
in old configuration files are inert, and libretro no longer advertises them.
Enhancement-chip firmware must be supplied wherever the
low-level implementation requires it; there is no high-level fallback.

Frame pacing occurs immediately before core execution so the event-driven
Win32 Raw Input thread can publish controller changes until the latest safe
point before the SNES polls its controller ports.

The native Vulkan driver waits for upload ownership and a swapchain image
before core execution rather than dropping a newly emulated frame. Its true
fullscreen path requests `VK_EXT_full_screen_exclusive` when supported, with a
borderless fullscreen fallback on drivers that do not expose the extension.

RAM Trace HTTP API
------------------

RAM Trace is controlled by `FastLink/RAMTraceServer` in the config. When
enabled, the service listens on localhost port `9000` and returns a raw 128 KiB
WRAM snapshot suitable for local tools such as
`research/snes9x-fastlink/fastlink-spectra/prototype.html`.

The service uses a separate low-priority thread, short socket timeouts,
`TCP_NODELAY`, and triple-buffered snapshots so local HTTP reads do not stall the
emulation frame path. Snapshot requests are coalesced and synchronized to
emulation-frame publication, preventing an unpaced client from repeatedly
transmitting stale 128 KiB snapshots. With the server enabled but no request
pending, the emulation-frame path performs only two atomic checks; with the
server disabled, it performs one. The only requested-frame work on the
emulation thread is a bounded 128 KiB copy into a preallocated snapshot slot;
socket I/O and the second copy into the response buffer run on an idle-priority
worker.

Windows Defaults
----------------

On Windows, this fork normalizes old or unsafe settings back to the FastLink
runtime path:

  - Video driver: `Win32 Vulkan`
  - Video present mode: `Mailbox` by default when available
  - Video swapchain images: configurable from 2-4; Mailbox automatically
    requests at least three images so one render target remains free
  - Audio driver: WASAPI when available
  - Audio exclusive mode: disabled for WASAPI shared-device compatibility
  - Audio blocking: disabled
  - Native file dialogs: enabled
  - Cycle-rendered PPU: the only compiled PPU
  - Software filters: disabled by default on the latency path

Unique Features
---------------

  - True Super Game Boy emulation (using the SameBoy core by Lior Halphon)
  - Low-level enhancement-chip emulation when required firmware is available
  - Multi-threaded PPU graphics renderer
  - Speed mode settings which retain smooth audio output (50%, 75%, 100%, 150%, 200%)
  - Built-in games database with thousands of game entries
  - Built-in cheat code database for hundreds of popular games (by mightymo)
  - Built-in save state manager with screenshot previews and naming capabilities
  - Customizable per-byte game mappings to support any cartridges, including prototype games
  - 7-zip decompression support
  - Extensive Satellaview emulation, including BS Memory flash write and wear-leveling emulation
  - Optional higan game folder support (standard game ROM files are also fully supported!)
  - Advanced mapping system allowing multiple bindings to every emulated input

Standard Features
-----------------

  - MSU1 support
  - BPS and IPS soft-patching support
  - Save states with undo and redo support (for reverting accidental saves and loads)
  - OpenGL multi-pass pixel shaders
  - Several built-in software filters, including HQ2x (by MaxSt) and snes_ntsc (by blargg)
  - Adaptive sync and dynamic rate control for perfect audio/video synchronization
  - Just-in-time input polling for minimal input latency
  - Vulkan-focused low-latency video output on Windows
  - Shared-mode WASAPI audio path for low-latency playback while other programs keep audio access
  - Controller-only Win32 Raw Input: USB HID joystick/gamepad collections; no keyboard, mouse, hotkey polling, or legacy mapping worker
  - Selectable unknown-device HID mapping via `FastLink/GenericHIDMapping`
  - Localhost RAM Trace HTTP service for external practice tooling
  - Periodic auto-saving of game saves
  - Frame advance support
  - Screenshot support
  - Cheat code search support
  - HiDPI support
  - Multi-monitor support
  - Turbo support for controller inputs

Build
-----

The Windows build is produced from Linux with:

```sh
./build_windows.sh build
```

The build script writes `build_win/bsnes.exe` and copies the result to
`/mnt/PhantomIDE/bsnes.exe` by default.

Links
-----

  - [Official git repository](https://github.com/bsnes-emu/bsnes)
  - [Official Discord](https://discord.gg/B27hf27ZVf)

Preview
-------

![bsnes user interface](.assets/user-interface.png)
![bsnes running Bahamut Lagoon](.assets/bahamut-lagoon.png)
![bsnes running Tengai Makyou Zero](.assets/tengai-makyou-zero.png)
