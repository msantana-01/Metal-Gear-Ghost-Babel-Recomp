#!/usr/bin/env bash
#
# build.sh - compiles YOUR ROM into a game that runs on this computer.
#
#   ./build.sh my-rom.gbc
#
# What happens here:
#   1. check the file really is a Game Boy ROM
#   2. turn the ROM into C (gbrecomp)
#   3. compile that C and link it with the runtime (make)
#   4. actually run the result for a moment to prove it is alive
#
# None of this is specific to one game: any Game Boy ROM goes through here.

set -uo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLS_DIR="$REPO_DIR/.tools"
BUILD_DIR="$REPO_DIR/build"
LOG_FILE="$REPO_DIR/.build.log"

C_RESET=$'\033[0m'; C_OK=$'\033[32m'; C_ERR=$'\033[31m'; C_HDR=$'\033[1;36m'
[ -t 1 ] || { C_RESET=; C_OK=; C_ERR=; C_HDR=; }

step() { printf '\n%s==> %s%s\n' "$C_HDR" "$1" "$C_RESET"; }
ok()   { printf '    %sok%s   %s\n' "$C_OK" "$C_RESET" "$1"; }
info() { printf '    -    %s\n' "$1"; }

usage() {
  cat <<EOF
usage: ./build.sh <rom-file>

  example:
      ./build.sh ~/games/ghost-babel.gbc

When it is done, run:
      ./run.sh
EOF
}

[ $# -ge 1 ] || { usage; exit 2; }
ROM="$1"

case "$ROM" in
  -h|--help) usage; exit 0 ;;
esac

[ -x "$REPO_DIR/.tools/gbrecomp" ] || {
  printf '%sYou have not run setup yet.%s\n' "$C_ERR" "$C_RESET" >&2
  printf 'Run this first, from the project folder:\n\n    ./bootstrap.sh\n\n' >&2
  exit 1
}
command -v make >/dev/null 2>&1 || {
  printf '%smake is not installed.%s\n\n' "$C_ERR" "$C_RESET" >&2
  printf '    sudo apt-get install -y build-essential   # Debian/Ubuntu\n' >&2
  printf '    sudo pacman -S --needed base-devel       # Arch\n' >&2
  printf '    sudo dnf install -y gcc-c++ make         # Fedora\n\n' >&2
  exit 1
}

: > "$LOG_FILE"

# --------------------------------------------------------------- 1. the ROM
step "Checking the ROM"
if ! python3 "$REPO_DIR/tools/validate_rom.py" "$ROM"; then
  printf '\n%sNothing was compiled.%s Check the file path.\n' "$C_ERR" "$C_RESET" >&2
  exit 1
fi

ROM_ABS="$(cd "$(dirname "$ROM")" && pwd)/$(basename "$ROM")"

# --------------------------------------------------------------- 2. generate
step "Translating the ROM"
info "this takes a minute or two; the ROM becomes ~280 C files"
rm -rf "$BUILD_DIR/project" "$BUILD_DIR/work"
mkdir -p "$BUILD_DIR"

# Analysis profile: some ROMs need extra entry points so the analyzer reaches
# every code path (see profiles/*.json). Only addresses and titles are used -
# nothing of the game itself is distributed.
PROFILE_ARGS="$(python3 "$REPO_DIR/tools/pick_profile.py" "$ROM_ABS" 2>/dev/null || true)"
if [ -n "$PROFILE_ARGS" ]; then
  info "analysis profile: $(python3 "$REPO_DIR/tools/pick_profile.py" --name "$ROM_ABS" 2>/dev/null || echo ROM)"
fi

# gbrecomp embeds a copy of the runtime in the generated project (the
# Makefile uses its headers). It tries to find that runtime on its own, but
# the path baked into the binary is the one from the build it was compiled
# in - which only exists on the machine that compiled it. So we point at it
# explicitly, otherwise the tool fails on every other machine.
RUNTIME_DIR=""
for candidate in "$REPO_DIR/runtime" "$REPO_DIR/.tools/engine/runtime"; do
  [ -d "$candidate" ] && { RUNTIME_DIR="$candidate"; break; }
done
if [ -n "$RUNTIME_DIR" ]; then
  GBRECOMP_RUNTIME_ARGS=(--runtime-dir "$RUNTIME_DIR")
  info "runtime: ${RUNTIME_DIR#$REPO_DIR/}"
else
  GBRECOMP_RUNTIME_ARGS=()
  warn "achei o gbrecomp mas nao o runtime dele; se a geracao falhar, o bootstrap esta incompleto"
fi

# The GB* lines are read by the GUI to show progress. They are markers, not
# prose, so the interface never has to guess what stage we are in.
echo "GBPHASE translate"

# shellcheck disable=SC2086
if ! "$TOOLS_DIR/gbrecomp" "$ROM_ABS" -o "$BUILD_DIR/project" \
      "${GBRECOMP_RUNTIME_ARGS[@]}" $PROFILE_ARGS >>"$LOG_FILE" 2>&1; then
  printf '\n%sCode generation failed.%s\n' "$C_ERR" "$C_RESET" >&2
  printf 'This can mean an unsupported ROM. The log says:\n\n' >&2
  tail -20 "$LOG_FILE" >&2
  printf '\nFull log: %s\n' "$LOG_FILE" >&2
  exit 1
fi
N_C="$(ls "$BUILD_DIR/project"/*.c 2>/dev/null | wc -l)"
ok "$N_C C files generated"

# ---------------------------------------------------------------- 3. compile
step "Compiling (a few minutes)"
echo "GBPHASE compile"
# make writes to BOTH the log and stdout: the GUI reads stdout to show real
# progress, and .build.log keeps the full record. PIPESTATUS is needed here
# because `if ! make | tee` would only ever report tee's status, and a failed
# compile would look like a success.
make -C "$REPO_DIR" -f Makefile --no-print-directory -j"${JOBS:-$(nproc 2>/dev/null || echo 4)}" \
     ROM_SRC="$BUILD_DIR/project" OUT="$BUILD_DIR/ghost-babel" 2>&1 | tee -a "$LOG_FILE"
MAKE_RC="${PIPESTATUS[0]}"
if [ "$MAKE_RC" -ne 0 ]; then
  printf '\n%sCompilation failed.%s\n' "$C_ERR" "$C_RESET" >&2
  tail -20 "$LOG_FILE" >&2
  printf '\nFull log: %s\n' "$LOG_FILE" >&2
  exit 1
fi
ok "binary: $BUILD_DIR/ghost-babel ($(du -h "$BUILD_DIR/ghost-babel" | cut -f1))"

# ----------------------------------------------------------------- 4. smoke
# Compiling is not enough: if the runtime define is missing the binary still
# links, but it is born without video, input or save states. Only running it
# tells us the truth.
#
# The timeout is not paranoia: a ROM whose emulation never reaches a frame
# left the build hanging forever, and whoever waited assumed it was still
# compiling. (The clean-distro test found this, with a minimal test ROM.)
step "Testing the result"
SMOKE_TIMEOUT="${SMOKE_TIMEOUT:-120}"
timeout "$SMOKE_TIMEOUT" "$BUILD_DIR/ghost-babel" --headless --limit-frames 120 >/dev/null 2>&1
SMOKE_RC=$?

case "$SMOKE_RC" in
  0)
    ok "the game ran 120 frames without errors" ;;
  124)
    printf '\n%sIt compiled, but produced no picture within %ss.%s\n' \
      "$C_ERR" "$SMOKE_TIMEOUT" "$C_RESET" >&2
    printf 'This happens with ROMs the analyzer does not fully cover; the\n' >&2
    printf 'build may still work. To skip this check:\n\n' >&2
    printf '    SMOKE_TIMEOUT=1 ./build.sh %s\n\n' "$ROM" >&2
    printf 'If you believe the ROM is fine, please open an issue with the log:\n' >&2
    printf '    %s\n' "$LOG_FILE" >&2
    exit 1 ;;
  *)
    printf '\n%sIt compiled but did not run (exit code %s).%s\n' "$C_ERR" "$SMOKE_RC" "$C_RESET" >&2
    printf 'This is our bug. Please report it on GitHub with the log:\n\n' >&2
    printf '    %s\n\n' "$LOG_FILE" >&2
    exit 1 ;;
esac

echo "GBPHASE done"

# ------------------------------------------------------ 5. reclaim the disk
# The generated C and the object files add up to hundreds of megabytes, and
# the game never reads them: the executable links only against system
# libraries. So we can offer to delete them - after the build worked, never
# before, and never without asking.
GEN_KB="$(du -sk "$BUILD_DIR/project" "$BUILD_DIR/work" 2>/dev/null | awk '{s+=$1} END {print s+0}')"
if [ "${GEN_KB:-0}" -gt 51200 ] && [ -t 0 ] && [ -t 1 ]; then
  printf '\n'
  printf '  The generated code and the build files take %s MB on disk.\n' "$((GEN_KB / 1024))"
  printf '  The game does not need them; it only needs build/ghost-babel.\n'
  printf '  Your save file (build/baserom.sav) is not touched.\n'
  printf '  Delete them now? [Y/n] '
  read -r answer || answer=""
  case "$answer" in
    [nN]*|"no"*|"No"*) ;;
    *) rm -rf "$BUILD_DIR/project" "$BUILD_DIR/work"
       printf '  Removed. Free space; rebuild with ./build.sh if you want them back.\n' ;;
  esac
fi

printf '\n%s%sDone!%s\n\n' "$C_OK" "" "$C_RESET"
cat <<EOF
  To play:   ./run.sh
  To rebuild with another ROM: ./build.sh <other-rom>
  To free this space later:    make clean

EOF