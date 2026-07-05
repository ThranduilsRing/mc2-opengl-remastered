#!/usr/bin/env python3
"""check-vulkan-bindings.py — VULKAN-BINDINGS-CHECK-1 / PER-PIPELINE-CLASSIFIER-1

Offline Vulkan-prep report over the GPU binding-slot census
(`docs/render-backend-seams/binding-slot-occupancy.json`), classified BY PIPELINE.

WHY THIS EXISTS
---------------
Under GL, a buffer binding-base slot number is only semantic *inside* a pass:
the same slot is legally rebound to different buffers in unrelated passes
(intentional multiplexing — see `binding-slot-occupancy.md`). Under Vulkan a
descriptor-set layout pins a fixed `(set, binding)` per pipeline. The ONLY real
Vulkan blocker is two DISTINCT resources sharing one `(set, binding)` *inside the
SAME pipeline's* descriptor-set layout. A slot shared by distinct resources that
live in DIFFERENT (disjoint) pipelines is per-pipeline-isolatable — benign.

ROOT-CAUSE FIX (slice BINDING-CHECKER-PER-PIPELINE-CLASSIFIER-1)
---------------------------------------------------------------
The previous version aggregated physical-slot reuse across the WHOLE shader
corpus, so it flagged benign disjoint-pipeline shares (SurfaceVertexBuf@20 in
terrain-surface vs LightsData@20 in the lit pipelines) as collisions — a false
alarm. The fix is better CLASSIFICATION, not renumbering: group each slot's GLSL
role-declarations by owning pipeline (declared manifest
`docs/render-backend-seams/pipeline-shader-map.json`) and only ERROR when ONE
pipeline owns >1 distinct role on a slot.

THREE VERDICTS
--------------
  ERROR : same pipeline, same (set,)binding, >1 distinct resource
          -> real Vulkan descriptor collision. FAILS (exit nonzero).
  WARN  : one binding reused across DISJOINT pipelines, NOT yet classified benign
          -> visible, non-failing; needs a human to classify (add to allowlist
          or split). Exit 0.
  OK    : disjoint-pipeline share WITH an explicit benign classification in the
          allowlist (`scripts/check-vulkan-bindings.allowlist.json`) -> listed,
          non-failing.

A role declared in an `.hglsl` include belongs to every pipeline that includes
that file (an include is NOT itself a pipeline). C++ `glBindBufferBase(literal)`
sites carry no GLSL logical role and are ignored for resource-distinctness.

Exit code: ERROR present -> nonzero. OK/benign + WARN only -> 0. `--strict`
additionally fails on any un-allowlisted WARN (future tightening).

Tooling only. No runtime, no engine change, no shader edit.

Usage:
  py -3 scripts/check-vulkan-bindings.py [--json <occupancy.json>]
        [--pipeline-map <map.json>] [--allowlist <allowlist.json>]
        [--strict] [--quiet] [--self-test]
"""
import argparse
import json
import os
import sys


# ---------------------------------------------------------------------------
# Loading
# ---------------------------------------------------------------------------
def load_json(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def build_file_to_pipelines(pipeline_map):
    """Map each shader FILE -> set of owning pipeline names.

    Stage files map directly to their pipeline. Binding-bearing include files
    map to every pipeline that lists them under 'includes'."""
    f2p = {}
    for name, spec in pipeline_map.get("pipelines", {}).items():
        for stage in spec.get("stages", []):
            f2p.setdefault(stage, set()).add(name)
        for inc in spec.get("includes", []):
            f2p.setdefault(inc, set()).add(name)
    return f2p


# ---------------------------------------------------------------------------
# Per-pipeline classification
# ---------------------------------------------------------------------------
def classify_slot(slot, entries, file_to_pipelines):
    """Classify one physical slot's GLSL occupancy by pipeline.

    Returns a dict:
      verdict          : 'single' | 'error' | 'disjoint' | 'unmodeled'
      pipeline_roles   : {pipeline_name -> [distinct roles]}
      colliding        : [(pipeline, [roles])]  (pipelines owning >1 distinct role)
      distinct_roles   : [all distinct GLSL roles on the slot]
      unmodeled_files  : [glsl files with no pipeline mapping]
      cpp_sites        : count of C++ glBindBufferBase literal sites
    """
    pipeline_roles = {}   # pipeline -> set(role)
    distinct_roles = []
    unmodeled_files = []
    cpp_sites = 0

    for e in entries:
        if e.get("side") == "cpp":
            cpp_sites += 1
            continue
        if e.get("side") != "glsl":
            continue
        role = e.get("role")
        f = e.get("file")
        if role and role not in distinct_roles:
            distinct_roles.append(role)
        pipes = file_to_pipelines.get(f)
        if not pipes:
            if f not in unmodeled_files:
                unmodeled_files.append(f)
            continue
        for p in pipes:
            pipeline_roles.setdefault(p, set()).add(role)

    # A pipeline owning >1 DISTINCT role on this slot is a real collision.
    colliding = [(p, sorted(rs)) for p, rs in pipeline_roles.items() if len(rs) > 1]

    if colliding:
        verdict = "error"
    elif len(distinct_roles) <= 1:
        verdict = "single"
    elif unmodeled_files:
        # >1 distinct role but at least one declarer has no known pipeline:
        # cannot prove disjoint -> surface as unmodeled (WARN-worthy).
        verdict = "unmodeled"
    else:
        verdict = "disjoint"

    return {
        "slot": slot,
        "verdict": verdict,
        "pipeline_roles": {p: sorted(rs) for p, rs in pipeline_roles.items()},
        "colliding": colliding,
        "distinct_roles": distinct_roles,
        "unmodeled_files": unmodeled_files,
        "cpp_sites": cpp_sites,
    }


def _slot_sort_key(slot):
    ns, _, num = slot.partition(":")
    try:
        return (ns, int(num))
    except ValueError:
        return (ns, 1 << 30)


def classify(occupancy, file_to_pipelines, allowlist):
    """Return dict slot -> classification, with a resolved 'report_verdict'
    in {ERROR, WARN, OK, single} applying the benign allowlist."""
    benign = allowlist.get("benign_disjoint_shares", {})
    benign_same = allowlist.get("benign_same_pipeline", {})
    out = {}
    for slot in sorted(occupancy.keys(), key=_slot_sort_key):
        rec = classify_slot(slot, occupancy[slot], file_to_pipelines)
        v = rec["verdict"]

        if v == "error":
            # A same-pipeline share is a real ERROR only if the colliding
            # (slot|pipeline) pair is NOT source-verified-benign (VS/FS mirror
            # of one bound buffer, or mutually-exclusive #if/#else branch).
            real = [(p, roles) for (p, roles) in rec["colliding"]
                    if ("%s|%s" % (slot, p)) not in benign_same]
            waived = [(p, roles) for (p, roles) in rec["colliding"]
                      if ("%s|%s" % (slot, p)) in benign_same]
            rec["waived_same_pipeline"] = waived
            if real:
                rec["colliding"] = real
                rec["report_verdict"] = "ERROR"
                out[slot] = rec
                continue
            # All same-pipeline shares waived. Re-derive the leftover verdict:
            # if distinct roles still span >1 pipeline it is a disjoint share,
            # else it collapses to a benign single-resource slot.
            rec["colliding"] = []
            if len([1 for p, rs in rec["pipeline_roles"].items() if rs]) > 1 \
                    and len(rec["distinct_roles"]) > 1:
                v = "disjoint"
            else:
                rec["report_verdict"] = "OK"
                rec["benign_rationale"] = "same-pipeline VS/FS mirror or #if branch (allowlisted)"
                rec["benign_evidence"] = "; ".join(
                    "%s|%s" % (slot, p) for p, _ in waived)
                out[slot] = rec
                continue

        if v == "single":
            rec["report_verdict"] = "single"
        elif v == "disjoint" and slot in benign:
            rec["report_verdict"] = "OK"
            rec["benign_rationale"] = benign[slot].get("rationale", "")
            rec["benign_evidence"] = benign[slot].get("evidence", "")
        else:  # disjoint (not allowlisted) or unmodeled
            rec["report_verdict"] = "WARN"
        out[slot] = rec
    return out


# ---------------------------------------------------------------------------
# Negative self-test: a synthetic SAME-PIPELINE dup must classify ERROR.
# ---------------------------------------------------------------------------
def run_self_test():
    fake_map = {
        "pipelines": {
            "synthetic_pipe": {
                "stages": ["shaders/_synthetic.vert", "shaders/_synthetic.frag"],
                "includes": [],
            },
            "other_pipe": {"stages": ["shaders/_other.comp"], "includes": []},
        }
    }
    f2p = build_file_to_pipelines(fake_map)

    # Case 1: two DISTINCT roles in the SAME pipeline -> must be ERROR.
    same_pipe = [
        {"side": "glsl", "file": "shaders/_synthetic.vert", "role": "RoleA"},
        {"side": "glsl", "file": "shaders/_synthetic.frag", "role": "RoleB"},
    ]
    r1 = classify_slot("SSBO:99", same_pipe, f2p)
    ok1 = r1["verdict"] == "error" and ("synthetic_pipe", ["RoleA", "RoleB"]) in \
        [(p, roles) for p, roles in r1["colliding"]]

    # Case 2: same two distinct roles but in DISJOINT pipelines -> must NOT error.
    disjoint = [
        {"side": "glsl", "file": "shaders/_synthetic.vert", "role": "RoleA"},
        {"side": "glsl", "file": "shaders/_other.comp", "role": "RoleB"},
    ]
    r2 = classify_slot("SSBO:98", disjoint, f2p)
    ok2 = r2["verdict"] == "disjoint"

    print("SELF-TEST: same-pipeline distinct dup   -> %s (expect ERROR)"
          % ("ERROR" if r1["verdict"] == "error" else r1["verdict"].upper()))
    print("SELF-TEST: disjoint-pipeline distinct   -> %s (expect DISJOINT)"
          % r2["verdict"].upper())
    if ok1 and ok2:
        print("SELF-TEST: PASS")
        return 0
    print("SELF-TEST: FAIL", file=sys.stderr)
    return 1


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    here = os.path.dirname(os.path.abspath(__file__))
    seams = os.path.join(here, "..", "docs", "render-backend-seams")
    default_json = os.path.join(seams, "binding-slot-occupancy.json")
    default_map = os.path.join(seams, "pipeline-shader-map.json")
    default_allow = os.path.join(here, "check-vulkan-bindings.allowlist.json")

    ap = argparse.ArgumentParser(
        description="Per-pipeline Vulkan shared-binding collision report.")
    ap.add_argument("--json", default=default_json,
                    help="path to binding-slot-occupancy.json")
    ap.add_argument("--pipeline-map", default=default_map,
                    help="path to pipeline-shader-map.json")
    ap.add_argument("--allowlist", default=default_allow,
                    help="path to the benign-classification allowlist JSON")
    ap.add_argument("--strict", action="store_true",
                    help="also exit nonzero if any un-allowlisted WARN remains")
    ap.add_argument("--quiet", action="store_true",
                    help="suppress the report body; still honors exit code")
    ap.add_argument("--self-test", action="store_true",
                    help="run the built-in negative self-test and exit")
    args = ap.parse_args()

    if args.self_test:
        return run_self_test()

    for label, path in (("occupancy JSON", args.json),
                        ("pipeline map", args.pipeline_map),
                        ("allowlist", args.allowlist)):
        if not os.path.isfile(os.path.normpath(path)):
            print("check-vulkan-bindings: %s not found: %s"
                  % (label, os.path.normpath(path)), file=sys.stderr)
            return 2

    occupancy = load_json(os.path.normpath(args.json)).get("occupancy", {})
    pipeline_map = load_json(os.path.normpath(args.pipeline_map))
    allowlist = load_json(os.path.normpath(args.allowlist))

    file_to_pipelines = build_file_to_pipelines(pipeline_map)
    results = classify(occupancy, file_to_pipelines, allowlist)

    errors = [r for r in results.values() if r["report_verdict"] == "ERROR"]
    warns = [r for r in results.values() if r["report_verdict"] == "WARN"]
    oks = [r for r in results.values() if r["report_verdict"] == "OK"]
    singles = [r for r in results.values() if r["report_verdict"] == "single"]

    if not args.quiet:
        print("=" * 72)
        print("  VULKAN PER-PIPELINE BINDING REPORT  (PER-PIPELINE-CLASSIFIER-1)")
        print("=" * 72)
        print("  occupancy : %s" % os.path.normpath(args.json))
        print("  pipelines : %s" % os.path.normpath(args.pipeline_map))
        print("  allowlist : %s" % os.path.normpath(args.allowlist))
        print("  total slots : %d   ERROR %d / WARN %d / OK-benign %d / single %d"
              % (len(results), len(errors), len(warns), len(oks), len(singles)))
        print("-" * 72)

        if errors:
            print("  ERROR — same pipeline names >1 distinct resource on one binding")
            print("  (real Vulkan (set,binding) collision — SPLIT before descriptor assign):")
            for r in errors:
                for pipe, roles in r["colliding"]:
                    print("    [%s]  pipeline '%s'  roles=%s"
                          % (r["slot"], pipe, ", ".join(roles)))
            print("-" * 72)

        if warns:
            print("  WARN — binding shared across DISJOINT pipelines, not yet classified")
            print("  (benign under Vulkan IF pipelines truly disjoint — classify or split):")
            for r in warns:
                pr = "; ".join("%s:{%s}" % (p, ",".join(roles))
                               for p, roles in sorted(r["pipeline_roles"].items()))
                print("    %-9s %s" % (r["slot"], pr or "(no modeled pipeline)"))
                if r["unmodeled_files"]:
                    print("        unmodeled declarer(s): %s"
                          % ", ".join(r["unmodeled_files"]))
            print("-" * 72)

        if oks:
            print("  OK — allowlisted benign disjoint-pipeline shares:")
            for r in oks:
                print("    %-9s roles=%s" % (r["slot"], ", ".join(r["distinct_roles"])))
                print("        %s" % r.get("benign_rationale", ""))
            print("-" * 72)

        if errors:
            print("  %d ERROR(s) — real descriptor collision(s). FAIL." % len(errors))
        elif warns:
            print("  No same-pipeline collisions. %d WARN(s) need human classification."
                  % len(warns))
        else:
            print("  No collisions, no unclassified shares. Clear for descriptor assignment.")
        print("=" * 72)

    if warns:
        slots = ", ".join(r["slot"] for r in warns)
        print("check-vulkan-bindings: NOTE — unclassified disjoint share(s): %s"
              % slots, file=sys.stderr)

    if errors:
        if args.quiet:
            slots = ", ".join(r["slot"] for r in errors)
            print("check-vulkan-bindings: FAIL — same-pipeline collision on %s" % slots,
                  file=sys.stderr)
        return 1
    if args.strict and warns:
        print("check-vulkan-bindings: FAIL (--strict): unclassified WARN present",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
