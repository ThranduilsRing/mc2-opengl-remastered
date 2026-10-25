#!/usr/bin/env python3
"""check-spirv-reflection-contract.py — SPIRV-REFLECTION-CONTRACT-CHECK-1

CI-cheap, tool-free reflection-contract gate over the baked SPIR-V sidecars
(shaders/spv/*.json from OFFLINE-SHADER-VARIANT-BUILD-1). Catches binding /
sampler / output / vertex-input LAYOUT DRIFT across a program's variants before
it can reach runtime — strengthening every shipped pilot (postprocess, mech).

It derives the contract from the artifacts themselves (no hand-maintained
expected-interface lists), so it stays correct as variants are added:

  Interface elements fall into classes with DIFFERENT correctness rules:
    - VERTEX ATTRS (vert inputs) and FBO OUTPUTS (frag outputs): bound to a fixed
      external layout (VAO / FBO draw buffers) -> must be CROSS-VARIANT CONSISTENT
      (same location everywhere it appears).
    - UBO / SSBO bindings: bound by C++ to literal slots shared across variants ->
      CROSS-VARIANT CONSISTENT + present in binding-slot-occupancy.json.
    - INTER-STAGE VARYINGS (vert outputs <-> frag inputs): auto-mapped per linked
      program; cross-variant location drift is HARMLESS, but within EACH variant
      the vert-output location MUST equal the frag-input location (link compat).
    - SAMPLERS: resolved by NAME at runtime (glGetUniformLocation) -> location
      drift is harmless; only cross-check the name against sampler-unit-occupancy.

  Plus, for every class: MONOTONIC GATING (up-set) — if an element is present for
  define-set V it must be present in every baked variant whose defines are a
  SUPERSET of V (a #define may ADD interface, never REMOVE it; e.g. v_objectId@2
  appears IFF MC2_OBJECT_ID_BUFFER, ViewUniformsBlock@3 IFF MC2_USE_VIEW_UNIFORMS).

Exit 0 unless a FAIL is found.

Usage:
  py -3 scripts/check-spirv-reflection-contract.py [--root R] [--json OUT] [--quiet]
"""
from __future__ import annotations
import argparse
import json
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/shader_offline_build'))
from build_variants import occupancy_slots  # noqa: E402

PILOTS = "tools/shader_offline_build/pilots.json"
BINDING_OCC = "docs/render-backend-seams/binding-slot-occupancy.json"
SAMPLER_OCC = "docs/render-backend-seams/sampler-unit-occupancy.json"


def occ_slots(p):
    return occupancy_slots(p)


def sampler_names(p):
    if not p.exists():
        return None
    try:
        doc = json.load(open(p, encoding="utf-8"))
    except Exception:
        return None
    names = set()
    # tolerate a few shapes: {"occupancy": {...}} / {"samplers":[...]} / list
    def walk(o):
        if isinstance(o, dict):
            for k, v in o.items():
                if k in ("name", "uniform", "sampler") and isinstance(v, str):
                    names.add(v)
                walk(v)
        elif isinstance(o, list):
            for x in o:
                walk(x)
    walk(doc)
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None)
    ap.add_argument("--json", default=None)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()
    root = Path(args.root) if args.root else Path(__file__).resolve().parents[1]

    cfg = json.load(open(root / PILOTS, encoding="utf-8"))
    sources = {(p['program'], stage): rel for p in cfg['pilots']
               for stage, rel in p['stages'].items()}
    spv_dir = root / cfg.get("spv_dir", "shaders/spv")
    occ = occ_slots(root / BINDING_OCC)
    smp = sampler_names(root / SAMPLER_OCC)

    # Collect baked sidecars grouped by (base, stage) and by (base, variant).
    groups = {}                 # (base, stage) -> [(defs, meta)]
    by_variant = {}             # (base, vname) -> {stage: meta, "defs": defs}
    for pilot in cfg["pilots"]:
        base = pilot["program"]
        for variant in pilot["variants"]:
            vname = variant["name"]
            for stage in pilot["stages"]:
                sc = spv_dir / f"{base}.{stage}.{vname}.json"
                if not sc.exists():
                    continue
                m = json.load(open(sc, encoding="utf-8"))
                defs = frozenset(m.get("defines", []))
                groups.setdefault((base, stage), []).append((defs, m))
                by_variant.setdefault((base, vname), {"defs": defs})[stage] = m

    fails, warns, checked = [], [], []

    def elems(meta, kind):
        b = meta.get("bindings", {}); i = meta.get("interface", {})
        if kind == "outputs":  return {x["name"]: x.get("location") for x in i.get("outputs", [])}
        if kind == "inputs":   return {x["name"]: x.get("location") for x in i.get("inputs", [])}
        if kind in ("ubos", "ssbos"):
            return {x["name"]: (x.get("set", 0), x.get("binding")) for x in b.get(kind, [])}
        if kind == "samplers": return {x["name"]: x.get("location") for x in b.get("samplers", [])}
        return {}

    # Which (stage, kind) require cross-variant CONSISTENCY (fixed external layout)
    # vs are variation-tolerant (varyings, by-name samplers).
    def consistency_required(stage, kind):
        if kind in ("ubos", "ssbos"):              return True   # C++ literal slots
        if stage == "vert" and kind == "inputs":   return True   # vertex attributes (VAO)
        if stage == "frag" and kind == "outputs":  return True   # FBO draw buffers
        return False  # varyings (vert outputs / frag inputs) + samplers: tolerant

    for (base, stage), variants in sorted(groups.items()):
        checked.append(f"{base}.{stage} ({len(variants)} variant(s))")
        for kind in ("outputs", "inputs", "ubos", "ssbos", "samplers"):
            present = {}
            for defs, meta in variants:
                for name, val in elems(meta, kind).items():
                    present.setdefault(name, {})[defs] = val
            for name, by_def in present.items():
                vals = set(by_def.values())
                # CONSISTENCY (only for fixed-external-layout classes)
                if consistency_required(stage, kind) and len(vals) > 1:
                    fails.append(f"{base}.{stage} {kind} '{name}': inconsistent "
                                 f"location/binding across variants {sorted(map(str,vals))} "
                                 f"(fixed-layout element must not move between variants)")
                # MONOTONIC GATING (up-set over baked variants) — all classes
                present_defs = set(by_def.keys())
                for d_present in present_defs:
                    for d_all, _ in variants:
                        if d_all >= d_present and d_all not in present_defs:
                            fails.append(
                                f"{base}.{stage} {kind} '{name}': present for defines "
                                f"{sorted(d_present)} but MISSING in superset variant "
                                f"{sorted(d_all)} (non-monotonic gating / define removed it)")
                # MANIFEST AGREEMENT
                if kind in ("ubos", "ssbos") and occ is not None:
                    ns = "UBO" if kind == "ubos" else "SSBO"
                    b = next(iter(vals)) if len(vals) == 1 else None
                    if b is not None and (ns, *b, name, sources[base, stage]) not in occ:
                        fails.append(f"{base}.{stage} {ns} '{name}' binding={b} "
                                     f"absent from binding-slot-occupancy.json")
                if kind == "samplers" and smp is not None and name not in smp:
                    warns.append(f"{base}.{stage} sampler '{name}' not named in "
                                 f"sampler-unit-occupancy.json (C++ binder side)")

    # INTER-STAGE VARYING LINK COMPAT: within each variant, a varying read by the
    # frag (input) must sit at the same location the vert wrote it (output).
    for (base, vname), st in sorted(by_variant.items()):
        if "vert" not in st or "frag" not in st:
            continue
        vo = elems(st["vert"], "outputs")
        fi = elems(st["frag"], "inputs")
        for name in sorted(set(vo) & set(fi)):
            if vo[name] != fi[name]:
                fails.append(f"{base}.{vname} varying '{name}': vert out loc {vo[name]} "
                             f"!= frag in loc {fi[name]} (would mis-link)")

    report = {"summary": {"groups": len(groups), "fails": len(fails),
                          "warns": len(warns)},
              "checked": checked, "fails": fails, "warns": warns}
    if args.json:
        json.dump(report, open(args.json, "w", encoding="utf-8"), indent=2)
    if not args.quiet:
        print("[check-spirv-reflection-contract] SPIRV-REFLECTION-CONTRACT-CHECK-1")
        for c in checked:
            print(f"    {c}")
        for w in warns:
            print(f"  WARN: {w}")
        for f in fails:
            print(f"  FAIL: {f}")
        print(f"  result: {'FAIL' if fails else 'PASS'} "
              f"({len(fails)} fail, {len(warns)} warn)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
