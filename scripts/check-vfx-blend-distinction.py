#!/usr/bin/env python3
"""VFX-VISUAL-GATE-1 — blend-mode DISTINCTION checker (the trap-catcher).

check-pipeline-desc.py only verifies the `blend` FIELD EXISTS (completeness). It
does NOT verify the VALUE, so it would NOT catch a tube->billboard collapse. This
checker asserts the exact BlendMode per registered VFX row, so the gate FAILS the
moment additive tube (ONE/ONE) is collapsed into billboard/mesh additive
(SRC_ALPHA/ONE) — the distinction the operator flagged as load-bearing:

    billboard/mesh additive = SRC_ALPHA / ONE  -> BlendMode::AdditiveSrcAlphaOne
    tube     additive       = ONE / ONE        -> BlendMode::AdditiveOneOne
    all alpha variants                          -> BlendMode::AlphaBlend

Parses RenderCore/PipelineRegistry.cpp s_descs rows by their `// [N] <Name>`
header + the row's `/* blend */ BlendMode::<X>` initializer. Exit 0 = PASS.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REGISTRY = ROOT / "RenderCore" / "PipelineRegistry.cpp"

# The contract. Collapsing tube into AdditiveSrcAlphaOne (or vice-versa) FAILS.
EXPECTED = {
    "VfxTubeAlpha":         "AlphaBlend",
    "VfxTubeAdditive":      "AdditiveOneOne",       # ONE/ONE — the DISTINCTION
    "VfxMeshAlpha":         "AlphaBlend",
    "VfxMeshAdditive":      "AdditiveSrcAlphaOne",
}

# header: `// [14] VfxTubeAdditive — ...`  then later `BlendMode::AdditiveOneOne`
ROW_HEADER = re.compile(r"//\s*\[(\d+)\]\s*(Vfx\w+)\b")
BLEND_INIT = re.compile(r"/\*\s*blend\s*\*/\s*BlendMode::(\w+)")


def parse_vfx_blends(text: str) -> dict[str, str]:
    """Map VFX row name -> its blend enum, by walking each `// [N] VfxX` header to
    the first `/* blend */ BlendMode::Y` that follows."""
    found: dict[str, str] = {}
    lines = text.splitlines()
    pending = None
    for ln in lines:
        h = ROW_HEADER.search(ln)
        if h:
            pending = h.group(2)
            continue
        if pending:
            b = BLEND_INIT.search(ln)
            if b:
                found[pending] = b.group(1)
                pending = None
    return found


def native_blend_issues(text: str) -> list[str]:
    """Pin the visible Vulkan blend factors and its three pipeline variants."""
    text = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)
    compact = re.sub(r"\s+", "", text)
    required = [
        "cba[0].srcColorBlendFactor=oneOne?VK_BLEND_FACTOR_ONE:VK_BLEND_FACTOR_SRC_ALPHA;",
        "cba[0].dstColorBlendFactor=additive?VK_BLEND_FACTOR_ONE:VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;",
        "cba[0].srcAlphaBlendFactor=oneOne?VK_BLEND_FACTOR_ONE:VK_BLEND_FACTOR_SRC_ALPHA;",
        "cba[0].dstAlphaBlendFactor=additive?VK_BLEND_FACTOR_ONE:VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;",
        "s->alphaPipe=make_pipeline(s,w,h,false);",
        "s->addPipe=make_pipeline(s,w,h,true);",
        "s->oneOnePipe=make_pipeline(s,w,h,true,true);",
    ]
    return ["Vulkan VFX blend contract missing: " + clause for clause in required if clause not in compact]


def main() -> int:
    quiet = "--quiet" in sys.argv
    if not REGISTRY.exists():
        print(f"[vfx-blend] ERROR registry missing: {REGISTRY}", file=sys.stderr)
        return 2
    found = parse_vfx_blends(REGISTRY.read_text(encoding="utf-8", errors="replace"))

    fails = native_blend_issues((ROOT / "GameOS/gameos/vulkan_vfx_island.cpp").read_text(encoding="utf-8"))
    for name, want in EXPECTED.items():
        got = found.get(name)
        if got is None:
            fails.append(f"{name}: row not found in s_descs (expected blend {want})")
        elif got != want:
            fails.append(f"{name}: blend={got} but MUST be {want} "
                         f"(additive-collapse / wrong blend = the trap)")

    # Cross-check the distinction explicitly: tube additive != billboard additive.
    t = found.get("VfxTubeAdditive")
    b = found.get("VfxMeshAdditive")
    if t is not None and b is not None and t == b:
        fails.append(f"VfxTubeAdditive ({t}) == VfxMeshAdditive ({b}) — "
                     "tube additive (ONE/ONE) COLLAPSED into billboard "
                     "(SRC_ALPHA/ONE). This is exactly the forbidden collapse.")

    if fails:
        print("[vfx-blend] FAIL — VFX blend-mode distinction broken:", file=sys.stderr)
        for f in fails:
            print(f"  - {f}", file=sys.stderr)
        return 1
    if not quiet:
        print("[vfx-blend] PASS — 4 GL VFX rows and native billboard blend-distinct "
              "(tube=AdditiveOneOne, billboard/mesh=AdditiveSrcAlphaOne, "
              "alpha=AlphaBlend).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
