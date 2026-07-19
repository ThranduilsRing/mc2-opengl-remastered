#!/usr/bin/env python3
"""tracy_capture_compare.py -- agent-runnable Tracy capture + compare harness for MC2 perf work.

Two subcommands:

  capture  -- record a headless Tracy trace of an MC2 smoke run (GL or VK arm):
      py -3 tools/tracy_capture_compare.py capture \
          --install A:/Games/mc2-opengl/mc2-win64-0.4c --arm gl \
          --mission mc2_01 --seconds 30 -o gl_before.tracy

  compare  -- per-zone before/after delta table from two .tracy files:
      py -3 tools/tracy_capture_compare.py compare before.tracy after.tracy \
          [--zones DrawScreen,SwapWindow.SDL]

Facts baked in (recon 2026-07-18):
  * Tracy tools: tracy-capture.exe (headless recorder: -a HOST -p PORT -s SECONDS -f -o out.tracy)
    and tracy-csvexport.exe (.tracy -> per-zone-stats CSV; -f NAME filters by zone-name
    SUBSTRING). Default location C:/Users/Joe/Downloads/TracyProfiler, override with
    env TRACY_TOOLS_DIR.
  * Every non-ASAN MC2 build compiles with TRACY_ENABLE + TRACY_ON_DEMAND
    (root CMakeLists.txt:139-148): the client buffers nothing until a recorder connects,
    so we start tracy-capture BEFORE launching the game.
  * csvexport CSV columns:
      name,src_file,src_line,total_ns,total_perc,counts,mean_ns,min_ns,max_ns,std_ns
    (note: "counts", not "count"; mean/min/max are integers, std is float).
  * VK arm: MC2_RENDER_BACKEND=vulkan + MC2_VK_WATER_INTEROP=1 + MC2_VK_SHADOW_INTEROP=1
    + MC2_VK_PRESENT_VERIFY=1 (mirrors tracy-vk-capture.bat). The game window MUST stay
    visible and non-minimized during a VK capture -- a minimized window gives the VK
    present path a zero surface extent and it FAIL-SOFTS to GL (you would profile the
    GL present path instead).
  * SCOPE NOTE: the VK islands (water/shadow/terrain interop) have NO Tracy zones.
    Their per-island timings come from [VK_*_PERF] log receipts, parsed by
    .claude/fence-defer/measure.py -- out of scope for this tool. The default zone
    allowlist here covers the GL-side frame: DrawScreen, renderWaterFastPath,
    SwapWindow.SDL, Camera.UpdateRenderers, Terrain::IndirectDraw.

Safety: capture ABORTS if any mc2.exe is already running (the user may be playing).
Only processes spawned by this script are ever killed, by Popen handle.
"""

import argparse
import csv
import io
import os
import subprocess
import sys
import time
from pathlib import Path

DEFAULT_TRACY_DIR = Path("C:/Users/Joe/Downloads/TracyProfiler")
DEFAULT_ZONES = [
    "DrawScreen",
    "renderWaterFastPath",
    "SwapWindow.SDL",
    "Camera.UpdateRenderers",
    "Terrain::IndirectDraw",
]
MIN_TRACE_BYTES = 64 * 1024  # a real 20s+ trace is multi-MB; below this = broken capture

VK_ENV = {
    "MC2_RENDER_BACKEND": "vulkan",
    "MC2_VK_WATER_INTEROP": "1",
    "MC2_VK_SHADOW_INTEROP": "1",
    "MC2_VK_PRESENT_VERIFY": "1",
}


def tracy_dir() -> Path:
    return Path(os.environ.get("TRACY_TOOLS_DIR", str(DEFAULT_TRACY_DIR)))


def tracy_exe(name: str) -> Path:
    exe = tracy_dir() / name
    if not exe.is_file():
        sys.exit(f"ERROR: {exe} not found (set TRACY_TOOLS_DIR to the Tracy tools folder)")
    return exe


def mc2_running() -> bool:
    """True if any mc2.exe process exists. Windows-first (tasklist), psutil-free."""
    try:
        out = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq mc2.exe", "/NH", "/FO", "CSV"],
            capture_output=True, text=True, timeout=15,
        ).stdout
        return "mc2.exe" in out.lower()
    except FileNotFoundError:
        # non-Windows fallback
        out = subprocess.run(["pgrep", "-f", "mc2.exe"], capture_output=True, text=True).stdout
        return bool(out.strip())


# ---------------------------------------------------------------- capture ----

def cmd_capture(args: argparse.Namespace) -> int:
    install = Path(args.install)
    game = install / "mc2.exe"
    if not game.is_file():
        sys.exit(f"ERROR: {game} not found")
    out = Path(args.output).resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    cap_exe = tracy_exe("tracy-capture.exe")

    if mc2_running():
        sys.exit("ABORT: an mc2.exe is already running -- the user may be playing. "
                 "Not launching another instance; nothing was touched.")

    # Recorder first: TRACY_ON_DEMAND means the game buffers nothing until connected.
    cap_cmd = [str(cap_exe), "-a", "127.0.0.1", "-p", "8086",
               "-s", str(args.seconds), "-f", "-o", str(out)]
    cap_log = out.with_suffix(".capture.log")
    cap_lf = open(cap_log, "w", encoding="utf-8", errors="replace")
    recorder = subprocess.Popen(cap_cmd, stdout=cap_lf, stderr=subprocess.STDOUT)
    time.sleep(1.0)  # let it bind the port before the game boots

    env = os.environ.copy()
    # scrub stray backend vars so the arm is exactly what we asked for
    for k in list(env):
        if k.startswith("MC2_VK") or k == "MC2_RENDER_BACKEND":
            env.pop(k)
    env.update({
        "MC2_NO_LAUNCHER": "1",
        "MC2_SMOKE_MODE": "1",
        "MC2_SMOKE_FIXED_TIMESTEP": "1",
    })
    if args.arm == "vk":
        env.update(VK_ENV)
        print("NOTE: VK arm -- keep the game window visible/non-minimized or VK "
              "fail-softs to GL and you profile the wrong present path.")

    duration = args.seconds + 5  # game outlives the recorder window
    game_cmd = [str(game), "--profile", "stock", "--mission", args.mission,
                "--duration", str(duration)]
    game_log = out.with_suffix(".game.log")
    game_lf = open(game_log, "w", encoding="utf-8", errors="replace")
    print(f"[capture] recorder pid={recorder.pid} -> {out}")
    gp = subprocess.Popen(game_cmd, stdout=game_lf, stderr=subprocess.STDOUT,
                          cwd=str(install), env=env)
    print(f"[capture] game pid={gp.pid} arm={args.arm} mission={args.mission} "
          f"duration={duration}s")

    rc = 0
    try:
        gp.wait(timeout=duration + 90)
    except subprocess.TimeoutExpired:
        print("WARN: game exceeded its duration budget -- killing OUR spawned pid "
              f"{gp.pid} only", file=sys.stderr)
        gp.kill()
        rc = 1
    try:
        recorder.wait(timeout=60)
    except subprocess.TimeoutExpired:
        print(f"WARN: tracy-capture still running -- killing OUR spawned pid "
              f"{recorder.pid} only", file=sys.stderr)
        recorder.kill()
        rc = 1
    cap_lf.close()
    game_lf.close()

    if not out.is_file() or out.stat().st_size < MIN_TRACE_BYTES:
        size = out.stat().st_size if out.is_file() else 0
        sys.exit(f"ERROR: trace {out} missing or trivial ({size} bytes < "
                 f"{MIN_TRACE_BYTES}); see {cap_log} / {game_log}")

    print(f"OK capture arm={args.arm} mission={args.mission} seconds={args.seconds} "
          f"-> {out} ({out.stat().st_size / 1e6:.1f} MB)")
    return rc


# ---------------------------------------------------------------- compare ----

def zone_stats(trace: Path, zone: str, export_exe: Path):
    """Return dict(total_ns, counts, mean_ns) for exact zone name, or None.

    csvexport -f is a substring filter, so we still exact-match the name column.
    If a zone name appears in multiple rows (same name at several src lines),
    totals/counts are summed and the mean recomputed.
    """
    p = subprocess.run([str(export_exe), "-f", zone, str(trace)],
                       capture_output=True, text=True, timeout=300)
    if p.returncode != 0:
        print(f"WARN: csvexport failed on {trace} for zone '{zone}': "
              f"{p.stderr.strip()}", file=sys.stderr)
        return None
    total = cnt = 0
    for row in csv.DictReader(io.StringIO(p.stdout)):
        if row.get("name") == zone:
            total += int(row["total_ns"])
            cnt += int(row["counts"])
    if cnt == 0:
        return None
    return {"total_ns": total, "counts": cnt, "mean_ns": total / cnt}


def fmt_delta(before, after):
    if before in (None, 0):
        return "n/a"
    d = (after - before) / before * 100.0
    return f"{d:+.1f}%"


def cmd_compare(args: argparse.Namespace) -> int:
    export_exe = tracy_exe("tracy-csvexport.exe")
    before, after = Path(args.before), Path(args.after)
    for t in (before, after):
        if not t.is_file():
            sys.exit(f"ERROR: trace not found: {t}")
    zones = [z.strip() for z in args.zones.split(",")] if args.zones else DEFAULT_ZONES

    cols = ["zone", "mean_ns B", "mean_ns A", "d_mean", "total_ms B", "total_ms A",
            "d_total", "count B", "count A"]
    rows = []
    warns = []
    for z in zones:
        b = zone_stats(before, z, export_exe)
        a = zone_stats(after, z, export_exe)
        if b is None:
            warns.append(f"WARN: zone '{z}' missing from BEFORE trace {before.name}")
        if a is None:
            warns.append(f"WARN: zone '{z}' missing from AFTER trace {after.name}")
        rows.append([
            z,
            f"{b['mean_ns']:.0f}" if b else "-",
            f"{a['mean_ns']:.0f}" if a else "-",
            fmt_delta(b and b["mean_ns"], a["mean_ns"]) if (b and a) else "-",
            f"{b['total_ns'] / 1e6:.1f}" if b else "-",
            f"{a['total_ns'] / 1e6:.1f}" if a else "-",
            fmt_delta(b and b["total_ns"], a["total_ns"]) if (b and a) else "-",
            str(b["counts"]) if b else "-",
            str(a["counts"]) if a else "-",
        ])

    widths = [max(len(cols[i]), *(len(r[i]) for r in rows)) for i in range(len(cols))]
    def line(vals):
        return "  ".join(v.ljust(widths[i]) if i == 0 else v.rjust(widths[i])
                         for i, v in enumerate(vals))
    print(f"before: {before}")
    print(f"after:  {after}")
    print(line(cols))
    print(line(["-" * w for w in widths]))
    for r in rows:
        print(line(r))
    for w in warns:
        print(w)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("capture", help="record a headless Tracy trace of an MC2 smoke run")
    c.add_argument("--install", required=True, help="MC2 install dir containing mc2.exe")
    c.add_argument("--arm", choices=["gl", "vk"], required=True)
    c.add_argument("--mission", default="mc2_01")
    c.add_argument("--seconds", type=int, default=30, help="Tracy recording window")
    c.add_argument("-o", "--output", required=True, help="output .tracy path")
    c.set_defaults(fn=cmd_capture)

    d = sub.add_parser("compare", help="per-zone before/after delta table")
    d.add_argument("before")
    d.add_argument("after")
    d.add_argument("--zones", help="comma-separated zone names "
                                   f"(default: {','.join(DEFAULT_ZONES)})")
    d.set_defaults(fn=cmd_compare)

    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
