#!/usr/bin/env bash
#
# packaging/make_release.sh - builds the release artifact from a WHITELIST.
#
# The list is written file by file, never "whatever is in the folder". The
# difference matters: a folder glob will one day include build/ and ship the
# game with the tools.
#
# Usage:  ./packaging/make_release.sh <version>

set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

VERSION="${1:-}"
[ -n "$VERSION" ] || { echo "usage: $0 <version>   (e.g. 1.0.0)"; exit 2; }

# Copyright lock BEFORE packing anything.
bash tools/guard_copyright.sh || { echo "ABORTED: the copyright guard failed."; exit 1; }

OUT="packaging/dist"
rm -rf "$OUT"; mkdir -p "$OUT/ghost-babel-tools/bin" "$OUT/ghost-babel-tools/lib"

# ---- whitelist: only this goes into the artifact
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
  [ -f "$f" ] || { echo "MISSING from the repo: $f"; exit 1; }
  mkdir -p "$OUT/ghost-babel-tools/$(dirname "$f")"
  cp "$f" "$OUT/ghost-babel-tools/$f"
done

# ---- compiled tools (never the game)
[ -x .tools/gbrecomp ] || { echo "MISSING .tools/gbrecomp: run ./bootstrap.sh"; exit 1; }
[ -f .tools/libgbrt.a ] || { echo "MISSING .tools/libgbrt.a: run ./bootstrap.sh"; exit 1; }
cp .tools/gbrecomp "$OUT/ghost-babel-tools/bin/gbrecomp"
cp .tools/libgbrt.a "$OUT/ghost-babel-tools/lib/libgbrt.a"

# The runtime source goes along because gbrecomp needs to EMBED a copy of it
# in the generated project (those are the headers the Makefile uses). The
# binary carries the absolute path of the machine it was built on, which only
# exists there: without this directory in the package, gbrecomp fails
# everywhere else with "Could not locate the runtime snapshot".
#
# The runtime can be in two places: in the engine clone (when the tools were
# compiled from source) or at the root (when they arrived from a release).
RUNTIME_SRC=""
for candidate in .tools/engine/runtime runtime; do
  [ -d "$candidate" ] && { RUNTIME_SRC="$candidate"; break; }
done
[ -n "$RUNTIME_SRC" ] || { echo "MISSING the runtime: run ./bootstrap.sh"; exit 1; }
mkdir -p "$OUT/ghost-babel-tools/runtime"
cp -r "$RUNTIME_SRC"/. "$OUT/ghost-babel-tools/runtime/"

# GUI (source, not bundled, in this release)
mkdir -p "$OUT/ghost-babel-tools/tools/gui"
cp tools/gui/app.py "$OUT/ghost-babel-tools/tools/gui/"
cp -r profiles "$OUT/ghost-babel-tools/profiles"

TARBALL="$OUT/ghost-babel-tools-linux-x86_64-$VERSION.tar.gz"
# --owner/--group/--numeric-owner: an archive that carries the uid/gid of
# whoever built it fails to unpack where that uid does not exist - extracting
# as root, or inside a container with a restricted uid map, dies with
# "Cannot change ownership to uid 1000". Ownership in a release artifact is
# meaningless anyway; normalising it also makes the tarball reproducible.
tar -czf "$TARBALL" --owner=0 --group=0 --numeric-owner -C "$OUT" ghost-babel-tools
( cd "$OUT" && sha256sum "$(basename "$TARBALL")" > "$(basename "$TARBALL").sha256" )

echo
echo "artifact:  $TARBALL ($(du -h "$TARBALL" | cut -f1))"
echo "sha256:    $(awk '{print $1}' "$TARBALL.sha256")"
echo
echo "contents (top level):"
tar -tzf "$TARBALL" | grep -E '^ghost-babel-tools/[^/]+/?$' | sed 's/^/  /'
echo
echo "checking that nothing ROM-derived is in the artifact:"
if tar -tzf "$TARBALL" | grep -Ei '\.(gbc|gb|sav)$|baserom|^ghost-babel-tools/build/|baserom_funcs' ; then
  echo "  FAILED: the artifact contains ROM-derived material. ABORTED."
  exit 1
else
  echo "  ok: tools only."
fi