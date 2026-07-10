#!/usr/bin/env python3
"""Check a Vulkan no-interop proof log for forbidden fallback/interoperability paths.

HARDENING (VK-NOINTEROP-STRICT-REVIEW, 2026-07-10) -- this checker was rewritten
after an adversarial review found a REALIZED false-pass: the prior checker exited 0
while a 124 ms/frame 8192^2 glGetTexImage CSM readback was active in the render path.
Root holes closed here:

  H1  FORBIDDEN previously greped only trace/present strings -- none of the GL
      readback / interop entry points. The per-frame render-path readback emits a
      dedicated marker ([VK_*_PERFRAME_READBACK]); copy-once asset-feed imports
      (documented in .claude/METAL-GATE-AUDIT-2.md) never emit it and are allowed.
      The marker + the forbidden_* counters are the authoritative per-frame signal;
      raw entry-point names are also greped as defense-in-depth.
  H2  The islands emit forbidden_* counters but the prior checker never read them
      AND the offending terrain fallback never incremented its own counter. Now:
      every island health line is REQUIRED present, every forbidden_*/depthin_*
      counter on it is asserted == 0, and a missing island line = FAIL.
  H3  Prior code used max(values) so 1 good frame out of 300 passed. Now the frame
      counters are read from the authoritative single [VK_PRESENT] line and asserted
      frame-consistent (>= --min-frames AND each within a band of present_frames),
      so a lone good frame can no longer carry the proof.
  H4  Scene/screen-shadow GL canaries were emitted but never asserted (and the proof
      runs didn't even enable their gates). Now the canary lines are REQUIRED present
      with draws=0 / any_samples=0; a missing canary line = FAIL (pass --require-canaries,
      default on; disable only for a diagnostic partial run with --no-canaries).
  H5  The proof env sets MC2_VK_*_INTEROP=1 names inside a "no-interop" proof; those
      are reconciled ONLY by direct-chain. The checker now requires
      MC2_VK_SHADOW_DIRECT_CHAIN=1 in the exe-confirmed gate_env whenever any
      MC2_VK_*_INTEROP=1 name is set.
  H6  Provenance: FAIL on dirty:true or a build/commit sha != --expect-sha, using both
      the capture manifest git block and every per-bookmark engine build stamp. A
      terrain stage_us ceiling (--stage-us-ceiling, default 2000) catches a
      readback-scale regression numerically (post-fix ~3 us; the killed readback was
      ~126815 us).
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


# --- H1: forbidden log strings ------------------------------------------------
# Scope: these all indicate a PER-FRAME render-path GL readback or a GL<->VK
# interop handoff. Copy-once asset-feed imports (glGetTexImage/glGetCompressedTex
# Image/glGetBufferSubData at map/mission load) are ALLOWED and never appear here --
# they emit no marker. The engine's per-frame readback fallback emits a
# [VK_*_PERFRAME_READBACK] marker (see vulkan_terrain_island.cpp), which is the
# authoritative trip-wire; the raw entry-point names are defense-in-depth in case
# any site logs one directly.
FORBIDDEN = [
    "[VK_NO_GL_INTEROP_STRICT]",
    "MC2_VK_PRESENT_NATIVE_STRICT abort",
    "present_image_not_vk_owned",
    "glInteropSignalToVk",
    "glInteropWaitFromVk",
    "glInteropViewCreate",
    "vkImageResourceCreate(exportable)",
    "VK_POST_HZB_TRACE",
    "VK_POST_SCREENFX_TRACE",
    "VK_POST_SSAO_TRACE",
    "VK_PRESENT_COMPOSITE_TRACE",
    "Crash Report",
    "DEVICE_LOST",
    # per-frame readback markers (any island):
    "PERFRAME_READBACK",
]


REQUIRED = [
    "[VK_PRESENT]",
]


# --- H2: island health lines (all REQUIRED present) ---------------------------
# Each island prints one cumulative health line at destroy carrying its forbidden_*
# counters. A MISSING line = FAIL (so an absent counter cannot silently pass).
REQUIRED_ISLAND_TAGS = [
    "[VK_TERRAIN_TIER1]",
    "[VK_PROP_COLOR_TIER1]",
    "[VK_MECH_TIER1]",
    "[VK_SHADOW_INTEROP]",
    "[VK_WATER_TIER1]",
]


# --- H3: frame counters from the authoritative [VK_PRESENT] line --------------
REQUIRED_POSITIVE_COUNTERS = [
    "present_frames",
    "copy1_skip_frames",
    "gl_handoff_skip_frames",
    "composite_vk_frames",
    "imgui_vk_frames",
]

REQUIRED_ZERO_COUNTERS = [
    "copy1_blit_frames",
]


REQUIRED_ENV = [
    "MC2_VK_NO_GL_INTEROP_STRICT=1",
    "MC2_VK_NO_GL_INTEROP_RESOURCE_STRICT=1",
    "MC2_VK_PRESENT_NO_GL_IMPORT=1",
    "MC2_VK_PRESENT_NATIVE_STRICT=1",
]


# --- H4: GL canaries ----------------------------------------------------------
REQUIRED_CANARIES = [
    "[VK_SCENE_GL_DRAW_CANARY v1]",
    "[VK_SCREENSHADOW_GL_CANARY v1]",
]


def _present_line(text: str) -> str | None:
    for line in text.splitlines():
        if line.startswith("[VK_PRESENT]") and "present_frames=" in line:
            return line
    return None


def _u(line: str, name: str) -> int | None:
    m = re.search(rf"\b{re.escape(name)}=(\d+)", line)
    return int(m.group(1)) if m else None


def check_forbidden(text: str) -> bool:
    failed = False
    for needle in FORBIDDEN:
        count = text.count(needle)
        print(f"forbidden {needle!r}: {count}")
        if count:
            failed = True
    return failed


def check_required_strings(text: str) -> bool:
    failed = False
    for needle in REQUIRED:
        present = needle in text
        print(f"required {needle!r}: {'yes' if present else 'NO'}")
        if not present:
            failed = True
    return failed


def check_islands(text: str) -> bool:
    """H2: every island line present; every forbidden_*/depthin_* counter on it == 0."""
    failed = False
    lines = text.splitlines()
    for tag in REQUIRED_ISLAND_TAGS:
        line = next((ln for ln in lines if ln.startswith(tag)), None)
        if line is None:
            print(f"island {tag}: MISSING (FAIL)")
            failed = True
            continue
        counters = re.findall(r"\b(forbidden_\w+|depthin_\w+)=(\d+)", line)
        if not counters:
            print(f"island {tag}: present but NO forbidden_ counters (FAIL)")
            failed = True
            continue
        bad = [f"{n}={v}" for n, v in counters if int(v) != 0]
        summary = " ".join(f"{n}={v}" for n, v in counters)
        print(f"island {tag}: {summary}" + (f"  <-- NONZERO {bad} (FAIL)" if bad else "  ok"))
        if bad:
            failed = True
    return failed


def check_frame_consistency(text: str, min_frames: int) -> bool:
    """H3: read counters from the single authoritative [VK_PRESENT] line; assert
    frame-consistency instead of max()-over-all-lines."""
    failed = False
    line = _present_line(text)
    if line is None:
        print("frame-consistency: no [VK_PRESENT] health line (FAIL)")
        return True
    present_frames = _u(line, "present_frames")
    if present_frames is None or present_frames < min_frames:
        print(f"frame-consistency: present_frames={present_frames} < min {min_frames} (FAIL)")
        failed = True
        present_frames = present_frames or 0

    band = max(1, int(present_frames * 0.5))
    for name in REQUIRED_POSITIVE_COUNTERS:
        v = _u(line, name)
        ok = v is not None and v >= band
        print(f"counter {name}={v} (need >= {band} = 0.5*present_frames) {'ok' if ok else 'FAIL'}")
        if not ok:
            failed = True

    for name in REQUIRED_ZERO_COUNTERS:
        v = _u(line, name)
        ok = v == 0
        print(f"counter {name}={v} (need 0) {'ok' if ok else 'FAIL'}")
        if not ok:
            failed = True
    return failed


def check_canaries(text: str) -> bool:
    """H4: canary lines REQUIRED present, each with any_samples=0 (and draws=0 for scene)."""
    failed = False
    lines = text.splitlines()
    for tag in REQUIRED_CANARIES:
        hits = [ln for ln in lines if tag in ln]
        if not hits:
            print(f"canary {tag}: MISSING (FAIL)")
            failed = True
            continue
        for ln in hits:
            anys = _u(ln, "any_samples")
            draws = _u(ln, "draws")
            bad = (anys not in (0, None) and anys != 0) or (draws is not None and draws != 0)
            print(f"canary {tag}: any_samples={anys} draws={draws} {'FAIL' if bad else 'ok'}")
            if bad or anys is None:
                failed = True
    return failed


def check_env(env_text: str, no_resource_strict: bool) -> bool:
    """H5 + base env receipts."""
    failed = False
    required_env = REQUIRED_ENV if not no_resource_strict else [
        e for e in REQUIRED_ENV if not e.startswith("MC2_VK_NO_GL_INTEROP_RESOURCE_STRICT=")
    ]
    for needle in required_env:
        present = needle in env_text
        print(f"env {needle}: {'yes' if present else 'NO'}")
        if not present:
            failed = True

    # H5: any MC2_VK_*_INTEROP=1 must be reconciled by direct-chain.
    interop_on = re.findall(r"^(MC2_VK_\w*INTEROP)=1\s*$", env_text, re.MULTILINE)
    if interop_on:
        dc = "MC2_VK_SHADOW_DIRECT_CHAIN=1" in env_text
        print(f"env H5: interop names set {interop_on}; "
              f"MC2_VK_SHADOW_DIRECT_CHAIN=1 present: {'yes' if dc else 'NO'}")
        if not dc:
            failed = True
    return failed


def check_stage_ceiling(text: str, ceiling: float) -> bool:
    """H6: terrain stage_us ceiling -- catches a readback-scale regression numerically."""
    line = next((ln for ln in text.splitlines() if ln.startswith("[VK_TERRAIN_TIER1]")), None)
    if line is None:
        return False  # island-missing already failed in check_islands
    m = re.search(r"\bstage_us=([\d.]+)", line)
    if not m:
        print(f"stage_us: missing on [VK_TERRAIN_TIER1] (FAIL)")
        return True
    val = float(m.group(1))
    ok = val < ceiling
    print(f"terrain stage_us={val} (ceiling {ceiling}) {'ok' if ok else 'FAIL'}")
    return not ok


def check_provenance(proof_dir: Path, expect_sha: str) -> bool:
    """H6: FAIL on dirty:true or sha != expect-sha, from manifest git block AND every
    per-bookmark engine build stamp."""
    failed = False
    expect = expect_sha.lower()

    manifests = list(proof_dir.glob("*_capture_manifest.json"))
    if not manifests:
        print(f"provenance: no *_capture_manifest.json in {proof_dir} (FAIL)")
        return True
    for mf in manifests:
        try:
            git = json.loads(mf.read_text())["identity"]["git"]
        except Exception as exc:  # noqa: BLE001
            print(f"provenance: cannot read {mf.name} git block: {exc} (FAIL)")
            failed = True
            continue
        commit = str(git.get("commit", "")).lower()
        dirty = git.get("dirty")
        okc = commit.startswith(expect) or expect.startswith(commit[:12])
        print(f"provenance {mf.name}: commit={commit[:12]} dirty={dirty} "
              f"{'ok' if (okc and dirty is False) else 'FAIL'}")
        if dirty is not False or not okc:
            failed = True

    bstamps = 0
    for jf in proof_dir.glob("*.json"):
        if jf.name.endswith("_capture_manifest.json"):
            continue
        try:
            build = json.loads(jf.read_text()).get("build")
        except Exception:  # noqa: BLE001
            continue
        if not isinstance(build, dict) or "dirty" not in build:
            continue
        bstamps += 1
        sha = str(build.get("sha", "")).lower()
        dirty = build.get("dirty")
        oks = sha.startswith(expect[:12]) or expect.startswith(sha[:12])
        if dirty is not False or not oks:
            print(f"provenance {jf.name}: build.sha={sha[:12]} dirty={dirty} FAIL")
            failed = True
    print(f"provenance: {bstamps} per-bookmark build stamps checked "
          f"(expect-sha {expect[:12]})")
    if bstamps == 0:
        print("provenance: NO per-bookmark build stamps found (FAIL)")
        failed = True
    return failed


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("log", type=Path)
    ap.add_argument("--env-file", type=Path,
                    help="vk_strict_env.txt written by run_vk_nointerop_strict_visible.py.")
    ap.add_argument("--proof-dir", type=Path,
                    help="Capture out dir (for the fingerprint/provenance check, H6). "
                         "Defaults to the log's parent directory.")
    ap.add_argument("--expect-sha", type=str,
                    help="Required build/commit sha (HEAD the proof was regenerated at). "
                         "When set, enables the provenance gate (H6).")
    ap.add_argument("--min-frames", type=int, default=300,
                    help="Floor for present_frames (H3). Default 300.")
    ap.add_argument("--stage-us-ceiling", type=float, default=2000.0,
                    help="Terrain stage_us ceiling (H6). Default 2000 us.")
    ap.add_argument("--no-resource-strict", action="store_true",
                    help="Do not require the resource-strict env receipt.")
    ap.add_argument("--no-canaries", action="store_true",
                    help="Diagnostic only: skip the H4 canary requirement.")
    args = ap.parse_args()

    text = args.log.read_text(errors="replace")
    failed = False

    print("== H1 forbidden strings ==")
    failed |= check_forbidden(text)
    print("== required strings ==")
    failed |= check_required_strings(text)
    print("== H2 island forbidden_* counters ==")
    failed |= check_islands(text)
    print("== H3 frame consistency ==")
    failed |= check_frame_consistency(text, args.min_frames)
    if not args.no_canaries:
        print("== H4 GL canaries ==")
        failed |= check_canaries(text)
    print("== H6 terrain stage_us ceiling ==")
    failed |= check_stage_ceiling(text, args.stage_us_ceiling)

    if args.env_file:
        print("== H5 + env receipts ==")
        failed |= check_env(args.env_file.read_text(errors="replace"),
                            args.no_resource_strict)

    if args.expect_sha:
        print("== H6 provenance ==")
        proof_dir = args.proof_dir or args.log.parent
        failed |= check_provenance(proof_dir, args.expect_sha)

    print("== VERDICT ==", "FAIL" if failed else "PASS")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
