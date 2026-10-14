#!/usr/bin/env bash
#
# packaging/make_release.sh — monta o artefato de release por LISTA BRANCA.
#
# A lista é escrita arquivo a arquivo, nunca "o que tem na pasta". A diferença
# importa: um glob de pasta um dia vai incluir build/ e levar o jogo junto.
#
# Uso:  ./packaging/make_release.sh [versao]

set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

VERSION="${1:-}"
[ -n "$VERSION" ] || { echo "uso: $0 <versao>   (ex: 1.0.0)"; exit 2; }

# Trava de copyright ANTES de empacotar qualquer coisa.
bash tools/guard_copyright.sh || { echo "ABORTADO: guarda de copyright reprovou."; exit 1; }

OUT="packaging/dist"
rm -rf "$OUT"; mkdir -p "$OUT/ghost-babel-tools/bin" "$OUT/ghost-babel-tools/lib"

# ---- lista branca: só isto entra no artefato
FILES=(
  README.md
  LICENSE
  pins.lock
  bootstrap.sh
  build.sh
  run.sh
  Makefile
  patches/gb-recompiled-ghost-babel.patch
  tools/validate_rom.py
  tools/pick_profile.py
  tools/guard_copyright.sh
  tools/gui/app.py
  tools/gui/icon.png
  tools/gui/make_icon.py
  tests/make_test_rom.py
  tests/smoke.sh
  tests/container.sh
)

for f in "${FILES[@]}"; do
  [ -f "$f" ] || { echo "FALTA no repo: $f"; exit 1; }
  mkdir -p "$OUT/ghost-babel-tools/$(dirname "$f")"
  cp "$f" "$OUT/ghost-babel-tools/$f"
done

# ---- ferramentas compiladas (nunca o jogo)
[ -x .tools/gbrecomp ] || { echo "FALTA .tools/gbrecomp: rode ./bootstrap.sh"; exit 1; }
[ -f .tools/libgbrt.a ] || { echo "FALTA .tools/libgbrt.a: rode ./bootstrap.sh"; exit 1; }
cp .tools/gbrecomp "$OUT/ghost-babel-tools/bin/gbrecomp"
cp .tools/libgbrt.a "$OUT/ghost-babel-tools/lib/libgbrt.a"

# O runtime source vai junto porque o gbrecomp precisa EMBUTIR uma cópia dele
# no projeto gerado (headers que o Makefile usa). O binario traz o caminho
# absoluto do build em que foi compilado, que só existe na máquina de quem
# compilou: sem este diretorio no pacote, o gbrecomp falha em qualquer outra
# maquina com "Could not locate the runtime snapshot".
# O runtime pode estar em dois lugares: no clone do engine (quando as
# ferramentas foram compiladas a partir do fonte) ou na raiz, empacotado
# (quando vieram prontas de um release anterior).
RUNTIME_SRC=""
for candidate in .tools/engine/runtime runtime; do
  [ -d "$candidate" ] && { RUNTIME_SRC="$candidate"; break; }
done
[ -n "$RUNTIME_SRC" ] || { echo "FALTA o runtime: rode ./bootstrap.sh"; exit 1; }
mkdir -p "$OUT/ghost-babel-tools/runtime"
cp -r "$RUNTIME_SRC"/. "$OUT/ghost-babel-tools/runtime/"

# GUI (opcional neste release: fonte, não empacotado)
mkdir -p "$OUT/ghost-babel-tools/tools/gui"
cp tools/gui/app.py "$OUT/ghost-babel-tools/tools/gui/"
cp -r profiles "$OUT/ghost-babel-tools/profiles"

TARBALL="$OUT/ghost-babel-tools-linux-x86_64-$VERSION.tar.gz"
tar -czf "$TARBALL" -C "$OUT" ghost-babel-tools
( cd "$OUT" && sha256sum "$(basename "$TARBALL")" > "$(basename "$TARBALL").sha256" )

echo
echo "artefato:  $TARBALL ($(du -h "$TARBALL" | cut -f1))"
echo "sha256:    $(awk '{print $1}' "$TARBALL.sha256")"
echo
echo "conteúdo (top-level):"
tar -tzf "$TARBALL" | grep -E '^ghost-babel-tools/[^/]+/?$' | sed 's/^/  /'
echo
echo "conferindo que não há nada derivado de ROM no artefato:"
if tar -tzf "$TARBALL" | grep -Ei '\.(gbc|gb|sav)$|baserom|^ghost-babel-tools/build/|baserom_funcs' ; then
  echo "  FALHA: o artefato contém material derivado de ROM. ABORTADO."
  exit 1
else
  echo "  ok: só ferramentas."
fi