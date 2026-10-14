#!/usr/bin/env python3
"""Checks that a file really looks like a Game Boy ROM.

The rules are deliberately conservative: this tool inspects the cartridge
*structure* (manufacturer signature, size, header coherence) and never its
*content*. We publish no ROM hash and compute none - a published hash would
identify someone's dump.

Exit code: 0 = looks like a valid ROM, 1 = it does not, 2 = bad usage.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

# Every Game Boy ROM starts with this signature.
SIGNATURES = {
    bytes([0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73,
           0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D]): "Nintendo",
    bytes([0xFF] * 16): "unsigned (homebrew/copy)",
}

MBC_NAMES = {
    0x00: "ROM", 0x01: "MBC1", 0x02: "MBC1+RAM", 0x03: "MBC1+RAM+BATTERY",
    0x05: "MBC2", 0x06: "MBC2+BATTERY", 0x08: "ROM+RAM", 0x09: "ROM+RAM+BATTERY",
    0x0B: "MMM01", 0x0C: "MMM01+RAM", 0x0D: "MMM01+RAM+BATTERY",
    0x0F: "MMM01+RAM+BATTERY", 0x10: "MBC3", 0x11: "MBC3+RAM",
    0x12: "MBC3+RAM+BATTERY", 0x13: "MBC3+BATTERY",
    0x19: "MBC5", 0x1A: "MBC5+RAM", 0x1B: "MBC5+RAM+BATTERY",
    0x1C: "MBC5+RUMBLE", 0x1D: "MBC5+RUMBLE+RAM", 0x1E: "MBC5+RUMBLE+RAM+BATTERY",
}

RAM_SIZES = {0: "none", 1: "2 KiB", 2: "8 KiB", 3: "32 KiB", 4: "128 KiB", 5: "64 KiB"}


def _title(raw: bytes) -> str:
    """The title area, minus control bytes and junk that is not a title."""
    chunk = raw[0x134:0x144].split(b"\x00", 1)[0]
    text = "".join(chr(b) for b in chunk if 32 <= b < 127)
    return " ".join(text.split()) if text else "(no title)"


def _header_ok(data: bytes) -> tuple[bool, int, str]:
    code = data[0x148]
    if code > 0x08:
        return False, 0, ""
    declared = (32 << code) * 1024  # 0x00 = 32 KiB ... 0x08 = 8 MiB
    if len(data) == declared:
        return True, declared, ""
    return False, declared, (
        f"the header declares {declared:,} bytes (code {code:#04x}) but the file has "
        f"{len(data):,} bytes. Truncated file, or a different game."
    )


def validate(path: Path) -> tuple[bool, list[str], list[str]]:
    """Returns (ok, errors, human-readable info lines)."""
    errors: list[str] = []
    infos: list[str] = []

    if not path.exists():
        return False, [f"the file '{path}' does not exist"], []
    if path.is_dir():
        return False, [f"'{path}' is a folder, not a ROM file"], []

    data = path.read_bytes()
    size = len(data)

    if size < 0x150:
        errors.append(
            f"the file is {size} bytes; every Game Boy ROM is at least 336 "
            "(0x150). This usually means a corrupt file or the wrong one."
        )
        return False, errors, infos

    logo = data[0x104:0x114]
    if logo in SIGNATURES:
        infos.append(f"signature: {SIGNATURES[logo]}")
    else:
        errors.append(
            "the manufacturer signature (0x104-0x113) matches no Game Boy ROM. "
            "This file is probably not a ROM."
        )

    header_ok, declared, header_error = _header_ok(data)
    if header_ok:
        infos.append(f"size in header: {declared:,} bytes")
    else:
        errors.append(header_error)
    infos.append(f"actual size: {size:,} bytes")

    if size % 0x4000 != 0:
        errors.append(
            f"the size ({size} bytes) is not a multiple of 16 KiB, which is the "
            "size of one ROM bank. That usually means a corrupt file."
        )

    if data[0x148] > 0x08:
        errors.append(
            f"the ROM-size code in the header is {data[0x148]:#04x}, meaning 8 MiB "
            "or more. This project does not compile ROMs that large."
        )

    cgb = data[0x143]
    infos.append(f"title: {_title(data)}")
    infos.append("mode: " + {
        0x80: "Game Boy Color (CGB)",
        0xC0: "Game Boy Color + Game Boy Advance",
    }.get(cgb, "classic Game Boy (DMG)"))
    infos.append(f"cartridge: {MBC_NAMES.get(data[0x147], f'code {data[0x147]:#04x}')}")
    infos.append(f"RAM: {RAM_SIZES.get(data[0x149], f'code {data[0x149]}')}")

    return (not errors), errors, infos


def main() -> int:
    ap = argparse.ArgumentParser(description="Check whether a file looks like a Game Boy ROM.")
    ap.add_argument("rom", type=Path, help="path to the .gbc / .gb file")
    ap.add_argument("-q", "--quiet", action="store_true",
                    help="print nothing unless there is a problem (useful in scripts)")
    args = ap.parse_args()

    ok, errors, infos = validate(args.rom)

    if not args.quiet:
        for line in infos:
            print(f"    {line}")
    if errors:
        print("\n  This ROM did not pass validation:", file=sys.stderr)
        for e in errors:
            print(f"    - {e}", file=sys.stderr)
        print("", file=sys.stderr)
        return 1
    if not args.quiet:
        print("\n  Valid ROM. Ready to build.")
    return 0


if __name__ == "__main__":
    sys.exit(main())