#!/usr/bin/env bash
#
# packaging/make_release.sh - builds the Linux release artifact from a WHITELIST.
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
[ -n "$VERSION" ] || { echo "usage: $0 <version>   (e.g. 0.6)"; exit 2; }

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
  remaster/remastmain.c
  remaster/native_presenter.c
  remaster/native_presenter.h
  remaster/camera_extent.c
  remaster/camera_extent.h
  remaster/room_record.c
  remaster/room_record.h
  remaster/room_layers.c
  remaster/room_layers.h
  remaster/room_map.c
  remaster/room_map.h
  remaster/room_links.c
  remaster/room_links.h
  remaster/room_compositor.c
  remaster/room_compositor.h
  remaster/learned_links.c
  remaster/learned_links.h
  remaster/room_actors.c
  remaster/room_actors.h
  remaster/room_controls.c
  remaster/room_controls.h
  remaster/room_rumble.c
  remaster/room_rumble.h
  remaster/codec_lipsync.c
  remaster/codec_lipsync.h
  remaster/sprite_shadow.c
  remaster/sprite_shadow.h
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

# The runtime source goes along because gbrecomp embeds a copy of it in the
# generated project and Makefile uses its headers.
RUNTIME_SRC=""
for candidate in .tools/engine/runtime runtime; do
  [ -d "$candidate" ] && { RUNTIME_SRC="$candidate"; break; }
done
[ -n "$RUNTIME_SRC" ] || { echo "MISSING the runtime: run ./bootstrap.sh"; exit 1; }
mkdir -p "$OUT/ghost-babel-tools/runtime"
cp -r "$RUNTIME_SRC"/. "$OUT/ghost-babel-tools/runtime/"

cp -r profiles "$OUT/ghost-babel-tools/profiles"

TARBALL="$OUT/ghost-babel-tools-linux-x86_64-$VERSION.tar.gz"
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
if tar -tzf "$TARBALL" | grep -Ei '\.(gbc|gb|sav|exe|dll)$|baserom|^ghost-babel-tools/build/|baserom_funcs|ghost-babel-(remaster|authentic)' ; then
  echo "  FAILED: the artifact contains ROM-derived material. ABORTED."
  exit 1
else
  echo "  ok: tools only."
fi
