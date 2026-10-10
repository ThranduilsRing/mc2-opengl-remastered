#!/usr/bin/env python3
"""FRAME-RESOURCE-FEEDBACK-CHECKER-1 — scene-color feedback-loop guard.

THE RULE (simple + brutal): no pass may sample the LIVE scene color attachment
`sceneColorTex_` (COLOR0) while it could be the active render target. Mid-frame
consumers (VFX distortion, blackbody, soft-color particles, projected decals) MUST
sample the feedback-safe copy `sceneColorCopyTex_` (via getSceneColorCopyTexture()),
which is snapshotted by copySceneColorForVfx() before VFX draws. This protects the
scene-color-grab / distortion work from the classic read-from-bound-attachment loop.

Mechanism: scan the render source for any `glBindTexture*(..., sceneColorTex_)`
sampler bind. A bind is ALLOWED only if it is either
  (a) the allocation/config bind (immediately after glGenTextures(&sceneColorTex_)), or
  (b) explicitly justified with an inline `FEEDBACK-SAFE:` comment within 3 lines
      (e.g. the endScene composite resolve, whose target is the backbuffer).
Any other live-color sampler bind FAILS — use the copy or justify it.

Also asserts the safe path still exists: copySceneColorForVfx + sceneColorCopyTex_ +
getSceneColorCopyTexture, so the copy plumbing can't be silently deleted out from
under the consumers.

Scope note (v1): guards `sceneColorTex_` only. `sceneDepthTex_` / `sceneNormalTex_`
are legitimately sampled live by post-fx (depth-write OFF, COLOR1 not in the
SingleColor draw set) — the accepted read-only pattern, NOT a feedback loop — so they
are out of scope here. The static table check (check-pass-attachment-contract.py)
already enforces the write+read-same-attachment rule across all attachments.

Exit 0 = PASS.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PP_PIECES = sorted((ROOT / "GameOS" / "gameos").glob("gos_postprocess*.cpp"))   # gos_postprocess.cpp + its split pieces
SCAN = PP_PIECES + [
    ROOT / "GameOS" / "gameos" / "gos_particle_bridge.cpp",
    ROOT / "GameOS" / "gameos" / "gameos_graphics.cpp",
]

BIND = re.compile(r"glBindTexture\w*\s*\([^;]*?\bsceneColorTex_\b")
GEN = re.compile(r"glGenTextures\s*\([^;]*&\s*sceneColorTex_")
MARKER = "FEEDBACK-SAFE:"


def main() -> int:
    quiet = "--quiet" in sys.argv
    fails = []

    for f in SCAN:
        if not f.exists():
            continue
        lines = f.read_text(encoding="utf-8", errors="replace").splitlines()
        for i, ln in enumerate(lines):
            if not BIND.search(ln):
                continue
            # (a) allocation/config bind: a glGenTextures(&sceneColorTex_) in the prior 2 lines
            if any(GEN.search(lines[j]) for j in range(max(0, i - 2), i)):
                continue
            # (b) inline FEEDBACK-SAFE justification within 6 lines above (allows a
            # short multi-line rationale + intervening glActiveTexture)
            if any(MARKER in lines[j] for j in range(max(0, i - 6), i + 1)):
                continue
            fails.append(f"{f.name}:{i+1}: samples LIVE sceneColorTex_ (COLOR0) — "
                         f"feedback-loop risk. Sample sceneColorCopyTex_ "
                         f"(getSceneColorCopyTexture) or add an inline "
                         f"'// FEEDBACK-SAFE: <reason>' comment.")

    # safe-path existence guard
    pptext = chr(10).join(p.read_text(encoding="utf-8", errors="replace") for p in PP_PIECES)
    for sym in ("copySceneColorForVfx", "sceneColorCopyTex_", "getSceneColorCopyTexture"):
        if sym not in pptext:
            fails.append(f"safe-path symbol '{sym}' missing from the gos_postprocess pieces "
                         f"— scene-color copy plumbing removed; consumers would have to "
                         f"sample the live attachment")

    if fails:
        print("[frame-feedback] FAIL — scene-color feedback-loop risks:", file=sys.stderr)
        for x in fails:
            print(f"  - {x}", file=sys.stderr)
        return 1
    if not quiet:
        print("[frame-feedback] PASS — no unguarded live sceneColorTex_ sample; "
              "copy plumbing intact.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
