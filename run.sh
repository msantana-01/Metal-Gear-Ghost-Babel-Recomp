#!/usr/bin/env bash
#
# run.sh - runs the compiled game.
#
#   ./run.sh              run Widescreen Remaster (or single target for generic ROMs)
#   ./run.sh --authentic  run 160px Authentic build
#   ./run.sh --gui        open the window to pick a ROM and build it

set -uo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REMASTER_BIN="$REPO_DIR/build/ghost-babel-remaster"
AUTH_BIN="$REPO_DIR/build/ghost-babel-authentic"
LEGACY_BIN="$REPO_DIR/build/ghost-babel"

case "${1:-}" in
  --gui)
    exec python3 "$REPO_DIR/tools/gui/app.py"
    ;;
  --authentic)
    shift
    if [ -x "$AUTH_BIN" ]; then
      exec "$AUTH_BIN" "$@"
    elif [ -x "$LEGACY_BIN" ]; then
      exec "$LEGACY_BIN" "$@"
    fi
    ;;
  *)
    if [ -x "$REMASTER_BIN" ]; then
      exec "$REMASTER_BIN" --widescreen "$@"
    elif [ -x "$AUTH_BIN" ]; then
      exec "$AUTH_BIN" "$@"
    elif [ -x "$LEGACY_BIN" ]; then
      exec "$LEGACY_BIN" "$@"
    fi
    ;;
esac

printf 'The game has not been built yet.\n\n'
printf 'Run:\n\n    ./build.sh /path/to/your-rom.gbc\n\n'
printf 'Or pick the ROM in a window instead:\n\n    ./run.sh --gui\n\n'
exit 1
