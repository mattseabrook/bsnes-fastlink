#ifndef NALL_WINDOWS_GUARD_HPP
#define NALL_WINDOWS_GUARD_HPP

#define boolean WindowsBoolean
//clang-cl/MSVC: COM headers need interface=struct, SDK needs far/near=nothing
#undef interface
#define interface struct
#undef far
#undef near
#define far
#define near

#undef UNICODE
#undef WIN32_LEAN_AND_MEAN
#undef NOMINMAX
#undef PATH_MAX

#define UNICODE
#ifndef WINVER
#ifdef _WIN32_WINNT
#define WINVER _WIN32_WINNT
#else
#define WINVER 0x0601
#endif
#endif
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT WINVER
#endif
#define NOMINMAX
#define PATH_MAX 260

#else
#undef NALL_WINDOWS_GUARD_HPP

#undef boolean
#undef interface
#undef far
#undef near

#endif
