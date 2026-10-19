#!/usr/bin/env python3
"""MDI-SUBMISSION-SCAFFOLD-1 — indirect-draw submission ownership checker.

GL indirect draws (glMultiDraw*Indirect / glDraw*Indirect) require the scaffold.
Native Vulkan indirect draws provide a scan witness after GL draw retirement.
MDI-SUBMISSION-LEDGER-RECON-1 catalogued every submitter. This checker keeps the
submission layer observable so a new indirect submitter can't slip in unnamed (the
terrain rake at the draw-submission level).

THE RULE (TU-level, robust to line drift): any translation unit that issues a real
GL indirect draw MUST (a) #include "mdi_submit.h" and (b) contain at least one
mdi_submit::trace(...) call, so every indirect-submitting file participates in the
[MDI_SUBMIT] self-report. A file that adds an indirect draw without wiring the scaffold
FAILs.

Also asserts the ledger doc exists (docs/render-backend-seams/mdi-submission-ledger-recon-1.md).

Comments/strings mentioning the GL calls are ignored — only real call statements count.

Exit 0 = PASS.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LEDGER = ROOT / "docs" / "render-backend-seams" / "mdi-submission-ledger-recon-1.md"

# Directories that host the renderer.
SCAN_DIRS = ["GameOS", "mclib", "RenderCore", "RenderWorld", "GameAdapters", "code"]

# A real indirect-draw CALL statement (line begins with optional ws then the GL fn + '(').
CALL = re.compile(r"^\s*gl(Multi)?Draw(Elements|Arrays)Indirect\s*\(", re.M)
NATIVE_CALL = re.compile(r"^\s*vkCmdDraw(?:Indexed)?Indirect(?:Count)?\s*\(", re.M)


def main() -> int:
    quiet = "--quiet" in sys.argv
    fails = []
    tus_with_indirect = []
    native_tus = []

    for d in SCAN_DIRS:
        base = ROOT / d
        if not base.exists():
            continue
        for f in base.rglob("*.cpp"):
            text = f.read_text(encoding="utf-8", errors="replace")
            if NATIVE_CALL.search(text):
                native_tus.append(f)
            if not CALL.search(text):
                continue
            tus_with_indirect.append(f)
            rel = f.relative_to(ROOT)
            if '#include "mdi_submit.h"' not in text:
                fails.append(f"{rel}: issues an indirect draw but does not "
                             f'#include "mdi_submit.h"')
            if "mdi_submit::trace(" not in text:
                fails.append(f"{rel}: issues an indirect draw but has no "
                             f"mdi_submit::trace() — submission is unnamed in [MDI_SUBMIT]")

    if not tus_with_indirect and not native_tus:
        fails.append("no GL or native indirect-draw TUs found — scan path or detection regression")
    if not LEDGER.exists():
        fails.append(f"ledger doc missing: {LEDGER.relative_to(ROOT)}")

    if fails:
        print("[mdi-submit] FAIL — indirect-draw submission ownership gaps:", file=sys.stderr)
        for x in fails:
            print(f"  - {x}", file=sys.stderr)
        return 1
    if not quiet:
        print(f"[mdi-submit] PASS — {len(tus_with_indirect)} indirect-draw TU(s) all "
              f"wire mdi_submit (#include + trace); {len(native_tus)} native TU(s); ledger present.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
