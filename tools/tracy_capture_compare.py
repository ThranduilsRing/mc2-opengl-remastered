#!/usr/bin/env python3
"""tracy_capture_compare.py -- canonical Tracy capture/compare/baseline harness for MC2 perf work.

Subcommands:

  capture  -- record a headless Tracy trace of an MC2 smoke run (the VK arm):
      py -3 tools/tracy_capture_compare.py capture \
          --install A:/Games/mc2-opengl/mc2-win64-0.4c --arm vk \
          --mission mc2_01 --seconds 30 -o vk_before.tracy

  compare  -- per-zone before/after delta table from two .tracy files:
      py -3 tools/tracy_capture_compare.py compare before.tracy after.tracy \
          [--zones DrawScreen,SwapWindow.SDL]

  baseline -- capture the vk arm at a named pose and write a dated
              baseline dir (traces + baseline.json + summary.md). Uncapped,
              free-timestep (the only hygienic FPS measurement -- see below):
      py -3 tools/tracy_capture_compare.py baseline \
          --install A:/Games/mc2-opengl/mc2-win64-0.4c [--name 2026-07-23]

  regress  -- re-capture the baseline's arms and compare against a named baseline dir,
              with pass/warn/fail thresholds on frame-count drop + per-zone
              mean deltas (exit 0=pass, 1=warn, 2=fail):
      py -3 tools/tracy_capture_compare.py regress \
          --baseline release_assets/tracy-baselines/2026-07-23 \
          --install A:/Games/mc2-opengl/mc2-win64-0.4c

Facts baked in (recon 2026-07-18):
  * Tracy tools: tracy-capture.exe (headless recorder: -a HOST -p PORT -s SECONDS -f -o out.tracy)
    and tracy-csvexport.exe (.tracy -> per-zone-stats CSV; -f NAME filters by zone-name
    SUBSTRING). Default location C:/Users/Joe/Downloads/TracyProfiler, override with
    env TRACY_TOOLS_DIR. The two exes are NOT committed to the repo (large third-party
    binaries) -- see docs/tracy-harness.md for the download source.
  * Every non-ASAN MC2 build compiles with TRACY_ENABLE + TRACY_ON_DEMAND
    (root CMakeLists.txt:139-148): the client buffers nothing until a recorder connects,
    so we start tracy-capture BEFORE launching the game.
  * csvexport CSV columns:
      name,src_file,src_line,total_ns,total_perc,counts,mean_ns,min_ns,max_ns,std_ns
    (note: "counts", not "count"; mean/min/max are integers, std is float).
  * VK arm: MC2_RENDER_BACKEND=vulkan + MC2_VK_SHARED_GBUFFER=1, shadow interop OFF
    (see VK_ENV note). PRESENT_VERIFY deliberately NOT set: it is a diagnostic readback
    that taxes the present path (~25% tax) and skews perf numbers. The game window MUST
    stay visible and non-minimized during a VK capture -- a minimized window gives the VK
    present path a zero surface extent and it FAIL-SOFTS to GL (you would profile the
    GL present path instead).
  * SCOPE NOTE: the VK islands (water/shadow/terrain interop) have NO Tracy zones.
    Their per-island timings come from [VK_*_PERF] log receipts, parsed by
    .claude/fence-defer/measure.py -- out of scope for this tool. The default zone
    allowlist covers the CPU frame zones the VK frame still runs: DrawScreen,
    renderWaterFastPath, SwapWindow.SDL, Camera.UpdateRenderers, Terrain::IndirectDraw.

MEASUREMENT HYGIENE (baseline/regress enforce this; capture opts in via flags):
  * NEVER fixed-timestep for FPS/frame-count comparison -- fixed timestep decouples
    frame count from wall-clock render cost. baseline/regress run free-timestep.
  * NEVER MC2_VK_PRESENT_VERIFY=1 -- ~25% present-path tax.
  * Uncapped = MC2_FPS_CAP=0. baseline/regress set it so the frame rate is
    render-bound, not vsync/limiter-bound.
  * Both arms are pinned to the same camera pose (MC2_CAMERA_PARK) so the two
    traces measure the same scene, and a regress re-run measures the same scene
    as its baseline.

Safety: capture waits for the host lease before starting the recorder or game.
Only processes spawned by this script are ever killed, by Popen handle.
"""

import argparse
import csv
import io
import json
import os
import subprocess
import sys
import time
from contextlib import ExitStack
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
# OWNER RULE: the engine is launched through nofocus.spawn ONLY (background launch).
from smoke_lib import host_lease, nofocus  # noqa: E402
from smoke_lib import render_options_pin, render_resolution  # noqa: E402
from smoke_lib import via_menu  # noqa: E402  (--via-menu: the player's boot path)
import perf_baseline_report  # noqa: E402  (GPU memory of a trace: the VRAM plot and the [VRAM v1] log lines)

DEFAULT_TRACY_DIR = Path("C:/Users/Joe/Downloads/TracyProfiler")
DEFAULT_ZONES = [
    "DrawScreen",
    "renderWaterFastPath",
    "SwapWindow.SDL",
    "Camera.UpdateRenderers",
    "Terrain::IndirectDraw",
]
# The top-level per-frame zone (gameosmain.cpp:2183, ZoneScopedN("DrawScreen"))
# present in BOTH backends -- its count over a fixed recording window is the
# frame-count / FPS proxy used by baseline+regress.
FRAME_ZONE = "DrawScreen"
MIN_TRACE_BYTES = 64 * 1024  # a real 15s+ trace is multi-MB; below this = broken capture
# Below this frame count the arm almost certainly never reached steady-state
# render inside the recording window -- cold-cache mission load (the FIRST arm on
# a heavy map like mc2_10 pays the load cost), an early crash, or a VK fail-soft
# to GL. Such an arm's fps is meaningless; flag it loudly, never trust it.
MIN_STEADY_FRAMES = 60

# The mc2_10 corner-park pose (MC2_CAMERA_PARK = x,y,rot,proj,alt[@frame]); park at
# frame 600 so the scene is settled before the measurement window. This is the
# canonical release/regression pose (a busy corner of the largest map).
DEFAULT_POSE = "-6656,-6656,150.625,10.9094,6045.2358@600"
DEFAULT_BASELINE_MISSION = "mc2_10"
DEFAULT_BASELINE_SECONDS = 15  # 15s runs (standing launch rule); ~9s of parked frames
DEFAULT_ARMS = ["vk"]
# The only arm: perf is measured on the shipped backend, and the selfcheck proves
# the harness on VK; a request for any other arm is refused.
ARMS = ("vk",)
BASELINE_ROOT = Path("release_assets/tracy-baselines")  # gitignored; traces never committed
REGRESS_WARN_PCT = 5.0   # frame-count drop that WARNs (exit 1)
REGRESS_FAIL_PCT = 10.0  # frame-count drop that FAILs (exit 2)

# The vk arm measures the shipped VK frame: backend + shared GBuffer, every umbrella member
# at its default. Per-capture overrides go through --env (e.g. --env MC2_VK_SHARED_GBUFFER=0).
VK_ENV = {
    "MC2_RENDER_BACKEND": "vulkan",
    "MC2_VK_SHARED_GBUFFER": "1",
}


def tracy_dir() -> Path:
    return Path(os.environ.get("TRACY_TOOLS_DIR", str(DEFAULT_TRACY_DIR)))


def tracy_exe(name: str) -> Path:
    exe = tracy_dir() / name
    if not exe.is_file():
        sys.exit(f"ERROR: {exe} not found (set TRACY_TOOLS_DIR to the Tracy tools folder; "
                 f"see docs/tracy-harness.md for the download source)")
    return exe


def git_head() -> str:
    """Short HEAD sha of the repo this script lives in (provenance stamp)."""
    try:
        p = subprocess.run(["git", "rev-parse", "--short", "HEAD"],
                           cwd=str(Path(__file__).resolve().parent.parent),
                           capture_output=True, text=True, timeout=15,
                           **nofocus.hidden_kwargs())
        return p.stdout.strip() if p.returncode == 0 else "unknown"
    except Exception:
        return "unknown"


# ---------------------------------------------------------------- capture ----

def _stop_capture_child(child: subprocess.Popen) -> None:
    """Reap our child before releasing the lease, including interrupted or failed captures."""
    if child.poll() is None:
        child.kill()
    child.wait()


def options_sidecar(trace: Path) -> Path:
    """Where capture_trace records the render options of a trace."""
    return Path(trace).with_suffix(".options.json")


def require_same_options(trace_a: Path, trace_b: Path, vary, a_name: str = "A", b_name: str = "B") -> None:
    """Exit unless both traces recorded their render options and they agree on every key not in `vary` (a mismatch is a hard failure, like the
    resolution)."""
    try:
        render_options_pin.require_same(render_options_pin.read(options_sidecar(trace_a)), render_options_pin.read(options_sidecar(trace_b)),
                                        vary, a_name, b_name)
    except render_options_pin.OptionsMismatch as e:
        sys.exit(f"REFUSED: {e}")


def vary_list(text) -> "list[str]":
    return [k.strip() for k in (text or "").split(",") if k.strip()]


def game_env(arm: str, fixed_timestep: bool, fps_cap, pose, extra_env, via_menu_boot: bool = False) -> dict:
    """The environment the game runs under for a trace: the shell's, minus stray backend variables, plus the harness's settings, the pinned
    drawable size, the arm, and the caller's --env entries (which win). A function of its inputs so the pin is testable without an engine."""
    if arm not in ARMS:
        sys.exit(f"ERROR: arm '{arm}' refused: the only measured arm is {', '.join(ARMS)} (the GL arm was retired)")
    env = os.environ.copy()
    # scrub stray backend vars so the arm is exactly what we asked for
    for k in list(env):
        if k.startswith("MC2_VK") or k == "MC2_RENDER_BACKEND":
            env.pop(k)
    env.update({
        "TRACY_PORT": "8086",  # explicit opt-in to the loopback profiling listener
        "MC2_NO_LAUNCHER": "1",
        "MC2_SMOKE_MODE": "1",
        # Never fullscreen / never grab the pointer.
        # setdefault-equivalent is not used here because this dict IS the
        # harness intent; a caller overriding via --env still wins downstream.
        "MC2_WINDOWED": "1",
        "MC2_NO_CURSOR_GRAB": "1",
        # Never occupy visible desktop area: this harness hand-rolls its own
        # STARTUPINFO and Popen (no nofocus), and its argv name `game_cmd` is
        # not in check-background-launch's ENGINE_ARGV heuristic, so it does
        # not appear in the launch inventory. Combined with SW_SHOWNOACTIVATE
        # below, that would put a
        # genuinely VISIBLE, options.cfg-sized window on the user's desktop for
        # the entire capture.
        #
        # Safe for the measurement, which is the whole point of this file:
        # gameosmain.cpp's 10ms background throttle keys on
        # `SDL_GetWindowFlags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)`, and
        # an offscreen-but-shown window sets NEITHER -- so
        # Frame.BackgroundThrottle still never fires and
        # assert_window_was_visible() still passes. Offscreen is a POSITION, not
        # a visibility state. Same reason the VK arm is unaffected: the
        # swapchain surface extent comes from the drawable, which keeps its full
        # size, so the VK present path does not fail-soft to GL.
        "MC2_WINDOW_OFFSCREEN": "1",
        # Without this the engine sends stdout to NUL and
        # the .game.log is zero bytes -- no pose/gate/mission receipt survives,
        # so nothing about the run can be verified afterwards.
        "MC2_LOG": "1",
    })
    # Every trace is taken at the pinned drawable size, not whatever options.cfg says
    # (smoke_lib/render_resolution.py carries the reason); --env MC2_WIDTH=... --env MC2_HEIGHT=... overrides it (one side alone is refused).
    try:
        render_resolution.pin_unless_declared(env, dict(kv.partition("=")[::2] for kv in (extra_env or [])))
    except render_resolution.HalfDeclaredSize as e:
        sys.exit(f"ERROR: {e}")
    if fixed_timestep:
        env["MC2_SMOKE_FIXED_TIMESTEP"] = "1"
    if fps_cap is not None:
        env["MC2_FPS_CAP"] = str(fps_cap)
    if pose:
        env["MC2_CAMERA_PARK"] = pose
    if arm == "vk":
        env.update(VK_ENV)
        print("NOTE: VK arm -- keep the game window visible/non-minimized or VK "
              "fail-softs to GL and you profile the wrong present path.")
    if via_menu_boot:
        # The player's path: no smoke mode (so no fixed timestep and no --mission), the main menu starts the stock campaign itself.
        via_menu.apply_via_menu_env(env)
    for kv in (extra_env or []):
        k, sep, v = kv.partition("=")
        if not sep:
            sys.exit(f"ERROR: --env expects KEY=VAL, got '{kv}'")
        env[k] = v
        print(f"[capture] extra env {k}={v}")
    return env


def capture_trace(install: Path, arm: str, mission: str, seconds: int, out: Path,
                  extra_env=None, pose=None, fixed_timestep=True, fps_cap=None,
                  show_window: bool = True, ticks: int = 0, via_menu_boot: bool = False) -> int:
    """Record one trace under the host lease; retain a caller's existing lease.

    Free timestep and an uncapped FPS limit measure renderer throughput.
    A visible, non-activating window avoids the engine's background throttle.
    MC2_LOG preserves mission and option receipts. Return nonzero on timeout.
    """
    game = install / "mc2.exe"
    if not game.is_file():
        sys.exit(f"ERROR: {game} not found")
    out = Path(out).resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    cap_exe = tracy_exe("tracy-capture.exe")

    if via_menu_boot and ticks:
        sys.exit("ERROR: --via-menu cannot be combined with --ticks (the tick budget is a smoke-mode exit)")
    env = game_env(arm, fixed_timestep, fps_cap, pose, extra_env, via_menu_boot)   # before the recorder starts: a refused --env must leave nothing running

    with ExitStack() as cleanup:
        # The recorder must share the game's host lease so another lane cannot feed it a trace.
        if not host_lease.holds():
            host_lease.acquire(f"Tracy capture {arm} {mission}")
            cleanup.callback(host_lease.release)

        # Recorder first: TRACY_ON_DEMAND means the game buffers nothing until connected.
        cap_cmd = [str(cap_exe), "-a", "127.0.0.1", "-p", "8086",
                   *([] if ticks else ["-s", str(seconds)]), "-f", "-o", str(out)]
        cap_log = out.with_suffix(".capture.log")
        cap_lf = cleanup.enter_context(open(cap_log, "w", encoding="utf-8", errors="replace"))
        # background-launch: allow Tracy recorder console tool, windowless via nofocus.hidden_kwargs.
        recorder = subprocess.Popen(cap_cmd, stdout=cap_lf, stderr=subprocess.STDOUT,
                                    **nofocus.hidden_kwargs())  # tracy-capture is a console tool
        cleanup.callback(_stop_capture_child, recorder)
        time.sleep(1.0)  # let it bind the port before the game boots

        # Render options belong to the trace so comparisons can refuse mismatched inputs.
        options_rec = render_options_pin.record(install, env)
        render_options_pin.write(options_sidecar(out), options_rec)
        for rj in options_rec["rejected"]:
            print(f"WARN: render option rejected by the engine's grammar: {rj}", file=sys.stderr)

        duration = seconds + 5  # game outlives the recorder window
        game_cmd = [str(game), "--profile", "stock", "--mission", mission,
                    "--duration", str(duration)]
        if ticks:
            game_cmd += ["--headless", "--ticks", str(ticks)]
        if via_menu_boot:
            game_cmd = via_menu.game_argv(game)   # no --mission/--duration: the recorder window (--seconds, which must cover menu + load) ends the run
        game_log = out.with_suffix(".game.log")
        game_lf = cleanup.enter_context(open(game_log, "w", encoding="utf-8", errors="replace"))

        # A minimized window throttles the game, so only deliberate CPU-only captures use it.
        if not show_window:
            print("WARN: --no-show-window: the game runs MINIMIZED, the loop takes "
                  "the 10ms/frame background throttle, and FRAGMENT-BOUND COSTS "
                  "ARE INVISIBLE. Valid only for CPU-zone work you have argued is "
                  "unaffected. The post-capture visibility assert will refuse to "
                  "grade this trace.", file=sys.stderr)

        print(f"[capture] recorder pid={recorder.pid} -> {out}")
        # The pinned drawable size must survive the background launch for comparable measurements.
        gp = nofocus.spawn(game_cmd, stdout=game_lf, stderr=subprocess.STDOUT,
                           cwd=str(install), env=env,
                           show_window=show_window, small_window=False)
        cleanup.callback(_stop_capture_child, gp)
        print(f"[capture] game pid={gp.pid} arm={arm} mission={mission} "
              f"duration={duration}s fixed_ts={fixed_timestep} fps_cap={fps_cap} pose={pose}")

        rc = 0
        if via_menu_boot:
            try:
                recorder.wait(timeout=seconds + 90)
            except subprocess.TimeoutExpired:
                rc = 1
            _stop_capture_child(gp)   # the game has no auto-quit outside smoke mode: end OUR spawned pid once the recording is over
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

        if ticks:
            log_text = game_log.read_text(encoding="utf-8", errors="replace")
            if f"[SMOKE v1] event=tick_budget ticks={ticks}" not in log_text or "Crash Report" in log_text:
                print(f"ERROR: capture did not complete {ticks} sim ticks; see {game_log}", file=sys.stderr)
                rc = 1

        if not out.is_file() or out.stat().st_size < MIN_TRACE_BYTES:
            size = out.stat().st_size if out.is_file() else 0
            sys.exit(f"ERROR: trace {out} missing or trivial ({size} bytes < "
                     f"{MIN_TRACE_BYTES}); see {cap_log} / {game_log}")

        print(f"OK capture arm={arm} mission={mission} seconds={seconds} "
              f"-> {out} ({out.stat().st_size / 1e6:.1f} MB)")
        return rc


def zone_summary(trace: Path, top: int = 10, csv_out: Path | None = None) -> None:
    """Print the top-N zones by total time; write the full csvexport table to csv_out only when asked (--csv)."""
    exe = tracy_dir() / "tracy-csvexport.exe"
    if not exe.is_file():
        print(f"WARN: {exe} not found; no zone summary", file=sys.stderr)
        return
    # background-launch: allow Tracy csvexport console tool, not the game; windowless via hidden_kwargs
    p = subprocess.run([str(exe), str(trace)], capture_output=True, text=True, timeout=300, **nofocus.hidden_kwargs())
    if p.returncode != 0:
        print(f"WARN: csvexport failed on {trace}: {p.stderr.strip()}", file=sys.stderr)
        return
    if csv_out is not None:
        csv_out.write_text(p.stdout, encoding="utf-8")
        print(f"csv: {csv_out}")
    rows = sorted(csv.DictReader(io.StringIO(p.stdout)), key=lambda r: -int(r["total_ns"]))[:top]
    for r in rows:
        print(f"  {int(r['total_ns']) / 1e6:10.2f} ms  x{r['counts']:>8}  {r['name']}")


def cmd_capture(args: argparse.Namespace) -> int:
    rc = capture_trace(
        Path(args.install), args.arm, args.mission, args.seconds,
        Path(args.output), extra_env=args.env, pose=args.pose,
        fixed_timestep=not args.free_timestep, fps_cap=args.fps_cap,
        show_window=args.show_window, ticks=args.ticks, via_menu_boot=args.via_menu,
    )
    if rc == 0:
        out = Path(args.output)
        zone_summary(out, csv_out=out.with_suffix(".zones.csv") if args.csv else None)
    return rc


# ---------------------------------------------------------------- compare ----

def zone_stats(trace: Path, zone: str, export_exe: Path):
    """Return dict(total_ns, counts, mean_ns) for exact zone name, or None.

    csvexport -f is a substring filter, so we still exact-match the name column.
    If a zone name appears in multiple rows (same name at several src lines),
    totals/counts are summed and the mean recomputed.
    """
    # background-launch: allow Tracy csvexport console tool, not the game; windowless via hidden_kwargs
    p = subprocess.run([str(export_exe), "-f", zone, str(trace)],
                       capture_output=True, text=True, timeout=300,
                       **nofocus.hidden_kwargs())
    if p.returncode != 0:
        print(f"WARN: csvexport failed on {trace} for zone '{zone}': "
              f"{p.stderr.strip()}", file=sys.stderr)
        return None
    total = cnt = 0
    min_ns = None
    for row in csv.DictReader(io.StringIO(p.stdout)):
        if row.get("name") == zone:
            min_ns = min(int(row["min_ns"]), min_ns) if min_ns is not None and row.get("min_ns") else (int(row["min_ns"]) if row.get("min_ns") else min_ns)
            total += int(row["total_ns"])
            cnt += int(row["counts"])
    if cnt == 0:
        return None
    return {"total_ns": total, "counts": cnt, "mean_ns": total / cnt, "min_ns": min_ns}  # min = best uncontended estimate; csv has no median


# ------------------------------------------------- Log capture guard --------

# The engine's background-throttle zone (gameosmain.cpp). Its PRESENCE in a
# trace is positive proof the window was minimized/hidden for at least one
# frame, i.e. that the loop was sleeping instead of rendering.
THROTTLE_ZONE = "Frame.BackgroundThrottle"


def zone_samples(trace: Path, zone: str, export_exe: Path):
    """Per-event durations (ns) for one zone, via csvexport --unwrap.

    `-u` reports every individual zone event instead of aggregate stats, which
    is what makes real percentiles possible. The aggregate CSV only carries
    mean/min/max/std -- a p99 cannot be recovered from those, which is why the
    owed p50/p99 table was never produced before.
    """
    # background-launch: allow Tracy csvexport console tool, not the game; windowless via hidden_kwargs
    p = subprocess.run([str(export_exe), "-f", zone, "-u", str(trace)],
                       capture_output=True, text=True, timeout=600,
                       **nofocus.hidden_kwargs())
    if p.returncode != 0:
        return []
    rows = list(csv.DictReader(io.StringIO(p.stdout)))
    if not rows:
        return []
    # Column name for the per-event duration differs across Tracy builds
    # (ns / duration_ns / exec_time_ns). Pick the first integer-ish one that
    # is not a timestamp/line number.
    cand = [c for c in rows[0]
            if c and c.lower() in ("ns", "duration_ns", "exec_time_ns", "duration")]
    if not cand:
        cand = [c for c in rows[0] if c and c.lower().endswith("_ns")
                and "start" not in c.lower() and "time" != c.lower()]
    if not cand:
        return []
    col = cand[0]
    out = []
    for r in rows:
        if r.get("name") != zone:
            continue
        try:
            out.append(int(float(r[col])))
        except (TypeError, ValueError):
            continue
    return out


def frame_intervals_ms(trace: Path, export_exe: Path, zone: str = FRAME_ZONE,
                       warmup_frac: float = 0.4, hitch_ms: float = 200.0):
    """TRUE frame times: wall-clock deltas between consecutive frame starts.

    THE central correction of PERF-HARNESS-TRUTH-1. The old harness reported the
    *CPU duration* of the DrawScreen zone (`exec_time_ns`). GL is asynchronous:
    the CPU records its commands and returns long before the GPU has shaded a
    single fragment, so a fragment-bound cost -- light-pool loops, fill rate,
    post -- does NOT appear in that number. That is why raising the light-pool
    cap 4x measured as +0.2%: the CPU-side submission genuinely did not change.

    Frame-to-frame wall time DOES capture it, because the driver eventually
    blocks on the queued GPU work. Measured this way the same two arms separate
    by ~29%, consistent with the reported regression.

    `ns_since_start` (from csvexport --unwrap) is the zone's start timestamp;
    consecutive deltas are the real frame period.

    warmup_frac drops the leading fraction of frames -- mission load and the
    settle to the parked pose are not steady-state and would poison p99.
    hitch_ms discards outliers above a threshold (load hitches / alt-tab).
    """
    # background-launch: allow Tracy csvexport console tool, not the game; windowless via hidden_kwargs
    p = subprocess.run([str(export_exe), "-f", zone, "-u", str(trace)],
                       capture_output=True, text=True, timeout=600,
                       **nofocus.hidden_kwargs())
    if p.returncode != 0:
        return []
    ts = sorted(int(r["ns_since_start"])
                for r in csv.DictReader(io.StringIO(p.stdout))
                if r.get("name") == zone and r.get("ns_since_start"))
    if len(ts) < 10:
        return []
    ts = ts[int(len(ts) * warmup_frac):]
    d = [(b - a) / 1e6 for a, b in zip(ts, ts[1:])]
    return [x for x in d if x < hitch_ms]


def pct(samples, q: float):
    """Nearest-rank percentile of a list of numbers (q in 0..100)."""
    if not samples:
        return None
    s = sorted(samples)
    k = max(0, min(len(s) - 1, int(round(q / 100.0 * (len(s) - 1)))))
    return s[k]


def assert_window_was_visible(trace: Path, export_exe: Path, context: str = "") -> None:
    """HARD REFUSAL if the capture ran against a minimized/hidden window.

    This is the ratchet for the defect that voided LIGHT-CLUSTER-PERF-1's
    frame-time table: a throttled loop sleeps 10ms/frame, so every arm converges
    to the same meaningless number and a real ~34% fragment regression measures
    as 0%. We refuse to report rather than emit numbers that look fine.
    """
    thr = zone_stats(trace, THROTTLE_ZONE, export_exe)
    if thr and thr["counts"] > 0:
        sys.exit(
            f"REFUSING TO REPORT{(' (' + context + ')') if context else ''}: trace "
            f"{trace.name} contains {thr['counts']} '{THROTTLE_ZONE}' zones -- the "
            f"game window was MINIMIZED or HIDDEN, so the loop slept "
            f"{thr['mean_ns'] / 1e6:.1f}ms/frame instead of rendering. "
            f"Fragment-bound costs are INVISIBLE in this configuration and any "
            f"frame-time number derived from it is nonsense.\n"
            f"  Fix: run with --show-window (the default), do not minimize the "
            f"game window during the capture, and keep the desktop unlocked."
        )


def assert_receipt(game_log: Path, needle: str, what: str) -> str:
    """Confirm an expected engine receipt line is present in the game log."""
    try:
        txt = game_log.read_text(encoding="utf-8", errors="replace")
    except OSError:
        sys.exit(f"REFUSING TO REPORT: game log {game_log} unreadable -- cannot "
                 f"verify {what} applied.")
    if game_log.stat().st_size == 0:
        sys.exit(f"REFUSING TO REPORT: game log {game_log} is ZERO BYTES -- "
                 f"MC2_LOG was not honoured, so nothing about this run "
                 f"(pose, gates, mission) can be verified.")
    for ln in txt.splitlines():
        if needle in ln:
            return ln.strip()
    sys.exit(f"REFUSING TO REPORT: no '{needle}' receipt in {game_log.name} -- "
             f"{what} did NOT take effect. The run measured something else.")


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
    require_same_options(before, after, vary_list(args.vary), "BEFORE", "AFTER")

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


# --------------------------------------------------------------- baseline ----

def measure_arm(trace: Path, zones, seconds: int, export_exe: Path) -> dict:
    """Pull frame count + per-zone stats out of one trace into a JSON-able dict.

    PERF-HARNESS-TRUTH-1: refuses outright if the trace proves the window was
    minimized/hidden -- baseline and regress both funnel through here, so no
    path in this tool can report throttled numbers.
    """
    assert_window_was_visible(trace, export_exe, context=trace.stem)
    frame = zone_stats(trace, FRAME_ZONE, export_exe)
    frames = frame["counts"] if frame else 0
    zstats = {}
    for z in zones:
        s = zone_stats(trace, z, export_exe)
        if s:
            zstats[z] = {"mean_ns": round(s["mean_ns"], 1),
                         "total_ns": s["total_ns"], "counts": s["counts"]}
    return {
        "trace": trace.name,
        "frames": frames,
        "fps": round(frames / seconds, 2) if seconds else 0.0,
        "suspect": frames < MIN_STEADY_FRAMES,
        "zones": zstats,
    }


def write_baseline_summary(md_path: Path, meta: dict) -> None:
    zones = meta["zones"]
    arms = meta["arms"]
    lines = [
        f"# Tracy baseline — {meta['name']}",
        "",
        f"- captured (UTC): {meta['date_utc']}",
        f"- repo HEAD: `{meta['head']}`",
        f"- mission: `{meta['mission']}`",
        f"- pose (MC2_CAMERA_PARK): `{meta['pose']}`",
        f"- recording window: {meta['seconds']}s — uncapped (MC2_FPS_CAP=0), free-timestep",
        f"- install: `{meta['install']}`",
        f"- frame zone (FPS proxy): `{FRAME_ZONE}`",
        "",
        "## Frame rate",
        "",
        "| arm | frames | fps | trace |",
        "|---|--:|--:|---|",
    ]
    for arm in meta["arm_order"]:
        a = arms.get(arm)
        if not a:
            continue
        flag = " ⚠ SUSPECT" if a.get("suspect") else ""
        lines.append(f"| {arm} | {a['frames']}{flag} | {a['fps']:.2f} | `{a['trace']}` |")
    if any(arms.get(arm, {}).get("suspect") for arm in meta["arm_order"]):
        lines += ["",
                  f"> ⚠ **SUSPECT arm(s)**: frame count < {MIN_STEADY_FRAMES} means the arm never "
                  "reached steady-state render inside the window (cold-cache mission load on the "
                  "FIRST arm of a heavy map, an early crash, or a VK fail-soft to GL). Re-run to "
                  "capture against a now-warm cache, or raise `--seconds`. Do NOT trust a suspect "
                  "arm's fps."]
    vram_rows = []
    for arm in meta["arm_order"]:
        v = (arms.get(arm) or {}).get("vram") or {}
        plot = v.get("plot")
        logged = "; ".join(f"{s['phase']} {s['backend']}" for s in v.get("log") or []) or "-"
        vram_rows.append(f"| {arm} | " + (f"{plot['median_mb']:.0f} / {plot['max_mb']:.0f} / {plot['last_mb']:.0f}" if plot else "no plot") + f" | {logged} |")
    lines += ["", "## GPU memory (process dedicated video memory, MiB)", "",
              "| arm | plot median / max / last | `[VRAM v1]` samples in the game log |", "|---|---|---|"] + vram_rows
    lines += ["", "## Per-zone mean (ns)", "",
              "| zone | " + " | ".join(meta["arm_order"]) + " |",
              "|---|" + "|".join("--:" for _ in meta["arm_order"]) + "|"]
    for z in zones:
        cells = []
        for arm in meta["arm_order"]:
            a = arms.get(arm, {})
            zs = a.get("zones", {}).get(z)
            cells.append(f"{zs['mean_ns']:.0f}" if zs else "-")
        lines.append(f"| `{z}` | " + " | ".join(cells) + " |")
    lines += ["",
              "_Regenerate/compare with:_ "
              f"`py -3 tools/tracy_capture_compare.py regress --baseline {md_path.parent.as_posix()} "
              "--install <mc2 install>`", ""]
    md_path.write_text("\n".join(lines), encoding="utf-8")


def cmd_baseline(args: argparse.Namespace) -> int:
    export_exe = tracy_exe("tracy-csvexport.exe")
    install = Path(args.install)
    zones = [z.strip() for z in args.zones.split(",")] if args.zones else DEFAULT_ZONES
    arms = [a.strip() for a in args.arms.split(",")] if args.arms else DEFAULT_ARMS
    name = args.name or datetime.now(timezone.utc).strftime("%Y-%m-%d")
    out_dir = Path(args.out_dir) if args.out_dir else (BASELINE_ROOT / name)
    out_dir.mkdir(parents=True, exist_ok=True)

    print(f"[baseline] name={name} arms={arms} mission={args.mission} "
          f"seconds={args.seconds} -> {out_dir}")
    arm_results = {}
    rc = 0
    for arm in arms:
        trace = out_dir / f"{name}-{arm}.tracy"
        r = capture_trace(install, arm, args.mission, args.seconds, trace,
                          pose=args.pose, fixed_timestep=False, fps_cap=0)
        rc = rc or r
        arm_results[arm] = measure_arm(trace, zones, args.seconds, export_exe)
        arm_results[arm]["options"] = (render_options_pin.read(options_sidecar(trace)) or {}).get("values")
        # GPU memory next to the frame rate: a baseline taken at a different working set is not the same baseline.
        arm_results[arm]["vram"] = perf_baseline_report.vram_record(trace)

    meta = {
        "name": name,
        "date_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "head": git_head(),
        "mission": args.mission,
        "pose": args.pose,
        "seconds": args.seconds,
        "install": str(install),
        "zones": zones,
        "arm_order": arms,
        "arms": arm_results,
    }
    (out_dir / "baseline.json").write_text(json.dumps(meta, indent=2), encoding="utf-8")
    write_baseline_summary(out_dir / "summary.md", meta)

    print(f"\nOK baseline '{name}' -> {out_dir}")
    suspect = False
    for arm in arms:
        a = arm_results[arm]
        flag = "  !! SUSPECT (never reached steady-state render)" if a.get("suspect") else ""
        suspect = suspect or a.get("suspect")
        print(f"  {arm}: {a['frames']} frames / {args.seconds}s = {a['fps']:.2f} fps "
              f"({a['trace']}){flag}")
    print(f"  wrote {out_dir/'baseline.json'} + {out_dir/'summary.md'}")
    if suspect:
        print(f"WARNING: >=1 arm has < {MIN_STEADY_FRAMES} frames -- likely cold-cache load "
              "(first arm on a heavy map), crash, or VK fail-soft. Re-run (cache now warm) or "
              "raise --seconds before trusting this baseline.", file=sys.stderr)
        rc = rc or 1
    return rc


# ---------------------------------------------------------------- regress ----

def cmd_regress(args: argparse.Namespace) -> int:
    export_exe = tracy_exe("tracy-csvexport.exe")
    base_dir = Path(args.baseline)
    base_json = base_dir / "baseline.json"
    if not base_json.is_file():
        sys.exit(f"ERROR: {base_json} not found (need a baseline dir from the "
                 f"`baseline` subcommand)")
    base = json.loads(base_json.read_text(encoding="utf-8"))
    install = Path(args.install)
    warn_pct, fail_pct = args.warn_pct, args.fail_pct
    zones = base["zones"]
    arms = base["arm_order"]
    seconds = base["seconds"]

    out_dir = Path(args.out_dir) if args.out_dir else (
        base_dir / ("regress-" + datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")))
    out_dir.mkdir(parents=True, exist_ok=True)

    print(f"[regress] baseline={base['name']} (head {base['head']}) arms={arms} "
          f"mission={base['mission']} seconds={seconds} -> {out_dir}")

    verdict = "PASS"   # PASS -> WARN -> FAIL
    md = [f"# Tracy regress vs baseline `{base['name']}`", "",
          f"- baseline HEAD: `{base['head']}`  |  current HEAD: `{git_head()}`",
          f"- mission `{base['mission']}`, pose `{base['pose']}`, {seconds}s uncapped free-timestep",
          f"- thresholds: WARN >{warn_pct:.0f}% frame drop, FAIL >{fail_pct:.0f}% frame drop",
          "", "## Frame rate", "",
          "| arm | base frames | new frames | Δfps | Δframes | verdict |",
          "|---|--:|--:|--:|--:|---|"]

    for arm in arms:
        trace = out_dir / f"regress-{arm}.tracy"
        capture_trace(install, arm, base["mission"], seconds, trace,
                      pose=base["pose"], fixed_timestep=False, fps_cap=0)
        base_arm = base["arms"].get(arm, {})
        try:
            render_options_pin.require_same({"values": base_arm.get("options")}, render_options_pin.read(options_sidecar(trace)),
                                            vary_list(args.vary), f"baseline {base['name']}", "this run")
        except render_options_pin.OptionsMismatch as e:
            sys.exit(f"REFUSED: {e}")
        new = measure_arm(trace, zones, seconds, export_exe)
        bf = base_arm.get("frames", 0)
        nf = new["frames"]
        drop = (bf - nf) / bf * 100.0 if bf else 0.0  # +drop = slower
        arm_verdict = "PASS"
        if drop > fail_pct:
            arm_verdict, verdict = "FAIL", "FAIL"
        elif drop > warn_pct:
            arm_verdict = "WARN"
            if verdict != "FAIL":
                verdict = "WARN"
        susp = " ⚠SUSPECT" if new.get("suspect") else ""
        md.append(f"| {arm} | {bf} | {nf}{susp} | {fmt_delta(base_arm.get('fps'), new['fps'])} "
                  f"| {-drop:+.1f}% | {arm_verdict} |")
        print(f"  {arm}: base {bf} -> new {nf} frames ({-drop:+.1f}%) [{arm_verdict}]"
              f"{'  !! SUSPECT re-capture (cold-cache/crash/fail-soft, not a real regression)' if new.get('suspect') else ''}")

        # per-zone mean deltas
        md += ["", f"### {arm} — per-zone mean (ns)", "",
               "| zone | base | new | Δmean |", "|---|--:|--:|--:|"]
        for z in zones:
            bz = base_arm.get("zones", {}).get(z)
            nz = new["zones"].get(z)
            bm = bz["mean_ns"] if bz else None
            nm = nz["mean_ns"] if nz else None
            md.append(f"| `{z}` | {bm:.0f} | {nm:.0f} | {fmt_delta(bm, nm)} |"
                      if (bz and nz) else
                      f"| `{z}` | {'-' if not bz else f'{bm:.0f}'} "
                      f"| {'-' if not nz else f'{nm:.0f}'} | - |")

    md += ["", f"## Verdict: **{verdict}**", ""]
    report = out_dir / "regress.md"
    report.write_text("\n".join(md), encoding="utf-8")
    print(f"\n{verdict}: regress vs '{base['name']}' -> {report}")
    return {"PASS": 0, "WARN": 1, "FAIL": 2}[verdict]


# ------------------------------------------------------------- selfcheck ----

# Two drawable sizes that MUST measure apart: the 4K arm shades four times the
# fragments of the pinned 1080p arm with the same scene, so the pair is a known
# fragment-bound cost on the VK frame (the shipped backend): 33.0% p50 apart on
# mc2_10, against the selfcheck's 8% bar. The retired GL
# light-pool cap 64 vs 256, moves the VK frame only 1.3%, so it could not prove a
# VK harness.
SELFCHECK_SIZES = (render_resolution.RESOLUTION_TIER_SIZES[0], render_resolution.RESOLUTION_TIER_SIZES[1])
# Minimum p50 separation (%) we require between those two arms before we are
# willing to believe this harness can see a fragment-bound cost at all.
SELFCHECK_MIN_PCT = 8.0
VK_RECEIPT = "Vulkan backend selected"
FAIL_SOFT_RECEIPT = "[RENDER_BACKEND] fail-soft"


def assert_drew_vk(game_log: Path) -> None:
    """A VK measurement is refused unless the engine selected VK and never fell back to GL."""
    assert_receipt(game_log, VK_RECEIPT, "the VK backend")
    if FAIL_SOFT_RECEIPT in game_log.read_text(encoding="utf-8", errors="replace"):
        sys.exit(f"REFUSING TO REPORT: {game_log.name} logs a VK fail-soft to GL -- the run measured the GL frame.")


def _measure_pose(install: Path, out_dir: Path, tag: str, mission: str, pose: str,
                  seconds: int, env_list, export_exe: Path, zone: str) -> dict:
    """One VK capture + verified per-sample stats. Refuses on any integrity failure."""
    trace = out_dir / f"{tag}.tracy"
    capture_trace(install, "vk", mission, seconds, trace, extra_env=env_list,
                  pose=pose, fixed_timestep=False, fps_cap=0, show_window=True)
    assert_window_was_visible(trace, export_exe, context=tag)
    assert_drew_vk(trace.with_suffix(".game.log"))
    if pose:
        assert_receipt(trace.with_suffix(".game.log"), "[CAMERA_PARK", f"pose {pose}")
    # TRUE frame time (wall-clock frame-to-frame), NOT the CPU zone duration --
    # see frame_intervals_ms() for why the latter is blind to fragment cost.
    s = frame_intervals_ms(trace, export_exe, zone)
    if not s:
        sys.exit(f"REFUSING TO REPORT: zone '{zone}' yielded no frame intervals "
                 f"in {trace.name}.")
    cpu = zone_samples(trace, zone, export_exe)
    p50 = pct(s, 50)
    return {"tag": tag, "n": len(s),
            "options": (render_options_pin.read(options_sidecar(trace)) or {}).get("values"),
            "p50_ms": p50, "p99_ms": pct(s, 99),
            "mean_ms": sum(s) / len(s),
            "fps_p50": 1000.0 / p50 if p50 else 0.0,
            # Kept for contrast: the CPU-zone timing, not wall-clock frame time.
            "cpu_zone_p50_ms": (pct(cpu, 50) / 1e6) if cpu else None}


def cmd_selfcheck(args: argparse.Namespace) -> int:
    """Prove the harness can SEE a known fragment-bound cost, or fail loudly.

    Runs the same VK scene at the pinned 1080p and at 4K (four times the
    fragments) at a busy night pose. Those two arms are known to differ
    substantially. If this harness reports them as the same, it cannot see
    fragment cost, and every perf number it produces is suspect -- so we exit
    non-zero instead of letting a future lane cite a flat table.
    """
    export_exe = tracy_exe("tracy-csvexport.exe")
    out_dir = Path(args.out_dir); out_dir.mkdir(parents=True, exist_ok=True)
    print(f"[selfcheck] sizes={SELFCHECK_SIZES} pose={args.pose} "
          f"mission={args.mission} zone={args.zone}")
    res = {}
    for w, h in SELFCHECK_SIZES:
        res[(w, h)] = _measure_pose(
            Path(args.install), out_dir, f"selfcheck_{w}x{h}", args.mission,
            args.pose, args.seconds,
            [f"{k}={v}" for k, v in render_resolution.size_env(w, h).items()],
            export_exe, args.zone)
        print(f"  {w}x{h} p50={res[(w, h)]['p50_ms']:.3f}ms "
              f"p99={res[(w, h)]['p99_ms']:.3f}ms n={res[(w, h)]['n']}")

    lo, hi = res[SELFCHECK_SIZES[0]]["p50_ms"], res[SELFCHECK_SIZES[1]]["p50_ms"]
    sep = abs(hi - lo) / lo * 100.0 if lo else 0.0
    print(f"[selfcheck] p50 separation = {sep:.1f}% "
          f"(require >= {SELFCHECK_MIN_PCT:.0f}%)")
    if sep < SELFCHECK_MIN_PCT:
        print(
            f"\nSELFCHECK FAILED: {SELFCHECK_SIZES[0]} and {SELFCHECK_SIZES[1]} "
            f"measured within {sep:.1f}% of each other. Shading 4x the fragments "
            f"MUST cost more than that at a busy night pose, so "
            f"this harness is NOT seeing fragment-bound work. Do not report frame "
            f"times from it. Likely causes, in the order they have actually bitten:\n"
            f"  1. Measuring CPU ZONE DURATION instead of frame-to-frame wall time. "
            f"The GPU is async, so fragment cost never shows up in a CPU zone. This "
            f"is what made the original 34% regression read as 0.2%.\n"
            f"  2. Window minimized/hidden -> 10ms background throttle.\n"
            f"  3. The pose did not apply (no [CAMERA_PARK] receipt).",
            file=sys.stderr)
        return 2
    print("SELFCHECK PASSED: the harness resolves a known fragment-bound cost.")
    return 0


# ---------------------------------------------------------------- matrix ----

def cmd_matrix(args: argparse.Namespace) -> int:
    """arms x poses frame-time table with real p50/p99, every arm verified."""
    export_exe = tracy_exe("tracy-csvexport.exe")
    out_dir = Path(args.out_dir); out_dir.mkdir(parents=True, exist_ok=True)
    spec = json.loads(Path(args.spec).read_text(encoding="utf-8"))
    arms, poses = spec["arms"], spec["poses"]
    results = {}
    for pname, pdef in poses.items():
        # A pose entry is either a bare MC2_CAMERA_PARK string, or an object
        # {"mission": ..., "pose": ...} when the pose lives on another map
        # (e.g. the day/no-pool control is a different mission entirely).
        if isinstance(pdef, dict):
            mission, pose = pdef.get("mission", args.mission), pdef.get("pose")
        else:
            mission, pose = args.mission, pdef
        for aname, aenv in arms.items():
            tag = f"{aname}__{pname}"
            print(f"[matrix] {tag} mission={mission} pose={pose}")
            results[tag] = _measure_pose(
                Path(args.install), out_dir, tag, mission, pose,
                args.seconds, aenv, export_exe, args.zone)
            results[tag].update(arm=aname, pose=pname, mission=mission)
    (out_dir / "matrix.json").write_text(json.dumps(results, indent=2), encoding="utf-8")

    md = [f"# Frame-time matrix (zone `{args.zone}`)", "",
          f"- mission `{args.mission}`, {args.seconds}s, uncapped free-timestep, "
          f"VISIBLE window (throttle-free, asserted per arm)",
          f"- repo HEAD `{git_head()}`", "",
          "| pose | arm | p50 ms | p99 ms | mean ms | samples |",
          "|---|---|--:|--:|--:|--:|"]
    for k, r in results.items():
        md.append(f"| {r['pose']} | {r['arm']} | {r['p50_ms']:.3f} | "
                  f"{r['p99_ms']:.3f} | {r['mean_ms']:.3f} | {r['n']} |")
    (out_dir / "matrix.md").write_text("\n".join(md) + "\n", encoding="utf-8")
    print("\n".join(md))
    print(f"\nwrote {out_dir/'matrix.json'} + {out_dir/'matrix.md'}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("capture", help="record a headless Tracy trace of an MC2 smoke run")
    c.add_argument("--install", required=True, help="MC2 install dir containing mc2.exe")
    c.add_argument("--arm", choices=list(ARMS), default="vk")
    c.add_argument("--mission", default="mc2_01")
    c.add_argument("--seconds", type=int, default=15, help="Tracy recording window (default 15)")
    c.add_argument("--via-menu", action="store_true",
                   help="launch as a player does (boot, main menu, stock campaign, mission; no smoke mode, no --mission, no fixed timestep). "
                        "--seconds must cover menu + mission load (use 90+); the trace includes the front end. [VIA_MENU v1]/[FIRST60 v1] lines are in the .game.log")
    c.add_argument("--ticks", type=int, default=0, help="capture this many headless sim ticks, including mission load; no recorder wall-clock cutoff")
    c.add_argument("--csv", action="store_true", help="also write the full per-zone csvexport table next to the trace (default: top-10 summary only)")
    c.add_argument("-o", "--output", required=True, help="output .tracy path")
    c.add_argument("--pose", default=None, metavar="X,Y,ROT,PROJ,ALT[@FRAME]",
                   help="MC2_CAMERA_PARK pose to pin the camera (optional)")
    c.add_argument("--free-timestep", action="store_true",
                   help="run free wall-clock timestep (required for FPS/frame-count "
                        "measurement; default is deterministic fixed timestep)")
    c.add_argument("--fps-cap", type=int, default=None, metavar="N",
                   help="set MC2_FPS_CAP (0 = uncapped/render-bound)")
    c.add_argument("--no-show-window", dest="show_window", action="store_false",
                   help="launch the game MINIMIZED (SW_SHOWMINNOACTIVE). The loop "
                        "then throttles to 10ms/frame and fragment-bound costs "
                        "become invisible -- CPU-zone work only, and the "
                        "visibility assert will refuse the trace. Default is a "
                        "visible, non-focus-stealing window.")
    c.set_defaults(show_window=True)
    c.add_argument("--env", action="append", default=[], metavar="KEY=VAL",
                   help="extra env var(s) for the game, applied AFTER the arm's "
                        "defaults + the MC2_VK* scrub (e.g. --env MC2_VK_SHARED_GBUFFER=0). "
                        "Repeatable.")
    c.set_defaults(fn=cmd_capture)

    d = sub.add_parser("compare", help="per-zone before/after delta table")
    d.add_argument("before")
    d.add_argument("after")
    d.add_argument("--zones", help="comma-separated zone names "
                                   f"(default: {','.join(DEFAULT_ZONES)})")
    d.add_argument("--vary", default="", help="comma-separated options.cfg keys that differ between the traces on purpose "
                                              "(any other difference in the recorded render options is refused)")
    d.set_defaults(fn=cmd_compare)

    b = sub.add_parser("baseline", help="capture the arms (default vk) at a pose -> dated baseline dir")
    b.add_argument("--install", required=True, help="MC2 install dir containing mc2.exe")
    b.add_argument("--arms", default=",".join(DEFAULT_ARMS),
                   help="arms to measure; the only arm is vk since the GL arm was retired (any other value is refused)")
    b.add_argument("--mission", default=DEFAULT_BASELINE_MISSION)
    b.add_argument("--pose", default=DEFAULT_POSE, metavar="X,Y,ROT,PROJ,ALT[@FRAME]")
    b.add_argument("--seconds", type=int, default=DEFAULT_BASELINE_SECONDS,
                   help="Tracy recording window (default 15)")
    b.add_argument("--name", default=None, help="baseline name (default: UTC date)")
    b.add_argument("--out-dir", default=None,
                   help=f"output dir (default: {BASELINE_ROOT.as_posix()}/<name>)")
    b.add_argument("--zones", help="comma-separated zone names "
                                   f"(default: {','.join(DEFAULT_ZONES)})")
    b.set_defaults(fn=cmd_baseline)

    r = sub.add_parser("regress", help="re-capture both arms + compare vs a baseline dir")
    r.add_argument("--baseline", required=True, help="baseline dir (holds baseline.json)")
    r.add_argument("--install", required=True, help="MC2 install dir containing mc2.exe")
    r.add_argument("--out-dir", default=None,
                   help="output dir for the regress traces + report "
                        "(default: <baseline>/regress-<timestamp>)")
    r.add_argument("--warn-pct", type=float, default=REGRESS_WARN_PCT,
                   help=f"frame-count drop%% that WARNs (default {REGRESS_WARN_PCT})")
    r.add_argument("--fail-pct", type=float, default=REGRESS_FAIL_PCT,
                   help=f"frame-count drop%% that FAILs (default {REGRESS_FAIL_PCT})")
    r.add_argument("--vary", default="", help="comma-separated options.cfg keys that differ from the baseline on purpose "
                                              "(any other difference in the recorded render options is refused)")
    r.set_defaults(fn=cmd_regress)

    s = sub.add_parser("selfcheck", help="prove the harness can see a known "
                                         "fragment-bound cost (1080p vs 4K on VK)")
    s.add_argument("--install", required=True)
    s.add_argument("--mission", default=DEFAULT_BASELINE_MISSION)
    s.add_argument("--pose", default=DEFAULT_POSE)
    s.add_argument("--seconds", type=int, default=DEFAULT_BASELINE_SECONDS)
    s.add_argument("--zone", default=FRAME_ZONE)
    s.add_argument("--out-dir", required=True)
    s.set_defaults(fn=cmd_selfcheck)

    m = sub.add_parser("matrix", help="arms x poses p50/p99 frame-time table")
    m.add_argument("--install", required=True)
    m.add_argument("--spec", required=True,
                   help='JSON {"arms":{name:[ENV=V,..]}, "poses":{name:pose}}')
    m.add_argument("--mission", default=DEFAULT_BASELINE_MISSION)
    m.add_argument("--seconds", type=int, default=DEFAULT_BASELINE_SECONDS)
    m.add_argument("--zone", default=FRAME_ZONE)
    m.add_argument("--out-dir", required=True)
    m.set_defaults(fn=cmd_matrix)

    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
