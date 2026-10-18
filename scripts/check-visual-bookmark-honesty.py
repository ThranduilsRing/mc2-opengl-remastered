#!/usr/bin/env python3
"""check-visual-bookmark-honesty.py -- VERIFY-INTEGRITY-3

Fail the build when a visual-capture bookmark file CANNOT SHOW WHAT IT GRADES.

WHY THIS EXISTS
---------------
`tests/visual/bookmarks/*.json` files are the poses every visual A/B is graded
at. A pose that does not frame its subject does not merely fail to prove the
fix -- it actively PASSES, because both arms render the same empty terrain and
the diff is zero. That is worse than having no pose at all: it manufactures
evidence.

This was not hypothetical. Four files (mc2_03, mc2_10, mc2_17, mc2_24) carried
the same three poses, BYTE-IDENTICAL across four different maps, under a note
saying "S9 v1 PROVISIONAL ... re-author from an in-game [BOOKMARK v1] dump".
Nobody re-authored them. Lane VIS-SEMANTICS-FINISH-2 then ran a real A/B on
mc2_10, got byte-identical frames in BOTH arms while the log recorded 4801
rejects, and an earlier lane had already "passed at four poses" while the user
kept seeing the bug. All four files were deleted; this checker stops the next
copy-paste from reintroducing them.

WHAT IT CHECKS
--------------
1. DUPLICATE POSE SETS ACROSS MISSIONS. Identical (pos, rot, proj, alt) tuples
   in files targeting DIFFERENT missions. A pose set that is map-independent is
   by construction not framing anything map-specific. This is the check that
   would have caught the original defect on day one.
2. PROVISIONAL/TEMPLATE SELF-ADMISSION. A file whose own note says it is
   PROVISIONAL / TEMPLATE / PLACEHOLDER / TODO-re-author is telling you it is
   not evidence. Honour it.
3. MISSING PROVENANCE. Every file must carry a non-trivial `_note` saying where
   its poses came from and what they frame.
4. EMPTY / UNDECLARED `covers`. A pose that does not declare what it is
   supposed to show cannot be audited for whether it shows it.

WHAT IT DELIBERATELY DOES NOT CHECK
-----------------------------------
Whether the subject is ACTUALLY in the rendered frame. That needs a render and
a human or an oracle, and faking it here would be the same sin one level up.
This checker enforces the cheap structural invariants that are sufficient to
catch copy-paste templating, and no more.

Exit 0 = clean, 1 = violations found.
"""
from __future__ import annotations

import json
import subprocess
import sys
from collections import defaultdict
from pathlib import Path
import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

ROOT = Path(__file__).resolve().parents[1]
BOOKMARK_DIR = ROOT / "tests" / "visual" / "bookmarks"
GOLDEN_DIR = ROOT / "tests" / "visual" / "goldens"

# The two pose populations, and why both must be graded.
#
# Poses in tests/visual/bookmarks/ serve an ad-hoc lane A/B. Poses that grade
# RELEASES come from <install>/saved_bookmarks/ and are vendored per golden set
# as tests/visual/goldens/<set>/poses/*.json. The two sets are DISJOINT: a
# fraudulent golden pose -- one that frames empty terrain, so both arms match
# and every region "passes" -- would otherwise escape this checker.
#
# A hand audit is not a ratchet; the next golden gets no such treatment. Both
# populations are graded by the same rules.
GOLDEN_POSE_GLOB = "*/poses/*.json"
REGION_DIR = ROOT / "tools" / "visual_verify" / "regions"


def golden_sets() -> list[Path]:
    if not GOLDEN_DIR.is_dir():
        return []
    return sorted(d for d in GOLDEN_DIR.iterdir()
                  if d.is_dir() and (d / "golden.json").is_file())


def tracked_golden_pose_files() -> list[Path]:
    """Vendored golden poses, tracked-only (same rule as bookmarks)."""
    try:
        out = subprocess.run(
            ["git", "ls-files", "--", f"tests/visual/goldens/{GOLDEN_POSE_GLOB}"],
            cwd=str(ROOT), capture_output=True, text=True, timeout=30, **console_child.child_kwargs(redirected=True))
        if out.returncode == 0 and out.stdout.strip():
            return sorted(ROOT / line for line in out.stdout.split() if line)
    except Exception:
        pass
    return sorted(GOLDEN_DIR.glob(GOLDEN_POSE_GLOB))


def tracked_bookmark_files() -> list[Path]:
    """Only grade files that are IN THE REPO.

    Untracked JSON in this directory is a live lane's scratch (scouting grids,
    one-off sweeps). Those are that lane's business and are not evidence
    anybody else reads, so failing CI on them would just punish whoever happens
    to have a working file on disk. Falls back to a plain glob outside a repo.
    """
    try:
        out = subprocess.run(
            ["git", "ls-files", "--", "tests/visual/bookmarks/*.json"],
            cwd=str(ROOT), capture_output=True, text=True, timeout=30, **console_child.child_kwargs(redirected=True))
        if out.returncode == 0 and out.stdout.strip():
            return sorted(ROOT / line for line in out.stdout.split() if line)
    except Exception:
        pass
    return sorted(BOOKMARK_DIR.glob("*.json"))

# Self-admissions that a pose set is not finished evidence.
PROVISIONAL_MARKERS = ("provisional", "placeholder", "template poses",
                       "re-author", "reauthor", "todo", "fixme", "temporary")

# Files may opt out of the duplicate-pose check with this key when sharing
# poses across missions is the deliberate point (e.g. a cross-map A/B oracle
# that must hold the camera constant). The opt-out must carry a reason.
OPT_OUT_KEY = "_shared_poses_reason"

# Explicit, owned acknowledgement that a file's framing is not yet proven.
# Downgrades a PROVISIONAL failure to a persistent WARNING (see main()).
ACK_KEY = "_unverified_framing"


def pose_signature(bookmark: dict) -> tuple:
    """Geometry-only identity of a pose. Name and `covers` deliberately excluded
    -- renaming a copy-pasted pose must not launder it."""
    return (
        tuple(round(float(c), 4) for c in bookmark.get("pos", [])),
        round(float(bookmark.get("rot", 0.0)), 4),
        round(float(bookmark.get("proj", 0.0)), 4),
        round(float(bookmark.get("alt", 0.0)), 4),
    )


def main() -> int:
    if not BOOKMARK_DIR.is_dir():
        print(f"[bookmark-honesty] SKIP: no {BOOKMARK_DIR}")
        return 0

    lane_files = [p for p in tracked_bookmark_files() if p.is_file()]
    golden_files = [p for p in tracked_golden_pose_files() if p.is_file()]
    files = lane_files + golden_files
    golden_set_members = set(golden_files)
    if not files:
        print("[bookmark-honesty] SKIP: no bookmark files")
        return 0

    violations: list[str] = []
    warnings: list[str] = []

    # A golden set whose poses are NOT vendored cannot be audited by anything --
    # not by this checker, not by a human, not by the next lane. Its poses exist
    # only inside a deploy install, which is not version-controlled; if that dir
    # is wiped the set can never be re-captured OR checked for fraud. That is a
    # strictly worse position than a bad pose you can read.
    for gset in golden_sets():
        if not any((gset / "poses").glob("*.json")):
            warnings.append(
                f"tests/visual/goldens/{gset.name}/: NO vendored poses/ dir -- "
                f"the poses that define this golden are UNAUDITABLE and "
                f"unrecoverable (they live only in a deploy install). Nothing, "
                f"including this checker, can tell whether they frame their "
                f"subjects. Vendor them as poses/*.json before this set is "
                f"promoted or cited.")
    # full pose-set signature -> [(file, mission)]
    setsig: dict[tuple, list[tuple[str, str]]] = defaultdict(list)

    for path in files:
        rel = path.relative_to(ROOT).as_posix()
        try:
            doc = json.loads(path.read_text(encoding="utf-8"))
        except Exception as exc:
            violations.append(f"{rel}: unparseable JSON ({exc})")
            continue

        mission = doc.get("mission", "")
        note = str(doc.get("_note", "") or "")
        marks = doc.get("bookmarks", []) or []

        # (3) provenance
        if len(note.strip()) < 40:
            violations.append(
                f"{rel}: missing/'too thin' _note ({len(note.strip())} chars). "
                f"State where these poses came from and what they frame.")

        # (2) self-admitted provisional
        low = note.lower()
        hits = [m for m in PROVISIONAL_MARKERS if m in low]
        # An explicit, owned acknowledgement DOWNGRADES the failure to a warning.
        # This is deliberate: some poses genuinely cannot be proven without an
        # operator eyeball, and the honest response to that is to SURFACE it on
        # every run, not to quietly delete the file or quietly let it pass. The
        # acknowledgement must name what is unproven, so it cannot be used as a
        # blanket silencer.
        ack = str(doc.get(ACK_KEY, "") or "").strip()
        if hits and "provenance correction" not in low and ack:
            if len(ack) < 30:
                violations.append(
                    f"{rel}: {ACK_KEY} is too thin ({len(ack)} chars) to be a "
                    f"real acknowledgement. Name what is unproven and who owns "
                    f"proving it.")
            else:
                warnings.append(f"{rel}: UNVERIFIED FRAMING -- {ack}")
        elif hits and "provenance correction" not in low:
            violations.append(
                f"{rel}: _note self-admits the poses are not finished evidence "
                f"({', '.join(sorted(hits))}). Re-author from an in-game "
                f"[BOOKMARK v1] dump or delete the file -- a pose that cannot "
                f"show what it grades passes both A/B arms and manufactures a "
                f"false PASS.")

        if not marks:
            violations.append(f"{rel}: no bookmarks defined")
            continue

        # (4) undeclared subject.
        #
        # The two pose populations declare their subject in different places,
        # and applying the lane rule verbatim to golden poses would be a false
        # positive that invites someone to paste an empty `covers` key to
        # silence it. A lane bookmark declares `covers` inline. A GOLDEN pose is
        # graded region-by-region against tools/visual_verify/regions/<stem>.json,
        # which carries `framing` (what the pose frames) and a per-region `check`
        # (what that crop must show) -- a strictly richer declaration. Require
        # THAT, and require it to be real prose, not a stub.
        if path in golden_set_members:
            regfile = REGION_DIR / f"{path.stem}.json"
            if not regfile.is_file():
                violations.append(
                    f"{rel}: golden pose has NO region definition at "
                    f"{regfile.relative_to(ROOT).as_posix()}. Nothing declares "
                    f"what this pose is supposed to show, so no crop taken at "
                    f"it can be audited -- and an empty frame would grade as a "
                    f"clean PASS in both arms.")
            else:
                try:
                    rdoc = json.loads(regfile.read_text(encoding="utf-8"))
                except Exception as exc:
                    violations.append(f"{rel}: region file unparseable ({exc})")
                    rdoc = {}
                if len(str(rdoc.get("framing", "") or "").strip()) < 20:
                    violations.append(
                        f"{rel}: region file declares no/thin `framing`. State "
                        f"what this pose actually frames.")
                regions = rdoc.get("regions") or []
                if not regions:
                    violations.append(
                        f"{rel}: region file declares zero regions -- this "
                        f"golden pose grades nothing.")
                for r in regions:
                    if len(str(r.get("check", "") or "").strip()) < 20:
                        violations.append(
                            f"{rel}: region '{r.get('id','?')}' declares no/thin "
                            f"`check` -- it cannot be audited for whether the "
                            f"crop shows its subject.")
        else:
            for b in marks:
                if not b.get("covers"):
                    violations.append(
                        f"{rel}: pose '{b.get('name','?')}' declares no `covers` "
                        f"-- it cannot be audited for whether it shows its "
                        f"subject.")

        if OPT_OUT_KEY not in doc:
            try:
                sig = tuple(pose_signature(b) for b in marks)
            except (TypeError, ValueError) as exc:
                violations.append(f"{rel}: malformed pose geometry ({exc})")
                continue
            setsig[sig].append((rel, mission))

    # (1) identical pose sets across DIFFERENT missions
    for sig, owners in setsig.items():
        missions = {m for _, m in owners}
        if len(owners) > 1 and len(missions) > 1:
            listed = ", ".join(f"{f} (mission={m or '?'})" for f, m in owners)
            violations.append(
                f"identical pose set shared across DIFFERENT missions: {listed}. "
                f"{len(sig)} pose(s), byte-identical geometry. A map-independent "
                f"pose set cannot be framing anything map-specific -- this is the "
                f"copy-paste template defect that produced byte-identical A/B "
                f"frames on mc2_10. Author per-map poses, or add "
                f'"{OPT_OUT_KEY}": "<why sharing is correct here>" to each file.')

    # Acknowledged-but-unproven framing is printed on EVERY run, pass or fail.
    # An acknowledgement that nobody ever sees is just a silent pass with extra
    # steps -- which is the failure mode this whole script exists to stop.
    for w in warnings:
        print(f"[bookmark-honesty] WARN {w}")

    if violations:
        print(f"\n[bookmark-honesty] FAIL: {len(violations)} violation(s)\n")
        for v in violations:
            print(f"  - {v}")
        print("\n  Rule: a pose that cannot show the thing it grades is worse "
              "than no pose.")
        return 1

    print(f"[bookmark-honesty] OK: {len(files)} tracked pose file(s) clean "
          f"({len(lane_files)} lane bookmarks + {len(golden_files)} vendored "
          f"golden poses across {len(golden_sets())} golden set(s))"
          + (f"; {len(warnings)} carry ACKNOWLEDGED unverified framing "
             f"(see WARN above -- do not cite those as proof)"
             if warnings else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
