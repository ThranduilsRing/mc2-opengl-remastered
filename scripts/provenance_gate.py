#!/usr/bin/env python3
"""GOLDEN-PROVENANCE-TRUTH-1 -- can this artifact prove WHICH BINARY produced it?

WHY THIS EXISTS
---------------
CAPTURE-PROVENANCE-TRUTH-1 built an excellent forensic pathologist:
scripts/check-capture-provenance.py scans the evidence trees after the fact and
refuses manifests that cannot name the binary they measured. The emergency room
was left unguarded. At the two moments that actually MINT authority --

    * tools/visual_verify/bless.py   mints a GOLDEN (the reference every later
                                     release is graded against), and
    * tools/visual_verify/verify.py  mints a VERDICT against that golden,

-- nothing asked the question at all. bless.py recorded the exe by PATH+MTIME
and stamped `blessed_commit` from `git rev-parse HEAD` in the BLESSER's
worktree, re-laundering the exact source-HEAD-as-provenance defect back into the
most authoritative artifact class in the repo. verify.py graded with zero
provenance checks of any kind.

MEASURED, 2026-07-30, on the three committed goldens:
  * v0.5.1-baseline and v0.5.2-baseline-PROPOSED record the SAME exe path with
    TWO DIFFERENT mtimes. The file at that path today has the mtime of the
    second one. The binary that minted v0.5.1-baseline was overwritten in place
    and was never identified by content: it is gone and unrecoverable.
  * baseline-5ef261f4 carries a HAND-ADDED identity block claiming
    exe_sha256=99a12c41... . The file at that path today is 4b195092..., built
    from a:871061e8682 on branch claude/readback-verbs-1 -- a different lane's
    build, sitting exactly where the golden points.
  * 0 of 3 could prove which binary minted them.

PROVENANCE-FORGERY-CLOSE-1 -- THE HOLE THIS MODULE SHIPPED WITH
---------------------------------------------------------------
The paragraph above ("a hand-added identity block is not accepted") was FALSE
for the entire life of af4f79c8, in the very module written to stop forgeries.

A 16-line hand-typed golden.json naming `Z:/no/such/dir/never_existed_vtu1.exe`
-- a binary that has never existed on any machine -- graded
`[OK ] RECORDED-AT-MINT commit=d15ea5edbeef`, exit 0. Two reasons, both here:

  * `establish()` returned ok=True whenever the recorded exe path was simply
    ABSENT FROM DISK. "I cannot check this" was spelled the same as "I checked
    this". Absence of contradiction was read as corroboration.
  * NOTHING outside the file was ever consulted. Self-consistency means
    `embedded_fingerprint.exe_sha256_at_scan == exe.sha256` -- two fields in one
    JSON object, both typed by the same hand in the same keystroke. That is not
    a cross-check; it is the file agreeing with itself.

The original self-test passed while blind because its "hand-added" case OMITTED
`exe_sha256_at_scan` -- the one field a forger must type. It proved that a
careless forgery fails, and called that proof against a careful one.

THE RULE
--------
Provenance is ESTABLISHED only when the artifact names its binary by CONTENT,
records that binary's OWN embedded build fingerprint read from those same bytes,
AND something OUTSIDE the artifact agrees. Four levels; only the first two may
be graded against:

  VERIFIED-LIVE         recorded sha256 + embedded fingerprint, the binary is
                        still on disk byte-identical, and a live re-read of its
                        stamp agrees. The strongest statement available, and the
                        only one that is proof rather than corroboration.
  CORROBORATED-AT-MINT  the binary is gone or replaced, so it cannot be re-read
                        -- but at least one INDEPENDENT WITNESS, outside this
                        file, agrees with what the record claims. Gradeable.
  UNCORROBORATED        internally self-consistent and nothing more. Every field
                        in it is hand-typeable and no witness anywhere agrees.
                        REFUSE TO GRADE. This is the forged-golden level.
  UNESTABLISHED         malformed, incomplete or CONTRADICTED. REFUSE TO GRADE.

WITNESSES -- what "outside this file" means
-------------------------------------------
A witness must be checkable by this module without trusting the artifact's own
prose. Any ONE suffices; every witness found is listed in the result.

  W-REPO-COMMIT     the commit named by the embedded stamp resolves as a real
                    commit object in this repository (`git cat-file -e
                    <sha>^{commit}`). All lane worktrees share one object
                    database (`git rev-parse --git-common-dir`), so a binary
                    built in ANY lane corroborates here -- and it keeps
                    corroborating long after that lane's install is deleted,
                    which is the property a golden needs.
  W-ARTIFACT        another JSON on disk, NAMED BY THIS RECORD and pinned by its
                    own sha256, that independently records the SAME exe digest.
                    Tamper-evident: edit the witness to agree and its digest
                    stops matching, so it stops counting.

WHY "ANY ONE", NOT "ALL"
------------------------
Because a gate that refuses all history gets deleted within a day, and then
protects nothing. Goldens legitimately outlive the binaries that minted them:
installs are overwritten every build, scratch capture trees are pruned, lanes
are deleted. Requiring live bytes would refuse every golden older than its
install -- i.e. all of them, eventually. W-REPO-COMMIT is the arm that survives
that, because git objects are not deleted when an install is.

HONEST LIMITATION -- state it rather than imply more
----------------------------------------------------
Neither witness is proof, and since ANY ONE suffices, each must be read at its
own weakest. State that plainly rather than let "corroborated" imply more:

  * W-REPO-COMMIT proves the commit is REAL. It does not prove THIS BINARY was
    built from it. A forger who copies a genuine sha out of `git log` satisfies
    it on its own.
  * W-ARTIFACT proves a SECOND file on disk already carries this digest. It does
    not prove either file was tool-emitted. A forger who writes two consistent
    files satisfies it on its own.

Nothing here closes those, and nothing can without a signature or an external
append-only store, neither of which exists. What changed is the cost of a
forgery: from typing twelve hex characters into one file, to producing something
that survives contact with either the object database or a second artifact.
Only VERIFIED-LIVE is proof. The level names say so, and a reader who needs
proof should require that LEVEL rather than the `ok` flag, which deliberately
spans both gradeable levels.

WHAT IS DELIBERATELY *NOT* ACCEPTED
-----------------------------------
  * path + mtime. An mtime is not an identity; overwrite the file and the
    measurement is unattributable, which is precisely what happened above.
  * a self-consistent record with no witness. This is the forgery the module
    shipped blind to, and it is now its own named level (UNCORROBORATED) so a
    reader can tell "unwitnessed" apart from "malformed".
  * a malformed digest or commit. `exe.sha256` must be exactly 64 hex
    characters and the stamped commit 7..40; the forged golden that started
    this slice carried a SIXTY-FIVE character "sha256" and nothing noticed,
    because nothing looked.
  * the source worktree HEAD, under any spelling. `blessed_commit`,
    `identity.git.commit`, `source_worktree_head_*` -- none of them describe the
    binary. A multi-lane worktree setup makes them routinely a DIFFERENT commit
    from the one compiled in.
  * the generated header. cmake/GenerateBuildFingerprint.cmake stamps
    generated/build_fingerprint.h; a failed or skipped compile leaves the header
    fresh and the exe stale (c5d934d5). Truth is read out of the binary's bytes.

There is NO override flag. An escape hatch on a provenance gate is the gate.
When a golden cannot be established the remedy is to re-bless it from an
attributable capture, or to grade without a golden (verify.py --run, no
--golden, still emits every crop for eyeball comparison).

Exit: 0 all supplied artifacts establish | 1 at least one does not | 2 selftest.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))
import exe_fingerprint as ef  # noqa: E402
import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

ROOT = _HERE.parent

#: Levels that may be graded against, strongest first. UNCORROBORATED is
#: deliberately absent: a record that only agrees with itself is the forgery
#: shape this module exists to refuse.
GRADEABLE = ("VERIFIED-LIVE", "CORROBORATED-AT-MINT")

#: A sha256 is exactly 64 hex characters. A 65-character value can still
#: compare equal to itself, so the length is independently enforced.
_HEX64 = re.compile(r"^[0-9a-f]{64}$")
#: An abbreviated-or-full git commit id.
_HEXCOMMIT = re.compile(r"^[0-9a-f]{7,40}$")


class ProvenanceUnestablished(Exception):
    """Raised at a decision point when an input cannot name its binary."""


def _sha256_file(p: Path) -> "str | None":
    try:
        h = hashlib.sha256()
        with open(p, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        return h.hexdigest()
    except Exception:
        return None


#: Memo for repo_has_commit. Keyed by (repo_root, sha).
_COMMIT_CACHE: "dict[tuple[str, str], bool]" = {}
_GITDIR_CACHE: "dict[str, bool]" = {}


def git_object_db_available(repo_root: "str | Path" = None) -> bool:
    """Is there a git object database to ask at all?

    Kept separate from `repo_has_commit` so a refusal can say which of the two
    very different things happened. "This commit does not exist" and "I had no
    way to look" must not print the same sentence -- collapsing those two is the
    same mistake as grading an absent binary OK, one layer up.
    """
    root = str(repo_root or ROOT)
    if root in _GITDIR_CACHE:
        return _GITDIR_CACHE[root]
    try:
        r = subprocess.run(["git", "-C", root, "rev-parse", "--git-dir"],
                           capture_output=True, timeout=30, **console_child.child_kwargs(redirected=True))
        avail = (r.returncode == 0)
    except Exception:  # noqa: BLE001 -- git missing entirely
        avail = False
    _GITDIR_CACHE[root] = avail
    return avail


def repo_has_commit(sha: "str | None", repo_root: "str | Path" = None) -> bool:
    """W-REPO-COMMIT: does `sha` resolve to a real commit OBJECT in this repo?

    This is the witness that survives everything else. Installs are overwritten
    on every build and scratch capture trees are pruned, but a commit object is
    not deleted when an install is -- and every lane worktree shares ONE object
    database (`git rev-parse --git-common-dir`), so a binary built in another
    lane still corroborates from here.

    It proves the commit is REAL, not that this binary was built from it. See
    the module docstring's HONEST LIMITATION section; do not let a caller read
    more into it than that.
    """
    if not sha or not _HEXCOMMIT.match(sha):
        return False
    root = str(repo_root or ROOT)
    key = (root, sha)
    if key in _COMMIT_CACHE:
        return _COMMIT_CACHE[key]
    try:
        r = subprocess.run(["git", "-C", root, "cat-file", "-e", f"{sha}^{{commit}}"],
                           capture_output=True, timeout=30, **console_child.child_kwargs(redirected=True))
        found = (r.returncode == 0)
    except Exception:  # noqa: BLE001 -- no git, not a repo: no witness, not a lie
        found = False
    _COMMIT_CACHE[key] = found
    return found


def _witness_records(doc: "dict[str, Any]") -> "list[dict[str, Any]]":
    """The corroborating artifacts this record NAMES, if any.

    Emitted by the minting tool (bless.py) under
    `identity.binary_corroboration.witnesses`; each entry pins one other file by
    its OWN sha256 so that editing the witness to agree stops it counting.
    """
    ident = doc.get("identity")
    if not isinstance(ident, dict):
        return []
    bc = ident.get("binary_corroboration")
    if not isinstance(bc, dict):
        return []
    ws = bc.get("witnesses")
    return [w for w in ws if isinstance(w, dict)] if isinstance(ws, list) else []


def _exe_sha_of(doc: "dict[str, Any]") -> "str | None":
    blk = _exe_block(doc)
    return blk.get("sha256") if isinstance(blk, dict) else None


def check_witness_artifacts(doc: "dict[str, Any]", sha: str, self_path: "Path | None"
                            ) -> "tuple[list[str], str | None]":
    """(usable witness descriptions, contradiction).

    A witness counts only if it is a DIFFERENT file that still exists, whose own
    digest matches what this record pinned, and whose independently-recorded exe
    digest is the same one. A witness that exists, verifies, and names a
    DIFFERENT binary is not a missing witness -- it is a contradiction, and the
    record must not grade at all.
    """
    good: "list[str]" = []
    for w in _witness_records(doc):
        wp_raw = w.get("path")
        if not isinstance(wp_raw, str) or not wp_raw:
            continue
        wp = Path(wp_raw)
        if self_path is not None:
            try:
                if wp.resolve() == self_path.resolve():
                    continue  # a file is never its own witness
            except Exception:  # noqa: BLE001
                pass
        if not wp.exists():
            continue
        pinned = w.get("artifact_sha256")
        actual = _sha256_file(wp)
        if not pinned or actual != pinned:
            continue  # edited or unreadable since mint: no longer tamper-evident
        try:
            wdoc = json.loads(wp.read_text(encoding="utf-8"))
        except Exception:  # noqa: BLE001
            continue
        if not isinstance(wdoc, dict):
            continue
        wsha = _exe_sha_of(wdoc)
        if not wsha:
            continue
        if wsha != sha:
            return good, (f"names witness {wp} (digest pinned and matching), but "
                          f"that artifact independently records exe "
                          f"{wsha[:12]} while this record claims {sha[:12]} -- "
                          f"the witness it chose CONTRADICTS it.")
        good.append(f"W-ARTIFACT:{wp.name} (pinned sha matches, records the "
                    f"same exe digest)")
    return good, None


def _exe_block(doc: "dict[str, Any]") -> "dict[str, Any] | None":
    """Locate the tool-emitted exe block across the species that carry one.

    Accepted shapes, in order:
      identity.exe            unified manifest_schema block (capture_run.json,
                              <mission>_capture_manifest.json, golden.json v3)
      binary_identity         the same block promoted to the top level (the
                              golden-parity combined manifest, which is
                              synthesized rather than emitted by
                              manifest_schema)

    A bare top-level `exe` STRING is deliberately NOT accepted: that is the
    path+mtime shape this module exists to reject.
    """
    ident = doc.get("identity")
    if isinstance(ident, dict) and isinstance(ident.get("exe"), dict):
        return ident["exe"]
    bi = doc.get("binary_identity")
    if isinstance(bi, dict) and isinstance(bi.get("exe"), dict):
        return bi["exe"]
    if isinstance(bi, dict) and "sha256" in bi:
        return bi
    return None


def establish(doc: "dict[str, Any]", label: str = "artifact",
              self_path: "Path | None" = None) -> "dict[str, Any]":
    """Answer 'can this artifact prove which binary produced it?'.

    Returns a dict that is always shaped the same:
        {label, level, ok, commit, dirty, branch, exe_sha256, exe_path,
         witnesses, why}
    `ok` is True only for a level in GRADEABLE. `why` always states, in one
    sentence a human can act on, what is missing or what was proven.

    `self_path` is this document's own file, when known, so that a record
    cannot be offered as its own witness.
    """
    out = {"label": label, "level": "UNESTABLISHED", "ok": False,
           "commit": None, "dirty": None, "branch": None,
           "exe_sha256": None, "exe_path": None, "witnesses": [], "why": ""}

    blk = _exe_block(doc)
    if blk is None:
        legacy = doc.get("exe")
        if isinstance(legacy, str):
            out["exe_path"] = legacy
            out["why"] = (
                f"names its binary by PATH ONLY ({legacy!r}"
                + (f", mtime {doc.get('exe_mtime')!r}" if doc.get("exe_mtime")
                   else "")
                + "). A path plus an mtime is not an identity: overwrite that "
                  "file and this artifact becomes permanently unattributable. "
                  "That is not hypothetical -- it already happened to "
                  "tests/visual/goldens/v0.5.1-baseline.")
        else:
            out["why"] = ("carries no exe block at all, so it does not say "
                          "which binary produced it.")
        return out

    sha = blk.get("sha256")
    out["exe_path"] = blk.get("path")
    out["exe_sha256"] = sha
    if not sha:
        out["why"] = ("records an exe block with no sha256 -- the binary is "
                      "not identified by content.")
        return out
    if not isinstance(sha, str) or not _HEX64.match(sha):
        out["why"] = (f"records exe.sha256={sha!r}, which is not a sha256 "
                      f"(want exactly 64 lowercase hex characters, got "
                      f"{len(sha) if isinstance(sha, str) else 'a non-string'}). "
                      f"A digest nothing ever validated is a string, not an "
                      f"identity.")
        return out

    fp = blk.get("embedded_fingerprint")
    if not isinstance(fp, dict):
        out["why"] = ("records no `embedded_fingerprint`: the binary's own "
                      "compiled-in build stamp was never read, so the commit "
                      "this artifact exercised is nowhere in the file.")
        return out
    if not fp.get("present"):
        out["why"] = (f"the binary carried no readable build stamp at mint time "
                      f"(reason={fp.get('reason')!r}); the commit it was built "
                      f"from was never recorded and cannot be recovered.")
        return out

    scanned = fp.get("exe_sha256_at_scan")
    if not scanned:
        out["why"] = ("records a fingerprint with no `exe_sha256_at_scan`, so "
                      "there is no proof the stamp was read from the bytes "
                      "`exe.sha256` names. A hand-added claim is not "
                      "provenance (PROVENANCE-EMIT-1).")
        return out
    if scanned != sha:
        out["why"] = (f"the fingerprint was read from {scanned[:12]} but the "
                      f"exe digest records {sha[:12]} -- the artifact describes "
                      f"two different binaries.")
        return out

    commit = fp.get("sha")
    if not isinstance(commit, str) or not _HEXCOMMIT.match(commit):
        out["why"] = (f"records a build stamp whose commit is {commit!r}, which "
                      f"is not a git object id (want 7..40 hex characters). "
                      f"Nothing that cannot even be a commit can name one.")
        return out
    out["commit"] = commit
    out["dirty"] = fp.get("dirty")
    out["branch"] = fp.get("branch")

    p = Path(blk.get("path") or "")
    live_sha = _sha256_file(p) if p.name and p.exists() else None

    # ---- External corroboration ------------------------------------------ #
    # Everything checked so far lives INSIDE this one JSON object and was typed
    # in one keystroke by whoever wrote it. Before the binary is consulted, ask
    # what outside this file agrees. Without this question, `establish` would
    # answer OK on the strength of the file agreeing with itself.
    witnesses: "list[str]" = []
    no_commit_witness = (
        f"commit {commit} is not a commit object in {ROOT}"
        if git_object_db_available()
        else f"there is no readable git object database at {ROOT}, so commit "
             f"{commit} could not be checked at all")
    art_w, contradiction = check_witness_artifacts(doc, sha, self_path)
    if contradiction:
        out["why"] = contradiction
        return out
    witnesses += art_w
    if repo_has_commit(commit):
        witnesses.append(f"W-REPO-COMMIT:{commit} resolves to a real commit "
                         f"object in {ROOT}")
    out["witnesses"] = witnesses

    if live_sha is None:
        if not witnesses:
            out["level"] = "UNCORROBORATED"
            out["why"] = (
                f"names binary {p}, which is NOT ON DISK, and nothing outside "
                f"this file agrees with it: {no_commit_witness}, and no on-disk "
                f"witness artifact records exe "
                f"{sha[:12]}. Every field here is hand-typeable and only this "
                f"file vouches for it. 'I could not check' is not 'I checked'.")
            return out
        out["level"] = "CORROBORATED-AT-MINT"
        out["ok"] = True
        out["why"] = (f"binary {p} is no longer on disk, so the record cannot "
                      f"be re-verified live; it names commit {commit} and is "
                      f"corroborated from outside the file by "
                      f"{'; '.join(witnesses)}.")
        return out
    if live_sha != sha:
        if not witnesses:
            out["level"] = "UNCORROBORATED"
            out["why"] = (
                f"binary at {p} has been replaced since mint (now "
                f"{live_sha[:12]}, recorded {sha[:12]}), so the record cannot "
                f"be re-verified live -- and nothing outside this file agrees "
                f"with it: {no_commit_witness}, and no on-disk witness "
                f"artifact records exe {sha[:12]}. A "
                f"recorded digest that names neither the bytes on disk nor "
                f"anything else is just a string someone typed.")
            return out
        out["level"] = "CORROBORATED-AT-MINT"
        out["ok"] = True
        out["why"] = (f"binary at {p} has been replaced since mint (now "
                      f"{live_sha[:12]}, recorded {sha[:12]}), so the record "
                      f"cannot be re-verified live; it names commit {commit} "
                      f"and is corroborated from outside the file by "
                      f"{'; '.join(witnesses)}.")
        return out

    live = ef.extract(str(p))
    if not live.get("present"):
        out["why"] = (f"artifact records a fingerprint (commit "
                      f"{out['commit']}) but the byte-identical binary on disk "
                      f"carries none ({live.get('reason')}) -- the recording "
                      f"is contradicted by the bytes it names.")
        return out
    if live.get("sha") != out["commit"]:
        out["why"] = (f"artifact records commit {out['commit']} but the "
                      f"byte-identical binary on disk states {live.get('sha')} "
                      f"-- CONTRADICTED.")
        return out
    if bool(live.get("dirty")) != bool(out["dirty"]):
        out["why"] = (f"artifact records dirty={out['dirty']} but the "
                      f"byte-identical binary on disk states "
                      f"dirty={live.get('dirty')} -- CONTRADICTED.")
        return out

    out["level"] = "VERIFIED-LIVE"
    out["ok"] = True
    out["why"] = (f"binary still on disk byte-identical; live re-read of its "
                  f"own stamp agrees: commit {out['commit']} "
                  f"(branch {out['branch']}, dirty={out['dirty']}).")
    return out


def establish_path(path: "str | Path", label: str = None) -> "dict[str, Any]":
    """`establish` over a JSON file. An unreadable file is UNESTABLISHED, never
    an exception -- the caller's refusal message must name the file either way."""
    p = Path(path)
    lbl = label or p.name
    try:
        doc = json.loads(p.read_text(encoding="utf-8"))
    except Exception as e:  # noqa: BLE001
        return {"label": lbl, "level": "UNESTABLISHED", "ok": False,
                "commit": None, "dirty": None, "branch": None,
                "exe_sha256": None, "exe_path": None, "witnesses": [],
                "why": f"cannot be read as JSON ({type(e).__name__}: {e})."}
    if not isinstance(doc, dict):
        return {"label": lbl, "level": "UNESTABLISHED", "ok": False,
                "commit": None, "dirty": None, "branch": None,
                "exe_sha256": None, "exe_path": None, "witnesses": [],
                "why": "top level is not a JSON object."}
    # `self_path` so a record cannot list ITSELF as its corroborating witness.
    est = establish(doc, lbl, self_path=p)
    est["source"] = str(p)
    return est


def refuse_unless_established(ests: "list[dict[str, Any]]", *,
                              decision: str, remedy: str) -> None:
    """Raise ProvenanceUnestablished naming EVERY input that cannot establish.

    `decision` names the verdict being minted ("grade a candidate against a
    golden"), `remedy` the concrete way out. The message is deliberately long:
    a refusal a reader cannot act on gets worked around.
    """
    bad = [e for e in ests if not e.get("ok")]
    if not bad:
        return
    lines = [f"PROVENANCE UNESTABLISHED -- refusing to {decision}.",
             "",
             "At the moment a verdict is minted, an unattributable input must "
             "not be able to produce a clean-looking output. These inputs "
             "cannot name the binary that produced them:"]
    for e in bad:
        lines.append(f"  * {e['label']}: {e['why']}")
        if e.get("source"):
            lines.append(f"      file: {e['source']}")
    ok = [e for e in ests if e.get("ok")]
    if ok:
        lines.append("")
        lines.append("Established inputs (for contrast):")
        for e in ok:
            lines.append(f"  * {e['label']}: {e['level']} -- commit "
                         f"{e['commit']} ({e['why']})")
    lines += ["", "REMEDY: " + remedy, "",
              "There is no override flag. An escape hatch on a provenance gate "
              "is the gate."]
    raise ProvenanceUnestablished("\n".join(lines))


def same_binary(ests: "list[dict[str, Any]]") -> "tuple[bool, str]":
    """Do these established artifacts all name the SAME binary, by content?

    Used where the comparison's meaning depends on it: an A/B parity run whose
    two arms ran different binaries is not a gate flip, and a three-run bless
    whose runs came from different binaries has an all-pairs 'reproducibility'
    number that measures nothing.
    """
    # A missing digest must never read as agreement. Two artifacts that both
    # failed to record their binary would otherwise collapse to {None} -- one
    # element -- and this function would answer "yes, the same binary", which is
    # the precise shape of lie the whole module exists to remove. Callers are
    # expected to run refuse_unless_established() first; this makes the wrong
    # call order fail loudly instead of agreeing.
    if any(not e.get("exe_sha256") for e in ests):
        missing = ", ".join(e["label"] for e in ests if not e.get("exe_sha256"))
        return False, (f"cannot be compared: {missing} record(s) no exe sha256, "
                       f"and an absent digest is UNKNOWN, never 'the same'")
    shas = {e.get("exe_sha256") for e in ests}
    if len(shas) <= 1:
        return True, ""
    detail = "; ".join(f"{e['label']}={str(e.get('exe_sha256'))[:12]} "
                       f"(commit {e.get('commit')})" for e in ests)
    return False, detail


# --------------------------------------------------------------------------- #
# Self-test: build real fingerprint-carrying binaries and prove each verdict
# --------------------------------------------------------------------------- #

def _synthetic_binary(sha: str, branch: str, built: str, src: str,
                      dirty: bool) -> bytes:
    """A byte image carrying a REAL, parseable build-fingerprint stamp -- the
    same .rdata layout MSVC produces for the printf in code/mechcmd2.cpp."""
    pad = b"\x00filler string one\x00filler string two\x00"
    ops = (src.encode() + b"\x00" + built.encode() + b"\x00"
           + branch.encode() + b"\x00" + sha.encode() + b"\x00")
    blob = b"MZ" + b"\x00" * 64 + pad + ops + ef.BANNER_FMT
    if dirty:
        blob += ef.DIRTY_MARKER + b"\n\x00"
    return blob + b"\x00" * 32


def _absent_commit(seed: int) -> str:
    """A well-formed 12-hex commit id that is NOT an object in this repo.

    The witness arms below must be unambiguous. If the self-test's synthetic
    binary carried a commit that happens to be REAL here (the original carried
    79360fdb565e, which is), W-REPO-COMMIT would fire on every case and the
    'no witness at all' assertions would silently test nothing.
    """
    for i in range(64):
        cand = f"{(seed + i * 0x9E3779B9) & 0xFFFFFFFFFFFF:012x}"
        if not repo_has_commit(cand):
            return cand
    raise RuntimeError("could not find a commit id absent from this repo")


def _selftest() -> int:
    import tempfile
    print("[prov-gate] self-test: proving each verdict on real stamped bytes")
    ok = True

    def chk(label: str, cond: bool, detail: str = "") -> None:
        nonlocal ok
        print(f"  [{'PASS' if cond else 'FAIL'}] {label}"
              + (f" -- {detail}" if detail else ""))
        if not cond:
            ok = False

    # Two commit ids guaranteed absent from this repo, so "corroborated" below
    # can only ever come from a witness this test actually built.
    SYN_A = _absent_commit(0x5E1F7E57)
    SYN_B = _absent_commit(0x1234ABCD)
    print(f"  [ok  ] control: synthetic commits {SYN_A}/{SYN_B} are absent from "
          f"this repo, so W-REPO-COMMIT cannot mask a missing witness")

    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        binp = root / "engine.bin"
        binp.write_bytes(_synthetic_binary(SYN_A,
                                           "claude/spotlight-direction-1",
                                           "2026-07-29T13:52:25",
                                           "A:/Games/mc2-spotdir1", dirty=False))
        true_sha = _sha256_file(binp)
        fp = ef.extract(str(binp))
        if not (fp.get("present") and fp["sha"] == SYN_A):
            print("  [FAIL] extractor could not read the injected stamp -- "
                  "every verdict below would be a false positive", file=sys.stderr)
            return 2
        print("  [ok  ] control: extractor reads the injected stamp")

        def doc(exe_blk):
            return {"kind": "visual-verify-golden", "identity": {"exe": exe_blk}}

        good = {"path": str(binp), "sha256": true_sha,
                "embedded_fingerprint": dict(fp)}

        e = establish(doc(good), "good")
        chk("byte-identical binary on disk -> VERIFIED-LIVE",
            e["level"] == "VERIFIED-LIVE" and e["ok"]
            and e["commit"] == SYN_A, e["level"])

        # THE defect: path + mtime, the exact shape of the committed goldens.
        e = establish({"kind": "visual-verify-golden",
                       "exe": r"A:\x\mc2.exe",
                       "exe_mtime": "2026-07-23T04:20:42Z"}, "pathmtime")
        chk("path+mtime -> UNESTABLISHED (the committed-golden shape)",
            not e["ok"] and "not an identity" in e["why"], e["level"])

        # A sha256 with no stamp read.
        e = establish(doc({"path": str(binp), "sha256": true_sha}), "nofp")
        chk("sha256 but no embedded_fingerprint -> UNESTABLISHED",
            not e["ok"] and "embedded_fingerprint" in e["why"], e["level"])

        # Hand-added claim: fields look right, but no proof of which bytes.
        hand = {"path": str(binp), "sha256": true_sha,
                "embedded_fingerprint": {"present": True, "sha": SYN_A,
                                         "dirty": False, "branch": "b"}}
        e = establish(doc(hand), "handadded")
        chk("hand-added fingerprint (no exe_sha256_at_scan) -> UNESTABLISHED",
            not e["ok"] and "hand-added" in e["why"], e["level"])

        # ------------------------------------------------------------------ #
        # A CAREFUL FORGERY: every field typed.
        #
        # The "handadded" case above OMITS
        # `exe_sha256_at_scan`. That single missing field is the only thing that
        # failed it -- so that test proves only that a CARELESS forgery is
        # caught. A forger's entire obligation here is to type the field.
        # Below it IS typed, which is the actual threat, and the block names a
        # binary that has never existed anywhere.
        # ------------------------------------------------------------------ #
        forged_sha = "ab" * 32
        forged = {"kind": "visual-verify-golden", "identity": {"exe": {
            "path": "Z:/no/such/dir/never_existed_vtu1.exe",
            "sha256": forged_sha,
            "embedded_fingerprint": {
                "present": True, "sha": "d15ea5edbeef", "dirty": False,
                "branch": "claude/totally-made-up", "built": "2026-07-30T00:00:00",
                "src": "Z:/no/such/dir", "method": "pe-string-scan/v1",
                "exe_sha256_at_scan": forged_sha}}}}
        e = establish(forged, "forged")
        chk("FORGED golden, every field typed, binary never existed -> "
            "UNCORROBORATED (refused)",
            not e["ok"] and e["level"] == "UNCORROBORATED"
            and "not a commit object" in e["why"], e["level"])

        # The same forgery with a REAL commit id still gets no artifact witness,
        # but W-REPO-COMMIT does fire -- and the module says so out loud rather
        # than implying proof. This asserts the documented limitation is the
        # ACTUAL behaviour, so the docstring cannot drift back into a promise.
        real_commit = None
        try:
            rc = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"],
                                capture_output=True, text=True, timeout=30, **console_child.child_kwargs(redirected=True))
            if rc.returncode == 0:
                real_commit = rc.stdout.strip()[:12]
        except Exception:  # noqa: BLE001
            pass
        if real_commit:
            forged2 = json.loads(json.dumps(forged))
            forged2["identity"]["exe"]["embedded_fingerprint"]["sha"] = real_commit
            e = establish(forged2, "forged-real-commit")
            chk("documented limitation is REAL: a forgery naming a genuine "
                "commit reaches CORROBORATED-AT-MINT, never VERIFIED-LIVE",
                e["level"] == "CORROBORATED-AT-MINT" and e["ok"]
                and any("W-REPO-COMMIT" in w for w in e["witnesses"]),
                e["level"])

        # "This commit does not exist" and "I had no way to look" must not print
        # the same sentence. Collapsing those two is the same mistake as grading
        # an absent binary OK, one layer up -- so assert the distinction exists
        # rather than trusting that it reads well.
        _GITDIR_CACHE[str(ROOT)] = False
        e_nogit = establish(forged, "forged-no-gitdb")
        _GITDIR_CACHE.pop(str(ROOT), None)
        chk("with no git object database the refusal says 'could not be "
            "checked', not 'does not exist'",
            not e_nogit["ok"] and "could not be checked at all" in e_nogit["why"]
            and "is not a commit object" not in e_nogit["why"], e_nogit["level"])

        # A 65-character "sha256" -- the literal shape of the golden that opened
        # this slice. It graded clean because nothing ever looked at the field
        # except to compare it with itself.
        bad_len = "a" * 65
        e = establish(doc({"path": str(binp), "sha256": bad_len,
                           "embedded_fingerprint": {
                               "present": True, "sha": SYN_A,
                               "exe_sha256_at_scan": bad_len}}), "badsha")
        chk("a 65-character 'sha256' -> UNESTABLISHED (it is not a digest)",
            not e["ok"] and "not a sha256" in e["why"], e["level"])

        # A commit id that cannot be one.
        e = establish(doc({"path": str(binp), "sha256": true_sha,
                           "embedded_fingerprint": {
                               "present": True, "sha": "not-a-commit",
                               "exe_sha256_at_scan": true_sha}}), "badcommit")
        chk("a non-hex 'commit' -> UNESTABLISHED",
            not e["ok"] and "not a git object id" in e["why"], e["level"])

        # Stamp read from other bytes than the digest names.
        crossed = dict(good)
        crossed["embedded_fingerprint"] = dict(fp)
        crossed["embedded_fingerprint"]["exe_sha256_at_scan"] = "00" * 32
        e = establish(doc(crossed), "crossed")
        chk("fingerprint read from other bytes -> UNESTABLISHED",
            not e["ok"] and "two different binaries" in e["why"], e["level"])

        # Binary replaced in place -- the v0.5.1-baseline situation.
        binp.write_bytes(_synthetic_binary(SYN_B, "other",
                                           "2026-07-30T00:00:00", "A:/z", False))

        # ...with NO witness. A file agreeing with itself proves nothing:
        # it is the same shape as
        # the forgery: nothing outside the record backs any of it.
        e = establish(doc(good), "replaced-unwitnessed")
        chk("binary replaced, NO witness -> UNCORROBORATED (refused)",
            not e["ok"] and e["level"] == "UNCORROBORATED", e["level"])

        # ...and with a real W-ARTIFACT witness: a genuinely separate file that
        # independently records the same exe digest, pinned by its own sha256.
        # THIS is the case a golden that outlived its binary must still pass, so
        # the gate is not one that refuses all history.
        wpath = root / "r1_capture_run.json"
        wpath.write_text(json.dumps(
            {"kind": "visual-verify-capture",
             "identity": {"exe": {"path": str(binp), "sha256": true_sha}}},
            indent=2), encoding="utf-8")
        wit = {"path": str(wpath), "artifact_sha256": _sha256_file(wpath),
               "records_exe_sha256": true_sha}

        def doc_w(exe_blk, witnesses):
            return {"kind": "visual-verify-golden",
                    "identity": {"exe": exe_blk,
                                 "binary_corroboration": {"witnesses": witnesses}}}

        e = establish(doc_w(good, [wit]), "replaced-witnessed")
        chk("binary replaced, ONE pinned witness agrees -> CORROBORATED-AT-MINT "
            "(history still grades)",
            e["ok"] and e["level"] == "CORROBORATED-AT-MINT"
            and any("W-ARTIFACT" in w for w in e["witnesses"]), e["level"])

        # Binary entirely absent from disk, but witnessed: the legitimate
        # "golden outlived its install" case. Must still grade.
        gone = {"path": str(root / "deleted_install" / "mc2.exe"),
                "sha256": true_sha, "embedded_fingerprint": dict(fp)}
        e = establish(doc_w(gone, [wit]), "absent-witnessed")
        chk("binary GONE but witnessed -> CORROBORATED-AT-MINT (a golden may "
            "outlive its install)",
            e["ok"] and e["level"] == "CORROBORATED-AT-MINT", e["level"])
        e = establish(doc(gone), "absent-unwitnessed")
        chk("binary GONE and unwitnessed -> UNCORROBORATED (refused)",
            not e["ok"] and e["level"] == "UNCORROBORATED", e["level"])

        # A witness that was EDITED after mint stops counting: its pinned digest
        # no longer matches, so agreeing with the record buys nothing.
        tampered = dict(wit)
        tampered["artifact_sha256"] = "cc" * 32
        e = establish(doc_w(gone, [tampered]), "witness-tampered")
        chk("a witness whose pinned digest no longer matches does NOT count",
            not e["ok"] and e["level"] == "UNCORROBORATED", e["level"])

        # A witness that verifies but names a DIFFERENT binary is not a missing
        # witness -- it is a contradiction, and must refuse outright.
        wpath2 = root / "r2_capture_run.json"
        wpath2.write_text(json.dumps(
            {"kind": "visual-verify-capture",
             "identity": {"exe": {"path": str(binp), "sha256": "dd" * 32}}},
            indent=2), encoding="utf-8")
        e = establish(doc_w(gone, [{"path": str(wpath2),
                                    "artifact_sha256": _sha256_file(wpath2)}]),
                      "witness-contradicts")
        chk("a verified witness naming a DIFFERENT binary -> UNESTABLISHED",
            not e["ok"] and "CONTRADICTS" in e["why"], e["level"])

        # A record must not be its own witness. `establish_path` knows the file
        # it came from; without that, a forger lists the forgery as its own
        # proof and the "outside this file" rule is decoration.
        #
        # The control is the TWIN below: byte-identical content at a different
        # path DOES corroborate. So the only difference between the two cases is
        # the self-reference -- not a stale digest, not a shape problem.
        selfw = root / "selfwitness.json"
        selfw.write_text(json.dumps(doc_w(gone, [{"path": str(selfw)}]),
                                    indent=2), encoding="utf-8")
        e = establish_path(selfw, "selfwitness")
        chk("a record listing ITSELF as its witness -> UNCORROBORATED",
            not e["ok"] and e["level"] == "UNCORROBORATED", e["level"])

        twin = root / "twin.json"
        twin.write_bytes(selfw.read_bytes())
        e = establish(doc_w(gone, [{"path": str(twin),
                                    "artifact_sha256": _sha256_file(twin)}]),
                      "twinwitness")
        chk("control: the SAME bytes at a different path DO corroborate "
            "(so the case above failed on self-reference, nothing else)",
            e["ok"] and e["level"] == "CORROBORATED-AT-MINT", e["level"])

        # Contradicted: the artifact claims a commit the byte-identical binary
        # on disk denies.
        live_sha = _sha256_file(binp)
        lying = {"path": str(binp), "sha256": live_sha,
                 "embedded_fingerprint": {"present": True,
                                          "sha": SYN_A, "dirty": False,
                                          "branch": "b", "built": "x", "src": "y",
                                          "method": "pe-string-scan/v1",
                                          "exe_sha256_at_scan": live_sha}}
        e = establish(doc(lying), "lying")
        chk("commit contradicted by the byte-identical binary -> UNESTABLISHED",
            not e["ok"] and "CONTRADICTED" in e["why"], e["level"])

        # THE COMMENT TRAP -- every keyword this module looks for, as free text
        # only. Structure and bytes are checked, not identifiers.
        trap = {"kind": "visual-verify-golden",
                "exe": r"A:\x\mc2.exe", "exe_mtime": "2026-01-01T00:00:00Z",
                "_comment": ("embedded_fingerprint exe_sha256_at_scan "
                             "binary_provenance VERIFIED-LIVE present true "
                             "79360fdb565e commit_source="
                             "exe-embedded-build-fingerprint")}
        e = establish(trap, "commenttrap")
        chk("keywords present only in a comment -> still UNESTABLISHED",
            not e["ok"], e["level"])

        # refuse_unless_established must raise, and must NAME the bad input.
        # NOTE the witnessed form: `good` alone no longer establishes here,
        # because the binary was replaced in place above and nothing outside
        # that record vouches for it. Using the unwitnessed form would make the
        # "silent when all establish" arm below pass for the wrong reason.
        est_ok = lambda lbl: establish(doc_w(good, [wit]), lbl)  # noqa: E731
        try:
            refuse_unless_established(
                [est_ok("golden"), establish(trap, "candidate")],
                decision="grade", remedy="re-bless")
            chk("refuse_unless_established raises on a bad input", False,
                "returned normally")
        except ProvenanceUnestablished as exc:
            chk("refuse_unless_established raises and names the bad input",
                "candidate" in str(exc), "")
        # ...and must NOT raise when everything establishes.
        try:
            refuse_unless_established([est_ok("golden")],
                                      decision="grade", remedy="x")
            chk("refuse_unless_established is silent when all establish", True)
        except ProvenanceUnestablished:
            chk("refuse_unless_established is silent when all establish", False,
                "raised on a clean input -- a gate that refuses everything "
                "proves nothing")

        a = est_ok("A")
        b = establish(doc(lying), "B")
        chk("same_binary detects two different binaries",
            same_binary([a, b])[0] is False)
        chk("same_binary accepts one binary", same_binary([a, a])[0] is True)
        # Two artifacts that BOTH failed to record a digest must not collapse
        # to {None} and read as agreement.
        nod = establish(trap, "nodigest")
        chk("same_binary refuses when a digest is MISSING (not 'the same')",
            same_binary([nod, nod])[0] is False
            and "never 'the same'" in same_binary([nod, nod])[1])

    print("[prov-gate] self-test " + ("PASS" if ok else "FAILED"))
    return 0 if ok else 2


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("artifacts", nargs="*",
                    help="JSON artifacts (golden.json / capture_run.json / "
                         "*_capture_manifest.json) to interrogate")
    ap.add_argument("--selftest", action="store_true",
                    help="prove each verdict on binaries carrying real stamps")
    args = ap.parse_args(argv)
    if args.selftest:
        return _selftest()
    if not args.artifacts:
        ap.error("nothing to interrogate (pass artifacts, or --selftest)")
    bad = 0
    for a in args.artifacts:
        e = establish_path(a)
        mark = "OK " if e["ok"] else "NO "
        print(f"[{mark}] {e['level']:<16} {a}")
        print(f"          commit={e['commit']} exe_sha256="
              f"{str(e['exe_sha256'])[:12]}")
        print(f"          {e['why']}")
        bad += 0 if e["ok"] else 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
