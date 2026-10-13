#!/usr/bin/env bash
#
# tests/smoke.sh — prova de fumaça do caminho inteiro, sem ROM de ninguém.
#
# Gera uma ROM sintética de 32 KiB (tests/make_test_rom.py), passa por
# validação, geração, compilação pelo nosso Makefile e link, e verifica
# que o binário realmente carrega a camada de plataforma.
#
# Uso:  ./tests/smoke.sh
# Saída: 0 = tudo passou, 1 = algo quebrou.

set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/gbrecomp-smoke.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

PASS=0; FAIL=0
ok()   { printf '  \033[32mok\033[0m   %s\n' "$1"; PASS=$((PASS+1)); }
bad()  { printf '  \033[31mFALHA\033[0m %s\n' "$1"; FAIL=$((FAIL+1)); }
head_() { printf '\n\033[1m%s\033[0m\n' "$1"; }
check() { if [ "$1" = 0 ]; then ok "$2"; else bad "$2"; fi; }

cd "$REPO"

# --------------------------------------------------------------- sintaxe
head_ "1. Sintaxe dos scripts"
for s in bootstrap.sh build.sh run.sh tests/smoke.sh; do
  bash -n "$s" 2>/dev/null
  check $? "$s compila como bash"
done
for p in tools/validate_rom.py tools/pick_profile.py tools/gui/app.py tests/make_test_rom.py; do
  python3 -m py_compile "$p" 2>/dev/null
  check $? "$p compila como python"
done

# -------------------------------------------------------------- validador
head_ "2. Validação de ROM"
ROM="$WORK/testrom.gbc"
python3 tests/make_test_rom.py "$ROM" >/dev/null 2>&1
check $? "gera a ROM sintética"

python3 tools/validate_rom.py -q "$ROM" >/dev/null 2>&1
check $? "aceita a ROM sintética"

printf 'lixo' > "$WORK/lixo.gb"
python3 tools/validate_rom.py -q "$WORK/lixo.gb" >/dev/null 2>&1
[ $? -ne 0 ] && check 0 "recusa arquivo que não é ROM" || check 1 "recusa arquivo que não é ROM"

head -c 20000 "$ROM" > "$WORK/truncada.gbc"   # cortei a ROM pela metade
python3 tools/validate_rom.py -q "$WORK/truncada.gbc" >/dev/null 2>&1
[ $? -ne 0 ] && check 0 "recusa ROM truncada" || check 1 "recusa ROM truncada"

# --------------------------------------------------------- caminho inteiro
head_ "3. Gerar e compilar (Makefile do usuário)"
[ -x .tools/gbrecomp ] || { bad "ferramentas ausentes: rode ./bootstrap.sh"; exit 1; }

PROJ="$WORK/proj"
.tools/gbrecomp "$ROM" -o "$PROJ" > "$WORK/gbrecomp.log" 2>&1
check $? "gbrecomp gera o projeto"

N_C="$(ls "$PROJ"/*.c 2>/dev/null | wc -l)"
[ "$N_C" -ge 3 ]
check $? "gbrecomp gerou $N_C arquivos C"

make -f Makefile ROM_SRC="$PROJ" OUT="$WORK/ghost-babel" > "$WORK/make.log" 2>&1
check $? "Makefile compila e linka"

[ -x "$WORK/ghost-babel" ]
check $? "binário gerado e executável"

# ---------------------------------------------------- a armadilha do define
head_ "4. O binário tem mesmo a camada de plataforma?"
#
# Sem -DGB_HAS_SDL2 o C gerado nem inclui o cabeçalho da plataforma: o
# binário linka igual, mas nasce sem vídeo, sem input e sem dump. Linkar não
# prova que o jogo funciona — esta checagem existe por causa disso.
MAIN_O="$REPO/build/work/testrom_main.o"
N_PLATFORM="$(nm -u "$MAIN_O" 2>/dev/null | grep -c gb_platform)"
[ "${N_PLATFORM:-0}" -gt 0 ]
check $? "o main gerado referencia a plataforma ($N_PLATFORM símbolos gb_platform_*)"

# ------------------------------------------------------------- vivo ou morto
head_ "5. O runtime executa"
timeout 60 "$WORK/ghost-babel" --headless --limit 200000 > "$WORK/run.log" 2>&1
check $? "o binário executa instruções do guest (rom sintética não chega a frame)"

printf '\n'
if [ "$FAIL" -eq 0 ]; then
  printf '\033[32m%d verificações passaram.\033[0m\n' "$PASS"
  exit 0
fi
printf '\033[31m%d falharam\033[0m (de %d)\n' "$FAIL" "$((PASS+FAIL))"
echo "Logs em: (removidos ao sair; rode de novo com WORK=/tmp/x para preservar)"
exit 1