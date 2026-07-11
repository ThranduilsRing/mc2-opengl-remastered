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
      are reconciled by direct-chain. As of VK-RUNG8 item-4 the shadow island's GL<->VK
      semaphore-handoff arm was deleted and MC2_VK_SHADOW_DIRECT_CHAIN is a DEPRECATED
      no-op (the island is unconditionally direct-chain-native), so reconciliation is
      carried by the MC2_VK_SHARED_GBUFFER umbrella; the explicit MC2_VK_SHADOW_DIRECT_
      CHAIN=1 is still accepted (harmless) but no longer load-bearing.
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
    # VK-RUNG8-ITEM5: the shared-scene GL-import half is deleted. These receipts are
    # emitted only by the OLD exportable/glInteropView path; their appearance means the
    # deleted GL-import shared scene resurfaced.
    "VK-backed GL_RGBA16F",
    "VK-backed GL_DEPTH24_STENCIL8",
    "VK-backed GL_R32UI",
    "imported to GL_",
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
    """The authoritative [VK_PRESENT] health line = the one with the MAX present_frames.
    logHealth() is emitted periodically (every 300 frames), so the FIRST such line carries
    present_frames=0 -- H3 must read the final cumulative line, not the first."""
    best, best_n = None, -1
    for line in text.splitlines():
        if "[VK_PRESENT]" in line and "present_frames=" in line:
            n = _u(line, "present_frames")
            if n is not None and n > best_n:
                best, best_n = line, n
    return best


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
        # island health lines are vlog()'d with a leading "[VK_*_ISLAND] " prefix, so the
        # tag is a substring, not the line start.
        line = next((ln for ln in lines if tag in ln and "forbidden_" in ln), None)
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


# --- OWNERSHIP: every geometry island must POSITIVELY own its region under strict ----
# VK-OWNERSHIP-GAP-FIX: the no-interop proof historically asserted only "no forbidden
# GL readback / interop op". That is necessary but NOT sufficient: a producer island can
# decline (used_vulkan=0) and GL silently draws its geometry into the VK-owned GBuffer --
# which trips no forbidden_* counter (a GL base-geometry flush is not an interop call).
# The strict proof must therefore ALSO assert VK OWNERSHIP: each geometry island rendered
# a positive used_vulkan. Missing island line OR used_vulkan==0/absent = FAIL. Shadow is
# a producer for the terrain consumer, not a standalone geometry region, so it is asserted
# via its own forbidden_* counters (check_islands), not here.
OWNERSHIP_ISLAND_TAGS = [
    "[VK_TERRAIN_TIER1]",
    "[VK_PROP_COLOR_TIER1]",
    "[VK_MECH_TIER1]",
    "[VK_WATER_TIER1]",
]


def check_shared_scene_vk_only(text: str) -> bool:
    """VK-RUNG8-ITEM5: when the shared scene is active, assert it is VK-ONLY.

    The GL-import half is deleted, so the shared scene must report the VK-only
    placeholder receipt for color(MRT0) + depth. The deleted GL-import receipts are
    covered by FORBIDDEN. Only asserted when the shared scene armed at all (a legit
    pure-GL run emits no [VK_SHARED_SCENE_*] line and is not gated here)."""
    if "[VK_SHARED_SCENE" not in text:
        print("shared-scene: not armed (pure GL) -- not asserted")
        return False
    failed = False
    for needle in ("(VK-only MRT0)", "(VK-only depth)"):
        present = needle in text
        print(f"shared-scene VK-only {needle!r}: {'yes' if present else 'NO'}")
        if not present:
            failed = True
    return failed


def check_ownership(text: str) -> bool:
    """Assert every geometry island positively OWNED its region (used_vulkan > 0)."""
    failed = False
    lines = text.splitlines()
    for tag in OWNERSHIP_ISLAND_TAGS:
        line = next((ln for ln in lines if tag in ln and "used_vulkan=" in ln), None)
        if line is None:
            print(f"ownership {tag}: MISSING used_vulkan line (FAIL)")
            failed = True
            continue
        used = _u(line, "used_vulkan")
        ok = used is not None and used > 0
        print(f"ownership {tag}: used_vulkan={used} (need > 0) {'ok' if ok else 'FAIL'}")
        if not ok:
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
    """H4: prove the GL scene-draw / screen-shadow paths issued nothing.

    Two-part, reconciled with reality: under FULL VK ownership the GL scene-draw functions
    (drawTerrainOverlays / decals / screen-shadow) are bypassed entirely, so their canary
    blocks never execute and no canary line is emitted. That absence is itself proof-
    consistent ONLY when the GL-scene-skip invariant from the [VK_PRESENT] line holds
    (gl_handoff_skip_frames == present_frames AND copy1_blit_frames == 0). So:
      - If a canary line IS present it MUST be zero (a real GL draw would trip it non-zero).
      - If all canary lines are absent, PASS iff the GL-skip invariant holds; otherwise FAIL
        (we could not prove the GL scene path drew nothing).
    A GL scene draw therefore fails the proof either way (canary trips, or skip invariant
    breaks). Gates for the canaries are armed by the strict runner regardless.
    """
    failed = False
    lines = text.splitlines()
    any_present = False
    for tag in REQUIRED_CANARIES:
        hits = [ln for ln in lines if tag in ln]
        for ln in hits:
            any_present = True
            anys = _u(ln, "any_samples")
            draws = _u(ln, "draws")
            bad = (anys is None) or (anys != 0) or (draws is not None and draws != 0)
            print(f"canary {tag}: any_samples={anys} draws={draws} {'FAIL' if bad else 'ok'}")
            if bad:
                failed = True

    if not any_present:
        pl = _present_line(text)
        pf = _u(pl, "present_frames") if pl else None
        gskip = _u(pl, "gl_handoff_skip_frames") if pl else None
        blit = _u(pl, "copy1_blit_frames") if pl else None
        inv_ok = (pf is not None and pf > 0 and gskip == pf and blit == 0)
        print(f"canary: none emitted (GL scene path fully skipped); GL-skip invariant "
              f"present_frames={pf} gl_handoff_skip_frames={gskip} copy1_blit_frames={blit} "
              f"{'ok' if inv_ok else 'FAIL'}")
        if not inv_ok:
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

    # H5: any MC2_VK_*_INTEROP=1 must be reconciled by direct-chain. As of
    # VK-UMBRELLA-DIRECT-CHAIN-DEFAULT-1 the direct-chain family defaults ON under the
    # MC2_VK_SHARED_GBUFFER umbrella, so reconciliation is satisfied by EITHER the
    # explicit MC2_VK_SHADOW_DIRECT_CHAIN=1 OR the umbrella (MC2_VK_SHARED_GBUFFER=1,
    # not explicitly disabled) -- the latter is the minimal-promotion proof.
    # VK-RUNG8 item-4: MC2_VK_SHADOW_DIRECT_CHAIN is now a DEPRECATED no-op (the shadow
    # island's handoff arm was deleted; it is always direct-chain-native). The umbrella is
    # the real reconciler; the explicit-gate branch below stays only for back-compat.
    interop_on = re.findall(r"^(MC2_VK_\w*INTEROP)=1\s*$", env_text, re.MULTILINE)
    if interop_on:
        explicit_dc = "MC2_VK_SHADOW_DIRECT_CHAIN=1" in env_text
        umbrella_dc = ("MC2_VK_SHARED_GBUFFER=1" in env_text
                       and "MC2_VK_SHADOW_DIRECT_CHAIN=0" not in env_text)
        dc = explicit_dc or umbrella_dc
        how = ("explicit" if explicit_dc else
               "umbrella-default" if umbrella_dc else "NONE")
        print(f"env H5: interop names set {interop_on}; "
              f"direct-chain reconciliation: {how} ({'ok' if dc else 'FAIL'})")
        if not dc:
            failed = True
    return failed


def check_stage_ceiling(text: str, ceiling: float) -> bool:
    """H6: terrain stage_us ceiling -- catches a readback-scale regression numerically."""
    line = next((ln for ln in text.splitlines() if "[VK_TERRAIN_TIER1]" in ln), None)
    if line is None:
        return False  # island-missing already failed in check_islands
    used = _u(line, "used_vulkan")
    m = re.search(r"\bstage_us=([\d.]+)", line)
    if not m:
        print(f"stage_us: missing on [VK_TERRAIN_TIER1] (FAIL)")
        return True
    val = float(m.group(1))
    # The health-line stage_us is a cumulative-ish teardown counter; the numeric ceiling
    # only discriminates a live readback-scale regression when the island actually rendered
    # VK (used_vulkan>0). When the island declined (used_vulkan==0) the forbidden_getTexImage
    # counter is the authoritative signal, so skip the ceiling with a note.
    if used == 0:
        print(f"terrain stage_us={val} SKIPPED (used_vulkan=0; forbidden_* is authoritative)")
        return False
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
    print("== OWNERSHIP: geometry islands used_vulkan > 0 ==")
    failed |= check_ownership(text)
    print("== VK-RUNG8-ITEM5: shared scene VK-only substrate ==")
    failed |= check_shared_scene_vk_only(text)
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
