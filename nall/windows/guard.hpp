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
#undef WINVER
#undef WIN32_LEAN_AND_MEAN
#undef _WIN32_WINNT
#undef NOMINMAX
#undef PATH_MAX

#define UNICODE
#define WINVER 0x0601
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT WINVER
#define NOMINMAX
#define PATH_MAX 260

#else
#undef NALL_WINDOWS_GUARD_HPP

#undef boolean
#undef interface
#undef far
#undef near

#endif
