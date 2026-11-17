#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#if !defined(_WIN32) || !defined(_WIN64)
#error bsnes-fastlink requires Windows x64
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0a00
#endif
#include <ruby/ruby.hpp>
// Keep SDK aliases inside the compatibility guard, including headers that
// need the SDK's interface/far/near macros. Core headers see only nall types.
#include <nall/windows/guard.hpp>
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <mmsystem.h>
#include <nall/windows/guard.hpp>
#include <emulator/emulator.hpp>
#include <filter/filter.hpp>
#include <lzma/lzma.hpp>
#include <nall/decode/rle.hpp>
#include <nall/decode/zip.hpp>
#include <nall/encode/rle.hpp>
#include <nall/encode/zip.hpp>
#include <nall/hash/crc16.hpp>
#include <nall/path.hpp>
#include <nall/location.hpp>
#include "video/video.hpp"
#include "fastlink/debug.hpp"
#include "fastlink/ram-trace.hpp"
#include "settings/settings.hpp"
#include "input/input.hpp"
#include "native-ui.hpp"
#include "program/program.hpp"
#include "resource/resource.hpp"
extern NativeVideoManager video;
extern ruby::Audio audio;
extern unique_pointer<Emulator::Interface> emulator;
auto locate(string name) -> string;
