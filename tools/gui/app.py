#!/usr/bin/env python3
"""Graphical front end: pick a ROM, build it, play.

It does not try to be pretty. It tries to be impossible to get wrong: one
step at a time, progress you can actually read, and no message that does not
say what to do next.

    python3 tools/gui/app.py
"""

from __future__ import annotations

import queue
import shutil
import subprocess
import sys
import threading
from pathlib import Path

try:
    import tkinter as tk
    from tkinter import filedialog, ttk
except ImportError:  # pragma: no cover
    sys.stderr.write(
        "This window needs the Tk library, which is not installed.\n\n"
        "    sudo apt-get install -y python3-tk      # Debian/Ubuntu\n"
        "    sudo pacman -S --needed tk               # Arch/Omarchy\n"
        "    sudo dnf install -y python3-tkinter      # Fedora\n\n"
        "The command line does the same work without it:\n"
        "    ./build.sh /path/to/rom.gbc\n"
    )
    raise SystemExit(1)

REPO = Path(__file__).resolve().parent.parent.parent
GBRECOMP = REPO / ".tools" / "gbrecomp"
BUILD_SH = REPO / "build.sh"
RUN_SH = REPO / "run.sh"
GAME = REPO / "build" / "ghost-babel"
VALIDATE = REPO / "tools" / "validate_rom.py"
ICON = Path(__file__).with_name("icon.png")

ROM_TYPES = [
    ("Game Boy ROMs", ("*.gbc", "*.GBC", "*.gb", "*.GB")),
    ("All files", ("*",)),
]

TITLE = "Metal Gear: Ghost Babel - Recomp"
SUBTITLE = "Build your own copy of the ROM and play it natively on this computer"


class App(tk.Tk):
    # A window with no named background can come out translucent on
    # compositors that treat XWayland clients that way, and text over a
    # translucent window is hard to read. Naming the colour keeps the window
    # opaque on every desktop.
    BG = "#f4f4f5"

    # Sized for a 1080p screen, which is still what most people have: about
    # three quarters of the height, leaving room for a taskbar and a terminal
    # without covering the desktop. Taller screens are fine with this, and the
    # window is resizable for anyone who wants more.
    WIDTH, HEIGHT = 680, 800

    def __init__(self) -> None:
        super().__init__()
        self.title(TITLE)
        self.configure(padx=20, pady=16, background=self.BG)
        self.resizable(True, True)
        self._fit_to_screen()

        self._q: queue.Queue[tuple[str, object]] = queue.Queue()
        self._proc: subprocess.Popen | None = None
        self._icon_img: tk.PhotoImage | None = None

        self._build_widgets()
        self.after(100, self._drain)
        self._check_prereqs()

    def _fit_to_screen(self) -> None:
        """Ask for a comfortable size, only smaller if the screen is small.

        No arithmetic on the screen beyond that, and no coordinates: window
        managers on tiled desktops (Hyprland with XWayland, for one) decide
        where a window goes and how big it ends up, whatever the client asked
        for. A sensible default the user can drag beats trying to outsmart
        the compositor - which is what the previous three attempts did.
        """
        sw, sh = self.winfo_screenwidth(), self.winfo_screenheight()
        width = min(self.WIDTH, max(480, sw - 80))
        height = min(self.HEIGHT, max(420, sh - 120))
        self.geometry(f"{width}x{height}")

    # ------------------------------------------------------------------ ui
    def _build_widgets(self) -> None:
        pad = {"padx": 6, "pady": 4}

        # --- header: icon + name
        head = tk.Frame(self, background=self.BG)
        head.grid(row=0, column=0, columnspan=3, sticky="w", pady=(0, 14))
        if ICON.exists():
            try:
                self._icon_img = tk.PhotoImage(file=str(ICON))
                # PhotoImage has no scaling method; Tk 8.6 can zoom by
                # repeating, which keeps the pixel-art crisp.
                if self._icon_img.width() > 64:
                    factor = max(1, self._icon_img.width() // 64)
                    self._icon_img = self._icon_img.subsample(factor, factor)
                tk.Label(head, image=self._icon_img, borderwidth=0).pack(side="left", padx=(0, 12))
            except tk.TclError:
                self._icon_img = None  # a missing icon is never worth failing over

        titles = tk.Frame(head, background=self.BG)
        titles.pack(side="left")
        tk.Label(titles, text=TITLE, font=("", 13, "bold"), anchor="w").pack(anchor="w")
        tk.Label(titles, text=SUBTITLE, foreground="#555555", anchor="w",
                 wraplength=420, justify="left").pack(anchor="w")

        # --- step 1
        tk.Label(self, text="1.  Choose your ROM", font=("", 10, "bold"),
                 anchor="w").grid(row=1, column=0, columnspan=3, sticky="w")
        self.rom_var = tk.StringVar()
        self.rom_entry = ttk.Entry(self, textvariable=self.rom_var)
        self.rom_entry.grid(row=2, column=0, sticky="ew", **pad)
        self.rom_entry.bind("<Return>", lambda _e: self._validate())

        ttk.Button(self, text="Browse...", command=self._browse).grid(
            row=2, column=1, sticky="w", **pad)
        ttk.Button(self, text="Check", command=self._validate).grid(
            row=2, column=2, sticky="w", **pad)
        self.columnconfigure(0, weight=1)

        # Validation becomes one line here. The detail is still in the log
        # below for whoever wants it, but nobody has to read a wall of text
        # to find out whether their file is fine.
        self.status = tk.Label(self, text="", foreground="#555555", wraplength=620,
                               justify="left", anchor="w")
        self.status.grid(row=3, column=0, columnspan=3, sticky="w", pady=(6, 14))

        # --- step 2
        tk.Label(self, text="2.  Build", font=("", 10, "bold"),
                 anchor="w").grid(row=4, column=0, columnspan=3, sticky="w")

        self.progress_label = tk.Label(self, text="", foreground="#555555", anchor="w")
        self.progress_label.grid(row=5, column=0, columnspan=3, sticky="w", pady=(6, 2))

        self.progress = ttk.Progressbar(self, mode="determinate", length=300,
                                        maximum=1000)
        self.progress.grid(row=6, column=0, columnspan=3, sticky="w", pady=(0, 10))

        btns = ttk.Frame(self)
        btns.grid(row=7, column=0, columnspan=3, sticky="w", pady=(0, 14))
        self.compile_btn = ttk.Button(btns, text="Compile", command=self._build)
        self.compile_btn.pack(side="left")
        self.play_btn = ttk.Button(btns, text="Play", command=self._play,
                                   state="disabled")
        self.play_btn.pack(side="left", padx=6)
        # The build leaves hundreds of megabytes of generated C behind and the
        # game never reads them. The command line asks whether to delete them;
        # there is no terminal to ask on here, so it is a button that appears
        # once there is something to clean.
        self.clean_btn = ttk.Button(btns, text="Free build space",
                                    command=self._clean, state="disabled")
        self.clean_btn.pack(side="left", padx=6)

        # --- step 3
        tk.Label(self, text="3.  What is happening", font=("", 10, "bold"),
                 anchor="w").grid(row=8, column=0, columnspan=3, sticky="w")

        log_frame = ttk.Frame(self)
        log_frame.grid(row=9, column=0, columnspan=3, sticky="nsew")
        log_frame.columnconfigure(0, weight=1)
        log_frame.rowconfigure(0, weight=1)
        # Row 9 takes all the spare height, so the log fills the window
        # instead of leaving the bottom half empty.
        self.rowconfigure(9, weight=1)

        self.log = tk.Text(log_frame, height=18, width=86, wrap="none",
                           font=("monospace", 9), background="#111111",
                           foreground="#dddddd", insertbackground="#dddddd",
                           relief="flat", padx=8, pady=6)
        self.log.grid(row=0, column=0, sticky="nsew")
        sb = ttk.Scrollbar(log_frame, orient="vertical", command=self.log.yview)
        sb.grid(row=0, column=1, sticky="ns")
        self.log.configure(yscrollcommand=sb.set)
        self.log.tag_configure("erro", foreground="#ff6b6b")
        self.log.tag_configure("ok", foreground="#7bd88f")
        self.log.configure(state="disabled")

        # The on-screen log is the log. There is no "open full log" button:
        # the detail already lives here, and a button that opens a second
        # window with the same text is just one more thing to click.
        # build.sh still writes .build.log, which is what bug reports want.

    # ------------------------------------------------------------- helpers
    def _say(self, msg: str, tag: str = "") -> None:
        self.log.configure(state="normal")
        self.log.insert("end", msg + "\n", tag)
        self.log.see("end")
        self.log.configure(state="disabled")

    def _status(self, msg: str, color: str = "#555555") -> None:
        self.status.configure(text=msg, foreground=color)

    def _check_prereqs(self) -> None:
        if not GBRECOMP.exists():
            self._status(
                "Setup has not run yet. Close this window and run, in the "
                "project folder:\n\n    ./bootstrap.sh",
                "#b5651d",
            )
            self.compile_btn.configure(state="disabled")
            self._say("Tools are not ready yet ( run ./bootstrap.sh ).", "erro")
            return
        # A previous build may already exist: enable Play right away.
        # Without this, someone who built and reopened the window saw a grey
        # "Play" button with no explanation.
        if GAME.exists():
            self.play_btn.configure(state="normal")

    # -------------------------------------------------------------- actions
    def _browse(self) -> None:
        path = filedialog.askopenfilename(title="Choose the ROM", filetypes=ROM_TYPES)
        if path:
            self.rom_var.set(path)
            self._validate()

    def _validate(self) -> None:
        rom = self.rom_var.get().strip()
        if not rom:
            self._status("Choose a ROM file first.", "#b5651d")
            return
        proc = subprocess.run([sys.executable, str(VALIDATE), rom],
                              capture_output=True, text=True)
        out = (proc.stdout + proc.stderr).strip()
        if proc.returncode != 0:
            self._status("That does not look like a Game Boy ROM. Details below.",
                         "#b5651d")
            self._say(out, "erro")
            return

        # One line for the person, everything else in the log.
        def field(prefix: str) -> str:
            for line in out.splitlines():
                if line.strip().startswith(prefix):
                    return line.split(":", 1)[1].strip()
            return ""

        title = field("title") or "ROM"
        size = field("actual size")
        cartridge = field("cartridge")
        mode = field("mode")
        self._status(f"Valid ROM  -  {title}  -  {size}  -  {cartridge}  -  {mode}",
                     "#2e7d32")
        self._say(out, "ok")

    def _build(self) -> None:
        rom = self.rom_var.get().strip()
        if not rom:
            self._status("Choose a ROM file first.", "#b5651d")
            return
        if not GBRECOMP.exists():
            self._status("Run ./bootstrap.sh before building.", "#b5651d")
            return

        self.log.configure(state="normal")
        self.log.delete("1.0", "end")
        self.log.configure(state="disabled")
        self.compile_btn.configure(state="disabled")
        self.play_btn.configure(state="disabled")
        self.progress.configure(value=0)
        self._status("Building. This takes a few minutes: the ROM is being "
                     "translated into native code.", "#1a73e8")
        self.progress_label.configure(text="Starting...")
        self._say(f"$ ./build.sh {rom}")
        threading.Thread(target=self._run_build, args=(rom,), daemon=True).start()

    def _run_build(self, rom: str) -> None:
        self._proc = subprocess.Popen(
            [str(BUILD_SH), rom],
            cwd=REPO, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=1,
        )
        assert self._proc.stdout is not None
        for line in self._proc.stdout:
            self._q.put(("line", line.rstrip()))
        rc = self._proc.wait()
        self._q.put(("done", rc))

    def _drain(self) -> None:
        try:
            while True:
                kind, payload = self._q.get_nowait()
                if kind == "line":
                    self._on_line(str(payload))
                else:
                    self._finish(int(payload))
        except queue.Empty:
            pass
        self.after(100, self._drain)

    def _on_line(self, line: str) -> None:
        # Machine-readable markers: the interface never has to guess a stage
        # from English prose, which would break the day the wording changes.
        if line.startswith("GBPHASE "):
            phase = line.split(" ", 1)[1].strip()
            if phase == "translate":
                # One opaque step, no percentage available: be honest and
                # show it as indeterminate rather than inventing progress.
                self.progress.configure(mode="indeterminate")
                self.progress.start(12)
                self.progress_label.configure(text="Translating the ROM...")
            elif phase == "compile":
                self.progress.stop()
                self.progress.configure(mode="determinate", value=0, maximum=1000)
                self.progress_label.configure(text="Compiling 0 files")
            elif phase == "link":
                self.progress.stop()
                self.progress.configure(value=1000)
                self.progress_label.configure(text="Linking...")
            elif phase == "done":
                self.progress.stop()
                self.progress.configure(value=1000)
            return

        if line.startswith("GBPROGRESS "):
            parts = line.split()
            if len(parts) == 3:
                done, total = int(parts[1]), int(parts[2])
                pct = int(done * 1000 / total) if total else 0
                self.progress.configure(value=pct)
                self.progress_label.configure(
                    text=f"Compiling {done}/{total} files ({done * 100 // total}%)"
                )
            return

        tag = ""
        if line.startswith("ok"):
            tag = "ok"
        elif line.startswith("FAILED") or "ERROR" in line:
            tag = "erro"
        if line.strip() and not line.startswith(("cc ", "c++ ", "mkdir")):
            self._say(line, tag)

    def _finish(self, rc: int) -> None:
        self.progress.stop()
        self.compile_btn.configure(state="normal")
        self._proc = None
        if rc == 0:
            self.play_btn.configure(state="normal")
            if (REPO / "build" / "project").exists():
                self.clean_btn.configure(state="normal")
            self.progress_label.configure(text="Ready.")
            self._status("Done. You can play it now.", "#2e7d32")
        else:
            self.progress_label.configure(text="Stopped.")
            self._status(
                "The build failed. The reason is in the log above, and kept "
                "in .build.log.", "#b5651d",
            )

    def _clean(self) -> None:
        # Delete the generated code and the object files, keeping the game
        # and the save file. The executable links only against system
        # libraries, so it runs perfectly well without them.
        build = REPO / "build"
        freed = 0
        for name in ("project", "work"):
            path = build / name
            if path.exists():
                freed += sum(f.stat().st_size for f in path.rglob("*") if f.is_file())
                shutil.rmtree(path, ignore_errors=True)
        self.clean_btn.configure(state="disabled")
        self.progress_label.configure(text="Build space freed.")
        self._say(f"Removed the generated code: about {freed / (1024 * 1024):.0f} MB "
                  f"freed. The game and your saves are still there.")

    def _play(self) -> None:
        if GAME.exists():
            subprocess.Popen([str(RUN_SH)], cwd=REPO)


if __name__ == "__main__":
    App().mainloop()