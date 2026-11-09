#!/usr/bin/env python3
"""S9 -- in-engine bookmark-capture runner (the missing Baseline-A wrapper).

S9 shipped the engine primitive (GameOS/gameos/gos_visual_capture.cpp) and the
bookmark JSON format (tests/visual/bookmarks/<mission>.json), but NO script to
actually launch mc2.exe, drive the deterministic sweep, collect the per-bookmark
PNGs, and verify byte-stability. This is that runner -- the thing you point at a
release candidate to produce Baseline-A golden-frame candidates.

Unlike scripts/capture_baseline.py (an OS-level pyautogui screenshot of the
desktop), this uses the engine's glReadPixels FBO readback: it does NOT grab the
screen, runs minimized, and is byte-stable across runs WHEN the deterministic
clock is engaged (MC2_SMOKE_FIXED_TIMESTEP=1 -- the engine stamps each sidecar's
`deterministic` flag honestly, so a non-deterministic capture is never
mislabeled golden).

Engine contract (gos_visual_capture.cpp):
  MC2_SMOKE_MODE=1               required (mission/seed determinism only defined here)
  MC2_SMOKE_SEED=0xC0FFEE        fixed RNG seed
  MC2_SMOKE_FIXED_TIMESTEP=1     fixed-step clock -> cross-run byte-stability
  MC2_DETERMINISTIC_RNG=1        per-mission simulation RNG seed (with the fixed clock)
  MC2_VISUAL_BOOKMARK_CAPTURE    path to the bookmark JSON (arms the sweep)
  MC2_VISUAL_CAPTURE_DIR         output dir for <mission>_<bookmark>.{png,json}
  MC2_VISUAL_CAPTURE_FRAME       deterministic trigger frame (default 120)
  MC2_VISUAL_SETTLE              settle frames per bookmark (default 30)
Engine emits, per bookmark: <dir>/<mission>_<safeName>.{png,json}
(safeName flattens / \\ : . -> _).

This runner additionally writes a unified-manifest sidecar
(<mission>_capture_manifest.json) carrying the S12 identity block + per-bookmark
sha256, so a capture set is self-describing and joinable with every other MC2
artifact on the same identity fields (scripts/manifest_schema.py).

Usage:
  py -3 scripts/run_visual_capture.py --mission mc2_01 \
      --exe "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe"
  py -3 scripts/run_visual_capture.py --mission mc2_01 --verify   # 2x, byte-diff
"""
from __future__ import annotations

import argparse
import atexit
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

# CAPTURE-ENV-PIN-1: shipped-by-default MC2_* pinning + sidecar recording.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import mc2_env_pin as env_pin  # noqa: E402

# WATER-VISUAL-GATE-1 concurrent-safety:
#   _SKIP_KILL  — when True, do NOT taskkill foreign mc2.exe (set by --no-kill).
#   _ORIG_CURSOR — saved desktop cursor pos, restored at exit so the harness
#                  never leaves the mouse permanently warped.
_SKIP_KILL = False
_ORIG_CURSOR = None
# CURSOR-STEAL-HARNESS-GUARD-1: last park outcome, so the per-poll park logs
# once per state change instead of spamming a line every iteration.
_PARK_OUTCOME_SEEN = None
# HARNESS-ISOLATION-1: own-only-your-children. Every child mc2.exe we launch is
# tracked here; we reap ONLY our own (terminate/kill) at run-start and atexit, and
# NEVER taskkill a foreign/other-session mc2.exe. With MC2_SMOKE_SEED set (always),
# the launcher-bootstrap skip-guard (gameosmain.cpp) fires, so each child is a single
# mc2.exe with no grandchildren — a tracked-PID reap is sufficient (no Job Object).
_ACTIVE: "list[subprocess.Popen]" = []


def _reap_active() -> None:
    """Terminate any still-alive child mc2.exe WE launched. Foreign instances are
    never in _ACTIVE, so they are untouched."""
    for proc in _ACTIVE:
        try:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    try:
                        proc.wait(timeout=5)
                    except Exception:
                        pass
        except Exception as e:
            print(f"[viscap] WARN reap child: {e}", file=sys.stderr)
    _ACTIVE.clear()

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import manifest_schema as ms  # noqa: E402
sys.path.insert(0, str(ROOT / "tools" / "visual_verify"))
from ab import payload_identity, stamp_identity  # noqa: E402
# BACKGROUND-SMOKE-1: no agent capture may take focus / fullscreen / the cursor.
from smoke_lib import nofocus  # noqa: E402
from smoke_lib import capture_backend, render_options_pin, render_resolution  # noqa: E402
from smoke_lib import exe_guard  # noqa: E402
from smoke_lib.firewall_dialog import Guard as FirewallGuard

DEFAULT_EXE = Path(r"A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe")
BOOKMARK_DIR = (Path(os.environ["MC2_VISUAL_BOOKMARK_DIR"])
                if os.environ.get("MC2_VISUAL_BOOKMARK_DIR")
                else ROOT / "tests" / "visual" / "bookmarks")
DEFAULT_OUT = ROOT / "tests" / "visual" / "captures"
# Candidate golden sets land beside the blessed ones; the legacy
# tests/visual/golden-sets.json registry (and scripts/run_visual.py) are retired.
GOLDENS_DIR = ROOT / "tests" / "visual" / "goldens"


def _safe_name(name: str) -> str:
    out = []
    for c in name:
        out.append("_" if c in "/\\:." else c)
    return "".join(out)


def _load_bookmarks(path: Path) -> tuple[str, list[dict]]:
    doc = json.loads(path.read_text(encoding="utf-8"))
    return doc.get("mission", ""), doc.get("bookmarks", [])


def _kill_existing_mc2() -> None:
    # HARNESS-ISOLATION-1: the harness is now ALWAYS concurrent-safe. It never
    # taskkills foreign/other-session mc2.exe; instead it reaps ONLY its own tracked
    # children (_reap_active). The legacy blanket `taskkill /F /IM mc2.exe` is GONE.
    # `--no-kill` is retained as a deprecated no-op for callers (e.g. the gate) that
    # still pass it; both paths now behave identically (own-only reap).
    _reap_active()
    if _SKIP_KILL:
        print("[viscap] --no-kill (now default): own-children-only reap, "
              "foreign mc2.exe untouched", file=sys.stderr)


def _save_cursor() -> None:
    """No-op: the harness no longer moves the pointer, so there is nothing to put back."""


def _restore_cursor() -> None:
    """No-op (see _save_cursor)."""


def park_cursor_center() -> None:
    """No-op. The capture child runs MC2_SMOKE_MODE=1, in which the engine ignores the real pointer (no edge scroll, no grab); the harness
    never moves a person's pointer. Kept as a name because the poll loop calls it."""


def _disk_guard_advisory(path, need_gb: float, label: str) -> None:
    """DISK-GUARD-1 advisory probe. Prints the verdict; never blocks a capture.

    Capture output is the tree's biggest untracked-byte producer, so this is the
    natural place to surface a tightening drive. It stays advisory because
    aborting an investigation on a threshold is its own failure mode; the sites
    that abort are build_release.sh and deploy_payload.py.
    """
    guard = Path(__file__).resolve().parent / "check-disk-space.py"
    if not guard.is_file():
        return
    try:
        # background-launch: allow disk-guard advisory prints its verdict to the caller's console; inherits it (no new window when the parent has one)
        subprocess.run([sys.executable, str(guard), "--path", str(path),
                        "--need-gb", str(need_gb), "--label", label],
                       timeout=60)
    except Exception as e:  # noqa: BLE001 -- advisory only
        print(f"[visual_capture] disk guard skipped ({e})", file=sys.stderr)


def _engine_capture_done(log_path: Path, n_expected: int) -> bool:
    """True if the engine log shows >= n_expected '[VISUAL_CAPTURE ...] wrote'
    lines, or shows no 'wrote' protocol at all (older RC / VK readback path),
    in which case file stability alone decides."""
    try:
        txt = log_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return True
    wrote = txt.count("] wrote ")
    if wrote == 0:
        return "[VISUAL_CAPTURE" not in txt or "readback requested" in txt
    return wrote >= n_expected


def _mission_started(log_path: Path) -> bool:
    """True once the engine has presented its first mission frame (the in-mission clock starts there)."""
    try:
        return "event=first_frame_presented" in log_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False


_FATAL_LINE = re.compile(r"^(?:\[crash\] (?:code=|\w+ violation)|(?:\[[a-z0-9_ ]+\] )?fatal[: ]|fatal error:)", re.M)


def _load_failed(log_path: Path) -> bool:
    """True when the engine log reports a fatal error before the first presented frame (do not wait out the load cap)."""
    try:
        txt = log_path.read_text(encoding="utf-8", errors="replace").lower()
    except OSError:
        return False
    # Anchored at line start: the engine's own fatal lines ("[CRASH] code=", "[CRASH] ... violation", "[TAG] FATAL:"), not any line that mentions the words.
    return _FATAL_LINE.search(txt) is not None


def guard_exe(exe: Path) -> bool:
    """Vet `exe` before any launch; print the identity line and the refusal.

    A capture against an exe that cannot stay off the desktop (no
    MC2_WINDOW_OFFSCREEN, or built before the cursor-grab fix) puts a window on
    the user's screen, so it is refused unless MC2_OWNER_VISIBLE_OK=1.
    """
    verdict = exe_guard.check_exe(exe, purpose="capture")
    print(verdict.banner().replace("[SMOKE]", "[CAPTURE]"), file=sys.stderr, flush=True)
    if not verdict.ok:
        print(verdict.refusal(), file=sys.stderr, flush=True)
    return verdict.ok


def build_capture_env(bookmarks_path: Path, out_dir: Path, trigger_frame: int, settle: int, deterministic_clock: bool,
                      declared_env: "dict | None") -> "tuple[dict, dict]":
    """The environment a capture launches the engine under, and its record: the harness's own settings, the pinned drawable size, the declared
    variables, nothing else from the shell. Split out of run_one so the pinned size is testable without an engine."""
    # CAPTURE-ENV-PIN-1: do NOT inherit the ambient MC2_* environment.
    # os.environ.copy() would silently ride every stray gate in
    # the operator's shell into the captured frame -- unrecorded. See
    # scripts/mc2_env_pin.py for the incident that motivated this.
    harness_sets = {
        "MC2_SMOKE_MODE": "1",
        # MC2_LOG master gate: keep stdout live (engine default redirects it -> NUL
        # so normal play never hitches on the unbuffered printf flush). Capture
        # detection reads [VISUAL_CAPTURE] from stderr, but this keeps capture-run
        # behavior identical to pre-gate. See gameosmain.cpp after setvbuf.
        "MC2_LOG": "1",
        "MC2_SMOKE_SEED": "0xC0FFEE",
        "MC2_HEARTBEAT": "1",
        "MC2_VISUAL_BOOKMARK_CAPTURE": str(bookmarks_path),
        "MC2_VISUAL_CAPTURE_DIR": str(out_dir),
        "MC2_VISUAL_CAPTURE_FRAME": str(trigger_frame),
        "MC2_VISUAL_SETTLE": str(settle),
    }
    if deterministic_clock:
        harness_sets["MC2_SMOKE_FIXED_TIMESTEP"] = "1"
        # The simulation stream is wall-clock seeded in play; the night turret searchlights pick their sweep targets from it, so without the
        # per-mission seed two launches light a night pose differently (mc2_10 nightlow a: 13.3 mean |RGB| run to run, 0.000 with it).
        harness_sets["MC2_DETERMINISTIC_RNG"] = "1"

    # BACKGROUND-SMOKE-1: an agent capture must never take the display
    # fullscreen or grab the pointer. These are HARNESS_CONTROL vars in
    # mc2_env_pin, so they survive the scrub and are recorded under
    # `harness_control` in the sidecar.
    #
    # The drawable size is pinned (smoke_lib/render_resolution.py carries the
    # reason): a capture is graded at one resolution on every install instead of
    # at whatever options.cfg ResolutionX/Y says. A caller that declares
    # MC2_WIDTH/MC2_HEIGHT (--env-declare) takes the size it asked for.
    try:
        render_resolution.pin_unless_declared(harness_sets, declared_env)
    except render_resolution.HalfDeclaredSize as e:
        raise SystemExit(f"[viscap] {e}")
    for _k, _v in nofocus.NOFOCUS_ENV.items():
        harness_sets.setdefault(_k, _v)

    # NOFOCUS-FINISH-1: deliver MC2_WINDOW_OFFSCREEN too. This is the busiest
    # capture path in the project and it was the one still exposed: it builds
    # its child env through env_pin.build_pinned_env() rather than
    # nofocus.apply_env(), env_pin is deny-by-default, and MC2_WINDOW_OFFSCREEN
    # was not in its HARNESS_CONTROL passthrough -- so the engine-side fix
    # arrived inert and the only thing left was the 250ms park poller, which
    # cannot fire before the window exists. This is the fix for the
    # flash, not the backstop for it.
    #
    # Free of pixel consequences: the GL drawable keeps its full size, and this
    # runner reads pixels via the engine's glReadPixels FBO readback, not the
    # desktop composite -- same reason park_window_offscreen(force=True) below
    # is safe here and is not safe in the pyautogui harnesses.
    #
    # OWNER RULE (background-only): --show-window no longer opts out. It used to
    # mean "put the window on the desktop", which is exactly the focus/cursor
    # theft the owner banned. It now only means "not MINIMIZED" (SW_SHOWNOACTIVATE
    # instead of SW_SHOWMINNOACTIVE, so SDL does not throttle the loop); the
    # window is still parked offscreen and still non-activating.
    for _k, _v in nofocus.OFFSCREEN_ENV.items():
        harness_sets.setdefault(_k, _v)

    env, env_record = env_pin.build_pinned_env(harness_sets, declared_env)
    warn = env_pin.format_warning(env_record)
    if warn:
        print(warn, file=sys.stderr)
    return env, env_record


DEFAULT_DURATION_S = 15


def smoke_capture(bookmarks, mission, out, verify=False, exe=None):
    """Share a stock smoke boot with matching deterministic bookmark captures."""
    env = {}
    matching = [b for b in bookmarks if _load_bookmarks(b)[0] == mission]
    if len(matching) > 1:
        raise ValueError(f"multiple capture bookmarks for {mission}")
    for bookmark in matching:
        _, marks = _load_bookmarks(bookmark)
        pose_dir = out.resolve() / bookmark.stem
        if not verify:
            pose_dir.mkdir(parents=True, exist_ok=False)
            values, _ = build_capture_env(bookmark.resolve(), pose_dir, 431, 30, True,
                                         {"MC2_OPTIONS_OVERRIDE": "RemasteredBuildings=TRUE", "MC2_VISUAL_CAPTURE_CLEAN": "1"})
            env.update({k: v for k, v in values.items() if k.startswith("MC2_")})
        else:
            if len(marks) < 2:
                raise RuntimeError("capture requires at least two bookmark poses")
            for mark in marks:
                png = pose_dir / f"{mission}_{_safe_name(mark['name'])}.png"
                side = png.with_suffix(".json")
                if not png.is_file() or not side.is_file():
                    raise RuntimeError(f"tier1 capture missing PNG/sidecar: {png}")
                metadata = json.loads(side.read_text(encoding="utf-8"))
                if (not metadata.get("deterministic") or metadata.get("backend") != "vulkan"
                        or metadata.get("render_options", {}).get("RemasteredBuildings") is not True):
                    raise RuntimeError(f"invalid tier1 capture: {side}")
            if exe:
                # Keep the ordinary capture-sweep artifact contract, including
                # executable identity for a later --before-dir reuse.
                path = out / "capture_run.json"
                manifest = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {
                    "kind": "visual-verify-capture", "schema_v": 2, "poses": [],
                    "frame": 431, "settle": 30, "deterministic_clock": True,
                    "n_requested": len(bookmarks), "install": str(Path(exe).resolve().parent),
                    "render_options": metadata["render_options"],
                    "backend": {"requested": "vulkan", "drew": ["vulkan"]},
                }
                if manifest.get("identity") and manifest["identity"]["exe"]["sha256"] != ms.file_sha256(exe):
                    raise RuntimeError("shared capture executable changed")
                primary = next((m for m in marks if not m["name"].endswith("_dup")), marks[0])
                png = pose_dir / f"{mission}_{_safe_name(primary['name'])}.png"
                manifest["poses"] = [p for p in manifest["poses"] if p["bookmark"] != bookmark.stem]
                manifest["poses"].append({"bookmark": bookmark.stem, "mission": mission,
                    "primary_png": str(png), "primary_sha256": ms.file_sha256(png),
                    "all_present": True, "engine_deterministic": True})
                manifest["n_captured"] = len(manifest["poses"])
                ms.attach(manifest, ms.identity_block(generator="tier1_shared_capture", exe_path=exe,
                                                      repo_root=str(ROOT), deploy_target=str(Path(exe).resolve().parent)))
                path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
                stamp_identity(out, payload_identity(Path(exe).resolve().parent))
    return env


def default_duration(n_bookmarks: int, trigger_frame: int, settle: int) -> int:
    """About 15 s of in-mission time (owner rule); longer only when the sweep's frames cannot fit at 15 fps. The run still ends
    as soon as the PNGs and sidecars are complete, so this is a timeout."""
    return max(DEFAULT_DURATION_S, 5 + -(-(trigger_frame + settle * n_bookmarks) // 15))


def run_one(exe: Path, mission: str, bookmarks_path: Path, out_dir: Path,
            trigger_frame: int, settle: int, duration: int | None,
            deterministic_clock: bool, show_window: bool = False,
            declared_env: dict | None = None, classic_assets: bool = False) -> dict:
    """Launch one capture sweep; return a result dict (never raises on engine
    failure -- records it instead)."""
    install_identity = payload_identity(exe.parent)
    if not guard_exe(exe):
        raise RuntimeError(f"exe guard refused {exe}")
    # The engine runs with cwd = the deploy dir and resolves
    # MC2_VISUAL_CAPTURE_DIR relative to THAT, so a relative out_dir would write
    # under the deploy (and fopen-fail if absent). Always pass an absolute path.
    out_dir = Path(out_dir).resolve()
    bookmarks_path = Path(bookmarks_path).resolve()

    # DISK-GUARD-1 (2026-07-29 ENOSPC incident): probe BEFORE mkdir / engine
    # launch. Capture sweeps are the single largest untracked-byte producer in
    # the tree (34.7 GB of PNG/TGA/.rdc accumulated under .claude/ across two
    # lanes and filled the drive). Advisory: warns, never blocks a capture.
    _disk_guard_advisory(out_dir, need_gb=2, label="visual capture sweep")

    out_dir.mkdir(parents=True, exist_ok=True)
    file_mission, marks = _load_bookmarks(bookmarks_path)
    mission_token = file_mission or mission
    # HARNESS-ISOLATION-1 / bug-2 guard: the engine bookmark iterator
    # (gos_visual_capture.cpp) silently fails to fire the capture for a SINGLE-pose
    # bookmark (engine_capture_fired=True but present=False). A >=2-pose bookmark
    # fires fine. Until the engine ticket (HARNESS-SINGLEPOSE-2) lands, WARN loudly.
    if len(marks) < 2:
        print(f"[viscap] WARN single-pose bookmark ({len(marks)} pose) — engine "
              f"capture iterator may silently NOT fire; add a 2nd pose. "
              f"({bookmarks_path})", file=sys.stderr)
    expected_pngs = {
        m["name"]: out_dir / f"{mission_token}_{_safe_name(m['name'])}.png"
        for m in marks
    }
    # Clear any stale PNGs for these bookmarks so presence-polling is honest.
    for p in expected_pngs.values():
        for ext in (".png", ".json"):
            f = p.with_suffix(ext)
            if f.exists():
                f.unlink()

    env, env_record = build_capture_env(bookmarks_path, out_dir, trigger_frame, settle, deterministic_clock, declared_env)

    _kill_existing_mc2()
    park_cursor_center()   # before launch: no edge-scroll from a stray cursor

    log_path = out_dir / f"{mission_token}_capture.log"
    log_fp = open(log_path, "w", encoding="utf-8", errors="replace")

    # The render options are an input of the capture like the drawable size: the install's options.cfg plus MC2_OPTIONS_OVERRIDE.
    options_rec = render_options_pin.record(exe.parent, env)
    if classic_assets:
        options_rec["values"]["RemasteredBuildings"] = False
    for rj in options_rec["rejected"]:
        print(f"[viscap] WARN render option rejected by the engine's grammar: {rj}", file=sys.stderr)

    if duration is None:
        duration = default_duration(len(marks), trigger_frame, settle)
    proc_args = [str(exe), "--profile", "stock", "--mission", mission_token,
                 "--duration", str(duration)]
    if classic_assets:
        proc_args.append("--classic-assets")
    print(f"[viscap] mission={mission_token} bookmarks={len(marks)} "
          f"trigger={trigger_frame} settle={settle} dur={duration}s "
          f"fixed_clock={deterministic_clock}")
    # OWNER RULE: the engine is launched through nofocus.spawn ONLY. spawn()
    # supplies the non-activating STARTUPINFO (SW_SHOWNOACTIVATE for
    # --show-window, else SW_SHOWMINNOACTIVE), re-applies the windowing env on
    # top of the pinned `env` (setdefault: pinned values win), and starts the
    # offscreen-park backstop for exes that predate MC2_WINDOW_OFFSCREEN. That is
    # safe HERE (unlike the pyautogui/mss harnesses) because this runner reads
    # pixels via the engine's glReadPixels FBO readback -- the window
    # contributes nothing to the capture and the drawable keeps its full size.
    # small_window=False: the pinned size above must not be replaced by the 640x480 smoke default.
    try:
        proc = nofocus.spawn(proc_args, stdout=log_fp, stderr=subprocess.STDOUT,
                             cwd=str(exe.parent), env=env,
                             show_window=show_window, small_window=False)
        _ACTIVE.append(proc)  # HARNESS-ISOLATION-1: track for own-only reap
        firewall = FirewallGuard(exe)
        # The budget starts once the engine is running: spawn() blocks on the host-wide game lease, and a wait
        # behind another lane's smoke must not eat the poses' time (it did, twice).
        t0 = time.time()

        # Poll for all expected PNGs, or process exit, or timeout. Re-park the
        # cursor every iteration so it stays pinned center through the ENTIRE sweep
        # window (trigger frame + per-bookmark settle) -- a single pre-launch park
        # is not enough if the cursor moves during the run.
        # The in-mission budget starts at the first presented frame, not at spawn: a slow or contended load (31 s measured at 4K, 74 s on a
        # busy host) used to eat the whole budget and kill the engine before any bookmark was requested, which then read as
        # "no sidecar or no backend/readback_source".
        load_cap = 300
        ready_t = None
        deadline = t0 + load_cap
        done_early = False
        last_sizes = None
        while time.time() < deadline:
            firewall.poll()
            park_cursor_center()
            if ready_t is None and _mission_started(log_path):
                ready_t = time.time()
                deadline = ready_t + duration + 30
            elif ready_t is None and _load_failed(log_path):
                break  # fatal error before the first frame: fail now (the process exit check below already covers a dead engine)
            if all(p.exists() for p in expected_pngs.values()):
                # CAPTURE-EARLY-EXIT-1: finished only when every PNG + sidecar
                # exists, sizes are non-zero and unchanged across two polls, and
                # (when the engine logs "wrote" lines) it has logged one per PNG.
                sizes = []
                for p in expected_pngs.values():
                    try:
                        sizes.append((p.stat().st_size,
                                      p.with_suffix(".json").stat().st_size))
                    except OSError:
                        sizes = None
                        break
                if sizes and all(a > 0 and b > 0 for a, b in sizes):
                    if sizes == last_sizes and _engine_capture_done(
                            log_path, len(expected_pngs)):
                        done_early = True
                        break
                last_sizes = sizes
            else:
                last_sizes = None
            if proc.poll() is not None:
                break
            time.sleep(0.5)

        # Drain the process. After early exit, shut down now (terminate, then kill
        # after 5 s -- same path as _reap_active); otherwise wait out the budget.
        try:
            if done_early and proc.poll() is None:
                proc.terminate()
                proc.wait(timeout=5)
            else:
                remaining = max(1, ((ready_t or t0) + duration + 15) - time.time())
                proc.wait(timeout=remaining)
        except subprocess.TimeoutExpired:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except Exception:
                pass
        firewall.poll(final=True)
        print(f"[viscap] capture {'complete, early exit' if done_early else 'ended'}"
              f" after {time.time() - t0:.1f}s (budget {duration}s)")
    finally:
        log_fp.flush()
    log_fp.close()

    # Collect results per bookmark.
    bm_results = []
    sidecars = []
    all_present = True
    any_nondet = False
    for m in marks:
        png = expected_pngs[m["name"]]
        side = png.with_suffix(".json")
        present = png.exists()
        all_present = all_present and present
        sha = ms.file_sha256(png) if present else None
        det = None
        # SIM-IDENTITY-HASH-1: carry the engine's world-state hash out of the
        # sidecar and into the manifest. Without this the block dies here and
        # the refuse-to-grade gate downstream sees nothing -- which it would
        # correctly report as UNGRADED, i.e. a broken pipe would look exactly
        # like a real desync.
        simid = None
        engine_options = None
        sc = None
        if side.exists():
            try:
                sc = json.loads(side.read_text(encoding="utf-8"))
                det = sc.get("deterministic")
                simid = sc.get("sim_identity")
                engine_options = sc.get("render_options")
            except Exception:
                det = None
                simid = None
                sc = None
        sidecars.append((m["name"], sc))
        if det is False:
            any_nondet = True
        bm_results.append({
            "name": m["name"],
            "png": str(png),
            "present": present,
            "sha256": sha,
            "covers": m.get("covers", []),
            "engine_deterministic": det,
            "sim_identity": simid,
            "render_options": engine_options,
        })

    # Detect whether the engine's capture path even fired (RC may predate S9).
    fired = False
    try:
        logtxt = log_path.read_text(encoding="utf-8", errors="replace")
        fired = "[VISUAL_CAPTURE" in logtxt
    except Exception:
        pass

    # The engine's own report of the options is the record; this module's reading of the install must agree with it (a disagreement means the
    # grammar port or the launch environment is wrong, and the capture cannot be attributed).
    engine_opts = next((b["render_options"] for b in bm_results if b.get("render_options")), None)
    options_disagree = render_options_pin.check_engine(options_rec["values"], engine_opts)
    for line in options_disagree:
        print(f"[viscap] FAIL render options: harness reading and engine report disagree: {line}", file=sys.stderr)

    # The backend the capture asked for must be the one that drew it: a Vulkan request that fell back to GL is not a Vulkan capture.
    want_backend = capture_backend.requested(env)
    backend_disagree = capture_backend.check(want_backend, sidecars)
    for line in backend_disagree:
        print(f"[viscap] FAIL render backend: {line}", file=sys.stderr)

    # The GPU the run asked for and the one the engine logged, so A/Bs never mix GPUs.
    try:
        from smoke_lib import vk_profile
        logged_gpus = sorted({p.device for p in vk_profile.parse(log_path.read_text(encoding="utf-8", errors="replace"))})
    except Exception:
        logged_gpus = []
    gpu_rec = {"requested": nofocus.gpu_hint(env), "logged": logged_gpus}

    return {
        "mission": mission_token,
        "install_identity": install_identity,
        "gpu": gpu_rec,
        "backend": {"requested": want_backend,
                    "drew": sorted({d for d in (capture_backend.drew(sc) for _n, sc in sidecars if sc) if d}),
                    "disagree": backend_disagree},
        "render_options": {"values": {**options_rec["values"], **(engine_opts or {})},
                           "from": "engine + install" if engine_opts and options_rec["values"].keys() - engine_opts.keys() else "engine" if engine_opts else "install",
                           "inferred": sorted(options_rec["values"].keys() - (engine_opts or {}).keys()),
                           "rejected": options_rec["rejected"], "disagree": options_disagree},
        "bookmarks_path": str(bookmarks_path),
        "out_dir": str(out_dir),
        "all_present": all_present,
        "engine_capture_fired": fired,
        "any_nondeterministic": any_nondet,
        "show_window": show_window,
        "bookmarks": bm_results,
        "log": str(log_path),
        # CAPTURE-ENV-PIN-1: what the engine actually saw.
        "mc2_env": env_record,
    }


def fail_reasons(result: dict) -> "list[str]":
    """Why a capture result is a FAIL, one line each (empty when it passes): the manifest used to say FAIL and nothing else."""
    reasons = []
    if not result["all_present"]:
        reasons.append("not every bookmark produced a PNG")
    if not result["engine_capture_fired"]:
        reasons.append("the engine capture did not fire")
    reasons += [f"render options: {m}" for m in (result.get("render_options") or {}).get("disagree") or []]
    reasons += [f"render backend: {m}" for m in (result.get("backend") or {}).get("disagree") or []]
    return reasons


def write_manifest(result: dict, exe: Path) -> Path:
    out_dir = Path(result["out_dir"])
    identity = payload_identity(exe.parent)
    if result.get("install_identity", identity) != identity:
        raise ValueError("capture payload changed during capture")
    reasons = fail_reasons(result)
    verdict = "FAIL" if reasons else "PASS"
    ident = ms.identity_block(
        generator="visual_capture",
        exe_path=exe,
        repo_root=str(ROOT),
        deploy_target=str(exe.parent),
        # CAPTURE-ENV-PIN-1: snapshot the env the ENGINE saw (pinned), not the
        # harness's own ambient env, and not a hardcoded 5-name subset.
        env=(result.get("mc2_env") or {}).get("effective") or {},
        env_keys=None,
        extra={
            "bookmarks_path": result["bookmarks_path"],
            "show_window": bool(result.get("show_window", False)),
            "mc2_env": result.get("mc2_env"),
            "render_options": result.get("render_options"),
            "backend": result.get("backend"),
            "gpu": result.get("gpu"),
        },
    )
    rep = ms.report_summary(
        verdict=verdict,
        missions=[result["mission"]],
        artifact_dir=str(out_dir),
        extra={
            "fail_reasons": reasons,
            "engine_capture_fired": result["engine_capture_fired"],
            "all_bookmarks_present": result["all_present"],
            "any_nondeterministic": result["any_nondeterministic"],
            "bookmarks": result["bookmarks"],
        },
    )
    manifest = ms.attach({"kind": "visual-capture"}, ident, rep)
    path = out_dir / f"{result['mission']}_capture_manifest.json"
    path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    run = out_dir / "capture_run.json"
    sidecar = json.loads(run.read_text(encoding="utf-8")) if run.exists() else {}
    sidecar["identity"] = ident
    run.write_text(json.dumps(sidecar, indent=2), encoding="utf-8")
    stamp_identity(out_dir, identity)
    return path


def compare_runs(runs: list[dict]) -> tuple[bool, dict[str, dict]]:
    """Per-bookmark stability across the supplied runs. A bookmark is stable
    iff its sha256 is non-null and identical in EVERY run. Returns
    (all_stable, {name: {stable, shas}})."""
    if not runs:
        return False, {}
    names = [b["name"] for b in runs[0]["bookmarks"]]
    per: dict[str, dict] = {}
    all_stable = True
    for name in names:
        shas = []
        for r in runs:
            match = next((b for b in r["bookmarks"] if b["name"] == name), None)
            shas.append(match["sha256"] if match else None)
        stable = (shas[0] is not None and all(s == shas[0] for s in shas))
        all_stable = all_stable and stable
        per[name] = {"stable": stable, "shas": shas}
    return all_stable, per


def materialize_candidate(set_name: str, last_run: dict, per: dict,
                          exe: Path, n_runs: int, n_warmup: int,
                          allow_partial: bool = False) -> Path:
    """Copy the (stabilized) last run's PNGs+sidecars into the golden set dir
    and write its set.json with status=candidate.

    A set is captured as 'candidate', reviewed by a human walking the gallery,
    then 'blessed' by a COMMIT flipping set.json status. This writes the
    candidate; the human blesses.

    allow_partial: include only bookmarks that were byte-stable across the
    compared runs; record the rest as `excluded_unstable` (a small-but-real
    baseline, per the advisor). When False, the caller has already gated on
    all-stable so every present bookmark is a member.
    """
    import shutil
    set_dir = GOLDENS_DIR / set_name
    set_dir.mkdir(parents=True, exist_ok=True)
    members = []
    excluded = []
    for b in last_run["bookmarks"]:
        if not b["present"]:
            continue
        stable = per.get(b["name"], {}).get("stable")
        if allow_partial and not stable:
            excluded.append({"name": b["name"], "covers": b["covers"],
                             "reason": "byte-unstable across compared runs",
                             "shas": per.get(b["name"], {}).get("shas")})
            continue
        src_png = Path(b["png"])
        dst_png = set_dir / src_png.name
        shutil.copy2(src_png, dst_png)
        src_side = src_png.with_suffix(".json")
        if src_side.exists():
            shutil.copy2(src_side, dst_png.with_suffix(".json"))
        members.append({
            "name": b["name"],
            "png": dst_png.name,
            "sha256": b["sha256"],
            "covers": b["covers"],
            "engine_deterministic": b["engine_deterministic"],
            "stable_across_runs": stable,
        })

    ident = ms.identity_block(
        generator="visual_capture",
        exe_path=exe,
        repo_root=str(ROOT),
        deploy_target=str(exe.parent),
        env=(last_run.get("mc2_env") or {}).get("effective") or {},
        env_keys=None,
        extra={"mc2_env": last_run.get("mc2_env")},
    )
    set_record = {
        "set_id": set_name,
        "status": "candidate",          # human blesses via a set.json commit
        "blessed_commit": None,
        "supersedes": None,
        "mission": last_run["mission"],
        "path": str(set_dir),
        "stabilization": {
            "runs": n_runs,
            "warmup_discarded": n_warmup,
            "rule": f"runs[{n_warmup}:] byte-identical per bookmark",
            "all_stable": (not excluded) and bool(members),
            "partial": bool(excluded),
        },
        "identity": ident,
        # What drew the set and under which options: a blessed set says it was GL or Vulkan and which render options it ran with.
        "backend": {k: v for k, v in (last_run.get("backend") or {}).items() if k != "disagree"},
        "render_options": (last_run.get("render_options") or {}).get("values"),
        "bookmarks": members,
        "excluded_unstable": excluded,
    }
    # Write the per-set record next to the PNGs (the lab's set.json).
    (set_dir / "set.json").write_text(json.dumps(set_record, indent=2) + "\n",
                                      encoding="utf-8")

    return set_dir


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--mission", default="mc2_01",
                    help="mission stem; bookmarks default to "
                         "tests/visual/bookmarks/<mission>.json")
    ap.add_argument("--bookmarks", default=None,
                    help="explicit bookmark JSON path (overrides --mission lookup)")
    ap.add_argument("--exe", default=str(DEFAULT_EXE))
    ap.add_argument("--classic-assets", action="store_true", help="use stock building art; override this run only")
    ap.add_argument("--out-dir", default=None,
                    help="output dir (default tests/visual/captures/<mission>)")
    ap.add_argument("--trigger-frame", type=int, default=120)
    ap.add_argument("--settle", type=int, default=30)
    ap.add_argument("--duration", type=int, default=None,
                    help="timeout in in-mission seconds (default: about 15, see default_duration); the run ends when the captures land")
    ap.add_argument("--no-fixed-clock", action="store_true",
                    help="DISABLE MC2_SMOKE_FIXED_TIMESTEP (captures will NOT be "
                         "byte-stable across runs; use only for eyeball checks)")
    ap.add_argument("--show-window", action="store_true",
                    help="launch the game window visible instead of minimized. "
                         "Required for Vulkan present/swapchain harnesses that "
                         "need a nonzero drawable extent; ordinary baseline "
                         "captures should keep the default minimized mode.")
    ap.add_argument("--runs", type=int, default=1,
                    help="number of capture runs (default 1). With --warmup K, "
                         "the first K runs are discarded as cold-start warmup "
                         "and the remaining runs[K:] must be byte-identical.")
    ap.add_argument("--warmup", type=int, default=0,
                    help="leading runs to DISCARD as warmup before stability "
                         "comparison (shader compile / texture residency / "
                         "static-prop streaming / PBR cache first-use noise). "
                         "Baseline-A recommended: --runs 3 --warmup 1.")
    ap.add_argument("--verify", action="store_true",
                    help="alias for --runs 2 --warmup 0 (raw 2-run byte-diff, "
                         "no warmup discard). Prefer --runs 3 --warmup 1 for bless.")
    ap.add_argument("--candidate-set", default=None,
                    help="on stability PASS, materialize the last run as a "
                         "golden set with this id (e.g. baselineA-rc1) under "
                         "tests/visual/goldens/<id>/ with status=candidate in "
                         "its set.json.")
    ap.add_argument("--allow-partial", action="store_true",
                    help="materialize the STABLE bookmark subset even if some "
                         "bookmarks drift (the drifters are recorded as "
                         "excluded_unstable). A small-but-real baseline. "
                         "Requires >=1 stable bookmark + engine fired.")
    # CAPTURE-ENV-PIN-1: the ONLY sanctioned way to deviate from the shipped
    # MC2_* configuration. Everything else in the ambient shell is scrubbed.
    ap.add_argument("--focus", metavar="mech[:N]", default=None,
                    help="MC2_CAPTURE_FOCUS: move every bookmark pose onto the Nth live "
                         "player-team mech (default 0); the bookmark still supplies rot/proj.")
    ap.add_argument("--focus-zoom", type=float, default=None, metavar="ALT",
                    help="camera altitude with --focus (default 200 = mech at about a fifth of the frame)")
    ap.add_argument("--env-declare", action="append", default=[],
                    metavar="MC2_KEY=VALUE",
                    help="explicitly opt this MC2_* var into the capture "
                         "environment (repeatable). Ambient MC2_* vars that are "
                         "NOT declared are scrubbed so the capture runs the "
                         "shipped configuration; the full effective set is "
                         "recorded in the sidecar.")
    ap.add_argument("--no-kill", action="store_true",
                    help="WATER-VISUAL-GATE-1: do NOT taskkill foreign mc2.exe "
                         "(concurrent-safe). The capture still launches its own "
                         "child exe; only the global taskkill is skipped.")
    ap.add_argument("--screen", default=None, metavar="ID[@N]",
                    help="SCREEN-SETTLE-SHOT-1: screenshot a front-end screen (logistics, campaign_missions, mechbay, ...) after N settled "
                         "frames instead of a mission bookmark sweep; delegates to scripts/screen_shot.py")
    args = ap.parse_args()
    if args.screen:
        import screen_shot
        return screen_shot.run(Path(args.exe), args.screen, Path(args.out_dir or "tests/visual/captures/screens"),
                               declared=env_pin.parse_declarations(args.env_declare))

    # WATER-VISUAL-GATE-1 concurrent-safety: opt out of the global kill, and
    # always restore the desktop cursor on exit (the sweep parks it center).
    global _SKIP_KILL
    _SKIP_KILL = args.no_kill
    _save_cursor()
    atexit.register(_restore_cursor)
    atexit.register(_reap_active)  # HARNESS-ISOLATION-1: never leave our child alive

    exe = Path(args.exe)
    if not exe.exists():
        print(f"[viscap] ERROR exe missing: {exe}", file=sys.stderr)
        return 4
    if not guard_exe(exe):
        return 7
    bookmarks_path = (Path(args.bookmarks) if args.bookmarks
                      else BOOKMARK_DIR / f"{args.mission}.json")
    if not bookmarks_path.exists():
        print(f"[viscap] ERROR bookmarks missing: {bookmarks_path}", file=sys.stderr)
        print(f"[viscap] available: "
              f"{[p.name for p in BOOKMARK_DIR.glob('*.json')]}", file=sys.stderr)
        return 4

    base_out = Path(args.out_dir) if args.out_dir else (DEFAULT_OUT / args.mission)
    det_clock = not args.no_fixed_clock

    try:
        args.env_declare = list(args.env_declare) + env_pin.focus_declarations(args.focus, args.focus_zoom)
        declared_env = env_pin.parse_declarations(args.env_declare)
    except ValueError as e:
        print(f"[viscap] ERROR {e}", file=sys.stderr)
        return 4
    if declared_env:
        print(f"[viscap] env-declare: "
              f"{', '.join(f'{k}={v}' for k, v in sorted(declared_env.items()))}")

    # Resolve run plan. --verify is the legacy 2-run alias.
    n_runs = 2 if (args.verify and args.runs <= 1) else max(1, args.runs)
    n_warmup = max(0, args.warmup)
    if n_warmup >= n_runs:
        print(f"[viscap] ERROR --warmup {n_warmup} >= --runs {n_runs} "
              f"(nothing left to compare)", file=sys.stderr)
        return 4

    # Single-run mode: capture once, no stability claim.
    if n_runs == 1:
        result = run_one(exe, args.mission, bookmarks_path, base_out,
                         args.trigger_frame, args.settle, args.duration,
                         det_clock, args.show_window, declared_env, args.classic_assets)
        man = write_manifest(result, exe)
        _print_result(result, man)
        return 0 if not fail_reasons(result) else 1   # one predicate: the manifest, the log and the exit code agree

    # Multi-run stabilization mode.
    runs = []
    for i in range(1, n_runs + 1):
        # HARNESS-ISOLATION-1: honor the FULL out-dir leaf. Was
        # `base_out.parent / f"{mission}_r{i}"` which dropped the caller's leaf, so
        # gate `after/` + `before/` both collapsed to `base/{mission}_rN` and
        # clobbered each other. Now each owns `<out_dir>/rN`.
        out_i = base_out / f"r{i}"
        r = run_one(exe, args.mission, bookmarks_path, out_i,
                    args.trigger_frame, args.settle, args.duration, det_clock,
                    args.show_window, declared_env, args.classic_assets)
        write_manifest(r, exe)
        runs.append(r)
        time.sleep(2)

    compared = runs[n_warmup:]
    all_stable, per = compare_runs(compared)

    print(f"\n[viscap] === STABILITY ({n_runs} runs, {n_warmup} warmup "
          f"discarded; comparing runs {n_warmup + 1}..{n_runs}) ===")
    for name, info in per.items():
        shas = [(s or "MISSING")[:12] for s in info["shas"]]
        flag = "STABLE" if info["stable"] else "DRIFT"
        print(f"[viscap]   {name:20} {flag:7} {' '.join(shas)}")
    fired = all(r["engine_capture_fired"] for r in runs)
    # p31: the same predicate as single-run mode; a run whose engine capture did not fire proves nothing about stability.
    bad = [(i, r_) for i, r in enumerate(runs, 1) for r_ in fail_reasons(r)]
    if bad:
        for i, why in bad:
            print(f"[viscap] FAIL: run {i}: {why}; the stability result is void")
        return 1
    if not det_clock:
        print("[viscap]   NOTE: --no-fixed-clock set; drift is EXPECTED.")
    print(f"[viscap] STABILITY {'PASS (all bookmarks byte-stable post-warmup)' if all_stable else 'FAIL (residual drift)'}")

    n_stable = sum(1 for v in per.values() if v["stable"])
    if args.candidate_set:
        gate_ok = fired and (all_stable or (args.allow_partial and n_stable >= 1))
        if gate_ok:
            set_dir = materialize_candidate(args.candidate_set, runs[-1], per,
                                            exe, n_runs, n_warmup,
                                            allow_partial=args.allow_partial)
            kind = ("FULL" if all_stable
                    else f"PARTIAL ({n_stable}/{len(per)} stable)")
            print(f"[viscap] candidate set written ({kind}): {set_dir}")
            print(f"[viscap] set record: {set_dir / 'set.json'} (status=candidate)")
            if not all_stable:
                drifters = [n for n, v in per.items() if not v['stable']]
                print(f"[viscap] excluded_unstable: {drifters} "
                      "(known v2 items -- need engine streaming-freeze)")
            print("[viscap] BLESS = a human commit to set.json flipping "
                  "status->blessed with blessed_commit set.")
        else:
            print("[viscap] candidate NOT materialized (stability/fire gate "
                  "failed). Use --allow-partial to bless the stable subset, or "
                  "fix drift first.")
            return 1
    # Exit 0 when fully stable OR a partial candidate was successfully written.
    if all_stable:
        return 0
    if args.candidate_set and args.allow_partial and fired and n_stable >= 1:
        return 0
    return 1


def _print_result(result: dict, manifest_path: Path) -> None:
    print(f"[viscap] engine_capture_fired={result['engine_capture_fired']} "
          f"all_present={result['all_present']} "
          f"nondeterministic={result['any_nondeterministic']}")
    for b in result["bookmarks"]:
        sha = (b["sha256"] or "MISSING")[:16]
        det = b["engine_deterministic"]
        print(f"[viscap]   {b['name']:20} present={b['present']!s:5} "
              f"det={det!s:5} {sha} covers={b['covers']}")
    print(f"[viscap] manifest: {manifest_path}")
    print(f"[viscap] log:      {result['log']}")


if __name__ == "__main__":
    sys.exit(main())
