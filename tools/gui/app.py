#!/usr/bin/env python3
"""Graphical front end: pick a ROM, prepare tools, build, play.

It does not try to be pretty. It tries to be impossible to get wrong: one
step at a time, progress you can actually read, and no message that does not
say what to do next.

    python3 tools/gui/app.py
"""

from __future__ import annotations

import os
import queue
import re
import shutil
import signal
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
TOOLS_DIR = REPO / ".tools"
GBRECOMP = TOOLS_DIR / "gbrecomp"
TOOLS_VER_FILE = TOOLS_DIR / "VERSION"
PINS_LOCK = REPO / "pins.lock"
BOOTSTRAP_SH = REPO / "bootstrap.sh"
BUILD_SH = REPO / "build.sh"
RUN_SH = REPO / "run.sh"
GAME_REMASTER = REPO / "build" / "ghost-babel-remaster"
GAME_AUTHENTIC = REPO / "build" / "ghost-babel-authentic"
GAME_LEGACY = REPO / "build" / "ghost-babel"
VALIDATE = REPO / "tools" / "validate_rom.py"
ICON = Path(__file__).with_name("icon.png")

ROM_TYPES = [
    ("Game Boy ROMs", ("*.gbc", "*.GBC", "*.gb", "*.GB")),
    ("All files", ("*",)),
]

TITLE = "Metal Gear: Ghost Babel - Recomp"
SUBTITLE = "Build your own copy of the ROM and play it natively in widescreen or authentic mode"

ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")


def _expected_tools_version() -> str:
    if not PINS_LOCK.exists():
        return ""
    in_tools = False
    for raw in PINS_LOCK.read_text(errors="replace").splitlines():
        line = raw.strip()
        if line.startswith("["):
            in_tools = line == "[tools]"
        elif in_tools and line.startswith("version"):
            _, _, val = line.partition("=")
            return val.strip()
    return ""


class App(tk.Tk):
    BG = "#f4f4f5"
    WIDTH, HEIGHT = 720, 820

    def __init__(self) -> None:
        super().__init__()
        self.title(TITLE)
        self.configure(padx=20, pady=16, background=self.BG)
        self.resizable(True, True)
        self._fit_to_screen()

        self._q: queue.Queue[tuple[str, object]] = queue.Queue()
        self._proc: subprocess.Popen[str] | None = None
        self._active_task: str = ""
        self._icon_img: tk.PhotoImage | None = None

        self._build_widgets()
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.after(100, self._drain)
        self._refresh_buttons()

    def _fit_to_screen(self) -> None:
        sw, sh = self.winfo_screenwidth(), self.winfo_screenheight()
        width = min(self.WIDTH, max(500, sw - 80))
        height = min(self.HEIGHT, max(460, sh - 120))
        self.geometry(f"{width}x{height}")

    # ------------------------------------------------------------------ ui
    def _build_widgets(self) -> None:
        pad = {"padx": 6, "pady": 4}

        head = tk.Frame(self, background=self.BG)
        head.grid(row=0, column=0, columnspan=3, sticky="w", pady=(0, 14))
        if ICON.exists():
            try:
                self._icon_img = tk.PhotoImage(file=str(ICON))
                if self._icon_img.width() > 64:
                    factor = max(1, self._icon_img.width() // 64)
                    self._icon_img = self._icon_img.subsample(factor, factor)
                tk.Label(head, image=self._icon_img, borderwidth=0).pack(side="left", padx=(0, 12))
            except tk.TclError:
                self._icon_img = None

        titles = tk.Frame(head, background=self.BG)
        titles.pack(side="left")
        tk.Label(titles, text=TITLE, font=("", 13, "bold"), anchor="w").pack(anchor="w")
        tk.Label(
            titles,
            text=SUBTITLE,
            foreground="#555555",
            anchor="w",
            wraplength=500,
            justify="left",
        ).pack(anchor="w")

        # --- step 1
        tk.Label(self, text="1.  Choose your ROM", font=("", 10, "bold"), anchor="w").grid(
            row=1, column=0, columnspan=3, sticky="w"
        )
        self.rom_var = tk.StringVar()
        self.rom_entry = ttk.Entry(self, textvariable=self.rom_var)
        self.rom_entry.grid(row=2, column=0, sticky="ew", **pad)
        self.rom_entry.bind("<Return>", lambda _e: self._validate())

        ttk.Button(self, text="Browse...", command=self._browse).grid(
            row=2, column=1, sticky="w", **pad
        )
        ttk.Button(self, text="Check", command=self._validate).grid(
            row=2, column=2, sticky="w", **pad
        )
        self.columnconfigure(0, weight=1)

        self.status = tk.Label(
            self, text="", foreground="#555555", wraplength=660, justify="left", anchor="w"
        )
        self.status.grid(row=3, column=0, columnspan=3, sticky="w", pady=(6, 14))

        # --- step 2
        tk.Label(self, text="2.  Build & Play", font=("", 10, "bold"), anchor="w").grid(
            row=4, column=0, columnspan=3, sticky="w"
        )

        self.progress_label = tk.Label(self, text="", foreground="#555555", anchor="w")
        self.progress_label.grid(row=5, column=0, columnspan=3, sticky="w", pady=(6, 2))

        self.progress = ttk.Progressbar(self, mode="determinate", length=340, maximum=1000)
        self.progress.grid(row=6, column=0, columnspan=3, sticky="w", pady=(0, 10))

        btns = ttk.Frame(self)
        btns.grid(row=7, column=0, columnspan=3, sticky="w", pady=(0, 14))
        self.setup_btn = ttk.Button(btns, text="Setup Tools", command=self._setup)
        self.compile_btn = ttk.Button(btns, text="Compile", command=self._build)
        self.cancel_btn = ttk.Button(btns, text="Cancel", command=self._cancel, state="disabled")
        self.remaster_btn = ttk.Button(
            btns, text="Play Remaster (Widescreen)", command=self._play_remaster, state="disabled"
        )
        self.authentic_btn = ttk.Button(
            btns, text="Play Authentic (160px)", command=self._play_authentic, state="disabled"
        )
        self.clean_btn = ttk.Button(
            btns, text="Free build space", command=self._clean, state="disabled"
        )

        self.setup_btn.pack(side="left", padx=(0, 6))
        self.compile_btn.pack(side="left", padx=(0, 6))
        self.cancel_btn.pack(side="left", padx=(0, 12))
        self.remaster_btn.pack(side="left", padx=(0, 6))
        self.authentic_btn.pack(side="left", padx=(0, 6))
        self.clean_btn.pack(side="left")

        # --- step 3
        tk.Label(self, text="3.  What is happening", font=("", 10, "bold"), anchor="w").grid(
            row=8, column=0, columnspan=3, sticky="w"
        )

        log_frame = ttk.Frame(self)
        log_frame.grid(row=9, column=0, columnspan=3, sticky="nsew")
        log_frame.columnconfigure(0, weight=1)
        log_frame.rowconfigure(0, weight=1)
        self.rowconfigure(9, weight=1)

        self.log = tk.Text(
            log_frame,
            height=18,
            width=88,
            wrap="none",
            font=("monospace", 9),
            background="#111111",
            foreground="#dddddd",
            insertbackground="#dddddd",
            relief="flat",
            padx=8,
            pady=6,
        )
        self.log.grid(row=0, column=0, sticky="nsew")
        sb = ttk.Scrollbar(log_frame, orient="vertical", command=self.log.yview)
        sb.grid(row=0, column=1, sticky="ns")
        self.log.configure(yscrollcommand=sb.set)
        self.log.tag_configure("erro", foreground="#ff6b6b")
        self.log.tag_configure("ok", foreground="#7bd88f")
        self.log.configure(state="disabled")

    # ------------------------------------------------------------- helpers
    def _say(self, msg: str, tag: str = "") -> None:
        clean = ANSI_RE.sub("", msg)
        self.log.configure(state="normal")
        self.log.insert("end", clean + "\n", tag)
        self.log.see("end")
        self.log.configure(state="disabled")

    def _status(self, msg: str, color: str = "#555555") -> None:
        self.status.configure(text=msg, foreground=color)

    def _tools_status(self) -> tuple[bool, str]:
        want = _expected_tools_version()
        have_ver = TOOLS_VER_FILE.read_text(errors="replace").strip() if TOOLS_VER_FILE.exists() else ""
        if not GBRECOMP.exists() or not (TOOLS_DIR / "libgbrt.a").exists():
            return False, "Tools not prepared yet. Click 'Setup Tools' or run ./bootstrap.sh."
        if want and have_ver != want:
            return False, f"Tools need update (installed: {have_ver or 'legacy'}, expected: {want}). Click 'Setup Tools'."
        return True, f"Tools ready (v{have_ver or want or 'local'})."

    def _refresh_buttons(self) -> None:
        running = self._proc is not None
        tools_ok, tools_msg = self._tools_status()

        self.setup_btn.configure(state="disabled" if running else "normal")
        self.compile_btn.configure(state="normal" if (not running and tools_ok) else "disabled")
        self.cancel_btn.configure(state="normal" if running else "disabled")

        has_remaster = GAME_REMASTER.exists()
        has_auth = GAME_AUTHENTIC.exists() or GAME_LEGACY.exists()
        self.remaster_btn.configure(state="normal" if (not running and has_remaster) else "disabled")
        self.authentic_btn.configure(state="normal" if (not running and has_auth) else "disabled")

        has_work = (REPO / "build" / "project").exists() or (REPO / "build" / "work").exists()
        self.clean_btn.configure(state="normal" if (not running and has_work) else "disabled")

        if not running and not tools_ok:
            self._status(tools_msg, "#b5651d")

    # -------------------------------------------------------------- actions
    def _browse(self) -> None:
        path = filedialog.askopenfilename(title="Choose the ROM", filetypes=ROM_TYPES)
        if path:
            self.rom_var.set(path)
            self._validate()

    def _validate(self) -> bool:
        rom = self.rom_var.get().strip()
        if not rom:
            self._status("Choose a ROM file first.", "#b5651d")
            return False
        proc = subprocess.run([sys.executable, str(VALIDATE), rom], capture_output=True, text=True)
        out = (proc.stdout + proc.stderr).strip()
        if proc.returncode != 0:
            self._status("That does not look like a Game Boy ROM. Details below.", "#b5651d")
            self._say(out, "erro")
            return False

        def field(prefix: str) -> str:
            for line in out.splitlines():
                if line.strip().startswith(prefix):
                    return line.split(":", 1)[1].strip()
            return ""

        title = field("title") or "ROM"
        size = field("actual size")
        cartridge = field("cartridge")
        mode = field("mode")
        self._status(f"Valid ROM  -  {title}  -  {size}  -  {cartridge}  -  {mode}", "#2e7d32")
        self._say(out, "ok")
        return True

    def _setup(self) -> None:
        self._start_process("setup", [str(BOOTSTRAP_SH)], "Preparing tools (./bootstrap.sh)...")

    def _build(self) -> None:
        rom = self.rom_var.get().strip()
        if not rom:
            self._status("Choose a ROM file first.", "#b5651d")
            return
        tools_ok, tools_msg = self._tools_status()
        if not tools_ok:
            self._status(tools_msg, "#b5651d")
            return
        self._start_process(
            "build",
            [str(BUILD_SH), rom],
            "Building. This takes a few minutes: translating and compiling your ROM...",
        )

    def _start_process(self, kind: str, cmd: list[str], status_text: str) -> None:
        if self._proc is not None:
            return
        self._active_task = kind
        self.log.configure(state="normal")
        self.log.delete("1.0", "end")
        self.log.configure(state="disabled")

        self.progress.configure(mode="indeterminate", value=0)
        self.progress.start(12)
        self._status(status_text, "#1a73e8")
        self.progress_label.configure(text="Starting...")
        self._say(f"$ {' '.join(cmd)}")

        def worker() -> None:
            try:
                self._proc = subprocess.Popen(
                    cmd,
                    cwd=REPO,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    bufsize=1,
                    start_new_session=True,
                )
                assert self._proc.stdout is not None
                for line in self._proc.stdout:
                    self._q.put(("line", line.rstrip("\r\n")))
                rc = self._proc.wait()
            except OSError as exc:
                self._q.put(("line", f"FAILED: {exc}"))
                rc = 1
            self._q.put(("done", rc))

        threading.Thread(target=worker, daemon=True).start()
        self._refresh_buttons()

    def _cancel(self) -> None:
        if self._proc is None:
            return
        self._say("Cancelling process...", "erro")
        try:
            os.killpg(os.getpgid(self._proc.pid), signal.SIGTERM)
        except OSError:
            try:
                self._proc.terminate()
            except OSError:
                pass

    def _on_close(self) -> None:
        self._cancel()
        self.destroy()

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
        clean = ANSI_RE.sub("", line)
        if clean.startswith("GBPHASE "):
            phase = clean.split(" ", 1)[1].strip()
            if phase == "translate":
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
                self.progress_label.configure(text="Linking executable(s)...")
            elif phase == "done":
                self.progress.stop()
                self.progress.configure(value=1000)
            return

        if clean.startswith("GBPROGRESS "):
            parts = clean.split()
            if len(parts) == 3:
                done, total = int(parts[1]), int(parts[2])
                pct = int(done * 1000 / total) if total else 0
                self.progress.configure(value=pct)
                self.progress_label.configure(
                    text=f"Compiling {done}/{total} files ({done * 100 // total if total else 0}%)"
                )
            return

        tag = ""
        stripped = clean.lstrip()
        if stripped.startswith("ok"):
            tag = "ok"
        elif stripped.startswith("FAILED") or "ERROR" in stripped:
            tag = "erro"
        if stripped and not stripped.startswith(("cc ", "c++ ", "mkdir")):
            self._say(clean, tag)

    def _finish(self, rc: int) -> None:
        self.progress.stop()
        task = self._active_task
        self._proc = None
        self._active_task = ""
        self._refresh_buttons()
        if rc == 0:
            self.progress.configure(mode="determinate", value=1000)
            if task == "setup":
                self.progress_label.configure(text="Tools ready.")
                self._status("Setup complete. Choose your ROM and click Compile.", "#2e7d32")
            else:
                self.progress_label.configure(text="Ready.")
                self._status("Done. Choose Play Remaster (Widescreen) or Play Authentic.", "#2e7d32")
        else:
            self.progress.configure(mode="determinate", value=0)
            self.progress_label.configure(text="Stopped.")
            log_name = ".bootstrap.log" if task == "setup" else ".build.log"
            self._status(
                f"Failed (exit {rc}). See the log above or {log_name}.",
                "#b5651d",
            )

    def _clean(self) -> None:
        build = REPO / "build"
        freed = 0
        for name in ("project", "work"):
            path = build / name
            if path.exists():
                freed += sum(f.stat().st_size for f in path.rglob("*") if f.is_file())
                shutil.rmtree(path, ignore_errors=True)
        self._refresh_buttons()
        self.progress_label.configure(text="Build space freed.")
        self._say(
            f"Removed generated build artifacts: about {freed / (1024 * 1024):.0f} MB "
            f"freed. Your executables and saves are preserved."
        )

    def _play_remaster(self) -> None:
        if GAME_REMASTER.exists():
            subprocess.Popen([str(RUN_SH)], cwd=REPO)

    def _play_authentic(self) -> None:
        if GAME_AUTHENTIC.exists() or GAME_LEGACY.exists():
            subprocess.Popen([str(RUN_SH), "--authentic"], cwd=REPO)


if __name__ == "__main__":
    App().mainloop()
