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
      --exe "A:/Games/mc2-opengl/releases/mc2-win64-v0.4d-rc1/mc2.exe"
  py -3 scripts/run_visual_capture.py --mission mc2_01 --verify   # 2x, byte-diff
"""
from __future__ import annotations

import argparse
import atexit
import json
import os
import subprocess
import sys
import time
from pathlib import Path

# WATER-VISUAL-GATE-1 concurrent-safety:
#   _SKIP_KILL  — when True, do NOT taskkill foreign mc2.exe (set by --no-kill).
#   _ORIG_CURSOR — saved desktop cursor pos, restored at exit so the harness
#                  never leaves the mouse permanently warped.
_SKIP_KILL = False
_ORIG_CURSOR = None
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

DEFAULT_EXE = Path(r"A:/Games/mc2-opengl/releases/mc2-win64-v0.4d-rc1/mc2.exe")
BOOKMARK_DIR = (Path(os.environ["MC2_VISUAL_BOOKMARK_DIR"])
                if os.environ.get("MC2_VISUAL_BOOKMARK_DIR")
                else ROOT / "tests" / "visual" / "bookmarks")
DEFAULT_OUT = ROOT / "tests" / "visual" / "captures"
GOLDENS_DIR = ROOT / "tests" / "visual" / "baselines"
GOLDEN_SETS_REGISTRY = ROOT / "tests" / "visual" / "golden-sets.json"


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
    """WATER-VISUAL-GATE-1: snapshot the desktop cursor once so we can restore it."""
    global _ORIG_CURSOR
    if os.name != "nt" or _ORIG_CURSOR is not None:
        return
    try:
        import ctypes
        pt = ctypes.wintypes.POINT() if hasattr(ctypes, "wintypes") else None
        if pt is None:
            import ctypes.wintypes
            pt = ctypes.wintypes.POINT()
        ctypes.windll.user32.GetCursorPos(ctypes.byref(pt))
        _ORIG_CURSOR = (int(pt.x), int(pt.y))
    except Exception as e:
        print(f"[viscap] WARN save cursor: {e}", file=sys.stderr)


def _restore_cursor() -> None:
    """Restore the cursor to where it was before the sweep parked it."""
    if os.name != "nt" or _ORIG_CURSOR is None:
        return
    try:
        import ctypes
        ctypes.windll.user32.SetCursorPos(_ORIG_CURSOR[0], _ORIG_CURSOR[1])
    except Exception as e:
        print(f"[viscap] WARN restore cursor: {e}", file=sys.stderr)


def park_cursor_center() -> None:
    """Pin the OS cursor to screen center.

    LOAD-BEARING for determinism: the MC2 RTS camera edge-scrolls whenever the
    desktop cursor sits at a screen edge -- regardless of window focus or
    whether mc2.exe is foreground/minimized (memory: smoke_autonomous_run_pattern
    fact #2). The bookmark sweep re-applies the pose each settle frame, but
    edge-scroll adds a per-frame camera delta on top, so an un-parked cursor
    makes wide/high-altitude framings drift nondeterministically (it "sometimes
    matches" only when the cursor happens to land off-edge). Parking center kills
    the edge-scroll input entirely. Dependency-free (Win32 SetCursorPos); no-op
    off-Windows.
    """
    if os.name != "nt":
        return
    _save_cursor()  # WATER-VISUAL-GATE-1: snapshot once before the first park
    try:
        import ctypes
        user32 = ctypes.windll.user32
        w = user32.GetSystemMetrics(0)  # SM_CXSCREEN
        h = user32.GetSystemMetrics(1)  # SM_CYSCREEN
        user32.SetCursorPos(w // 2, h // 2)
    except Exception as e:
        print(f"[viscap] WARN park cursor: {e}", file=sys.stderr)


def run_one(exe: Path, mission: str, bookmarks_path: Path, out_dir: Path,
            trigger_frame: int, settle: int, duration: int,
            deterministic_clock: bool) -> dict:
    """Launch one capture sweep; return a result dict (never raises on engine
    failure -- records it instead)."""
    # The engine runs with cwd = the deploy dir and resolves
    # MC2_VISUAL_CAPTURE_DIR relative to THAT, so a relative out_dir would write
    # under the deploy (and fopen-fail if absent). Always pass an absolute path.
    out_dir = Path(out_dir).resolve()
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

    _kill_existing_mc2()
    park_cursor_center()   # before launch: no edge-scroll from a stray cursor

    log_path = out_dir / f"{mission_token}_capture.log"
    log_fp = open(log_path, "w", encoding="utf-8", errors="replace")

    env = os.environ.copy()
    env["MC2_SMOKE_MODE"] = "1"
    # MC2_LOG master gate: keep stdout live (engine default redirects it -> NUL
    # so normal play never hitches on the unbuffered printf flush). Capture
    # detection reads [VISUAL_CAPTURE] from stderr, but this keeps capture-run
    # behavior identical to pre-gate. See gameosmain.cpp after setvbuf.
    env["MC2_LOG"] = "1"
    env["MC2_SMOKE_SEED"] = "0xC0FFEE"
    if deterministic_clock:
        env["MC2_SMOKE_FIXED_TIMESTEP"] = "1"
    env["MC2_HEARTBEAT"] = "1"
    env["MC2_VISUAL_BOOKMARK_CAPTURE"] = str(bookmarks_path)
    env["MC2_VISUAL_CAPTURE_DIR"] = str(out_dir)
    env["MC2_VISUAL_CAPTURE_FRAME"] = str(trigger_frame)
    env["MC2_VISUAL_SETTLE"] = str(settle)

    startupinfo = None
    if os.name == "nt":
        startupinfo = subprocess.STARTUPINFO()
        startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startupinfo.wShowWindow = 7  # SW_SHOWMINNOACTIVE -- do not steal focus

    proc_args = [str(exe), "--profile", "stock", "--mission", mission_token,
                 "--duration", str(duration)]
    print(f"[viscap] mission={mission_token} bookmarks={len(marks)} "
          f"trigger={trigger_frame} settle={settle} dur={duration}s "
          f"fixed_clock={deterministic_clock}")
    t0 = time.time()
    proc = subprocess.Popen(proc_args, stdout=log_fp, stderr=subprocess.STDOUT,
                            cwd=str(exe.parent), env=env,
                            startupinfo=startupinfo)
    _ACTIVE.append(proc)  # HARNESS-ISOLATION-1: track for own-only reap

    # Poll for all expected PNGs, or process exit, or timeout. Re-park the
    # cursor every iteration so it stays pinned center through the ENTIRE sweep
    # window (trigger frame + per-bookmark settle) -- a single pre-launch park
    # is not enough if the cursor moves during the run.
    deadline = t0 + duration + 30
    while time.time() < deadline:
        park_cursor_center()
        if all(p.exists() for p in expected_pngs.values()):
            # Give the engine a beat to finish the last sidecar write.
            time.sleep(1.0)
            break
        if proc.poll() is not None:
            break
        time.sleep(0.5)

    # Drain the process.
    try:
        remaining = max(1, (t0 + duration + 15) - time.time())
        proc.wait(timeout=remaining)
    except subprocess.TimeoutExpired:
        proc.kill()
        try:
            proc.wait(timeout=10)
        except Exception:
            pass
    log_fp.close()

    # Collect results per bookmark.
    bm_results = []
    all_present = True
    any_nondet = False
    for m in marks:
        png = expected_pngs[m["name"]]
        side = png.with_suffix(".json")
        present = png.exists()
        all_present = all_present and present
        sha = ms.file_sha256(png) if present else None
        det = None
        if side.exists():
            try:
                det = json.loads(side.read_text(encoding="utf-8")).get("deterministic")
            except Exception:
                det = None
        if det is False:
            any_nondet = True
        bm_results.append({
            "name": m["name"],
            "png": str(png),
            "present": present,
            "sha256": sha,
            "covers": m.get("covers", []),
            "engine_deterministic": det,
        })

    # Detect whether the engine's capture path even fired (RC may predate S9).
    fired = False
    try:
        logtxt = log_path.read_text(encoding="utf-8", errors="replace")
        fired = "[VISUAL_CAPTURE" in logtxt
    except Exception:
        pass

    return {
        "mission": mission_token,
        "bookmarks_path": str(bookmarks_path),
        "out_dir": str(out_dir),
        "all_present": all_present,
        "engine_capture_fired": fired,
        "any_nondeterministic": any_nondet,
        "bookmarks": bm_results,
        "log": str(log_path),
    }


def write_manifest(result: dict, exe: Path) -> Path:
    out_dir = Path(result["out_dir"])
    verdict = ("PASS" if result["all_present"] and result["engine_capture_fired"]
               else "FAIL")
    ident = ms.identity_block(
        generator="visual_capture",
        exe_path=exe,
        repo_root=str(ROOT),
        deploy_target=str(exe.parent),
        env_keys=["MC2_SMOKE_MODE", "MC2_SMOKE_SEED", "MC2_SMOKE_FIXED_TIMESTEP",
                  "MC2_VISUAL_CAPTURE_FRAME", "MC2_VISUAL_SETTLE"],
        extra={"bookmarks_path": result["bookmarks_path"]},
    )
    rep = ms.report_summary(
        verdict=verdict,
        missions=[result["mission"]],
        artifact_dir=str(out_dir),
        extra={
            "engine_capture_fired": result["engine_capture_fired"],
            "all_bookmarks_present": result["all_present"],
            "any_nondeterministic": result["any_nondeterministic"],
            "bookmarks": result["bookmarks"],
        },
    )
    manifest = ms.attach({"kind": "visual-capture"}, ident, rep)
    path = out_dir / f"{result['mission']}_capture_manifest.json"
    path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
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
    and register the set in tests/visual/golden-sets.json as status=candidate.

    Per visual-regression-lab-architecture.md §3: a set is captured as
    'candidate', reviewed by a human walking the gallery, then 'blessed' by a
    COMMIT to golden-sets.json. This writes the candidate; the human blesses.

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
        env_keys=["MC2_SMOKE_MODE", "MC2_SMOKE_SEED", "MC2_SMOKE_FIXED_TIMESTEP",
                  "MC2_VISUAL_CAPTURE_FRAME", "MC2_VISUAL_SETTLE"],
    )
    set_record = {
        "set_id": set_name,
        "status": "candidate",          # human blesses via a golden-sets.json commit
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
        "bookmarks": members,
        "excluded_unstable": excluded,
    }
    # Write the per-set record next to the PNGs (the lab's set.json).
    (set_dir / "set.json").write_text(json.dumps(set_record, indent=2) + "\n",
                                      encoding="utf-8")

    # Upsert into the registry (append-only; never delete a prior set).
    registry = {}
    if GOLDEN_SETS_REGISTRY.exists():
        try:
            registry = json.loads(GOLDEN_SETS_REGISTRY.read_text(encoding="utf-8"))
        except Exception:
            registry = {}
    registry.setdefault("schema_v", 1)
    sets = registry.setdefault("sets", {})
    sets[set_name] = {k: set_record[k] for k in
                      ("set_id", "status", "blessed_commit", "supersedes",
                       "mission", "path", "stabilization", "identity")}
    GOLDEN_SETS_REGISTRY.write_text(json.dumps(registry, indent=2) + "\n",
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
    ap.add_argument("--out-dir", default=None,
                    help="output dir (default tests/visual/captures/<mission>)")
    ap.add_argument("--trigger-frame", type=int, default=120)
    ap.add_argument("--settle", type=int, default=30)
    ap.add_argument("--duration", type=int, default=45)
    ap.add_argument("--no-fixed-clock", action="store_true",
                    help="DISABLE MC2_SMOKE_FIXED_TIMESTEP (captures will NOT be "
                         "byte-stable across runs; use only for eyeball checks)")
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
                         "tests/visual/baselines/<id>/ and register it in "
                         "tests/visual/golden-sets.json as status=candidate.")
    ap.add_argument("--allow-partial", action="store_true",
                    help="materialize the STABLE bookmark subset even if some "
                         "bookmarks drift (the drifters are recorded as "
                         "excluded_unstable). A small-but-real baseline. "
                         "Requires >=1 stable bookmark + engine fired.")
    ap.add_argument("--no-kill", action="store_true",
                    help="WATER-VISUAL-GATE-1: do NOT taskkill foreign mc2.exe "
                         "(concurrent-safe). The capture still launches its own "
                         "child exe; only the global taskkill is skipped.")
    args = ap.parse_args()

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
    bookmarks_path = (Path(args.bookmarks) if args.bookmarks
                      else BOOKMARK_DIR / f"{args.mission}.json")
    if not bookmarks_path.exists():
        print(f"[viscap] ERROR bookmarks missing: {bookmarks_path}", file=sys.stderr)
        print(f"[viscap] available: "
              f"{[p.name for p in BOOKMARK_DIR.glob('*.json')]}", file=sys.stderr)
        return 4

    base_out = Path(args.out_dir) if args.out_dir else (DEFAULT_OUT / args.mission)
    det_clock = not args.no_fixed_clock

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
                         args.trigger_frame, args.settle, args.duration, det_clock)
        man = write_manifest(result, exe)
        _print_result(result, man)
        return 0 if (result["all_present"] and result["engine_capture_fired"]) else 1

    # Multi-run stabilization mode.
    runs = []
    for i in range(1, n_runs + 1):
        # HARNESS-ISOLATION-1: honor the FULL out-dir leaf. Was
        # `base_out.parent / f"{mission}_r{i}"` which dropped the caller's leaf, so
        # gate `after/` + `before/` both collapsed to `base/{mission}_rN` and
        # clobbered each other. Now each owns `<out_dir>/rN`.
        out_i = base_out / f"r{i}"
        r = run_one(exe, args.mission, bookmarks_path, out_i,
                    args.trigger_frame, args.settle, args.duration, det_clock)
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
    if not fired:
        print("[viscap]   WARNING: engine capture path did not fire in some run "
              "-- RC may predate S9. Check the .log.")
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
            print(f"[viscap] registry: {GOLDEN_SETS_REGISTRY} (status=candidate)")
            if not all_stable:
                drifters = [n for n, v in per.items() if not v['stable']]
                print(f"[viscap] excluded_unstable: {drifters} "
                      "(known v2 items -- need engine streaming-freeze)")
            print("[viscap] BLESS = a human commit to golden-sets.json flipping "
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
