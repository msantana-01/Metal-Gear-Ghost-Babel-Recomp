#!/usr/bin/env python3
"""Interface gráfica: escolher a ROM, compilar, jogar.

Não tenta ser bonita. Tenta ser Impossible de errar: um botão por vez,
log sempre visível, e nenhuma mensagem que não diga o que fazer.

    python3 tools/gui/app.py
"""

from __future__ import annotations

import os
import queue
import subprocess
import sys
import threading
from pathlib import Path

try:
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
except ImportError:  # pragma: no cover
    sys.stderr.write(
        "A interface grafica precisa da biblioteca do Tk, que nao veio.\n\n"
        "    sudo apt-get install -y python3-tk      # Debian/Ubuntu\n"
        "    sudo pacman -S --needed tk               # Arch/Omarchy\n"
        "    sudo dnf install -y python3-tkinter      # Fedora\n\n"
        "Alternativa: use a linha de comando, que faz o mesmo trabalho:\n"
        "    ./build.sh /caminho/da/rom.gbc\n"
    )
    raise SystemExit(1)

REPO = Path(__file__).resolve().parent.parent.parent
GBRECOMP = REPO / ".tools" / "gbrecomp"
BUILD_SH = REPO / "build.sh"
RUN_SH = REPO / "run.sh"
GAME = REPO / "build" / "ghost-babel"
VALIDATE = REPO / "tools" / "validate_rom.py"

ROM_TYPES = [
    ("ROMs de Game Boy", ("*.gbc", "*.GBC", "*.gb", "*.GB")),
    ("Todos os arquivos", ("*",)),
]


class App(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("Metal Gear Ghost Babel Recomp")
        self.configure(padx=18, pady=14)
        self.resizable(True, False)

        self._q: queue.Queue[tuple[str, object]] = queue.Queue()
        self._proc: subprocess.Popen | None = None

        self._build_widgets()

        self.after(100, self._drain)
        self._check_prereqs()

    # ------------------------------------------------------------- layout
    def _build_widgets(self) -> None:
        pad = {"padx": 6, "pady": 4}

        tk.Label(
            self,
            text="Roda o seu jogo de Game Boy como programa nativo no PC.",
            font=("", 11),
        ).grid(row=0, column=0, columnspan=3, sticky="w", pady=(0, 12))

        # --- passo 1: escolher a ROM
        tk.Label(self, text="1. Escolha a sua ROM:").grid(
            row=1, column=0, columnspan=3, sticky="w"
        )
        self.rom_var = tk.StringVar()
        self.rom_entry = ttk.Entry(self, textvariable=self.rom_var)
        self.rom_entry.grid(row=2, column=0, sticky="ew", **pad)
        self.rom_entry.bind("<Return>", lambda _e: self._validate())

        ttk.Button(self, text="Procurar...", command=self._browse).grid(
            row=2, column=1, sticky="w", **pad
        )
        ttk.Button(self, text="Conferir", command=self._validate).grid(
            row=2, column=2, sticky="w", **pad
        )
        self.columnconfigure(0, weight=1)

        self.status = tk.Label(self, text="", foreground="#555555", wraplength=620,
                               justify="left")
        self.status.grid(row=3, column=0, columnspan=3, sticky="w", pady=(6, 12))

        # --- passo 2: compilar
        tk.Label(self, text="2. Compilar:").grid(
            row=4, column=0, columnspan=3, sticky="w"
        )
        btns = ttk.Frame(self)
        btns.grid(row=5, column=0, columnspan=3, sticky="w", pady=(4, 12))
        self.compile_btn = ttk.Button(btns, text="Compilar", command=self._build)
        self.compile_btn.pack(side="left")
        self.play_btn = ttk.Button(btns, text="Jogar", command=self._play, state="disabled")
        self.play_btn.pack(side="left", padx=6)

        self.progress = ttk.Progressbar(self, mode="indeterminate", length=300)
        self.progress.grid(row=6, column=0, columnspan=3, sticky="w", pady=(0, 12))

        # --- passo 3: log
        tk.Label(self, text="3. O que está acontecendo:").grid(
            row=7, column=0, columnspan=3, sticky="w"
        )
        log_frame = ttk.Frame(self)
        log_frame.grid(row=8, column=0, columnspan=3, sticky="nsew")
        log_frame.columnconfigure(0, weight=1)
        log_frame.rowconfigure(0, weight=1)

        self.log = tk.Text(log_frame, height=14, width=78, wrap="none",
                           font=("monospace", 9), background="#111111",
                           foreground="#dddddd", insertbackground="#dddddd")
        self.log.grid(row=0, column=0, sticky="nsew")
        sb = ttk.Scrollbar(log_frame, orient="vertical", command=self.log.yview)
        sb.grid(row=0, column=1, sticky="ns")
        self.log.configure(yscrollcommand=sb.set)
        self.log.tag_configure("erro", foreground="#ff6b6b")
        self.log.tag_configure("ok", foreground="#7bd88f")

        ttk.Button(self, text="Abrir log completo", command=self._open_log).grid(
            row=9, column=0, columnspan=3, sticky="w", pady=(8, 0)
        )

    # ------------------------------------------------------------ helpers
    def _say(self, msg: str, tag: str = "") -> None:
        self.log.insert("end", msg + "\n", tag)
        self.log.see("end")

    def _status(self, msg: str, color: str = "#555555") -> None:
        self.status.configure(text=msg, foreground=color)

    def _check_prereqs(self) -> None:
        if not GBRECOMP.exists():
            self._status(
                "Falta a preparação. Feche esta janela e rode, na pasta do projeto:\n"
                "    ./bootstrap.sh",
                "#b5651d",
            )
            self.compile_btn.configure(state="disabled")
            self._say("Ferramentas ainda não preparadas ( rode ./bootstrap.sh ).", "erro")
            return
        if not GAME.exists():
            self.play_btn.configure(state="disabled")
        else:
            # O jogo já foi compilado numa sessão anterior: habilita direto.
            # Sem isto, quem compila e reabre a janela vê "Jogar" cinza sem
            # nenhuma explicação.
            self.play_btn.configure(state="normal")

    def _open_log(self) -> None:
        for candidate in (REPO / ".build.log", REPO / ".bootstrap.log"):
            if candidate.exists():
                if sys.platform.startswith("linux"):
                    subprocess.Popen(["xdg-open", str(candidate)])
                elif sys.platform == "darwin":
                    subprocess.Popen(["open", str(candidate)])
                else:  # pragma: no cover
                    os.startfile(candidate)  # type: ignore[attr-defined]
                return
        messagebox.showinfo("Log", "Ainda não há log: nenhum build foi tentado ainda.")

    # -------------------------------------------------------------- ações
    def _browse(self) -> None:
        path = filedialog.askopenfilename(title="Escolha a ROM", filetypes=ROM_TYPES)
        if path:
            self.rom_var.set(path)
            self._validate()

    def _validate(self) -> None:
        rom = self.rom_var.get().strip()
        if not rom:
            self._status("Escolha um arquivo de ROM primeiro.", "#b5651d")
            return
        proc = subprocess.run(
            [sys.executable, str(VALIDATE), rom],
            capture_output=True, text=True,
        )
        out = (proc.stdout + proc.stderr).strip()
        if proc.returncode == 0:
            title = next(
                (ln.strip() for ln in out.splitlines() if "título" in ln), "ROM válida"
            )
            self._status(f"{title} — pode compilar.", "#2e7d32")
            self._say(out, "ok")
        else:
            self._status("Isso não parece uma ROM de Game Boy. Veja o detalhe abaixo.",
                         "#b5651d")
            self._say(out, "erro")

    def _build(self) -> None:
        rom = self.rom_var.get().strip()
        if not rom:
            self._status("Escolha um arquivo de ROM primeiro.", "#b5651d")
            return
        if not GBRECOMP.exists():
            self._status("Rode ./bootstrap.sh antes de compilar.", "#b5651d")
            return

        self.log.delete("1.0", "end")
        self.compile_btn.configure(state="disabled")
        self.play_btn.configure(state="disabled")
        self.progress.start(12)
        self._status("Compilando. Leva alguns minutos — é a ROM virando código nativo.",
                     "#1a73e8")
        self._say(f"$ ./build.sh {rom}")
        threading.Thread(target=self._run_build, args=(rom,), daemon=True).start()

    def _run_build(self, rom: str) -> None:
        assert self._proc is None
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
                    line = str(payload)
                    tag = ""
                    if line.startswith("ok") or "Pronto" in line:
                        tag = "ok"
                    elif line.startswith("FALTOU") or "ERRO" in line or "falhou" in line:
                        tag = "erro"
                    if line.strip():
                        self._say(line, tag)
                else:
                    self._finish(int(payload))
        except queue.Empty:
            pass
        self.after(100, self._drain)

    def _finish(self, rc: int) -> None:
        self.progress.stop()
        self.compile_btn.configure(state="normal")
        self._proc = None
        if rc == 0:
            self.play_btn.configure(state="normal")
            self._status("Pronto! Pode jogar.", "#2e7d32")
        else:
            self._status(
                "A compilação falhou. O motivo está no log acima — e em .build.log.",
                "#b5651d",
            )

    def _play(self) -> None:
        if GAME.exists():
            subprocess.Popen([str(RUN_SH)], cwd=REPO)


if __name__ == "__main__":
    App().mainloop()