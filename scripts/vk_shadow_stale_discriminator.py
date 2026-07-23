#!/usr/bin/env python3
"""VK-SHADOW-STALE-DISCRIMINATOR-1 (Part A harness) -- reproduce the user-sighted
intermittent stale/static shadow under the VK umbrella.

USER SYMPTOM (VK-ARC-STATE OPEN ITEMS, 2026-07-10): a large terrain face rendered
near-black with a shadow that DID NOT MOVE WITH THE CAMERA; one run bad, next fine.
Suspect chain: (a) the 7a5bf1fa strict fail-soft on the 2-frame warmup CSM GL
readback may leave shadow products stale/absent early; (b) the VK shadow producer
under direct-chain (62707331) may not re-produce on camera motion in some ordering.

DISCRIMINATOR DESIGN
  EARLY vs SETTLED (same pose, same camera): capture the umbrella bookmark sweep at a
  LOW trigger frame + minimal settle (the warmup window) and at a HIGH trigger frame +
  long settle. A shadow that is warmup-stale/absent shows up as a per-pose EARLY-vs-
  SETTLED MAD far above the known cross-run pixel self-noise floor (~2/ch for far poses).
  If EARLY == SETTLED per pose, the shadow is NOT warmup-dependent (non-repro).

  CAMERA-MOTION (settled): the 11-pose sweep already varies the camera. Diffing the
  settled VK run against a settled GL-reference run per pose reveals a shadow that is
  frozen/wrong relative to GL at any camera pose (the "does not move with camera" claim
  is a shadow that is present-but-static -> it mismatches GL at the poses where GL's
  shadow shifted).

This runner LEAVES the strict-visible env intact (the umbrella the user runs); it only
varies --trigger/--settle. It is a verification-only tool (no engine mutation). Default
attempts=1; pass --attempts 4 for the mission's N=4 repro budget.

Usage:
  py -3 scripts/vk_shadow_stale_discriminator.py --exe <0.4c/mc2.exe> --mission mc2_01 \
      --attempts 4 [--gl-reference]
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Far-pose cross-run pixel self-noise floor measured in VK-ARC-STATE (settled captures
# are byte-stable at scene-deterministic poses; warmup timing lifts a few). A per-pose
# EARLY-vs-SETTLED MAD above this floor at a shadow-bearing pose = warmup-stale shadow.
# With BOTH triggers past scene-load warmup, cross-run per-pose max_delta stays low
# (fixed-timestep captures are byte-stable). A shadow-bearing pose whose max_delta jumps
# far above this floor between an early-settled and a late-settled capture = repro signal.
MAXDELTA_FLOOR = 24


def run_capture(exe: str, mission: str, out_dir: Path, trigger: int, settle: int,
                duration: int, gl_reference: bool) -> int:
    """Drive one umbrella (or GL-reference) bookmark sweep via the strict-visible runner."""
    if gl_reference:
        # GL reference = gates OFF. Use run_visual_capture.py directly with no VK env.
        cmd = [sys.executable, str(ROOT / "scripts" / "run_visual_capture.py"),
               "--exe", exe, "--mission", mission, "--out-dir", str(out_dir),
               "--duration", str(duration), "--settle", str(settle),
               "--trigger", str(trigger), "--show-window"]
        return subprocess.call(cmd, cwd=str(ROOT))
    cmd = [sys.executable, str(ROOT / "scripts" / "run_vk_nointerop_strict_visible.py"),
           "--exe", exe, "--mission", mission, "--out-dir", str(out_dir),
           "--duration", str(duration), "--settle", str(settle), "--trigger", str(trigger)]
    return subprocess.call(cmd, cwd=str(ROOT))


def diff_dirs(golden: Path, candidate: Path, json_out: Path) -> dict:
    cmd = [sys.executable, str(ROOT / "scripts" / "visual_diff.py"),
           "--golden-dir", str(golden), "--candidate-dir", str(candidate),
           "--json", str(json_out)]
    subprocess.call(cmd, cwd=str(ROOT))
    try:
        return json.loads(json_out.read_text(encoding="utf-8"))
    except Exception:
        return {}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default="A:/Games/mc2-opengl/mc2-win64-0.4c/mc2.exe")
    ap.add_argument("--mission", default="mc2_01")
    ap.add_argument("--attempts", type=int, default=1)
    # Both triggers are PAST scene-load warmup (fog/LOD/exposure settle by ~frame 240) so
    # the early-vs-settled diff isolates late-emerging shadow staleness, not raw load noise.
    ap.add_argument("--early-trigger", type=int, default=300)
    ap.add_argument("--early-settle", type=int, default=40)
    ap.add_argument("--settled-trigger", type=int, default=1200)
    ap.add_argument("--settled-settle", type=int, default=80)
    ap.add_argument("--duration", type=int, default=150)
    ap.add_argument("--gl-reference", action="store_true",
                    help="also capture a settled GL-reference sweep for VK-vs-GL parity.")
    ap.add_argument("--out-root")
    args = ap.parse_args()

    stamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H-%M-%SZ")
    out_root = Path(args.out_root) if args.out_root else \
        ROOT / ".claude" / f"vk-shadow-stale-discriminator-{stamp}"
    out_root.mkdir(parents=True, exist_ok=True)

    verdicts = []
    for attempt in range(1, args.attempts + 1):
        a_dir = out_root / f"attempt{attempt}"
        early = a_dir / "early"
        settled = a_dir / "settled"
        print(f"\n==== ATTEMPT {attempt}/{args.attempts}: EARLY capture "
              f"(trigger={args.early_trigger} settle={args.early_settle}) ====")
        run_capture(args.exe, args.mission, early, args.early_trigger,
                    args.early_settle, args.duration, gl_reference=False)
        print(f"==== ATTEMPT {attempt}: SETTLED capture "
              f"(trigger={args.settled_trigger} settle={args.settled_settle}) ====")
        run_capture(args.exe, args.mission, settled, args.settled_trigger,
                    args.settled_settle, args.duration, gl_reference=False)

        d = diff_dirs(settled, early, a_dir / "early_vs_settled.json")
        # visual_diff records carry max_delta (0..255) + changed_pct. With both triggers
        # past scene-load warmup, a per-pose max_delta on a shadow-bearing pose far above
        # the cross-run floor = a late-emerging / warmup-dependent shadow (the repro).
        worst = 0
        worst_pose = ""
        for item in d.get("records", []):
            md = item.get("max_delta", 0) or 0
            if md > worst:
                worst = md
                worst_pose = Path(item.get("golden", "?")).stem
        repro = worst > MAXDELTA_FLOOR
        verdicts.append({"attempt": attempt, "worst_pose": worst_pose,
                         "worst_early_vs_settled_max_delta": worst,
                         "repro": repro})
        print(f"[attempt {attempt}] worst early-vs-settled max_delta = {worst}/255 at "
              f"'{worst_pose}' (floor {MAXDELTA_FLOOR}) -> "
              f"{'STALE-SHADOW SUSPECT (inspect PNGs)' if repro else 'no repro'}")

        if args.gl_reference:
            glref = a_dir / "gl_settled"
            print(f"==== ATTEMPT {attempt}: GL-reference SETTLED capture ====")
            run_capture(args.exe, args.mission, glref, args.settled_trigger,
                        args.settled_settle, args.duration, gl_reference=True)
            diff_dirs(glref, settled, a_dir / "vk_vs_gl_settled.json")

    summary = {"mission": args.mission, "exe": args.exe, "attempts": args.attempts,
               "maxdelta_floor": MAXDELTA_FLOOR, "verdicts": verdicts,
               "reproduced_any": any(v["repro"] for v in verdicts),
               "note": ("early-vs-settled FULL-FRAME diff is CONFOUNDED by animation phase "
                        "(water/fog/units advance between trigger frames), so a nonzero "
                        "max_delta is NOT by itself a shadow repro -- inspect the shadow-"
                        "bearing pose PNGs (ridge_lowangle) and rely on each capture's own "
                        "nondeterministic flag (fixed-timestep -> internally byte-stable when "
                        "clean). The deterministic harness cannot surface an intermittent "
                        "per-launch timing race by construction.")}
    (out_root / "discriminator_summary.json").write_text(
        json.dumps(summary, indent=2), encoding="utf-8")
    print("\n==== DISCRIMINATOR SUMMARY ====")
    print(json.dumps(summary, indent=2))
    print(f"artifacts: {out_root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
