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

// Last-operation marker only: no log file, worker, timer, or disk activity.
inline thread_local const char* fastLinkFaultStage="startup";
inline LONG WINAPI fastLinkReportException(EXCEPTION_POINTERS* fault) {
  static LONG reporting=0;
  if(InterlockedExchange(&reporting,1))return EXCEPTION_EXECUTE_HANDLER;
  HMODULE module{};wchar_t modulePath[MAX_PATH]=L"unknown module";
  void* address=fault->ExceptionRecord->ExceptionAddress;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
    (LPCWSTR)address,&module);
  if(module)GetModuleFileNameW(module,modulePath,MAX_PATH);
  wchar_t message[1024]{};
  _snwprintf_s(message,1024,_TRUNCATE,
    L"bsnes-fastlink %hs\nException: 0x%08lX\nModule: %ls\nOffset: 0x%llX\nLast operation: %hs\n\nPress Ctrl+C to copy this report.",
    FASTLINK_VERSION_STRING,fault->ExceptionRecord->ExceptionCode,modulePath,
    (unsigned long long)((uintptr_t)address-(uintptr_t)module),fastLinkFaultStage);
  MessageBoxW(nullptr,message,L"bsnes-fastlink crash",MB_OK|MB_ICONERROR|MB_SETFOREGROUND);
  return EXCEPTION_EXECUTE_HANDLER;
}

namespace {
inline char fastLinkCrashStepBuffer[512] = {};

inline auto fastLinkCrashWrite(const char* reason, DWORD code = 0, void* addr = nullptr, ULONG_PTR i0 = 0, ULONG_PTR i1 = 0) -> void {
}

inline auto fastLinkCrashStep(const char* step) -> void {
  fastLinkFaultStage=step;
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
