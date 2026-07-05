#!/usr/bin/env python3
"""run_golden_parity.py -- GOLDEN-SCENE-PARITY-1 orchestration wrapper.

The proof-loop driver that validates a real render island (e.g. a Vulkan island
swapped in behind a gate) against the GL baseline. It is the ONE-LINER a future
island slice runs to *prove* pixel + structural parity instead of eyeballing.

It orchestrates three already-shipped pieces:

  * run_visual_capture.py -- the PIXEL ORACLE.  Deterministic, bookmark-driven,
    glReadPixels FBO readback, byte-stable per-bookmark sha256 under
    MC2_SMOKE_FIXED_TIMESTEP=1.  This -- NOT golden_scene.py's fly-through
    pixel_hash -- is what catches a real island shading difference, because the
    camera is a PINNED bookmark pose (no fly-through drift) and the readback is
    off an FBO at a fixed deterministic frame.
  * golden_scene.py       -- the STRUCTURAL manifest (registry_hash,
    pass_counters, render_health, exe_md5, gate_set).  Its own pixel_hash is a
    fly-through best-effort fingerprint and is NOT trusted here for pixels;
    only its deterministic structural fields are.
  * golden_compare.py / golden_diff_report.py -- noise-floor classification +
    culprit-field/pass diagnosis over a COMBINED manifest we synthesize below.

WHY run_visual_capture as the pixel oracle (not golden_scene):
  golden_scene.py itself documents that its pixel_hash is captured at a fixed
  EARLY fly-through frame and is "best-effort ... pixel-EXACT stability requires
  a fixed camera (a follow-up slice)".  run_visual_capture.py IS that fixed
  camera: bookmark poses + parked cursor (kills RTS edge-scroll) + fixed-step
  clock -> the engine stamps each sidecar `deterministic:true` and the per-
  bookmark PNG sha256 is byte-identical run-to-run.  A render island changes
  PIXELS, so the oracle must be the sharpest deterministic pixel path we have.

COMBINED MANIFEST (what we floor/compare):
  For a state we run BOTH captures and emit one JSON that carries:
    * from golden_scene: registry_hash, pass_counters, render_health, exe_md5,
      gate_set, build_config  (deterministic structural fields)
    * from run_visual_capture: per-bookmark sha256 map under "pixel_shas" and an
      aggregate "pixel_sha_all" (FNV-ish join) -- the load-bearing pixel oracle,
      EXACT-comparable.
  golden_compare then floors these: registry_hash / exe_md5 / pass_counters /
  each pixel_sha land EXACT; anything that legitimately drifts (frame count,
  epoch) lands DRIFT/ignore.  A pixel_sha that changes between OFF and ON is an
  EXACT-field-changed BEYOND -> parity FAIL with the culprit bookmark named.

DETERMINISM / RESIDUAL DRIFT ENVELOPE:
  We always run captures with MC2_SMOKE_FIXED_TIMESTEP=1 (via
  run_visual_capture's default fixed clock; do NOT pass --no-fixed-clock).  With
  the fixed clock + parked cursor the per-bookmark sha256 is byte-stable in the
  common case.  Residual drift that CAN still appear (documented, treated as
  the noise floor): first-run shader-compile / texture-residency / static-prop
  streaming / PBR first-use cache warmup -- which is exactly why the floor is
  built from N>=2 OFF captures and why run_visual_capture recommends
  --runs 3 --warmup 1.  Bookmarks that still drift OFF-vs-OFF are recorded by
  the floor as non-exact and cannot then be used to fail an ON candidate; if
  ALL pixel bookmarks drift OFF-vs-OFF the harness says so loudly (the oracle is
  too blunt for this scene -- see the RESULT line and the docstring caveat).

EXIT: 0 iff A(gate OFF) vs B(gate ON) is WITHIN the established floor for every
field INCLUDING every exact pixel_sha; nonzero otherwise, printing the culprit
fields + the diff-report path.

Pure stdlib, Windows / py-3.  Captures run against the v0.4 deploy.
NEVER --kill-existing (run_visual_capture reaps only its own children).

Usage:
  # one-shot parity check of a gate that changes pixels:
  py -3 scripts/run_golden_parity.py mc2_01 MC2_MY_ISLAND_GATE
  # reuse a committed floor instead of rebuilding it:
  py -3 scripts/run_golden_parity.py mc2_01 MC2_MY_ISLAND_GATE \
      --noise-floor golden_floors/mc2_01.json
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / "scripts"
GOLDEN_SCENE = SCRIPTS / "golden_scene.py"
VISUAL_CAPTURE = SCRIPTS / "run_visual_capture.py"
GOLDEN_COMPARE = SCRIPTS / "golden_compare.py"
GOLDEN_DIFF = SCRIPTS / "golden_diff_report.py"
DEFAULT_EXE = Path(r"A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe")
FLOORS_DIR = ROOT / "golden_floors"
# Per-capture wall budget: a smoke/visual sweep is ~30-45s; give generous margin.
CAP_TIMEOUT_S = 180


def _run(cmd: list[str], env: dict | None = None, timeout: int = CAP_TIMEOUT_S) -> int:
    """Shell a child python tool; stream nothing, just return exit code.
    Raises on timeout (a hung/crashed capture must abort the harness clearly)."""
    print("[golden_parity] $ %s" % " ".join(cmd), file=sys.stderr)
    try:
        return subprocess.call(cmd, cwd=str(ROOT), env=env, timeout=timeout)
    except subprocess.TimeoutExpired:
        raise RuntimeError("capture TIMED OUT after %ds: %s"
                           % (timeout, " ".join(cmd)))


def capture_state(label: str, scene: str, exe: Path, work: Path,
                  gate_name: str | None, gate_value: str | None,
                  fixed_timestep: bool,
                  backend: str | None = None,
                  region: str | None = None) -> dict:
    """Run BOTH the visual-capture pixel oracle and the golden_scene structural
    manifest for ONE state (gate OFF or ON, or a backend selection), then
    synthesize a combined manifest.
    Returns the combined manifest dict (also written to work/<label>_combined.json).

    label: 'A' / 'B' / 'floor0' ... (namespaces artifacts).
    gate_name/value: the island gate to set for THIS state (None => OFF/unset).
    backend: BACKEND-COMPARE mode. When set ('gl'|'vk'), the state is defined by
      MC2_RENDER_BACKEND_REGION_IFACE=1 + MC2_POSTPROCESS_BACKEND=<backend>
      (region-selectable PostprocessFog), NOT by a single feature gate. region
      names the region-under-test (default PostprocessFog) for reporting.
    """
    state_dir = work / label
    state_dir.mkdir(parents=True, exist_ok=True)

    # Base env for both children.
    env = os.environ.copy()
    if backend is not None:
        # BACKEND-COMPARE: the state is "which backend implements the region",
        # not a feature-gate on/off. Enable the region interface + pick backend.
        env["MC2_RENDER_BACKEND_REGION_IFACE"] = "1"
        env["MC2_POSTPROCESS_BACKEND"] = backend
    elif gate_name:
        # FEATURE-GATE mode: OFF and ON differ only by this one knob.
        if gate_value is None:
            env.pop(gate_name, None)
        else:
            env[gate_name] = gate_value
    # Determinism knobs the visual capture needs are set by that tool itself;
    # golden_scene sets its own screenshot/dump knobs. We only ensure the fixed
    # timestep intent is visible to golden_scene's child too.
    if fixed_timestep:
        env["MC2_SMOKE_FIXED_TIMESTEP"] = "1"

    # --- 1) PIXEL ORACLE: run_visual_capture (bookmark, deterministic FBO) -----
    viscap_out = state_dir / "viscap"
    viscap_cmd = [
        sys.executable, str(VISUAL_CAPTURE),
        "--mission", scene,
        "--exe", str(exe),
        "--out-dir", str(viscap_out),
        "--no-kill",
    ]
    if not fixed_timestep:
        viscap_cmd.append("--no-fixed-clock")
    rc = _run(viscap_cmd, env=env)
    # single-run mode returns 0 only if engine fired + all present.
    viscap_manifest = None
    for p in viscap_out.rglob("%s_capture_manifest.json" % scene):
        viscap_manifest = p
        break
    if rc != 0 or viscap_manifest is None:
        raise RuntimeError(
            "[%s] pixel-oracle capture FAILED (rc=%d, manifest=%s) -- a capture "
            "died or the engine capture path never fired; aborting parity run."
            % (label, rc, viscap_manifest))
    vm = json.loads(viscap_manifest.read_text(encoding="utf-8"))
    bmarks = (vm.get("report", {}) or vm).get("bookmarks")
    # report_summary nests under a summary/report block; probe both shapes.
    if bmarks is None:
        bmarks = _dig_bookmarks(vm)
    pixel_shas = {}
    present_map = {}
    any_nondet = False
    for b in bmarks or []:
        name = b.get("name", "?")
        pixel_shas[name] = b.get("sha256")
        present_map[name] = bool(b.get("present"))
        if b.get("engine_deterministic") is False:
            any_nondet = True
    # engine_capture_fired lives on the visual-capture report_summary extras;
    # probe both the top-level and the nested report shape.
    engine_fired = bool(
        vm.get("engine_capture_fired")
        or (vm.get("report", {}) or {}).get("engine_capture_fired")
        or _dig_field(vm, "engine_capture_fired"))
    if not pixel_shas:
        raise RuntimeError("[%s] pixel-oracle produced NO bookmark shas (%s)"
                           % (label, viscap_manifest))
    pixel_sha_all = "|".join("%s=%s" % (k, pixel_shas[k])
                             for k in sorted(pixel_shas))

    # --- 2) STRUCTURAL: golden_scene (registry_hash / counters / health) -------
    gs_out = state_dir / "golden_manifest.json"
    gs_cmd = [
        sys.executable, str(GOLDEN_SCENE),
        "--mission", scene,
        "--scene", scene,
        "--exe", str(exe),
        "--duration", "30",
        "--out", str(gs_out),
    ]
    rc = _run(gs_cmd, env=env)
    if rc != 0 or not gs_out.exists():
        raise RuntimeError(
            "[%s] structural capture FAILED (rc=%d, manifest exists=%s) -- "
            "aborting parity run." % (label, rc, gs_out.exists()))
    gs = json.loads(gs_out.read_text(encoding="utf-8"))

    # --- 2b) BACKEND-REGION health (region_impl) for fallback detection --------
    # golden_scene does not carry render_backend_region; read it straight from the
    # engine state dump it just produced. region_impl == "FallbackGL" means the
    # Vulkan backend failed to init and the region silently ran on GL -- a
    # backend compare against that is GL-vs-GL, NOT a real backend compare.
    region_health = read_backend_region_health(exe)

    # --- 3) COMBINED manifest (the thing we floor/compare) ---------------------
    combined = {
        "schema": "GOLDEN_PARITY_COMBINED_V1",
        "label": label,
        "scene": scene,
        "gate_name": gate_name or "",
        "gate_value": gate_value,
        # BACKEND-COMPARE provenance (empty in feature-gate mode):
        "backend": backend or "",
        "region": region or "",
        "region_impl": region_health.get("region_impl"),
        "backend_region_selected": region_health.get("backend_region_selected"),
        "fallback_reason": region_health.get("fallback_reason"),
        # deterministic structural fields from golden_scene:
        "registry_hash": gs.get("registry_hash"),
        "registry_resource_count": gs.get("registry_resource_count"),
        "exe_md5": gs.get("exe_md5"),
        "build_config": gs.get("build_config"),
        "gate_set": gs.get("gate_set"),
        "pass_counters": gs.get("pass_counters"),
        "render_health": gs.get("render_health"),
        # LOAD-BEARING pixel oracle from run_visual_capture (EXACT-comparable):
        "pixel_shas": pixel_shas,
        "pixel_sha_all": pixel_sha_all,
        "pixel_any_nondeterministic": any_nondet,
        # CAPTURE-LIVENESS fields (asserted by preflight BEFORE any compare):
        "bookmark_present": present_map,
        "engine_capture_fired": engine_fired,
        # provenance / volatile:
        "viscap_manifest": str(viscap_manifest),
        "golden_scene_manifest": str(gs_out),
        "generated_at_epoch": int(time.time()),
    }
    combined_path = work / ("%s_combined.json" % label)
    combined_path.write_text(json.dumps(combined, indent=2, sort_keys=True) + "\n",
                             encoding="utf-8")
    combined["_path"] = str(combined_path)
    if any_nondet:
        print("[golden_parity] WARN [%s]: engine marked >=1 bookmark "
              "NON-deterministic -- pixel oracle is soft for that bookmark."
              % label, file=sys.stderr)
    return combined


def _dig_bookmarks(obj):
    """Find the bookmarks list wherever manifest_schema.report_summary nested it."""
    if isinstance(obj, dict):
        if "bookmarks" in obj and isinstance(obj["bookmarks"], list):
            return obj["bookmarks"]
        for v in obj.values():
            r = _dig_bookmarks(v)
            if r:
                return r
    elif isinstance(obj, list):
        for v in obj:
            r = _dig_bookmarks(v)
            if r:
                return r
    return None


def _dig_field(obj, key):
    """Find the first value for `key` anywhere in a nested manifest dict/list."""
    if isinstance(obj, dict):
        if key in obj:
            return obj[key]
        for v in obj.values():
            r = _dig_field(v, key)
            if r is not None:
                return r
    elif isinstance(obj, list):
        for v in obj:
            r = _dig_field(v, key)
            if r is not None:
                return r
    return None


def read_backend_region_health(exe: Path) -> dict:
    """Read the render_backend_region block from the engine state dump that
    golden_scene just produced (<exe_dir>/debug_state/latest_render_state.json).
    Returns {} if absent (e.g. an older exe). Keys of interest:
      region_impl: "GLInline" | "VulkanSubgraph" | "FallbackGL" | "None"
      backend_region_selected: "gl" | "vk" | "fallback_gl" | "none"
      fallback_reason: str
    """
    dump = exe.parent / "debug_state" / "latest_render_state.json"
    if not dump.exists():
        return {}
    try:
        d = json.loads(dump.read_text(encoding="utf-8"))
    except Exception:
        return {}
    rbr = d.get("render_backend_region")
    return rbr if isinstance(rbr, dict) else {}


def detect_fallback(man: dict) -> str | None:
    """If a backend-compare state's region fell back to GL, return a human
    reason string; else None. A FallbackGL region means the Vulkan backend
    failed to init -> the compare is GL-vs-GL, not a real backend compare."""
    impl = man.get("region_impl")
    if impl == "FallbackGL":
        why = man.get("fallback_reason") or "(no reason reported)"
        return "region_impl=FallbackGL (fallback_reason=%s)" % why
    return None


class CaptureLivenessError(RuntimeError):
    """A capture did not actually fire / produced empty data. Distinct from a
    parity FAIL: this means the ORACLE is broken, not that pixels differ."""


def capture_liveness_preflight(combined: dict, gate_label: str,
                               expected_bookmarks: list[str] | None) -> None:
    """Assert a combined manifest represents a LIVE capture BEFORE any pixel
    comparison. Raises CaptureLivenessError on the FIRST failure with a message
    of the exact form:
        CAPTURE-LIVENESS FAIL: <reason> (gate=OFF/ON, bookmark=X)

    Checks, in order:
      1. engine_capture_fired == True
      2. capture produced > 0 images (at least one present bookmark w/ non-null sha)
      3. every EXPECTED bookmark name is present (present=True)
      4. the scene FBO resolved (present bookmark => non-null pixel hash; a
         getSceneFBO()==0 / zero-dim capture yields present=False or null hash)
      5. pixel hashes populated (no present bookmark has a null hash)
    """
    def fail(reason: str, bookmark: str = "-") -> None:
        raise CaptureLivenessError(
            "CAPTURE-LIVENESS FAIL: %s (gate=%s, bookmark=%s)"
            % (reason, gate_label, bookmark))

    present_map = combined.get("bookmark_present") or {}
    pixel_shas = combined.get("pixel_shas") or {}

    # 1) engine capture path fired at all
    if not combined.get("engine_capture_fired"):
        fail("engine_capture_fired=False (engine capture path never ran)")

    # 2) at least one real image
    produced = [n for n, sha in pixel_shas.items()
                if present_map.get(n) and sha]
    if not produced:
        fail("capture produced 0 images (no present bookmark with a pixel hash)")

    # 3) every expected bookmark present
    if expected_bookmarks:
        for name in expected_bookmarks:
            if not present_map.get(name):
                fail("expected bookmark missing / present=False", name)

    # 4 + 5) FBO resolved & hashes populated for present bookmarks
    #        (a present bookmark with a null hash == zero-dim / getSceneFBO()==0)
    for name, sha in pixel_shas.items():
        if present_map.get(name) and not sha:
            fail("scene FBO did not resolve (present bookmark has null pixel "
                 "hash -> zero-dim / getSceneFBO()==0)", name)
    # Any expected bookmark whose hash is null even though we got here:
    for name in (expected_bookmarks or pixel_shas.keys()):
        if present_map.get(name) and not pixel_shas.get(name):
            fail("pixel hash not populated for present bookmark", name)


def load_expected_bookmarks(scene: str) -> list[str] | None:
    """Read the expected bookmark NAMES for a scene from its bookmark JSON, so
    the preflight can assert every one came back present. Returns None if the
    file is absent (then presence is asserted against whatever was captured)."""
    bm_dir = (Path(os.environ["MC2_VISUAL_BOOKMARK_DIR"])
              if os.environ.get("MC2_VISUAL_BOOKMARK_DIR")
              else ROOT / "tests" / "visual" / "bookmarks")
    bm_path = bm_dir / ("%s.json" % scene)
    if not bm_path.exists():
        return None
    try:
        data = json.loads(bm_path.read_text(encoding="utf-8"))
    except Exception:
        return None
    marks = data.get("bookmarks", data) if isinstance(data, dict) else data
    names = []
    for m in marks or []:
        if isinstance(m, dict) and m.get("name"):
            names.append(m["name"])
    return names or None


def build_floor(scene: str, exe: Path, work: Path, n: int,
                fixed_timestep: bool, out_path: Path,
                backend: str | None = None, region: str | None = None) -> Path:
    """Capture N same-state captures and build a noise floor over the combined
    manifests. In feature-gate mode these are N OFF captures; in backend-compare
    mode these are N captures of backend-A (a real GL-vs-GL floor for THIS exe)."""
    manifests = []
    for i in range(n):
        m = capture_state("floor%d" % i, scene, exe, work,
                          gate_name=None, gate_value=None,
                          fixed_timestep=fixed_timestep,
                          backend=backend, region=region)
        manifests.append(m["_path"])
    out_path.parent.mkdir(parents=True, exist_ok=True)
    cmd = [sys.executable, str(GOLDEN_COMPARE), "noise-floor",
           *manifests, "--out", str(out_path)]
    rc = _run(cmd, timeout=60)
    if rc != 0 or not out_path.exists():
        raise RuntimeError("noise-floor build FAILED (rc=%d)" % rc)
    return out_path


def classify_pixel_fields(floor_path: Path) -> tuple[list[str], list[str]]:
    """Return (exact_pixel_fields, nonexact_pixel_fields) from a floor -- so we
    can honestly report how sharp the pixel oracle came out."""
    floor = json.loads(floor_path.read_text(encoding="utf-8"))
    fields = floor.get("fields", {})
    exact, nonexact = [], []
    for k, r in fields.items():
        if k == "pixel_sha_all" or k.startswith("pixel_shas."):
            (exact if r.get("kind") == "exact" else nonexact).append(k)
    return sorted(exact), sorted(nonexact)


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scene", help="scene/mission stem (e.g. mc2_01); a bookmark "
                                   "JSON tests/visual/bookmarks/<scene>.json must exist")
    ap.add_argument("gate_name", nargs="?", default=None,
                    help="FEATURE-GATE mode: island gate env-var to toggle "
                         "OFF->ON. Omit when using --backend-a/--backend-b "
                         "(BACKEND-COMPARE mode).")
    ap.add_argument("--gate-value", default="1",
                    help="value to set gate_name to for state B (default '1'); "
                         "state A always clears it")
    # --- BACKEND-COMPARE mode (RENDER-BACKEND-REGION-IFACE-1) ------------------
    ap.add_argument("--backend-a", default=None,
                    help="BACKEND-COMPARE: backend for state A (e.g. 'gl'). "
                         "Requires --backend-b. Sets MC2_RENDER_BACKEND_REGION_"
                         "IFACE=1 + MC2_POSTPROCESS_BACKEND=<a> for that state.")
    ap.add_argument("--backend-b", default=None,
                    help="BACKEND-COMPARE: backend for state B (e.g. 'vk').")
    ap.add_argument("--region", default="PostprocessFog",
                    help="BACKEND-COMPARE: region under test (default "
                         "PostprocessFog -- the only region-selectable region).")
    ap.add_argument("--backend-compare", action="store_true",
                    help="BACKEND-COMPARE shorthand: gl vs vk on --region.")
    ap.add_argument("--exe", default=str(DEFAULT_EXE),
                    help="deployed mc2.exe (default v0.4 deploy)")
    ap.add_argument("--noise-floor", default=None,
                    help="existing floor JSON; if omitted, build one from "
                         "--n-floor OFF captures")
    ap.add_argument("--n-floor", type=int, default=3,
                    help="OFF captures for a freshly-built floor (default 3)")
    ap.add_argument("--fixed-timestep", dest="fixed_timestep",
                    action="store_true", default=True,
                    help="(default) use MC2_SMOKE_FIXED_TIMESTEP=1 for max "
                         "pixel determinism")
    ap.add_argument("--no-fixed-timestep", dest="fixed_timestep",
                    action="store_false",
                    help="disable the fixed clock (drift EXPECTED; debug only)")
    ap.add_argument("--work-dir", default=None,
                    help="artifact dir (default <exe_dir>/debug_state/"
                         "golden_parity/<scene>)")
    args = ap.parse_args()

    # --- MODE RESOLUTION: feature-gate (default) vs backend-compare -----------
    backend_a = args.backend_a
    backend_b = args.backend_b
    if args.backend_compare and not (backend_a or backend_b):
        backend_a, backend_b = "gl", "vk"
    backend_mode = bool(backend_a or backend_b)
    if backend_mode:
        if not (backend_a and backend_b):
            print("[golden_parity] ERROR: BACKEND-COMPARE needs BOTH --backend-a "
                  "and --backend-b (or --backend-compare for gl vs vk).",
                  file=sys.stderr)
            return 2
        if args.gate_name:
            print("[golden_parity] ERROR: a positional gate_name is incompatible "
                  "with BACKEND-COMPARE mode; drop it.", file=sys.stderr)
            return 2
    else:
        if not args.gate_name:
            print("[golden_parity] ERROR: FEATURE-GATE mode needs a gate_name "
                  "(or use --backend-a/--backend-b for BACKEND-COMPARE).",
                  file=sys.stderr)
            return 2

    exe = Path(args.exe).resolve()
    if not exe.exists():
        print("[golden_parity] ERROR: exe not found: %s" % exe, file=sys.stderr)
        return 2

    work = (Path(args.work_dir) if args.work_dir
            else exe.parent / "debug_state" / "golden_parity" / args.scene)
    work.mkdir(parents=True, exist_ok=True)

    t0 = time.time()
    try:
        # --- floor ------------------------------------------------------------
        if args.noise_floor:
            floor_path = Path(args.noise_floor)
            if not floor_path.exists():
                print("[golden_parity] ERROR: --noise-floor not found: %s"
                      % floor_path, file=sys.stderr)
                return 2
            print("[golden_parity] using existing floor: %s" % floor_path)
        else:
            floor_path = work / "noise_floor.json"
            if backend_mode:
                print("[golden_parity] building REAL %s-vs-%s floor from %d "
                      "backend-A(%s) captures on region=%s..."
                      % (backend_a, backend_a, args.n_floor, backend_a, args.region))
                build_floor(args.scene, exe, work, args.n_floor,
                            args.fixed_timestep, floor_path,
                            backend=backend_a, region=args.region)
            else:
                print("[golden_parity] building floor from %d OFF captures..."
                      % args.n_floor)
                build_floor(args.scene, exe, work, args.n_floor,
                            args.fixed_timestep, floor_path)

        exact_px, nonexact_px = classify_pixel_fields(floor_path)

        if backend_mode:
            # --- state A (backend-A) + state B (backend-B) --------------------
            man_a = capture_state("A", args.scene, exe, work,
                                  gate_name=None, gate_value=None,
                                  fixed_timestep=args.fixed_timestep,
                                  backend=backend_a, region=args.region)
            man_b = capture_state("B", args.scene, exe, work,
                                  gate_name=None, gate_value=None,
                                  fixed_timestep=args.fixed_timestep,
                                  backend=backend_b, region=args.region)
        else:
            # --- state A (gate OFF) + state B (gate ON) -----------------------
            man_a = capture_state("A", args.scene, exe, work,
                                  gate_name=args.gate_name, gate_value=None,
                                  fixed_timestep=args.fixed_timestep)
            man_b = capture_state("B", args.scene, exe, work,
                                  gate_name=args.gate_name, gate_value=args.gate_value,
                                  fixed_timestep=args.fixed_timestep)
    except CaptureLivenessError as e:
        # Should not normally reach here (capture_state raises plain RuntimeError
        # on child failure); kept for symmetry.
        print("[golden_parity] %s" % e, file=sys.stderr)
        print("RESULT: CAPTURE-LIVENESS-FAIL scene=%s gate=%s reason=%s"
              % (args.scene, args.gate_name, e))
        return 4
    except RuntimeError as e:
        print("[golden_parity] ABORT: %s" % e, file=sys.stderr)
        print("RESULT: ABORT scene=%s gate=%s reason=capture-liveness"
              % (args.scene, args.gate_name))
        return 3

    # --- CAPTURE-LIVENESS PREFLIGHT (BOTH gate-OFF and gate-ON) ----------------
    # Formalized up-front gate: assert the captures actually fired and produced
    # populated pixel data BEFORE we ever call golden_compare, so "capture broke"
    # (exit 4) is never confused with "pixels differ" (parity FAIL, exit 1).
    expected = load_expected_bookmarks(args.scene)
    lbl_a = ("backend=%s" % backend_a) if backend_mode else "OFF"
    lbl_b = ("backend=%s" % backend_b) if backend_mode else "ON"
    ctx = ("region=%s" % args.region) if backend_mode else args.gate_name
    try:
        capture_liveness_preflight(man_a, lbl_a, expected)
        capture_liveness_preflight(man_b, lbl_b, expected)
    except CaptureLivenessError as e:
        print("[golden_parity] %s" % e, file=sys.stderr)
        print("RESULT: CAPTURE-LIVENESS-FAIL scene=%s %s reason=%s"
              % (args.scene, ctx, e))
        return 4
    print("[golden_parity] capture-liveness preflight OK (%s+%s fired, "
          "all expected bookmarks present, hashes populated)"
          % (lbl_a, lbl_b))

    # --- BACKEND-COMPARE fallback detection -----------------------------------
    # If the Vulkan backend failed to init and the region ran on GL (FallbackGL),
    # a "backend compare" is really GL-vs-GL and would pass trivially. DETECT and
    # report it rather than let a fallback masquerade as proven backend parity.
    fell_back = None
    if backend_mode:
        fb_a = detect_fallback(man_a)
        fb_b = detect_fallback(man_b)
        # backend-B is the one expected to be VulkanSubgraph; a fallback there
        # (or on A) collapses the compare to same-backend.
        if fb_b or fb_a:
            fell_back = fb_b or fb_a
        print("[golden_parity] backend-region impl: A(%s)=%s  B(%s)=%s"
              % (backend_a, man_a.get("region_impl"),
                 backend_b, man_b.get("region_impl")))

    # --- compare A vs B against the floor -------------------------------------
    cmp_cmd = [sys.executable, str(GOLDEN_COMPARE), "compare",
               man_a["_path"], man_b["_path"], "--noise-floor", str(floor_path)]
    cmp_rc = _run(cmp_cmd, timeout=60)

    # --- diff report (culprit fields / passes / pixel bookmarks) --------------
    report_dir = work / "report"
    diff_cmd = [sys.executable, str(GOLDEN_DIFF),
                man_a["_path"], man_b["_path"], "--out-dir", str(report_dir)]
    _run(diff_cmd, timeout=60)
    report_path = report_dir / "golden_report.json"

    # --- culprit fields (re-derive from combined manifests for the RESULT line)
    culprits = _culprit_fields(man_a, man_b, floor_path)

    dt = int(time.time() - t0)
    px_sharp = ("SHARP(%d exact px fields)" % len(exact_px) if exact_px
                else "BLUNT(0 exact px fields -- pixel oracle too noisy for this "
                     "scene; cannot catch a shading-only island diff)")
    verdict = "PASS" if cmp_rc == 0 else "FAIL"

    if backend_mode:
        # Distinct backend-parity wording + named region + named bucket/culprit.
        within = cmp_rc == 0
        culprit_str = (",".join(culprits) if culprits else "(none)")
        if fell_back:
            # A fallback must NOT masquerade as proven backend parity.
            print("RESULT: FALLBACK backend parity: %s vs %s (region=%s) "
                  "NOT-A-REAL-BACKEND-COMPARE -- Vulkan backend fell back to GL "
                  "[%s]; compare degenerates to GL-vs-GL. within_floor=%s "
                  "pixel_oracle=%s report=%s elapsed=%ds"
                  % (backend_a, backend_b, args.region, fell_back,
                     within, px_sharp,
                     report_path if report_path.exists() else "(none)", dt))
            print("[golden_parity] A green here would be GL-vs-GL, not a proven "
                  "GL-vs-Vulkan backend parity. Treating as INCONCLUSIVE.")
            return 5
        print("RESULT: %s backend parity: %s vs %s (region=%s) within_floor=%s "
              "pixel_oracle=%s culprit_bookmark=%s report=%s elapsed=%ds"
              % (verdict, backend_a.upper() if backend_a == "gl" else backend_a,
                 "Vulkan" if backend_b == "vk" else backend_b, args.region,
                 within, px_sharp, culprit_str,
                 report_path if report_path.exists() else "(none)", dt))
    else:
        print("RESULT: %s scene=%s gate=%s=%s within_floor=%s pixel_oracle=%s "
              "culprits=%s report=%s elapsed=%ds"
              % (verdict, args.scene, args.gate_name, args.gate_value,
                 cmp_rc == 0, px_sharp,
                 (",".join(culprits) if culprits else "(none)"),
                 report_path if report_path.exists() else "(none)", dt))
    if nonexact_px:
        drift_ref = "backend-A-vs-backend-A" if backend_mode else "OFF-vs-OFF"
        print("[golden_parity] NOTE: %d pixel field(s) were NOT exact in the "
              "floor (drift %s), so they cannot fail the candidate: %s"
              % (len(nonexact_px), drift_ref, ", ".join(nonexact_px)))
    return 0 if cmp_rc == 0 else 1


def _culprit_fields(man_a: dict, man_b: dict, floor_path: Path) -> list[str]:
    """Lightweight re-derivation of beyond-floor fields for the RESULT line
    (golden_compare already printed the authoritative list)."""
    try:
        sys.path.insert(0, str(SCRIPTS))
        import golden_compare as gc  # noqa: E402
        floor = json.loads(floor_path.read_text(encoding="utf-8"))
        # write temp manifests? we already have them on disk.
        beyond, _within, _ = gc.compare(man_a["_path"], man_b["_path"], floor)
        return [k for k, *_ in beyond]
    except Exception:
        return []


def _selftest() -> int:
    """Drive capture_liveness_preflight with SYNTHETIC combined manifests (no
    GPU / no game run). Proves: (a) healthy PASSES, (b) zero-image ABORTS,
    (c) partial ABORTS naming the missing bookmark. Returns 0 iff all 3 behave.
    """
    expected = ["overview", "close_mech", "water_edge"]

    def healthy() -> dict:
        return {
            "engine_capture_fired": True,
            "bookmark_present": {b: True for b in expected},
            "pixel_shas": {b: "%064x" % (i + 1) for i, b in enumerate(expected)},
        }

    def zero_image() -> dict:
        return {
            "engine_capture_fired": False,
            "bookmark_present": {b: False for b in expected},
            "pixel_shas": {b: None for b in expected},
        }

    def partial() -> dict:
        m = healthy()
        m["bookmark_present"]["water_edge"] = False
        m["pixel_shas"]["water_edge"] = None
        return m

    ok = True

    # (a) healthy -> PASSES
    try:
        capture_liveness_preflight(healthy(), "OFF", expected)
        print("[selftest] (a) healthy manifest        -> PASS (proceeds)")
    except CaptureLivenessError as e:
        ok = False
        print("[selftest] (a) healthy manifest        -> UNEXPECTED ABORT: %s" % e)

    # (b) zero-image -> ABORTS (engine_capture_fired reason first)
    try:
        capture_liveness_preflight(zero_image(), "ON", expected)
        ok = False
        print("[selftest] (b) zero-image manifest     -> UNEXPECTED PASS")
    except CaptureLivenessError as e:
        want = "engine_capture_fired=False" in str(e) and "gate=ON" in str(e)
        ok = ok and want
        print("[selftest] (b) zero-image manifest     -> ABORT (%s) [%s]"
              % (e, "reason OK" if want else "WRONG REASON"))

    # (c) partial -> ABORTS naming the missing bookmark
    try:
        capture_liveness_preflight(partial(), "OFF", expected)
        ok = False
        print("[selftest] (c) partial manifest        -> UNEXPECTED PASS")
    except CaptureLivenessError as e:
        want = "bookmark=water_edge" in str(e) and "missing" in str(e)
        ok = ok and want
        print("[selftest] (c) partial manifest        -> ABORT (%s) [%s]"
              % (e, "names missing bookmark" if want else "WRONG BOOKMARK"))

    # (d) BACKEND-COMPARE fallback detection: a FallbackGL manifest is flagged,
    #     a VulkanSubgraph manifest is not. (arg-plumbing / no-GPU logic check.)
    fb_real = {"region_impl": "VulkanSubgraph", "fallback_reason": ""}
    fb_fell = {"region_impl": "FallbackGL",
               "fallback_reason": "vk device init failed"}
    d_ok = (detect_fallback(fb_real) is None
            and detect_fallback(fb_fell) is not None
            and "FallbackGL" in (detect_fallback(fb_fell) or ""))
    ok = ok and d_ok
    print("[selftest] (d) backend fallback detect  -> %s (real->None, "
          "FallbackGL->flagged)" % ("OK" if d_ok else "WRONG"))

    print("[selftest] RESULT: %s" % ("ALL PASS" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    if "--self-test" in sys.argv[1:]:
        sys.exit(_selftest())
    sys.exit(main())
