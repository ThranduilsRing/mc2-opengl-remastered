#!/usr/bin/env python3
"""check-spirv-artifacts.py — OFFLINE-SHADER-VARIANT-BUILD-1 verifier

CI-cheap, tool-free verification that the committed pilot SPIR-V artifacts are
present and in sync with their GLSL source + the binding manifest. Does NOT run
glslang/spirv-cross (it recomputes the source hash with shader_common, which is
pure Python) — so it can live in check-contracts.sh next to the other static
checks. Regenerate artifacts with:
  py -3 tools/shader_offline_build/build_variants.py

Asserts (FAIL):
  1. every pilot (variant, stage) has a .spv + sidecar on disk;
  2. the sidecar source_sha256 matches the CURRENT flattened GLSL source
     (a GLSL edit without a rebuild = stale artifact = drift);
  3. every reflected UBO/SSBO binding in a sidecar is present in
     binding-slot-occupancy.json (binding drift);
  4. the sidecar's named .spv exists.

Usage:
  py -3 scripts/check-spirv-artifacts.py [--root R] [--json OUT] [--quiet]
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import shader_common  # noqa: E402
sys.path.insert(0, str(ROOT / 'tools/shader_offline_build'))
import slang_compile  # noqa: E402

PILOTS = "tools/shader_offline_build/pilots.json"
BINDING_OCC = "docs/render-backend-seams/binding-slot-occupancy.json"


import re as _re

def sha256(s: str) -> str:
    return hashlib.sha256(s.encode("utf-8")).hexdigest()


def canon_source(s: str) -> str:
    """Path-independent + newline-normalized source form (matches build_variants).
    build_shader_source embeds `#line N // <abs path>` which varies per worktree;
    strip the path label so the hash is portable across checkouts."""
    s = s.replace("\r\n", "\n").replace("\r", "\n")
    return _re.sub(r"(?m)^(#line\s+\d+)\s*//.*$", r"\1", s)


def variant_id(base, defines):
    return sha256(base + "|" + ";".join(sorted(defines)))[:12]


def occupancy_slots(root):
    p = root / BINDING_OCC
    if not p.exists():
        return None
    occ = json.load(open(p, encoding="utf-8")).get("occupancy", {})
    out = set()
    for key in occ:
        ns, _, slot = key.partition(":")
        if slot.isdigit():
            out.add((ns, int(slot)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None)
    ap.add_argument("--json", default=None)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()
    root = Path(args.root) if args.root else ROOT

    cfg = json.load(open(root / PILOTS, encoding="utf-8"))
    spv_dir = root / cfg.get("spv_dir", "shaders/spv")
    occ = occupancy_slots(root)

    # The runtime consumer resolves variants via
    # this index (base|stage|sorted-defines -> artifact). Load it for coverage.
    idx_path = spv_dir / "spirv_index.json"
    index = {}
    if idx_path.exists():
        for r in json.load(open(idx_path, encoding="utf-8")).get("records", []):
            index[r["key"]] = r["artifact"]

    fails, checked = [], []
    for pilot in cfg["pilots"]:
        base = pilot["program"]
        for variant in pilot["variants"]:
            vname, defines = variant["name"], variant.get("defines", [])
            for stage, rel in pilot["stages"].items():
                sc = spv_dir / f"{base}.{stage}.{vname}.json"
                tag = f"{base}.{stage}.{vname}"
                checked.append(tag)
                if not sc.exists():
                    fails.append(f"{tag}: sidecar missing ({sc.relative_to(root)}) "
                                 f"— run build_variants.py")
                    continue
                meta = json.load(open(sc, encoding="utf-8"))
                # 1/4: .spv present
                spv = spv_dir / meta.get("artifact", "")
                if not meta.get("artifact") or not spv.exists():
                    fails.append(f"{tag}: .spv artifact missing ({meta.get('artifact')})")
                # 2: source hash in sync with current GLSL
                try:
                    if rel.endswith(".slang"):
                        cur = slang_compile.source_bundle(root / rel)
                    else:
                        cur = canon_source(shader_common.build_shader_source(root / rel, defines))
                    if meta.get("source_sha256") != sha256(cur):
                        fails.append(f"{tag}: source drift - shader changed since bake "
                                     f"(rebuild with build_variants.py)")
                except Exception as e:
                    fails.append(f"{tag}: cannot rebuild source: {e}")
                is_slang = rel.endswith(".slang")
                if is_slang:
                    bindings = meta.get('bindings', {})
                    try:
                        errors = slang_compile.validate_reflection(bindings, root / rel, root)
                        fails.extend(f'{tag}: {e}' for e in errors)
                    except (OSError, ValueError) as e:
                        fails.append(f'{tag}: {e}')
                for blk in ([] if is_slang else meta.get("bindings", {}).get("ubos", [])):
                    b = blk.get("binding")
                    if b is not None and occ is not None and ("UBO", b) not in occ:
                        fails.append(f"{tag}: UBO '{blk.get('name')}' binding={b} "
                                     f"absent from binding-slot-occupancy.json")
                for blk in ([] if is_slang else meta.get("bindings", {}).get("ssbos", [])):
                    b = blk.get("binding")
                    if b is not None and occ is not None and ("SSBO", b) not in occ:
                        fails.append(f"{tag}: SSBO '{blk.get('name')}' binding={b} "
                                     f"absent from binding-slot-occupancy.json")
                # 5: index coverage — the consumer keys on base|stage|sorted-defines.
                ikey = f"{base}|{stage}|" + ";".join(sorted(defines))
                if not idx_path.exists():
                    fails.append(f"{tag}: spirv_index.json missing — run build_variants.py")
                elif index.get(ikey) != meta.get("artifact"):
                    fails.append(f"{tag}: index key '{ikey}' -> {index.get(ikey)} "
                                 f"!= sidecar artifact {meta.get('artifact')}")

    report = {"summary": {"checked": len(checked), "fails": len(fails)},
              "artifacts": checked, "fails": fails}
    if args.json:
        json.dump(report, open(args.json, "w", encoding="utf-8"), indent=2)
    if not args.quiet:
        print("[check-spirv-artifacts] OFFLINE-SHADER-VARIANT-BUILD-1")
        print(f"  pilot artifacts checked : {len(checked)}")
        for t in checked:
            print(f"    {t}")
        for f in fails:
            print(f"  FAIL: {f}")
        print(f"  result: {'FAIL' if fails else 'PASS'} ({len(fails)} fail)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
