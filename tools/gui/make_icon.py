#!/usr/bin/env python3
"""Generates the GUI icon: a Game Boy cartridge, in pixel art.

This artwork is OURS. Nothing here comes from the ROM, from the game, or from
any third-party image - these are geometric shapes (a cartridge seen from the
front) drawn by code. That is why it can live in the public repository.

Tkinter only reads PNG and GIF, never JPEG, so the icon is generated as PNG
already. No external dependency (no Pillow): the PNG is written by hand with
zlib.

Usage:  python3 tools/gui/make_icon.py [output.png]
"""

from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path

GRID = 32       # desenho em 32x32, como pixel art de verdade
SCALE = 4       # nearest-neighbor upscale
OUT = 128       # GRID * SCALE

TRANSPARENT = (0, 0, 0, 0)
INK = (26, 28, 38, 255)        # contorno escuro
SHELL = (74, 80, 96, 255)      # cartridge plastic
SHELL_HI = (108, 116, 136, 255)  # highlight on the plastic
LABEL = (238, 240, 246, 255)   # etiqueta
ACCENT = (59, 111, 212, 255)   # faixa azul
WARM = (224, 160, 32, 255)     # detalhe âmbar


class Canvas:
    def __init__(self, size: int) -> None:
        self.size = size
        self.px = [[TRANSPARENT] * size for _ in range(size)]

    def set(self, x: int, y: int, color) -> None:
        if 0 <= x < self.size and 0 <= y < self.size:
            self.px[y][x] = color

    def rect(self, x0: int, y0: int, x1: int, y1: int, color) -> None:
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                self.set(x, y, color)

    def frame(self, x0: int, y0: int, x1: int, y1: int, color) -> None:
        for x in range(x0, x1 + 1):
            self.set(x, y0, color)
            self.set(x, y1, color)
        for y in range(y0, y1 + 1):
            self.set(x0, y, color)
            self.set(x1, y, color)

    def rounded(self, x0: int, y0: int, x1: int, y1: int, r: int, color) -> None:
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                dx = min(x - x0, x1 - x)
                dy = min(y - y0, y1 - y)
                if dx < r and dy < r and (r - dx) ** 2 + (r - dy) ** 2 > r * r:
                    continue
                self.set(x, y, color)


def draw_cartridge() -> Canvas:
    c = Canvas(GRID)

    # --- cartridge body
    c.rounded(3, 3, 28, 30, 2, SHELL)
    c.frame(3, 3, 28, 30, INK)

    # --- light on the top-left corner: gives the plastic some volume
    for i in range(4):
        c.rect(5 + i, 5, 5 + i, 7, SHELL_HI)

    # --- metal connector at the top, with its two notches
    c.rect(8, 1, 23, 4, SHELL_HI)
    c.frame(8, 1, 23, 4, INK)
    for x in (12, 17):                      # entalhes do conector
        c.rect(x, 1, x + 2, 4, TRANSPARENT)

    # --- pins inside the connector
    for x in range(9, 12):
        c.rect(x, 2, x, 3, INK)
    for x in range(14, 17):
        c.rect(x, 2, x, 3, INK)
    for x in range(20, 23):
        c.rect(x, 2, x, 3, INK)

    # --- label
    c.rect(7, 9, 24, 24, LABEL)
    c.frame(7, 9, 24, 24, INK)
    c.rect(8, 10, 23, 12, ACCENT)           # faixa colorida
    c.frame(8, 10, 23, 12, INK)

    # --- "text lines" on the label: blocks, not letters (readable at 32
    #     pixels, and it does not imitate the game's lettering)
    c.rect(10, 15, 21, 16, SHELL)
    c.rect(10, 18, 18, 19, SHELL)
    c.rect(10, 21, 20, 22, WARM)

    # --- grip notch at the bottom
    c.rect(12, 26, 19, 28, TRANSPARENT)
    c.frame(12, 26, 19, 28, INK)
    c.rect(13, 27, 18, 27, SHELL)

    return c


def scale(c: Canvas, factor: int) -> Canvas:
    out = Canvas(c.size * factor)
    for y in range(c.size):
        for x in range(c.size):
            color = c.px[y][x]
            for dy in range(factor):
                for dx in range(factor):
                    out.px[y * factor + dy][x * factor + dx] = color
    return out


def write_png(path: Path, c: Canvas) -> None:
    raw = bytearray()
    for row in c.px:
        raw.append(0)  # filtro "none" por linha
        for px in row:
            raw.extend(bytes(px))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", c.size, c.size, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    path.write_bytes(png)


def main() -> int:
    dest = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).with_name("icon.png")
    art = draw_cartridge()
    write_png(dest, scale(art, SCALE))
    print(f"icon: {dest} ({OUT}x{OUT}, original artwork drawn at {GRID}x{GRID} px)")
    return 0


if __name__ == "__main__":
    sys.exit(main())