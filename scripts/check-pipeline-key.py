#!/usr/bin/env python3
"""check-pipeline-key.py — PIPELINE-KEY-SCHEMA-1 consistency gate

Cross-validates the repo-owned logical PipelineKey schema against the two
contracts it is built from, so the schema cannot silently drift from reality:

  - docs/render-backend-seams/pipeline-key-schema.json     (this schema)
  - docs/render-backend-seams/shader-permutation-inventory.json  (variant macros)
  - RenderCore/PipelineRegistry.h                          (registered pipelines)

Asserts (FAIL):
  1. Every shaderVariantId macro named by any pipeline exists in the shader
     inventory (governance link — no phantom variant macros in a key).
  2. Every specializationParam (field-level + the params list) is typed.
  3. Every PipelineKey field declares a status in the allowed enum (no silent
     "is this authoritative?" — missing PSO fields must be explicit).
  4. The schema's registered_pipelines set matches the non-Invalid PipelineId
     enum exactly (no stale/missing pipeline row).

Read-only. No Vulkan, no PSO impl. Companion: pipeline-key-schema.md.

Usage:
  py -3 scripts/check-pipeline-key.py [--root <repo>] [--json <out>] [--quiet]
"""
import argparse
import json
import os
import re
import sys

SCHEMA = "docs/render-backend-seams/pipeline-key-schema.json"
INVENTORY = "docs/render-backend-seams/shader-permutation-inventory.json"
REGISTRY_H = "RenderCore/PipelineRegistry.h"
DESC_H = "RenderCore/PipelineDesc.h"

ALLOWED_STATUS = {"AUTHORITATIVE", "PARTIAL", "DESCRIPTIVE", "MISSING"}


def read(p):
    with open(p, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def parse_enum_ids(text):
    """PipelineId enum names in order, excluding Count_ and Invalid."""
    m = re.search(r"enum\s+class\s+PipelineId\s*:[^{]*\{(.*?)\}", text, re.S)
    if not m:
        return []
    ids = []
    for line in m.group(1).splitlines():
        em = re.match(r"\s*([A-Za-z_]\w*)\s*=\s*(\d+)", line)
        if em and em.group(1) not in ("Count_", "Invalid"):
            ids.append((em.group(1), int(em.group(2))))
    ids.sort(key=lambda x: x[1])
    return [n for n, _ in ids]


def parse_enum_values(text, enum_name):
    """Bare enum-class value names (no '= N' required), excluding Count_/Invalid.
    For RenderCore::FrontFace { Ccw, Cw } / CullMode { None, Back, Front }."""
    m = re.search(r"enum\s+class\s+" + re.escape(enum_name) +
                  r"\s*:[^{]*\{(.*?)\}", text, re.S)
    if not m:
        return []
    body = re.sub(r"//.*", "", m.group(1))
    out = []
    for tok in body.split(","):
        nm = tok.split("=")[0].strip()
        if nm and nm not in ("Count_", "Invalid"):
            out.append(nm)
    return out


def parse_vertex_layouts(text):
    """VERTEXLAYOUT-AUTHORITY-1: VertexLayoutId enum value -> canonical layout
    name from its '// layout: <token>' comment. Excludes Count_/Invalid. A value
    with no layout comment maps to None (the checker flags it)."""
    m = re.search(r"enum\s+class\s+VertexLayoutId\s*:[^{]*\{(.*?)\}", text, re.S)
    if not m:
        return {}
    out = {}
    for line in m.group(1).splitlines():
        em = re.match(
            r"\s*([A-Za-z_]\w*)\s*=\s*\d+\s*,?\s*(?://\s*layout:\s*(\S+))?", line)
        if em and em.group(1) not in ("Count_", "Invalid"):
            out[em.group(1)] = em.group(2)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None)
    ap.add_argument("--json", default=None)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()
    root = args.root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    schema = json.load(open(os.path.join(root, SCHEMA), encoding="utf-8"))
    inv = json.load(open(os.path.join(root, INVENTORY), encoding="utf-8"))
    inv_macros = {m["name"] for m in inv["macros"]}
    registry_raw = read(os.path.join(root, REGISTRY_H))
    enum_ids = parse_enum_ids(re.sub(r"/\*.*?\*/", "", registry_raw, flags=re.S))
    # Read raw text so `// layout:` comments are preserved.
    vlayouts = parse_vertex_layouts(registry_raw)
    # The enums live in PipelineDesc.h
    desc_raw = read(os.path.join(root, DESC_H))
    frontfaces = parse_enum_values(desc_raw, "FrontFace")
    cullmodes = parse_enum_values(desc_raw, "CullMode")

    fails, warns = [], []

    # 3. every field has an allowed status -----------------------------------
    field_status = {}
    for fld in schema.get("fields", []):
        name = fld.get("field", "<unnamed>")
        st = fld.get("status")
        field_status[name] = st
        if st not in ALLOWED_STATUS:
            fails.append(f"field '{name}' has status '{st}' not in {sorted(ALLOWED_STATUS)}")
        if st in ("MISSING", "PARTIAL") and not (fld.get("gaps") or fld.get("descriptive_subaxes")):
            warns.append(f"field '{name}' is {st} but lists no gaps — gaps should be explicit")

    # 2. specialization params typed -----------------------------------------
    spec_field = next((f for f in schema["fields"] if f["field"] == "specializationParams"), None)
    if spec_field:
        for p in spec_field.get("params", []):
            if not p.get("type"):
                fails.append(f"specializationParam '{p.get('name')}' has no type")

    # 1 + 4. per-pipeline variant macros in inventory; pipeline set matches ---
    schema_ids = [p["id"] for p in schema.get("registered_pipelines", [])]
    missing_from_schema = [i for i in enum_ids if i not in schema_ids]
    stale_in_schema = [i for i in schema_ids if i not in enum_ids]
    if missing_from_schema:
        fails.append(f"registered PipelineId(s) missing from schema: {missing_from_schema}")
    if stale_in_schema:
        fails.append(f"schema lists pipeline(s) not in PipelineId enum: {stale_in_schema}")

    for p in schema.get("registered_pipelines", []):
        for mac in p.get("shaderVariantId", {}).get("macros", []):
            if mac not in inv_macros:
                fails.append(
                    f"pipeline '{p['id']}' names variant macro '{mac}' absent "
                    f"from {INVENTORY} (phantom / ungoverned variant)")
        # per-pipeline specializationParams must also be typed if present
        for sp in p.get("specializationParams", []):
            if isinstance(sp, dict) and not sp.get("type"):
                fails.append(f"pipeline '{p['id']}' spec param '{sp.get('name')}' untyped")

        # vertexLayout is AUTHORITATIVE, recorded in
    # RenderCore::VertexLayoutId and checked here. (1) The field must declare
    # AUTHORITATIVE so the promotion cannot silently regress. (2) Every enum
    # value must carry a canonical "// layout: <name>" comment. (3) Every
    # registered pipeline must declare a vertexLayoutId that EXISTS in the enum
    # (missing -> FAIL; stale/renamed enum value -> FAIL) whose canonical name
    # EQUALS the pipeline's vertexLayout string (rename drift -> FAIL).
    if field_status.get("vertexLayout") != "AUTHORITATIVE":
        fails.append("field 'vertexLayout' must be AUTHORITATIVE "
                     "(VERTEXLAYOUT-AUTHORITY-1 promotion); "
                     f"found '{field_status.get('vertexLayout')}'")
    if not vlayouts:
        fails.append("VertexLayoutId enum not found / empty in "
                     f"{REGISTRY_H} (vertex layout authority missing)")
    for name, lname in vlayouts.items():
        if not lname:
            fails.append(f"VertexLayoutId '{name}' has no '// layout: <name>' "
                         "canonical-name comment")
    for p in schema.get("registered_pipelines", []):
        vid = p.get("vertexLayoutId")
        # Descriptive shadow rows defer
        # vertex-layout modeling — a pipeline that declares NEITHER vertexLayout
        # NOR vertexLayoutId is exempt. A pipeline that declares one but not the
        # other still FAILs (half-declared = drift).
        if not vid and "vertexLayout" not in p:
            continue
        if not vid:
            fails.append(f"pipeline '{p['id']}' has no vertexLayoutId "
                         "(vertexLayout is authoritative — id is required)")
            continue
        if vid not in vlayouts:
            fails.append(f"pipeline '{p['id']}' vertexLayoutId '{vid}' absent "
                         f"from VertexLayoutId enum (stale/renamed)")
            continue
        canon = vlayouts[vid]
        if canon and p.get("vertexLayout") != canon:
            fails.append(
                f"pipeline '{p['id']}' vertexLayout '{p.get('vertexLayout')}' "
                f"!= enum canonical '{canon}' for {vid} (rename drift)")

    # rasterState frontFace (and
    # the existing cullMode) are now repo-owned per-pipeline rows. (1) the field
    # must be PARTIAL or AUTHORITATIVE (not MISSING/DESCRIPTIVE) so the promotion
    # cannot silently regress; (2) the FrontFace enum must be present in
    # PipelineDesc.h; (3) every registered pipeline must declare a frontFace
    # whose value EXISTS in the enum (missing -> FAIL; stale/renamed -> FAIL);
    # (4) its cullState.mode must be a valid CullMode value (zero-risk sibling
    # check, mirrors the C++ row).
    if field_status.get("rasterState") not in ("PARTIAL", "AUTHORITATIVE"):
        fails.append("field 'rasterState' must be PARTIAL or AUTHORITATIVE "
                     "(PIPELINEKEY-RASTERSTATE-FRONTFACE-AUTHORITY-1); "
                     f"found '{field_status.get('rasterState')}'")
    if not frontfaces:
        fails.append("FrontFace enum not found / empty in "
                     f"{DESC_H} (frontFace authority missing)")
    for p in schema.get("registered_pipelines", []):
        ff = p.get("frontFace")
        if not ff:
            fails.append(f"pipeline '{p['id']}' has no frontFace "
                         "(rasterState frontFace is authoritative — required)")
        elif ff not in frontfaces:
            fails.append(f"pipeline '{p['id']}' frontFace '{ff}' absent from "
                         f"FrontFace enum {frontfaces} (stale/renamed)")
        mode = p.get("cullState", {}).get("mode")
        if cullmodes and mode and mode not in cullmodes:
            fails.append(f"pipeline '{p['id']}' cullState.mode '{mode}' absent "
                         f"from CullMode enum {cullmodes} (stale/renamed)")

    # polygonOffsetEnable is an
    # authoritative ENABLE/DISABLE-only subaxis. (1) every registered pipeline
    # must declare a boolean polygonOffsetEnable (missing -> FAIL); (2) the
    # PipelineDesc struct must carry the bool field (parsed from PipelineDesc.h);
    # (3) the factor/units MAGNITUDES must NOT be modeled as static fields —
    # neither a schema pipeline key nor a PipelineDesc field — because they are
    # dynamic/ImGui state (FAIL if present).
    if "polygonOffsetEnable" not in desc_raw:
        fails.append(f"PipelineDesc in {DESC_H} has no polygonOffsetEnable field "
                     "(SHADOW-CASTER-PIPELINE-REGISTRATION-1)")
    for p in schema.get("registered_pipelines", []):
        if not isinstance(p.get("polygonOffsetEnable"), bool):
            fails.append(f"pipeline '{p['id']}' missing boolean polygonOffsetEnable "
                         "(rasterState polygonOffset enable is authoritative)")
        for forbidden in ("polygonOffsetFactor", "polygonOffsetUnits",
                          "polygonOffsetBias", "depthBiasConstant", "depthBiasSlope"):
            if forbidden in p:
                fails.append(f"pipeline '{p['id']}' models polygon-offset magnitude "
                             f"'{forbidden}' as a static field — factor/units are "
                             "dynamic state, must NOT be keyed")
    # factor/units must not be a static PipelineDesc field either
    for forbidden in ("polygonOffsetFactor", "polygonOffsetUnits"):
        if forbidden in desc_raw:
            fails.append(f"PipelineDesc in {DESC_H} declares '{forbidden}' — "
                         "polygon-offset magnitude is dynamic state, must not be a field")

    # Blend FACTORS are authoritative and the
    # factor NAME now encodes the exact GL func (no per-id truth table needed).
    # CANONICAL_BLEND is the single source of factor->(src,dst,equation) truth.
    # Rules for every registered pipeline row:
    #   (1) its blendState.factor must be a real RenderCore::BlendMode value;
    #   (2) the COARSE legacy "Additive" is FORBIDDEN in a registered row (it
    #       cannot distinguish SRC_ALPHA/ONE from ONE/ONE — use the precise
    #       AdditiveOneOne / AdditiveSrcAlphaOne; coarse Additive is reserved for
    #       the render-contract bridge);
    #   (3) a blend-ENABLED row must declare explicit srcFactor/dstFactor/equation;
    #   (4) those MUST equal CANONICAL_BLEND[factor] — this FAILs if SRC_ALPHA/ONE
    #       is collapsed into ONE/ONE (declared factors no longer match the name).
    CANONICAL_BLEND = {
        "AlphaBlend":          ("SRC_ALPHA", "ONE_MINUS_SRC_ALPHA", "FUNC_ADD"),
        "AdditiveOneOne":      ("ONE",       "ONE",                 "FUNC_ADD"),
        "AdditiveSrcAlphaOne": ("SRC_ALPHA", "ONE",                 "FUNC_ADD"),
    }
    blendmodes = parse_enum_values(desc_raw, "BlendMode")  # from PipelineDesc.h
    NO_BLEND = {"Opaque", "AlphaTest"}
    for p in schema.get("registered_pipelines", []):
        bs = p.get("blendState", {})
        factor = bs.get("factor")
        if not factor:
            continue
        if blendmodes and factor not in blendmodes:
            fails.append(f"pipeline '{p['id']}' blendState.factor '{factor}' is not "
                         f"a RenderCore::BlendMode value {blendmodes} (stale/typo)")
            continue
        if factor == "Additive":
            fails.append(f"pipeline '{p['id']}' uses the COARSE legacy blend factor "
                         "'Additive' — registered rows must use the precise "
                         "AdditiveOneOne / AdditiveSrcAlphaOne (BLENDMODE-ADDITIVE-VOCABULARY-1)")
            continue
        if factor in NO_BLEND:
            continue
        triple = (bs.get("srcFactor"), bs.get("dstFactor"), bs.get("equation"))
        if not all(triple):
            fails.append(f"pipeline '{p['id']}' blend factor '{factor}' but missing "
                         "explicit srcFactor/dstFactor/equation")
        elif factor in CANONICAL_BLEND and triple != CANONICAL_BLEND[factor]:
            fails.append(f"pipeline '{p['id']}' blend factors {triple} != canonical "
                         f"{CANONICAL_BLEND[factor]} for '{factor}' "
                         "(blend collapse / drift — e.g. SRC_ALPHA/ONE flattened to ONE/ONE)")

    # Cross-link the pipeline-key
    # contract to the baked SPIR-V artifact contract. For any registered pipeline
    # whose shaderVariantId.base is also a baked SPIR-V family, the schema's
    # variant macros MUST equal the macro-name set the package actually bakes
    # (define names, stripped of "=VALUE"). This proves the runtime
    # recordPipelineVariantKey identity, the pipeline-key schema, and the baked
    # artifacts all describe the SAME MechOpaque variant space — SPIR-V selection
    # cannot fork pipeline-key accounting.
    pkg_path = os.path.join(root, "shaders/spv/spirv_package.json")
    if os.path.exists(pkg_path):
        pkg = json.load(open(pkg_path, encoding="utf-8"))
        pkg_macros = {}
        for fam in pkg.get("variant_matrix", []):
            macs = set()
            for v in fam.get("variants", []):
                for d in v.get("defines", []):
                    macs.add(d.split("=", 1)[0])
            pkg_macros[fam["base"]] = macs
        for p in schema.get("registered_pipelines", []):
            base = p.get("shaderVariantId", {}).get("base")
            if base in pkg_macros:
                schema_macs = set(p["shaderVariantId"].get("macros", []))
                if schema_macs != pkg_macros[base]:
                    fails.append(
                        f"pipeline '{p['id']}' shaderVariantId macros {sorted(schema_macs)} "
                        f"!= baked '{base}' package macros {sorted(pkg_macros[base])} "
                        f"(pipeline-key contract drifted from SPIR-V artifacts)")

    report = {
        "summary": {
            "schema_verdict": schema["summary"]["verdict_schema"],
            "impl_verdict": schema["summary"]["verdict_implementation"],
            "fields": len(schema.get("fields", [])),
            "registered_pipelines": len(schema_ids),
            "enum_ids": enum_ids,
            "fails": len(fails),
            "warns": len(warns),
        },
        "field_status": field_status,
        "fails": fails,
        "warns": warns,
    }
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=2)

    if not args.quiet:
        print("[check-pipeline-key] PIPELINE-KEY-SCHEMA-1")
        print(f"  schema verdict   : {schema['summary']['verdict_schema']} "
              f"(impl {schema['summary']['verdict_implementation']})")
        print(f"  key fields       : {len(schema.get('fields', []))}")
        print(f"  registered pipes : {len(schema_ids)} ({', '.join(schema_ids)})")
        for n, st in field_status.items():
            print(f"    {n:<26} {st}")
        for w in warns:
            print(f"  WARN: {w}")
        for fl in fails:
            print(f"  FAIL: {fl}")
        print(f"  result: {'FAIL' if fails else 'PASS'} "
              f"({len(fails)} fail, {len(warns)} warn)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
