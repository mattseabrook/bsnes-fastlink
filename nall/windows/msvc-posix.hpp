#pragma once
//===============================================================================
// nall/windows/msvc-posix.hpp — POSIX compatibility shims for clang-cl / MSVC
// Provides definitions that MinGW supplies but the MSVC CRT does not.
//===============================================================================

#if defined(PLATFORM_WINDOWS) || defined(_WIN32)

#include <io.h>
#include <direct.h>
#include <process.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/utime.h>
#include <stdarg.h>

// ssize_t
#if !defined(_SSIZE_T_DEFINED)
  #define _SSIZE_T_DEFINED
  #if defined(_WIN64)
    using ssize_t = long long;
  #else
    using ssize_t = long;
  #endif
#endif

// access() mode flags
#if !defined(F_OK)
  #define F_OK 0
#endif
#if !defined(R_OK)
  #define R_OK 4
#endif
#if !defined(W_OK)
  #define W_OK 2
#endif
#if !defined(X_OK)
  #define X_OK 0
#endif

// open() flags (MSVC fcntl.h uses _O_ prefix)
#if !defined(O_RDONLY)
  #define O_RDONLY _O_RDONLY
#endif
#if !defined(O_WRONLY)
  #define O_WRONLY _O_WRONLY
#endif
#if !defined(O_RDWR)
  #define O_RDWR _O_RDWR
#endif
#if !defined(O_CREAT)
  #define O_CREAT _O_CREAT
#endif
#if !defined(O_EXCL)
  #define O_EXCL _O_EXCL
#endif
#if !defined(O_TRUNC)
  #define O_TRUNC _O_TRUNC
#endif
#if !defined(O_APPEND)
  #define O_APPEND _O_APPEND
#endif

// stat macros
#if !defined(S_ISREG)
  #define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif
#if !defined(S_ISDIR)
  #define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#if !defined(S_IRUSR)
  #define S_IRUSR _S_IREAD
#endif
#if !defined(S_IWUSR)
  #define S_IWUSR _S_IWRITE
#endif
#if !defined(S_IXUSR)
  #define S_IXUSR 0
#endif
#if !defined(S_IRGRP)
  #define S_IRGRP 0
#endif
#if !defined(S_IWGRP)
  #define S_IWGRP 0
#endif
#if !defined(S_IXGRP)
  #define S_IXGRP 0
#endif
#if !defined(S_IROTH)
  #define S_IROTH 0
#endif
#if !defined(S_IWOTH)
  #define S_IWOTH 0
#endif
#if !defined(S_IXOTH)
  #define S_IXOTH 0
#endif
#if !defined(S_IRWXU)
  #define S_IRWXU (S_IRUSR | S_IWUSR | S_IXUSR)
#endif

// clock_gettime shim using QueryPerformanceCounter
// Note: UCRT provides struct timespec via <time.h>; we only shim clock_gettime()
#include <time.h>
#if !defined(CLOCK_MONOTONIC)
  #define CLOCK_MONOTONIC 1
  #define CLOCK_REALTIME  0

  inline int clock_gettime(int, struct timespec* tp) {
    static const LARGE_INTEGER freq = [] {
      LARGE_INTEGER value;
      QueryPerformanceFrequency(&value);
      return value;
    }();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    tp->tv_sec  = (time_t)(counter.QuadPart / freq.QuadPart);
    tp->tv_nsec = (long)((counter.QuadPart % freq.QuadPart) * 1000000000LL / freq.QuadPart);
    return 0;
  }
#endif

// usleep shim (Sleep is available from <windows.h>)
#if !defined(usleep)
  inline void usleep(unsigned int us) { Sleep(us / 1000 > 0 ? us / 1000 : 1); }
#endif

// POSIX I/O functions — provided as inline wrappers when CRT doesn't expose them
// (UCRT hides them behind #if !__STDC__ which clang-cl sets to 1)
#if defined(__STDC__) && __STDC__
  inline int open(const char* path, int flags, ...) {
    int mode = 0;
    if(flags & _O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    return ::_open(path, flags, mode);
  }
  inline int close(int fd) { return ::_close(fd); }
  inline int read(int fd, void* buf, unsigned count) { return ::_read(fd, buf, count); }
  inline int write(int fd, const void* buf, unsigned count) { return ::_write(fd, buf, count); }
  inline long lseek(int fd, long offset, int origin) { return ::_lseek(fd, offset, origin); }
#endif

// ftruncate
#if !defined(ftruncate)
  #define ftruncate _chsize
#endif

// fileno
#if !defined(fileno)
  #define fileno _fileno
#endif

// getpid
#if !defined(getpid)
  #define getpid _getpid
#endif

#endif //PLATFORM_WINDOWS
