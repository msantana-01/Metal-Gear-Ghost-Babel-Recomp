#!/usr/bin/env bash
#
# run.sh - runs the compiled game.
#
#   ./run.sh            run it
#   ./run.sh --gui      open the window to pick a ROM and build it

set -uo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$REPO_DIR/build/ghost-babel"

if [ "${1:-}" = "--gui" ]; then
  exec python3 "$REPO_DIR/tools/gui/app.py"
fi

if [ ! -x "$BIN" ]; then
  printf 'The game has not been built yet.\n\n'
  printf 'Run:\n\n    ./build.sh /path/to/your-rom.gbc\n\n'
  printf 'Or pick the ROM in a window instead:\n\n    ./run.sh --gui\n\n'
  exit 1
fi

exec "$BIN" "$@"