#!/usr/bin/env python3
"""check-toolchain-bom.py -- lightweight toolchain bill-of-materials check.

VALIDATION-SCAFFOLD-PREFLIGHT-1 / TOOLCHAIN-BOM-CHECK-1.

Detects presence/absence of the build & asset tools the upcoming pipeline lanes
depend on, repo-relative (no machine-only absolute paths, no build cache). Prints
a summary table. Returns nonzero ONLY when a REQUIRED_NOW tool is missing; FUTURE
tools that are absent WARN but do not fail. Mirrors docs/toolchain-bom.md.

Usage:
  py -3 scripts/check-toolchain-bom.py
  py -3 scripts/check-toolchain-bom.py --verbose

Exit 0 = all REQUIRED_NOW tools present. Exit 1 = a REQUIRED_NOW tool missing.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

REQUIRED_NOW = "REQUIRED_NOW"
FUTURE = "FUTURE"


def file_exists(rel: str) -> bool:
    return (ROOT / rel).exists()


def text_contains(rel: str, needle: str) -> bool:
    p = ROOT / rel
    if not p.exists():
        return False
    try:
        return needle in p.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return False


def detect_gltfpack() -> bool:
    # Only count gltfpack if it is WIRED at repo level (CMake/scripts), NOT the
    # dead vendored source inside 3rdparty/meshoptimizer/gltf/.
    if text_contains("CMakeLists.txt", "gltfpack"):
        return True
    this_file = Path(__file__).resolve()
    scripts_dir = ROOT / "scripts"
    if scripts_dir.is_dir():
        for p in scripts_dir.rglob("*"):
            if p.resolve() == this_file:
                continue  # don't match our own source (it names the tool)
            if p.is_file() and p.suffix in (".py", ".sh", ".bat", ".cmake"):
                try:
                    if "gltfpack" in p.read_text(encoding="utf-8", errors="ignore"):
                        return True
                except OSError:
                    pass
    return False


# (name, status, detector, hint)
TOOLS = [
    ("Assimp importer",      REQUIRED_NOW,
     lambda: file_exists("3rdparty/assimp/CMakeLists.txt") and text_contains("CMakeLists.txt", "ENABLE_ASSIMP_IMPORTER"),
     "3rdparty/assimp/ vendored + ENABLE_ASSIMP_IMPORTER in CMakeLists.txt (default ON)"),
    ("shader_reflect",       REQUIRED_NOW,
     lambda: file_exists("tools/shader_reflect/reflect.py"),
     "tools/shader_reflect/reflect.py (active shader-reflection CI gate)"),
    ("mc2texcook",           REQUIRED_NOW,
     lambda: file_exists("tools/mc2texcook/mc2texcook.py"),
     "tools/mc2texcook/mc2texcook.py (KTX2 producer)"),
    ("KTX2 runtime loader",  REQUIRED_NOW,
     lambda: text_contains("GameOS/gameos/gos_static_prop_batcher_finalize.cpp", "KTX2"),
     "KTX2 sidecar loader in gos_static_prop_batcher_finalize.cpp (finalizeGeometry)"),
    ("validate_asset_manifest", REQUIRED_NOW,
     lambda: file_exists("tools/asset_cook/validate_asset_manifest.py"),
     "tools/asset_cook/validate_asset_manifest.py (canonical, ruling C1)"),
    ("meshoptimizer",        FUTURE,
     lambda: file_exists("3rdparty/meshoptimizer/CMakeLists.txt") and text_contains("CMakeLists.txt", "ENABLE_CDAG_COOKER"),
     "vendored 3rdparty/meshoptimizer/ + ENABLE_CDAG_COOKER (OFF; cdag_cooker not built yet)"),
    ("gltfpack",             FUTURE,
     detect_gltfpack,
     "not wired at repo level (source exists only inside vendored meshoptimizer)"),
]


def main() -> int:
    verbose = "--verbose" in sys.argv[1:]
    rows = []
    missing_required = []
    missing_future = []
    for name, status, detect, hint in TOOLS:
        present = bool(detect())
        rows.append((name, status, present, hint))
        if not present:
            (missing_required if status == REQUIRED_NOW else missing_future).append(name)

    name_w = max(len(r[0]) for r in rows)
    print(f"{'TOOL':<{name_w}}  {'STATUS':<12}  PRESENT")
    print(f"{'-'*name_w}  {'-'*12}  -------")
    for name, status, present, hint in rows:
        mark = "yes" if present else ("NO (warn)" if status == FUTURE else "NO (FAIL)")
        print(f"{name:<{name_w}}  {status:<12}  {mark}")
        if verbose:
            print(f"{'':<{name_w}}  detect: {hint}")

    print()
    if missing_future:
        print(f"[toolchain-bom] WARN: future tools not present (expected): {', '.join(missing_future)}")
    if missing_required:
        print(f"[toolchain-bom] FAIL: REQUIRED_NOW tools missing: {', '.join(missing_required)}", file=sys.stderr)
        return 1
    print("[toolchain-bom] OK: all REQUIRED_NOW tools present.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
