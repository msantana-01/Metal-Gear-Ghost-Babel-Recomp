# Metal Gear Ghost Babel Recomp

Runs **Metal Gear Solid: Ghost Babel** (Game Boy Color) as a native program
on your computer — same inputs, same hardware emulation, no emulator sitting
in the hot path. No game data is downloaded or shipped here: you use your own
ROM.

```bash
git clone https://github.com/msantana-01/Metal-Gear-Ghost-Babel-Recomp.git
cd Metal-Gear-Ghost-Babel-Recomp
./bootstrap.sh
./build.sh ~/my-rom.gbc
./run.sh
```

Three commands. Or two, if you prefer a window: `./run.sh --gui`.

---

## What this is, and what it is not

The Game Boy ran a 4.19 MHz 8-bit processor with 8 KiB of RAM. Emulators
rebuild that processor and *interpret* the ROM. This does the opposite: the ROM
is translated **once** into native C, and the resulting program runs directly
on your machine with no interpreter in the loop.

| | a typical emulator | here |
|---|---|---|
| how it runs | interprets the ROM every frame | the ROM becomes C, the C runs natively |
| speed | however good the emulation is | the Game Boy's own 59.7 fps, using ~7% of each frame |
| needs the ROM at run time | yes | **no** — it becomes an executable |

**Not included, ever:** the ROM, the game's code, graphics, or audio. Those are
not ours to distribute. What is included is the *tooling* — the translator and
the runtime — which anyone can run against their own copy of the game.

The 59.7 fps figure is not a round number chosen to look good: the runtime
paces itself against the Game Boy's real frame length, so it cannot run faster
than the hardware did without changing how the game plays. Measured over 2500
frames with a window open: p50 16.743 ms, p95 16.945 ms, p99 17.059 ms, and 6.5%
of the frame budget spent producing a frame. `tools/frame_pacing.py` reproduces
it. With no window and no pacing at all the same build runs ~2700 fps.

> You need a legitimate copy of the game. This project does not download, sell
> or include any part of it — not the ROM, not the executable produced from
> the ROM. This is the same situation as any emulator.

---

## What you need first

Four things, each one a single install command:

| needs | usually already there? |
|---|---|
| `git` | yes |
| a C/C++ compiler | sometimes (`build-essential` on Debian, `base-devel` on Arch) |
| `make` | sometimes (it comes with the compiler) |
| SDL2 development headers | no — this is the one package to install |
| Python 3 | yes |

On Arch, Debian/Ubuntu, Fedora, Linux Mint or openSUSE, `./bootstrap.sh`
detects your system and **prints the exact command** for anything missing.
You do not have to look up package names.

The graphical window additionally needs the Tk library
(`python3-tk` on Debian/Ubuntu, `tk` on Arch, `python3-tkinter` on Fedora).
The command line does not need it.

---

## Step by step

### 1. Install what is missing (only if you see a warning)

`./bootstrap.sh` tells you. The commands are:

```bash
# Arch / Omarchy
sudo pacman -S --needed base-devel pkgconf sdl2

# Debian / Ubuntu / Mint
sudo apt-get update && sudo apt-get install -y build-essential pkg-config libsdl2-dev

# Fedora
sudo dnf install -y gcc-c++ make pkgconf-pkg-config SDL2-devel
```

### 2. `./bootstrap.sh`

Downloads the translator (pinned by commit hash plus a versioned patch you can
read line by line) and builds the tools. About 30 seconds the first time,
seconds after that.

### 3. `./build.sh path/to/rom.gbc`

- checks the file really is a Game Boy ROM;
- translates it into ~283 C files;
- compiles and links them;
- **runs the game for 120 frames** to prove it was born alive.

Three to four minutes. That is the price of turning 2 MB of ROM into native
code; it is not a hang.

### 4. `./run.sh`

The game opens.

Prefer a window to typing paths? `./run.sh --gui` opens the same flow
graphically, with the build progress on screen.

### 5. Free the disk, if you want to

Translating the ROM leaves a few hundred megabytes behind: the generated C and
the object files. The game never reads them - the executable links only
against system libraries - so they can go:

```bash
make clean       # deletes the generated code, keeps the game and your saves
make distclean   # deletes build/ entirely, game and saves included
```

After a build the command line asks whether to do it, and the window shows a
**Free build space** button. Deleting them costs nothing but a rebuild if you
ever want them back.

---

## How to play

The keyboard is the controller. There is no mouse, because the game had none.

| key | action |
|---|---|
| arrows or `WASD` | movement and aiming |
| `Z` or `J` | A button |
| `X` or `K` | B button |
| `Enter` | Start |
| `Backspace` or right `Shift` | Select |
| `Tab` | fast-forward (skips waiting) |
| `F1` / `F2` | diagnostics overlay / UI |
| `F5` / `F8` | save / load state |
| `F9` | next screen effect (scanlines, LCD grid, CRT) |
| `F4` | toggle LCD response (panel trail) |
| `M` | mute |
| `Esc` | configuration menu |

A gamepad works too.

### Screen effects

The settings menu has a **Screen Effect** selector — Off, Scanlines, LCD Grid or
CRT — plus a strength percentage. These draw over the finished image only; they
never touch the ROM or the emulated hardware, so frame-accurate comparisons are
unaffected.

**LCD Response** (`F4`) makes the screen lag the way a handheld panel does, with
a response time in milliseconds. It is off by default and remembered across
restarts.

The gap between simulated pixels is sized against the current window, so the
same setting holds from a small window up to 4K instead of aliasing at one size
and disappearing at another.

---

## If something goes wrong

Every message this project prints says **what to do**, not just what happened.
If you hit something the message does not solve, the full log is kept in the
project folder:

```bash
cat .build.log        # last build
cat .bootstrap.log    # last setup
```

If it is our bug, open an issue with that log attached. A log is worth far
more than a description.

---

## Honest answers

**Is it frame-accurate?**
The runtime passes 75 of 75 Game Boy hardware test ROMs (Blargg and Mooneye),
and its output matches a cycle-accurate reference emulator frame for frame on
the material we measured. Where a 3–4 pixel difference survives in specific
scenes, that is documented rather than hidden.

**Does it need my exact ROM?**
No. The tools work with any Game Boy ROM. Ghost Babel has an analysis profile
([`profiles/ghost-babel.json`](profiles/ghost-babel.json)) listing three entry
points the analyzer does not find on its own — addresses only, no game data.

**Will this run on my machine?**
Linux today. Tested on Arch/Omarchy and Ubuntu 24.04 from scratch, including
a machine with nothing installed. Windows and Android are planned; they are not
finished, and this README will not pretend otherwise.

**Can I share the executable it produces?**
No — and we do not try to help with that. The generated executable *is* the
game. This repository ships tools only.

---

## Keeping it up to date

Two different things can change, and they need two different actions.

**A new version of these tools.** Download the new release, drop it in place
and rebuild:

```bash
./build.sh ~/my-rom.gbc
```

That is the whole update. Your ROM is untouched, nothing about your own copy
of the game changes, and you reinstall nothing - the tools are a folder you
replace. Watch the Releases page on GitHub to know when there is a new one.

**A new ROM dump.** Point `build.sh` at the new file:

```bash
./build.sh ~/other-dump.gbc
```

Your saves live in `build/`, so keep that folder if you care about them.

**What never changes:** your ROM. Nothing here ever modifies it, and no
version of these tools will. If the same ROM ever produced different output,
that is a bug worth reporting.

Every version is pinned in [`pins.lock`](pins.lock) and checked by hash during
setup, so a tool version cannot quietly change under you: what you build with
today is what you keep, until you choose to update.

---

## How it works

```
your-rom.gbc
   │
   ├─ gbrecomp ............. ROM → 283 C files (~385 MB on disk)
   │
   ├─ make ................. compiles the generated C
   │
   └─ libgbrt.a ............ runtime: CPU, PPU, timers, APU, MBC5, SDL
                 │
                 ▼
          build/ghost-babel        ← native executable, ~75 MB
```

The translator is [`gb-recompiled`](https://github.com/arcanite24/gb-recompiled)
(MIT), plus one patch from this repository
([`patches/`](patches/gb-recompiled-ghost-babel.patch)) fixing CGB hardware
behaviour and post-boot DIV per boot-ROM revision. Every version is pinned in
[`pins.lock`](pins.lock) and checked by hash, so the engine cannot change
underneath you.

Build output is reproducible: the same ROM and the same tool version produce
the same binary, whatever path you took to build it.

---

## Translations

The interface is English only. For the commands and package names:

<details>
<summary><b>Português do Brasil</b></summary>

### O que você precisa

`git`, um compilador C/C++, `make`, os headers de desenvolvimento do SDL2 e
Python 3. Um comando instala tudo:

```bash
# Arch / Omarchy
sudo pacman -S --needed base-devel pkgconf sdl2
# Debian / Ubuntu / Mint
sudo apt-get update && sudo apt-get install -y build-essential pkg-config libsdl2-dev
# Fedora
sudo dnf install -y gcc-c++ make pkgconf-pkg-config SDL2-devel
```

A interface gráfica também precisa do Tk: `python3-tk` (Debian/Ubuntu),
`tk` (Arch) ou `python3-tkinter` (Fedora). A linha de comando não precisa.

### Os três comandos

```bash
git clone https://github.com/msantana-01/Metal-Gear-Ghost-Babel-Recomp.git
cd Metal-Gear-Ghost-Babel-Recomp
./bootstrap.sh                  # prepara tudo (~30s)
./build.sh /caminho/da/rom.gbc  # compila (~3 a 4 minutos)
./run.sh                        # joga
```

Ou, preferindo janela: `./run.sh --gui`.

### Se der problema

`./bootstrap.sh` e `build.sh` dizem o que fazer a cada falha. O log completo
fica em `.build.log` e `.bootstrap.log`, na pasta do projeto — vale anexá-lo
a um issue.

</details>

<details>
<summary><b>Deutsch</b></summary>

Benötigt werden `git`, ein C/C++-Compiler, `make`, die SDL2-Entwicklerheader
und Python 3. Alles installiert ein einziger Befehl:

```bash
# Debian / Ubuntu
sudo apt-get update && sudo apt-get install -y build-essential pkg-config libsdl2-dev
# Arch
sudo pacman -S --needed base-devel pkgconf sdl2
# Fedora
sudo dnf install -y gcc-c++ make pkgconf-pkg-config SDL2-devel
```

Dann `./bootstrap.sh` (Vorbereitung), `./build.sh rom.gbc` (übersetzen und
bauen) und `./run.sh` (spielen). Die grafische Oberfläche braucht zusätzlich Tk
(`python3-tk` bzw. `tk`); die Kommandozeile kommt ohne aus.

</details>

## License

The tools in this repository are MIT — see [LICENSE](LICENSE).

Metal Gear Solid: Ghost Babel is a work of Konami / Nintendo. This project
does not include, distribute or obtain the ROM, the game's source, its
graphics, its music, or the executable generated from the ROM. Building that
executable from your own ROM is a derivative work and your responsibility on
your own computer.
