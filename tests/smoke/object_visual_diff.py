#!/usr/bin/env py -3
"""Stage 2.E pinned-camera visual-diff harness.

Phase 1 Step 1.6 — Python orchestration of the engine-side capture path.

Modes:
  --measure-variance   Run each mission TWICE at MC2_GPU_OBJECTS=0;
                       compute per-pixel diff stats; report. NOT a gate.
                       This is the architectural-validation step: does
                       the engine's natural mission camera produce
                       reproducible output across same-config runs?

  --gate               Run each mission TWICE at MC2_GPU_OBJECTS=0;
                       apply spec tolerance (<=2 LSB / <=0.5%);
                       PASS / FAIL gate. Exit 0 / 2.

Per-mission frameN values in CAPTURE_FRAMES below are frames AFTER
gos_terrain_indirect::IsFrameSolidArmed() returns true (intro pan
complete; the engine's own gate). The wall-clock intro-pan jitter is
upstream of this latch, so small values (60-90) are sufficient.

Architecture per Stage 2.E phase 1 plan (round-6 simplification):
  - No camera teleport. Engine captures the natural mission camera at
    frameN ticks after gos_terrain_indirect::IsFrameSolidArmed().
  - mc2.exe launches sequentially (feedback_smoke_serial_only.md).
  - No menu canary (feedback_smoke_no_canary.md).
"""
from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
# OWNER RULE: the engine is launched through nofocus.run ONLY (background launch).
from smoke_lib import nofocus  # noqa: E402

# -----------------------------------------------------------------------------
# Configuration
# -----------------------------------------------------------------------------

TIER1   = ["mc2_01", "mc2_03", "mc2_10", "mc2_17", "mc2_24"]
STRESS  = ["mc2_18"]
MISSIONS = TIER1 + STRESS

# Per-mission capture frame. Frames AFTER gos_terrain_indirect::IsFrameSolidArmed()
# / intro-pan-complete; small values OK because the wall-clock intro-pan
# jitter is upstream of the engine-side latch and cancels across runs. 60
# frames is ~0.4-1s of settle depending on FPS -- plenty for
# terrain/shadow/water-reflection to converge.
CAPTURE_FRAMES = {
    "mc2_01": 60,
    "mc2_03": 60,
    "mc2_05": 60,
    "mc2_06": 60,
    "mc2_10": 60,
    "mc2_17": 60,
    "mc2_18": 60,
    "mc2_24": 60,
}

# Spec tolerance: pixel "differs" if any channel delta > 2 LSB.
# Mission gate: count(differing) / count(total) <= 0.005 (0.5%).
LSB_THRESHOLD     = 2
RATIO_THRESHOLD   = 0.005

# Hot-regions grid for cause diagnosis.
HOT_GRID = 8

# Build & deploy paths (per worktree CLAUDE.md).
WORKTREE   = Path(__file__).resolve().parent.parent.parent
DEPLOY_DIR = Path("A:/Games/mc2-opengl/mc2-win64-v0.4")
MC2_EXE    = DEPLOY_DIR / "mc2.exe"

# -----------------------------------------------------------------------------
# Capture launch
# -----------------------------------------------------------------------------

def harness_duration_s(frame_n: int) -> int:
    """Wall-clock budget for one capture run.

    With the IsFrameSolidArmed() latch, the run must budget for:
      (1) intro-pan wall-clock, which can hit ~60s on mc2_01;
      (2) frame_n frames AFTER intro complete (at ~60 FPS worst case);
      (3) a small settle buffer for window/GL/mission load.
    """
    # Worst-case observed intro is ~60s on mc2_01; budget 75s to cover it.
    INTRO_BUDGET_S = 75
    return max(75, INTRO_BUDGET_S + int(frame_n / 60) + 10)


def run_capture(mission: str, gpu_objects: bool, out_path: Path,
                frame_n: int) -> tuple[bool, str]:
    """Launch a single mc2.exe with visual-diff env. Serial; never concurrent.

    Returns (ok, stderr_tail). ok=True on event=capture_done; False on
    timeout / pose-missing / capture-failed / nonzero exit.
    """
    out_path.parent.mkdir(parents=True, exist_ok=True)
    if out_path.exists():
        out_path.unlink()

    duration = harness_duration_s(frame_n)
    env = os.environ.copy()
    env["MC2_VISUAL_DIFF_CAPTURE"]  = "1"
    env["MC2_VISUAL_DIFF_MISSION"]  = mission
    env["MC2_VISUAL_DIFF_OUT"]      = str(out_path)
    env["MC2_VISUAL_DIFF_FRAME_N"]  = str(frame_n)
    env["MC2_VISUAL_DIFF_MAX_FRAMES"] = str(frame_n + 200)
    env["MC2_SMOKE_MODE"]           = "1"
    env["MC2_GPU_OBJECTS"]          = "1" if gpu_objects else "0"

    # Cwd matters: mc2.exe loads data/ relative to its install dir.
    # OWNER RULE: background launch only. nofocus.run adds the non-activating
    # STARTUPINFO + windowing env; MC2_VISUAL_DIFF_CAPTURE reads the engine's own
    # framebuffer, so an offscreen window costs nothing. small_window=False keeps
    # the diff at options.cfg resolution.
    proc = nofocus.run(
        [str(MC2_EXE), "--mission", mission, "--duration", str(duration)],
        cwd=str(DEPLOY_DIR),
        env=env,
        capture_output=True,
        text=True,
        timeout=duration + 30,  # wall-clock backstop
        small_window=False,
    )

    # Engine logs via stderr. Tail is useful diagnostic.
    err_tail = "\n".join(proc.stderr.splitlines()[-10:])
    captured = "event=capture_done" in proc.stderr
    if not captured:
        return False, err_tail
    if not out_path.exists():
        return False, f"capture_done logged but no file at {out_path}\n{err_tail}"
    return True, err_tail


# -----------------------------------------------------------------------------
# TGA reader (matches gos::screenshot::writeTGA — uncompressed BGR 24-bit)
# -----------------------------------------------------------------------------

def read_tga_bgr(path: Path) -> tuple[int, int, bytes]:
    """Read a 24-bit uncompressed TGA produced by gos::screenshot::writeTGA.
    Returns (w, h, bgr_bytes). bgr_bytes is row-major, (w*h*3) bytes.
    """
    with path.open("rb") as f:
        header = f.read(18)
        if len(header) < 18:
            raise ValueError(f"{path}: TGA header truncated")
        if header[2] != 2:
            raise ValueError(f"{path}: not uncompressed true-color (image type {header[2]})")
        if header[16] != 24:
            raise ValueError(f"{path}: not 24-bit (got {header[16]} bpp)")
        w = struct.unpack("<H", header[12:14])[0]
        h = struct.unpack("<H", header[14:16])[0]
        body = f.read(w * h * 3)
        if len(body) != w * h * 3:
            raise ValueError(f"{path}: pixel data short ({len(body)} vs {w*h*3})")
    return w, h, body


# -----------------------------------------------------------------------------
# Diff stats
# -----------------------------------------------------------------------------

def compute_stats(path_a: Path, path_b: Path) -> dict:
    """Per-pixel stats over two BGR TGAs of identical dimensions.

    A pixel "differs" iff any of its 3 channels has abs(delta) > LSB_THRESHOLD.

    Returns:
      max_channel_delta:        int, worst single-channel delta across all pixels
      pixels_total:             int, total pixel count
      pixels_exceeding_2_lsb:   int, count of differing pixels
      ratio_exceeding_2_lsb:    float, differing / total
      hot_regions:              list of HOT_GRID*HOT_GRID ints, count per cell
    """
    wa, ha, a = read_tga_bgr(path_a)
    wb, hb, b = read_tga_bgr(path_b)
    if (wa, ha) != (wb, hb):
        raise ValueError(f"size mismatch: {path_a} {wa}x{ha} vs {path_b} {wb}x{hb}")

    w, h = wa, ha
    total_pixels = w * h
    grid = [0] * (HOT_GRID * HOT_GRID)
    cell_w = max(1, w // HOT_GRID)
    cell_h = max(1, h // HOT_GRID)

    # Per-pixel scan. Pure Python is slow but tractable for 4K (8.3M pixels
    # ~ 5-8s on this hardware). If it becomes a bottleneck, swap to numpy.
    max_delta = 0
    differing = 0
    for i in range(total_pixels):
        o = i * 3
        db = abs(a[o]   - b[o])
        dg = abs(a[o+1] - b[o+1])
        dr = abs(a[o+2] - b[o+2])
        m = db
        if dg > m: m = dg
        if dr > m: m = dr
        if m > max_delta: max_delta = m
        if m > LSB_THRESHOLD:
            differing += 1
            # Map pixel index to grid cell.
            x = i % w
            y = i // w
            cx = min(HOT_GRID - 1, x // cell_w)
            cy = min(HOT_GRID - 1, y // cell_h)
            grid[cy * HOT_GRID + cx] += 1

    return {
        "w":                       w,
        "h":                       h,
        "max_channel_delta":       max_delta,
        "pixels_total":            total_pixels,
        "pixels_exceeding_2_lsb":  differing,
        "ratio_exceeding_2_lsb":   differing / total_pixels if total_pixels else 0.0,
        "hot_regions":             grid,
    }


# -----------------------------------------------------------------------------
# Modes
# -----------------------------------------------------------------------------

def fmt_hot(grid: list[int]) -> str:
    """One-line formatter for the 8x8 hot-region grid."""
    cells = [f"{c:>5}" for c in grid]
    rows = [" ".join(cells[r*HOT_GRID:(r+1)*HOT_GRID]) for r in range(HOT_GRID)]
    return "\n        ".join(rows)


def measure_variance(workdir: Path, missions: list[str]) -> int:
    """Run each mission twice at MC2_GPU_OBJECTS=0; report stats. Not a gate."""
    print(f"[VISUAL_DIFF v1] mode=measure-variance missions={len(missions)}")
    print(f"[VISUAL_DIFF v1] tolerance ratio<= {RATIO_THRESHOLD} max_channel_delta<= {LSB_THRESHOLD}")
    any_exceeds = False
    for m in missions:
        if m not in CAPTURE_FRAMES:
            print(f"[FAIL] {m}: no CAPTURE_FRAMES entry")
            return 1
        n = CAPTURE_FRAMES[m]
        a = workdir / f"a_{m}.tga"
        b = workdir / f"b_{m}.tga"

        t0 = time.time()
        ok_a, err_a = run_capture(m, gpu_objects=False, out_path=a, frame_n=n)
        t_a = time.time() - t0
        if not ok_a:
            print(f"[FAIL] {m}: capture A failed in {t_a:.1f}s")
            print(f"        stderr tail: {err_a}")
            continue

        t0 = time.time()
        ok_b, err_b = run_capture(m, gpu_objects=False, out_path=b, frame_n=n)
        t_b = time.time() - t0
        if not ok_b:
            print(f"[FAIL] {m}: capture B failed in {t_b:.1f}s")
            print(f"        stderr tail: {err_b}")
            continue

        s = compute_stats(a, b)
        budget = "OK" if s["ratio_exceeding_2_lsb"] <= RATIO_THRESHOLD else "EXCEEDS"
        if budget != "OK":
            any_exceeds = True
        print(f"[{budget}] {m}: frameN={n} {s['w']}x{s['h']} "
              f"max_delta={s['max_channel_delta']} "
              f"differing={s['pixels_exceeding_2_lsb']}/{s['pixels_total']} "
              f"ratio={s['ratio_exceeding_2_lsb']:.6f} "
              f"(t_a={t_a:.1f}s t_b={t_b:.1f}s)")
        if s["ratio_exceeding_2_lsb"] > RATIO_THRESHOLD * 0.1:
            print(f"        hot_regions ({HOT_GRID}x{HOT_GRID}):\n        {fmt_hot(s['hot_regions'])}")

    print(f"[VISUAL_DIFF v1] mode=measure-variance summary "
          f"any_exceeds={'yes' if any_exceeds else 'no'}")
    return 0


def gate(workdir: Path, missions: list[str]) -> int:
    """Apply spec tolerance per mission; PASS / FAIL gate."""
    print(f"[VISUAL_DIFF v1] mode=gate missions={len(missions)}")
    failed = []
    for m in missions:
        if m not in CAPTURE_FRAMES:
            print(f"[FAIL] {m}: no CAPTURE_FRAMES entry")
            return 1
        n = CAPTURE_FRAMES[m]
        a = workdir / f"a_{m}.tga"
        b = workdir / f"b_{m}.tga"
        ok_a, err_a = run_capture(m, gpu_objects=False, out_path=a, frame_n=n)
        if not ok_a:
            print(f"[FAIL] {m}: capture A failed\n        {err_a}")
            failed.append(m)
            continue
        ok_b, err_b = run_capture(m, gpu_objects=False, out_path=b, frame_n=n)
        if not ok_b:
            print(f"[FAIL] {m}: capture B failed\n        {err_b}")
            failed.append(m)
            continue
        s = compute_stats(a, b)
        ok = s["ratio_exceeding_2_lsb"] <= RATIO_THRESHOLD
        verdict = "PASS" if ok else "FAIL"
        print(f"[{verdict}] {m}: ratio={s['ratio_exceeding_2_lsb']:.6f} "
              f"max_delta={s['max_channel_delta']}")
        if not ok:
            failed.append(m)
            print(f"        hot_regions:\n        {fmt_hot(s['hot_regions'])}")

    if failed:
        print(f"[VISUAL_DIFF v1] gate=FAIL missions={','.join(failed)}")
        return 2
    print("[VISUAL_DIFF v1] gate=PASS")
    return 0


# -----------------------------------------------------------------------------
# CLI
# -----------------------------------------------------------------------------

def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--measure-variance", action="store_true",
                   help="Same-config repeat measurement; not a gate.")
    p.add_argument("--gate", action="store_true",
                   help="Same-config tolerance gate (Phase 2).")
    p.add_argument("--mission", action="append",
                   help="Restrict to specific mission(s); repeatable.")
    p.add_argument("--workdir",
                   help="Output dir for TGAs; defaults to a tempdir.")
    args = p.parse_args()

    if args.measure_variance == args.gate:
        p.error("specify exactly one of --measure-variance or --gate")

    missions = args.mission if args.mission else MISSIONS
    for m in missions:
        if m not in MISSIONS:
            p.error(f"unknown mission {m}; choose from {MISSIONS}")

    workdir = Path(args.workdir) if args.workdir \
              else Path(tempfile.mkdtemp(prefix="visual_diff_"))
    print(f"[VISUAL_DIFF v1] workdir={workdir}")

    if not MC2_EXE.exists():
        print(f"[VISUAL_DIFF v1] event=missing_exe path={MC2_EXE}")
        return 1

    if args.measure_variance:
        return measure_variance(workdir, missions)
    return gate(workdir, missions)


if __name__ == "__main__":
    sys.exit(main())
