#!/usr/bin/env python3
"""Valida uma ROM de Game Boy sem precisar saber nada sobre ela.

Regras deliberadamente conservadoras: a ferramenta confere a *estrutura* do
cartucho (assinatura do fabricante, tamanho, coerência do cabeçalho) e nunca
o *conteúdo*. Não publicamos nem calculamos hash da ROM de ninguém.

Exit code: 0 = parece uma ROM válida, 1 = não parece, 2 = erro de uso.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

#-assinatura do fabricante que toda ROM de Game Boy precisa ter
LOGOS = {
    bytes([0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73,
           0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D]): "Nintendo",
    bytes([0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
           0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF]): "sem assinatura (cópia/homebrew)",
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

# title area: bytes 0x134-0x143, most games end it with 0x00
def _title(raw: bytes) -> str:
    chunk = raw[0x134:0x144].split(b"\x00", 1)[0]
    # bytes de controle e lixo não são título: fica só o que é imprimível
    text = "".join(chr(b) for b in chunk if 32 <= b < 127)
    return " ".join(text.split()) if text else "(sem título)"


def _ram_size(code: int) -> str:
    sizes = {0: "nenhuma", 1: "2 KiB", 2: "8 KiB", 3: "32 KiB", 4: "128 KiB", 5: "64 KiB"}
    return sizes.get(code, f"código {code}")


def validate(path: Path) -> tuple[bool, list[str], list[str]]:
    """Devolve (ok, erros, informações legíveis)."""
    errors: list[str] = []
    infos: list[str] = []

    if not path.exists():
        return False, [f"o arquivo '{path}' não existe"], []
    if path.is_dir():
        return False, [f"'{path}' é uma pasta, não um arquivo de ROM"], []

    data = path.read_bytes()
    size = len(data)

    if size < 0x150:
        errors.append(
            f"o arquivo tem {size} bytes — toda ROM de Game Boy tem pelo menos 336 (0x150). "
            "Isso costuma ser um arquivo corrompido ou o arquivo errado."
        )
        return False, errors, infos

    logo = data[0x104:0x114]
    if logo in LOGOS:
        infos.append(f"assinatura: {LOGOS[logo]}")
    else:
        errors.append(
            "a assinatura do fabricante (0x104-0x113) não bate com nenhuma ROM de Game Boy. "
            "O arquivo provavelmente não é uma ROM."
        )

    header_ok, declared, header_size = _header_ok(data)
    if not header_ok:
        errors.append(header_size)
    else:
        infos.append(f"tamanho no cabeçalho: {declared:,} bytes".replace(",", "."))
    infos.append(f"tamanho real: {size:,} bytes".replace(",", "."))

    if size % 0x4000 != 0:
        errors.append(
            f"o tamanho ({size} bytes) não é múltiplo de 16 KiB, que é o tamanho de "
            "cada banco de ROM. Isso é característico de arquivo corrompido."
        )

    rom_size = data[0x148]
    if rom_size > 0x08:
        errors.append(
            f"código de tamanho de ROM no cabeçalho é {rom_size:#04x}, que indica "
            "8 MiB ou mais. Este projeto não compila ROMs desse tamanho."
        )

    title = _title(data)
    cgb = data[0x143]
    mode = {0x80: "Game Boy Color (CGB)", 0xC0: "Game Boy Color + Game Boy Advance"}.get(
        cgb, "Game Boy clássico (DMG)"
    )
    infos.append(f"título: {title}")
    infos.append(f"modo: {mode}")
    infos.append(f"cartucho: {MBC_NAMES.get(data[0x147], f'código {data[0x147]:#04x}')}")
    infos.append(f"RAM: {_ram_size(data[0x149])}")

    return (not errors), errors, infos


def _header_ok(data: bytes) -> tuple[bool, int, str]:
    code = data[0x148]
    if code > 0x08:
        return False, 0, ""
    declared_kib = 32 << code  # 0x00 = 32 KiB ... 0x08 = 8 MiB
    declared = declared_kib * 1024
    if len(data) == declared:
        return True, declared, ""
    return False, declared, (
        f"o cabeçalho declara {declared:,} bytes (código {code:#04x}) mas o arquivo tem "
        f"{len(data):,} bytes. Arquivo truncado ou de outro jogo."
        .replace(",", ".")
    )


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Confere se um arquivo parece uma ROM de Game Boy válida.",
    )
    ap.add_argument("rom", type=Path, help="caminho do arquivo .gbc / .gb")
    ap.add_argument("-q", "--quiet", action="store_true",
                    help="só imprime em caso de erro (útil em scripts)")
    args = ap.parse_args()

    ok, errors, infos = validate(args.rom)

    if not args.quiet:
        for line in infos:
            print(f"    {line}")
    if errors:
        print("\n  Esta ROM não passou na validação:", file=sys.stderr)
        for e in errors:
            print(f"    - {e}", file=sys.stderr)
        print("", file=sys.stderr)
        return 1
    if not args.quiet:
        print("\n  ROM válida. Pode compilar.")
    return 0


if __name__ == "__main__":
    sys.exit(main())