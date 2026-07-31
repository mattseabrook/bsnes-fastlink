#pragma once

#if defined(PLATFORM_WINDOWS)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {
inline char fastLinkCrashStepBuffer[512] = {};

inline auto fastLinkCrashWrite(const char* reason, DWORD code = 0, void* addr = nullptr, ULONG_PTR i0 = 0, ULONG_PTR i1 = 0) -> void {
}

inline auto fastLinkCrashStep(const char* step) -> void {
}
}

inline auto fastLinkDebug(const char* format, ...) -> void {
}

#else

inline auto fastLinkCrashStep(const char*) -> void {
}

inline auto fastLinkDebug(const char*, ...) -> void {
}

#endif
