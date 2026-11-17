Build Configuration
===================

bsnes' build system supports many different configuration options,
allowing you to customise the build to your needs.
These options are generally provided on the command-line
when building the software.
For example,
to use the `build=debug` option to make a debug build,
put it on the command-line when building, like this:

    make -C bsnes build=debug

**Note:** The build system does not do a good job of detecting
when configuration options have changed,
so before you build with a new configuration option,
do a `make clean` first.

Build targets
-------------

  - `target=bsnes` `binary=application`
    builds the standalone bsnes application
  - `target=libretro` `binary=library`
    builds the Libretro core

Build variations
----------------

  - `build=debug` makes a build with optimization disabled (`-Og`),
    debug symbols,
    and additional debug checks
  - `build=stable` makes a build with little optimization (`-O1`)
  - `build=size` makes the smallest executable it can (`-Os`)
  - `build=release` makes a build with sensible optimizations (`-O2`)
  - `build=performance` (**default**)
    makes a build with all the optimizations (`-O3`)
  - `lto=true` makes a build with link-time-optimization enabled
    for maximum performance
  - `lto=false` (**default**) makes a build with
    link-time-optimization disabled
    for quick compile times
  - `openmp=true` (**default**, but not available on macOS)
    enables OpenMP for easy parellelism of things like HD Mode 7
  - `openmp=false` disables OpenMP
  - `local=true` (**default** for `target=bsnes`)
    makes a build optimized for the specific CPU in your machine;
    running such a build on other computers
    might cause "illegal instruction" errors
  - `local=false` makes a build that is likely to run
    on any computer compatible with the one in your machine

Toolchain settings
------------------

  - `compiler` should be set to a command that runs a C++ compiler
    (like `g++` or `clang++`),
    used for linking and as the default for the following options,
    default varies per platform
  - `compiler.c` should be set to a command that runs a C compiler,
    defaults to `compiler` plus flags to set the language to C
  - `compiler.cpp` should be set to a command that runs a C++ compiler,
    defaults to `compiler` plus flags to set the language to C++
  - `compiler.objc` should be set to a command that runs
    an Objective C compiler,
    defaults to `compiler` plus flags to set the language to Objective C
  - `compiler.objcpp` should be set to a command that runs
    an Objective C++ compiler,
    defaults to `compiler` plus flags to set the language to Objective C++

Windows x64 application
-----------------------

The application uses the Win32 API directly and is built with `./build.sh`.
There is no selectable GUI toolkit. The retained core-only build is separate
from the Windows application.
