#!/usr/bin/env python3
"""RENDER-PASS-ATTACHMENT-SCAFFOLD-1 — pass attachment contract checker.

Parses the constexpr `kContracts[]` table in
GameOS/gameos/render_pass_attachment.h and enforces:

  1. DRAW-BUFFER CONSISTENCY — a SingleColor0 pass must NOT declare writes to
     COLOR1/COLOR2 (only COLOR0 is in its draw set); a Backbuffer-target pass must
     use the Backbuffer draw set.
  2. FEEDBACK RULE (static form) — a pass rendering into SceneFBO must not sample a
     LIVE scene attachment it also writes (write COLOR0 + read SceneColor, etc.).
     This is the seed the later FRAME-RESOURCE-FEEDBACK-CHECKER-1 generalizes.
  3. SELF-CONSISTENT feedbackSafe — the asserted `feedbackSafe` flag must equal the
     value recomputed from (target, writes, reads). Catches a stale/false assertion.
  4. COLORMASK DRIFT: native post passes must write RGBA to color0 only;
     surviving GL passes with ownsColorMask=true must keep a binder row.

Exit 0 = PASS.
"""
from __future__ import annotations

import re
import importlib.util
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "GameOS" / "gameos" / "render_pass_attachment.h"
BINDER = ROOT / "GameOS" / "gameos" / "pipeline_binder.cpp"

# These attachment rows describe native passes; no GL binder rows remain.
NATIVE_MASK_PASSES = {"ScreenShadow", "CloudShadow", "Shoreline", "EdgeFog", "FogOob", "Composite"}

LIVE = {"SceneColor", "SceneNormal", "SceneObjectId", "SceneDepth"}
# write-bit index -> the live attachment that bit renders into
WRITE_TO_LIVE = {0: "SceneColor", 1: "SceneNormal", 2: "SceneObjectId"}

ENTRY = re.compile(
    r'\{\s*"(?P<pass>\w+)"\s*,\s*'
    r'TargetFbo::(?P<target>\w+)\s*,\s*'
    r'DrawBufferSetId::(?P<dbuf>\w+)\s*,\s*'
    r'(?P<w0>true|false)\s*,\s*(?P<w1>true|false)\s*,\s*(?P<w2>true|false)\s*,\s*'
    r'\{(?P<reads>[^}]*)\}\s*,\s*'
    r'(?P<mask>true|false)\s*,\s*(?P<safe>true|false)\s*\}',
    re.DOTALL)


def parse_contracts(text: str):
    out = []
    # only scan the kContracts initializer block
    start = text.find("kContracts[] = {")
    body = text[start:] if start >= 0 else text
    for m in ENTRY.finditer(body):
        reads = re.findall(r"FrameResourceId::(\w+)", m.group("reads"))
        reads = [r for r in reads if r != "None"]
        out.append({
            "pass": m.group("pass"),
            "target": m.group("target"),
            "dbuf": m.group("dbuf"),
            "w": [m.group("w0") == "true", m.group("w1") == "true", m.group("w2") == "true"],
            "reads": reads,
            "mask": m.group("mask") == "true",
            "safe": m.group("safe") == "true",
        })
    return out


def compute_feedback_safe(c) -> bool:
    if c["target"] != "SceneFBO":
        return True
    for bit, live in WRITE_TO_LIVE.items():
        if c["w"][bit] and live in c["reads"]:
            return False
    return True


def main() -> int:
    quiet = "--quiet" in sys.argv
    if not HEADER.exists():
        print(f"[pass-attach] ERROR header missing: {HEADER}", file=sys.stderr)
        return 2
    contracts = parse_contracts(HEADER.read_text(encoding="utf-8", errors="replace"))
    if not contracts:
        print("[pass-attach] ERROR no contracts parsed from kContracts[]", file=sys.stderr)
        return 2

    btext = BINDER.read_text(encoding="utf-8", errors="replace") if BINDER.exists() else ""
    fails = []
    spec = importlib.util.spec_from_file_location("native_masks", ROOT / "scripts/check-colormask-ownership.py")
    masks = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(masks)
    for filename in ("vulkan_post_screenfx_native.cpp", "vulkan_post_fog_native.cpp", "engine_present_vk.cpp"):
        source = (ROOT / "GameOS/gameos" / filename).read_text(encoding="utf-8")
        if filename == "engine_present_vk.cpp":
            start = source.find("bool ensureCompositeObjects(")
            end = source.find("bool ensureShadowDebugOverlayObjects(", start)
            source = source[start:end] if start >= 0 and end > start else ""
        fails.extend(filename + ": " + f for f in masks.validate_native_color_mask(source))

    for c in contracts:
        p = c["pass"]
        # 1. draw-buffer consistency
        if c["dbuf"] == "SingleColor0" and (c["w"][1] or c["w"][2]):
            fails.append(f"{p}: SingleColor0 draw set but declares COLOR1/COLOR2 writes")
        if c["target"] == "Backbuffer" and c["dbuf"] not in ("Backbuffer", "SingleColor0"):
            fails.append(f"{p}: Backbuffer target but drawBuffers={c['dbuf']}")
        # 2. feedback rule
        for bit, live in WRITE_TO_LIVE.items():
            if c["target"] == "SceneFBO" and c["w"][bit] and live in c["reads"]:
                fails.append(f"{p}: writes {live} (COLOR{bit}) AND samples it LIVE "
                             f"— feedback loop (use a copy)")
        # 3. self-consistent feedbackSafe
        computed = compute_feedback_safe(c)
        if computed != c["safe"]:
            fails.append(f"{p}: feedbackSafe={c['safe']} but recomputed={computed}")
        # 4. colorMask drift
        if p in NATIVE_MASK_PASSES:
            if c["w"] != [True, False, False]:
                fails.append(f"{p}: native post must write only scene color0")
        elif c["mask"]:
            row = "PostProcess" + p
            if row not in btext:
                fails.append(f"{p}: ownsColorMask=true but '{row}' not in "
                             f"pipeline_binder.cpp rowOwnsColorMask — out of sync")

    if fails:
        print("[pass-attach] FAIL — pass attachment contract violations:", file=sys.stderr)
        for f in fails:
            print(f"  - {f}", file=sys.stderr)
        return 1
    if not quiet:
        print(f"[pass-attach] PASS — {len(contracts)} pass contracts consistent "
              f"(draw-buffer + feedback + colorMask sync).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
