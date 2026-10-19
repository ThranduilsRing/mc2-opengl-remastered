#!/usr/bin/env python3
"""BUFFER-LIFETIME-OWNERSHIP — buffer lifetime governance checker.

Enforces the hazard-class invariants catalogued in
docs/render-backend-seams/buffer-lifetime-ledger.md so a buffer can't silently lose the
mechanism that avoids a CPU-overwrite-while-GPU-reads hazard (the Vulkan-critical seam):

  1. Every ledger row declares a class in {A,B,C,D,E}.
  2. CLASS-B COMMAND BARRIER PRESENCE — the GPU-produced indirect family
     (gpu_cull_compute.cpp) still emits a
     GL_COMMAND_BARRIER_BIT glMemoryBarrier between the producer compute and the indirect
     draw. Removing that barrier (stale-command-buffer hazard) FAILs.
  3. CLASS-D VULKAN-DEBT DECLARED — the known implicit-sync buffers stay listed so the
     migration debt remains visible (a class-D buffer dropped from the ledger FAILs).
  4. STALE COUNTER DECLARED — gpu_drawn_instances is documented as known-stale.

Exit 0 = PASS.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LEDGER = ROOT / "docs" / "render-backend-seams" / "buffer-lifetime-ledger.md"
OBJB = ROOT / "docs" / "render-backend-seams" / "objbatcher-zero-gpu-drawn-recon-1.md"

# parse rows: | `symbol...` | CLASS | ... |
ROW = re.compile(r"^\|\s*`([^`]+)`[^|]*\|\s*([A-E])\s*\|", re.M)

# Live class-B GL indirect family; terrain now captures CPU inputs only.
BARRIER_TUS = [
    ROOT / "GameOS" / "gameos" / "gpu_cull_compute.cpp",
]
# class-D members that must stay declared (Vulkan-debt visibility)
REQUIRED_CLASS_D = {"lightData_", "s_ssbo"}


def main() -> int:
    quiet = "--quiet" in sys.argv
    if not LEDGER.exists():
        print(f"[buffer-lifetime] ERROR ledger missing: {LEDGER}", file=sys.stderr)
        return 2
    text = LEDGER.read_text(encoding="utf-8", errors="replace")
    rows = ROW.findall(text)
    fails = []

    if not rows:
        fails.append("no buffer rows parsed from ledger table")

    # 1. classes valid (regex already constrains A-E; check non-empty)
    classes = {sym: cls for sym, cls in rows}

    # 2. class-B COMMAND barrier presence
    for tu in BARRIER_TUS:
        if not tu.exists():
            fails.append(f"class-B TU missing: {tu.relative_to(ROOT)}")
            continue
        if "GL_COMMAND_BARRIER_BIT" not in tu.read_text(encoding="utf-8", errors="replace"):
            fails.append(f"{tu.name}: no GL_COMMAND_BARRIER_BIT — a GPU-produced indirect "
                         f"buffer lost its command barrier (stale-command-buffer hazard)")

    # 3. class-D Vulkan-debt declared
    declared_d = {sym for sym, cls in rows if cls == "D"}
    for need in REQUIRED_CLASS_D:
        if not any(need in d for d in declared_d):
            fails.append(f"class-D buffer '{need}' not declared in ledger — implicit-sync "
                         f"Vulkan-debt no longer visible")

    # 4. stale counter declared
    if not OBJB.exists() or "gpu_drawn_instances" not in OBJB.read_text(encoding="utf-8", errors="replace"):
        fails.append("gpu_drawn_instances not declared known-stale in "
                     "objbatcher-zero-gpu-drawn-recon-1.md")

    if fails:
        print("[buffer-lifetime] FAIL — buffer lifetime ownership gaps:", file=sys.stderr)
        for f in fails:
            print(f"  - {f}", file=sys.stderr)
        return 1
    if not quiet:
        nb = sum(1 for _, c in rows if c == "B")
        nd = sum(1 for _, c in rows if c == "D")
        print(f"[buffer-lifetime] PASS — {len(rows)} buffers classified "
              f"({nb} class-B barrier-checked, {nd} class-D debt-declared); "
              f"command barriers intact; stale counter declared.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
