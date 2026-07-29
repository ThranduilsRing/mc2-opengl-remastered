#!/usr/bin/env python3
"""GOLDEN-COVERAGE-LEDGER-1 (VERIFY-DEBT-1) -- committed goldens must account for
every region they could have covered, and must not contain un-failable crops.

Two distinct rots this closes:

  (A) SILENT COVERAGE LOSS. Eleven regions of baseline-5ef261f4 were correctly
      refused for run-to-run instability and their crops deleted -- but nothing
      recorded that they had ever existed. Ten of the eleven are on mc2_10 /
      mc2_05. A future reader counting "28 blessed regions" would have no way to
      learn that 12 more were dropped, nor what subject each one protected. A
      hole that is merely absent is indistinguishable from a hole nobody ever
      dug. So: every region in a blessed bookmark's manifest must be EITHER
      blessed OR listed in golden.json's `refused` ledger with a rule, a reason,
      and the subject it was protecting.

  (B) UN-FAILABLE CROPS SURVIVING IN A COMMITTED SET. bless.py now refuses to
      MINT a degenerate crop, but that only protects goldens blessed from here
      on. This re-applies the same floors to what is already committed, so a
      flat crop cannot sit in a golden set counting toward its region total.
      (This is what caught mc2_10_saved_204907/distant_ridge_base: 167 bytes,
      one unique colour, severity `critical`.)

It also verifies each member's crop file exists and still hashes to the sha256
the golden recorded -- a golden whose crops have drifted from their manifest is
not a reference.

Run:  py -3 scripts/check-golden-coverage.py [--quiet]
      py -3 scripts/check-golden-coverage.py --selftest
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "visual"))
sys.path.insert(0, str(ROOT / "tools" / "visual_verify"))

import regions as regionlib  # noqa: E402
from bless import crop_stats, degeneracy_reason  # noqa: E402

GOLDENS = ROOT / "tests" / "visual" / "goldens"
REQUIRED_HOLE_KEYS = ("bookmark", "region", "rule", "reason", "subject_protected")

# ---------------------------------------------------------------------------
# Legacy waivers (VERIFY-DEBT-1)
# ---------------------------------------------------------------------------
# Golden sets minted BEFORE this ledger existed. Their findings are reported as
# WARN, not FAIL, so the contract can gate new work without this lane silently
# rewriting artifacts that are somebody else's decision. Each waiver must carry a
# reason; the waiver list is itself checked (an unknown or stale id FAILs), so a
# waiver cannot outlive the set it excuses.
#
# NOTE THE THIRD ONE. The degenerate mc2_10_saved_204907/distant_ridge_base crop
# that motivated DEGENERATE-CROP-REFUSAL-1 is present in FOUR sets, including
# v0.5.2-baseline-PROPOSED -- the set queued for USER promotion as the release
# baseline. Removing a region from a set a human is about to promote is a user
# call, not an agent's, so it is waived-and-surfaced rather than edited here.
LEGACY_WAIVERS = {
    "v0.5.1-baseline": (
        "shipped release baseline (39 regions, blessed before the three-run "
        "all-pairs and degeneracy rules existed). Carries the degenerate "
        "distant_ridge_base crop. Historical artifact -- do not rewrite."),
    "v0.5.2-baseline-PROPOSED": (
        "USER-GATED: this is the set awaiting promotion to release golden. It "
        "carries the degenerate distant_ridge_base crop (severity `critical`, "
        "un-failable) and blesses all 11 regions later refused for instability. "
        "Editing it would change what the user is about to promote. RAISE THIS "
        "AT THE PROMOTION GATE."),
}

# ---------------------------------------------------------------------------
# Retired sets (GOLDEN-6C47-AUDIT-1)
# ---------------------------------------------------------------------------
# A golden set that was deleted from the tree ON PURPOSE, with the reasoning
# kept here so the deletion is a decision on the record and not an unexplained
# absence -- the same failure mode this checker exists to prevent, one level up.
#
# The guard below FAILs if a retired id reappears on disk. Restoring a retired
# set must therefore be a deliberate edit to this table, never a silent
# `git checkout` of the directory.
#
# Full audit + restore instructions: tests/visual/goldens/RETIRED.md
RETIRED_SETS = {
    "baseline-6c47e149": (
        "Retired by GOLDEN-6C47-AUDIT-1: strictly dominated by baseline-5ef261f4 "
        "and carried un-failable coverage. (1) ZERO unique coverage -- all 27 of "
        "its blessed regions are either blessed in 5ef261f4 (24) or recorded "
        "there as explicit rule/reason/subject coverage holes (3). (2) 5ef261f4 "
        "additionally BLESSES 3 regions 6c47e149 had refused. (3) Two of its 3 "
        "non-overlapping blessings are provably bad references: "
        "distant_ridge_base is the 167-byte all-black un-failable crop, and "
        "runway_grid_complete was blessed only because 6c47e149's rule compared "
        "r1-v-r2 and r1-v-r3 but never r2-v-r3 -- the pair on which it later "
        "measured 5.16% movement. (4) Its 10 vendored poses survive: 5ef261f4 "
        "carries all 10, verified byte-identical."),
}


def check_golden(gdir: Path, manifests: dict) -> "tuple[list, list]":
    fails, notes = [], []
    gj = gdir / "golden.json"
    if not gj.exists():
        return fails, notes
    g = json.loads(gj.read_text(encoding="utf-8"))
    members = g.get("members", [])
    refused = g.get("refused", [])
    gid = gdir.name

    blessed = {(m["bookmark"], m["region"]) for m in members}
    holes = {(r.get("bookmark"), r.get("region")) for r in refused}

    # (0) a region cannot be both blessed and recorded as a hole
    for key in sorted(blessed & holes):
        fails.append(f"{gid}: {key[0]}/{key[1]} is BOTH blessed and listed as a "
                     f"coverage hole -- the ledger contradicts the member list")

    # (1) every hole entry must actually say something useful
    for r in refused:
        miss = [k for k in REQUIRED_HOLE_KEYS if not r.get(k)]
        if miss:
            fails.append(f"{gid}: coverage-hole entry "
                         f"{r.get('bookmark')}/{r.get('region')} missing {miss} -- "
                         f"an unexplained hole is not a record")

    # (2) full accounting: every region of every bookmark this set touches
    bookmarks = {m["bookmark"] for m in members} | {r.get("bookmark") for r in refused}
    for bm in sorted(b for b in bookmarks if b):
        doc = manifests.get(bm)
        if doc is None:
            fails.append(f"{gid}: references bookmark {bm!r} with no region manifest")
            continue
        for reg in doc["regions"]:
            key = (bm, reg["id"])
            if key not in blessed and key not in holes:
                fails.append(
                    f"{gid}: {bm}/{reg['id']} (severity {reg.get('severity')}) is "
                    f"neither blessed nor recorded as a coverage hole -- coverage "
                    f"was lost silently. Bless it, or add it to golden.json's "
                    f"`refused` ledger with rule/reason/subject_protected.")

    # (3) crops present, hashes intact, and NOT degenerate
    for m in members:
        p = gdir / m["path"]
        if not p.exists():
            fails.append(f"{gid}: member {m['bookmark']}/{m['region']} crop missing "
                         f"({m['path']})")
            continue
        data = p.read_bytes()
        want = m.get("sha256")
        if want and hashlib.sha256(data).hexdigest() != want:
            fails.append(f"{gid}: member {m['bookmark']}/{m['region']} crop no longer "
                         f"matches its recorded sha256 -- the golden has drifted")
            continue
        dr = degeneracy_reason(crop_stats(data))
        if dr is not None:
            fails.append(f"{gid}: blessed crop {m['bookmark']}/{m['region']} is "
                         f"un-failable -- {dr}")

    notes.append(f"{gid}: {len(members)} blessed, {len(refused)} recorded "
                 f"coverage hole(s)")
    return fails, notes


def run(quiet: bool = False, goldens: "Path | None" = None,
        manifests: "dict | None" = None, waivers: "dict | None" = None,
        retired: "dict | None" = None) -> int:
    gdir_root = goldens or GOLDENS
    retired = RETIRED_SETS if retired is None else retired
    manifests = manifests if manifests is not None else regionlib.load_all_manifests()
    waivers = LEGACY_WAIVERS if waivers is None else waivers
    fails, warns, notes = [], [], []
    n = 0
    seen = set()
    for gdir in sorted(p for p in gdir_root.iterdir() if p.is_dir()):
        if not (gdir / "golden.json").exists():
            continue
        n += 1
        seen.add(gdir.name)
        f, nt = check_golden(gdir, manifests)
        notes += nt
        if gdir.name in waivers:
            warns += [f"[WAIVED] {x}" for x in f]
        else:
            fails += f
    # A waiver must not outlive its set.
    for wid in sorted(set(waivers) - seen):
        fails.append(f"stale legacy waiver for golden set {wid!r}: no such set on "
                     f"disk -- remove it from LEGACY_WAIVERS")
    # A retired set must stay retired: resurrection has to be a deliberate edit
    # to RETIRED_SETS, not a silent restore of the directory.
    for rid in sorted(set(retired) & seen):
        fails.append(f"golden set {rid!r} is recorded RETIRED but is present on "
                     f"disk again -- it was deleted on purpose (see "
                     f"tests/visual/goldens/RETIRED.md). Either remove the "
                     f"directory, or remove it from RETIRED_SETS and state why "
                     f"the retirement no longer holds.")
    # A set cannot be simultaneously excused and retired.
    for bid in sorted(set(retired) & set(waivers)):
        fails.append(f"golden set {bid!r} appears in BOTH LEGACY_WAIVERS and "
                     f"RETIRED_SETS -- a retired set has no findings to waive")
    if not quiet:
        print("[check-golden-coverage] GOLDEN-COVERAGE-LEDGER-1")
        print(f"  golden sets inspected      : {n} "
              f"({len(seen & set(waivers))} legacy-waived)")
        for x in notes:
            print(f"  {x}")
        for wid in sorted(seen & set(waivers)):
            print(f"  WAIVED {wid}: {waivers[wid]}")
        for rid in sorted(retired):
            print(f"  RETIRED {rid}: {retired[rid]}")
        for w in warns:
            print(f"  WARN: {w}")
        for f in fails:
            print(f"  FAIL: {f}")
        print(f"  result: {'FAIL' if fails else 'PASS'} "
              f"({len(fails)} fail, {len(warns)} waived-warn)")
    return 1 if fails else 0


# ---------------------------------------------------------------------------
# Self-test: prove each clause fires on synthetic bad input.
# ---------------------------------------------------------------------------
def _selftest() -> int:
    sys.path.insert(0, str(ROOT / "tools" / "visual"))
    from png_io import encode_png
    failures = []

    def check(label, ok, detail=""):
        print(f"  [{'PASS' if ok else 'FAIL'}] {label}" + (f" -- {detail}" if detail else ""))
        if not ok:
            failures.append(label)

    def mk_png(flat: bool, w=32, h=32):
        px = bytearray(w * h * 3)
        if not flat:
            for y in range(h):
                for x in range(w):
                    i = (y * w + x) * 3
                    px[i] = (x * 7) % 256
                    px[i + 1] = (y * 11) % 256
                    px[i + 2] = (x * y) % 256
        return encode_png(w, h, 3, px)

    BM = "cov_selftest"
    manifests = {BM: {"bookmark": BM, "regions": [
        {"id": "a", "severity": "high", "rect": {"mode": "norm", "x": 0, "y": 0, "w": 1, "h": 1}},
        {"id": "b", "severity": "low", "rect": {"mode": "norm", "x": 0, "y": 0, "w": 1, "h": 1}},
    ]}}

    def build(tmp, members, refused, flat=False):
        gr = tmp / "goldens"
        gd = gr / "set1"
        (gd / BM).mkdir(parents=True, exist_ok=True)
        ms = []
        for rid in members:
            data = mk_png(flat)
            (gd / BM / f"{rid}.png").write_bytes(data)
            ms.append({"bookmark": BM, "region": rid, "path": f"{BM}/{rid}.png",
                       "sha256": hashlib.sha256(data).hexdigest()})
        (gd / "golden.json").write_text(json.dumps(
            {"members": ms, "refused": refused}), encoding="utf-8")
        return gr

    print("check-golden-coverage --selftest: GOLDEN-COVERAGE-LEDGER-1")
    cases = [
        ("C0 fully accounted set passes", ["a"],
         [{"bookmark": BM, "region": "b", "rule": "R", "reason": "why",
           "subject_protected": "what"}], False, 0),
        ("C1 unaccounted region is caught", ["a"], [], False, 1),
        ("C2 hole entry with no reason is caught", ["a"],
         [{"bookmark": BM, "region": "b", "rule": "R", "subject_protected": "x"}], False, 1),
        ("C3 region both blessed AND holed is caught", ["a", "b"],
         [{"bookmark": BM, "region": "b", "rule": "R", "reason": "w",
           "subject_protected": "x"}], False, 1),
        ("C4 degenerate blessed crop is caught", ["a", "b"], [], True, 1),
    ]
    for label, mem, ref, flat, want in cases:
        tmp = Path(tempfile.mkdtemp(prefix="covsel_"))
        try:
            gr = build(tmp, mem, ref, flat)
            rc = run(quiet=True, goldens=gr, manifests=manifests, waivers={})
            check(label, rc == want, f"exit {rc}, wanted {want}")
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    # C5: the degenerate clause must specifically name un-failability
    tmp = Path(tempfile.mkdtemp(prefix="covsel_"))
    try:
        gr = build(tmp, ["a", "b"], [], True)
        f, _ = check_golden(gr / "set1", manifests)
        check("C5 degenerate failure names it un-failable",
              any("un-failable" in x for x in f), (f[0][:90] if f else "no fail"))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print()
    if failures:
        print(f"check-golden-coverage selftest: FAIL ({len(failures)})")
        return 1
    print("check-golden-coverage selftest: OK -- every clause fired")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return _selftest()
    return run(args.quiet)


if __name__ == "__main__":
    sys.exit(main())
