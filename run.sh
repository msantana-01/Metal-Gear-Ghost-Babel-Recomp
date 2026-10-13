#!/usr/bin/env bash
#
# run.sh — roda o jogo compilado.
#
#   ./run.sh            roda normalmente
#   ./run.sh --gui      abre a janelinha para escolher a ROM e compilar

set -uo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$REPO_DIR/build/ghost-babel"

if [ "${1:-}" = "--gui" ]; then
  exec python3 "$REPO_DIR/tools/gui/app.py"
fi

if [ ! -x "$BIN" ]; then
  printf 'O jogo ainda nao foi compilado.\n\n'
  printf 'Rode:\n\n    ./build.sh /caminho/para/sua-rom.gbc\n\n'
  printf 'Se preferir escolher a ROM por uma janela:\n\n    ./run.sh --gui\n\n'
  exit 1
fi

exec "$BIN" "$@"