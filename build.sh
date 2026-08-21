#!/usr/bin/env bash
set -euo pipefail

# Windows-first entrypoint for this workspace.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$SCRIPT_DIR/build_windows.sh" "$@"

#===============================================================================
# bsnes-fastlink — Linux native build (g++ / clang++)
# - Replaces the GNUmakefile-based build with a single shell entrypoint
# - Auto-detects available ruby drivers via pkg-config
# - Builds the bsnes GUI application (GTK3 + X11/GL/ALSA/PulseAudio/udev)
# - Parallel compilation via xargs
#
# Usage: ./build.sh [build|clean|install|help]
#   build (default) - build bsnes
#   clean           - remove build artifacts
#   install         - install to prefix
#   help            - show usage
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

CONFIG="${CONFIG:-performance}"   # debug|stable|size|release|performance
TARGET="${TARGET:-bsnes}"         # bsnes|libretro
HIRO="${HIRO:-gtk3}"              # gtk3|gtk3-se|qt5|qt6
BUILD_DIR="${BUILD_DIR:-build}"
COMPILER="${COMPILER:-g++}"
LOCAL="${LOCAL:-true}"             # -march=native
OPENMP="${OPENMP:-true}"
LTO="${LTO:-false}"
JOBS="${JOBS:-$(nproc)}"
PREFIX="${PREFIX:-$HOME/.local}"
MNT_COPY_DIR="${MNT_COPY_DIR:-/mnt/PhantomIDE}"

# SameBoy version (from gb/version.mk)
GB_VERSION="$(cat bsnes/gb/version.mk 2>/dev/null | grep -oP 'VERSION\s*:=\s*\K.*' || echo '1.0.2')"

# ---------------------- Platform detection -----------------------------------
detect_platform() {
  local uname_s
  uname_s="$(uname -s 2>/dev/null || true)"
  case "$uname_s" in
    Linux*)  PLATFORM="linux" ;;
    Darwin*) PLATFORM="macos" ;;
    *BSD*)   PLATFORM="bsd" ;;
    *)       die "Unsupported platform: $uname_s" ;;
  esac
  export PLATFORM
  ok "Platform: $PLATFORM"
}

# ---------------------- Compiler flags ---------------------------------------
setup_flags() {
  # Base flags
  FLAGS="-I./bsnes -I."
  OPTIONS=""

  # C/C++ standard
  C_STD="-std=c11"
  CXX_STD="-std=c++17"

  if [[ "$COMPILER" == *"g++"* ]]; then
    FLAGS="$FLAGS -fno-strict-aliasing -fwrapv -Wno-trigraphs"
    # g++ on windows uses gnu++17, but on linux c++17
  elif [[ "$COMPILER" == *"clang++"* ]]; then
    FLAGS="$FLAGS -fno-strict-aliasing -fwrapv -Wno-everything"
  fi

  # Build optimization
  case "$CONFIG" in
    debug)       FLAGS="$FLAGS -Og -g -DBUILD_DEBUG" ;;
    stable)      FLAGS="$FLAGS -O1 -DBUILD_STABLE" ;;
    size)        FLAGS="$FLAGS -Os -DBUILD_SIZE" ;;
    release)     FLAGS="$FLAGS -O2 -DBUILD_RELEASE" ;;
    performance) FLAGS="$FLAGS -O3 -DBUILD_PERFORMANCE" ;;
    *) die "Unknown CONFIG: $CONFIG" ;;
  esac

  # Native arch
  if [[ "$LOCAL" == "true" ]]; then
    FLAGS="$FLAGS -march=native"
  fi

  # OpenMP
  if [[ "$OPENMP" == "true" && "$PLATFORM" != "macos" ]]; then
    FLAGS="$FLAGS -fopenmp"
    OPTIONS="$OPTIONS -fopenmp"
  fi

  # LTO
  if [[ "$LTO" == "true" ]]; then
    FLAGS="$FLAGS -fwhole-program -flto -fno-fat-lto-objects"
    OPTIONS="$OPTIONS -fwhole-program -flto=jobserver"
  fi

  # Game Boy (SameBoy) flags
  GB_FLAGS="-DGB_INTERNAL -DGB_DISABLE_DEBUGGER -DGB_DISABLE_CHEATS -D_GNU_SOURCE -DGB_VERSION=\"$GB_VERSION\" -DNDEBUG -Wno-multichar"
  FLAGS="$FLAGS $GB_FLAGS"

  # Platform-specific link flags
  if [[ "$PLATFORM" == "linux" ]]; then
    OPTIONS="$OPTIONS -Wl,-export-dynamic -lX11 -lXext -ldl"
  elif [[ "$PLATFORM" == "bsd" ]]; then
    FLAGS="$FLAGS -I/usr/local/include"
    OPTIONS="$OPTIONS -Wl,-export-dynamic -lX11 -lXext -Wl,-rpath=/usr/local/lib -lstdc++ -lm"
  elif [[ "$PLATFORM" == "macos" ]]; then
    FLAGS="$FLAGS -stdlib=libc++"
    OPTIONS="$OPTIONS -lc++ -lobjc"
  fi

  export FLAGS OPTIONS C_STD CXX_STD
}

# ---------------------- Ruby driver detection --------------------------------
detect_ruby_drivers() {
  local ruby_drivers=()
  local ruby_flags=""
  local ruby_options=""

  if [[ "$PLATFORM" == "linux" || "$PLATFORM" == "bsd" ]]; then
    # Video drivers
    # Vulkan: check for vulkan headers and libvulkan
    if pkg-config --exists vulkan 2>/dev/null || [[ -f /usr/include/vulkan/vulkan.h ]]; then
      ruby_drivers+=("video.vulkan")
    fi
    ruby_drivers+=("video.glx" "video.glx2" "video.xvideo" "video.xshm")

    # Audio drivers
    ruby_drivers+=("audio.oss")
    pkg-config --exists alsa 2>/dev/null && ruby_drivers+=("audio.alsa")
    pkg-config --exists openal 2>/dev/null && ruby_drivers+=("audio.openal")
    pkg-config --exists libpulse 2>/dev/null && ruby_drivers+=("audio.pulseaudio" "audio.pulseaudiosimple")
    pkg-config --exists ao 2>/dev/null && ruby_drivers+=("audio.ao")

    # Input drivers
    ruby_drivers+=("input.xlib")
    pkg-config --exists sdl2 2>/dev/null && ruby_drivers+=("input.sdl")
    pkg-config --exists udev 2>/dev/null && ruby_drivers+=("input.udev")

    # Build defines from driver names (audio.alsa -> -DAUDIO_ALSA)
    for drv in "${ruby_drivers[@]}"; do
      local def
      def=$(echo "$drv" | tr '[:lower:].' '[:upper:]_')
      ruby_flags="$ruby_flags -D$def"
    done

    # SDL2 flags
    if printf '%s\n' "${ruby_drivers[@]}" | grep -q "input.sdl"; then
      ruby_flags="$ruby_flags $(sdl2-config --cflags 2>/dev/null || pkg-config --cflags sdl2 2>/dev/null || true)"
    fi

    # Link options per driver
    ruby_options="-lX11 -lXext -lXrandr"
    printf '%s\n' "${ruby_drivers[@]}" | grep -q "video.vulkan" && ruby_options="$ruby_options -lvulkan"
    printf '%s\n' "${ruby_drivers[@]}" | grep -q "video.glx" && ruby_options="$ruby_options -lGL"
    printf '%s\n' "${ruby_drivers[@]}" | grep -q "video.xvideo" && ruby_options="$ruby_options -lXv"
    printf '%s\n' "${ruby_drivers[@]}" | grep -q "audio.alsa" && ruby_options="$ruby_options -lasound"
    printf '%s\n' "${ruby_drivers[@]}" | grep -q "audio.ao" && ruby_options="$ruby_options -lao"
    printf '%s\n' "${ruby_drivers[@]}" | grep -q "audio.openal" && ruby_options="$ruby_options -lopenal"
    printf '%s\n' "${ruby_drivers[@]}" | grep -q "audio.pulseaudio" && ruby_options="$ruby_options -lpulse"
    printf '%s\n' "${ruby_drivers[@]}" | grep -q "audio.pulseaudiosimple" && ruby_options="$ruby_options -lpulse-simple"
    printf '%s\n' "${ruby_drivers[@]}" | grep -q "input.sdl" && ruby_options="$ruby_options $(sdl2-config --libs 2>/dev/null || pkg-config --libs sdl2 2>/dev/null || true)"
    printf '%s\n' "${ruby_drivers[@]}" | grep -q "input.udev" && ruby_options="$ruby_options -ludev"
  fi

  info "Ruby drivers: ${ruby_drivers[*]}"
  export RUBY_FLAGS="$ruby_flags"
  export RUBY_OPTIONS="$ruby_options"
}

# ---------------------- Hiro GUI detection ------------------------------------
detect_hiro() {
  local hiro_flags=""
  local hiro_options=""

  case "$HIRO" in
    gtk3)
      hiro_flags="-DHIRO_GTK=3 $(pkg-config --cflags gtk+-3.0) -Wno-deprecated-declarations"
      hiro_options="-L/usr/local/lib -lX11 $(pkg-config --libs gtk+-3.0)"
      ;;
    gtk3-se)
      FLAGS="$FLAGS -DHiro_SourceEdit"
      hiro_flags="-DHIRO_GTK=3 $(pkg-config --cflags gtk+-3.0 gtksourceview-3.0) -Wno-deprecated-declarations"
      hiro_options="-L/usr/local/lib -lX11 $(pkg-config --libs gtk+-3.0 gtksourceview-3.0)"
      ;;
    qt5)
      hiro_flags="-DHIRO_QT=5 -fPIC $(pkg-config --cflags Qt5Core Qt5Gui Qt5Widgets)"
      hiro_options="-L/usr/local/lib -lX11 $(pkg-config --libs Qt5Core Qt5Gui Qt5Widgets)"
      ;;
    qt6)
      hiro_flags="-DHIRO_QT=6 -fPIC $(pkg-config --cflags Qt6Core Qt6Gui Qt6Widgets)"
      hiro_options="-L/usr/local/lib -lX11 $(pkg-config --libs Qt6Core Qt6Gui Qt6Widgets)"
      ;;
    *)
      die "Unknown HIRO backend: $HIRO"
      ;;
  esac

  info "Hiro backend: $HIRO"
  export HIRO_FLAGS="$hiro_flags"
  export HIRO_OPTIONS="$hiro_options"
}

# ---------------------- Compile helpers --------------------------------------
compile_cpp() {
  local src="$1" obj="$2" extra_flags="${3:-}"
  $COMPILER -x c++ $CXX_STD -MMD -MP -MF "${obj%.o}.d" $FLAGS $extra_flags -c "$src" -o "$obj"
}

compile_c() {
  local src="$1" obj="$2" extra_flags="${3:-}"
  $COMPILER -x c $C_STD -MMD -MP -MF "${obj%.o}.d" $FLAGS $extra_flags -c "$src" -o "$obj"
}

# ---------------------- Build compilation units ------------------------------
compile_objects() {
  banner "Compiling bsnes ($CONFIG)"

  local objdir="$BUILD_DIR/obj"
  mkdir -p "$objdir"

  # Build a manifest: source object [c|cpp] [extra_flags]
  local manifest="$BUILD_DIR/compile_manifest.txt"
  rm -f "$manifest"

  # Working directory for source paths is bsnes/
  local B="bsnes"

  # --- Hiro (GUI toolkit) ---
  echo "hiro/hiro.cpp|$objdir/hiro-${HIRO}.o|cpp|$HIRO_FLAGS" >> "$manifest"

  # --- Ruby (hardware drivers) ---
  echo "ruby/ruby.cpp|$objdir/ruby.o|cpp|$RUBY_FLAGS" >> "$manifest"

  # --- Base objects ---
  echo "libco/libco.c|$objdir/libco.o|c|" >> "$manifest"
  echo "$B/emulator/emulator.cpp|$objdir/emulator.o|cpp|" >> "$manifest"
  echo "$B/filter/filter.cpp|$objdir/filter.o|cpp|" >> "$manifest"
  echo "$B/lzma/lzma.cpp|$objdir/lzma.o|cpp|" >> "$manifest"

  # --- SFC (SNES) core ---
  echo "$B/sfc/interface/interface.cpp|$objdir/sfc-interface.o|cpp|" >> "$manifest"
  echo "$B/sfc/system/system.cpp|$objdir/sfc-system.o|cpp|" >> "$manifest"
  echo "$B/sfc/controller/controller.cpp|$objdir/sfc-controller.o|cpp|" >> "$manifest"
  echo "$B/sfc/cartridge/cartridge.cpp|$objdir/sfc-cartridge.o|cpp|" >> "$manifest"
  echo "$B/sfc/memory/memory.cpp|$objdir/sfc-memory.o|cpp|" >> "$manifest"
  echo "$B/sfc/cpu/cpu.cpp|$objdir/sfc-cpu.o|cpp|" >> "$manifest"
  echo "$B/sfc/smp/smp.cpp|$objdir/sfc-smp.o|cpp|" >> "$manifest"
  echo "$B/sfc/dsp/dsp.cpp|$objdir/sfc-dsp.o|cpp|" >> "$manifest"
  echo "$B/sfc/ppu/ppu.cpp|$objdir/sfc-ppu.o|cpp|" >> "$manifest"
  echo "$B/sfc/expansion/expansion.cpp|$objdir/sfc-expansion.o|cpp|" >> "$manifest"
  echo "$B/sfc/coprocessor/coprocessor.cpp|$objdir/sfc-coprocessor.o|cpp|" >> "$manifest"
  echo "$B/sfc/slot/slot.cpp|$objdir/sfc-slot.o|cpp|" >> "$manifest"

  # --- Game Boy (SameBoy) — C sources ---
  local gb_srcs=(apu camera rumble display gb joypad mbc memory printer random rewind save_state sgb sm83_cpu timing)
  for s in "${gb_srcs[@]}"; do
    echo "$B/gb/Core/$s.c|$objdir/gb-$s.o|c|" >> "$manifest"
  done

  # --- Processors ---
  echo "$B/processor/arm7tdmi/arm7tdmi.cpp|$objdir/processor-arm7tdmi.o|cpp|" >> "$manifest"
  echo "$B/processor/spc700/spc700.cpp|$objdir/processor-spc700.o|cpp|" >> "$manifest"
  echo "$B/processor/wdc65816/wdc65816.cpp|$objdir/processor-wdc65816.o|cpp|" >> "$manifest"

  # --- UI (target-bsnes) ---
  echo "$B/target-bsnes/bsnes.cpp|$objdir/ui-bsnes.o|cpp|" >> "$manifest"
  echo "$B/target-bsnes/program/program.cpp|$objdir/ui-program.o|cpp|" >> "$manifest"
  echo "$B/target-bsnes/input/input.cpp|$objdir/ui-input.o|cpp|" >> "$manifest"
  echo "$B/target-bsnes/presentation/presentation.cpp|$objdir/ui-presentation.o|cpp|" >> "$manifest"
  echo "$B/target-bsnes/settings/settings.cpp|$objdir/ui-settings.o|cpp|" >> "$manifest"
  echo "$B/target-bsnes/tools/tools.cpp|$objdir/ui-tools.o|cpp|" >> "$manifest"
  echo "$B/target-bsnes/resource/resource.cpp|$objdir/ui-resource.o|cpp|" >> "$manifest"

  local total
  total=$(wc -l < "$manifest")
  info "Compiling $total units with $JOBS parallel jobs..."

  # Export for subshell
  export COMPILER FLAGS C_STD CXX_STD

  # Compile in parallel
  if ! cat "$manifest" | xargs -P "$JOBS" -I {} sh -c '
    IFS="|" read -r src obj lang extra <<< "{}"
    log="${obj}.log"
    if [[ "$lang" == "c" ]]; then
      if ! $COMPILER -x c $C_STD -MMD -MP -MF "${obj%.o}.d" $FLAGS $extra -c "$src" -o "$obj" >"$log" 2>&1; then
        echo "FAILED: $src" >&2
        cat "$log" >&2
        exit 1
      fi
    else
      if ! $COMPILER -x c++ $CXX_STD -MMD -MP -MF "${obj%.o}.d" $FLAGS $extra -c "$src" -o "$obj" >"$log" 2>&1; then
        echo "FAILED: $src" >&2
        cat "$log" >&2
        exit 1
      fi
    fi
    rm -f "$log"
  '; then
    die "Compilation failed"
  fi

  local obj_count
  obj_count=$(find "$objdir" -name "*.o" 2>/dev/null | wc -l)
  [[ $obj_count -gt 0 ]] || die "No object files produced"
  ok "Compiled $obj_count object files"
}

# ---------------------- Link -------------------------------------------
link_exe() {
  banner "Linking bsnes"

  local objdir="$BUILD_DIR/obj"
  local outdir="$BUILD_DIR/out"
  mkdir -p "$outdir"

  local objs=("$objdir"/*.o)
  [[ -f "${objs[0]}" ]] || die "No object files in $objdir"

  local out="$outdir/bsnes"

  info "Linking $(basename "$out")..."

  $COMPILER -o "$out" "${objs[@]}" $HIRO_OPTIONS $RUBY_OPTIONS $OPTIONS

  ok "Built: $out ($(du -h "$out" | cut -f1))"

  if [[ -n "$MNT_COPY_DIR" ]]; then
    mkdir -p "$MNT_COPY_DIR" 2>/dev/null || true
    if cp -f "$out" "$MNT_COPY_DIR/bsnes" 2>/dev/null; then
      ok "Copied to $MNT_COPY_DIR/bsnes"
    else
      info "Could not copy to $MNT_COPY_DIR (mount not available?)"
    fi
  fi
}

# ---------------------- Clean ------------------------------------------
clean() {
  banner "Cleaning"
  rm -rf "$BUILD_DIR"
  # Also clean old-style obj/out dirs in bsnes/
  rm -rf bsnes/obj bsnes/out
  ok "Clean complete"
}

# ---------------------- Install ----------------------------------------
do_install() {
  local exe="$BUILD_DIR/out/bsnes"
  [[ -f "$exe" ]] || die "Build first: $exe not found"

  if [[ "$(id -un)" == "root" ]]; then
    die "Do not run install as root"
  fi

  banner "Installing bsnes"

  mkdir -p "$PREFIX/bin/"
  mkdir -p "$PREFIX/share/applications/"
  mkdir -p "$PREFIX/share/icons/hicolor/256x256/apps/"
  mkdir -p "$PREFIX/share/icons/hicolor/scalable/apps/"
  mkdir -p "$PREFIX/share/bsnes/"
  mkdir -p "$PREFIX/share/bsnes/Database/"
  mkdir -p "$PREFIX/share/bsnes/Firmware/"
  mkdir -p "$PREFIX/share/bsnes/Shaders/"

  cp "$exe" "$PREFIX/bin/bsnes"
  cp bsnes/target-bsnes/resource/bsnes.desktop "$PREFIX/share/applications/bsnes.desktop"
  cp bsnes/target-bsnes/resource/bsnes.png "$PREFIX/share/icons/hicolor/256x256/apps/bsnes.png"
  cp bsnes/target-bsnes/resource/bsnes.svg "$PREFIX/share/icons/hicolor/scalable/apps/bsnes.svg" 2>/dev/null || true
  cp bsnes/Database/* "$PREFIX/share/bsnes/Database/"
  cp -r shaders/* "$PREFIX/share/bsnes/Shaders/"

  ok "Installed to $PREFIX"
}

# ---------------------- Usage ------------------------------------------
usage() {
  cat <<EOF
Usage: $0 [build|clean|install|help]

Commands:
  build (default)  Build bsnes
  clean            Remove build artifacts
  install          Install to prefix

Environment variables:
  CONFIG=performance|release|stable|size|debug   Build optimization (default: performance)
  COMPILER=g++|clang++                           Compiler (default: g++)
  HIRO=gtk3|gtk3-se|qt5|qt6                     GUI backend (default: gtk3)
  LOCAL=true|false                               -march=native (default: true)
  OPENMP=true|false                              OpenMP support (default: true)
  LTO=true|false                                 Link-time optimization (default: false)
  JOBS=N                                         Parallel jobs (default: nproc)
  PREFIX=path                                    Install prefix (default: ~/.local)
  BUILD_DIR=path                                 Build directory (default: build)
EOF
}

# ---------------------- Main -------------------------------------------
main() {
  local cmd="${1:-build}"

  case "$cmd" in
    build)
      banner "bsnes-fastlink Build"
      detect_platform
      setup_flags
      detect_ruby_drivers
      detect_hiro
      compile_objects
      link_exe
      ;;
    clean) clean ;;
    install)
      do_install
      ;;
    -h|--help|help) usage ;;
    *) usage; exit 1 ;;
  esac
}

main "$@"
