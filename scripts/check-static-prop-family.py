#!/usr/bin/env python3
"""STATIC-PROP-FAMILY-LEDGER-1 — static-prop pass label/ownership checker.

The static-prop batcher is a NEST of draw paths (opaque / alpha-test / depth-prepass /
shadow / coalesce variants). STATIC-PROP-PATH-OWNERSHIP-RECON-1 found the live COLOR and
DEPTH applyPipeline calls were UNLABELED (no dbgName), so they were invisible to the
[PIPELINE_BIND] / [FRAME_PLAN] self-report — the exact terrain blind spot.

THE RULE (simple + brutal): every pipeline_binder::applyPipeline(...) call in
gos_static_prop_batcher.cpp MUST pass a string-literal dbgName, so every static-prop
pipeline bind is observable (bind-positive provable). An unlabeled bind = FAIL.

Also asserts the ledger doc exists (docs/render-backend-seams/static-prop-family-ledger.md).

Exit 0 = PASS.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BATCHER = ROOT / "GameOS" / "gameos" / "gos_static_prop_batcher.cpp"
LEDGER = ROOT / "docs" / "render-backend-seams" / "static-prop-family-ledger.md"


def applypipeline_calls(text: str):
    """Yield (line_no, call_text) for each applyPipeline( ... ) call, balanced to ')'."""
    # Only real calls (the pipeline_binder:: qualifier); bare "applyPipeline()" in
    # comments/prose is not a call.
    for m in re.finditer(r"pipeline_binder::applyPipeline\s*\(", text):
        i = m.end()
        depth = 1
        while i < len(text) and depth:
            if text[i] == "(":
                depth += 1
            elif text[i] == ")":
                depth -= 1
            i += 1
        call = text[m.start():i]
        line = text.count("\n", 0, m.start()) + 1
        yield line, call


def main() -> int:
    quiet = "--quiet" in sys.argv
    if not BATCHER.exists():
        print(f"[static-prop] ERROR missing: {BATCHER}", file=sys.stderr)
        return 2
    text = BATCHER.read_text(encoding="utf-8", errors="replace")

    fails = []
    n = 0
    for line, call in applypipeline_calls(text):
        n += 1
        # a dbgName literal is any "..." inside the call args
        if not re.search(r'"[^"]+"', call):
            fails.append(f"gos_static_prop_batcher.cpp:{line}: applyPipeline without a "
                         f"string dbgName — static-prop bind is invisible to "
                         f"[PIPELINE_BIND]/[FRAME_PLAN] (add a label)")

    # Instance positions come from StaticPropOrigin.h: modelMatrix[12..14] is always 0 (translation is [3][7][11]).
    for src in sorted((ROOT / "GameOS" / "gameos").glob("gos_static_prop_*.cpp")):
        for i, ln in enumerate(src.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            if re.search(r"modelMatrix\s*\[\s*1[234]\s*\]", ln):
                fails.append(f"{src.name}:{i}: modelMatrix[12..14] read (always 0); use RenderCore/StaticPropOrigin.h")

    if not LEDGER.exists():
        fails.append(f"ledger doc missing: {LEDGER.relative_to(ROOT)}")

    if fails:
        print("[static-prop] FAIL — static-prop family ownership gaps:", file=sys.stderr)
        for f in fails:
            print(f"  - {f}", file=sys.stderr)
        return 1
    if not quiet:
        print(f"[static-prop] PASS — {n} applyPipeline call(s) in the static-prop "
              f"batcher all carry a dbgName; ledger present.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
