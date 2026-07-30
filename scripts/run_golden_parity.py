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

GOLDEN-PROVENANCE-TRUTH-1 (2026-07-30) -- THE FLOOR IS A GOLDEN.
  A noise floor is the reference this harness grades against, and it was never
  asked which binary produced it. A floor measured on binary X does not bound
  binary Y's run-to-run noise: reuse it via --noise-floor after a rebuild and
  every field it froze as `exact` is now an assertion about a build that no
  longer exists. The committed golden_floors/mc2_01.json records `exe_md5` and
  three `sample_sources` filenames that no longer exist on this machine -- it
  cannot say which binary it measured, and neither could the harness.
  provenance_preflight() below now establishes the floor AND both arms from each
  one's own recorded binary identity (content sha256 + the binary's embedded
  build fingerprint) and refuses to compare unless all three establish and name
  the SAME binary. An A/B whose two arms ran different builds is not a gate flip.
  There is no override flag.

EXIT: 0 iff A(gate OFF) vs B(gate ON) is WITHIN the established floor for every
field INCLUDING every exact pixel_sha; nonzero otherwise, printing the culprit
fields + the diff-report path. Exit 6 = provenance unestablished (distinct from
a parity FAIL: nothing was graded).

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
sys.path.insert(0, str(SCRIPTS))
import provenance_gate as pg  # noqa: E402  (GOLDEN-PROVENANCE-TRUTH-1)
GOLDEN_SCENE = SCRIPTS / "golden_scene.py"
VISUAL_CAPTURE = SCRIPTS / "run_visual_capture.py"
GOLDEN_COMPARE = SCRIPTS / "golden_compare.py"
GOLDEN_DIFF = SCRIPTS / "golden_diff_report.py"
DEFAULT_EXE = Path(r"A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe")
FLOORS_DIR = ROOT / "golden_floors"
# Per-capture wall budget: a smoke/visual sweep is ~30-45s; give generous margin.
CAP_TIMEOUT_S = 180

# SIM-IDENTITY-HASH-1: exit code for "the two arms were not drawing the same
# world". Kept in sync with sim_identity.EXIT_UNGRADED (7). Distinct from 0/1
# (graded pass/fail), 3 (abort), 4 (capture-liveness), 5 (backend fallback) and
# 6 (GOLDEN-PROVENANCE-TRUTH-1 provenance unestablished). 6 and 7 are the same
# family -- refuse to grade -- on different axes: 6 = different BINARIES,
# 7 = different WORLDS.
SIM_EXIT_UNGRADED = 7

# BACKEND-COMPARE region map (VK-REGION-SSAO-1 made the region set >1).
# Each region names (a) the debug-state health block its fallback detection
# reads (region_impl / fallback_reason live per-region now) and (b) any extra
# env BOTH states need so the region actually runs in-frame (SSAO is
# default-OFF: without MC2_SSAO=1 a "compare" would trivially pass on a frame
# that never entered the region).
#
# PER-REGION TOLERANCE FLOOR -- SUPERVISOR ORACLE RULING (2026-07-02, verbatim):
#   "transcendental-class regions may declare an EXPLICIT per-region tolerance
#    floor -- max |diff| <= 1 LSB AND diff-pixel count <= 0.1% -- in
#    REGION_SPEC (same family as the oobfog envelope precedent). Byte-exact
#    remains the DEFAULT for all regions; the floor is a declared per-region
#    property, never global."
# Mechanics (evaluate_region_tolerance): the floor is consulted ONLY when the
# byte-exact compare already FAILED, ONLY in backend-compare mode, ONLY for the
# region under test, and ONLY when every beyond-floor culprit is a pixel field
# (any structural culprit -- registry_hash / pass_counters / render_health --
# is NEVER tolerated).  Each diverging bookmark's A/B PNGs are decoded
# (pure-stdlib) and must satisfy BOTH bounds; the RESULT line then reports
# within_floor=False (honest byte-exact verdict) alongside
# region_tolerance=WITHIN[per-bookmark stats].  A region without a
# "tolerance_floor" key keeps the byte-exact oracle unchanged.
REGION_SPEC = {
    "PostprocessFog": {
        "health_block": "render_backend_region",
        "extra_env": {},
    },
    "PostprocessHzb": {
        # VK-REGION-HZB-1. HZB output feeds NOTHING visible yet (diagnostic
        # substrate) -- the pixel oracle proves no-regression of the visible
        # frame with the gate ON; the region's math proof is the bit-tight
        # hzb_fixture (probe) + region_impl non-fallback here. NO tolerance
        # floor: min-reduce is bit-portable, byte-exact is the bar.
        "health_block": "render_backend_hzb_region",
        "extra_env": {"MC2_HZB_BUILD": "1"},
    },
    "PostprocessSSAO": {
        "health_block": "render_backend_ssao_region",
        "extra_env": {"MC2_SSAO": "1"},
        # Transcendental-class: 16-sample hard-branch occlusion accumulation
        # over sin/cos/fract/pow -- driver-GLSL vs glslang-SPIR-V frontends
        # legitimately flip +-1 LSB on a sub-0.1% pixel population
        # (N0 residual: 134/480000 px, max |diff| = 1, deterministic).
        "tolerance_floor": {
            "class": "transcendental",
            "max_abs_diff_lsb": 1,        # per-channel, 8-bit levels
            "max_diff_pixel_frac": 0.001,  # <= 0.1% of pixels may differ
        },
    },
}


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
    # CAPTURE-ENV-PIN-1: names this state deliberately sets/clears. Passed to
    # run_visual_capture via MC2_CAPTURE_ENV_DECLARE so its shipped-default
    # scrub does not drop the very knob under test (an undeclared gate would
    # make both parity arms identical and the comparison meaningless).
    _declared: list[str] = []
    if backend is not None:
        # BACKEND-COMPARE: the state is "which backend implements the region",
        # not a feature-gate on/off. Enable the region interface + pick backend.
        env["MC2_RENDER_BACKEND_REGION_IFACE"] = "1"
        env["MC2_POSTPROCESS_BACKEND"] = backend
        _declared += ["MC2_RENDER_BACKEND_REGION_IFACE", "MC2_POSTPROCESS_BACKEND"]
        # Region-specific enable env applies to BOTH states (and the floor):
        # the region must actually run in-frame for the compare to mean anything.
        for k, v in REGION_SPEC.get(region or "PostprocessFog",
                                    REGION_SPEC["PostprocessFog"])["extra_env"].items():
            env[k] = v
            _declared.append(k)
    elif gate_name:
        # FEATURE-GATE mode: OFF and ON differ only by this one knob.
        if gate_value is None:
            env.pop(gate_name, None)
        else:
            env[gate_name] = gate_value
        # Declared either way: the OFF arm's absence is as deliberate as the
        # ON arm's presence.
        _declared.append(gate_name)
    # Determinism knobs the visual capture needs are set by that tool itself;
    # golden_scene sets its own screenshot/dump knobs. We only ensure the fixed
    # timestep intent is visible to golden_scene's child too.
    if fixed_timestep:
        env["MC2_SMOKE_FIXED_TIMESTEP"] = "1"
        _declared.append("MC2_SMOKE_FIXED_TIMESTEP")
    env["MC2_CAPTURE_ENV_DECLARE"] = ",".join(sorted(set(_declared)))

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
    pixel_pngs = {}
    present_map = {}
    any_nondet = False
    # SIM-IDENTITY-HASH-1: the world-state hash per bookmark. Kept per-bookmark
    # rather than collapsed to one: each bookmark is captured at its own moment,
    # so one may be comparable while another is not, and refusing the whole run
    # because the last bookmark drifted would throw away real signal.
    sim_ids = {}
    for b in bmarks or []:
        name = b.get("name", "?")
        pixel_shas[name] = b.get("sha256")
        pixel_pngs[name] = b.get("png")
        present_map[name] = bool(b.get("present"))
        sim_ids[name] = b.get("sim_identity")
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

    # GOLDEN-PROVENANCE-TRUTH-1: carry the binary's identity FORWARD out of the
    # visual-capture manifest (run_visual_capture emits manifest_schema's
    # identity block, which reads the exe's own embedded build fingerprint out
    # of its bytes). Only the deterministic sub-fields are copied: everything
    # here is constant for a given binary, so an A/B on one build leaves them
    # identical and golden_compare never sees them -- which is exactly what
    # makes their DIVERGENCE meaningful if two arms ever ran different builds.
    _vm_exe = ((vm.get("identity") or {}).get("exe")) or {}
    _vm_fp = (_vm_exe.get("embedded_fingerprint") or {}) if isinstance(_vm_exe, dict) else {}
    binary_identity = {
        "exe": {"path": _vm_exe.get("path"),
                "sha256": _vm_exe.get("sha256"),
                "embedded_fingerprint": _vm_fp},
        "commit": _vm_fp.get("sha"),
        "branch": _vm_fp.get("branch"),
        "dirty": _vm_fp.get("dirty"),
        "source": "run_visual_capture identity block (exe bytes, not the "
                  "generated header)",
    }

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
    # golden_scene does not carry the region health; read it straight from the
    # engine state dump it just produced, from the block belonging to the
    # region under test (fog -> render_backend_region, ssao ->
    # render_backend_ssao_region). region_impl == "FallbackGL" means the
    # Vulkan backend failed to init and the region silently ran on GL -- a
    # backend compare against that is GL-vs-GL, NOT a real backend compare.
    region_health = read_backend_region_health(
        exe, REGION_SPEC.get(region or "PostprocessFog",
                             REGION_SPEC["PostprocessFog"])["health_block"])

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
        # PNG paths per bookmark (volatile; consumed by the per-region
        # tolerance-floor evaluation when a declared floor exists):
        "pixel_pngs": pixel_pngs,
        "pixel_any_nondeterministic": any_nondet,
        # CAPTURE-LIVENESS fields (asserted by preflight BEFORE any compare):
        "bookmark_present": present_map,
        "engine_capture_fired": engine_fired,
        # provenance / volatile:
        # WHICH BINARY THIS ARM MEASURED (GOLDEN-PROVENANCE-TRUTH-1). Read out
        # of the exe's own bytes, never from the source worktree HEAD and never
        # from generated/build_fingerprint.h.
        "binary_identity": binary_identity,
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


def read_backend_region_health(exe: Path,
                               block: str = "render_backend_region") -> dict:
    """Read the given per-region backend-health block from the engine state
    dump that golden_scene just produced
    (<exe_dir>/debug_state/latest_render_state.json).
    block: "render_backend_region" (fog) / "render_backend_ssao_region" (ssao)
    -- see REGION_SPEC. Returns {} if absent (e.g. an older exe). Keys:
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
    rbr = d.get(block)
    return rbr if isinstance(rbr, dict) else {}


def detect_fallback(man: dict) -> str | None:
    """If a backend-compare state's region fell back to GL, return a human
    reason string; else None. A FallbackGL region means the Vulkan backend
    failed to init -> the compare is GL-vs-GL, not a real backend compare.

    VK-REGION-HZB-1 hardening: a region that NEVER RAN (impl None/missing --
    e.g. its enable env was dropped by a runner allowlist, or its own gates
    skipped every frame) is equally NOT a real backend compare: neither
    backend shaded the region, so a green result proves nothing. Flag it the
    same way (caller exits 5 INCONCLUSIVE)."""
    impl = man.get("region_impl")
    if impl == "FallbackGL":
        why = man.get("fallback_reason") or "(no reason reported)"
        return "region_impl=FallbackGL (fallback_reason=%s)" % why
    if impl in (None, "", "None"):
        return ("region_impl=%s -- region NEVER RAN in this state (enable env "
                "dropped by a runner allowlist, or region gates off every "
                "frame); compare proves nothing" % impl)
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
    # GOLDEN-PROVENANCE-TRUTH-1: stamp the floor with the binary it measured.
    # golden_compare records only `sample_sources` -- three FILENAMES, which on
    # the committed golden_floors/mc2_01.json no longer resolve to anything, so
    # a reused floor could not be checked at all. Stamping the identity into the
    # floor itself means a floor stays checkable after its samples are gone.
    try:
        floor_doc = json.loads(out_path.read_text(encoding="utf-8"))
        floor_doc["binary_identity"] = (
            json.loads(Path(manifests[0]).read_text(encoding="utf-8"))
            .get("binary_identity"))
        floor_doc["binary_identity_note"] = (
            "The binary all floor samples measured. A floor bounds THAT "
            "binary's run-to-run noise and no other's; run_golden_parity "
            "refuses to grade arms from a different build against it.")
        out_path.write_text(json.dumps(floor_doc, indent=2, sort_keys=True)
                            + "\n", encoding="utf-8")
    except Exception as e:  # noqa: BLE001
        raise RuntimeError("noise-floor built but could not be stamped with "
                           "its binary identity (%s); refusing to hand back an "
                           "unattributable floor." % e)
    return out_path


class ProvenanceRefusal(RuntimeError):
    """The floor or an arm cannot name the binary it measured. Distinct from a
    parity FAIL (pixels differ) and from CAPTURE-LIVENESS (the oracle broke):
    here nothing was graded at all, because the reference is unattributable."""


def provenance_preflight(floor_path: Path, man_a: dict, man_b: dict) -> list:
    """GOLDEN-PROVENANCE-TRUTH-1 -- the gate at the moment a verdict is minted.

    Three questions, in order, all answered from the artifacts' OWN recorded
    binary identity (content sha256 + the binary's embedded build fingerprint):

      1. Can the FLOOR say which binary it measured?  A floor is a golden: it
         froze a set of fields as `exact` on one build. Against a different
         build those assertions are about code that is not running.
      2. Can each ARM say which binary it measured?
      3. Do all three name the SAME binary?  An A/B whose arms ran different
         builds is not a gate flip, and every culprit it names is unearned.

    Raises ProvenanceRefusal with a message a reader can act on. No override.
    """
    ests = []
    try:
        floor_doc = json.loads(Path(floor_path).read_text(encoding="utf-8"))
    except Exception as e:  # noqa: BLE001
        raise ProvenanceRefusal("noise floor %s could not be read (%s)"
                                % (floor_path, e))
    ests.append(pg.establish(floor_doc, "noise floor (%s)" % Path(floor_path).name))
    ests.append(pg.establish(man_a, "arm A"))
    ests.append(pg.establish(man_b, "arm B"))
    try:
        pg.refuse_unless_established(
            ests,
            decision=("grade an A/B parity verdict -- this call decides whether "
                      "a render island ships"),
            remedy=("rebuild the noise floor on the binary under test (drop "
                    "--noise-floor and let the harness capture --n-floor arms), "
                    "and capture both arms with a build that carries an "
                    "embedded build fingerprint. A floor whose samples are gone "
                    "and which never recorded its exe's digest -- e.g. the "
                    "committed golden_floors/mc2_01.json -- cannot be repaired "
                    "in place; it has to be re-measured."))
    except pg.ProvenanceUnestablished as e:
        raise ProvenanceRefusal(str(e))

    ok, detail = pg.same_binary(ests)
    if not ok:
        raise ProvenanceRefusal(
            "floor and arms did NOT measure the same binary (%s). A floor "
            "bounds one build's run-to-run noise; comparing another build "
            "against it grades a code change as if it were drift, and an A/B "
            "whose two arms ran different builds is not a gate flip at all."
            % detail)
    return ests


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


def _png_decode(path: Path) -> tuple[int, int, int, bytes]:
    """Minimal pure-stdlib PNG decoder for the engine's capture PNGs
    (8-bit, non-interlaced, RGB or RGBA). Returns (w, h, channels, raw bytes).
    Raises ValueError on anything outside that envelope -- the tolerance
    evaluation then refuses (it must never guess)."""
    import struct
    import zlib
    data = Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG: %s" % path)
    pos, idat, w, h, colortype = 8, b"", None, None, None
    while pos + 8 <= len(data):
        ln = int.from_bytes(data[pos:pos + 4], "big")
        typ = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if typ == b"IHDR":
            w, h, depth, colortype, _comp, _filt, interlace = struct.unpack(
                ">IIBBBBB", chunk)
            if depth != 8 or interlace != 0 or colortype not in (2, 6):
                raise ValueError(
                    "unsupported PNG shape (depth=%d colortype=%d interlace=%d)"
                    % (depth, colortype, interlace))
        elif typ == b"IDAT":
            idat += chunk
        elif typ == b"IEND":
            break
    if w is None or not idat:
        raise ValueError("truncated PNG: %s" % path)
    raw = zlib.decompress(idat)
    ch = 3 if colortype == 2 else 4
    stride = w * ch
    out = bytearray(h * stride)
    prev = bytearray(stride)
    pos = 0
    for y in range(h):
        f = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        if f == 1:      # Sub
            for i in range(ch, stride):
                line[i] = (line[i] + line[i - ch]) & 0xFF
        elif f == 2:    # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:    # Average
            for i in range(stride):
                a = line[i - ch] if i >= ch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif f == 4:    # Paeth
            for i in range(stride):
                a = line[i - ch] if i >= ch else 0
                b = prev[i]
                c = prev[i - ch] if i >= ch else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        elif f != 0:
            raise ValueError("unknown PNG filter %d at row %d" % (f, y))
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, ch, bytes(out)


def png_diff_stats(path_a: Path, path_b: Path) -> dict:
    """Per-pixel diff between two capture PNGs. A pixel counts as differing if
    ANY channel differs; max_abs_diff is the per-channel maximum over the whole
    image. Pure stdlib (the harness discipline)."""
    wa, ha, ca, da = _png_decode(path_a)
    wb, hb, cb, db = _png_decode(path_b)
    if (wa, ha, ca) != (wb, hb, cb):
        raise ValueError("PNG shape mismatch: %dx%dx%d vs %dx%dx%d"
                         % (wa, ha, ca, wb, hb, cb))
    total_px = wa * ha
    max_d = 0
    diff_px = 0
    for i in range(0, total_px * ca, ca):
        pd = 0
        for k in range(ca):
            d = da[i + k] - db[i + k]
            if d < 0:
                d = -d
            if d > pd:
                pd = d
        if pd:
            diff_px += 1
            if pd > max_d:
                max_d = pd
    return {
        "width": wa, "height": ha, "channels": ca,
        "total_px": total_px,
        "diff_px": diff_px,
        "diff_px_frac": (diff_px / total_px) if total_px else 0.0,
        "max_abs_diff": max_d,
    }


def evaluate_region_tolerance(man_a: dict, man_b: dict, culprits: list[str],
                              tol: dict) -> dict:
    """Apply a DECLARED per-region tolerance floor (supervisor oracle ruling
    2026-07-02 -- see REGION_SPEC comment) to a byte-exact-FAILED backend
    compare. Returns {"within": bool, "summary": str, "log": [str],
    "per_bookmark": {name: stats}}.

    Refuses (within=False) when: any culprit is non-pixel (structural
    divergence is never tolerated), the culprit list is empty (unknown
    divergence), or a diverging bookmark's PNGs are unavailable/undecodable
    (the floor must be PROVEN, never assumed)."""
    max_lsb = int(tol["max_abs_diff_lsb"])
    max_frac = float(tol["max_diff_pixel_frac"])
    log = ["REGION TOLERANCE FLOOR (class=%s): max|diff|<=%d LSB AND "
           "diff-px<=%s%% -- declared per-region (supervisor oracle ruling "
           "2026-07-02); byte-exact remains the default oracle"
           % (tol.get("class", "?"), max_lsb, max_frac * 100.0)]
    if not culprits:
        log.append("REFUSED: byte-exact compare failed but culprit list is "
                   "empty (unknown divergence cannot be tolerated)")
        return {"within": False, "summary": "unknown-culprits", "log": log,
                "per_bookmark": {}}
    nonpx = [c for c in culprits
             if c != "pixel_sha_all" and not c.startswith("pixel_shas.")]
    if nonpx:
        log.append("REFUSED: non-pixel culprit(s) %s -- structural divergence "
                   "is never tolerated" % ",".join(nonpx))
        return {"within": False, "summary": "non-pixel-culprits:%s"
                % ",".join(nonpx), "log": log, "per_bookmark": {}}
    # Derive the diverging bookmarks from the sha maps (pixel_sha_all is a
    # join of these; per-bookmark is the authoritative granularity).
    shas_a = man_a.get("pixel_shas") or {}
    shas_b = man_b.get("pixel_shas") or {}
    diff_names = sorted(n for n in set(shas_a) | set(shas_b)
                        if shas_a.get(n) != shas_b.get(n))
    if not diff_names:
        log.append("REFUSED: pixel culprits reported but no bookmark sha "
                   "actually differs (inconsistent manifests)")
        return {"within": False, "summary": "inconsistent-shas", "log": log,
                "per_bookmark": {}}
    pngs_a = man_a.get("pixel_pngs") or {}
    pngs_b = man_b.get("pixel_pngs") or {}
    per = {}
    within = True
    parts = []
    for name in diff_names:
        pa, pb = pngs_a.get(name), pngs_b.get(name)
        if not pa or not pb or not Path(pa).exists() or not Path(pb).exists():
            log.append("REFUSED: bookmark '%s' diverges but its A/B PNGs are "
                       "unavailable -- floor cannot be proven" % name)
            return {"within": False, "summary": "pngs-unavailable:%s" % name,
                    "log": log, "per_bookmark": per}
        try:
            st = png_diff_stats(Path(pa), Path(pb))
        except ValueError as e:
            log.append("REFUSED: bookmark '%s' PNG diff failed: %s" % (name, e))
            return {"within": False, "summary": "png-diff-failed:%s" % name,
                    "log": log, "per_bookmark": per}
        per[name] = st
        ok = (st["max_abs_diff"] <= max_lsb
              and st["diff_px_frac"] <= max_frac)
        within = within and ok
        stat_str = ("max|diff|=%d(<=%d) diff_px=%d/%d=%.4f%%(<=%.2f%%)"
                    % (st["max_abs_diff"], max_lsb, st["diff_px"],
                       st["total_px"], st["diff_px_frac"] * 100.0,
                       max_frac * 100.0))
        log.append("  %s: %s -> %s declared floor"
                   % (name, stat_str, "WITHIN" if ok else "EXCEEDS"))
        parts.append("%s:%s" % (name, stat_str))
    return {"within": within,
            "summary": "; ".join(parts),
            "log": log, "per_bookmark": per}


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
                    choices=sorted(REGION_SPEC),
                    help="BACKEND-COMPARE: region under test (default "
                         "PostprocessFog). PostprocessSSAO additionally forces "
                         "MC2_SSAO=1 for both states + the floor and reads the "
                         "render_backend_ssao_region health block.")
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
    ap.add_argument("--sim-affecting", action="store_true",
                    help="SIM-IDENTITY-HASH-1: declare that this gate "
                         "legitimately CHANGES THE SIM (an AI/rules fix, not a "
                         "shading change), so the two arms are expected to "
                         "produce different worlds. World divergence is then "
                         "reported but never refused. Without this, a "
                         "sim-affecting gate would be permanently UNGRADED.")
    ap.add_argument("--require-sim-identity", action="store_true",
                    help="SIM-IDENTITY-HASH-1 strict mode: an ABSENT world hash "
                         "becomes UNGRADED instead of a stamped UNVERIFIED. "
                         "Use once every deploy under test emits the block.")
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

    # --- PROVENANCE PREFLIGHT (GOLDEN-PROVENANCE-TRUTH-1) ---------------------
    # Liveness above proves the captures FIRED. It says nothing about WHAT they
    # fired. Run this before golden_compare so an unattributable floor or arm
    # can never produce a clean-looking RESULT line.
    try:
        prov = provenance_preflight(floor_path, man_a, man_b)
    except ProvenanceRefusal as e:
        print("[golden_parity] %s" % e, file=sys.stderr)
        print("RESULT: PROVENANCE-UNESTABLISHED scene=%s %s -- nothing was "
              "graded (this is neither a PASS nor a FAIL)" % (args.scene, ctx))
        return 6
    print("[golden_parity] provenance preflight OK: floor + both arms all "
          "measured binary %s (commit %s, branch %s, dirty=%s) [%s]"
          % (str(prov[0]["exe_sha256"])[:12], prov[0]["commit"],
             prov[0]["branch"], prov[0]["dirty"], prov[0]["level"]))

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
    # SIM-IDENTITY-HASH-1: golden_compare runs the world-state refuse-to-grade
    # gate before it grades anything, and returns 7 when the two arms were not
    # drawing the same world. Backend-compare mode is GL-vs-Vulkan on identical
    # sim input, so a world divergence there is never expected; feature-gate mode
    # forwards --sim-affecting for gates that legitimately change the sim.
    if args.sim_affecting:
        cmp_cmd.append("--sim-affecting")
    if args.require_sim_identity:
        cmp_cmd.append("--require-sim-identity")
    cmp_rc = _run(cmp_cmd, timeout=60)

    # UNGRADED is neither PASS nor FAIL and must not be collapsed into either.
    # Returning 1 here would file "these arms are not comparable" as a parity
    # regression; returning 0 would file it as proof of parity. Both are lies,
    # and the second is the one that got false fixes reported.
    if cmp_rc == SIM_EXIT_UNGRADED:
        print("RESULT: UNGRADED scene=%s %s reason=sim-identity-divergence "
              "(the two arms were not drawing the same world; nothing about "
              "pixels was measured -- see the [golden_compare] sim_identity "
              "block above for the first diverging mover and field)"
              % (args.scene, ctx))
        return SIM_EXIT_UNGRADED

    # --- diff report (culprit fields / passes / pixel bookmarks) --------------
    report_dir = work / "report"
    diff_cmd = [sys.executable, str(GOLDEN_DIFF),
                man_a["_path"], man_b["_path"], "--out-dir", str(report_dir)]
    _run(diff_cmd, timeout=60)
    report_path = report_dir / "golden_report.json"

    # --- culprit fields (re-derive from combined manifests for the RESULT line)
    culprits = _culprit_fields(man_a, man_b, floor_path)

    # --- PER-REGION TOLERANCE FLOOR (supervisor oracle ruling 2026-07-02) ------
    # Consulted ONLY on a byte-exact FAIL, in backend-compare mode, for a
    # region that DECLARES a floor in REGION_SPEC. within_floor in the RESULT
    # line stays the honest byte-exact verdict; the tolerance verdict is
    # reported separately and, when WITHIN, flips the overall exit to PASS.
    byte_exact_ok = cmp_rc == 0
    tol_result = None
    if backend_mode and cmp_rc != 0 and not fell_back:
        tol = REGION_SPEC.get(args.region, {}).get("tolerance_floor")
        if tol:
            tol_result = evaluate_region_tolerance(man_a, man_b, culprits, tol)
            for line in tol_result["log"]:
                print("[golden_parity] %s" % line, file=sys.stderr)
            if tol_result["within"]:
                cmp_rc = 0

    dt = int(time.time() - t0)
    px_sharp = ("SHARP(%d exact px fields)" % len(exact_px) if exact_px
                else "BLUNT(0 exact px fields -- pixel oracle too noisy for this "
                     "scene; cannot catch a shading-only island diff)")
    verdict = "PASS" if cmp_rc == 0 else "FAIL"

    if backend_mode:
        # Distinct backend-parity wording + named region + named bucket/culprit.
        within = byte_exact_ok
        tol_note = ""
        if tol_result is not None:
            tol_note = (" region_tolerance=%s[%s]"
                        % ("WITHIN" if tol_result["within"] else "EXCEEDED",
                           tol_result["summary"]))
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
        print("RESULT: %s backend parity: %s vs %s (region=%s) within_floor=%s%s "
              "pixel_oracle=%s culprit_bookmark=%s report=%s elapsed=%ds"
              % (verdict, backend_a.upper() if backend_a == "gl" else backend_a,
                 "Vulkan" if backend_b == "vk" else backend_b, args.region,
                 within, tol_note, px_sharp, culprit_str,
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
    fb_never = {"region_impl": "None", "fallback_reason": ""}
    fb_miss = {}
    d_ok = (detect_fallback(fb_real) is None
            and detect_fallback(fb_fell) is not None
            and "FallbackGL" in (detect_fallback(fb_fell) or "")
            # VK-REGION-HZB-1 hardening: never-ran / missing impl flagged too
            and "NEVER RAN" in (detect_fallback(fb_never) or "")
            and "NEVER RAN" in (detect_fallback(fb_miss) or ""))
    ok = ok and d_ok
    print("[selftest] (d) backend fallback detect  -> %s (real->None, "
          "FallbackGL->flagged)" % ("OK" if d_ok else "WRONG"))

    # (e) PER-REGION TOLERANCE FLOOR (supervisor oracle ruling 2026-07-02):
    #     synthetic PNG pairs prove the evaluator's bounds + refusals with no
    #     GPU. Floor under test = the PostprocessSSAO declaration (1 LSB/0.1%).
    import struct
    import tempfile
    import zlib

    def _chunk(typ: bytes, payload: bytes) -> bytes:
        return (struct.pack(">I", len(payload)) + typ + payload
                + struct.pack(">I", zlib.crc32(typ + payload) & 0xFFFFFFFF))

    def _write_png(path: Path, w: int, h: int, rgb: bytes) -> None:
        ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
        raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3]
                       for y in range(h))
        path.write_bytes(b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", ihdr)
                         + _chunk(b"IDAT", zlib.compress(raw))
                         + _chunk(b"IEND", b""))

    tol = REGION_SPEC["PostprocessSSAO"]["tolerance_floor"]
    w, h = 100, 100  # 10000 px -> 0.1% floor = at most 10 diff pixels
    base = bytes((i * 7 + k * 13) % 251 for i in range(w * h)
                 for k in range(3))

    def _mut(offsets_deltas) -> bytes:
        buf = bytearray(base)
        for off, dv in offsets_deltas:
            buf[off] = (buf[off] + dv) & 0xFF
        return bytes(buf)

    with tempfile.TemporaryDirectory() as td:
        tdir = Path(td)

        def _mans(name: str, b_rgb: bytes):
            pa, pb = tdir / ("%s_a.png" % name), tdir / ("%s_b.png" % name)
            _write_png(pa, w, h, base)
            _write_png(pb, w, h, b_rgb)
            ma = {"pixel_shas": {"bm": "sha_a"}, "pixel_pngs": {"bm": str(pa)}}
            mb = {"pixel_shas": {"bm": "sha_b"}, "pixel_pngs": {"bm": str(pb)}}
            return ma, mb

        # e1: 5 px at +-1 LSB (0.05% < 0.1%) -> WITHIN
        ma, mb = _mans("e1", _mut([(i * 3 * 97, 1) for i in range(5)]))
        r1 = evaluate_region_tolerance(ma, mb, ["pixel_shas.bm"], tol)
        e1 = r1["within"] and r1["per_bookmark"]["bm"]["max_abs_diff"] == 1
        # e2: one pixel at +-2 LSB -> EXCEEDS (max|diff| bound)
        ma, mb = _mans("e2", _mut([(0, 2)]))
        r2 = evaluate_region_tolerance(ma, mb, ["pixel_shas.bm"], tol)
        e2 = not r2["within"] and r2["per_bookmark"]["bm"]["max_abs_diff"] == 2
        # e3: 11 px at 1 LSB (0.11% > 0.1%) -> EXCEEDS (pixel-count bound)
        ma, mb = _mans("e3", _mut([(i * 3 * 53, 1) for i in range(11)]))
        r3 = evaluate_region_tolerance(ma, mb, ["pixel_shas.bm"], tol)
        e3 = not r3["within"] and r3["per_bookmark"]["bm"]["diff_px"] == 11
        # e4: structural culprit alongside the pixel culprit -> REFUSED
        ma, mb = _mans("e4", _mut([(0, 1)]))
        r4 = evaluate_region_tolerance(
            ma, mb, ["pixel_shas.bm", "registry_hash"], tol)
        e4 = not r4["within"] and "non-pixel" in r4["summary"]
        # e5: empty culprit list -> REFUSED (unknown divergence)
        r5 = evaluate_region_tolerance(ma, mb, [], tol)
        e5 = not r5["within"] and "unknown" in r5["summary"]
        # e6: diverging bookmark with missing PNG -> REFUSED (must be proven)
        mb2 = dict(mb)
        mb2["pixel_pngs"] = {"bm": str(tdir / "absent.png")}
        r6 = evaluate_region_tolerance(ma, mb2, ["pixel_shas.bm"], tol)
        e6 = not r6["within"] and "unavailable" in r6["summary"]

    e_ok = all((e1, e2, e3, e4, e5, e6))
    ok = ok and e_ok
    print("[selftest] (e) region tolerance floor   -> %s (within@1LSB/0.05%%=%s,"
          " 2LSB-exceeds=%s, 0.11%%-exceeds=%s, structural-refuse=%s,"
          " empty-refuse=%s, missing-png-refuse=%s)"
          % ("OK" if e_ok else "WRONG", e1, e2, e3, e4, e5, e6))

    # (f) PROVENANCE PREFLIGHT (GOLDEN-PROVENANCE-TRUTH-1). Synthetic binaries
    #     carrying REAL build-fingerprint stamps, plus -- when it is present --
    #     the actual committed golden_floors/mc2_01.json, which is the live
    #     regression rather than a fixture: it records `exe_md5` and three
    #     sample filenames that no longer exist, so it cannot name its binary.
    import tempfile as _tf
    f_ok = True
    with _tf.TemporaryDirectory() as _td:
        _root = Path(_td)
        _bin_a = _root / "a.bin"
        _bin_a.write_bytes(pg._synthetic_binary(
            "aaaaaaaaaaaa", "claude/lane-a", "2026-07-30T00:00:00", "A:/a", False))
        _bin_b = _root / "b.bin"
        _bin_b.write_bytes(pg._synthetic_binary(
            "bbbbbbbbbbbb", "claude/lane-b", "2026-07-30T00:00:00", "A:/b", False))

        def _bi(p: Path) -> dict:
            fp = pg.ef.extract(str(p))
            return {"exe": {"path": str(p), "sha256": fp["exe_sha256_at_scan"],
                            "embedded_fingerprint": fp},
                    "commit": fp["sha"], "branch": fp["branch"],
                    "dirty": fp["dirty"]}

        good_floor = _root / "floor_ok.json"
        good_floor.write_text(json.dumps(
            {"schema": "GOLDEN_NOISE_FLOOR_V1", "fields": {},
             "binary_identity": _bi(_bin_a)}), encoding="utf-8")
        blind_floor = _root / "floor_blind.json"
        blind_floor.write_text(json.dumps(
            {"schema": "GOLDEN_NOISE_FLOOR_V1", "fields": {},
             "sample_sources": ["floor0_combined.json"],
             "exe_md5": "b1597c01e2602a0fc97e0a37066a215e"}), encoding="utf-8")

        man_a = {"binary_identity": _bi(_bin_a)}
        man_b = {"binary_identity": _bi(_bin_a)}
        man_b_other = {"binary_identity": _bi(_bin_b)}

        # f1: everything establishes and agrees -> preflight passes silently.
        try:
            provenance_preflight(good_floor, man_a, man_b)
            f1 = True
        except ProvenanceRefusal as e:
            f1 = False
            print("[selftest]     f1 unexpected refusal: %s" % str(e)[:120])
        # f2: floor that cannot name its binary (the committed-floor shape).
        try:
            provenance_preflight(blind_floor, man_a, man_b)
            f2 = False
        except ProvenanceRefusal as e:
            f2 = "noise floor" in str(e) and "UNESTABLISHED" in str(e)
        # f3: arms from two different builds -> refused.
        try:
            provenance_preflight(good_floor, man_a, man_b_other)
            f3 = False
        except ProvenanceRefusal as e:
            f3 = "same binary" in str(e)
        # f4: an arm with no binary identity at all -> refused, naming the arm.
        try:
            provenance_preflight(good_floor, man_a, {"pixel_shas": {}})
            f4 = False
        except ProvenanceRefusal as e:
            f4 = "arm B" in str(e)
        f_ok = all((f1, f2, f3, f4))

    # f5: THE LIVE ARTIFACT. Not a fixture -- the floor committed in this repo.
    live_floor = ROOT / "golden_floors" / "mc2_01.json"
    if live_floor.exists():
        try:
            with _tf.TemporaryDirectory() as _td2:
                _p = Path(_td2) / "x.bin"
                _p.write_bytes(pg._synthetic_binary(
                    "cccccccccccc", "b", "2026-07-30T00:00:00", "A:/c", False))
                fp = pg.ef.extract(str(_p))
                m = {"binary_identity": {
                    "exe": {"path": str(_p), "sha256": fp["exe_sha256_at_scan"],
                            "embedded_fingerprint": fp}}}
                provenance_preflight(live_floor, m, m)
            f5 = False
            print("[selftest]     f5: the COMMITTED floor was accepted -- if it "
                  "has since been re-measured with a binary identity this is "
                  "correct; if not, the gate is blind")
        except ProvenanceRefusal:
            f5 = True
    else:
        f5 = True
        print("[selftest]     f5 skipped: golden_floors/mc2_01.json absent")
    f_ok = f_ok and f5
    ok = ok and f_ok
    print("[selftest] (f) provenance preflight     -> %s (agree-pass=%s, "
          "blind-floor-refuse=%s, two-builds-refuse=%s, no-identity-refuse=%s, "
          "LIVE-committed-floor-refuse=%s)"
          % ("OK" if f_ok else "WRONG", f1, f2, f3, f4, f5))

    print("[selftest] RESULT: %s" % ("ALL PASS" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    if "--self-test" in sys.argv[1:]:
        sys.exit(_selftest())
    sys.exit(main())
