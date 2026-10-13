#!/usr/bin/env python3
"""Escolhe o perfil de análise adequado para uma ROM.

Um "perfil" é só um conjunto de opções extras passadas ao gbrecomp. Hoje
existe um perfil, para o cartucho Ghost Babel: três endereços de entrada que
o analisador sozinho não descobre.

Os perfis contêm apenas fatos sobre a ROM (endereço de código, título) —
nada de código, dados ou artefato do jogo. É a mesma natureza de um arquivo
de patch: endereços, não conteúdo.

Uso (imprime as opções, ou nada se a ROM não tiver perfil):
    python3 tools/pick_profile.py /caminho/rom.gbc
"""

from __future__ import annotations

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
            print(f"aviso: perfil {f.name} ilegível: {exc}", file=sys.stderr)
    return out


def rom_title(path: Path) -> str:
    with path.open("rb") as fh:
        fh.seek(0x134)
        raw = fh.read(16).split(b"\x00", 1)[0]
    return "".join(chr(b) for b in raw if 32 <= b < 127).strip()


def match(rom: Path, profiles: list[dict]) -> dict | None:
    title = rom_title(rom)
    size = rom.stat().st_size
    for prof in profiles:
        want_title = prof.get("title")
        want_size = prof.get("size_bytes")
        if want_title is not None and want_title != title:
            continue
        if want_size is not None and want_size != size:
            continue
        if want_title is None and want_size is None:
            continue
        return prof
    return None


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    rom = Path(sys.argv[1])
    if not rom.is_file():
        return 1
    prof = match(rom, load_profiles())
    if not prof:
        return 0
    print(" ".join(shlex.quote(a) for a in prof.get("gbrecomp_args", [])))
    return 0


if __name__ == "__main__":
    sys.exit(main())