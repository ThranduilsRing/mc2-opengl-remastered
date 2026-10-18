#!/usr/bin/env python3
"""check-pass-coverage.py - PIPELINE-PASS-COVERAGE-LEDGER-1 gate

Keeps the pass-coverage lifecycle ledger honest so future slices MOVE a pass one
step forward instead of re-deciding from scratch whether it should be modeled.

Source of truth: docs/render-backend-seams/pipeline-pass-coverage-ledger.json
Cross-checked against:
  - RenderCore/PipelineRegistry.h   (PipelineId enum)
  - RenderCore/RenderPassContract.h (RenderPassId enum)
  - GameOS/ + mclib/ sources        (applyPipeline routing evidence)
  - shaders/spv/spirv_package.json  (SPIR-V family evidence, if present)

Asserts (FAIL):
  1. every ledger entry has a status in the allowed set.
  2. a DO_NOT_MODEL entry has a non-empty reason.
  3. every entry's pipelineId (if set) exists in the PipelineId enum (no stale).
  4. every registered PipelineId appears as some entry's pipelineId (no pass
     missing from the ledger).
  5. every entry's renderPassId (if set) exists in the RenderPassId enum.
  6. every RenderPassId (non-None) is referenced by >=1 ledger entry (no
     unclassified pass family).
  7. an entry claiming ROUTED_BY_APPLYPIPELINE / VISUAL_PROVEN / SPIRV_ELIGIBLE
     has actual applyPipeline(...PipelineId::<id>) evidence in the sources.
  8. a SPIRV_ELIGIBLE entry's shaderBase is a baked SPIR-V family (if the
     package manifest exists).

Warns:
  - an UNREGISTERED entry with no `next` (forward step undocumented).

Read-only. No GL, no build.
Usage: py -3 scripts/check-pass-coverage.py [--root <repo>] [--json <out>] [--quiet]
"""
import argparse
import glob
import json
import os
import re
import sys

LEDGER = "docs/render-backend-seams/pipeline-pass-coverage-ledger.json"
PIPELINE_H = "RenderCore/PipelineRegistry.h"
PASS_H = "RenderCore/RenderPassContract.h"
SPV_PKG = "shaders/spv/spirv_package.json"

ALLOWED = {
    "UNREGISTERED", "DESCRIPTIVE_REGISTERED", "ROUTED_BY_APPLYPIPELINE",
    "VISUAL_PROVEN", "SPIRV_ELIGIBLE", "DO_NOT_MODEL",
}
ROUTED_PLUS = {"ROUTED_BY_APPLYPIPELINE", "VISUAL_PROVEN", "SPIRV_ELIGIBLE"}


def read(p):
    with open(p, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def parse_enum(text, name, drop):
    """Enum-class member names, excluding `drop` and pure-sentinel tokens."""
    m = re.search(r"enum\s+class\s+" + re.escape(name) + r"\s*:[^{]*\{(.*?)\}",
                  text, re.S)
    if not m:
        return []
    body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
    body = re.sub(r"//[^\n]*", "", body)
    out = []
    for tok in body.split(","):
        nm = tok.split("=")[0].strip()
        if nm and nm not in drop:
            out.append(nm)
    return out


def routed_pipelines(root):
    """Set of PipelineId names that appear inside an applyPipeline(...) call in
    GameOS/ or mclib/ sources (handles multi-line calls via a forward window)."""
    found = set()
    files = []
    for sub in ("GameOS", "mclib"):
        files += glob.glob(os.path.join(root, sub, "**", "*.cpp"), recursive=True)
    for f in files:
        try:
            t = read(f)
        except OSError:
            continue
        for m in re.finditer(r"applyPipeline\s*\(", t):
            window = t[m.end():m.end() + 400]
            for pm in re.finditer(r"PipelineId::(\w+)", window):
                # stop at the first statement terminator to avoid bleeding
                # into the next call
                if ";" in window[:window.index(pm.group(0))]:
                    break
                found.add(pm.group(1))
    return found


def spirv_bases(root):
    p = os.path.join(root, SPV_PKG)
    if not os.path.exists(p):
        return None
    try:
        pkg = json.load(open(p, encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return {fam.get("base") for fam in pkg.get("variant_matrix", [])}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None)
    ap.add_argument("--json", default=None)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()
    root = args.root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    ledger = json.load(open(os.path.join(root, LEDGER), encoding="utf-8"))
    passes = ledger.get("passes", {})
    pipe_ids = set(parse_enum(read(os.path.join(root, PIPELINE_H)),
                              "PipelineId", {"Invalid", "Count_"}))
    pass_ids = set(parse_enum(read(os.path.join(root, PASS_H)),
                              "RenderPassId", {"None", "_SentinelLast", "Count_"}))
    routed = routed_pipelines(root)
    spv = spirv_bases(root)

    fails, warns = [], []

    ledger_pipe_ids = set()
    ledger_pass_ids = set()

    for name, e in passes.items():
        st = e.get("status")
        if st not in ALLOWED:
            fails.append(f"'{name}' status '{st}' not in {sorted(ALLOWED)}")
            continue
        if st == "DO_NOT_MODEL" and not (e.get("reason") or "").strip():
            fails.append(f"'{name}' is DO_NOT_MODEL but has no reason")

        pid = e.get("pipelineId")
        # A family may map to MANY PipelineIds (e.g. VFX = 6 rows): support a
        # `pipelineIds` list alongside the single `pipelineId`.
        for one in ([pid] if pid else []) + list(e.get("pipelineIds", [])):
            ledger_pipe_ids.add(one)
            if one not in pipe_ids:
                fails.append(f"'{name}' pipelineId '{one}' not in PipelineId enum (stale)")
        rpid = e.get("renderPassId")
        if rpid:
            ledger_pass_ids.add(rpid)
            if rpid not in pass_ids:
                fails.append(f"'{name}' renderPassId '{rpid}' not in RenderPassId enum (stale)")

        if st in ROUTED_PLUS:
            # evidence required for the single pipelineId OR every id in pipelineIds
            ids_for_evidence = ([pid] if pid else []) + list(e.get("pipelineIds", []))
            if not ids_for_evidence:
                fails.append(f"'{name}' status {st} requires a pipelineId / pipelineIds")
            for one in ids_for_evidence:
                if one not in routed:
                    fails.append(f"'{name}' status {st} but no applyPipeline(...PipelineId::{one}) evidence in sources")
        # Nondeterministic visual passes that are
        # ROUTED but cannot get a byte-hash gate may declare a documented
        # proofStatus instead of claiming VISUAL_PROVEN. Allowed values only;
        # VISUAL_PROVEN/SPIRV_ELIGIBLE must NOT carry a 'pending' proofStatus.
        ps = e.get("proofStatus")
        if ps is not None:
            # "landed" proofs vs "pending" (gate not yet obtained). A pass may not
            # claim VISUAL_PROVEN/SPIRV_ELIGIBLE while its proofStatus is pending.
            PROOF_LANDED  = {"byte_identical", "perceptual_ab", "oracle_coverage"}
            PROOF_PENDING = {"nondeterministic_visual_gate_pending",  # output is nondeterministic (e.g. VFX spawn)
                             "pass_not_exercised_in_smoke",           # deterministic but content-dependent; not drawn in tier1
                             "byte_ab_capture_pending"}               # deterministic + DRAWS + trace-confirms correct state; byte-A/B capture not yet run
            if ps not in (PROOF_LANDED | PROOF_PENDING):
                fails.append(f"'{name}' proofStatus '{ps}' not in {sorted(PROOF_LANDED | PROOF_PENDING)}")
            if st in ("VISUAL_PROVEN", "SPIRV_ELIGIBLE") and ps in PROOF_PENDING:
                fails.append(f"'{name}' claims {st} but proofStatus '{ps}' is still pending — "
                             "do not mark proven while the visual gate is unresolved")
        if st == "SPIRV_ELIGIBLE" and spv is not None:
            base = e.get("shaderBase")
            if base and base not in spv:
                fails.append(f"'{name}' SPIRV_ELIGIBLE but shaderBase '{base}' is not a baked SPIR-V family")
        if st == "UNREGISTERED" and not (e.get("next") or "").strip():
            warns.append(f"'{name}' is UNREGISTERED with no documented 'next'")

    # 4. every registered PipelineId must be in the ledger
    for pid in sorted(pipe_ids - ledger_pipe_ids):
        fails.append(f"registered PipelineId '{pid}' missing from ledger")
    # 6. every RenderPassId must be classified
    for rpid in sorted(pass_ids - ledger_pass_ids):
        fails.append(f"RenderPassId '{rpid}' has no ledger entry (unclassified pass family)")

    report = {
        "summary": {
            "passes": len(passes),
            "pipeline_ids": sorted(pipe_ids),
            "render_pass_ids": sorted(pass_ids),
            "routed_evidence": sorted(routed),
            "fails": len(fails),
            "warns": len(warns),
        },
        "fails": fails,
        "warns": warns,
    }
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=2)

    if not args.quiet:
        print("[check-pass-coverage] PIPELINE-PASS-COVERAGE-LEDGER-1")
        print(f"  ledger passes    : {len(passes)}")
        print(f"  PipelineId enum  : {len(pipe_ids)}  RenderPassId enum: {len(pass_ids)}")
        print(f"  routed evidence  : {', '.join(sorted(routed)) or '(none)'}")
        for name, e in passes.items():
            print(f"    {name:<20} {e.get('status')}")
        for w in warns:
            print(f"  WARN: {w}")
        for fl in fails:
            print(f"  FAIL: {fl}")
        print(f"  result: {'FAIL' if fails else 'PASS'} ({len(fails)} fail, {len(warns)} warn)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
