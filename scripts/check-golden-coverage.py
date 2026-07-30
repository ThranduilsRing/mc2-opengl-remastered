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

  (C) A PENDING SET SILENTLY BECOMING TRACKED (PENDING-UNTRACKED-1). The
      PENDING_SETS table below says a candidate awaiting a user decision is
      "deliberately UNTRACKED" -- but the check that introduced that table never
      called git, so committing or staging the set left this suite at
      bit-identical pass/fail. Tracked-ness is now asked OF GIT, against the
      INDEX, so a `git add` is caught before it is ever committed.

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
import subprocess
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
# The degenerate mc2_10_saved_204907/distant_ridge_base crop that motivated
# DEGENERATE-CROP-REFUSAL-1 is present in several sets. Removing a region from a
# set is a decision on the record, so it is waived-and-surfaced, not edited here.
LEGACY_WAIVERS = {
    "v0.5.1-baseline": (
        "shipped release baseline (39 regions, blessed before the three-run "
        "all-pairs and degeneracy rules existed). Carries the degenerate "
        "distant_ridge_base crop. Historical artifact -- do not rewrite."),
}

# ---------------------------------------------------------------------------
# Pending user decision (PENDING-SET-STATE-1)
# ---------------------------------------------------------------------------
# A THIRD lifecycle state, distinct from both of the above.
#
#   LEGACY_WAIVERS  -- committed set, excused findings, MUST be on disk.
#   RETIRED_SETS    -- deliberately deleted, MUST NOT be on disk.
#   PENDING_SETS    -- a candidate awaiting a USER promotion decision. It is
#                      deliberately UNTRACKED: it must not be committed (that
#                      would pre-empt the decision) and must not be deleted
#                      (that would discard the candidate). Therefore it MAY or
#                      MAY NOT be present in any given checkout, and BOTH are
#                      correct. Present = the deciding user's working tree.
#                      Absent = every other checkout, CI, and fresh clone.
#
# Before this table existed, the only way to excuse a pending set's findings was
# a LEGACY_WAIVERS entry -- but that list FAILs on a waiver whose set is not on
# disk, so the entry was correct in the author's tree and a hard contract FAIL
# everywhere else. The state was not representable; this makes it representable.
#
# Findings in a present pending set are WARNed, not FAILed, for the same reason
# a waiver is: editing a set a human is about to promote is the human's call.
PENDING_SETS = {
    "v0.5.2-baseline-PROPOSED": (
        "USER-GATED: the set awaiting promotion to release golden. Deliberately "
        "untracked -- do NOT commit, delete, or promote it. It carries the "
        "degenerate distant_ridge_base crop (severity `critical`, un-failable) "
        "and blesses all 11 regions later refused for instability. RAISE BOTH "
        "AT THE PROMOTION GATE."),
}

# ---------------------------------------------------------------------------
# "Deliberately untracked" is a claim about GIT, so ASK GIT (PENDING-UNTRACKED-1)
# ---------------------------------------------------------------------------
# PENDING-SET-STATE-1 introduced the table above in response to a near-miss in
# which a sweep STAGED all 40 files of v0.5.2-baseline-PROPOSED, and wrote
# "deliberately UNTRACKED -- it must not be committed" into the docstring. But
# the checker it wrote that into never called git: there was no `subprocess`
# import anywhere in this file, and the only set-existence knowledge it had came
# from `gdir_root.iterdir()` -- i.e. from DIRECTORIES ON DISK. A pending set is
# on disk in exactly the same way whether it is committed or untracked, so
# committing all 40 files would have left this suite at bit-identical pass/fail.
# The rule was a comment, in the file whose entire purpose is to stop rules from
# being comments.
#
# So the tracked-ness question is asked of git directly, and asked of the INDEX
# (`git ls-files`) rather than of HEAD: the near-miss was a `git add` that had
# not been committed yet, and a rule that only notices after the commit lands
# has already missed the moment where it was cheap to fix.
#
# THIS DOES NOT FAIL OPEN. If git cannot be run, or answers non-zero, that is a
# FAIL and not a silent pass -- "the tool could not tell" and "the tool checked
# and it was fine" must never produce the same exit code. (Same lesson as
# DISK-GUARD-3, "stop failing open".)


class GitQueryError(RuntimeError):
    """git could not be asked, or refused to answer."""


def git_tracked_under(relpath: str, root: "Path | None" = None) -> "list[str]":
    """-> tracked (index-resident) paths under `relpath`. Raises GitQueryError.

    `git ls-files` reports the INDEX, so a staged-but-uncommitted `git add`
    counts as TRACKED. That is deliberate; see the note above.
    """
    root = ROOT if root is None else root
    try:
        cp = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z", "--", relpath],
            capture_output=True, timeout=120)
    except (OSError, subprocess.SubprocessError) as exc:
        raise GitQueryError(f"could not run git: {exc}") from exc
    if cp.returncode != 0:
        raise GitQueryError(
            f"git ls-files exited {cp.returncode}: "
            f"{cp.stderr.decode('utf-8', 'replace').strip()}")
    return [p.decode("utf-8", "replace")
            for p in cp.stdout.split(b"\0") if p]


def _default_tracked_query(gid: str) -> "list[str]":
    return git_tracked_under(f"tests/visual/goldens/{gid}")


def _raise_git_error(_gid: str) -> "list[str]":
    """Self-test stand-in for 'git could not be asked'."""
    raise GitQueryError("simulated: git binary unavailable")

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
        retired: "dict | None" = None, pending: "dict | None" = None,
        tracked_query=None) -> int:
    gdir_root = goldens or GOLDENS
    retired = RETIRED_SETS if retired is None else retired
    pending = PENDING_SETS if pending is None else pending
    # Only ask git about the REAL goldens tree. A caller pointing `goldens` at a
    # scratch directory (the self-test) is not asking a question about this
    # repository's index, so the default query is not installed there -- such
    # callers inject their own instead.
    if tracked_query is None and gdir_root == GOLDENS:
        tracked_query = _default_tracked_query
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
        elif gdir.name in pending:
            warns += [f"[PENDING-USER] {x}" for x in f]
        else:
            fails += f
    # A PENDING set's defining property is that it is UNTRACKED. Ask git, not
    # the filesystem -- being on disk says nothing either way (PENDING-UNTRACKED-1).
    pending_tracked: dict = {}
    if tracked_query is not None:
        for pid in sorted(pending):
            try:
                hits = tracked_query(pid)
            except GitQueryError as exc:
                pending_tracked[pid] = None      # unknown, and that is a FAIL
                fails.append(
                    f"golden set {pid!r} is PENDING a user decision, but git "
                    f"could not be asked whether it is tracked ({exc}). "
                    f"Refusing to pass on an unanswered question -- 'could not "
                    f"check' must not look like 'checked and fine'.")
                continue
            pending_tracked[pid] = hits
            if hits:
                sample = ", ".join(hits[:3]) + (" ..." if len(hits) > 3 else "")
                fails.append(
                    f"golden set {pid!r} is recorded PENDING a USER decision but "
                    f"is TRACKED in git: {len(hits)} file(s) in the index "
                    f"({sample}). Committing or staging it PRE-EMPTS the "
                    f"decision it is waiting on. Un-track it with "
                    f"`git rm --cached -r tests/visual/goldens/{pid}` (the files "
                    f"stay on disk, which is correct), or -- if the user has "
                    f"actually promoted it -- move it out of PENDING_SETS and "
                    f"say so.")
    # A waiver must not outlive its set. (A PENDING set is exempt by design: its
    # absence is the expected state everywhere but the deciding user's tree.)
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
    # The three lifecycle states are mutually exclusive: each set is in at most
    # one table, or the tables disagree about what the set IS.
    for a, b, an, bn, why in (
            (retired, waivers, "RETIRED_SETS", "LEGACY_WAIVERS",
             "a retired set has no findings to waive"),
            (retired, pending, "RETIRED_SETS", "PENDING_SETS",
             "a set cannot be both deleted-on-purpose and awaiting promotion"),
            (waivers, pending, "LEGACY_WAIVERS", "PENDING_SETS",
             "a waiver requires the set on disk; a pending set need not be -- "
             "pick one"),
    ):
        for bid in sorted(set(a) & set(b)):
            fails.append(f"golden set {bid!r} appears in BOTH {an} and {bn} -- "
                         f"{why}")
    if not quiet:
        print("[check-golden-coverage] GOLDEN-COVERAGE-LEDGER-1")
        print(f"  golden sets inspected      : {n} "
              f"({len(seen & set(waivers))} legacy-waived, "
              f"{len(seen & set(pending))} pending-user)")
        for x in notes:
            print(f"  {x}")
        for wid in sorted(seen & set(waivers)):
            print(f"  WAIVED {wid}: {waivers[wid]}")
        for pid in sorted(pending):
            state = ("PRESENT on disk" if pid in seen
                     else "ABSENT from this checkout")
            if pid not in pending_tracked:
                vcs = "git not consulted"
            elif pending_tracked[pid] is None:
                vcs = "git UNAVAILABLE"
            elif pending_tracked[pid]:
                vcs = f"TRACKED in git ({len(pending_tracked[pid])} files)"
            else:
                vcs = "untracked in git (correct)"
            print(f"  PENDING-USER {pid} [{state}; {vcs}]: {pending[pid]}")
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

    # C6-C10: the three lifecycle states (PENDING-SET-STATE-1)
    HOLE = [{"bookmark": BM, "region": "b", "rule": "R", "reason": "why",
             "subject_protected": "what"}]
    lifecycle = [
        # (label, on-disk sets, waivers, pending, retired, want)
        ("C6 waiver whose set is absent FAILs (stale)",
         True, {"ghost": "r"}, {}, {}, 1),
        ("C7 PENDING set absent from checkout PASSes (untracked is expected)",
         True, {}, {"ghost": "r"}, {}, 0),
        ("C8 PENDING set present on disk has findings WARNed not FAILed",
         False, {}, {"set1": "r"}, {}, 0),
        ("C9 same set in PENDING and RETIRED FAILs",
         True, {}, {"x": "r"}, {"x": "r"}, 1),
        ("C10 same set in PENDING and LEGACY_WAIVERS FAILs",
         True, {"x": "r"}, {"x": "r"}, {}, 1),
    ]
    for label, good, wv, pd, rt, want in lifecycle:
        tmp = Path(tempfile.mkdtemp(prefix="covsel_"))
        try:
            # good=True -> a fully accounted set1 (no findings of its own)
            gr = build(tmp, ["a"], HOLE if good else [], flat=not good)
            rc = run(quiet=True, goldens=gr, manifests=manifests,
                     waivers=wv, pending=pd, retired=rt)
            check(label, rc == want, f"exit {rc}, wanted {want}")
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    # C12-C15: the tracked-ness rule (PENDING-UNTRACKED-1). These are UNIT
    # cases with an injected query and they are NOT the proof -- the proof is
    # the live injection recorded in the commit message (`git add` the real 40
    # paths, watch this contract go red, unstage, watch it go green). They exist
    # so a later refactor cannot delete the rule without a red test.
    tracked_cases = [
        ("C12 PENDING set TRACKED in git FAILs",
         lambda gid: [f"tests/visual/goldens/{gid}/golden.json"], 1),
        ("C13 PENDING set untracked in git PASSes",
         lambda gid: [], 0),
        ("C14 git query failure FAILs (does not fail open)",
         _raise_git_error, 1),
    ]
    for label, q, want in tracked_cases:
        tmp = Path(tempfile.mkdtemp(prefix="covsel_"))
        try:
            gr = build(tmp, ["a"], HOLE)
            rc = run(quiet=True, goldens=gr, manifests=manifests, waivers={},
                     pending={"set1": "pending"}, retired={}, tracked_query=q)
            check(label, rc == want, f"exit {rc}, wanted {want}")
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    # C15: the rule must be reachable through the DEFAULT path, not only when a
    # query is injected. Without this, deleting the `tracked_query = _default_*`
    # wiring in run() would leave C12-C14 green and the contract dead -- which is
    # precisely the failure this whole check exists to prevent.
    check("C15 real goldens root installs the git query by default",
          run.__defaults__ is not None and _default_tracked_query is not None
          and "tracked_query" in run.__code__.co_varnames,
          "run() must accept and default-install tracked_query")
    try:
        real = git_tracked_under("scripts/check-golden-coverage.py")
        check("C15b git_tracked_under sees a known-tracked file",
              real == ["scripts/check-golden-coverage.py"], f"got {real}")
    except GitQueryError as exc:
        check("C15b git_tracked_under sees a known-tracked file", False, str(exc))

    # C11: a retired set reappearing on disk still FAILs (GOLDEN-6C47-AUDIT-1
    # guard -- must survive the PENDING_SETS change intact)
    tmp = Path(tempfile.mkdtemp(prefix="covsel_"))
    try:
        gr = build(tmp, ["a"], HOLE)
        rc_absent = run(quiet=True, goldens=gr, manifests=manifests, waivers={},
                        pending={}, retired={"set1": "retired on purpose"})
        check("C11 retired set present on disk FAILs", rc_absent == 1,
              f"exit {rc_absent}, wanted 1")
        rc_gone = run(quiet=True, goldens=gr, manifests=manifests, waivers={},
                      pending={}, retired={"notondisk": "retired on purpose"})
        check("C11b retired set absent from disk PASSes", rc_gone == 0,
              f"exit {rc_gone}, wanted 0")
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
