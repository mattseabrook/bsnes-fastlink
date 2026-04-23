#!/usr/bin/env bash
set -euo pipefail

#===============================================================================
# bsnes-fastlink — Linux -> Windows cross build dependency builder
# - Builds missing dependencies (zlib, libpng) as static Windows libraries
# - Auto-detects Windows SDK layout (xwin-style or traditional)
# - Provides SDK setup/test helpers
#
# Usage: ./build_windows_libs.sh [setup|test|zlib|libpng|all]
#   setup    - detect and verify Windows SDK
#   test     - test cross-compilation
#   zlib     - build zlib static library
#   libpng   - build libpng static library
#   all      - setup + build all libraries
#===============================================================================

# ----------------------------- Pretty output ---------------------------------
if [[ -t 1 ]] && command -v tput >/dev/null 2>&1 && [[ $(tput colors 2>/dev/null || echo 0) -ge 8 ]]; then
  C0=$'\033[0m'; CB=$'\033[1m'; CR=$'\033[31m'; CG=$'\033[32m'; CY=$'\033[33m'; CC=$'\033[36m'
else
  C0=""; CB=""; CR=""; CG=""; CY=""; CC=""
fi

banner(){ local w=78; local line; line="$(printf '%*s' "$w" | tr ' ' '=')"
  printf "\n${CB}${CC}%s\n  %s\n%s${C0}\n\n" "$line" "$1" "$line"
}
info(){ printf "${CY}→ %s${C0}\n" "$*"; }
ok(){ printf "${CG}✓ %s${C0}\n" "$*"; }
die(){ printf "${CR}✗ %s${C0}\n" "$*" >&2; exit 1; }

# ------------------------------ Config ---------------------------------------
TARGET_TRIPLE="${TARGET_TRIPLE:-x86_64-pc-windows-msvc}"
WINSDK_BASE="${WINSDK_BASE:-/opt/winsdk}"
BUILD_DIR="${BUILD_DIR:-/tmp/bsnes-deps-build}"
LIBS_PREFIX="${LIBS_PREFIX:-/opt/windows-libs}"
ZLIB_DIR="${ZLIB_DIR:-$LIBS_PREFIX/zlib}"
LIBPNG_DIR="${LIBPNG_DIR:-$LIBS_PREFIX/libpng}"
CL="${CL:-clang-cl}"
LINKER="${LINKER:-lld-link}"
JOBS="${JOBS:-$(nproc)}"

# ---------------------- Windows SDK detection --------------------------------
detect_winsdk() {
  [[ -d "$WINSDK_BASE" ]] || die "WINSDK_BASE not found: $WINSDK_BASE (run 'xwin splat' first)"

  local incA="" libA=""

  if [[ -d "$WINSDK_BASE/sdk/include" ]]; then
    incA="$WINSDK_BASE/sdk/include"
  elif [[ -d "$WINSDK_BASE/Include" ]]; then
    incA="$WINSDK_BASE/Include"
  fi

  if [[ -d "$WINSDK_BASE/sdk/lib" ]]; then
    libA="$WINSDK_BASE/sdk/lib"
  elif [[ -d "$WINSDK_BASE/Lib" ]]; then
    libA="$WINSDK_BASE/Lib"
  fi

  [[ -n "$incA" && -n "$libA" ]] || die "Could not detect SDK include/lib roots under $WINSDK_BASE"

  local ver=""
  if compgen -G "$incA/*" >/dev/null; then
    ver="$(ls -1 "$incA" 2>/dev/null | grep -E '^10\.' | head -n 1 || true)"
  fi

  export SDK_INC_ROOT="$incA"
  export SDK_LIB_ROOT="$libA"
  export SDK_VER="$ver"
}

sdk_flags() {
  local inc_um inc_shared inc_ucrt lib_um lib_ucrt crt_inc crt_lib

  crt_inc="$WINSDK_BASE/crt/include"
  crt_lib="$WINSDK_BASE/crt/lib"

  if [[ -n "$SDK_VER" && -d "$SDK_INC_ROOT/$SDK_VER/um" ]]; then
    inc_um="$SDK_INC_ROOT/$SDK_VER/um"
    inc_shared="$SDK_INC_ROOT/$SDK_VER/shared"
    inc_ucrt="$SDK_INC_ROOT/$SDK_VER/ucrt"
    if [[ -d "$SDK_LIB_ROOT/um/x86_64" ]]; then
      lib_um="$SDK_LIB_ROOT/um/x86_64"
      lib_ucrt="$SDK_LIB_ROOT/ucrt/x86_64"
    else
      lib_um="$SDK_LIB_ROOT/$SDK_VER/um/x64"
      lib_ucrt="$SDK_LIB_ROOT/$SDK_VER/ucrt/x64"
    fi
  else
    inc_um="$SDK_INC_ROOT/um"
    inc_shared="$SDK_INC_ROOT/shared"
    inc_ucrt="$SDK_INC_ROOT/ucrt"
    lib_um="$SDK_LIB_ROOT/um/x86_64"
    lib_ucrt="$SDK_LIB_ROOT/ucrt/x86_64"
  fi

  [[ -d "$inc_um" && -d "$inc_shared" && -d "$inc_ucrt" ]] || die "Missing SDK include dirs"
  [[ -d "$lib_um" && -d "$lib_ucrt" ]] || die "Missing SDK lib dirs"

  export CRT_INC="$crt_inc"
  export CRT_LIB="$crt_lib"
  export INC_UM="$inc_um"
  export INC_SHARED="$inc_shared"
  export INC_UCRT="$inc_ucrt"
  export LIB_UM="$lib_um"
  export LIB_UCRT="$lib_ucrt"
}

# ---------------------- CMake toolchain for deps -----------------------------
create_toolchain_file() {
  local outdir="$1"
  local cmake_inc_flags=""
  [[ -d "$CRT_INC" ]] && cmake_inc_flags+="-imsvc $CRT_INC "
  [[ -d "$INC_UCRT" ]] && cmake_inc_flags+="-imsvc $INC_UCRT "
  [[ -d "$INC_UM" ]] && cmake_inc_flags+="-imsvc $INC_UM "
  [[ -d "$INC_SHARED" ]] && cmake_inc_flags+="-imsvc $INC_SHARED "

  local cmake_lib_flags=""
  [[ -d "$CRT_LIB/x86_64" ]] && cmake_lib_flags+="-libpath:$CRT_LIB/x86_64 "
  [[ -d "$LIB_UM" ]] && cmake_lib_flags+="-libpath:$LIB_UM "
  [[ -d "$LIB_UCRT" ]] && cmake_lib_flags+="-libpath:$LIB_UCRT "

  cat > "$outdir/windows-cross.cmake" << EOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_CXX_COMPILER_WORKS 1)
set(CMAKE_AR llvm-lib)
set(CMAKE_C_ARCHIVE_CREATE "<CMAKE_AR> /OUT:<TARGET> <OBJECTS>")
set(CMAKE_C_ARCHIVE_FINISH "")
set(CMAKE_CXX_ARCHIVE_CREATE "<CMAKE_AR> /OUT:<TARGET> <OBJECTS>")
set(CMAKE_CXX_ARCHIVE_FINISH "")
set(CMAKE_C_FLAGS_INIT "-fuse-ld=lld-link $cmake_inc_flags /MT -D_WIN32 -D_WIN64 -fms-compatibility")
set(CMAKE_CXX_FLAGS_INIT "-fuse-ld=lld-link $cmake_inc_flags /MT -D_WIN32 -D_WIN64 -fms-compatibility")
set(CMAKE_C_FLAGS_RELEASE_INIT "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-O2 -DNDEBUG")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
set(CMAKE_EXE_LINKER_FLAGS_INIT "$cmake_lib_flags -DEFAULTLIB:libcmt.lib -NODEFAULTLIB:msvcrt.lib")
set(CMAKE_FIND_ROOT_PATH "$WINSDK_BASE" "$LIBS_PREFIX")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF
}

# ---------------------- Dependency builders ----------------------------------
build_zlib() {
  banner "Building zlib"
  mkdir -p "$BUILD_DIR" && cd "$BUILD_DIR"

  if [[ ! -d "zlib-1.3.1" ]]; then
    info "Downloading zlib 1.3.1..."
    curl -sL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz | tar xz
  fi

  cd zlib-1.3.1
  rm -rf build_win && mkdir build_win && cd build_win
  create_toolchain_file "$(pwd)"

  cmake .. -DCMAKE_TOOLCHAIN_FILE=windows-cross.cmake -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$ZLIB_DIR" -DBUILD_SHARED_LIBS=OFF >/dev/null

  make zlibstatic -j"$JOBS" >/dev/null

  mkdir -p "$ZLIB_DIR/lib" "$ZLIB_DIR/include"
  cp zlibstatic.lib "$ZLIB_DIR/lib/zlib.lib"
  cp ../zlib.h zconf.h "$ZLIB_DIR/include/"
  ok "zlib installed to $ZLIB_DIR"
}

build_libpng() {
  banner "Building libpng"
  mkdir -p "$BUILD_DIR" && cd "$BUILD_DIR"

  if [[ ! -d "libpng-1.6.50" ]]; then
    info "Downloading libpng 1.6.50..."
    curl -sL https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.50.tar.gz | tar xz
  fi

  cd libpng-1.6.50

  if [[ -f "projects/vstudio/pnglibconf.h" ]]; then
    cp projects/vstudio/pnglibconf.h .
  else
    cp scripts/pnglibconf.h.prebuilt pnglibconf.h
  fi

  rm -rf build_win && mkdir build_win && cd build_win

  local cflags=(
    "--target=$TARGET_TRIPLE" "/nologo" "/c" "/O2" "/MT" "/DNDEBUG"
    "/DPNG_STATIC" "/DZLIB_STATIC" "/DPNG_NO_MMX_CODE"
    "-I.." "-I$ZLIB_DIR/include"
    "/I$CRT_INC" "/I$INC_UCRT" "/I$INC_UM" "/I$INC_SHARED"
  )

  local srcs=(png pngerror pngget pngmem pngpread pngread pngrio pngrtran
              pngrutil pngset pngtrans pngwio pngwrite pngwtran pngwutil)
  local objs=()

  for s in "${srcs[@]}"; do
    "$CL" "${cflags[@]}" "../$s.c" "/Fo$s.obj" 2>/dev/null && objs+=("$s.obj")
  done

  llvm-lib "/OUT:libpng.lib" "${objs[@]}" >/dev/null

  mkdir -p "$LIBPNG_DIR/lib" "$LIBPNG_DIR/include"
  cp libpng.lib "$LIBPNG_DIR/lib/libpng.lib"
  cp ../png.h ../pngconf.h ../pnglibconf.h "$LIBPNG_DIR/include/"
  ok "libpng installed to $LIBPNG_DIR"
}

# ---------------------- SDK setup ----------------------------------------
setup_winsdk() {
  banner "Windows SDK Setup"

  if [[ ! -d "$WINSDK_BASE" ]]; then
    info "Installing Windows SDK via xwin..."
    command -v xwin >/dev/null 2>&1 || die "xwin not found. Install with: cargo install xwin"
    mkdir -p "$WINSDK_BASE"
    xwin --accept-license splat --output "$WINSDK_BASE"
  fi

  detect_winsdk
  sdk_flags

  ok "SDK detected at $WINSDK_BASE"
  info "  SDK includes: $SDK_INC_ROOT"
  info "  SDK libs:     $SDK_LIB_ROOT"
  info "  SDK version:  ${SDK_VER:-<flat layout>}"
  info "  CRT includes: $CRT_INC"
  info "  CRT libs:     $CRT_LIB"
}

# ---------------------- Cross-compile test --------------------------------
test_cross() {
  banner "Testing Cross-Compilation"

  detect_winsdk
  sdk_flags

  local tmpdir
  tmpdir=$(mktemp -d)
  cat > "$tmpdir/test.c" << 'EOF'
#include <windows.h>
#include <stdio.h>
int main() { printf("Hello from cross-compiled Windows binary!\n"); return 0; }
EOF

  info "Compiling test program..."
  "$CL" --target="$TARGET_TRIPLE" /nologo /MT \
    "/I$CRT_INC" "/I$INC_UCRT" "/I$INC_UM" "/I$INC_SHARED" \
    -fuse-ld="$LINKER" \
    "$tmpdir/test.c" \
    /link /nologo \
    "/libpath:$CRT_LIB/x86_64" "/libpath:$LIB_UM" "/libpath:$LIB_UCRT" \
    /out:"$tmpdir/test.exe" \
    kernel32.lib user32.lib libcmt.lib libucrt.lib \
    /NODEFAULTLIB:msvcrt.lib 2>&1

  if [[ -f "$tmpdir/test.exe" ]]; then
    ok "Cross-compilation test passed ($(du -h "$tmpdir/test.exe" | cut -f1))"
    file "$tmpdir/test.exe"
  else
    die "Cross-compilation test failed"
  fi

  rm -rf "$tmpdir"
}

# ---------------------- Usage --------------------------------------------
usage() {
  cat <<EOF
Usage: $0 [setup|test|zlib|libpng|all]

Commands:
  setup          Detect and verify Windows SDK
  test           Test cross-compilation
  zlib           Build zlib static library
  libpng         Build libpng static library (requires zlib)
  all            Setup + build all libraries

Environment variables:
  WINSDK_BASE=/opt/winsdk          Windows SDK location
  LIBS_PREFIX=/opt/windows-libs    Where to install dependency libraries
  TARGET_TRIPLE=x86_64-pc-windows-msvc
  CL=clang-cl                      C/C++ compiler
  LINKER=lld-link                  Linker
  JOBS=N                           Parallel jobs (default: nproc)
EOF
}

# ---------------------- Main -------------------------------------------
main() {
  local cmd="${1:-help}"

  case "$cmd" in
    setup)    setup_winsdk ;;
    test)     test_cross ;;
    zlib)
      detect_winsdk; sdk_flags
      build_zlib
      ;;
    libpng)
      detect_winsdk; sdk_flags
      build_libpng
      ;;
    all)
      setup_winsdk
      build_zlib
      build_libpng
      ok "All dependencies built under $LIBS_PREFIX"
      ;;
    -h|--help|help) usage ;;
    *) usage; exit 1 ;;
  esac
}

main "$@"
