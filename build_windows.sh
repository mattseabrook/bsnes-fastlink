#!/usr/bin/env bash
set -euo pipefail

#===============================================================================
# bsnes-fastlink — Linux -> Windows cross build (clang-cl + lld-link)
# - Targets x86_64-pc-windows-msvc via Arch Linux cross toolchain
# - Uses xwin Windows SDK at /opt/winsdk
# - Builds bsnes.exe for Windows
#
# Usage: ./build_windows.sh [build|clean|help]
#   build (default) - build bsnes.exe
#   clean           - remove build artifacts
#   help            - show usage
#
# Prerequisites:
#   - clang, lld, llvm-rc (pacman -S clang lld llvm)
#   - xwin Windows SDK at /opt/winsdk (cargo install xwin && xwin splat --output /opt/winsdk)
#   - Static deps at /opt/windows-libs (zlib, libpng via build_windows_libs.sh)
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
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR"
cd "$ROOT_DIR"

TARGET_TRIPLE="${TARGET_TRIPLE:-x86_64-pc-windows-msvc}"
CONFIG="${CONFIG:-performance}"
BUILD_DIR="${BUILD_DIR:-build_win}"
WINSDK_BASE="${WINSDK_BASE:-/opt/winsdk}"
LIBS_PREFIX="${LIBS_PREFIX:-/opt/windows-libs}"
ZLIB_DIR="${ZLIB_DIR:-$LIBS_PREFIX/zlib}"
LIBPNG_DIR="${LIBPNG_DIR:-$LIBS_PREFIX/libpng}"
CL="${CL:-clang-cl}"
LINKER="${LINKER:-lld-link}"
JOBS="${JOBS:-$(nproc)}"
MNT_COPY_DIR="${MNT_COPY_DIR:-/mnt/PhantomIDE}"

# SameBoy version
GB_VERSION="$(cat bsnes/gb/version.mk 2>/dev/null | grep -oP 'VERSION\s*:=\s*\K.*' || echo '1.0.2')"

# ---------------------- Windows SDK detection --------------------------------
detect_winsdk() {
  [[ -d "$WINSDK_BASE" ]] || die "WINSDK_BASE not found: $WINSDK_BASE (run 'xwin --accept-license splat --output $WINSDK_BASE' first)"

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
  if compgen -G "$incA/*" >/dev/null 2>/dev/null; then
    ver="$(ls -1 "$incA" 2>/dev/null | grep -E '^10\.' | head -n 1 || true)"
  fi

  export SDK_INC_ROOT="$incA"
  export SDK_LIB_ROOT="$libA"
  export SDK_VER="$ver"
}

sdk_paths() {
  local crt_inc="$WINSDK_BASE/crt/include"
  local crt_lib="$WINSDK_BASE/crt/lib"

  if [[ -n "$SDK_VER" && -d "$SDK_INC_ROOT/$SDK_VER/um" ]]; then
    INC_UM="$SDK_INC_ROOT/$SDK_VER/um"
    INC_SHARED="$SDK_INC_ROOT/$SDK_VER/shared"
    INC_UCRT="$SDK_INC_ROOT/$SDK_VER/ucrt"
    if [[ -d "$SDK_LIB_ROOT/um/x86_64" ]]; then
      LIB_UM="$SDK_LIB_ROOT/um/x86_64"
      LIB_UCRT="$SDK_LIB_ROOT/ucrt/x86_64"
    else
      LIB_UM="$SDK_LIB_ROOT/$SDK_VER/um/x64"
      LIB_UCRT="$SDK_LIB_ROOT/$SDK_VER/ucrt/x64"
    fi
  else
    INC_UM="$SDK_INC_ROOT/um"
    INC_SHARED="$SDK_INC_ROOT/shared"
    INC_UCRT="$SDK_INC_ROOT/ucrt"
    LIB_UM="$SDK_LIB_ROOT/um/x86_64"
    LIB_UCRT="$SDK_LIB_ROOT/ucrt/x86_64"
  fi

  [[ -d "$INC_UM" && -d "$INC_SHARED" && -d "$INC_UCRT" ]] || die "Missing SDK include dirs"
  [[ -d "$LIB_UM" && -d "$LIB_UCRT" ]] || die "Missing SDK lib dirs"

  export CRT_INC="$crt_inc"
  export CRT_LIB="$crt_lib"
  export INC_UM INC_SHARED INC_UCRT LIB_UM LIB_UCRT

  ok "Windows SDK detected"
  info "  CRT include: $CRT_INC"
  info "  SDK version: ${SDK_VER:-direct}"
}

# ---------------------- Compiler/linker flags ---------------------------------
setup_flags() {
  # Optimization
  case "$CONFIG" in
    debug)       OPT=("-Od" "-DBUILD_DEBUG") ;;
    stable)      OPT=("-O1" "-DBUILD_STABLE") ;;
    size)        OPT=("-Os" "-DBUILD_SIZE") ;;
    release)     OPT=("-O2" "-DBUILD_RELEASE") ;;
    performance) OPT=("-O2" "-Oi" "-DBUILD_PERFORMANCE") ;;
    *) die "Unknown CONFIG: $CONFIG" ;;
  esac

  # Vulkan headers: copy from system to avoid pulling in Linux glibc
  local vulkan_inc="$BUILD_DIR/vulkan-include"
  if [[ -d /usr/include/vulkan ]]; then
    mkdir -p "$vulkan_inc"
    cp -ru /usr/include/vulkan "$vulkan_inc/" 2>/dev/null || true
    if [[ -d /usr/include/vk_video ]]; then
      cp -ru /usr/include/vk_video "$vulkan_inc/" 2>/dev/null || true
    fi
  fi

  # Regenerate vulkan-1.lib from the current source references.
  if command -v llvm-dlltool >/dev/null 2>&1; then
    local vk_def="$BUILD_DIR/vulkan-1.def"
    echo "LIBRARY vulkan-1.dll" > "$vk_def"
    echo "EXPORTS" >> "$vk_def"
    grep -roh 'vk[A-Z][0-9A-Za-z]*' ruby/video/vulkan.cpp 2>/dev/null | sort -u | grep -v 'vkCreateXlib' >> "$vk_def"
    llvm-dlltool -m i386:x86-64 -d "$vk_def" -l "$BUILD_DIR/vulkan-1.lib"
  fi

  # Include paths
  INC=(
    "-I." "-I./bsnes"
    "-I$ZLIB_DIR/include"
    "-I$LIBPNG_DIR/include"
    "-I$vulkan_inc"
  )

  # MSVC SDK includes
  MSVC_INC=(
    "/I$CRT_INC" "/I$INC_UCRT" "/I$INC_UM" "/I$INC_SHARED"
  )

  # Defines
  DEFS=(
    "-DNDEBUG"
    # Windows platform (auto-detected via _WIN32, but be explicit)
    "-DUNICODE" "-D_UNICODE"
    "-D_CRT_SECURE_NO_WARNINGS"
    "-D_CRT_NONSTDC_NO_WARNINGS"
    "-D_USE_MATH_DEFINES"
    # hiro: Windows GUI backend
    "-DHIRO_WINDOWS"
    # SameBoy
    "-DGB_INTERNAL" "-DGB_DISABLE_DEBUGGER" "-DGB_DISABLE_CHEATS"
    "-DGB_VERSION=\"$GB_VERSION\""
    # Ruby video driver
    "-DVIDEO_VULKAN"
    "-DVK_USE_PLATFORM_WIN32_KHR"
    # Ruby audio drivers
    "-DAUDIO_WASAPI"
    "-DAUDIO_XAUDIO2"
    "-DAUDIO_DIRECTSOUND"
    "-DAUDIO_WAVEOUT"
    # Ruby input driver
    "-DINPUT_WINDOWS"
  )

  # Base compiler flags
  CFLAGS=(
    "--target=$TARGET_TRIPLE"
    "/nologo"
    "/std:c++latest"
    "/EHsc"
    "/MT"
    "-fuse-ld=$LINKER"
    "-Wno-everything"
    "${OPT[@]}"
    "${DEFS[@]}"
    "${INC[@]}"
    "${MSVC_INC[@]}"
  )

  # C-specific flags (for libco, SameBoy)
  CFLAGS_C=(
    "--target=$TARGET_TRIPLE"
    "/nologo"
    "/MT"
    "-fuse-ld=$LINKER"
    "-Wno-everything"
    "${OPT[@]}"
    "${DEFS[@]}"
    "${INC[@]}"
    "${MSVC_INC[@]}"
  )

  # Linker libs
  LINK_LIBS=(
    "/libpath:$CRT_LIB/x86_64"
    "/libpath:$LIB_UCRT"
    "/libpath:$LIB_UM"
    "kernel32.lib" "user32.lib" "gdi32.lib" "comdlg32.lib" "advapi32.lib"
    "shell32.lib" "ole32.lib" "oleaut32.lib" "winmm.lib" "shlwapi.lib"
    "dxguid.lib" "dsound.lib"
    "opengl32.lib" "hid.lib"
    "comctl32.lib" "dwmapi.lib" "ws2_32.lib" "avrt.lib"
    "uxtheme.lib" "msimg32.lib"
    "$BUILD_DIR/vulkan-1.lib"
    "$ZLIB_DIR/lib/zlib.lib"
    "$LIBPNG_DIR/lib/libpng.lib"
  )
}

# ---------------------- Resource compilation ----------------------------------
compile_resources() {
  local objdir="$BUILD_DIR/obj"

  # bsnes application resource (icon + manifest)
  local rc_dir="bsnes/target-bsnes/resource"
  if [[ -f "$rc_dir/bsnes.rc" ]]; then
    info "Compiling bsnes.rc..."
    if command -v llvm-rc >/dev/null 2>&1; then
      pushd "$rc_dir" > /dev/null
      llvm-rc /nologo bsnes.rc /fo "../../../$objdir/bsnes.res" 2>/dev/null || true
      popd > /dev/null
    fi
  fi
}

# ---------------------- Compile all units ------------------------------------
compile_objects() {
  banner "Compiling bsnes ($CONFIG) for Windows"

  local objdir="$BUILD_DIR/obj"
  mkdir -p "$objdir"

  local manifest="$BUILD_DIR/compile_manifest.txt"
  rm -f "$manifest"

  local B="bsnes"

  # --- Hiro (Windows GUI) ---
  echo "hiro/hiro.cpp|$objdir/hiro-windows.obj|cpp" >> "$manifest"

  # --- Ruby (hardware drivers) ---
  echo "ruby/ruby.cpp|$objdir/ruby.obj|cpp" >> "$manifest"

  # --- Base objects ---
  echo "libco/libco.c|$objdir/libco.obj|c" >> "$manifest"
  echo "$B/emulator/emulator.cpp|$objdir/emulator.obj|cpp" >> "$manifest"
  echo "$B/filter/filter.cpp|$objdir/filter.obj|cpp" >> "$manifest"
  echo "$B/lzma/lzma.cpp|$objdir/lzma.obj|cpp" >> "$manifest"

  # --- SFC (SNES) core ---
  echo "$B/sfc/interface/interface.cpp|$objdir/sfc-interface.obj|cpp" >> "$manifest"
  echo "$B/sfc/system/system.cpp|$objdir/sfc-system.obj|cpp" >> "$manifest"
  echo "$B/sfc/controller/controller.cpp|$objdir/sfc-controller.obj|cpp" >> "$manifest"
  echo "$B/sfc/cartridge/cartridge.cpp|$objdir/sfc-cartridge.obj|cpp" >> "$manifest"
  echo "$B/sfc/memory/memory.cpp|$objdir/sfc-memory.obj|cpp" >> "$manifest"
  echo "$B/sfc/cpu/cpu.cpp|$objdir/sfc-cpu.obj|cpp" >> "$manifest"
  echo "$B/sfc/smp/smp.cpp|$objdir/sfc-smp.obj|cpp" >> "$manifest"
  echo "$B/sfc/dsp/dsp.cpp|$objdir/sfc-dsp.obj|cpp" >> "$manifest"
  echo "$B/sfc/ppu/ppu.cpp|$objdir/sfc-ppu.obj|cpp" >> "$manifest"
  echo "$B/sfc/ppu-fast/ppu.cpp|$objdir/sfc-ppu-fast.obj|cpp" >> "$manifest"
  echo "$B/sfc/expansion/expansion.cpp|$objdir/sfc-expansion.obj|cpp" >> "$manifest"
  echo "$B/sfc/coprocessor/coprocessor.cpp|$objdir/sfc-coprocessor.obj|cpp" >> "$manifest"
  echo "$B/sfc/slot/slot.cpp|$objdir/sfc-slot.obj|cpp" >> "$manifest"

  # --- Game Boy (SameBoy) ---
  local gb_c_files=(apu camera rumble display gb joypad mbc memory printer random rewind save_state sgb sm83_cpu timing)
  for unit in "${gb_c_files[@]}"; do
    echo "$B/gb/Core/$unit.c|$objdir/gb-$unit.obj|c" >> "$manifest"
  done

  # --- vasprintf shim (MSVC lacks it) ---
  echo "$BUILD_DIR/vasprintf.c|$objdir/vasprintf.obj|c" >> "$manifest"

  # --- Processors ---
  echo "$B/processor/arm7tdmi/arm7tdmi.cpp|$objdir/proc-arm7tdmi.obj|cpp" >> "$manifest"
  echo "$B/processor/spc700/spc700.cpp|$objdir/proc-spc700.obj|cpp" >> "$manifest"
  echo "$B/processor/wdc65816/wdc65816.cpp|$objdir/proc-wdc65816.obj|cpp" >> "$manifest"

  # --- Target bsnes (UI) ---
  echo "$B/target-bsnes/bsnes.cpp|$objdir/target-bsnes.obj|cpp" >> "$manifest"
  echo "$B/target-bsnes/program/program.cpp|$objdir/target-program.obj|cpp" >> "$manifest"
  echo "$B/target-bsnes/input/input.cpp|$objdir/target-input.obj|cpp" >> "$manifest"
  echo "$B/target-bsnes/presentation/presentation.cpp|$objdir/target-presentation.obj|cpp" >> "$manifest"
  echo "$B/target-bsnes/settings/settings.cpp|$objdir/target-settings.obj|cpp" >> "$manifest"
  echo "$B/target-bsnes/tools/tools.cpp|$objdir/target-tools.obj|cpp" >> "$manifest"
  echo "$B/target-bsnes/resource/resource.cpp|$objdir/target-resource.obj|cpp" >> "$manifest"

  local count
  count=$(wc -l < "$manifest")
  info "Compiling $count units with $JOBS parallel jobs..."

  # Export for xargs subshell
  export CL_CMD="$CL"
  export CFLAGS_CPP="${CFLAGS[*]}"
  export CFLAGS_C_STR="${CFLAGS_C[*]}"

  if ! cat "$manifest" \
    | xargs -P "$JOBS" -n 1 sh -c '
        IFS="|" read -r src obj lang <<< "$0"
        log="$obj.log"
        if [[ "$lang" == "c" ]]; then
          flags="$CFLAGS_C_STR"
        else
          flags="$CFLAGS_CPP"
        fi
        if ! $CL_CMD $flags /c "$src" /Fo"$obj" >"$log" 2>&1; then
          echo "FAILED: $src" >&2
          cat "$log" >&2
          exit 1
        fi
        rm -f "$log"
      '; then
    die "Compilation failed"
  fi

  local obj_count
  obj_count=$(find "$objdir" -name "*.obj" 2>/dev/null | wc -l)
  [[ $obj_count -gt 0 ]] || die "No object files produced"
  ok "Compiled $obj_count object files"

  # Compile Windows resources
  compile_resources
}

# ---------------------- Link --------------------------------------------------
link_exe() {
  banner "Linking bsnes.exe"

  local objdir="$BUILD_DIR/obj"
  local outdir="$BUILD_DIR"
  mkdir -p "$outdir"

  local objs=("$objdir"/*.obj)
  [[ -f "${objs[0]}" ]] || die "No object files in $objdir"

  local out="$outdir/bsnes.exe"

  info "Linking $(basename "$out")..."

  # Include .res if available
  local res_args=()
  if [[ -f "$objdir/bsnes.res" ]]; then
    res_args=("$objdir/bsnes.res")
  fi

  "$CL" --target="$TARGET_TRIPLE" /nologo /MT -fuse-ld="$LINKER" \
    "${objs[@]}" \
    "${res_args[@]}" \
    /link \
    /nologo \
    /subsystem:windows \
    /entry:mainCRTStartup \
    /incremental:no \
    /out:"$out" \
    "${LINK_LIBS[@]}" \
    /NODEFAULTLIB:msvcrt.lib \
    /errorlimit:0

  ok "Built: $out ($(du -h "$out" | cut -f1))"

  if [[ -n "$MNT_COPY_DIR" ]]; then
    mkdir -p "$MNT_COPY_DIR" 2>/dev/null || true
    if cp -f "$out" "$MNT_COPY_DIR/bsnes.exe" 2>/dev/null; then
      ok "Copied to $MNT_COPY_DIR/bsnes.exe"
    else
      info "Could not copy to $MNT_COPY_DIR (mount not available?)"
    fi
  fi
}

# ---------------------- Clean -------------------------------------------------
clean() {
  banner "Cleaning"
  rm -rf "$BUILD_DIR"
  ok "Clean complete"
}

# ---------------------- Usage -------------------------------------------------
usage() {
  cat << EOF

bsnes-fastlink Windows cross-build

Usage: $0 [command]

Commands:
  build (default)  Build bsnes.exe for Windows
  clean            Remove build artifacts
  help             Show this help

Environment:
  CONFIG=$CONFIG          Build type (debug|stable|size|release|performance)
  BUILD_DIR=$BUILD_DIR       Output directory
  WINSDK_BASE=$WINSDK_BASE    Windows SDK root
  LIBS_PREFIX=$LIBS_PREFIX   Static deps root (zlib, libpng)
  MNT_COPY_DIR=$MNT_COPY_DIR  Post-build copy destination
  JOBS=$JOBS               Parallel jobs
  CL=$CL          C/C++ compiler
  LINKER=$LINKER        Linker

EOF
}

# ---------------------- Main --------------------------------------------------
main() {
  local cmd="${1:-build}"

  case "$cmd" in
    clean) clean; exit 0 ;;
    help|-h|--help) usage; exit 0 ;;
    build) ;;
    *) die "Unknown command: $cmd" ;;
  esac

  banner "bsnes-fastlink Windows Build"

  # Detect SDK
  detect_winsdk
  sdk_paths

  # Verify deps
  [[ -f "$ZLIB_DIR/lib/zlib.lib" ]] || die "zlib not found at $ZLIB_DIR (run build_windows_libs.sh first)"
  [[ -f "$LIBPNG_DIR/lib/libpng.lib" ]] || die "libpng not found at $LIBPNG_DIR (run build_windows_libs.sh first)"
  ok "Static dependencies found"

  # Setup compiler/linker config
  setup_flags

  # Compile
  compile_objects

  # Link
  link_exe
}

main "$@"
