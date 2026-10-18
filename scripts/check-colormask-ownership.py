#!/usr/bin/env python3
"""COLORMASK-OWNERSHIP-1 — colorMask opt-in sanity checker.

When a PipelineDesc row OPTS IN to colorMask ownership (applyPipeline emits
glColorMaski from its colorAttachments), masking color0 (the scene/HDR color
attachment) OFF would draw a BLACK frame. This asserts every colorMask-owned row
keeps color0=true — the black-frame guard. The opt-in set MUST stay in sync with
pipeline_binder.cpp::rowOwnsColorMask.

Parses RenderCore/PipelineRegistry.cpp s_descs rows by `// [N] <Name>` header +
the row's `/* colorAttachments */ { c0, c1, c2 }`. Exit 0 = PASS.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REGISTRY = ROOT / "RenderCore" / "PipelineRegistry.cpp"
BINDER = ROOT / "GameOS" / "gameos" / "pipeline_binder.cpp"

# Rows that opt in to colorMask ownership. KEEP IN SYNC with rowOwnsColorMask().
# Composite re-opts-in: (the beginScene all-TRUE keystone heals
# its set-only leak each frame). This checker is the black-frame guard: an opted-in row
# masking color0 (the scene color attachment) OFF would draw black -> FAIL.
# The whole PostProcess-phase family opts in. Each modulates
# the scene color attachment (color0) only; registry rows are {true,false,false} so color0
# stays true (no black frame) and 1/2 OFF protects the GBuffer. Keep in sync with
# rowOwnsColorMask() in pipeline_binder.cpp.
OPTED_IN = {
    "PostProcessComposite",
    "PostProcessSsaoApply",
    "PostProcessScreenShadow",
    "PostProcessCloudShadow",
    "PostProcessShoreline",
    "PostProcessEdgeFog",
    "PostProcessFogOob",
}

ROW_HEADER = re.compile(r"//\s*\[(\d+)\]\s*([A-Za-z]\w+)\b")
COLOR_ATT = re.compile(
    r"/\*\s*colorAttachments\s*\*/\s*\{\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*\}")


def parse_color_attachments(text: str) -> dict[str, tuple[str, str, str]]:
    found: dict[str, tuple[str, str, str]] = {}
    pending = None
    for ln in text.splitlines():
        h = ROW_HEADER.search(ln)
        if h:
            pending = h.group(2)
            continue
        if pending:
            c = COLOR_ATT.search(ln)
            if c:
                found[pending] = (c.group(1), c.group(2), c.group(3))
                pending = None
    return found


def main() -> int:
    quiet = "--quiet" in sys.argv
    if not REGISTRY.exists():
        print(f"[colormask] ERROR registry missing: {REGISTRY}", file=sys.stderr)
        return 2
    found = parse_color_attachments(REGISTRY.read_text(encoding="utf-8", errors="replace"))

    # Drift guard: warn if the binder's opt-in set diverges from OPTED_IN.
    drift = ""
    if BINDER.exists():
        btext = BINDER.read_text(encoding="utf-8", errors="replace")
        for name in OPTED_IN:
            if name not in btext:
                drift = (f"opt-in row {name} not referenced in pipeline_binder.cpp "
                         f"rowOwnsColorMask — OPTED_IN out of sync")

    fails = []
    for name in OPTED_IN:
        att = found.get(name)
        if att is None:
            fails.append(f"{name}: colorMask-owned row not found in s_descs")
        elif att[0] != "true":
            fails.append(f"{name}: owns colorMask but color0={att[0]} — masking the "
                         f"scene color attachment OFF = BLACK FRAME (must be true)")
    if drift:
        fails.append(drift)

    if fails:
        print("[colormask] FAIL — colorMask opt-in rows unsafe:", file=sys.stderr)
        for f in fails:
            print(f"  - {f}", file=sys.stderr)
        return 1
    if not quiet:
        print(f"[colormask] PASS — {len(OPTED_IN)} colorMask-owned row(s) keep color0=true "
              f"(no black-frame): {sorted(OPTED_IN)}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
