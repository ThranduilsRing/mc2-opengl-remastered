#!/usr/bin/env python3
"""CAPTURE-PROVENANCE-TRUTH-1 ratchet -- a manifest must name the binary it measured.

THE DEFECT THIS EXISTS FOR
--------------------------
`identity.git.commit` is `git rev-parse HEAD` in the worktree the emitting tool
ran from. It has no causal relationship to the commit compiled into the binary
being measured, and it sits directly beside `"dirty": false`, which makes the
pair read as authoritative provenance.

Measured 2026-07-30 over 404 manifests on this machine: of the 31 whose recorded
exe still exists byte-identical -- the only ones where the truth is checkable at
all -- 28 name a commit their binary was NOT built from. scratch/mc203reg and
scratch/mc203air record SEVEN distinct `git.commit` values across 23 arms while
every arm ran the SAME binary, built from 79360fdb on branch
claude/spotlight-direction-1. A reader of `04_animstatic` would conclude it
exercised 3ab322a0 (OVERLAY-CEMENT-ADOPT-1); that code was never in the binary.
23 of the 31 also disagree on branch, 11 on dirty.

That is the failure mode that makes a fix look proven when it was never
compiled in. This checker refuses it.

WHAT IT ENFORCES
----------------
  FAIL  zero-byte or unparseable manifest (no provenance whatsoever)
  FAIL  a binary-bearing manifest with no identity.exe.sha256 -- an mtime is
        not an identity; the measurement is unattributable the moment the file
        is overwritten
  FAIL  identity.exe.embedded_fingerprint absent, when the exe is still on disk
        byte-identical and DOES carry a readable stamp (retrofit with --write)
  FAIL  recorded provenance contradicted by the binary: the manifest's
        fingerprint disagrees with a live re-read of the exe it names, or the
        fingerprint was read from bytes other than the sha256 it records
  FAIL  provably misattributed legacy manifest: exe still byte-identical, and
        its embedded commit disagrees with the recorded git.commit
  WARN  the recorded exe is gone or has been replaced -- provenance is
        permanently unknowable. `--strict-unverifiable` promotes this to FAIL.

A source HEAD that differs from the binary's commit is NOT a failure. That is
normal in a multi-lane worktree setup. The failure is a manifest that does not
SAY so.

WHAT IT DOES NOT DO
-------------------
It never reads generated/build_fingerprint.h. A failed compile leaves that
header freshly stamped and the exe stale, so a guard reading it passes while
measuring the wrong binary (c5d934d5 fixed exactly that in a sibling guard).
Truth is read out of the binary's bytes, or it is reported as unknown.

GOLDEN-PROVENANCE-TRUTH-1 (2026-07-30) -- THE RATCHET COULD NOT SEE THE GOLDENS
------------------------------------------------------------------------------
`golden.json` was in neither MANIFEST_NAMES nor DEFAULT_ROOTS, so the most
authoritative artifact class in the repo -- the reference every release is
graded against -- was 100% outside this checker. Same class of hole as
PROVENANCE-RATCHET-SCOPE-1 above (scratch/ excluded), and as a shader checker
that silently skipped every GLSL file: the instrument reported clean on a corpus
it never opened.

Adding both took the default scan from 141 manifests to 144 and immediately
surfaced 3 FAILs on tracked files. All three committed goldens name their binary
by PATH + MTIME only. `v0.5.1-baseline` and `v0.5.2-baseline-PROPOSED` name the
SAME path with different mtimes and the file there today matches only the
second, so the binary that minted the first is overwritten and gone.

A golden is graded by pg.establish() rather than by the generic exe rules: it is
the artifact class where "which binary" matters most, and its `blessed_commit`
field -- the BLESSER's worktree HEAD -- is explicitly refused as provenance.
`--write` will NOT touch a golden: a golden's binary identity had to be recorded
at mint time, and back-stamping one now would fabricate exactly the authority
this checker exists to protect.

RED MUST MEAN "ACT NOW" (KNOWN-FAILURE LEDGER)
----------------------------------------------
Two of the FAILs here are irreparable and correct: zero-byte manifests from an
ENOSPC truncation. A check that is permanently red trains people to ignore red,
and the usual cure -- a suppression list -- can hide a real failure.

So this is not a suppression list. scripts/provenance-known-failures.json pins
EXACT finding identities (code + path) and is consumed EXACTLY:

  * a FAIL not in the ledger            -> RED (regression)
  * a ledger entry with no matching FAIL -> RED (stale ledger, or -- the case
                                            that matters -- this instrument went
                                            blind and stopped seeing it)
  * a ledger entry on a REPAIRABLE code  -> RED (the ledger may only ever
                                            acknowledge findings no tool can fix)
  * a ledger entry past its expiry       -> RED
  * every FAIL ledgered, irreparable, unexpired -> exit 3, reported as KNOWN

KNOWN is never PASS. It prints its own row in the contract table with the reason
and the expiry. And because ledger entries must be OBSERVED, the ledger doubles
as a liveness assertion on the checker itself: if this scan ever stops finding
those two files, it fails loudly instead of turning green.

Usage:
    py -3 scripts/check-capture-provenance.py [ROOT ...]      # verify (default)
    py -3 scripts/check-capture-provenance.py --write [ROOT]  # backfill truth
    py -3 scripts/check-capture-provenance.py --selftest      # prove it fires

Exit: 0 clean | 1 violation(s) | 2 self-test failed | 3 failures, all KNOWN.
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import sys
import tempfile
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import exe_fingerprint as ef        # noqa: E402
import manifest_schema as ms        # noqa: E402
import provenance_gate as pg        # noqa: E402

#: Artifact trees the repo treats as EVIDENCE.
#:
#: `scratch/` must stay included. Operator working space is where conclusions
#: are formed, so it is exactly where attribution has to hold; excluding it left
#: the default run skipping the corpus most likely to be misattributed (arms
#: recording different git.commit values for one binary) and certifying nothing.
#: `tests/visual/goldens` and `golden.json` are likewise required: a filename
#: this checker does not recognise inside a root it does not walk is invisible
#: twice over, which left the artifact class that GRADES RELEASES unscanned.
DEFAULT_ROOTS = ("tests/visual/captures", "tests/visual/baselines",
                 "tests/visual/goldens", "run", "scratch")

#: Manifest filenames that record a measurement of a binary.
MANIFEST_SUFFIXES = ("_capture_manifest.json",)
MANIFEST_NAMES = ("capture_run.json", "manifest.json", "set.json",
                  "golden.json")

SKIP_DIRS = {".git", "build64", "__pycache__", "node_modules"}

#: Findings NO TOOL CAN REPAIR: the artifact recorded nothing, so there is
#: nothing to reconcile against any binary, ever. Only these may be ledgered as
#: known-correct failures. Every other code has a real remedy (--write, a
#: recapture, a re-bless) and must stay RED until someone performs it.
IRREPARABLE_CODES = ("EMPTY-MANIFEST", "UNPARSEABLE")

#: The known-correct-failure ledger. See the module docstring: this is NOT a
#: suppression list -- it is consumed exactly, in both directions.
LEDGER_PATH = ROOT / "scripts" / "provenance-known-failures.json"

#: The longest acknowledgement this ledger accepts.
#:
#: A KNOWN row never touches the suite's exit code -- that is the whole point of
#: the tri-state runner, and it is defensible only because an acknowledgement is
#: temporary. The expiry check below rejected a MISSING expiry with the words
#: "an acknowledgement with no expiry is a permanent suppression", and then
#: accepted `expires_utc: "2999-12-31"`, which is the same thing spelled in a
#: way that parses. An entry that outlives everyone who could review it is a
#: permanent suppression whether the date field is empty or absurd.
#:
#: One year: long enough that a genuinely irreparable artifact is not re-argued
#: every sprint, short enough that every entry is re-decided by a human who can
#: still remember why it is there.
#:
#: WHAT THIS CAP DOES NOT DO -- state it rather than let the number imply more.
#: The window is measured from the RUN DATE
#: (`today + MAX_ACK_DAYS` in reconcile_ledger below), not from when the entry
#: was first written. `acknowledged_utc` is recorded on every entry and is read
#: by NOTHING in this file -- grep it. So an entry CAN sit amber indefinitely:
#: editing one date field once a year renews it forever, and nothing here counts
#: renewals or remembers the original acknowledgement. What the cap buys is that
#: the renewal is an explicit, dated, tracked-file diff that a human has to type
#: on purpose -- not that the acknowledgement expires in fact.
#:
#: Closing that would need something this repo does not have (an append-only
#: store, or a first_acknowledged_utc that a renewer has no reason to leave
#: alone -- an honour-system field is not a control). It is recorded here as a
#: known limitation instead of being papered over with a mechanism that only
#: looks like one. If an entry here reaches its second renewal, that is the
#: signal to retire the artifacts rather than re-acknowledge them again.
MAX_ACK_DAYS = 365


# --------------------------------------------------------------------------- #
# Findings
# --------------------------------------------------------------------------- #

#: The emitting call site. If this printf is reformatted, exe_fingerprint stops
#: recognising the stamp -- and every manifest would then quietly record
#: "no fingerprint" while this checker found nothing to complain about. That is a
#: blind instrument, so the coupling is asserted on every run.
EMITTER_SRC = ROOT / "code" / "mechcmd2.cpp"


def verify_source_coupling() -> list[str]:
    """Assert the extractor's literals still match the code that emits them."""
    problems: list[str] = []
    try:
        src = EMITTER_SRC.read_text(encoding="utf-8", errors="replace")
    except Exception as e:  # noqa: BLE001
        return [f"cannot read {EMITTER_SRC}: {e}"]
    fmt = ef.BANNER_FMT[:-1].decode("ascii").replace("\n", "\\n")
    if fmt not in src:
        problems.append(
            f"exe_fingerprint.BANNER_FMT no longer matches the printf in "
            f"{EMITTER_SRC.name}. Every fingerprint read would silently come "
            f"back absent. Expected literal: {fmt!r}")
    if ef.DIRTY_MARKER.decode("ascii") not in src:
        problems.append(
            f"exe_fingerprint.DIRTY_MARKER no longer matches "
            f"{EMITTER_SRC.name}; the binary's dirty flag would always read 0.")
    if "#if MC2_BUILD_GIT_DIRTY" not in src:
        problems.append(
            "the dirty banner is no longer behind `#if MC2_BUILD_GIT_DIRTY`; "
            "absence of the marker string no longer implies a clean build, so "
            "the extracted `dirty` bit would be wrong.")
    return problems


class Finding:
    __slots__ = ("path", "level", "code", "msg")

    def __init__(self, path: Path | str, level: str, code: str, msg: str):
        self.path, self.level, self.code, self.msg = str(path), level, code, msg

    def __str__(self) -> str:
        return f"[{self.level}] {self.code}: {self.path}\n        {self.msg}"

    def ident(self) -> str:
        """Stable identity for the known-failure ledger: CODE + repo-relative
        path. Deliberately NOT a glob, a directory or a bare code -- a ledger
        entry must pin ONE finding on ONE file, so that a different failure in
        the same file, or the same failure in another file, is still RED."""
        p = Path(self.path)
        try:
            rel = p.resolve().relative_to(ROOT).as_posix()
        except Exception:  # noqa: BLE001 -- outside the repo: use it verbatim
            rel = p.as_posix()
        return f"{self.code}:{rel}"

    def repairable(self) -> bool:
        return self.code not in IRREPARABLE_CODES


def _sha256_file(p: Path) -> str | None:
    try:
        h = hashlib.sha256()
        with open(p, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        return h.hexdigest()
    except Exception:
        return None


def iter_manifests(root: Path):
    if root.is_file():
        yield root
        return
    for dp, dn, fn in os.walk(root):
        dn[:] = [d for d in dn if d not in SKIP_DIRS]
        for f in sorted(fn):
            if f.endswith(MANIFEST_SUFFIXES) or f in MANIFEST_NAMES:
                yield Path(dp) / f


# --------------------------------------------------------------------------- #
# The check
# --------------------------------------------------------------------------- #

def _recorded_exe(doc: dict[str, Any]) -> tuple[str | None, str | None, dict | None]:
    """(exe_path, recorded_sha256, exe_block) across the manifest species.

    The unified species carries identity.exe{path,sha256}. The
    visual-verify-capture species (tools/visual_verify) carries a bare top-level
    `exe` string and an `exe_mtime` -- no digest at all, which is precisely the
    weakness this checker fails on rather than tolerates.
    """
    ident = doc.get("identity")
    if isinstance(ident, dict) and isinstance(ident.get("exe"), dict):
        blk = ident["exe"]
        return blk.get("path"), blk.get("sha256"), blk
    if isinstance(doc.get("exe"), str):
        return doc["exe"], doc.get("exe_sha256"), None
    return None, None, None


def check_manifest(path: Path, strict_unverifiable: bool) -> list[Finding]:
    out: list[Finding] = []
    try:
        size = path.stat().st_size
    except Exception as e:  # noqa: BLE001
        return [Finding(path, "FAIL", "UNREADABLE", f"cannot stat: {e}")]

    if size == 0:
        return [Finding(path, "FAIL", "EMPTY-MANIFEST",
                        "zero-byte manifest -- this capture recorded NO "
                        "provenance at all. Nothing it produced can be "
                        "attributed to any binary or commit. Delete the "
                        "artifacts or recapture them.")]
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except Exception as e:  # noqa: BLE001
        return [Finding(path, "FAIL", "UNPARSEABLE", f"not valid JSON: {e}")]
    if not isinstance(doc, dict):
        return [Finding(path, "FAIL", "UNPARSEABLE", "top level is not an object")]

    # ---- GOLDEN species ------------------------------------------------------
    # A golden is the reference releases are graded against, so it gets the
    # strongest available statement rather than the generic exe rules:
    # provenance_gate.establish() accepts only a content digest plus the
    # binary's own embedded build stamp, read from those same bytes, and
    # re-reads the binary live when it is still on disk.
    if doc.get("kind") == "visual-verify-golden":
        # Fires on a CLAIM, not on the key's mere presence: the three legacy
        # goldens carry `blessed_commit: null`, which asserts nothing and is
        # already covered by GOLDEN-UNATTRIBUTABLE below -- two findings for one
        # root cause is how a report becomes wallpaper. A NON-NULL value is the
        # actual regression: a commit stated at the top level of a golden that
        # describes the blesser's lane rather than the rendering binary.
        if doc.get("blessed_commit"):
            out.append(Finding(
                path, "FAIL", "SOURCE-HEAD-AS-PROVENANCE",
                "carries a non-null top-level `blessed_commit`. That field was "
                "`git rev-parse HEAD` in the BLESSER'S WORKTREE -- the lane "
                "that ran bless.py, which is routinely neither the lane that "
                "built the exe nor the commit that rendered these crops, and "
                "blessing can happen days after capture. It sits at the top "
                "level of the repo's most authoritative artifact where it "
                "reads as provenance. Re-bless with the current bless.py, "
                "which emits identity.binary_provenance (the commit read out "
                "of the binary) and records the blesser's HEAD under "
                "`source_worktree_head_at_bless` with describes_binary=false."))
        # `self_path` so a golden cannot be offered as its own corroborating
        # witness.
        est = pg.establish(doc, "golden", self_path=Path(path))
        if not est["ok"]:
            out.append(Finding(
                path, "FAIL", "GOLDEN-UNATTRIBUTABLE",
                f"cannot prove which binary minted its crops: {est['why']} "
                f"Every verdict graded against this golden inherits that "
                f"unknown. Re-bless from captures that identify their binary "
                f"(tools/visual_verify/bless.py now refuses input that does "
                f"not). Re-blessing a committed golden is a tracked-file diff "
                f"and a USER decision -- `--write` deliberately will not do it."))
        else:
            problems = ms.validate_provenance(doc)
            for pr in problems:
                out.append(Finding(path, "FAIL", "PROVENANCE-SHAPE", pr))
        return out

    exe_path, rec_sha, exe_blk = _recorded_exe(doc)
    if exe_path is None:
        # Not a binary-bearing manifest (e.g. a pure index). Nothing to check.
        return out

    if not rec_sha:
        out.append(Finding(
            path, "FAIL", "NO-EXE-SHA256",
            f"records exe {exe_path!r} with no sha256. An mtime or a path is "
            f"not an identity: overwrite the file and the measurement becomes "
            f"permanently unattributable. Emit identity via "
            f"manifest_schema.identity_block(exe_path=...)."))

    ident = doc.get("identity") if isinstance(doc.get("identity"), dict) else {}
    rec_fp = (exe_blk or {}).get("embedded_fingerprint")
    src_head = (ident.get("source_worktree_head_at_capture")
                or ident.get("git") or {})
    head_commit = src_head.get("commit") if isinstance(src_head, dict) else None

    # Internal consistency: a recorded fingerprint must have been read from the
    # bytes the manifest names.
    if isinstance(rec_fp, dict) and rec_fp.get("present"):
        scanned = rec_fp.get("exe_sha256_at_scan")
        if rec_sha and scanned and scanned != rec_sha:
            out.append(Finding(
                path, "FAIL", "FINGERPRINT-BYTES-MISMATCH",
                f"embedded_fingerprint was read from {scanned[:12]} but "
                f"exe.sha256 records {rec_sha[:12]} -- the manifest describes "
                f"two different binaries."))

    # Live reconciliation, when the recorded binary still exists unchanged.
    p = Path(exe_path)
    if not p.exists():
        lvl = "FAIL" if strict_unverifiable else "WARN"
        out.append(Finding(
            path, lvl, "UNVERIFIABLE-EXE-MISSING",
            f"recorded exe {exe_path} no longer exists; provenance can never "
            f"be reconstructed for this artifact."))
        return out

    live_sha = _sha256_file(p)
    if rec_sha and live_sha != rec_sha:
        lvl = "FAIL" if strict_unverifiable else "WARN"
        out.append(Finding(
            path, lvl, "UNVERIFIABLE-EXE-REPLACED",
            f"recorded exe has been rebuilt/overwritten since capture "
            f"(recorded {rec_sha[:12]}, on disk {(live_sha or '?')[:12]}); "
            f"the measured binary's commit is no longer recoverable."))
        return out

    live_fp = ef.extract(str(p))
    if not live_fp.get("present"):
        if isinstance(rec_fp, dict) and rec_fp.get("present"):
            out.append(Finding(
                path, "FAIL", "FINGERPRINT-CONTRADICTED",
                f"manifest records an embedded fingerprint "
                f"(sha={rec_fp.get('sha')}) but the binary carries none "
                f"({live_fp.get('reason')})."))
        else:
            out.append(Finding(
                path, "WARN", "BINARY-CARRIES-NO-STAMP",
                f"the recorded binary carries no build fingerprint "
                f"({live_fp.get('reason')}), so the commit it was built from "
                f"is not knowable from the artifact OR the binary. Any commit "
                f"attribution for this capture is unproven."))
        return out

    live_sha12 = live_fp["sha"]

    if not isinstance(rec_fp, dict) or not rec_fp.get("present"):
        # Legacy manifest. The binary is still here and DOES state its commit,
        # so the manifest's silence is retrofittable -- and if its git.commit
        # disagrees, the artifact is actively misattributed today.
        agrees = ef.sha_agrees(live_sha12, head_commit)
        if agrees is False:
            out.append(Finding(
                path, "FAIL", "MISATTRIBUTED",
                f"records git.commit={str(head_commit)[:12]} "
                f"(branch {src_head.get('branch')}) but the binary it names was "
                f"built from {live_sha12} (branch {live_fp['branch']}, worktree "
                f"{live_fp['src']}). Any conclusion this artifact supports is "
                f"attributed to the wrong commit. Run with --write to record "
                f"the truth."))
        else:
            out.append(Finding(
                path, "FAIL", "NO-EMBEDDED-FINGERPRINT",
                f"does not record the binary's own build stamp, though the "
                f"binary states it ({live_sha12}). Run with --write."))
        return out

    # Modern manifest: the recorded fingerprint must match the live re-read.
    if rec_fp.get("sha") != live_sha12:
        out.append(Finding(
            path, "FAIL", "FINGERPRINT-CONTRADICTED",
            f"manifest records binary commit {rec_fp.get('sha')} but the exe "
            f"it names is built from {live_sha12}."))
    if bool(rec_fp.get("dirty")) != bool(live_fp["dirty"]):
        out.append(Finding(
            path, "FAIL", "FINGERPRINT-CONTRADICTED",
            f"manifest records binary dirty={rec_fp.get('dirty')} but the exe "
            f"says dirty={live_fp['dirty']}."))

    problems = ms.validate_provenance(doc)
    for pr in problems:
        out.append(Finding(path, "FAIL", "PROVENANCE-SHAPE", pr))
    return out


# --------------------------------------------------------------------------- #
# --write: record what is still knowable, and mark the rest unknowable
# --------------------------------------------------------------------------- #

def rewrite_manifest(path: Path) -> str:
    """Backfill binary provenance. Returns a one-word outcome for reporting."""
    try:
        if path.stat().st_size == 0:
            return "skipped-empty"
        doc = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return "skipped-unparseable"
    if not isinstance(doc, dict):
        return "skipped-unparseable"

    # A golden's binary identity had to be recorded AT MINT TIME, from the exe
    # that rendered its crops. Back-stamping one now would record whatever
    # binary happens to sit at that path today -- which is exactly the defect
    # (for baseline-5ef261f4 that is a different lane's build). Refusing here
    # also keeps `--write` from mass-editing tracked files, which is a USER
    # decision, not a tool's.
    if doc.get("kind") == "visual-verify-golden":
        return "skipped-golden-not-backstampable"

    exe_path, rec_sha, exe_blk = _recorded_exe(doc)
    if exe_path is None:
        return "skipped-no-exe"

    ident = doc.setdefault("identity", {}) if isinstance(doc.get("identity"), (dict, type(None))) else None
    if ident is None:
        return "skipped-bad-identity"

    # Label the source-HEAD blocks so they cannot be read as provenance.
    head = ident.get("source_worktree_head_at_capture") or ident.get("git")
    if isinstance(head, dict):
        head.setdefault("describes", "source_worktree_head_at_capture")
        head["describes_binary"] = False
        head.setdefault("dirty_describes", ms.SOURCE_DIRTY_NOTE)
        head.setdefault("note", ms.SOURCE_HEAD_NOTE)
        ident["source_worktree_head_at_capture"] = head
        ident["git"] = head

    p = Path(exe_path)
    live_sha = _sha256_file(p) if p.exists() else None
    if live_sha is not None and (rec_sha is None or live_sha == rec_sha):
        fp = ef.extract(str(p))
        outcome = "backfilled" if fp.get("present") else "recorded-absent"
    else:
        fp = {
            "present": False,
            "reason": ("exe-missing" if live_sha is None else "exe-replaced"),
            "detail": ("the binary named by this manifest is gone" if live_sha is None
                       else f"on disk now {live_sha[:12]}, manifest recorded "
                            f"{str(rec_sha)[:12]}"),
        }
        outcome = "unknowable"

    if isinstance(exe_blk, dict):
        exe_blk["embedded_fingerprint"] = fp
        if not exe_blk.get("sha256") and live_sha and rec_sha is None:
            exe_blk["sha256"] = live_sha
        target_exe = exe_blk
    else:
        target_exe = {"path": exe_path,
                      "sha256": rec_sha or (live_sha if rec_sha is None else None),
                      "mtime_iso": doc.get("exe_mtime"),
                      "size_bytes": (p.stat().st_size if p.exists() else None),
                      "embedded_fingerprint": fp}
        ident["exe"] = target_exe

    ident["binary_provenance"] = ms.binary_provenance(target_exe, head or {})
    ident.setdefault("schema", ms.SCHEMA)
    doc["identity"] = ident
    path.write_text(json.dumps(doc, indent=2) + "\n",
                    encoding="utf-8", newline="\n")
    return outcome


# --------------------------------------------------------------------------- #
# KNOWN-CORRECT-FAILURE ledger -- so that RED still means "act now"
# --------------------------------------------------------------------------- #

LEDGER_SCHEMA = "mc2-known-failure-ledger/1"


def load_ledger(path: Path = None) -> "tuple[list[dict], list[str]]":
    """(entries, problems). A malformed ledger is a PROBLEM, never an empty
    ledger: silently treating an unreadable suppression file as 'nothing
    suppressed' is how a guard's first version ended up allowing everything."""
    p = Path(path or LEDGER_PATH)
    if not p.exists():
        return [], []
    try:
        doc = json.loads(p.read_text(encoding="utf-8"))
    except Exception as e:  # noqa: BLE001
        return [], [f"ledger {p} is not valid JSON ({e}); refusing to treat an "
                    f"unreadable ledger as an empty one"]
    if not isinstance(doc, dict) or doc.get("schema") != LEDGER_SCHEMA:
        return [], [f"ledger {p} does not declare schema {LEDGER_SCHEMA!r}"]
    entries = doc.get("entries")
    if not isinstance(entries, list):
        return [], [f"ledger {p} has no `entries` list"]
    return entries, []


def _entry_in_scope(eid: str, roots: "list[Path] | None") -> bool:
    """Is the file an entry pins inside the roots this run actually scanned?

    `roots=None` (the default full scan) means everything is in scope, which is
    what the contract runs and where the blind-instrument property has to hold.
    An ad-hoc subset scan should not be told its ledger is stale merely because
    it did not look there -- and narrowing the roots is NOT a way to hide an
    entry, because it drops the corresponding FINDING too, which is a far more
    visible change and a tracked diff in check-contracts.sh.
    """
    if roots is None:
        return True
    try:
        p = (ROOT / eid.split(":", 1)[1]).resolve()
    except Exception:  # noqa: BLE001
        return True
    for r in roots:
        try:
            rr = Path(r).resolve()
        except Exception:  # noqa: BLE001
            continue
        if p == rr or rr in p.parents:
            return True
    return False


def reconcile_ledger(fails: "list[Finding]", entries: "list[dict]",
                     today: datetime.date = None,
                     roots: "list[Path] | None" = None
                     ) -> "tuple[list, list, list]":
    """(known_fails, unledgered_fails, ledger_problems).

    Consumed EXACTLY, in both directions. The reverse direction is the part
    that makes this not a suppression mechanism: an entry that matches nothing
    is itself a failure, so an entry cannot outlive the condition it describes,
    and this checker cannot go blind without the ledger noticing.
    """
    today = today or datetime.date.today()
    by_id: dict[str, Finding] = {f.ident(): f for f in fails}
    problems: list[str] = []
    known_ids: set[str] = set()

    seen: set[str] = set()
    for i, e in enumerate(entries):
        if not isinstance(e, dict):
            problems.append(f"entry #{i} is not an object")
            continue
        eid = e.get("id")
        if not isinstance(eid, str) or ":" not in eid:
            problems.append(f"entry #{i} has no usable `id` (want CODE:path)")
            continue
        if eid in seen:
            problems.append(f"entry {eid!r} appears twice")
            continue
        seen.add(eid)
        if not _entry_in_scope(eid, roots):
            continue
        if not e.get("why_irreparable"):
            problems.append(f"entry {eid!r} states no `why_irreparable`")
        exp = e.get("expires_utc")
        try:
            exp_d = datetime.date.fromisoformat(str(exp))
        except Exception:  # noqa: BLE001
            problems.append(
                f"entry {eid!r} has no valid `expires_utc` (YYYY-MM-DD). An "
                f"acknowledgement with no expiry is a permanent suppression.")
            exp_d = None
        if exp_d is not None and exp_d < today:
            problems.append(
                f"entry {eid!r} EXPIRED on {exp_d} -- re-decide it or remove "
                f"the artifacts; do not extend it reflexively.")
        elif exp_d is not None and exp_d > today + datetime.timedelta(days=MAX_ACK_DAYS):
            problems.append(
                f"entry {eid!r} expires {exp_d}, which is more than "
                f"{MAX_ACK_DAYS} days out. A KNOWN row never touches the exit "
                f"code, so an acknowledgement nobody will live to re-read is a "
                f"PERMANENT SUPPRESSION -- the exact thing the missing-expiry "
                f"rule above refuses, spelled in a way that parses. Set a date "
                f"within {MAX_ACK_DAYS} days and re-decide it then.")
            # Do not let it pin: an entry this checker refuses must not also be
            # the reason a finding stops being counted as unledgered.
            continue
        f = by_id.get(eid)
        if f is None:
            problems.append(
                f"entry {eid!r} matches NO current finding. Either the "
                f"condition was cleaned up (remove the entry) or this checker "
                f"stopped seeing it, which would mean the instrument is blind. "
                f"An unobserved ledger entry is a failure, not a courtesy.")
            continue
        if f.repairable():
            problems.append(
                f"entry {eid!r} acknowledges code {f.code}, which HAS a remedy. "
                f"The ledger may only ever pin findings no tool can repair "
                f"({', '.join(IRREPARABLE_CODES)}). Fix it instead.")
            continue
        known_ids.add(eid)

    known = [f for f in fails if f.ident() in known_ids]
    unledgered = [f for f in fails if f.ident() not in known_ids]
    return known, unledgered, problems


# --------------------------------------------------------------------------- #
# Self-test: inject the real regression, not a token
# --------------------------------------------------------------------------- #

def _synthetic_binary(sha: str, branch: str, built: str, src: str,
                      dirty: bool) -> bytes:
    """A byte image carrying a real, parseable build-fingerprint stamp.

    Mirrors the .rdata layout MSVC produces for the printf in
    code/mechcmd2.cpp: the four operands pooled, NUL-terminated, in reverse
    argument order immediately before the format literal.
    """
    pad = b"\x00filler string one\x00filler string two\x00"
    ops = (src.encode() + b"\x00" + built.encode() + b"\x00"
           + branch.encode() + b"\x00" + sha.encode() + b"\x00")
    blob = b"MZ" + b"\x00" * 64 + pad + ops + ef.BANNER_FMT
    if dirty:
        blob += ef.DIRTY_MARKER + b"\n\x00"
    return blob + b"\x00" * 32


def _write(path: Path, doc: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(doc, indent=2) + "\n",
                    encoding="utf-8", newline="\n")


def _selftest() -> int:
    """Prove the ratchet detects the REAL regression.

    A lane previously shipped two rules that passed their own self-tests while
    satisfied merely by an identifier appearing in a comment. So case 6 below
    puts every keyword this checker looks for into a comment string and asserts
    the manifest STILL fails, and case 5 asserts a genuinely-correct manifest
    passes -- a checker that fails everything proves nothing either.
    """
    print("[self-test] injecting binary/manifest provenance regressions ...")
    coupling = verify_source_coupling()
    if coupling:
        for c in coupling:
            print(f"[self-test] FAIL (source coupling): {c}", file=sys.stderr)
        return 2
    print("[self-test]   ok  extractor literals still match the emitting printf")
    ok = True

    def _c(label: str, cond: bool, detail: str = "") -> None:
        nonlocal ok
        if cond:
            print(f"[self-test]   ok  {label}")
        else:
            ok = False
            print(f"[self-test] FAIL: {label}"
                  + (f" -- {detail}" if detail else ""), file=sys.stderr)

    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        binp = root / "engine_under_test.bin"
        binp.write_bytes(_synthetic_binary(
            "79360fdb565e", "claude/spotlight-direction-1",
            "2026-07-29T13:52:25", "A:/Games/mc2-spotdir1", dirty=False))
        true_sha256 = _sha256_file(binp)

        # Sanity: the extractor must actually read the synthetic stamp,
        # otherwise every "detection" below is a false positive.
        fp = ef.extract(str(binp))
        if not (fp.get("present") and fp["sha"] == "79360fdb565e"
                and fp["dirty"] is False):
            print(f"[self-test] FAIL: extractor could not read the injected "
                  f"stamp: {fp}", file=sys.stderr)
            return 2
        dirtyp = root / "engine_dirty.bin"
        dirtyp.write_bytes(_synthetic_binary(
            "79360fdb565e", "b", "2026-07-29T13:52:25", "A:/x", dirty=True))
        if ef.extract(str(dirtyp)).get("dirty") is not True:
            print("[self-test] FAIL: dirty marker not detected", file=sys.stderr)
            return 2

        def ident(**kw):
            base = {
                "schema": ms.SCHEMA, "generator": "selftest",
                "generated_utc": "2026-07-30T00:00:00Z", "host": "t",
                "git": {"commit": "3ab322a0cadd0000000000000000000000000000",
                        "commit_short": "3ab322a0cadd",
                        "branch": "claude/nifty-mendeleev", "dirty": False,
                        "describe": "x", "describes_binary": False},
                "exe": {"path": str(binp), "sha256": true_sha256,
                        "mtime_iso": "2026-07-29T18:52:40Z", "size_bytes": 1},
                "deploy_target": None, "env_delta": {}, "zip_set": None,
            }
            base.update(kw)
            base["source_worktree_head_at_capture"] = base["git"]
            return base

        cases: list[tuple[str, Path, str | None]] = []

        # 1. THE defect: legacy manifest naming a commit the binary is not built from.
        p1 = root / "c1" / "mc2_03_capture_manifest.json"
        _write(p1, {"kind": "visual-capture", "identity": ident()})
        cases.append(("misattributed legacy manifest", p1, "MISATTRIBUTED"))

        # 2. Manifest that RECORDS a fingerprint contradicting the binary.
        p2 = root / "c2" / "mc2_03_capture_manifest.json"
        i2 = ident()
        i2["exe"]["embedded_fingerprint"] = {
            "present": True, "sha": "3ab322a0cadd", "dirty": False,
            "branch": "claude/nifty-mendeleev", "built": "x", "src": "y",
            "method": "pe-string-scan/v1", "exe_sha256_at_scan": true_sha256}
        i2["binary_provenance"] = {"commit": "3ab322a0cadd", "available": True}
        _write(p2, {"kind": "visual-capture", "identity": i2})
        cases.append(("fingerprint contradicted by binary", p2,
                      "FINGERPRINT-CONTRADICTED"))

        # 3. No exe sha256 at all (the cap_trees / visual-verify-capture shape).
        p3 = root / "c3" / "capture_run.json"
        _write(p3, {"kind": "visual-verify-capture", "schema_v": 1,
                    "exe": str(binp), "exe_mtime": "2026-07-29T18:52:40Z",
                    "poses": []})
        cases.append(("exe recorded with no sha256", p3, "NO-EXE-SHA256"))

        # 4. Zero-byte manifest.
        p4 = root / "c4" / "mc2_03_capture_manifest.json"
        p4.parent.mkdir(parents=True, exist_ok=True)
        p4.write_bytes(b"")
        cases.append(("zero-byte manifest", p4, "EMPTY-MANIFEST"))

        # 5. Correct manifest -- must PASS (a checker that fails everything
        #    detects nothing).
        p5 = root / "c5" / "mc2_03_capture_manifest.json"
        i5 = ident()
        i5["exe"]["embedded_fingerprint"] = dict(fp)
        i5["binary_provenance"] = ms.binary_provenance(i5["exe"], i5["git"])
        _write(p5, {"kind": "visual-capture", "identity": i5})
        cases.append(("honest manifest", p5, None))

        # 6. THE COMMENT TRAP: every keyword the checker looks for, present only
        #    as free text. Must still fail -- structure and bytes are checked,
        #    not identifiers.
        p6 = root / "c6" / "mc2_03_capture_manifest.json"
        i6 = ident()
        i6["_comment"] = ("binary_provenance embedded_fingerprint "
                          "exe_sha256_at_scan 79360fdb565e verified reconciled "
                          "commit_source=exe-embedded-build-fingerprint")
        _write(p6, {"kind": "visual-capture", "identity": i6,
                    "_note": "binary_provenance: 79360fdb565e"})
        cases.append(("keywords only in a comment", p6, "MISATTRIBUTED"))

        for label, p, want in cases:
            fs = check_manifest(p, strict_unverifiable=False)
            codes = {f.code for f in fs if f.level == "FAIL"}
            if want is None:
                if codes:
                    ok = False
                    print(f"[self-test] FAIL: {label}: expected clean, got "
                          f"{sorted(codes)}", file=sys.stderr)
                else:
                    print(f"[self-test]   ok  {label}: clean")
            elif want in codes:
                print(f"[self-test]   ok  {label}: detected {want}")
            else:
                ok = False
                print(f"[self-test] FAIL: {label}: expected {want}, got "
                      f"{sorted(codes) or 'nothing'}", file=sys.stderr)

        # 7. --write must convert case 1 from MISATTRIBUTED to clean, and must
        #    record the TRUE commit (not the manifest's claimed one).
        outcome = rewrite_manifest(p1)
        after = json.loads(p1.read_text(encoding="utf-8"))
        bp = after["identity"]["binary_provenance"]
        left = {f.code for f in check_manifest(p1, False) if f.level == "FAIL"}
        if outcome != "backfilled" or bp.get("commit") != "79360fdb565e" \
                or bp.get("verdict") != "SOURCE-HEAD-MISMATCH" or left:
            ok = False
            print(f"[self-test] FAIL: --write backfill wrong "
                  f"(outcome={outcome} commit={bp.get('commit')} "
                  f"verdict={bp.get('verdict')} residual={sorted(left)})",
                  file=sys.stderr)
        else:
            print("[self-test]   ok  --write records the binary's true commit "
                  "and flags SOURCE-HEAD-MISMATCH")
        if p1.read_bytes().count(b"\r\n"):
            ok = False
            print("[self-test] FAIL: --write emitted CRLF", file=sys.stderr)

        # ---- GOLDEN species -----------------------------------------------
        # 9. The committed-golden shape: exe named by path + mtime. This is a
        #    byte-for-byte copy of what tests/visual/goldens/v0.5.1-baseline
        #    records, and the artifact class this checker could not see at all
        #    until golden.json entered MANIFEST_NAMES *and* tests/visual/goldens
        #    entered DEFAULT_ROOTS.
        p9 = root / "g9" / "golden.json"
        _write(p9, {"kind": "visual-verify-golden", "schema_v": 1,
                    "golden_id": "v0.5.1-baseline",
                    "blessed_at": "2026-07-24T01:28:41Z",
                    "blessed_commit": None,
                    "exe": r"A:\Games\mc2-opengl\releases\mc2-win64-v0.5.1\mc2.exe",
                    "exe_mtime": "2026-07-23T04:20:42Z",
                    "members": []})
        cases9 = check_manifest(p9, strict_unverifiable=False)
        c9 = {f.code for f in cases9 if f.level == "FAIL"}
        if "GOLDEN-UNATTRIBUTABLE" in c9:
            print("[self-test]   ok  golden naming its exe by path+mtime: "
                  "detected GOLDEN-UNATTRIBUTABLE")
        else:
            ok = False
            print(f"[self-test] FAIL: legacy golden not caught (got "
                  f"{sorted(c9) or 'nothing'})", file=sys.stderr)
        if "SOURCE-HEAD-AS-PROVENANCE" in c9:
            ok = False
            print("[self-test] FAIL: a NULL blessed_commit asserts nothing and "
                  "must not be double-reported", file=sys.stderr)

        # 10. An otherwise-perfect golden that re-introduces the defect: a
        #     NON-NULL top-level blessed_commit, which is literally what the
        #     bless.py used to write
        #     (`golden["blessed_commit"] = _git(ROOT, "rev-parse", "HEAD")`).
        good_exe = {"path": str(binp), "sha256": true_sha256,
                    "embedded_fingerprint": dict(fp)}
        g_ident = {"schema": ms.SCHEMA, "generator": "visual_verify_bless",
                   "generated_utc": "2026-07-30T00:00:00Z", "host": "t",
                   "git": {"commit": "3ab322a0cadd", "commit_short": "3ab322a0cadd",
                           "branch": "b", "dirty": False, "describe": "x",
                           "describes_binary": False},
                   "source_worktree_head_at_capture": {
                       "commit": "3ab322a0cadd", "commit_short": "3ab322a0cadd",
                       "branch": "b", "dirty": False, "describe": "x",
                       "describes_binary": False},
                   "exe": good_exe, "deploy_target": None, "env_delta": {},
                   "zip_set": None}
        g_ident["binary_provenance"] = ms.binary_provenance(good_exe,
                                                            g_ident["git"])
        p10 = root / "g10" / "golden.json"
        _write(p10, {"kind": "visual-verify-golden", "schema_v": 3,
                     "identity": g_ident, "members": [],
                     "blessed_commit": "3ab322a0cadd0000000000000000000000000000"})
        c10 = {f.code for f in check_manifest(p10, False) if f.level == "FAIL"}
        if "SOURCE-HEAD-AS-PROVENANCE" in c10:
            print("[self-test]   ok  golden re-introducing blessed_commit: "
                  "detected SOURCE-HEAD-AS-PROVENANCE")
        else:
            ok = False
            print(f"[self-test] FAIL: a golden stamping the blesser's HEAD at "
                  f"the top level was NOT caught (got {sorted(c10) or 'nothing'})",
                  file=sys.stderr)

        # 11. The same golden WITHOUT that field must be clean -- a rule that
        #     fails every golden detects nothing.
        p11 = root / "g11" / "golden.json"
        _write(p11, {"kind": "visual-verify-golden", "schema_v": 3,
                     "identity": g_ident, "members": [],
                     "source_worktree_head_at_bless": {
                         "commit": "3ab322a0cadd", "describes_binary": False}})
        c11 = {f.code for f in check_manifest(p11, False) if f.level == "FAIL"}
        if not c11:
            print("[self-test]   ok  an honest golden passes")
        else:
            ok = False
            print(f"[self-test] FAIL: honest golden rejected ({sorted(c11)})",
                  file=sys.stderr)

        # 12. COMMENT TRAP for the golden species.
        p12 = root / "g12" / "golden.json"
        _write(p12, {"kind": "visual-verify-golden", "schema_v": 1,
                     "exe": r"A:\x\mc2.exe", "exe_mtime": "2026-01-01T00:00:00Z",
                     "_note": ("identity binary_provenance embedded_fingerprint "
                               "exe_sha256_at_scan VERIFIED-LIVE present true "
                               "79360fdb565e"), "members": []})
        c12 = {f.code for f in check_manifest(p12, False) if f.level == "FAIL"}
        if "GOLDEN-UNATTRIBUTABLE" in c12:
            print("[self-test]   ok  golden with keywords only in a comment "
                  "still fails")
        else:
            ok = False
            print("[self-test] FAIL: comment-only keywords satisfied the golden "
                  "rule", file=sys.stderr)

        # 13. --write must REFUSE to back-stamp a golden. Mass-stamping tracked
        #     goldens with whatever binary sits at that path today is the
        #     defect, not the fix.
        before = p9.read_bytes()
        outcome13 = rewrite_manifest(p9)
        if outcome13 == "skipped-golden-not-backstampable" and p9.read_bytes() == before:
            print("[self-test]   ok  --write refuses to back-stamp a golden")
        else:
            ok = False
            print(f"[self-test] FAIL: --write touched a golden "
                  f"(outcome={outcome13}, changed={p9.read_bytes() != before})",
                  file=sys.stderr)

        # ---- KNOWN-FAILURE LEDGER ----------------------------------------
        # The ledger must not be usable as a suppression mechanism. Each case
        # below is a way somebody would try, and each must stay RED.
        empty_f = Finding(p4, "FAIL", "EMPTY-MANIFEST", "zero bytes")
        golden_f = Finding(p9, "FAIL", "GOLDEN-UNATTRIBUTABLE", "path+mtime")
        today = datetime.date(2026, 7, 30)

        def led(**kw):
            base = {"id": empty_f.ident(), "why_irreparable": "zero bytes",
                    "expires_utc": "2026-10-31"}
            base.update(kw)
            return base

        k, u, pr = reconcile_ledger([empty_f], [led()], today)
        _c("ledger pins the irreparable finding -> KNOWN, no problems",
           len(k) == 1 and not u and not pr)
        k, u, pr = reconcile_ledger([empty_f, golden_f], [led()], today)
        _c("an UNLEDGERED fail stays RED", len(u) == 1 and not pr)
        k, u, pr = reconcile_ledger([empty_f, golden_f],
                                    [led(), led(id=golden_f.ident())], today)
        _c("ledgering a REPAIRABLE finding is itself a failure",
           any("HAS a remedy" in x for x in pr) and golden_f in u,
           f"problems={pr}")
        k, u, pr = reconcile_ledger([], [led()], today)
        _c("a ledger entry matching NOTHING is a failure (stale, or blind "
           "instrument)", any("matches NO current finding" in x for x in pr))
        # ...but a SUBSET scan must not be told its ledger is stale merely
        # because it did not look there. Narrowing roots is not a suppression
        # path: it drops the FINDING too, and it is a tracked diff in
        # check-contracts.sh, which runs the full default scan.
        k, u, pr = reconcile_ledger([], [led()], today,
                                    roots=[ROOT / "tests" / "visual"])
        _c("an out-of-scope entry is not evaluated on a subset scan", not pr,
           f"problems={pr}")
        k, u, pr = reconcile_ledger([empty_f], [led()], today, roots=[root])
        _c("an in-scope entry IS evaluated on a subset scan",
           len(k) == 1 and not pr)
        k, u, pr = reconcile_ledger([empty_f], [led(expires_utc="2026-01-01")],
                                    today)
        _c("an EXPIRED acknowledgement is a failure",
           any("EXPIRED" in x for x in pr))
        k, u, pr = reconcile_ledger([empty_f], [led(expires_utc=None)], today)
        _c("an acknowledgement with no expiry is refused",
           any("expires_utc" in x for x in pr))
        # A MISSING expiry was refused as "a
        # permanent suppression" while a year-2999 expiry sailed through -- the
        # same suppression, spelled so it parses. A KNOWN row never touches the
        # exit code, so this was the one way the ledger could quietly outlive
        # review.
        k, u, pr = reconcile_ledger([empty_f], [led(expires_utc="2999-12-31")],
                                    today)
        _c("an acknowledgement expiring beyond the cap is refused as a "
           "PERMANENT SUPPRESSION",
           any("PERMANENT SUPPRESSION" in x for x in pr) and len(k) == 0)
        # ...and the boundary still works, so the cap is not "refuse everything".
        just_inside = (today + datetime.timedelta(days=MAX_ACK_DAYS - 1)).isoformat()
        k, u, pr = reconcile_ledger([empty_f], [led(expires_utc=just_inside)],
                                    today)
        _c(f"an acknowledgement {MAX_ACK_DAYS - 1} days out is still accepted",
           len(k) == 1 and not pr, f"problems={pr}")
        k, u, pr = reconcile_ledger(
            [empty_f], [{"id": "EMPTY-MANIFEST:*", "why_irreparable": "x",
                         "expires_utc": "2026-10-31"}], today)
        _c("a GLOB id pins nothing and is refused",
           len(u) == 1 and any("matches NO current finding" in x for x in pr))
        _, pr_bad = load_ledger(root / "nope" / "not-json.json")
        (root / "badledger.json").write_text("{not json", encoding="utf-8")
        _, pr_bad = load_ledger(root / "badledger.json")
        _c("an UNREADABLE ledger is a problem, not an empty ledger",
           bool(pr_bad))

        # 8. A replaced binary must become UNVERIFIABLE, never silently "fine".
        binp.write_bytes(_synthetic_binary("deadbeef1234", "other",
                                           "2026-07-30T00:00:00", "A:/z", False))
        fs = check_manifest(p5, strict_unverifiable=False)
        if not any(f.code == "UNVERIFIABLE-EXE-REPLACED" for f in fs):
            ok = False
            print("[self-test] FAIL: replaced binary not reported unverifiable",
                  file=sys.stderr)
        else:
            print("[self-test]   ok  replaced binary reported unverifiable")

    print("[self-test] PASS" if ok else "[self-test] FAILED")
    return 0 if ok else 2


# --------------------------------------------------------------------------- #

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("roots", nargs="*",
                    help=f"dirs/files to scan (default: {', '.join(DEFAULT_ROOTS)})")
    ap.add_argument("--write", action="store_true",
                    help="backfill binary provenance where it is still "
                         "recoverable, and mark the rest explicitly unknowable")
    ap.add_argument("--strict-unverifiable", action="store_true",
                    help="promote 'exe gone/replaced' from WARN to FAIL")
    ap.add_argument("--selftest", action="store_true",
                    help="inject the regression and prove this checker fires")
    ap.add_argument("--ledger", default=None,
                    help=f"known-correct-failure ledger (default "
                         f"{LEDGER_PATH.name}). NOT a suppression list: it is "
                         f"consumed exactly, an entry that matches no finding "
                         f"is itself a failure, and only irreparable codes "
                         f"({', '.join(IRREPARABLE_CODES)}) may be pinned.")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    if args.selftest:
        return _selftest()

    # A blind instrument is worse than none: if the emitting printf drifted,
    # every fingerprint read comes back absent and this checker would find
    # nothing wrong with anything. Refuse to report at all in that state.
    coupling = verify_source_coupling()
    if coupling:
        for c in coupling:
            print(f"[provenance] INSTRUMENT BROKEN: {c}", file=sys.stderr)
        print("[provenance] refusing to report -- fix scripts/exe_fingerprint.py "
              "to match the emitter, then re-run.", file=sys.stderr)
        return 2

    roots = [Path(r) for r in (args.roots or [ROOT / r for r in DEFAULT_ROOTS])]
    # None == the full default scan: every ledger entry is in scope, and the
    # blind-instrument assertion (an entry that matches nothing is a failure)
    # applies in full. That is the mode check-contracts.sh runs.
    ledger_scope = None if not args.roots else roots
    manifests = [m for r in roots if r.exists() for m in iter_manifests(r)]

    if args.write:
        counts: dict[str, int] = {}
        for m in manifests:
            o = rewrite_manifest(m)
            counts[o] = counts.get(o, 0) + 1
            if not args.quiet and o in ("backfilled", "unknowable"):
                print(f"[provenance] {o:16} {m}")
        print(f"\n[provenance] --write over {len(manifests)} manifest(s): "
              + ", ".join(f"{k}={v}" for k, v in sorted(counts.items())))
        return 0

    findings: list[Finding] = []
    for m in manifests:
        findings.extend(check_manifest(m, args.strict_unverifiable))

    fails = [f for f in findings if f.level == "FAIL"]
    warns = [f for f in findings if f.level == "WARN"]
    if not args.quiet:
        for f in findings:
            print(str(f))

    entries, ledger_problems = load_ledger(args.ledger)
    known, unledgered, more = reconcile_ledger(fails, entries,
                                               roots=ledger_scope)
    ledger_problems += more

    print(f"\n[provenance] scanned={len(manifests)} FAIL={len(fails)} "
          f"(unledgered={len(unledgered)} known={len(known)}) "
          f"WARN={len(warns)}  roots={[str(r) for r in roots]}")

    for f in known:
        entry = next((e for e in entries if e.get("id") == f.ident()), {})
        print(f"[provenance] KNOWN  {f.ident()}\n"
              f"             correct+irreparable: {entry.get('why_irreparable')}\n"
              f"             remedy (a USER decision): {entry.get('remedy')}\n"
              f"             acknowledgement expires {entry.get('expires_utc')}")

    if ledger_problems:
        for p in ledger_problems:
            print(f"[provenance] LEDGER: {p}", file=sys.stderr)
        print(f"[provenance] the known-failure ledger ({LEDGER_PATH}) does not "
              f"reconcile with what was actually found. It is consumed exactly "
              f"in BOTH directions on purpose: an entry that matches nothing "
              f"means either the condition is gone or this checker went blind, "
              f"and neither may pass silently.", file=sys.stderr)
        return 1

    if unledgered:
        print(f"[provenance] {len(unledgered)} manifest(s) cannot state which "
              f"binary they measured. A capture whose commit is unknown cannot "
              f"prove a fix; re-emit via manifest_schema.identity_block, run "
              f"--write where the binary is still on disk, or re-bless the "
              f"golden. ACTION REQUIRED.", file=sys.stderr)
        return 1

    if known:
        print(f"[provenance] KNOWN-CORRECT-FAILURE: {len(known)} finding(s), all "
              f"pinned in {LEDGER_PATH.name}, all irreparable, none expired. "
              f"This is NOT a pass -- the artifacts remain unattributable and "
              f"the remedy is a USER decision. It is reported separately so "
              f"that a RED row still means 'act now'.")
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
