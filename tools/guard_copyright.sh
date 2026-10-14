#!/usr/bin/env bash
#
# tools/guard_copyright.sh - fails if a ROM-derived artifact gets into the repo.
#
# This project distributes TOOLS. The ROM, the C generated from it and the
# executable produced from it are the game - derivative work, and work that
# is not ours to give away. None of it may be committed, not even "just to
# test".
#
# Why a script and not just a .gitignore: the .gitignore is a request, and
# requests get forgotten at exactly the moment the temptation shows up - a
# `git add -A` right after building. This is a lock: it exits with an error.
#
# Usage:  ./tools/guard_copyright.sh [--staged]
# Without arguments it checks everything that is tracked.

set -uo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

FAIL=0
note() { printf '  %s\n' "$1"; }
fail() { printf '\033[31m  FAILED\033[0m %s\n' "$1"; FAIL=1; }

# Patterns for files derived from a ROM.
PATTERNS='\.(gbc|gb|sav|rtc|bin)$|baserom.*\.(c|h|json)$|gbrecomp-contract\.json$|ghost-babel(_[a-z]+)?$'

if [ "${1:-}" = "--staged" ]; then
  FILES="$(git diff --cached --name-only --diff-filter=ACM)"
  SCOPE="staged for commit"
else
  FILES="$(git ls-files)"
  SCOPE="tracked in the repository"
fi

[ -z "$FILES" ] && { echo "guard_copyright: nothing to check."; exit 0; }

# 1. Forbidden files
HIT="$(printf '%s\n' "$FILES" | grep -Ei "$PATTERNS" || true)"
HIT="$(printf '%s\n' "$HIT" | grep -vE '^tests/' || true)"   # synthetic fixtures are legal
if [ -n "$HIT" ]; then
  fail "these files are derived from a ROM and are $SCOPE:"
  printf '%s\n' "$HIT" | sed 's/^/         /'
  note "If it is the ROM YOU downloaded, it does not belong in this repository."
fi

# 2. Directories that should never be tracked.
# packaging/dist/ is here because make_release.sh assembles a copy of all the
# tools there: tracking it looks harmless (it is only our own code), but it
# duplicates the repository and the next edit never reaches the copy.
DIRS="$(printf '%s\n' "$FILES" | grep -E '^(build|\.tools|output|rom|roms|runtime|packaging/dist)/' || true)"
if [ -n "$DIRS" ]; then
  fail "content that must never be tracked, $SCOPE:"
  printf '%s\n' "$DIRS" | head -5 | sed 's/^/         /'
  N_DIRS="$(printf '%s\n' "$DIRS" | wc -l)"
  [ "$N_DIRS" -gt 5 ] && printf '         ... and %d more\n' "$((N_DIRS - 5))"
fi

# 3. Size: a Game Boy ROM is at least 32 KiB, and the ones for this game are
# megabytes. Anything this size is not a tool.
BIG="$(printf '%s\n' "$FILES" | while read -r f; do
  [ -f "$f" ] || continue
  sz=$(stat -c %s "$f" 2>/dev/null || stat -f%z "$f" 2>/dev/null || echo 0)
  [ "$sz" -gt 1048576 ] && printf '%s (%s bytes)\n' "$f" "$sz"
done)"
if [ -n "$BIG" ]; then
  fail "files too large to be a tool (>1 MiB), $SCOPE:"
  printf '%s\n' "$BIG" | sed 's/^/         /'
fi

# 4. The engine patch has to be the pinned one (nothing large wearing a
#    patch's clothes).
PATCH="patches/gb-recompiled-ghost-babel.patch"
if [ -f "$PATCH" ] && command -v sha256sum >/dev/null; then
  WANT="$(sed -n 's/^patch_sha256[[:space:]]*=[[:space:]]*//p' pins.lock | head -1)"
  GOT="$(sha256sum "$PATCH" | awk '{print $1}')"
  if [ -n "$WANT" ] && [ "$WANT" != "$GOT" ]; then
    fail "the engine patch changed after being pinned (pins.lock does not match)"
  fi
fi

if [ "$FAIL" -eq 0 ]; then
  printf '\033[32mguard_copyright: nothing ROM-derived %s (%d files).\033[0m\n' \
    "$SCOPE" "$(printf '%s\n' "$FILES" | wc -l)"
fi
exit $FAIL