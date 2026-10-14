#!/usr/bin/env bash
#
# tests/smoke.sh - proof that the whole path works, with nobody else's ROM.
#
# Generates a 32 KiB synthetic ROM (tests/make_test_rom.py), runs it through
# validation, generation, compilation with our Makefile and linking, and
# checks that the binary really carries the platform layer.
#
# Usage:  ./tests/smoke.sh
# Output: 0 = everything passed, 1 = something broke.

set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/gbrecomp-smoke.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

PASS=0; FAIL=0
ok()    { printf '  \033[32mok\033[0m    %s\n' "$1"; PASS=$((PASS+1)); }
bad()   { printf '  \033[31mFAILED\033[0m %s\n' "$1"; FAIL=$((FAIL+1)); }
head_() { printf '\n\033[1m%s\033[0m\n' "$1"; }
check() { if [ "$1" = 0 ]; then ok "$2"; else bad "$2"; fi; }

cd "$REPO"

# --------------------------------------------------------------- syntax
head_ "1. Script syntax"
for s in bootstrap.sh build.sh run.sh tests/smoke.sh packaging/make_release.sh tools/guard_copyright.sh; do
  bash -n "$s" 2>/dev/null
  check $? "$s parses as bash"
done
for p in tools/validate_rom.py tools/pick_profile.py tools/gui/app.py tools/gui/make_icon.py tests/make_test_rom.py; do
  python3 -m py_compile "$p" 2>/dev/null
  check $? "$p parses as python"
done

# ------------------------------------------------------------ validator
head_ "2. ROM validation"
ROM="$WORK/testrom.gbc"
python3 tests/make_test_rom.py "$ROM" >/dev/null 2>&1
check $? "generates the synthetic ROM"

python3 tools/validate_rom.py -q "$ROM" >/dev/null 2>&1
check $? "accepts the synthetic ROM"

printf 'rubbish' > "$WORK/rubbish.gb"
python3 tools/validate_rom.py -q "$WORK/rubbish.gb" >/dev/null 2>&1
[ $? -ne 0 ] && check 0 "rejects a file that is not a ROM" || check 1 "rejects a file that is not a ROM"

head -c 20000 "$ROM" > "$WORK/truncated.gbc"   # chopped in half
python3 tools/validate_rom.py -q "$WORK/truncated.gbc" >/dev/null 2>&1
[ $? -ne 0 ] && check 0 "rejects a truncated ROM" || check 1 "rejects a truncated ROM"

# ------------------------------------------------------- whole path
head_ "3. Generate and compile (the user's Makefile)"
[ -x .tools/gbrecomp ] || { bad "tools missing: run ./bootstrap.sh"; exit 1; }

PROJ="$WORK/proj"
if ! .tools/gbrecomp "$ROM" -o "$PROJ" > "$WORK/gbrecomp.log" 2>&1; then
  # Failing without saying why is the worst way to fail: whoever reads the CI
  # log needs to see what gbrecomp answered.
  bad "gbrecomp generates the project (output below)"
  tail -15 "$WORK/gbrecomp.log" | sed 's/^/         /'
else
  ok "gbrecomp generates the project"
fi

N_C="$(ls "$PROJ"/*.c 2>/dev/null | wc -l)"
[ "$N_C" -ge 3 ]
check $? "gbrecomp produced $N_C C files"

make -f Makefile ROM_SRC="$PROJ" OUT="$WORK/ghost-babel" > "$WORK/make.log" 2>&1
check $? "the Makefile compiles and links"

[ -x "$WORK/ghost-babel" ]
check $? "binary produced and executable"

# ------------------------------------------------ the define trap
head_ "4. Does the binary really have the platform layer?"
#
# Without -DGB_HAS_SDL2 the generated C does not even include the platform
# header: the binary links just the same, but is born without video, input or
# dumping. Linking is no proof that the game works - this check exists because
# of that.
MAIN_O="$REPO/build/work/testrom_main.o"
N_PLATFORM="$(nm -u "$MAIN_O" 2>/dev/null | grep -c gb_platform)"
[ "${N_PLATFORM:-0}" -gt 0 ]
check $? "the generated main references the platform ($N_PLATFORM gb_platform_* symbols)"

# ------------------------------------------------------- alive or dead
head_ "5. Does the runtime execute?"
timeout 60 "$WORK/ghost-babel" --headless --limit 200000 > "$WORK/run.log" 2>&1
check $? "the binary executes guest instructions (the synthetic ROM never reaches a frame)"

printf '\n'
if [ "$FAIL" -eq 0 ]; then
  printf '\033[32m%d checks passed.\033[0m\n' "$PASS"
  exit 0
fi
printf '\033[31m%d failed\033[0m (of %d)\n' "$FAIL" "$((PASS+FAIL))"
exit 1