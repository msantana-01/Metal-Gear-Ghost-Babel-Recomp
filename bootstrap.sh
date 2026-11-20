#!/usr/bin/env bash
#
# bootstrap.sh - prepares everything the build needs. Run this ONCE.
#
#   ./bootstrap.sh
#
# The script is written so that it never stops with a message that does not
# say what to do. If something is missing it says what is missing and the
# exact command that fixes it, for your distribution.

set -uo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLS_DIR="$REPO_DIR/.tools"
LOG_FILE="$REPO_DIR/.bootstrap.log"

[ -f "$REPO_DIR/pins.lock" ] || {
  echo "ERROR: pins.lock not found. Cloned the wrong thing?"
  exit 1
}

pin() { # pin <section> <key>
  sed -n "/^\[$1\]/,/^\[/s/^$2[[:space:]]*=[[:space:]]*//p" "$REPO_DIR/pins.lock" | head -1
}

# Release (tarball) or build from source. It has to exist even when empty:
# with `set -u`, referencing a variable that was never assigned aborts the
# script - and the git-clone path is exactly the one that does not assign it.
RELEASE_MODE=""

ENGINE_URL="$(pin engine url)"
ENGINE_COMMIT="$(pin engine commit)"
ENGINE_PATCH="$(pin engine patch)"
ENGINE_PATCH_SHA="$(pin engine patch_sha256)"
ENGINE_TREE="$(pin engine tree_after)"
SDL2_MIN="$(pin build sdl2_min)"
TOOLS_VERSION="$(pin tools version)"
RELEASE_BASE="$(pin tools release_base)"

if [ "${GB_BUILD_FROM_SOURCE:-0}" = "1" ]; then
  RELEASE_BASE=""
fi

C_RESET=$'\033[0m'; C_OK=$'\033[32m'; C_WARN=$'\033[33m'; C_ERR=$'\033[31m'; C_HDR=$'\033[1;36m'
[ -t 1 ] || { C_RESET=; C_OK=; C_WARN=; C_ERR=; C_HDR=; }

step()  { printf '\n%s==> %s%s\n' "$C_HDR" "$1" "$C_RESET"; }
ok()    { printf '    %sok%s   %s\n' "$C_OK" "$C_RESET" "$1"; }
info()  { printf '    -    %s\n' "$1"; }
warn()  { printf '    %snote%s %s\n' "$C_WARN" "$C_RESET" "$1"; }
log()   { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$1" >>"$LOG_FILE" 2>/dev/null || true; }

die() { # die <what is missing> <the command that fixes it>
  printf '\n%sFAILED: %s%s\n' "$C_ERR" "$1" "$C_RESET" >&2
  [ $# -ge 2 ] && printf '\n  Fix it like this:\n\n    %s\n\n' "$2" >&2
  printf '  Technical details: %s\n' "$LOG_FILE" >&2
  exit 1
}

mkdir -p "$TOOLS_DIR"
: > "$LOG_FILE"
: > "$TOOLS_DIR/env"
VER_INSTALLED="$(cat "$TOOLS_DIR/VERSION" 2>/dev/null || true)"

# ---------------------------------------------------------------- distro
DISTRO="$(. /etc/os-release 2>/dev/null && echo "${ID:-unknown}")"

# While compiling from source (or when no release tarball matches), we need
# cmake and ninja. Knowing BEFORE the first install avoids two trips to the
# package manager.
BUILD_EXTRA=""
if [ -z "$RELEASE_BASE" ]; then
  case "$DISTRO" in
    debian|ubuntu|linuxmint|pop|elementary|zorin) BUILD_EXTRA="cmake ninja-build" ;;
    arch|manjaro|endeavours|cachyos|garuda)        BUILD_EXTRA="cmake ninja" ;;
    fedora)                                       BUILD_EXTRA="cmake ninja-build" ;;
    opensuse*|sles)                               BUILD_EXTRA="cmake ninja" ;;
  esac
fi

install_hint() { # install_hint <packages...>
  case "$DISTRO" in
    arch|manjaro|endeavours|cachyos|garuda)
      printf 'sudo pacman -S --needed base-devel pkgconf sdl2 %s %s\n' "$BUILD_EXTRA" "${1:-}" ;;
    debian|ubuntu|linuxmint|pop|elementary|zorin)
      printf 'sudo apt-get update && sudo apt-get install -y build-essential pkg-config libsdl2-dev %s %s\n' "$BUILD_EXTRA" "${1:-}" ;;
    fedora)
      printf 'sudo dnf install -y gcc-c++ make pkgconf-pkg-config SDL2-devel %s %s\n' "$BUILD_EXTRA" "${1:-}" ;;
    opensuse*|sles)
      printf 'sudo zypper install -y gcc-c++ make pkg-config libSDL2-devel %s %s\n' "$BUILD_EXTRA" "${1:-}" ;;
    *)
      printf '# install: a C/C++ compiler, make, pkg-config and the SDL2 headers\n' ;;
  esac
}

have() { command -v "$1" >/dev/null 2>&1; }

# --------------------------------------------- 0.5 tools already shipped
#
# Whoever downloaded the release gets the compiled tools (bin/ and lib/) and
# needs no cmake, no ninja and no network. Re-copy whenever .tools/gbrecomp is
# missing or an upgrade changed pins.lock's tools version.
if [ "${GB_BUILD_FROM_SOURCE:-0}" != "1" ] \
   && [ -f "$REPO_DIR/bin/gbrecomp" ] && [ -f "$REPO_DIR/lib/libgbrt.a" ]; then
  if [ ! -x "$TOOLS_DIR/gbrecomp" ] || [ ! -f "$TOOLS_DIR/libgbrt.a" ] \
     || [ "$VER_INSTALLED" != "$TOOLS_VERSION" ]; then
    mkdir -p "$TOOLS_DIR"
    cp "$REPO_DIR/bin/gbrecomp" "$TOOLS_DIR/gbrecomp"
    cp "$REPO_DIR/lib/libgbrt.a" "$TOOLS_DIR/libgbrt.a"
    chmod +x "$TOOLS_DIR/gbrecomp"
    printf '%s\n' "$TOOLS_VERSION" > "$TOOLS_DIR/VERSION"
    VER_INSTALLED="$TOOLS_VERSION"
    info "release tools v$TOOLS_VERSION installed into .tools/"
  else
    info "release tools v$TOOLS_VERSION detected (nothing to compile)"
  fi
  RELEASE_MODE=1
fi

# With a published release, whoever cloned from git can download the ready
# tools instead of compiling them.
if [ -z "$RELEASE_MODE" ] && [ -n "$RELEASE_BASE" ]; then
  RELEASE_MODE=download
fi

# With a published release the tools arrive by download, which needs curl or
# wget. Put it in the first install hint so users make one trip.
if [ "$RELEASE_MODE" != "1" ] && [ -n "$RELEASE_BASE" ] \
   && ! have curl && ! have wget; then
  BUILD_EXTRA="$BUILD_EXTRA curl"
fi

# ------------------------------------------------------- 0. basic checks
step "Checking the essentials"

if [ "$(uname -s)" != "Linux" ]; then
  warn "This shell bootstrap is for Linux (you are on $(uname -s))."
  warn "On Windows, use GhostBabelRecomp.bat or windows/build.ps1."
fi

if [ -z "$RELEASE_MODE" ] && ! have git; then
  die "git not found." "$(install_hint git)"
fi
if have git; then
  ok "git $(git --version | awk '{print $3}')"
fi

CC_BIN=""
for c in cc gcc clang; do if have "$c"; then CC_BIN="$c"; break; fi; done
[ -n "$CC_BIN" ] || die "no C compiler found." "$(install_hint)"
CXX_BIN=""
for c in c++ g++ clang++; do if have "$c"; then CXX_BIN="$c"; break; fi; done
[ -n "$CXX_BIN" ] || die "no C++ compiler found." "$(install_hint)"
ok "compiler: $CC_BIN / $CXX_BIN"

if ! have make; then
  die "make not found." "$(install_hint)"
else
  ok "make $(make --version | head -1 | awk '{print $3}')"
fi

# ------------------------------------------------------- 1. SDL2 headers
step "Checking SDL2"

SDL_CFLAGS=""; SDL_LIBS=""; SDL_VER=""
if have pkg-config && pkg-config --exists sdl2 2>/dev/null; then
  SDL_CFLAGS="$(pkg-config --cflags sdl2)"
  SDL_LIBS="$(pkg-config --libs sdl2)"
  SDL_VER="$(pkg-config --modversion sdl2)"
elif have sdl2-config; then
  SDL_CFLAGS="$(sdl2-config --cflags)"
  SDL_LIBS="$(sdl2-config --libs)"
  SDL_VER="$(sdl2-config --version | awk '{print $1}')"
fi

if [ -z "$SDL_VER" ]; then
  die "SDL2 development headers not found.
This is the only extra package: it carries the .h files used when compiling." \
      "$(install_hint)"
fi

# Version comparison without depending on "sort -V" (not always present).
ver_ge() { [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V 2>/dev/null | head -1)" = "$2" ]; }
if ! ver_ge "$SDL_VER" "$SDL2_MIN"; then
  die "found SDL2 $SDL_VER, but this project needs $SDL2_MIN or newer." \
      "$(install_hint)"
fi
ok "SDL2 $SDL_VER (minimum $SDL2_MIN)"
printf '%s\n' "SDL_CFLAGS=$SDL_CFLAGS" >> "$TOOLS_DIR/env"

# ------------------------------------------------- 2. engine (pinned patch)
ENGINE_DIR="$TOOLS_DIR/engine"
if [ "$RELEASE_MODE" = "1" ]; then
  step "Release tools"
  ok "gbrecomp and the runtime are ready (v$TOOLS_VERSION): nothing to compile"
  ok "the runtime headers come with the release package"
  printf '%s\n' "GBRECOMP=$TOOLS_DIR/gbrecomp" >> "$TOOLS_DIR/env"
  printf '\n%sDone!%s\n' "$C_OK" "$C_RESET"
  printf '\n  Next step:\n\n      ./build.sh /path/to/your/rom.gbc\n\n'
  exit 0
fi

if [ -z "$RELEASE_MODE" ]; then

step "Downloading the engine (pinned version)"

if [ -d "$ENGINE_DIR/.git" ]; then
  ok "already in .tools/engine"
else
  info "cloning $ENGINE_URL"
  log "clone $ENGINE_URL"
  rm -rf "$ENGINE_DIR"
  have git || die "git not found."
  git clone -q --filter=blob:none "$ENGINE_URL" "$ENGINE_DIR" \
    || die "could not download the engine. No internet?" "git clone $ENGINE_URL $ENGINE_DIR"
  log "checkout $ENGINE_COMMIT"
  git -C "$ENGINE_DIR" checkout -q "$ENGINE_COMMIT" \
    || die "the pinned engine commit ($ENGINE_COMMIT) was not found." \
       "This usually means upstream removed the commit. Please report it on GitHub."
  ok "commit $ENGINE_COMMIT"
fi

# patch integrity
PATCH_FILE="$REPO_DIR/$ENGINE_PATCH"
[ -f "$PATCH_FILE" ] || die "engine patch missing: $ENGINE_PATCH" "git clone <this repo> again"
if have sha256sum; then
  GOT="$(sha256sum "$PATCH_FILE" | awk '{print $1}')"
  [ "$GOT" = "$ENGINE_PATCH_SHA" ] \
    || die "the engine patch does not match its pin (different sha256).
This usually means a corrupted file." "rm -rf .tools && git clone <this repo> again"
fi

TREE_NOW="$(git -C "$ENGINE_DIR" rev-parse HEAD^{tree} 2>/dev/null || true)"
if [ -n "$ENGINE_TREE" ] && [ "$TREE_NOW" = "$ENGINE_TREE" ]; then
  ok "patch already applied"
else
  if [ "$(git -C "$ENGINE_DIR" rev-parse HEAD 2>/dev/null)" != "$ENGINE_COMMIT" ]; then
    git -C "$ENGINE_DIR" am --abort >/dev/null 2>&1 || true
    git -C "$ENGINE_DIR" reset -q --hard "$ENGINE_COMMIT"
  fi
  info "applying the engine patch"
  log "git am $ENGINE_PATCH"
  git -C "$ENGINE_DIR" am -q "$PATCH_FILE" \
    || die "the engine patch did not apply cleanly.
This is our bug, not yours - please report it on GitHub with the output above." \
       "cd .tools/engine && git am --abort"
  ok "patch applied"
fi

TREE_NOW="$(git -C "$ENGINE_DIR" rev-parse HEAD^{tree} 2>/dev/null)"
if [ -n "$ENGINE_TREE" ] && [ "$TREE_NOW" != "$ENGINE_TREE" ]; then
  die "the engine came out different from expected after the patch.
Expected tree $ENGINE_TREE, got $TREE_NOW." \
      "cd .tools && rm -rf engine && ../bootstrap.sh"
fi
ok "engine integrity checked"

fi   # end: without a release, clone and compile the engine

if [ -n "$RELEASE_MODE" ]; then
  step "Engine"
  ok "skipping the engine clone: prebuilt runtime arrives from release v$TOOLS_VERSION"
  info "the tools arrive ready from the release (no cmake, no ninja)"
fi

# --------------------------------------------- 3. the tools (gbrecomp)
step "Preparing the tools"

GBRECOMP="$TOOLS_DIR/gbrecomp"
if [ -x "$GBRECOMP" ] && [ -f "$TOOLS_DIR/libgbrt.a" ] && [ "$VER_INSTALLED" = "$TOOLS_VERSION" ]; then
  ok "gbrecomp already ready (v$TOOLS_VERSION)"
elif [ -n "$RELEASE_BASE" ]; then
  info "downloading prebuilt tools (v$TOOLS_VERSION)"
  ARCH="$(uname -m)"
  case "$ARCH" in
    x86_64) PLAT=linux-x86_64 ;;
    *)
      die "the prebuilt tools are x86_64 only, and this machine reports '$ARCH'." \
        "GB_BUILD_FROM_SOURCE=1 ./bootstrap.sh"
      ;;
  esac
  TARBALL="$RELEASE_BASE/ghost-babel-tools-$PLAT-$TOOLS_VERSION.tar.gz"
  rm -rf "$TOOLS_DIR/runtime" "$TOOLS_DIR/bin" "$TOOLS_DIR/lib" "$TOOLS_DIR/tools.tar.gz"
  if have curl; then curl -fsSL "$TARBALL" -o "$TOOLS_DIR/tools.tar.gz"
  elif have wget; then wget -q "$TARBALL" -O "$TOOLS_DIR/tools.tar.gz"
  else die "I need curl or wget to download the tools." "$(install_hint curl)"
  fi
  tar -xzf "$TOOLS_DIR/tools.tar.gz" -C "$TOOLS_DIR" --strip-components=1 \
    || die "could not unpack the downloaded tools." "rm -f .tools/tools.tar.gz && ./bootstrap.sh"
  [ -f "$TOOLS_DIR/bin/gbrecomp" ] \
    || die "the downloaded package did not include gbrecomp." "$TARBALL"
  cp "$TOOLS_DIR/bin/gbrecomp" "$GBRECOMP" && chmod +x "$GBRECOMP"
  [ -f "$TOOLS_DIR/lib/libgbrt.a" ] \
    || die "the downloaded package did not include the runtime library." "$TARBALL"
  cp "$TOOLS_DIR/lib/libgbrt.a" "$TOOLS_DIR/libgbrt.a"
  if [ -d "$TOOLS_DIR/runtime" ]; then
    rm -rf "$REPO_DIR/runtime"
    mv "$TOOLS_DIR/runtime" "$REPO_DIR/runtime"
  fi
  rm -f "$TOOLS_DIR/tools.tar.gz"
  printf '%s\n' "$TOOLS_VERSION" > "$TOOLS_DIR/VERSION"
  VER_INSTALLED="$TOOLS_VERSION"
  ok "tools ready (gbrecomp + runtime, from release v$TOOLS_VERSION)"
else
  warn "building tools from source (cmake + ninja)."
  have cmake || die "building the tools from scratch needs cmake." "$(install_hint cmake ninja-build)"
  have ninja || warn "no ninja: cmake will use make (slower)."
  CMAKE_G="Ninja" ; have ninja || CMAKE_G="Unix Makefiles"
  log "cmake -S $ENGINE_DIR -B $ENGINE_DIR/build -G $CMAKE_G"
  cmake -G "$CMAKE_G" -S "$ENGINE_DIR" -B "$ENGINE_DIR/build" \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF >/dev/null \
    || die "cmake failed to configure the engine." "cmake -S .tools/engine -B .tools/engine/build"
  cmake --build "$ENGINE_DIR/build" --target gbrecomp -j "$(nproc 2>/dev/null || echo 4)" >/dev/null \
    || die "cmake failed to compile gbrecomp." "cmake --build .tools/engine/build --target gbrecomp"
  cp "$ENGINE_DIR/build/bin/gbrecomp" "$GBRECOMP" 2>/dev/null \
    || find "$ENGINE_DIR/build" -name gbrecomp -type f -exec cp {} "$GBRECOMP" \; \
    || die "built it but could not find the gbrecomp binary." "ls .tools/engine/build/bin"
  chmod +x "$GBRECOMP"
  ok "gbrecomp compiled from source"
fi
printf '%s\n' "GBRECOMP=$GBRECOMP" >> "$TOOLS_DIR/env"

# --------------------------------------------- 4. prebuilt runtime library
step "Preparing the runtime (the library that makes the game run)"

LIB="$TOOLS_DIR/libgbrt.a"
RT="$ENGINE_DIR/runtime"

if [ -n "$RELEASE_MODE" ] && [ -f "$LIB" ]; then
  ok "libgbrt.a ready (v$TOOLS_VERSION)"
elif [ -f "$LIB" ] && [ "$LIB" -nt "$ENGINE_DIR/.git/HEAD" ] && [ "$VER_INSTALLED" = "$TOOLS_VERSION" ] 2>/dev/null; then
  ok "libgbrt.a already built"
else
  log "build libgbrt.a"
  OBJ="$TOOLS_DIR/.obj"; rm -rf "$OBJ"; mkdir -p "$OBJ"
  # C runtime
  for f in "$RT"/src/*.c; do
    b="$(basename "${f%.c}")"
    [ "$b" = "gbrt_gameplay_mutation" ] && continue
    [ "$b" = "gbrt_native_patch" ] && continue
    [ -f "$f" ] || die "missing runtime source ($f).
It was supposed to be compiled from the cloned engine, but it is not in $RT." \
      "rm -rf .tools && ./bootstrap.sh"
    $CC_BIN -std=gnu11 -O3 -DGB_HAS_SDL2 -DGB_PLATFORM_LINUX \
      -I"$RT/include" $SDL_CFLAGS -c "$f" -o "$OBJ/$b.o" \
      || die "failed to compile $b.c" "$CC_BIN --version"
  done
  # C++ runtime (SDL + Dear ImGui)
  CXXF="-std=c++20 -O3 -DGB_HAS_SDL2 -DGB_PLATFORM_LINUX -I$RT/include -I$RT/vendor/imgui -I$RT/vendor/imgui/backends $SDL_CFLAGS"
  for f in "$RT/src/platform_sdl.cpp" \
           "$RT"/vendor/imgui/imgui.cpp "$RT"/vendor/imgui/imgui_draw.cpp \
           "$RT"/vendor/imgui/imgui_tables.cpp "$RT"/vendor/imgui/imgui_widgets.cpp \
           "$RT"/vendor/imgui/backends/imgui_impl_sdl2.cpp \
           "$RT"/vendor/imgui/backends/imgui_impl_sdlrenderer2.cpp; do
    b="$(basename "${f%.*}")"
    $CXX_BIN $CXXF -c "$f" -o "$OBJ/$b.o" || die "failed to compile $b.cpp" "$CXX_BIN --version"
  done
  ar rcs "$LIB" "$OBJ"/*.o || die "failed to assemble the static library." "ar --version"
  rm -rf "$OBJ"
  printf '%s\n' "$TOOLS_VERSION" > "$TOOLS_DIR/VERSION"
  ok "libgbrt.a ($(du -h "$LIB" | cut -f1))"
fi

# --------------------------------------------------- 5. real check
step "Checking that it runs"

if [ -x "$TOOLS_DIR/selftest" ]; then
  "$TOOLS_DIR/selftest" >/dev/null 2>&1 \
    && ok "the runtime responds" \
    || die "the runtime was compiled but does not respond." "$LOG_FILE"
else
  ok "runtime ready (running it is checked on the first build)"
fi

printf '\n%sAll set.%s\n' "$C_OK" "$C_RESET"
cat <<EOF

  Next step:

      ./build.sh /path/to/your/rom.gbc

  (or run ./run.sh --gui to pick the ROM in a window)

  Log of this run: $LOG_FILE

EOF
