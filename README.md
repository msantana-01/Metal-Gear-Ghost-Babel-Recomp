# Metal Gear Ghost Babel Recomp

Runs **Metal Gear Solid: Ghost Babel** (Game Boy Color) as a native program
on **Linux** and **Windows** — same inputs, same hardware emulation, no emulator
sitting in the hot path. No game data is downloaded or shipped here: you provide
your own ROM.

For Ghost Babel, `./build.sh` (or `GhostBabelRecomp.bat` on Windows) builds two
executables sharing the same core and save files (`build/baserom.sav`):

- **`ghost-babel-remaster`** *(default)* — 256 px widescreen view assembled from
  the cartridge room layers with a per-room VRAM tileset & palette cache, margin
  room/cutscene actors, world props & shadows, sliding 136 px camera framing,
  centred dialogue/item-pickup overlays, **Modern Stealth** gamepad controls
  (`B` crouch, `Start` Codec, `RB`/`LT`/`LB` quick weapon & item swap) with
  dynamic Xbox/PlayStation/Nintendo button prompts, an **animated Codec pause menu**
  (CRT unfold, scanlines, `140.85` signal meter, procedural UI SFX, 2×5 visual
  savestate gallery, true pause), shader-independent **Color Profiles** and
  **Tactical Edge FX** (Alert / low-HP border glow), `F12` debug snapshots, GPU
  OpenGL GLSL CRT-Lottes & GBC LCD shaders, linear-light LCD response, V-Sync +
  audio Dynamic Rate Control, controller rumble, and cycle-integrated APU audio
  mastering with headphone crossfeed.
- **`ghost-babel-authentic`** — original 160×144 hardware frame, unchanged.

```bash
# Linux
git clone https://github.com/msantana-01/Metal-Gear-Ghost-Babel-Recomp.git
cd Metal-Gear-Ghost-Babel-Recomp
./bootstrap.sh
./build.sh ~/my-rom.gbc
./run.sh            # Widescreen Remaster (use --authentic for 160px, --gui for window)
```

On **Windows**: download `ghost-babel-tools-windows-x86_64-0.8.0.zip` from
[Releases](https://github.com/msantana-01/Metal-Gear-Ghost-Babel-Recomp/releases),
extract anywhere, and double-click **`GhostBabelRecomp.bat`** (or drag & drop
your `.gbc` ROM onto **`build.bat`**). No Python, MSYS2, WSL, or administrator
rights required — the first build automatically downloads and SHA-256 verifies
the portable `w64devkit` compiler, and `SDL2.dll` is already included.

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
not ours to distribute. What is included is the *tooling* — the translator, the
remaster presenter, and the runtime — which anyone can run against their own
copy of the game.

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

## Windows (x86-64, Windows 10 / 11)

1. Download **`ghost-babel-tools-windows-x86_64-0.8.0.zip`** from
   [Releases](https://github.com/msantana-01/Metal-Gear-Ghost-Babel-Recomp/releases)
   and extract it to a folder of your choice.
2. Double-click **`GhostBabelRecomp.bat`** (graphical window) or drag & drop
   your `.gbc` ROM onto **`build.bat`**.
3. On the first run, the tool automatically downloads the pinned portable
   C/C++ toolchain (`w64devkit` 2.11.0, ~65 MB, verified by SHA-256) into
   `.tools/`. `SDL2.dll` is already bundled inside the release zip.
4. Launch the game anytime via **`run-remaster.bat`** (widescreen),
   **`run-authentic.bat`** (160 px), or the buttons in `GhostBabelRecomp.bat`.

*(If you cloned directly from Git on Windows instead of downloading the release
zip, `GhostBabelRecomp.bat` / `windows/bootstrap.ps1` downloads the prebuilt
`gbrecomp.exe`, runtime, and SDL2 package automatically from Releases — or you
can pass `-FromSource` with `git` + `cmake` installed.)*

---

## Linux (x86-64)

### 1. System packages (only if `./bootstrap.sh` asks)

`./bootstrap.sh` detects your distro and prints the exact command if anything is
missing:

```bash
# Arch / Omarchy
sudo pacman -S --needed base-devel pkgconf sdl2

# Debian / Ubuntu / Mint
sudo apt-get update && sudo apt-get install -y build-essential pkg-config libsdl2-dev

# Fedora
sudo dnf install -y gcc-c++ make pkgconf-pkg-config SDL2-devel
```

The graphical window (`./run.sh --gui`) additionally needs Tk (`python3-tk` on
Debian/Ubuntu, `tk` on Arch, `python3-tkinter` on Fedora). The command line does
not need it.

### 2. `./bootstrap.sh`

Verifies dependencies and prepares `.tools/gbrecomp` and `.tools/libgbrt.a`
(from the release tarball when downloaded from Releases, or fetched/compiled
according to [`pins.lock`](pins.lock)).

### 3. `./build.sh path/to/rom.gbc`

- checks the file really looks like a Game Boy ROM (no ROM hash published);
- translates it into C files using `gbrecomp --output-prefix baserom`;
- compiles and links `build/ghost-babel-remaster` (widescreen) and
  `build/ghost-babel-authentic` (160 px);
- **runs both executables for 120 frames** headless to prove they work.

### 4. `./run.sh`

- `./run.sh` — opens **Widescreen Remaster** (256 px)
- `./run.sh --authentic` — opens **Authentic** (160 px)
- `./run.sh --gui` — opens the graphical builder & launcher

### 5. Free the disk, if you want to

```bash
make clean       # deletes intermediate C/object files, keeps executables and saves
make distclean   # deletes build/ entirely, executables and saves included
```

---

## How to play

The keyboard is the controller. A gamepad works too.

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
| `F9` | next screen effect (Off, Scanlines, LCD Grid, CRT, CRT Trinitron (Flat), CRT Arcade (Curved), GBC LCD (Shader)) |
| `F4` | toggle LCD response (linear-light panel decay) |
| `F12` | save isolated debug snapshot bundle (`debug/snap_...`) |
| `M` | mute |
| `Esc` | Codec tactical pause menu |

### Modern Stealth Gamepad Controls (`ghost-babel-remaster` default)

`ghost-babel-remaster` defaults to **Modern Stealth** (switchable to **Classic GBC** in `Esc` / `L3` → **Controls**; `ghost-babel-authentic` keeps **Classic GBC**):

| Xbox / PS button | action |
|---|---|
| D-Pad / Left Stick | movement, aiming, and menu navigation |
| `A` / Cross | punch / interact / confirm |
| `B` / Circle | crouch / prone (`GB Start`) |
| `X` / Square or `RT` / R2 | fire / use weapon (`GB B`) |
| `Y` / Triangle or `Back` / Share | open equipment menu (`GB Select`) |
| `Start` / Options | open **Codec** in gameplay (works as normal Start on title screen) |
| `RB` / R1 | quick next weapon macro |
| `LT` / L2 | quick previous weapon macro |
| `LB` / L1 | quick next item macro |
| `L3` / `R3` | Codec tactical pause menu (`L3`) / native panel (`R3`) |

Inside the **Codec Tactical Pause Menu** (`Esc` or `L3`), emulation and game audio pause cleanly while animated CRT scanlines, phosphor sweep, `140.85` signal bars, and procedural Codec UI SFX accompany D-Pad / Stick navigation (`A` / Cross select, `B` / Circle resume, `LB`/`RB` or `L1`/`R1` switch tabs across `MISSION` 2×5 save gallery, `DISPLAY`, `AUDIO`, `CONTROLS`, `SYSTEM`). Prompts adapt automatically to **Xbox**, **PlayStation**, or **Nintendo** controllers.

### Screen effects & remaster features

- **Color Profile**: `Raw (Original)`, **`Modern Balanced`** *(remaster default)*, and `GBC Authentic`, shader-independent and persisted separately per artifact (`authentic` vs `remaster`).
- **Tactical Edge FX**: subtle screen-edge crimson vignette during `ALERT` (`$C4E3`) and critical HP (`<= 6/24`), toggleable in **Display** *(on by default in `ghost-babel-remaster`)*.
- **Screen Effect (`F9`)**: `Off`, `Scanlines`, `LCD Grid`, `CRT`, plus three real GPU **OpenGL GLSL** presets with automatic `SDL_Renderer` 2D fallback and live **Effect Strength %** (`10%..100%`, persisted automatically):
  - **`CRT Trinitron (Flat)`** *(remaster default)* — Public Domain **CRT-Lottes** with linear-light Gaussian scanlines, phosphor bloom, and Aperture Grille mask (no barrel warp so HUD/text stay straight).
  - **`CRT Arcade (Curved)`** — **CRT-Lottes** with subtle tube curvature, rounded bezel corners, gentle vignette, subtle RGB convergence offset, and staggered Slot Mask.
  - **`GBC LCD (Shader)`** — linear-light GBC TFT color-matrix correction with scale-aware pixel grid and RGB subpixels.
- **LCD Response (`F4`)**: physical linear-light asymmetric panel decay (`16 ms` CGB TFT default, adjustable `1..200 ms`), off by default, persisted separately per artifact (`authentic` vs `remaster`).
- **Audio mastering**: DC blocker + stereo-linked compressor + soft clipper enabled by default in `ghost-babel-remaster` (toggleable in the `Esc` menu).
- **Widescreen Remaster (256 px)**: reconstructs continuous room flanks from the cartridge layers (`RoomMap`), decodes culled margin room actors (`$C700..$CB3F`), Bank 21 world props/crates (`$CB40..$CBBF`), WRAM Bank 4 cutscene actors (`$DCED..$DD8C`), and oval shadows without mutating guest state (`guest_writeback_allowed = false`), keeps black margins on dark menus/splashes and contiguous status bars (`LIFE` + `O₂`), slides the active 136 px frame at room boundaries, and centres in-scene codec/item-pickup overlays.

---

## If something goes wrong

Full logs are written in the project folder:

```text
.build.log       # last build (Linux & Windows)
.bootstrap.log   # last setup (Linux & Windows)
```

If it is our bug, open an issue with that log attached.

---

## Honest answers

**Is it frame-accurate?**
The core runtime passes 75 of 75 Game Boy hardware test ROMs (Blargg and
Mooneye) and matches cycle-accurate reference emulators on `ghost-babel-authentic`.
The remaster is read-only over the same emulated machine and never alters guest
execution or saves.

**Does it need my exact ROM?**
The toolchain works with any Game Boy ROM (`build/ghost-babel` for generic ROMs).
Ghost Babel has an analysis profile ([`profiles/ghost-babel.json`](profiles/ghost-babel.json))
listing three jump-table entry points so both `ghost-babel-remaster` and
`ghost-babel-authentic` build automatically.

**Will this run on my machine?**
**Linux x86-64** and **Windows x86-64** (Windows 10/11). Both are built and
smoke-tested automatically in CI on every release.

**Can I share the `.exe` or executable it produces?**
No — and we do not try to help with that. The generated executable *is* the
game. This repository ships tools only.

---

## Keeping it up to date

Two different things can change, and they need two different actions.

**A new version of these tools.** Download the new release, drop it in place (or
extract to a new folder) and rebuild:

```bash
./build.sh ~/my-rom.gbc   # Linux (or GhostBabelRecomp.bat / build.bat on Windows)
```

That is the whole update. Your ROM is untouched, your saves in `build/` stay
intact (`build/baserom.sav`), and `.tools/` refreshes automatically against
[`pins.lock`](pins.lock).

**A new ROM dump.** Point `./build.sh` (or `build.bat`) at the new file.

---

## What's next

As of **v0.8.0**, both **`ghost-babel-remaster`** (256 px widescreen, **Modern Stealth** gamepad controls with `Start → Codec` and quick weapon/item swap, **animated Codec pause menu** with CRT scanlines, `140.85` signal bars, procedural UI SFX, 2×5 visual save gallery, and dynamic Xbox/PS/Nintendo prompts, shader-independent **Color Profiles**, **Tactical Edge FX**, `F12` debug snapshots, cartridge `RoomMap` margins with per-room VRAM cache, room/cutscene actors, world props & shadows, centred overlays, GPU GLSL CRT-Lottes & GBC LCD shaders, linear-light LCD response, V-Sync + audio DRC, controller rumble, and cycle-integrated APU mastering + crossfeed) and **`ghost-babel-authentic`** (160 px hardware-exact) are part of the public Linux and Windows x86-64 releases.

Ongoing work covers margin polish across later areas and future Android build
support.

---

## Translations

The interface is English only. For the commands and package names:

<details>
<summary><b>Português do Brasil</b></summary>

### Windows
Baixe `ghost-babel-tools-windows-x86_64-0.8.0.zip` em **Releases**, extraia em
uma pasta e dê duplo-clique em **`GhostBabelRecomp.bat`** (ou arraste sua ROM
`.gbc` sobre **`build.bat`**). Não precisa instalar Python, MSYS2 nem WSL: na
primeira execução o script baixa automaticamente o compilador portátil
`w64devkit` (verificado por SHA-256), e a `SDL2.dll` já vai inclusa. Use
**`run-remaster.bat`** (widescreen 256 px) ou **`run-authentic.bat`** (160 px).

### Linux
```bash
# Arch / Omarchy
sudo pacman -S --needed base-devel pkgconf sdl2
# Debian / Ubuntu / Mint
sudo apt-get update && sudo apt-get install -y build-essential pkg-config libsdl2-dev
# Fedora
sudo dnf install -y gcc-c++ make pkgconf-pkg-config SDL2-devel
```
Depois:
```bash
./bootstrap.sh
./build.sh /caminho/da/sua-rom.gbc
./run.sh               # Remaster widescreen (--authentic para 160px, --gui para janela)
```

</details>

<details>
<summary><b>Deutsch</b></summary>

### Windows
Lade `ghost-babel-tools-windows-x86_64-0.8.0.zip` von **Releases** herunter,
entpacke es und starte **`GhostBabelRecomp.bat`** per Doppelklick (oder ziehe
deine `.gbc`-ROM auf **`build.bat`**). Es werden weder Python noch MSYS2
benötigt — der portable Compiler (`w64devkit`) wird beim ersten Start
automatisch mit SHA-256-Prüfung eingerichtet, `SDL2.dll` liegt bei.

### Linux
```bash
# Debian / Ubuntu
sudo apt-get update && sudo apt-get install -y build-essential pkg-config libsdl2-dev
# Arch
sudo pacman -S --needed base-devel pkgconf sdl2
# Fedora
sudo dnf install -y gcc-c++ make pkgconf-pkg-config SDL2-devel
```
Danach `./bootstrap.sh`, `./build.sh rom.gbc` und `./run.sh` (oder `./run.sh --authentic` / `./run.sh --gui`).

</details>

## License

The tools in this repository are MIT — see [LICENSE](LICENSE).

Metal Gear Solid: Ghost Babel is a work of Konami / Nintendo. This project
does not include, distribute or obtain the ROM, the game's source, its
graphics, its music, or the executable generated from the ROM. Building that
executable from your own ROM is a derivative work and your responsibility on
your own computer.
