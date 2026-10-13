#!/usr/bin/env bash
#
# tools/guard_copyright.sh — falha se artefato derivado de ROM entrar no repo.
#
# Este projeto distribui FERRAMENTAS. A ROM, o C gerado a partir dela e o
# executável resultante são o jogo — obra derivada, e obra de quem não é
# nosso. Nenhum deles pode ser commitado, nem "só para testar".
#
# Por que um script, e não só um .gitignore: o .gitignore é um pedido, e
# pedidos são esquecidos no momento exato em que a tentação aparece — um
# `git add -A` depois de compilar. Isto é uma trava: sai com erro.
#
# Uso:  ./tools/guard_copyright.sh [--staged]
# Sem argumentos, verifica tudo que está versionado.

set -uo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

FAIL=0
note() { printf '  %s\n' "$1"; }
fail() { printf '\033[31m  FALHA\033[0m %s\n' "$1"; FAIL=1; }

# Padrões de arquivo derivado de ROM.
PATTERNS='\.(gbc|gb|sav|rtc|bin)$|baserom.*\.(c|h|json)$|gbrecomp-contract\.json$|ghost-babel(_[a-z]+)?$'

if [ "${1:-}" = "--staged" ]; then
  FILES="$(git diff --cached --name-only --diff-filter=ACM)"
  SCOPE="staged para commit"
else
  FILES="$(git ls-files)"
  SCOPE="versionados no repositório"
fi

[ -z "$FILES" ] && { echo "guard_copyright: nada para checar."; exit 0; }

# 1. Arquivos proibidos
HIT="$(printf '%s\n' "$FILES" | grep -Ei "$PATTERNS" || true)"
HIT="$(printf '%s\n' "$HIT" | grep -vE '^tests/' || true)"   # fixtures sintéticos são legais
if [ -n "$HIT" ]; then
  fail "estes arquivos são derivados de ROM e estão $SCOPE:"
  printf '%s\n' "$HIT" | sed 's/^/         /'
  note "Se for a ROM que VOCÊ baixou, ela não pertence a este repositório."
fi

# 2. Diretórios que nunca são versionados
DIRS="$(printf '%s\n' "$FILES" | grep -E '^(build|\.tools|output|rom|roms)/' || true)"
if [ -n "$DIRS" ]; then
  fail "conteúdo de build $SCOPE:"
  printf '%s\n' "$DIRS" | head -5 | sed 's/^/         /'
fi

# 3. Tamanho: uma ROM de Game Boy tem no mínimo 32 KiB, e as deste jogo, MB.
BIG="$(printf '%s\n' "$FILES" | while read -r f; do
  [ -f "$f" ] || continue
  sz=$(stat -c %s "$f" 2>/dev/null || stat -f%z "$f" 2>/dev/null || echo 0)
  [ "$sz" -gt 1048576 ] && printf '%s (%s bytes)\n' "$f" "$sz"
done)"
if [ -n "$BIG" ]; then
  fail "arquivos grandes demais para serem ferramenta (>1 MiB) $SCOPE:"
  printf '%s\n' "$BIG" | sed 's/^/         /'
fi

# 4. O patch do motor tem que ser o que está pinado (nada de trocar por
#    arquivo grande disfarçado de patch).
PATCH="patches/gb-recompiled-ghost-babel.patch"
if [ -f "$PATCH" ] && command -v sha256sum >/dev/null; then
  WANT="$(sed -n 's/^patch_sha256[[:space:]]*=[[:space:]]*//p' pins.lock | head -1)"
  GOT="$(sha256sum "$PATCH" | awk '{print $1}')"
  if [ -n "$WANT" ] && [ "$WANT" != "$GOT" ]; then
    fail "o patch do motor mudou de depois de pinado (pins.lock não bate)"
  fi
fi

if [ "$FAIL" -eq 0 ]; then
  printf '\033[32mguard_copyright: nada derivado de ROM %s (%d arquivos).\033[0m\n' \
    "$SCOPE" "$(printf '%s\n' "$FILES" | wc -l)"
fi
exit $FAIL