#!/usr/bin/env python3
"""VISUAL-VERIFY-PIPELINE-1 -- sequential bookmark-sweep capture.

Automates the "launch the release, replay every saved camera pose, collect the
deterministic PNGs" step that the manual verification fleet did by hand. It is a
thin sequential orchestrator over the already-hardened engine launcher in
scripts/run_visual_capture.py (own-PID reaping, cursor-parking against RTS
edge-scroll, MC2_LOG live stdout, deterministic fixed-timestep clock, per-pose
manifest). We do NOT re-implement the launch recipe -- we reuse it, one bookmark
JSON at a time, strictly sequentially (never two game instances at once).

Input: a release install directory containing:
    <install>/mc2.exe
    <install>/saved_bookmarks/*.json   (in-game "Save Camera Bookmark" output)

Each saved-bookmark JSON carries at least two distinct poses (the engine's
bookmark iterator silently fails on a single-pose file), so every file fires and
emits <mission>_<name>.png + a JSON sidecar.

Output layout (the capture dir):
    <capture-dir>/
      <bookmark>/               one dir per bookmark JSON
        <mission>_<name>.png    primary pose (the one verify.py grades)
        <mission>_<name>.json   engine sidecar (identity/determinism)
        <mission>_capture.log   engine stdout for this pose
      capture_run.json          sweep manifest (poses, shas, install identity)
      capture_run.log           this runner's own log

Lane discipline (enforced here):
  * Pre-flight: refuse to start if an mc2.exe launched from the TARGET install is
    already running (avoids clobbering a live game or another capture on the same
    exe). Foreign mc2.exe from OTHER installs are left untouched.
  * Own-PID only: the underlying runner reaps ONLY the children it launches.
  * Sequential: one bookmark at a time; the next launch waits for the previous
    child to fully exit.
  * MC2_LOG=1 + pre-created capture dirs: inherited from run_visual_capture.

Usage:
  py -3 tools/visual_verify/capture.py \
      --install "A:/Games/mc2-opengl/releases/mc2-win64-v0.5.1" \
      --bookmarks all --frame 431 \
      --out scratch/visual_verify/run_v0.5.1
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

_THIS = Path(__file__).resolve()
ROOT = _THIS.parents[2]  # repo root (tools/visual_verify/capture.py -> up 2)
sys.path.insert(0, str(_THIS.parent))  # sibling ab also resolves when capture is imported by a driver
sys.path.insert(0, str(ROOT / "scripts"))
import run_visual_capture as rvc  # noqa: E402  (the hardened launcher we build on)
import asset_parity  # noqa: E402  (ASSET-PARITY-1: refuse an install whose data/assets/*.fst drifted from v0.4)
import mc2_env_pin as env_pin  # noqa: E402  (CAPTURE-ENV-PIN-1 declared-env parsing)
import manifest_schema as ms  # noqa: E402  (unified identity + binary provenance)
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
# OWNER RULE: no window for any child process (hidden_kwargs: CREATE_NO_WINDOW + SW_HIDE).
from smoke_lib import nofocus  # noqa: E402
import bookmark_sets  # noqa: E402  (scripts/bookmark_sets.py: the shared bookmark-set resolver)


def _now_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def discover_bookmarks(install: Path) -> "list[Path]":
    bd = install / "saved_bookmarks"
    if not bd.is_dir():
        return []
    return sorted(p for p in bd.glob("*.json"))


def resolve_bookmarks(install: Path, wanted: "set[str] | None", out_dir: Path,
                      repo_dir: "Path | None" = None, ops_dir: "Path | None" = None) -> "tuple[list[Path], list[str], list[str]]":
    """The bookmark files to capture. Each wanted entry is a stem or a .json path; scripts/bookmark_sets.py resolves it:
    <install>/saved_bookmarks first, then tests/visual/bookmarks, then the ops dir (MC2_OPS_BOOKMARKS). The harness rewrites the
    bookmark json, so a file from outside the install is COPIED into <out_dir>/_repo_bookmarks/ (a dir this run owns) and never
    used in place; nothing is written into the install. Returns (files, log lines saying where each set came from, specs found
    nowhere). wanted=None means 'all' (install only)."""
    in_install = {p.stem: p for p in discover_bookmarks(install)}
    if wanted is None:
        return sorted(in_install.values()), [], []
    files, notes, missing = [], [], []
    for spec in sorted(wanted):
        stem = bookmark_sets.spec_stem(spec)
        src, _ = bookmark_sets.resolve_set(spec, install, repo_dir, ops_dir)
        if src is None:
            missing.append(spec)
            continue
        if src.parent == install / "saved_bookmarks":
            files.append(src)
            continue
        dst = Path(out_dir) / "_repo_bookmarks" / (stem + ".json")
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(src.read_bytes())
        files.append(dst)
        notes.append(f"bookmark {stem}: not in {install / 'saved_bookmarks'}; using {src} (copied to {dst})")
    return files, notes, missing


def exe_knows_env(exe: Path, keys) -> "dict[str, bool]":
    """key -> whether the exe's image contains the knob name at all (a knob the exe never registers is silently ignored)."""
    needles = {k: k.encode("ascii") for k in keys}
    found = {k: False for k in needles}
    if not needles:
        return found
    overlap = max(len(v) for v in needles.values())
    tail = b""
    with open(exe, "rb") as fh:
        while True:
            chunk = fh.read(1 << 24)
            if not chunk:
                break
            buf = tail + chunk
            for k, needle in needles.items():
                if not found[k] and needle in buf:
                    found[k] = True
            tail = buf[-overlap:]
    return found


def env_registration_problems(exe: Path, declared_env: dict, flag_keys) -> "tuple[list[str], list[str]]":
    """(fatal, warnings): declared MC2_* knobs whose name is not in the exe's image. A knob the exe never registers is silently
    ignored, so the capture would run without the effect. `flag_keys` are the knobs a capture.py flag itself implies
    (--clean, --live-settle-sec, --focus): the flag does nothing on this exe, so that is fatal; a knob from --env is a warning."""
    keys = sorted(k for k in declared_env if k.startswith("MC2_"))
    known = exe_knows_env(exe, keys)
    fatal, warn = [], []
    for k in keys:
        if known[k]:
            continue
        msg = f"{k} is not in {exe.name}'s image (the exe does not register that knob; it is silently ignored)"
        (fatal if k in flag_keys else warn).append(msg)
    return fatal, warn


def _running_mc2_from(install: Path) -> "list[tuple[int, str]]":
    """Return [(pid, exepath)] for any running mc2.exe whose image path is under
    `install`. Used as a lane-safety pre-flight (never kills anything)."""
    hits: list[tuple[int, str]] = []
    if os.name != "nt":
        return hits
    try:
        import subprocess
        # WMIC is deprecated but universally present; fall back to tasklist paths.
        out = subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             "Get-CimInstance Win32_Process -Filter \"Name='mc2.exe'\" | "
             "Select-Object ProcessId,ExecutablePath | ConvertTo-Json -Compress"],
            capture_output=True, text=True, timeout=30,
            **nofocus.hidden_kwargs())
        txt = (out.stdout or "").strip()
        if not txt:
            return hits
        data = json.loads(txt)
        if isinstance(data, dict):
            data = [data]
        target = str(install.resolve()).lower()
        for row in data:
            exep = (row.get("ExecutablePath") or "")
            pid = int(row.get("ProcessId") or 0)
            if exep and target in exep.replace("\\", "/").lower():
                hits.append((pid, exep))
    except Exception as e:
        print(f"[vv-capture] WARN mc2 pre-flight scan failed: {e}", file=sys.stderr)
    return hits


def engine_bookmark_load_failed(result: dict) -> bool:
    """True when the engine log says it could not open the bookmark file (ab.py side B on mc2_10, 11-10: the sweep read the file
    fine, then the engine's fopen of the same staged path failed once and the pose came back MISSING)."""
    if any(b.get("present") for b in result.get("bookmarks", [])):
        return False
    try:
        return "bookmark load FAILED" in Path(result["log"]).read_text(encoding="utf-8", errors="replace")
    except (KeyError, TypeError, OSError):
        return False


def run_pose(log=print, retries: int = 1, run_one=None, **kw) -> dict:
    """rvc.run_one, run again (up to `retries` times) when the engine reports 'bookmark load FAILED' for a file this process can read."""
    run_one = run_one or rvc.run_one
    bmk = Path(kw["bookmarks_path"])
    result = run_one(**kw)
    for attempt in range(1, retries + 1):
        if not engine_bookmark_load_failed(result):
            break
        log(f"    engine could not load {bmk} (readable here: {bmk.is_file()}); retry {attempt}/{retries}")
        result = run_one(**kw)
    return result


def capture_sweep(install: Path, bookmark_files: "list[Path]", out_dir: Path,
                  frame: int, settle: int, duration: int | None,
                  det_clock: bool, show_window: bool,
                  logf, declared_env: "dict | None" = None) -> dict:
    from ab import game_executable, payload_identity
    exe = game_executable(install)
    install_identity = payload_identity(install)
    out_dir.mkdir(parents=True, exist_ok=True)

    def log(msg: str) -> None:
        line = f"[vv-capture] {msg}"
        print(line)
        logf.write(line + "\n")
        logf.flush()

    # MC2_NO_LAUNCHER matches the documented capture recipe; run_visual_capture
    # inherits os.environ, so set it here (and MC2_LOG is set inside the runner).
    os.environ["MC2_NO_LAUNCHER"] = "1"

    results = rvc.batch_capture(
        bookmark_files, out_dir, exe=exe, trigger_frame=frame, settle=settle,
        duration=duration, deterministic_clock=det_clock, show_window=show_window,
        declared_env=declared_env)
    poses: list[dict] = []
    for i, bmk in enumerate(bookmark_files, 1):
        stem = bmk.stem
        pose_dir = out_dir / stem
        result = results[bmk]
        dt = result["seconds"]
        # Pick the PRIMARY pose PNG (the non-dup one) for grading.
        primary = None
        for b in result.get("bookmarks", []):
            if b.get("present") and not b["name"].endswith("_dup"):
                primary = b
                break
        if primary is None:
            # fall back to any present pose
            primary = next((b for b in result.get("bookmarks", [])
                            if b.get("present")), None)
        poses.append({
            "bookmark": stem,
            "mission": result.get("mission"),
            "pose_dir": str(pose_dir),
            "primary_png": primary["png"] if primary else None,
            "primary_sha256": primary["sha256"] if primary else None,
            "engine_deterministic": primary.get("engine_deterministic") if primary else None,
            "all_present": result.get("all_present"),
            "engine_capture_fired": result.get("engine_capture_fired"),
            "render_options": (result.get("render_options") or {}).get("values"),
            "render_options_disagree": (result.get("render_options") or {}).get("disagree") or [],
            "backend_requested": (result.get("backend") or {}).get("requested"),
            "backend_drew": (result.get("backend") or {}).get("drew"),
            "backend_disagree": (result.get("backend") or {}).get("disagree") or [],
            "seconds": round(dt, 1),
            "log": result.get("log"),
            "harness_set": (result.get("mc2_env") or {}).get("harness_set") or {},
        })
        ok = "OK" if (primary and primary.get("present")) else "MISSING"
        log(f"    {ok}  fired={result.get('engine_capture_fired')} "
            f"det={primary.get('engine_deterministic') if primary else None} "
            f"{dt:.1f}s")

    # CAPTURE-PROVENANCE-TRUTH-1: this manifest used to record only `exe` and
    # `exe_mtime`. An mtime is not an identity -- the moment that file is
    # rebuilt, every PNG in this sweep becomes unattributable, and nothing in
    # the file said which commit it exercised. Emit the unified identity block
    # so the sweep carries the binary's sha256 AND the binary's own embedded
    # build fingerprint (the only field that describes the code that ran).
    # Legacy keys stay for back-compat.
    manifest = {
        "install_identity": install_identity,
        "kind": "visual-verify-capture",
        "schema_v": 2,
        "generated_at": _now_iso(),
        "install": str(install.resolve()),
        "exe": str(exe.resolve()),
        "exe_mtime": _exe_mtime(exe),
        "frame": frame,
        "settle": settle,
        "duration": duration,
        "deterministic_clock": det_clock,
        "declared_env": declared_env or {},
        "poses": poses,
        # The options every pose of this sweep ran with (one install, one environment); capture_ab compares them between sets.
        "render_options": next((p["render_options"] for p in poses if p.get("render_options")), None),
        # The backend the sweep asked for and the backends that drew it (every pose's value, sorted: one entry unless the sweep was mixed); a
        # mismatch fails the sweep (a Vulkan request that fell back to GL).
        "backend": {"requested": next((p["backend_requested"] for p in poses if p.get("backend_requested")), None),
                    "drew": sorted({d for p in poses for d in _as_list(p.get("backend_drew"))})},
        "n_captured": sum(1 for p in poses if p["primary_png"]),
        "n_requested": len(bookmark_files),
        # What the harness itself set (determinism seed and clock, size, capture switches), so a golden shows the conditions its frames were
        # drawn under; the per-pose paths are left out because they name the sweep's own directories.
        "harness_env": _harness_env(poses),
        "effective_mc2_env": sorted({k for p in poses for k in p.get("harness_set", {})} | set(declared_env or {})),
    }
    ms.attach(manifest, ms.identity_block(
        generator="visual_verify_capture",
        exe_path=exe,
        repo_root=str(ROOT),
        deploy_target=str(install.resolve()),
    ))
    (out_dir / "capture_run.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    return manifest


def _as_list(value) -> list:
    """`drew` is a list of backend names (a string from an older record counts as one)."""
    return [] if not value else [value] if isinstance(value, str) else list(value)


_PER_POSE_PATH_KEYS = ("MC2_VISUAL_BOOKMARK_CAPTURE", "MC2_VISUAL_CAPTURE_DIR")


def _harness_env(poses: list) -> dict:
    """The harness-set MC2_* values shared by the sweep (first pose that recorded them), without the per-pose path keys."""
    first = next((p["harness_set"] for p in poses if p.get("harness_set")), {})
    return {k: v for k, v in sorted(first.items()) if k not in _PER_POSE_PATH_KEYS}


def backend_failures(manifest: dict) -> "list[str]":
    """Every pose whose sidecar is not from the render backend the sweep asked for (a Vulkan request that fell back to GL)."""
    return [m for p in manifest["poses"] for m in p.get("backend_disagree") or []]


def _exe_mtime(exe: Path) -> "str | None":
    try:
        return datetime.fromtimestamp(exe.stat().st_mtime, timezone.utc).strftime(
            "%Y-%m-%dT%H:%M:%SZ")
    except Exception:
        return None


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--install", required=True,
                    help="release install dir (contains mc2.exe + saved_bookmarks/)")
    ap.add_argument("--bookmarks", default="all",
                    help="'all' or a comma-separated list of bookmark stems "
                         "(e.g. mc2_10_saved_204523,mc2_01_saved_205644)")
    ap.add_argument("--out", required=True, help="capture output dir")
    ap.add_argument("--frame", type=int, default=431,
                    help="deterministic trigger frame (default 431, the frame the "
                         "verification fleet graded at)")
    ap.add_argument("--settle", type=int, default=30)
    ap.add_argument("--duration", type=int, default=None,
                    help="per-bookmark timeout in seconds (default: trigger-aware launcher budget; "
                         "ends early when every PNG and sidecar is complete)")
    ap.add_argument("--clean", action="store_true",
                    help="VISUAL-CAPTURE-CLEAN-1: declare MC2_VISUAL_CAPTURE_CLEAN=1 "
                         "(suppress HUD + movie-mode letterbox/fade on BOTH backends) "
                         "so GL and VK captures are pixel-comparable. Default OFF "
                         "keeps GL captures byte-identical to before.")
    ap.add_argument("--live-settle-sec", type=float, default=0.0, metavar="N",
                    help="CAPTURE-LIVE-CLOCK: at each bookmark pose keep the sim clock running for N "
                         "sim-seconds, then freeze and capture (time-driven effects such as water-reactive "
                         "ripples get age > 0). Default 0 = sim frozen at the trigger, captures unchanged. "
                         "Raise --duration for large N.")
    ap.add_argument("--no-fixed-clock", action="store_true",
                    help="disable the deterministic clock (captures NOT byte-stable)")
    ap.add_argument("--show-window", action="store_true")
    ap.add_argument("--focus", metavar="mech[:N]", default=None,
                    help="MC2_CAPTURE_FOCUS: move every bookmark pose onto the Nth live "
                         "player-team mech (default 0); the bookmark still supplies rot/proj.")
    ap.add_argument("--focus-zoom", type=float, default=None, metavar="ALT",
                    help="camera altitude with --focus (default 200 = mech at about a fifth of the frame)")
    ap.add_argument("--env-declare", "--env", dest="env_declare", action="append",
                    default=[], metavar="MC2_KEY=VALUE",
                    help="VK-VISUAL-CAPTURE-1: opt a MC2_* var into the capture "
                         "environment (repeatable), e.g. --env "
                         "MC2_RENDER_BACKEND=vulkan --env MC2_VK_SHARED_GBUFFER=1. "
                         "Same CAPTURE-ENV-PIN-1 mechanism as run_visual_capture.")
    ap.add_argument("--allow-running", action="store_true",
                    help="skip the pre-flight refusal when an mc2.exe from the "
                         "target install is already running (NOT recommended)")
    ap.add_argument("--allow-asset-drift", action="store_true",
                    help="capture although the install's data/assets/*.fst differ from "
                         "v0.4 (default: refuse, the capture would grade old art)")
    args = ap.parse_args(argv)

    install = Path(args.install).resolve()
    from ab import game_executable
    exe = game_executable(install)
    if not exe.exists():
        print(f"[vv-capture] ERROR mc2.exe missing: {exe}", file=sys.stderr)
        return 4
    if not rvc.guard_exe(exe):
        return 7
    drift = asset_parity.capture_drift(install)
    if drift is not None:
        print(f"[vv-capture] {asset_parity.verdict_line(drift, asset_parity.default_ref())}", file=sys.stderr)
        if not drift.ok and not args.allow_asset_drift:
            print(f"[vv-capture] ABORT: {len(drift.missing)} missing, {len(drift.size)} size-different, "
                  f"{len(drift.content)} content-different asset file(s) vs {asset_parity.default_ref()}; "
                  "redeploy (deploy_payload syncs it) or pass --allow-asset-drift.", file=sys.stderr)
            return 8

    out_dir = Path(args.out).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    wanted = None if args.bookmarks.strip().lower() == "all" else {x.strip() for x in args.bookmarks.split(",") if x.strip()}
    files, bookmark_notes, missing = resolve_bookmarks(install, wanted, out_dir)
    if wanted is None and not files:
        print(f"[vv-capture] ERROR no saved_bookmarks/*.json under {install}",
              file=sys.stderr)
        return 4
    if missing:
        print(f"[vv-capture] ERROR unknown bookmark(s): {sorted(missing)} (not in {install / 'saved_bookmarks'} "
              f"nor the repo/ops dirs)", file=sys.stderr)
        for spec in sorted(missing):
            print("[vv-capture] " + bookmark_sets.not_found_message(spec, bookmark_sets.search_dirs(install)), file=sys.stderr)
        print(f"[vv-capture] available: {sorted(p.stem for p in discover_bookmarks(install))}", file=sys.stderr)
        return 4
    for line in bookmark_notes:
        print(f"[vv-capture] {line}", flush=True)

    # Lane-safety pre-flight: refuse if the TARGET exe is already live.
    running = _running_mc2_from(install)
    if running and not args.allow_running:
        print(f"[vv-capture] ABORT: mc2.exe already running from target install "
              f"{install}:", file=sys.stderr)
        for pid, p in running:
            print(f"[vv-capture]   pid={pid} {p}", file=sys.stderr)
        print("[vv-capture] close it (or pass --allow-running) before capturing.",
              file=sys.stderr)
        return 3

    logf = open(out_dir / "capture_run.log", "w", encoding="utf-8", errors="replace")
    # fsr_dll= is the capture-time environment record merge_gate needs to call a missing-FSR pipeline INFRA (p31).
    fsr_dll = "present" if (install / "amd_fidelityfx_vk.dll").is_file() else "missing"
    logf.write(f"[vv-capture] start {_now_iso()} install={install} fsr_dll={fsr_dll} "
               f"bookmarks={len(files)} frame={args.frame}\n")

    for line in bookmark_notes:
        logf.write(f"[vv-capture] {line}\n")
    # Restore desktop cursor on exit (the sweep parks it center for determinism).
    import atexit
    rvc._save_cursor()
    atexit.register(rvc._restore_cursor)
    atexit.register(rvc._reap_active)

    if args.clean:
        args.env_declare.append("MC2_VISUAL_CAPTURE_CLEAN=1")
    if args.live_settle_sec > 0:
        args.env_declare.append(f"MC2_VISUAL_LIVE_SETTLE_SEC={args.live_settle_sec:g}")
    try:
        args.env_declare = list(args.env_declare) + env_pin.focus_declarations(args.focus, args.focus_zoom)
        declared_env = env_pin.parse_declarations(args.env_declare)
    except ValueError as e:
        print(f"[vv-capture] ERROR {e}", file=sys.stderr)
        return 4

    flag_keys = ({"MC2_VISUAL_CAPTURE_CLEAN"} if args.clean else set()) | (
        {"MC2_VISUAL_LIVE_SETTLE_SEC"} if args.live_settle_sec > 0 else set()) | (
        {"MC2_CAPTURE_FOCUS"} if args.focus is not None else set())
    fatal, warn = env_registration_problems(exe, declared_env, flag_keys)
    for m in warn:
        print(f"[vv-capture] WARNING !!! {m}", file=sys.stderr)
        logf.write(f"[vv-capture] WARNING {m}\n")
    if fatal:
        for m in fatal:
            print(f"[vv-capture] ERROR {m}", file=sys.stderr)
            logf.write(f"[vv-capture] ERROR {m}\n")
        print("[vv-capture] ERROR the capture would run without that effect; build the exe from a base that has the knob "
              "(or drop the flag)", file=sys.stderr)
        logf.close()
        return 4

    manifest = capture_sweep(
        install, files, out_dir,
        frame=args.frame, settle=args.settle, duration=args.duration,
        det_clock=not args.no_fixed_clock, show_window=args.show_window, logf=logf,
        declared_env=declared_env or None)
    logf.close()

    print(f"\n[vv-capture] captured {manifest['n_captured']}/{manifest['n_requested']} "
          f"bookmarks -> {out_dir}")
    print(f"[vv-capture] manifest: {out_dir / 'capture_run.json'}")
    n_det = sum(1 for p in manifest["poses"] if p.get("engine_deterministic") is True)
    print(f"[vv-capture] deterministic poses: {n_det}/{manifest['n_captured']}")
    bad_backend = backend_failures(manifest)
    if bad_backend:
        for m in bad_backend:
            print(f"[vv-capture] FAIL render backend: {m}")
        print("[vv-capture] FAIL: the sweep was not drawn by the requested render backend; its PNGs are not evidence for that backend")
        return 1
    if any(p.get("render_options_disagree") for p in manifest["poses"]):
        print("[vv-capture] FAIL: the engine's render options differ from the harness reading of the install (see the log above)")
        return 1
    return 0 if manifest["n_captured"] == manifest["n_requested"] else 1


if __name__ == "__main__":
    sys.exit(main())
