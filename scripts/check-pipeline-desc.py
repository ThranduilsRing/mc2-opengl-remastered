#!/usr/bin/env python3
"""check-pipeline-desc.py — PIPELINE-DESC-OCCUPANCY-CHECK-1

Check-time OCCUPANCY / COMPLETENESS checker for the RenderCore PipelineDesc
registry. This is NOT a Vulkan-ready PSO-key schema (see
docs/render-backend-seams/pipeline-state-contract-recon-1.md — that contract is
DEFERRED until shader-variant identity is enumerable). It asserts only that the
*currently registered* pipelines each declare the KNOWN FINITE fixed-function
axes that PipelineDesc already represents, and it documents the axes that are
explicitly MISSING or descriptive-only so they cannot silently grow.

What it asserts (FAIL):
  1. PipelineId enum (PipelineRegistry.h) and the s_descs table
     (PipelineRegistry.cpp) have the same row count — mirrors the C++
     static_assert at check time so a stale/unregistered extra row fails here.
  2. Every non-Invalid s_descs row declares ALL of PipelineDesc's declarative
     fields (glProgramName, blend, depthTest/Write/Func, cullMode,
     colorAttachments, objectIdWriteEnabled, frontFace, polygonOffsetEnable,
     ssboBindingsMask). A row missing a field fails — the "planted missing field"
     gate.

What it documents (never fails the build):
  - render-pass compatibility: which PipelineId maps to a RenderPassContract row
    with pipelineDescRegistered=true (sub-pass ids like AlphaTest/Depth have no
    own RenderPassId — reported as a sub-pass, not a failure).
  - axes PipelineDesc does NOT model (blendEquation, frontFace, explicit MRT
    draw-buffer mask, runtime shader define-set identity, material-variation
    define-set identity) — the DEFER frontier.

Exit code: 0 unless a FAIL is found.

Usage:
  py -3 scripts/check-pipeline-desc.py [--root <repo>] [--json <out>] [--quiet]
"""
import argparse
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import checklib  # noqa: E402

# The full declarative field set of RenderCore::PipelineDesc, in struct order.
# Each non-Invalid registry row MUST initialize exactly these (a missing field
# is the planted-failure gate). colorAttachments is one field (a {a,b,c} brace).
PIPELINE_DESC_FIELDS = [
    "glProgramName",
    "blend",
    "depthTestEnable",
    "depthWriteEnable",
    "depthFunc",
    "cullMode",
    "colorAttachments",
    "objectIdWriteEnabled",
    "frontFace",            # PIPELINEKEY-RASTERSTATE-FRONTFACE-AUTHORITY-1 (positional: before ssboBindingsMask)
    "polygonOffsetEnable",  # SHADOW-CASTER-PIPELINE-REGISTRATION-1 (positional: after frontFace)
    "drawBuffers",          # DRAWBUFFER-OWNERSHIP-1 (positional: before ssboBindingsMask)
    "ssboBindingsMask",
]
N_FIELDS = len(PIPELINE_DESC_FIELDS)

# Axes a real Vulkan PSO key needs but PipelineDesc does NOT model today.
# Documented so the DEFER frontier is explicit and checked-in.
MISSING_OR_DESCRIPTIVE_AXES = [
    {"axis": "blendEquation", "status": "MISSING",
     "note": "only blend factor (BlendMode) modeled; GL_FUNC_ADD assumed, set nowhere"},
    {"axis": "frontFace", "status": "MISSING",
     "note": "winding not in PipelineDesc; only the save/restore snapshot tracks it"},
    {"axis": "explicitMRTdrawBufferMask", "status": "DESCRIPTIVE",
     "note": "colorAttachments documents required attachments; applyPipeline does "
             "not reconfigure draw buffers (depth-prepass masks color via glColorMask)"},
    {"axis": "runtimeShaderDefineSetIdentity", "status": "MISSING (BLOCKER)",
     "note": "glProgramName is a runtime GL name, not a stable variant id; "
             "#define injection in makeShader is non-enumerable — see recon DEFER"},
    {"axis": "materialVariationDefineSetIdentity", "status": "MISSING (BLOCKER)",
     "note": "gosMaterialVariation injects an open-ended per-material define set; "
             "not representable as a finite id — prerequisite SHADER-PERMUTATION-INVENTORY-1"},
]


def read(p):
    with open(p, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def strip_comments(text):
    """Correct C/C++ comment stripping, delegated to the shared lexer.

    STRIPPER-AUDIT-1 (2026-07-31): repointed off the two stateless regexes that
    cannot notice a `/*` inside a `//` comment (the `//*****` banner idiom being
    the dominant trigger in this tree). MEASURED exposure for THIS checker:
    ZERO -- none of its three scanned files (RenderCore/PipelineRegistry.{h,cpp},
    RenderCore/RenderPassContract.h) currently carries a phantom block. Migrated
    for uniformity so the next banner comment added to the registry cannot
    silently blank the enum/row parse; not because a verdict was wrong today.
    """
    return checklib.strip_comments(text)


def split_top_level(s):
    """Split s on commas at brace-depth 0; drop empty (trailing-comma) parts."""
    out, depth, cur = [], 0, []
    for ch in s:
        if ch in "{(":
            depth += 1
        elif ch in "})":
            depth -= 1
        if ch == "," and depth == 0:
            out.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    if cur:
        out.append("".join(cur))
    return [p.strip() for p in out if p.strip()]


def extract_braced(text, start):
    """Given index `start` at an opening '{', return (inner, end_index_after)."""
    depth, i = 0, start
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start + 1:i], i + 1
        i += 1
    raise ValueError("unbalanced braces")


def parse_enum_ids(hpp_text):
    """Parse PipelineId enum names in declaration order (excluding Count_)."""
    m = re.search(r"enum\s+class\s+PipelineId\s*:[^{]*\{(.*?)\}", hpp_text, re.S)
    if not m:
        return []
    ids = []
    for line in m.group(1).splitlines():
        em = re.match(r"\s*([A-Za-z_]\w*)\s*=\s*(\d+)", line)
        if em and em.group(1) != "Count_":
            ids.append((em.group(1), int(em.group(2))))
    ids.sort(key=lambda x: x[1])
    return [name for name, _ in ids]


def parse_registry_rows(cpp_text):
    """Return list of rows; each row is a list of field-strings (the Invalid
    sentinel PipelineDesc{} -> empty list)."""
    m = re.search(r"s_descs\s*=\s*", cpp_text)
    if not m:
        raise ValueError("s_descs initializer not found")
    # first '{' after the '=' opens the outer aggregate ({{ ... }})
    brace = cpp_text.index("{", m.end())
    outer, _ = extract_braced(cpp_text, brace)
    # outer is "{ PipelineDesc{}, {..}, {..} }" — strip one more layer
    inner_start = outer.index("{")
    inner, _ = extract_braced(outer, inner_start)
    elements = split_top_level(inner)
    rows = []
    for el in elements:
        if "{" in el and not el.startswith("PipelineDesc{"):
            bs = el.index("{")
            body, _ = extract_braced(el, bs)
            rows.append(split_top_level(body))
        else:
            rows.append([])  # PipelineDesc{} sentinel
    return rows


def parse_passcontract_registered(text):
    """Map pass-name -> pipelineDescRegistered bool from RenderPassContract.h.

    Each contract row contributes exactly one `RenderPassId::Name` token and one
    `pipelineDescRegistered*/ <bool>` token, in that order, so we pair them by
    interleaved position rather than a single brittle span regex."""
    tokens = re.findall(
        r'RenderPassId::(\w+)\s*,'                       # row-opening id token
        r'|pipelineDescRegistered\s*\*/\s*(true|false)',  # the registration flag
        text,
    )
    out, pending = {}, None
    for name, reg in tokens:
        if name:
            pending = name
        elif reg and pending:
            out[pending] = (reg == "true")
            pending = None
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None)
    ap.add_argument("--json", default=None)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    root = args.root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    hpp = os.path.join(root, "RenderCore", "PipelineRegistry.h")
    cpp = os.path.join(root, "RenderCore", "PipelineRegistry.cpp")
    contract = os.path.join(root, "RenderCore", "RenderPassContract.h")

    fails, warns = [], []

    enum_ids = parse_enum_ids(strip_comments(read(hpp)))           # incl Invalid
    rows = parse_registry_rows(strip_comments(read(cpp)))
    registered = parse_passcontract_registered(read(contract))

    # --- FAIL 1: enum count vs table row count ------------------------------
    if len(enum_ids) != len(rows):
        fails.append(
            f"row-count mismatch: PipelineId enum has {len(enum_ids)} ids "
            f"({', '.join(enum_ids)}) but s_descs has {len(rows)} rows "
            f"(stale/unregistered row or missing row)"
        )

    # --- FAIL 2: per-row field completeness ---------------------------------
    pipelines = []
    for idx, fields in enumerate(rows):
        name = enum_ids[idx] if idx < len(enum_ids) else f"<row{idx}>"
        if name == "Invalid":
            # sentinel: PipelineDesc{} (zeroed) is intentional, no fields
            pipelines.append({"id": name, "index": idx, "sentinel": True})
            continue
        if len(fields) != N_FIELDS:
            fails.append(
                f"incomplete pipeline '{name}' (row {idx}): declares "
                f"{len(fields)} fields, expected {N_FIELDS} "
                f"({', '.join(PIPELINE_DESC_FIELDS)})"
            )
        declared = {
            PIPELINE_DESC_FIELDS[i]: fields[i]
            for i in range(min(len(fields), N_FIELDS))
        }
        # render-pass compatibility (informational)
        if name in registered:
            rp = ("registered (pipelineDescRegistered=true)"
                  if registered[name] else "RenderPassId exists but NOT registered")
            if not registered[name]:
                warns.append(f"'{name}' has a RenderPassId but pipelineDescRegistered=false")
        else:
            rp = "sub-pass (no own RenderPassId - descriptive)"
        pipelines.append({
            "id": name, "index": idx, "sentinel": False,
            "fields": declared, "renderPassCompat": rp,
        })

    report = {
        "summary": {
            "enum_ids": enum_ids,
            "row_count": len(rows),
            "fields_per_pipeline_required": N_FIELDS,
            "fails": len(fails),
            "warns": len(warns),
            "scope": "occupancy/completeness for current PipelineDesc — "
                     "NOT a Vulkan-ready PSO key (see pipeline-state-contract-recon-1.md)",
        },
        "pipelines": pipelines,
        "missing_or_descriptive_axes": MISSING_OR_DESCRIPTIVE_AXES,
        "fails": fails,
        "warns": warns,
    }

    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=2)

    if not args.quiet:
        print("[check-pipeline-desc] PIPELINE-DESC-OCCUPANCY-CHECK-1")
        print(f"  PipelineId enum rows : {len(enum_ids)} ({', '.join(enum_ids)})")
        print(f"  s_descs table rows   : {len(rows)}")
        print(f"  required fields/row  : {N_FIELDS}")
        for p in pipelines:
            if p.get("sentinel"):
                print(f"    [{p['index']}] {p['id']:<20} (zeroed sentinel)")
            else:
                print(f"    [{p['index']}] {p['id']:<20} {len(p['fields'])}/{N_FIELDS} fields"
                      f"  | {p['renderPassCompat']}")
        for w in warns:
            print(f"  WARN: {w}")
        for fl in fails:
            print(f"  FAIL: {fl}")
        print(f"  result: {'FAIL' if fails else 'PASS'} "
              f"({len(fails)} fail, {len(warns)} warn)")

    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
