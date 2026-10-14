#!/usr/bin/env python3
"""Measure interactive frame pacing of a recompiled game.

Why the mean is not enough: a game holding 60 fps on average can still stall
for a quarter of a second twice a minute, and the average hides it completely.
So this reports the tail (p95/p99/max) and the worst frames, not just the mean.

The runtime paces itself against a wall clock derived from the DMG frame length
(70224 cycles at 4194304 Hz = 16.7427 ms = 59.727 fps), and with
--debug-performance it emits one line per frame:

    [FRAME] #N emu=..ms render=..ms upload=..ms compose=..ms present=..ms
            cycles=.. fallbacks=.. interp_instr=.. interp_cycles=.. ...
    [VSYNC] #N wait=..ms cycles=..

Those lines carry per-phase costs but no timestamps. Adding up the phase costs
gives a number ~0.2 ms larger than the real interval, because the phase timers
overlap the pacing call -- so the interval is measured here by timestamping the
lines as they arrive instead. Two facts, measured two ways, both reported.

Usage:
    python3 tools/frame_pacing.py build/ghost-babel [--frames N] [--extra ...]

The binary is launched with a window and --debug-performance, so this measures
what a player experiences. It needs a display.
"""

from __future__ import annotations

import argparse
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path

# The DMG frame the runtime paces against: 70224 cycles at 4194304 Hz.
TARGET_FRAME_MS = 70224.0 / 4194304.0 * 1000.0
PHASES = ("emu", "render", "upload", "compose", "present")

FRAME_RE = re.compile(
    r"\[FRAME\] #(\d+) emu=([\d.]+)ms render=([\d.]+)ms upload=([\d.]+)ms "
    r"compose=([\d.]+)ms present=([\d.]+)ms cycles=(\d+) fallbacks=(\d+) "
    r"interp_instr=(\d+) interp_cycles=(\d+) "
    r"first=([0-9A-Fa-f]+:[0-9A-Fa-f]+) last=([0-9A-Fa-f]+:[0-9A-Fa-f]+) "
    r"total_fallbacks=(\d+) lcd_off_cycles=(\d+) lcd_transitions=(\d+)"
)
VSYNC_RE = re.compile(r"\[VSYNC\] #(\d+) wait=([\d.]+)ms")


def percentile(values: list[float], q: float) -> float:
    """Return the q-th percentile (0-100) of values, nearest-rank."""
    if not values:
        return 0.0
    ordered = sorted(values)
    idx = min(len(ordered) - 1, int(round((q / 100.0) * (len(ordered) - 1))))
    return ordered[idx]


def collect(
    binary: Path, frames: int, extra: list[str], timeout: float
) -> tuple[dict[int, float], dict[int, dict[str, float]], dict[int, float]]:
    """Run the game and timestamp its [FRAME] lines as they arrive.

    Returns (arrival times by frame index, phase costs by frame, pacing wait).
    """
    cmd = [
        str(binary),
        "--debug-performance",
        "--log-slow-frames",
        "0.05",
        "--log-slow-vsync",
        "0.01",
        *extra,
    ]
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    arrival: dict[int, float] = {}
    phases: dict[int, dict[str, float]] = {}
    waits: dict[int, float] = {}
    deadline = time.monotonic() + timeout
    assert proc.stdout is not None
    try:
        for line in proc.stdout:
            if time.monotonic() > deadline:
                break
            now = time.monotonic()
            if m := FRAME_RE.search(line):
                n = int(m.group(1))
                arrival[n] = now
                phases[n] = {name: float(m.group(i + 2)) for i, name in enumerate(PHASES)}
                phases[n]["fallbacks"] = float(m.group(8))
                phases[n]["interp_instr"] = float(m.group(9))
                phases[n]["last_pc"] = float(int(m.group(12).replace(":", ""), 16))
                phases[n]["lcd_off"] = float(m.group(14))
                phases[n]["lcd_trans"] = float(m.group(15))
                if n >= frames:
                    break
            elif m := VSYNC_RE.search(line):
                waits[int(m.group(1))] = float(m.group(2))
    finally:
        proc.kill()
        proc.wait()
    return arrival, phases, waits


def main() -> int:
    ap = argparse.ArgumentParser(description="Measure interactive frame pacing.")
    ap.add_argument("binary", type=Path, help="path to the recompiled game")
    ap.add_argument("--frames", type=int, default=2600, help="frames to measure")
    ap.add_argument("--warmup", type=int, default=60, help="frames to skip after startup")
    ap.add_argument("--timeout", type=float, default=300.0, help="seconds before giving up")
    ap.add_argument("--extra", nargs=argparse.REMAINDER, default=[],
                    help="extra flags passed to the game")
    args = ap.parse_args()

    if not args.binary.is_file():
        print(f"not a file: {args.binary}", file=sys.stderr)
        return 1

    arrival, phases, waits = collect(args.binary, args.frames, args.extra, args.timeout)
    ids = [n for n in sorted(arrival) if n > args.warmup and n in waits and n + 1 in arrival]
    if len(ids) < 100:
        print(
            f"only {len(ids)} usable frames after warmup; was the window opened?",
            file=sys.stderr,
        )
        return 1

    # The loop prints [FRAME] N, then [VSYNC] N, then [FRAME] N+1, so the wall
    # interval between two [FRAME] lines is that frame's pacing wait plus the
    # next frame's phase costs. Anything left over is time no timer covers.
    intervals = [(arrival[ids[i + 1]] - arrival[ids[i]]) * 1000.0 for i in range(len(ids) - 1)]
    spent = [sum(phases[n][p] for p in PHASES) for n in ids]
    emu = [phases[n]["emu"] for n in ids]

    print(f"frames measured: {len(intervals)} (warmup of {args.warmup} skipped)")
    print(f"engine pacing target: {TARGET_FRAME_MS:.4f} ms ({1000.0 / TARGET_FRAME_MS:.3f} fps)")
    print()
    print("real interval between frames (wall clock, what the player feels)")
    for q in (50, 95, 99):
        print(f"  p{q:<3} {percentile(intervals, q):8.3f} ms")
    print(f"  max  {max(intervals):8.3f} ms")
    print(f"  mean {statistics.fmean(intervals):8.3f} ms")
    print()
    print(f"  fps from p50 interval:  {1000.0 / percentile(intervals, 50):.3f}")
    print(f"  fps from mean interval: {1000.0 / statistics.fmean(intervals):.3f}")
    over = [i for i in intervals if i > TARGET_FRAME_MS * 1.5]
    print(f"  intervals over {TARGET_FRAME_MS * 1.5:.1f} ms: {len(over)} "
          f"({100.0 * len(over) / len(intervals):.3f}%)")
    print()
    print("time producing a frame, by phase")
    for name in PHASES:
        vals = [phases[n][name] for n in ids]
        print(f"  {name:<8} p50 {percentile(vals, 50):7.3f}  p99 {percentile(vals, 99):7.3f}  "
              f"max {max(vals):7.3f} ms")
    print(f"  {'total':<8} p50 {percentile(spent, 50):7.3f}  p99 {percentile(spent, 99):7.3f}  "
          f"max {max(spent):7.3f} ms")
    budget = TARGET_FRAME_MS
    used = 100.0 * statistics.fmean(spent) / budget
    print(f"  frame budget used: {used:.1f}% of {budget:.2f} ms")
    print(f"  emulation: p50 {percentile(emu, 50):.3f}  p99 {percentile(emu, 99):.3f}  "
          f"max {max(emu):.3f} ms")
    print()
    fb = sum(phases[n]["fallbacks"] for n in ids)
    instr = sum(phases[n]["interp_instr"] for n in ids)
    per_frame = [phases[n]["fallbacks"] for n in ids]
    hit = sum(1 for f in per_frame if f > 0)
    print(f"interpreter fallbacks: {int(fb)} total, {hit} of {len(ids)} frames "
          f"({100.0 * hit / len(ids):.1f}%)")
    print(f"interpreter instructions: {int(instr)} total")
    print(f"  fallbacks per frame: p50 {percentile(per_frame, 50):.0f}  "
          f"p99 {percentile(per_frame, 99):.0f}  max {max(per_frame):.0f}")
    print()
    print("worst intervals, with time no phase timer accounts for")
    pairs = sorted(zip(intervals, ids), reverse=True)[:5]
    for gap, n in pairs:
        work = sum(phases[n + 1][p] for p in PHASES)
        unaccounted = gap - waits[n] - work
        parts = " ".join(f"{p}={phases[n + 1][p]:.2f}" for p in PHASES)
        print(f"  #{n}->#{n + 1}: {gap:8.2f} ms = wait {waits[n]:.2f} + {parts} "
              f"+ unaccounted {unaccounted:.2f}")
        print(f"      fallbacks {int(phases[n + 1]['fallbacks'])} "
              f"(p50 {percentile(per_frame, 50):.0f}), "
              f"interp_instr {int(phases[n + 1]['interp_instr'])}, "
              f"last_pc {int(phases[n + 1]['last_pc']):06X}, "
              f"lcd_off {int(phases[n + 1]['lcd_off'])}, "
              f"lcd_trans {int(phases[n + 1]['lcd_trans'])}")
    lcd = [phases[n]["lcd_off"] for n in ids]
    print(f"  lcd_off per frame: p50 {percentile(lcd, 50):.0f}  p99 {percentile(lcd, 99):.0f}  "
          f"max {max(lcd):.0f}")
    pcs = [phases[n]["last_pc"] for n in ids]
    print(f"  distinct last_pc values across {len(ids)} frames: {len(set(pcs))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())