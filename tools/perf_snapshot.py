#!/usr/bin/env python3
"""perf_snapshot.py — VK-OFFLINE-PROOFS-1 one-command perf snapshot.

Launches mc2.exe (PID-tracked, exact-PID cleanup — no --kill-existing, no
process-name nukes), waits N seconds with MC2_DEBUG_STATE_DUMP=1 set, reads
the resulting debug-state dump's per-island timing sections (stage/draw/total
us — see GameOS/gameos/debug_state_dump.cpp "vulkan_terrain_island" /
"vulkan_color_prop_island" / "vulkan_mech_island" sections, commit 33b23c83),
kills the exact PID it launched, and prints ONE line:
    FPS=<estimate> terrain[stage=.. draw=.. total=..] prop[...] mech[...]

FPS is estimated from wall-clock settle time vs the dump's own
"total_us"-implied frame cadence is NOT directly available (no frame counter
in the dump today), so FPS here is a coarse proxy: 1e6 / total_us summed
across islands that reported nonzero timing, OR, if a frame counter field
is later added to the dump, this script should be the first updated to use
it directly (see NOTE below).

Usage:
    py -3 tools/perf_snapshot.py --exe A:/Games/mc2-opengl/mc2-win64-0.4c/mc2.exe \
        --mission mc2_01 --duration 20 --env MC2_VK_SHARED_GBUFFER=1

Exit 0 = snapshot captured and printed. Exit 1 = exe didn't produce a dump in
time, or exited early (stderr tail printed for diagnosis). Never leaves an
orphaned process: the exact PID this script started is killed in a finally
block regardless of outcome.
"""
import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))

from smoke_lib import nofocus  # noqa: E402


def taskkill_exact_pid(pid: int):
    subprocess.run(["taskkill", "/F", "/PID", str(pid)],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def read_dump(dump_dir: Path):
    p = dump_dir / "latest_render_state.json"
    if not p.is_file():
        return None
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return None


def fmt_island(name, d):
    if not d:
        return f"{name}[absent]"
    stage_key = "stage_us" if "stage_us" in d else ("stage_inst_us" if "stage_inst_us" in d else None)
    stage = d.get(stage_key, 0.0) if stage_key else 0.0
    draw = d.get("draw_us", 0.0)
    total = d.get("total_us", 0.0)
    used = d.get("used_vulkan", d.get("water_used_vulkan", 0))
    return f"{name}[used={used} stage={stage:.1f}us draw={draw:.1f}us total={total:.1f}us]"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--mission", default="mc2_01")
    ap.add_argument("--duration", type=int, default=20, help="seconds to let it run before sampling")
    ap.add_argument("--dump-dir", default="debug_state")
    ap.add_argument("--env", action="append", default=[],
                     help="KEY=VALUE, repeatable, set in the launched process env")
    args = ap.parse_args()

    exe = Path(args.exe)
    if not exe.is_file():
        print(f"perf_snapshot: exe not found: {exe}", file=sys.stderr)
        sys.exit(1)

    dump_dir = Path(args.dump_dir)
    dump_dir.mkdir(parents=True, exist_ok=True)
    stale = dump_dir / "latest_render_state.json"
    if stale.is_file():
        stale.unlink()  # avoid reading a stale prior-run snapshot

    env = os.environ.copy()
    env["MC2_DEBUG_STATE_DUMP"] = "1"
    env["MC2_DEBUG_STATE_DUMP_DIR"] = str(dump_dir.resolve())
    env["MC2_NO_LAUNCHER"] = "1"
    for kv in args.env:
        if "=" not in kv:
            print(f"perf_snapshot: bad --env {kv!r}, expected KEY=VALUE", file=sys.stderr)
            sys.exit(2)
        k, v = kv.split("=", 1)
        env[k] = v

    exe_dir = str(exe.resolve().parent)
    # show_window=True: this is a PERF measurement. SW_SHOWMINNOACTIVE would
    # leave the window minimized and SDL throttles a minimized window, so the
    # numbers would not be GPU throughput. SW_SHOWNOACTIVATE still NEVER
    # activates the window or takes focus.
    proc = nofocus.spawn(
        [str(exe), "-mission", args.mission] if args.mission else [str(exe)],
        show_window=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1,
        cwd=exe_dir, env=env,
    )
    pid = proc.pid
    print(f"perf_snapshot: launched pid={pid} exe={exe}", file=sys.stderr)

    try:
        deadline = time.monotonic() + args.duration
        state = None
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                out = proc.stdout.read() if proc.stdout else ""
                print(f"perf_snapshot: process exited early (code={proc.returncode})", file=sys.stderr)
                print(out[-4000:], file=sys.stderr)
                sys.exit(1)
            time.sleep(1.0)
        state = read_dump(dump_dir)
        if state is None:
            print("perf_snapshot: no debug-state dump produced within duration", file=sys.stderr)
            sys.exit(1)

        terrain = state.get("vulkan_terrain_island")
        prop = state.get("vulkan_color_prop_island")
        mech = state.get("vulkan_mech_island")

        # FPS proxy: NOTE dump has no frame counter today (see docstring). Report
        # island timing as the authoritative signal; FPS is a coarse total_us-based
        # estimate only when at least one island reports nonzero total_us.
        totals = [d.get("total_us", 0.0) for d in (terrain, prop, mech) if d and d.get("total_us", 0.0) > 0]
        fps = f"{1e6 / max(totals):.1f}(proxy,islands-only)" if totals else "n/a(no-island-timing)"

        line = (f"FPS={fps} " + " ".join([
            fmt_island("terrain", terrain),
            fmt_island("prop", prop),
            fmt_island("mech", mech),
        ]))
        print(line)
        sys.exit(0)
    finally:
        if proc.poll() is None:
            taskkill_exact_pid(pid)
            print(f"perf_snapshot: killed pid={pid}", file=sys.stderr)


if __name__ == "__main__":
    main()
