#!/usr/bin/env bash
#
# build.sh — compila a SUA ROM em um jogo que roda no PC.
#
#   ./build.sh minha-rom.gbc
#
# O que acontece aqui:
#   1. confere se o arquivo é mesmo uma ROM de Game Boy
#   2. transforma a ROM em código C (gbrecomp)
#   3. compila esse código e linka com o runtime (make)
#   4. testa o resultado de verdade, rodando o jogo alguns frames
#
# Nada disso é específico deste jogo: qualquer ROM de Game Boy passa por aqui.

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
uso: ./build.sh <arquivo-rom>

  exemplo:
      ./build.sh ~/games/ghost-babel.gbc

Depois de compilar, rode:
      ./run.sh
EOF
}

[ $# -ge 1 ] || { usage; exit 2; }
ROM="$1"

case "$ROM" in
  -h|--help) usage; exit 0 ;;
esac

[ -f "$REPO_DIR/.tools/gbrecomp" ] || {
  printf '%sVocê ainda não rodou o bootstrap.%s\n' "$C_ERR" "$C_RESET" >&2
  printf 'Rode isto primeiro, na pasta do projeto:\n\n    ./bootstrap.sh\n\n' >&2
  exit 1
}
command -v make >/dev/null 2>&1 || {
  printf '%sfalta o make.%s\n\n' "$C_ERR" "$C_RESET" >&2
  printf '    sudo apt-get install -y build-essential   # Debian/Ubuntu\n' >&2
  printf '    sudo pacman -S --needed base-devel       # Arch\n' >&2
  printf '    sudo dnf install -y gcc-c++ make         # Fedora\n\n' >&2
  exit 1
}

: > "$LOG_FILE"

# --------------------------------------------------------------- 1. ROM
step "Conferindo a ROM"
if ! python3 "$REPO_DIR/tools/validate_rom.py" "$ROM"; then
  printf '\n%sNada foi compilado.%s Confira o caminho do arquivo.\n' "$C_ERR" "$C_RESET" >&2
  exit 1
fi

ROM_ABS="$(cd "$(dirname "$ROM")" && pwd)/$(basename "$ROM")"

# --------------------------------------------------------------- 2. gerar
step "Gerando o código"
info "isso leva um ou dois minutos; são ~280 arquivos de C"
rm -rf "$BUILD_DIR/project"
mkdir -p "$BUILD_DIR"

# Perfil de análise: algumas ROMs precisam de pontos de entrada extras para
# que o analisador cubra todo o código (ver profiles/*.json). Só são usados
# endereços e títulos — nada do jogo é distribuído aqui.
PROFILE_ARGS="$(python3 "$REPO_DIR/tools/pick_profile.py" "$ROM_ABS" 2>/dev/null || true)"
if [ -n "$PROFILE_ARGS" ]; then
  PROFILE_NAME="$(python3 -c "import json,sys;print(json.load(open('$REPO_DIR/profiles/ghost-babel.json'))['name'])" 2>/dev/null || echo ROM)"
  info "perfil detectado: $PROFILE_NAME"
fi

# shellcheck disable=SC2086
if ! "$TOOLS_DIR/gbrecomp" "$ROM_ABS" -o "$BUILD_DIR/project" $PROFILE_ARGS >>"$LOG_FILE" 2>&1; then
  printf '\n%sA geração de código falhou.%s\n' "$C_ERR" "$C_RESET" >&2
  printf 'Isso pode ser uma ROM não suportada. Veja o log:\n\n' >&2
  tail -20 "$LOG_FILE" >&2
  printf '\nLog completo: %s\n' "$LOG_FILE" >&2
  exit 1
fi
N_C="$(ls "$BUILD_DIR/project"/*.c 2>/dev/null | wc -l)"
ok "$N_C arquivos C gerados"

# --------------------------------------------------------------- 3. compilar
step "Compilando (pode demorar alguns minutos)"
if ! make -C "$REPO_DIR" -f Makefile -j"${JOBS:-$(nproc 2>/dev/null || echo 4)}" \
      ROM_SRC="$BUILD_DIR/project" OUT="$BUILD_DIR/ghost-babel" >>"$LOG_FILE" 2>&1; then
  printf '\n%sA compilação falhou.%s\n' "$C_ERR" "$C_RESET" >&2
  tail -20 "$LOG_FILE" >&2
  printf '\nLog completo: %s\n' "$LOG_FILE" >&2
  exit 1
fi
ok "binário: $BUILD_DIR/ghost-babel ($(du -h "$BUILD_DIR/ghost-babel" | cut -f1))"

# --------------------------------------------------------------- 4. smoke
# Compilar não basta: se a define do runtime faltar, o binário linka mas
# nasce sem vídeo e sem input. Só rodando dá para saber.
step "Testando o resultado"
if "$BUILD_DIR/ghost-babel" --headless --limit-frames 120 >/dev/null 2>&1; then
  ok "o jogo rodou 120 frames sem erro"
else
  printf '\n%sO jogo compilou mas não rodou.%s\n' "$C_ERR" "$C_RESET" >&2
  printf 'Isso é um defeito nosso. Reporte no GitHub com o log:\n\n' >&2
  printf '    %s\n\n' "$LOG_FILE" >&2
  exit 1
fi

printf '\n%s%sPronto!%s\n\n' "$C_OK" "" "$C_RESET"
cat <<EOF
  Para jogar:   ./run.sh
  Para recompilar depois de trocar a ROM: ./build.sh <nova-rom>

EOF