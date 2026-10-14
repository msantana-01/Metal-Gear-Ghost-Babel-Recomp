#!/usr/bin/env python3
"""Generates a minimal test ROM, with nothing from any real game.

It exists to prove that the whole path (validate -> generate -> compile ->
run) works on a clean machine without depending on anyone's ROM. It is a
32 KiB "game" that paints a grey gradient and loops forever.

Usage:  python3 tests/make_test_rom.py destination.gbc
"""

from __future__ import annotations

import sys
from pathlib import Path

TITLE = b"GBRECOMP TEST"
SIZE = 32 * 1024  # two banks: the smallest cartridge there is


def build_rom() -> bytes:
    rom = bytearray(b"\x00" * SIZE)

    # --- reset vector at 0x0000
    rom[0x000:0x004] = bytes([0x00, 0xC3, 0x50, 0x01])  # nop; jp $0150

    # --- code at 0x150
    #
    # The wait is done by *polling* LCDY, not by HALT: HALT depends on
    # interrupt semantics, and a smoke test should not depend on that.
    #
    #   0150: di
    #   0151: ld sp,$DFFF
    #   0154: ld a,$91        ; LCD on, BG on
    #   0156: ldh ($40),a
    #   0158: loop: ld a,$E4
    #   015A:         ldh ($47),a   ; BGP: gradient
    #   015C:         ldh a,($44)   ; LCDY
    #   015E:         cp $90        ; reached line 144 (VBlank)?
    #   0160:         jr c,loop
    #   0162:         jr loop
    code = bytes([
        0xF3,                    # di
        0x31, 0xFF, 0xDF,        # ld sp,$DFFF
        0x3E, 0x91,              # ld a,$91
        0xE0, 0x40,              # ldh ($40),a
        0x3E, 0xE4,              # ld a,$E4
        0xE0, 0x47,              # ldh ($47),a
        0xF0, 0x44,              # ldh a,($44)
        0xFE, 0x90,              # cp $90
        0x38, 0xF6,              # jr c,-10   -> 0x0158
        0x18, 0xF4,              # jr    -12  -> 0x0158
    ])
    rom[0x150:0x150 + len(code)] = code

    # --- cartridge header
    rom[0x104:0x114] = b"\xff" * 16          # no logo (synthetic ROM)
    rom[0x134:0x134 + len(TITLE)] = TITLE    # title
    rom[0x143] = 0x80                        # CGB
    rom[0x144:0x146] = b"\x00\x00"           # not for sale
    rom[0x147] = 0x00                        # ROM only
    rom[0x148] = 0x00                        # 32 KiB
    rom[0x149] = 0x00                        # no RAM
    rom[0x14A] = 0x01                        # MBC1
    rom[0x14B] = 0x00
    rom[0x14C] = 0x00
    checksum = 0
    for b in rom[0x134:0x14D]:
        checksum = (checksum - b - 1) & 0xFF
    rom[0x14D] = checksum
    rom[0x14E] = 0x00                        # version
    rom[0x14F] = 0x00                        # more versioning
    rom[0x14F - 0] = 0x00
    return bytes(rom)


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    out = Path(sys.argv[1])
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(build_rom())
    print(f"test ROM written to {out} ({out.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())