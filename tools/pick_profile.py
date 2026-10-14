#!/usr/bin/env python3
"""Chooses the analysis profile that fits a ROM.

A profile is just a few extra options for gbrecomp. Today there is one, for
the Ghost Babel cartridge: three entry points the analyzer does not find on
its own.

Profiles contain only facts about the ROM (code addresses, title) - no game
code, data or asset. It is the same nature as a patch file: addresses, not
content.

Usage:
    python3 tools/pick_profile.py <rom>            # prints the extra options
    python3 tools/pick_profile.py --name <rom>     # prints the profile name
"""

from __future__ import annotations

import argparse
import json
import shlex
import sys
from pathlib import Path

PROFILES_DIR = Path(__file__).resolve().parent.parent / "profiles"


def load_profiles() -> list[dict]:
    if not PROFILES_DIR.is_dir():
        return []
    out = []
    for f in sorted(PROFILES_DIR.glob("*.json")):
        try:
            out.append(json.loads(f.read_text()))
        except (OSError, json.JSONDecodeError) as exc:  # pragma: no cover
            print(f"warning: unreadable profile {f.name}: {exc}", file=sys.stderr)
    return out


def rom_title(path: Path) -> str:
    with path.open("rb") as fh:
        fh.seek(0x134)
        raw = fh.read(16).split(b"\x00", 1)[0]
    return "".join(chr(b) for b in raw if 32 <= b < 127).strip()


def match(rom: Path, profiles: list[dict]) -> dict | None:
    """First profile whose title and/or size match the ROM."""
    title = rom_title(rom)
    size = rom.stat().st_size
    for prof in profiles:
        want_title = prof.get("title")
        want_size = prof.get("size_bytes")
        if want_title is None and want_size is None:
            continue  # a profile with no key would match every ROM
        if want_title is not None and want_title != title:
            continue
        if want_size is not None and want_size != size:
            continue
        return prof
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--name", action="store_true",
                    help="print the profile name instead of its options")
    ap.add_argument("rom", type=Path)
    args = ap.parse_args()

    if not args.rom.is_file():
        return 1
    prof = match(args.rom, load_profiles())
    if not prof:
        return 0
    if args.name:
        print(prof.get("name", "unknown"))
    else:
        print(" ".join(shlex.quote(a) for a in prof.get("gbrecomp_args", [])))
    return 0


if __name__ == "__main__":
    sys.exit(main())