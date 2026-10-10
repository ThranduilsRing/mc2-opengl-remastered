#!/usr/bin/env python3
"""SHADER-VARIANT-LOCKSTEP-CHECKER-1 — depth-prepass / color variant lockstep.

THE INVARIANT: the static-prop DEPTH-PREPASS program must be built with the SAME shader
variant (prefix define-set) as the COLOR program. If color uses one variant (e.g.
MC2_COALESCE / MC2_USE_VIEW_UNIFORMS / MC2_STATICPROP_PBR_SLOTS) and depth uses another,
the depth prepass lays gl_Position from a different vertex path and a later GL_EQUAL color
pass can fail SILENTLY (depth mismatch -> missing/flickering props in complex scenes). Not
cosmetic — a real render-correctness invariant.

Today the invariant holds by construction (gos_static_prop_batcher.cpp): the depth program
reuses the color prefix VARIABLES rather than re-deriving its own. This checker protects
that against regression — a future edit that builds depthPrefix from a fresh "#version"
string, or selects coalesce by a different condition than the color path, FAILs.

Enforced (static source lint on gos_static_prop_batcher.cpp):
  1. The "static_prop_depth" makeProgram call passes `depthPrefix` (a variable), not an
     inline-built prefix string.
  2. `depthPrefix` is assigned from coalescePrefix / legacyPrefix (the SAME prefixes used
     to build the color programs) — i.e. it reuses the color define-set.
  3. `depthUsesCoalesce` mirrors the color coalesce selection (s_staticPropProgramCoalesce
     != 0), so color and depth pick the same variant.
  4. The color programs are actually built from legacyPrefix and coalescePrefix (anchor:
     so reusing them in depth genuinely means lockstep).

Mech has no color/depth-prepass pair (it uses a separate shadow program), so there is no
mech lockstep pair to check. Terrain/water have no paired color/depth-prepass variants.

Exit 0 = PASS.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPB = ROOT / "GameOS" / "gameos" / "gos_static_prop_batcher_program.cpp"   # loadProgramsIfNeeded builds the color and depth programs


def main() -> int:
    quiet = "--quiet" in sys.argv
    if not SPB.exists():
        print(f"[shader-variant] ERROR missing: {SPB}", file=sys.stderr)
        return 2
    t = SPB.read_text(encoding="utf-8", errors="replace")
    fails = []

    # 1. depth program uses the depthPrefix variable (not an inline literal)
    depth_make = re.search(
        r'makeProgram\(\s*"static_prop_depth"\s*,[^;]*?,\s*([A-Za-z_]\w*)\s*\)',
        t, re.DOTALL)
    if not depth_make:
        fails.append('cannot find makeProgram("static_prop_depth", ...) call')
    elif depth_make.group(1) != "depthPrefix":
        fails.append(f'depth program prefix arg is `{depth_make.group(1)}`, expected '
                     f'`depthPrefix` (reuse the color prefix, do not inline a new one)')

    # 2. depthPrefix reuses coalescePrefix / legacyPrefix (the color define-sets)
    depth_assign = re.search(
        r'depthPrefix\s*=\s*[^;]*coalescePrefix\.c_str\(\)\s*:\s*legacyPrefix\.c_str\(\)',
        t, re.DOTALL)
    if not depth_assign:
        fails.append("depthPrefix is not assigned from coalescePrefix/legacyPrefix — depth "
                     "variant may drift from the color variant (lockstep broken)")

    # 3. depthUsesCoalesce mirrors the color coalesce selection
    if not re.search(r'depthUsesCoalesce\s*=\s*\(\s*s_staticPropProgramCoalesce\s*!=\s*0\s*\)', t):
        fails.append("depthUsesCoalesce does not mirror (s_staticPropProgramCoalesce != 0) "
                     "— depth may pick a different variant than the color path")

    # 4. anchor: color programs are built from legacyPrefix and coalescePrefix
    if not re.search(r'makeProgram\(\s*"static_prop"\s*,[^;]*legacyPrefix\.c_str\(\)', t, re.DOTALL):
        fails.append("color legacy program no longer built from legacyPrefix — anchor lost")
    if not re.search(r'coalescePrefix\.c_str\(\)', t):
        fails.append("coalescePrefix no longer used to build the color coalesce program — anchor lost")

    if fails:
        print("[shader-variant] FAIL — static-prop depth/color variant lockstep broken:", file=sys.stderr)
        for f in fails:
            print(f"  - {f}", file=sys.stderr)
        return 1
    if not quiet:
        print("[shader-variant] PASS — static-prop depth-prepass reuses the color "
              "variant prefix (coalesce/legacy lockstep intact).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
