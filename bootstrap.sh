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

# ---------------------------------------------------------------- distro
DISTRO="$(. /etc/os-release 2>/dev/null && echo "${ID:-unknown}")"

# While no release is published, the tools are compiled from source, which
# needs cmake and ninja. That can be known BEFORE the first install: without
# it, the user installs the basics, runs bootstrap again, and only then finds
# out cmake is needed (two trips to the package manager).
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
      # The `update` is not decoration: on a freshly installed machine, or
      # with a stale package list, `apt-get install` on its own fails with
      # "Unable to locate package". The clean-distro test found this.
      printf 'sudo apt-get update && sudo apt-get install -y build-essential pkg-config libsdl2-dev %s %s\n' "$BUILD_EXTRA" "${1:-}" ;;
    fedora)
      printf 'sudo dnf install -y gcc-c++ make pkgconf-pkg-config SDL2-devel %s %s\n' "$BUILD_EXTRA" "${1:-}" ;;
    opensuse*|sles)
      printf 'sudo zypper install -y gcc-c++ make pkg-config libSDL2-devel %s %s\n' "$BUILD_EXTRA" "${1:-}" ;;
    *)
      printf '# install: a C/C++ compiler, make, pkg-config, cmake and the SDL2 headers\n' ;;
  esac
}

have() { command -v "$1" >/dev/null 2>&1; }

# --------------------------------------------- 0.5 tools already shipped
#
# Whoever downloaded the release gets the compiled tools (bin/ and lib/) and
# needs no cmake, no ninja and no network. Copy them into the layout the
# build expects and carry on as if bootstrap had compiled them.
if [ -f "$REPO_DIR/bin/gbrecomp" ] && [ -f "$REPO_DIR/lib/libgbrt.a" ] \
   && [ ! -x "$TOOLS_DIR/gbrecomp" ]; then
  mkdir -p "$TOOLS_DIR"
  cp "$REPO_DIR/bin/gbrecomp" "$TOOLS_DIR/gbrecomp"
  cp "$REPO_DIR/lib/libgbrt.a" "$TOOLS_DIR/libgbrt.a"
  chmod +x "$TOOLS_DIR/gbrecomp"
  info "release tools detected (nothing to compile)"
  RELEASE_MODE=1
fi

# With a published release, whoever cloned from git can download the ready
# tools instead of compiling them. In that case the engine (16 MB of
# third-party source nobody asked for) is unnecessary: the headers the build
# needs are embedded in the project gbrecomp generates.
if [ -z "$RELEASE_MODE" ] && [ -n "$RELEASE_BASE" ]; then
  RELEASE_MODE=download
fi

# With a published release the tools arrive by download, which needs curl or
# wget. That has to be in the FIRST install hint: otherwise the user installs
# the basics, runs bootstrap again, and only then finds out they need a
# downloader (two trips to the package manager). The clean-distro test found
# this on an Ubuntu image that ships neither.
# NOTE: RELEASE_MODE is already resolved by this point ("1" for the tarball,
# "download" when a release exists). What matters is whether the tools will
# be FETCHED, not whether they are already here.
if [ "$RELEASE_MODE" != "1" ] && [ -n "$RELEASE_BASE" ] \
   && ! have curl && ! have wget; then
  BUILD_EXTRA="$BUILD_EXTRA curl"
fi

# ------------------------------------------------------- 0. basic checks
step "Checking the essentials"

if [ "$(uname -s)" != "Linux" ]; then
  warn "This project only works on Linux so far (you are on $(uname -s))."
  warn "Windows and Android are planned."
fi

if ! have git; then die "git not found." "$(install_hint git)"; fi
ok "git $(git --version | awk '{print $3}')"

CC_BIN=""
for c in cc gcc clang; do if have "$c"; then CC_BIN="$c"; break; fi; done
[ -n "$CC_BIN" ] || die "no C compiler found." "$(install_hint)"
CXX_BIN=""
for c in c++ g++ clang++; do if have "$c"; then CXX_BIN="$c"; break; fi; done
[ -n "$CXX_BIN" ] || die "no C++ compiler found." "$(install_hint)"
ok "compiler: $CC_BIN / $CXX_BIN"

if ! have make; then
  warn "make not found - only needed if you compile from source."
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
#
# Whoever downloaded the release tarball does not need the clone: gbrecomp
# embeds the runtime in the generated project, and the Makefile uses the
# headers from there. The clone is only needed to COMPILE the tools - and in
# that case they already arrived ready-made.
ENGINE_DIR="$TOOLS_DIR/engine"
if [ "$RELEASE_MODE" = "1" ]; then
  # Tarball: the tools are already here, nothing to fetch.
  step "Release tools"
  ok "gbrecomp and the runtime are ready: nothing to compile"
  ok "the runtime headers come with the generated C"
  mkdir -p "$TOOLS_DIR"
  printf '%s\n' "GBRECOMP=$TOOLS_DIR/gbrecomp" >> "$TOOLS_DIR/env"
  printf '\n%s%sDone!%s\n' "$C_OK" "" "$C_RESET"
  printf '\n  Next step:\n\n      ./build.sh /path/to/your/rom.gbc\n\n'
  exit 0
fi
# Without a release the engine is cloned and the tools compiled from it. With
# a release (tarball or download) none of that is needed: the headers are
# embedded in the generated project and the tools arrive ready-made. That is
# why the whole engine block is skipped, not merely announced.
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

if [ -d "$ENGINE_DIR/.git/rebase-apply" ] || [ -d "$ENGINE_DIR/.git/rebase-merge" ]; then
  ok "patch already applied"
else
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
  if [ "$RELEASE_MODE" = "1" ]; then
    ok "the tools arrive ready: no engine, no compiling"
  else
    ok "skipping the engine: the headers come with the generated C"
    info "and the tools arrive ready from the release (no cmake, no ninja)"
  fi
fi

# --------------------------------------------- 3. the tools (gbrecomp)
step "Preparing the tools"

GBRECOMP="$TOOLS_DIR/gbrecomp"
if [ -x "$GBRECOMP" ]; then
  ok "gbrecomp already ready"
elif [ -n "$RELEASE_BASE" ]; then
  info "downloading prebuilt tools (v$TOOLS_VERSION)"
  ARCH="$(uname -m)"
  case "$ARCH" in x86_64) PLAT=linux-x86_64 ;; aarch64|arm64) PLAT=linux-arm64 ;;
    *) PLAT="linux-$ARCH" ;; esac
  TARBALL="$RELEASE_BASE/ghost-babel-tools-$PLAT-$TOOLS_VERSION.tar.gz"
  if have curl; then curl -fsSL "$TARBALL" -o "$TOOLS_DIR/tools.tar.gz"
  elif have wget; then wget -q "$TARBALL" -O "$TOOLS_DIR/tools.tar.gz"
  else die "I need curl or wget to download the tools." "$(install_hint curl)"
  fi
  # --strip-components=1: the tarball has a ghost-babel-tools/ directory at
  # the root, and we want bin/ and lib/ straight inside .tools/.
  tar -xzf "$TOOLS_DIR/tools.tar.gz" -C "$TOOLS_DIR" --strip-components=1 \
    || die "could not unpack the downloaded tools." "rm -f .tools/tools.tar.gz && ./bootstrap.sh"
  # The tarball carries bin/gbrecomp and lib/libgbrt.a; the rest of the script
  # expects both in .tools/. Without this copy, the next step would try to
  # compile a runtime that does not exist (the engine was never cloned).
  [ -f "$TOOLS_DIR/bin/gbrecomp" ] \
    || die "the downloaded package did not include gbrecomp." "$TARBALL"
  cp "$TOOLS_DIR/bin/gbrecomp" "$GBRECOMP" && chmod +x "$GBRECOMP"
  [ -f "$TOOLS_DIR/lib/libgbrt.a" ] \
    || die "the downloaded package did not include the runtime library." "$TARBALL"
  cp "$TOOLS_DIR/lib/libgbrt.a" "$TOOLS_DIR/libgbrt.a"
  # The runtime lands in one place, <repo>/runtime, whether it came from the
  # cloned engine (.tools/engine/runtime) or from the package
  # (.tools/runtime). That way the build and the packager have a single path
  # to look in.
  if [ -d "$TOOLS_DIR/runtime" ] && [ ! -d "$REPO_DIR/runtime" ]; then
    mv "$TOOLS_DIR/runtime" "$REPO_DIR/runtime"
  fi
  ok "tools ready (gbrecomp + runtime, from release v$TOOLS_VERSION)"
else
  warn "no release with ready tools yet - building from source."
  warn "this needs cmake and ninja (one step, ~2 minutes)."
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
  ok "gbrecomp compiled from source"
fi
printf '%s\n' "GBRECOMP=$GBRECOMP" >> "$TOOLS_DIR/env"

# --------------------------------------------- 4. prebuilt runtime library
step "Preparing the runtime (the library that makes the game run)"

LIB="$TOOLS_DIR/libgbrt.a"
RT="$ENGINE_DIR/runtime"

if [ -f "$LIB" ] && [ "$LIB" -nt "$ENGINE_DIR/.git/HEAD" ] 2>/dev/null; then
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

printf '\n%s%sAll set.%s\n' "$C_OK" "" "$C_RESET"
cat <<EOF

  Next step:

      ./build.sh /path/to/your/rom.gbc

  (or run ./run.sh --gui to pick the ROM in a window)

  Log of this run: $LOG_FILE

EOF