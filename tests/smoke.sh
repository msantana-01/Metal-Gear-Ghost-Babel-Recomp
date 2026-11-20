#!/usr/bin/env bash
#
# tests/smoke.sh - proof that the whole path works, with nobody else's ROM.
#
# Generates a 32 KiB synthetic ROM (tests/make_test_rom.py), runs it through
# validation, generation, compilation with our Makefile (both authentic and
# widescreen remaster targets), checks that both binaries carry the platform
# layer, and runs them headless.
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
  [ -f "$s" ] || continue
  bash -n "$s" 2>/dev/null
  check $? "$s parses as bash"
done
for p in tools/validate_rom.py tools/pick_profile.py tools/gui/app.py tools/gui/make_icon.py tests/make_test_rom.py; do
  [ -f "$p" ] || continue
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
head_ "3. Generate and compile (authentic + remaster targets)"
[ -x .tools/gbrecomp ] || { bad "tools missing: run ./bootstrap.sh"; exit 1; }

RUNTIME_ARGS=()
for candidate in "$REPO/runtime" "$REPO/.tools/engine/runtime"; do
  [ -d "$candidate" ] && { RUNTIME_ARGS=(--runtime-dir "$candidate"); break; }
done

PROJ="$WORK/proj"
if ! .tools/gbrecomp "$ROM" -o "$PROJ" --output-prefix baserom "${RUNTIME_ARGS[@]}" > "$WORK/gbrecomp.log" 2>&1; then
  bad "gbrecomp generates the project (output below)"
  tail -15 "$WORK/gbrecomp.log" | sed 's/^/         /'
else
  ok "gbrecomp generates the project"
fi

N_C="$(ls "$PROJ"/*.c 2>/dev/null | wc -l)"
[ "$N_C" -ge 3 ]
check $? "gbrecomp produced $N_C C files"

rm -rf "$REPO/build/work"
make -f Makefile BUILD_REMASTER=1 ROM_SRC="$PROJ" \
     OUT_AUTH="$WORK/ghost-babel-authentic" \
     OUT_REMASTER="$WORK/ghost-babel-remaster" > "$WORK/make.log" 2>&1
check $? "Makefile compiles and links both authentic and remaster targets"

[ -x "$WORK/ghost-babel-authentic" ]
check $? "authentic binary produced and executable"
[ -x "$WORK/ghost-babel-remaster" ]
check $? "remaster binary produced and executable"

# ------------------------------------------------ the define & abi traps
head_ "4. Do both binaries carry the platform and presentation ABI?"
MAIN_O="$REPO/build/work/baserom_main.o"
N_PLATFORM="$(nm -u "$MAIN_O" 2>/dev/null | grep -c gb_platform || true)"
[ "${N_PLATFORM:-0}" -gt 0 ]
check $? "authentic main references platform ($N_PLATFORM gb_platform_* symbols)"

REMAST_O="$REPO/build/work/remaster_remastmain.o"
N_REMAST_PLAT="$(nm -u "$REMAST_O" 2>/dev/null | grep -c gb_platform || true)"
[ "${N_REMAST_PLAT:-0}" -gt 0 ]
check $? "remaster main references platform ($N_REMAST_PLAT gb_platform_* symbols)"

N_PRES="$(nm -u "$REMAST_O" 2>/dev/null | grep -c gbrt_presentation_source_init || true)"
[ "${N_PRES:-0}" -gt 0 ]
check $? "remaster links gbrt_presentation_source_init"

N_SHA="$(nm -u "$REMAST_O" 2>/dev/null | grep -c gbrt_sha256 || true)"
[ "${N_SHA:-0}" -gt 0 ]
check $? "remaster links gbrt_sha256 (auto ROM hash)"

# ------------------------------------------------------- alive or dead
head_ "5. Do the binaries execute?"
timeout 60 "$WORK/ghost-babel-authentic" --headless --limit 200000 > "$WORK/run_auth.log" 2>&1
check $? "authentic binary executes guest instructions"

timeout 10 "$WORK/ghost-babel-remaster" --help > "$WORK/run_remast.log" 2>&1
check $? "remaster binary executes cleanly (--help exit 0)"

rm -rf "$REPO/build/work"

printf '\n'
if [ "$FAIL" -eq 0 ]; then
  printf '\033[32m%d checks passed.\033[0m\n' "$PASS"
  exit 0
fi
printf '\033[31m%d failed\033[0m (of %d)\n' "$FAIL" "$((PASS+FAIL))"
exit 1
