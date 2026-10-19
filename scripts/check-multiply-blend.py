#!/usr/bin/env python3
"""BLENDMODE-MULTIPLY-1 — multiply blend value-checker (the trap-catcher).

Mirrors check-vfx-blend-distinction.py for the multiplicative post-fx darkening
passes. check-pipeline-desc.py only verifies the `blend` FIELD exists; it does NOT
verify the VALUE, so it would not catch a multiply row being silently swapped to an
additive/alpha/opaque blend. This asserts the exact BlendMode per registered
multiply row, so the gate FAILS the moment any of them is conflated:

    screenShadow / cloudShadow / shoreline / ssaoApply = DST_COLOR / ZERO
        -> BlendMode::Multiply

Checks the surviving GL SSAO row and the native screen/cloud/shoreline builder.
Parses RenderCore/PipelineRegistry.cpp s_descs rows by their `// [N] <Name>` header
+ the row's `/* blend */ BlendMode::<X>` initializer. Exit 0 = PASS.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REGISTRY = ROOT / "RenderCore" / "PipelineRegistry.cpp"

# The contract. Any of these as non-Multiply (additive/alpha/opaque) FAILS.
EXPECTED = {
    "PostProcessSsaoApply":    "Multiply",
}

ROW_HEADER = re.compile(r"//\s*\[(\d+)\]\s*(PostProcess\w+)\b")
BLEND_INIT = re.compile(r"/\*\s*blend\s*\*/\s*BlendMode::(\w+)")


def parse_blends(text: str) -> dict[str, str]:
    found: dict[str, str] = {}
    pending = None
    for ln in text.splitlines():
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


def validate_native_multiply(text: str) -> list[str]:
    code = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)
    required = {
        "blendEnable": "VK_TRUE", "srcColorBlendFactor": "VK_BLEND_FACTOR_DST_COLOR",
        "dstColorBlendFactor": "VK_BLEND_FACTOR_ZERO", "colorBlendOp": "VK_BLEND_OP_ADD",
    }
    return ["native screen/cloud/shoreline multiply: wrong " + name
            for name, value in required.items()
            if not re.search(r"\bcba\." + name + r"\s*=\s*" + value + r"\s*;", code)]


def main() -> int:
    quiet = "--quiet" in sys.argv
    if not REGISTRY.exists():
        print(f"[mul-blend] ERROR registry missing: {REGISTRY}", file=sys.stderr)
        return 2
    found = parse_blends(REGISTRY.read_text(encoding="utf-8", errors="replace"))

    native = ROOT / "GameOS/gameos/vulkan_post_screenfx_native.cpp"
    fails = validate_native_multiply(native.read_text(encoding="utf-8"))
    for name, want in EXPECTED.items():
        got = found.get(name)
        if got is None:
            fails.append(f"{name}: row not found in s_descs (expected blend {want})")
        elif got != want:
            fails.append(f"{name}: blend={got} but MUST be {want} "
                         f"(DST_COLOR/ZERO multiply; wrong blend = conflation)")

    if fails:
        print("[mul-blend] FAIL — multiply blend rows wrong:", file=sys.stderr)
        for f in fails:
            print(f"  - {f}", file=sys.stderr)
        return 1
    if not quiet:
        print("[mul-blend] PASS — GL SSAO and native screen/cloud/shoreline multiply "
              "(DST_COLOR/ZERO).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
