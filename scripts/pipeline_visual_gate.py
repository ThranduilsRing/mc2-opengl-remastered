#!/usr/bin/env python3
"""pipeline_visual_gate.py — PIPELINE-VISUAL-GATE-HARNESS-1

Thin, CONCURRENT-SAFE wrapper over scripts/run_visual_capture.py that proves a
routed render pass (a pipeline-pass-coverage-ledger entry) actually renders
correctly, so its proofStatus can move to a LANDED value and the pass to
VISUAL_PROVEN.

It does NOT re-implement capture/compare — it drives the existing tools through a
per-pass PROFILE:
  - run_visual_capture.py --no-kill (never taskkills foreign mc2.exe; restores
    the desktop cursor) with the deterministic clock (MC2_SMOKE_MODE=1 +
    MC2_SMOKE_FIXED_TIMESTEP=1 + MC2_SMOKE_SEED, applied by run_visual_capture)
    + sim-freeze at the trigger frame.
  - --runs 3 --warmup 1 byte-STABILITY (same exe, deterministic).
  - optional BEFORE/AFTER byte A/B across two exes (pre- vs post-routing).
  - a "pass actually drew" confirmation via the MC2_PIPELINE_BIND_TRACE
    [PIPELINE_BIND] <Row> line (and/or an occlusion-coverage oracle env).

On success the operator flips the ledger proofStatus to the landed value
(byte_identical / perceptual_ab / oracle_coverage) and status -> VISUAL_PROVEN
(check-pass-coverage.py enforces: no VISUAL_PROVEN while proofStatus pending).

This script LAUNCHES the game (capture) — only run it with explicit go-ahead and
when concurrent-safe (it passes --no-kill so a foreign mc2.exe is left alone).
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RVC = ROOT / "scripts" / "run_visual_capture.py"
DEFAULT_EXE = "A:/Games/mc2-opengl/mc2-win64-v0.4c/mc2.exe"

# Per-pass profiles. Each names the ledger pass it proves, the mission +
# bookmark that frames the pass, capture timing, the [PIPELINE_BIND] row whose
# presence confirms the pass drew, and any extra env to force it in-frame.
PROFILES = {
    "water": {
        "ledger_pass": "Water",
        "pipeline_row": "WaterArmed",
        "mission": "mc2_01",                 # clearwater — water by default
        "bookmark": "tests/visual/bookmarks/mc2_01_water.json",
        "trigger_frame": 120,
        "settle": 30,
        "duration": 45,
        "force_env": {},                     # water-fast is the DEFAULT path (no arming env)
        "gate": "byte_identical",
        "note": "WaterArmed draws every frame in clearwater; bookmark must FRAME "
                "water (stock cameras minimize it). Confirm via [PIPELINE_BIND] "
                "WaterArmed + visible water pixels.",
    },
    "vfx": {
        "ledger_pass": "VFX",
        "pipeline_row": "VfxTubeAdditive",   # tube ribbons (weapon fire)
        "mission": "mc2_01",
        "bookmark": "tests/visual/bookmarks/mc2_01_werewolf.json",
        "trigger_frame": 147,                # werewolf fires ~frame 151
        "settle": 4,
        "duration": 45,
        # MC2_FX_FORCE_SPAWN: 8 mechs fire all weapons once (dmgDone=0).
        # MC2_VFX_ORACLE_TUBE_COVERAGE: occlusion-query the tube ribbon draw ->
        # [VFX_ORACLE_TUBE coverage] samples=N to stderr (-> capture.log).
        # MC2_VFX_ORACLE_TUBE=1 enables the GPU tube-ribbon bridge (without it the
        # bridge is gated off -> queue empty -> samples=0); _COVERAGE=1 wraps the
        # ribbon draw in the occlusion query; _RENDER + GPU_PARTICLES select the
        # GPU VFX path. Proven combo: ribbons=12 samples=3210 (mc2_01 werewolf).
        "force_env": {"MC2_FX_FORCE_SPAWN": "1",
                      "MC2_VFX_ORACLE_TUBE": "1",
                      "MC2_VFX_ORACLE_TUBE_COVERAGE": "1",
                      "MC2_VFX_ORACLE_RENDER": "1",
                      "MC2_GPU_PARTICLES": "1"},
        "runs": 3,                           # sim-freeze (gos_visual_capture v1.5)
        "warmup": 1,                         # makes VFX DETERMINISTIC -> byte-stable
        "gate": "oracle_coverage",
        # Additive rows that MUST bind with their schema-exact (non-collapsed) blend.
        # The runtime [PIPELINE_BIND] blend= field (VFX-VISUAL-GATE-1) is asserted
        # against these — tube=ONE/ONE, billboard/mesh=SRC_ALPHA/ONE.
        "blend_contract": {
            "VfxTubeAdditive": "AdditiveOneOne",
            "VfxBillboardAdditive": "AdditiveSrcAlphaOne",
            "VfxMeshAdditive": "AdditiveSrcAlphaOne",
        },
        "require_bound": ["VfxTubeAdditive"],   # tube is the oracle-exercised row
        "perceptual_family": "vfx",          # visual_compare policy family
        "note": "VFX is sim-frozen at the capture frame (gos_visual_capture v1.5 "
                "pauses the mission clock for the whole sweep -> deterministic, "
                "det=True), so it IS 3-run byte-stable. Proof = (1) tube occlusion "
                "oracle samples>0, (2) [PIPELINE_BIND] blend= confirms the additive "
                "cases are NOT collapsed (tube AdditiveOneOne vs billboard/mesh "
                "AdditiveSrcAlphaOne), (3) 3-run stability + perceptual compare. "
                "Static check-vfx-blend-distinction.py catches a collapse at "
                "check-time.",
    },
}


def _capture(exe: str, prof: dict, out_dir: Path, runs: int, warmup: int,
             trace: bool, dry_run: bool) -> int:
    env = os.environ.copy()
    _declared = dict(prof.get("force_env", {}))
    env.update(_declared)
    if trace:
        env["MC2_PIPELINE_BIND_TRACE"] = "1"
        _declared["MC2_PIPELINE_BIND_TRACE"] = "1"
    # CAPTURE-ENV-PIN-1: run_visual_capture scrubs ambient MC2_* by default.
    # Declare the gates THIS profile deliberately forces, or the A/B silently
    # runs with them dropped and both arms come out identical.
    env["MC2_CAPTURE_ENV_DECLARE"] = ",".join(sorted(_declared))
    cmd = [sys.executable, str(RVC),
           "--no-kill",                       # concurrent-safe: never kill foreign mc2.exe
           "--exe", exe,
           "--mission", prof["mission"],
           "--bookmarks", str(ROOT / prof["bookmark"]),
           "--out-dir", str(out_dir),
           "--trigger-frame", str(prof["trigger_frame"]),
           "--settle", str(prof["settle"]),
           "--duration", str(prof["duration"]),
           "--runs", str(runs), "--warmup", str(warmup)]
    print(f"[gate] {'DRY-RUN ' if dry_run else ''}capture: {' '.join(cmd)}")
    if dry_run:
        return 0
    return subprocess.run(cmd, env=env, creationflags=(getattr(subprocess, "CREATE_NO_WINDOW", 0) if not (sys.stdout and sys.stdout.isatty()) else 0)).returncode


def _row_drew(out_dir: Path, row: str) -> int:
    """Count [PIPELINE_BIND] <row> lines in capture logs (pass actually drew)."""
    n = 0
    for log in out_dir.rglob("*_capture.log"):
        try:
            for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
                if "PIPELINE_BIND" in line and f" {row} " in line:
                    n += 1
        except OSError:
            pass
    return n


def _oracle_tube_samples(out_dir: Path) -> int:
    """Max `samples=N` across all [VFX_ORACLE_TUBE coverage] lines in the capture
    logs. >0 = the tube ribbon pass rasterized fragments into the scene FBO (the
    occlusion-query 'did the pass draw' oracle). Log-based, so it works even when
    the single-pose bookmark never fires a PNG."""
    best = 0
    for log in out_dir.rglob("*_capture.log"):
        try:
            for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
                if "[VFX_ORACLE_TUBE coverage]" in line and "samples=" in line:
                    try:
                        tok = line.split("samples=", 1)[1].split()[0]
                        best = max(best, int(tok))
                    except (ValueError, IndexError):
                        pass
        except OSError:
            pass
    return best


def _pipeline_blends(out_dir: Path) -> dict:
    """Map [PIPELINE_BIND] row -> set of blend= values seen across capture logs.
    Requires the VFX-VISUAL-GATE-1 trace extension (blend= field). Lets the gate
    confirm at RUNTIME that additive cases are not collapsed."""
    import re
    rowblend: dict = {}
    pat = re.compile(r"\[PIPELINE_BIND\]\s+(\S+).*?\bblend=(\S+)")
    for log in out_dir.rglob("*_capture.log"):
        try:
            for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
                m = pat.search(line)
                if m:
                    rowblend.setdefault(m.group(1), set()).add(m.group(2))
        except OSError:
            pass
    return rowblend


def _bookmark_name(prof: dict) -> str:
    """First pose name in the profile's bookmark JSON (PNG stem uses the pose
    name, not the file name)."""
    import json
    doc = json.loads((ROOT / prof["bookmark"]).read_text(encoding="utf-8"))
    marks = doc.get("bookmarks", [])
    return marks[0]["name"] if marks else prof["mission"]


def _perceptual_stable(out_dir: Path, mission: str, bookmark: str,
                       family: str) -> tuple[bool, str]:
    """Compare the post-warmup run PNGs pairwise with visual_compare.py + the
    tolerance policy. Returns (ok, detail). Used as the stability proof when the
    byte-hashes are not identical."""
    safe = "".join("_" if c in "/\\:." else c for c in bookmark)
    pngs = sorted(out_dir.glob(f"r*/{mission}_{safe}.png"))
    if len(pngs) < 2:
        return False, f"need >=2 run PNGs to compare, found {len(pngs)}"
    vc = ROOT / "scripts" / "visual_compare.py"
    worst = "all pairs within tolerance"
    for cand in pngs[1:]:
        rc = subprocess.run([sys.executable, str(vc), str(pngs[0]), str(cand),
                             "--family", family], capture_output=True, text=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        if rc.returncode != 0:
            return False, f"{pngs[0].parent.name} vs {cand.parent.name}: " \
                          f"{rc.stdout.strip() or rc.stderr.strip()}"
    return True, worst


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("profile", choices=sorted(PROFILES))
    ap.add_argument("--exe", default=DEFAULT_EXE, help="AFTER (routed) exe")
    ap.add_argument("--before-exe", default=None,
                    help="pre-routing exe for a before/after byte A/B (optional)")
    ap.add_argument("--out-dir", default=None)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--warmup", type=int, default=1)
    ap.add_argument("--dry-run", action="store_true",
                    help="print the capture plan without launching the game")
    args = ap.parse_args()

    prof = PROFILES[args.profile]
    base = Path(args.out_dir) if args.out_dir else (ROOT / "tests" / "visual" /
                                                    "gate" / args.profile)
    after_dir = base / "after"

    print(f"[gate] profile={args.profile} pass={prof['ledger_pass']} "
          f"row={prof['pipeline_row']} gate={prof['gate']}")

    # Per-profile run plan (VFX is nondeterministic -> runs=1, no byte-stability).
    runs = prof.get("runs", args.runs)
    warmup = prof.get("warmup", args.warmup)

    # AFTER capture on the routed exe.
    rc = _capture(args.exe, prof, after_dir, runs, warmup,
                  trace=True, dry_run=args.dry_run)
    if args.dry_run:
        if args.before_exe:
            _capture(args.before_exe, prof, base / "before", runs,
                     warmup, trace=False, dry_run=True)
        print("[gate] DRY-RUN complete (no launch).")
        return 0

    # ── oracle_coverage gate (LOG-BASED) ──────────────────────────────────
    # No golden PNG, no byte-stability: the proof the pass rasterized is the
    # occlusion-query samples + [PIPELINE_BIND] binds in the capture log. This is
    # robust to (a) nondeterministic spawn and (b) the single-pose bookmark not
    # firing a PNG (engine bug-2). So we IGNORE the capture rc here.
    if prof["gate"] == "oracle_coverage":
        logs = list(after_dir.rglob("*_capture.log"))
        samples = _oracle_tube_samples(after_dir)
        drew = _row_drew(after_dir, prof["pipeline_row"])
        blends = _pipeline_blends(after_dir)
        print(f"[gate] oracle: tube samples={samples}  "
              f"{prof['pipeline_row']} binds={drew}  logs={len(logs)}")
        if not logs:
            print("[gate] FAIL: no capture log — engine never launched/logged",
                  file=sys.stderr)
            return 1
        # (1) tube coverage — the pass rasterized.
        if samples <= 0 and drew <= 0:
            print("[gate] FAIL: tube did not rasterize (samples=0, binds=0) — "
                  "check MC2_FX_FORCE_SPAWN / MC2_VFX_ORACLE_TUBE / bookmark frame",
                  file=sys.stderr)
            return 2

        # (2) additive NOT collapsed — runtime [PIPELINE_BIND] blend= must match the
        #     schema-exact blend for every additive row that bound this frame.
        contract = prof.get("blend_contract", {})
        collapse_fail = []
        seen_rows = []
        for row, want in contract.items():
            got = blends.get(row)
            if got:
                seen_rows.append(f"{row}={'/'.join(sorted(got))}")
                if got != {want}:
                    collapse_fail.append(f"{row}: blend={sorted(got)} expected {want}")
        # the trap: tube and billboard additive must NOT share a blend at runtime.
        ta, ba = blends.get("VfxTubeAdditive"), blends.get("VfxBillboardAdditive")
        if ta and ba and ta == ba:
            collapse_fail.append(f"VfxTubeAdditive {sorted(ta)} == "
                                 f"VfxBillboardAdditive {sorted(ba)} (COLLAPSED)")
        print(f"[gate] additive blends: {', '.join(seen_rows) or '(none bound)'}")
        for row in prof.get("require_bound", []):
            if row not in blends:
                print(f"[gate] FAIL: required row {row} never bound (no "
                      f"[PIPELINE_BIND]) — fixture/frame did not exercise it",
                      file=sys.stderr)
                return 2
        if collapse_fail:
            print("[gate] FAIL: additive blend COLLAPSE at runtime:", file=sys.stderr)
            for c in collapse_fail:
                print(f"  - {c}", file=sys.stderr)
            return 3

        # (3) 3-run stability: byte-identical (rc==0) else perceptual within policy.
        stable_proof = None
        if rc == 0:
            stable_proof = "3-run byte-stable"
        else:
            ok, detail = _perceptual_stable(after_dir, prof["mission"],
                                            _bookmark_name(prof), prof["perceptual_family"])
            if not ok:
                print(f"[gate] FAIL: not byte-stable AND perceptual compare failed: "
                      f"{detail}", file=sys.stderr)
                return 1
            stable_proof = f"perceptual within tolerance ({detail})"

        print(f"[gate] PASS oracle_coverage for {prof['ledger_pass']}: "
              f"tube samples={samples}; additive non-collapsed; {stable_proof}. "
              "Operator: set ledger proofStatus -> oracle_coverage + perceptual_ab, "
              "status VISUAL_PROVEN.")
        return 0

    # ── byte_identical gate (golden PNG, deterministic) ───────────────────
    if rc != 0:
        print(f"[gate] FAIL: AFTER capture/stability rc={rc}", file=sys.stderr)
        return 1

    drew = _row_drew(after_dir, prof["pipeline_row"])
    print(f"[gate] {prof['pipeline_row']} drew: {drew} [PIPELINE_BIND] lines "
          f"({'OK' if drew else 'NOT DRAWN — bookmark may not frame the pass'})")
    if drew == 0:
        print("[gate] FAIL: pass did not draw — fix the bookmark framing/force env",
              file=sys.stderr)
        return 2

    # Optional BEFORE/AFTER byte A/B.
    if args.before_exe:
        rcb = _capture(args.before_exe, prof, base / "before", runs,
                       warmup, trace=False, dry_run=False)
        if rcb != 0:
            print(f"[gate] FAIL: BEFORE capture rc={rcb}", file=sys.stderr)
            return 1
        # Compare last-run PNGs by sha (deterministic captures).
        print("[gate] before/after: compare the materialized run shas "
              "(use scripts/visual_compare.py for perceptual fallback).")

    print(f"[gate] PASS prerequisites met for {prof['ledger_pass']}: stable + drew. "
          "Operator: set ledger proofStatus -> landed + status VISUAL_PROVEN.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
