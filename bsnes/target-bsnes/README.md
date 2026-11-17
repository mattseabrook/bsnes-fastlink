Windows x64 frontend
====================

The application uses Win32 windows, menus, controls, dialogs, and its own
message loop. Build it from the repository root with `./build.sh`.

`native-ui.cpp` owns the windows and command handlers. `program/` connects the
native frontend to the emulator and preserves cartridge/save formats.
`video/native-vulkan.cpp` uses Vulkan C API handles and functions directly.
`input/native-win32.cpp` runs the dedicated Raw Input worker.
`settings/` stores configuration without creating UI objects.
