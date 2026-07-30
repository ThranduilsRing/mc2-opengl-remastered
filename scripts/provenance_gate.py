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

THE RULE
--------
Provenance is ESTABLISHED only when the artifact names its binary by CONTENT and
records that binary's OWN embedded build fingerprint, read from those same bytes.
Three levels, and only the first two may be graded against:

  VERIFIED-LIVE     recorded sha256 + embedded fingerprint, the binary is still
                    on disk byte-identical, and a live re-read of its stamp
                    agrees. The strongest statement available.
  RECORDED-AT-MINT  same recording, but the binary is gone or replaced. It
                    cannot be re-read -- yet the record is internally
                    self-consistent (`embedded_fingerprint.exe_sha256_at_scan`
                    equals `exe.sha256`, i.e. the stamp was demonstrably read
                    from the very bytes the digest names) and was tool-emitted.
  UNESTABLISHED     everything else. REFUSE TO GRADE.

WHAT IS DELIBERATELY *NOT* ACCEPTED
-----------------------------------
  * path + mtime. An mtime is not an identity; overwrite the file and the
    measurement is unattributable, which is precisely what happened above.
  * a hand-added identity block. PROVENANCE-EMIT-1 already ruled that provenance
    is tool-emitted or it is nothing: a hand-typed claim is exactly as
    trustworthy as the human who typed it, and the one in the tree today is
    already contradicted by the bytes.
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
import sys
from pathlib import Path
from typing import Any

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))
import exe_fingerprint as ef  # noqa: E402

#: Levels that may be graded against, strongest first.
GRADEABLE = ("VERIFIED-LIVE", "RECORDED-AT-MINT")


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


def establish(doc: "dict[str, Any]", label: str = "artifact") -> "dict[str, Any]":
    """Answer 'can this artifact prove which binary produced it?'.

    Returns a dict that is always shaped the same:
        {label, level, ok, commit, dirty, branch, exe_sha256, exe_path, why}
    `ok` is True only for a level in GRADEABLE. `why` always states, in one
    sentence a human can act on, what is missing or what was proven.
    """
    out = {"label": label, "level": "UNESTABLISHED", "ok": False,
           "commit": None, "dirty": None, "branch": None,
           "exe_sha256": None, "exe_path": None, "why": ""}

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

    out["commit"] = fp.get("sha")
    out["dirty"] = fp.get("dirty")
    out["branch"] = fp.get("branch")

    p = Path(blk.get("path") or "")
    live_sha = _sha256_file(p) if p.name and p.exists() else None
    if live_sha is None:
        out["level"] = "RECORDED-AT-MINT"
        out["ok"] = True
        out["why"] = (f"binary {p} is no longer on disk, so the record cannot "
                      f"be re-verified live; the recording is self-consistent "
                      f"(stamp read from the digested bytes) and names commit "
                      f"{out['commit']}.")
        return out
    if live_sha != sha:
        out["level"] = "RECORDED-AT-MINT"
        out["ok"] = True
        out["why"] = (f"binary at {p} has been replaced since mint (now "
                      f"{live_sha[:12]}, recorded {sha[:12]}), so the record "
                      f"cannot be re-verified live; the recording is "
                      f"self-consistent and names commit {out['commit']}.")
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
                "exe_sha256": None, "exe_path": None,
                "why": f"cannot be read as JSON ({type(e).__name__}: {e})."}
    if not isinstance(doc, dict):
        return {"label": lbl, "level": "UNESTABLISHED", "ok": False,
                "commit": None, "dirty": None, "branch": None,
                "exe_sha256": None, "exe_path": None,
                "why": "top level is not a JSON object."}
    est = establish(doc, lbl)
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

    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        binp = root / "engine.bin"
        binp.write_bytes(_synthetic_binary("79360fdb565e",
                                           "claude/spotlight-direction-1",
                                           "2026-07-29T13:52:25",
                                           "A:/Games/mc2-spotdir1", dirty=False))
        true_sha = _sha256_file(binp)
        fp = ef.extract(str(binp))
        if not (fp.get("present") and fp["sha"] == "79360fdb565e"):
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
            and e["commit"] == "79360fdb565e", e["level"])

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
                "embedded_fingerprint": {"present": True, "sha": "79360fdb565e",
                                         "dirty": False, "branch": "b"}}
        e = establish(doc(hand), "handadded")
        chk("hand-added fingerprint (no exe_sha256_at_scan) -> UNESTABLISHED",
            not e["ok"] and "hand-added" in e["why"], e["level"])

        # Stamp read from other bytes than the digest names.
        crossed = dict(good)
        crossed["embedded_fingerprint"] = dict(fp)
        crossed["embedded_fingerprint"]["exe_sha256_at_scan"] = "00" * 32
        e = establish(doc(crossed), "crossed")
        chk("fingerprint read from other bytes -> UNESTABLISHED",
            not e["ok"] and "two different binaries" in e["why"], e["level"])

        # Binary replaced in place -- the v0.5.1-baseline situation.
        binp.write_bytes(_synthetic_binary("deadbeef1234", "other",
                                           "2026-07-30T00:00:00", "A:/z", False))
        e = establish(doc(good), "replaced")
        chk("binary replaced in place -> RECORDED-AT-MINT (still gradeable)",
            e["level"] == "RECORDED-AT-MINT" and e["ok"]
            and e["commit"] == "79360fdb565e", e["level"])

        # Contradicted: the artifact claims a commit the byte-identical binary
        # on disk denies.
        live_sha = _sha256_file(binp)
        lying = {"path": str(binp), "sha256": live_sha,
                 "embedded_fingerprint": {"present": True,
                                          "sha": "79360fdb565e", "dirty": False,
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
        try:
            refuse_unless_established(
                [establish(doc(good), "golden"), establish(trap, "candidate")],
                decision="grade", remedy="re-bless")
            chk("refuse_unless_established raises on a bad input", False,
                "returned normally")
        except ProvenanceUnestablished as exc:
            chk("refuse_unless_established raises and names the bad input",
                "candidate" in str(exc), "")
        # ...and must NOT raise when everything establishes.
        try:
            refuse_unless_established([establish(doc(good), "golden")],
                                      decision="grade", remedy="x")
            chk("refuse_unless_established is silent when all establish", True)
        except ProvenanceUnestablished:
            chk("refuse_unless_established is silent when all establish", False,
                "raised on a clean input -- a gate that refuses everything "
                "proves nothing")

        a = establish(doc(good), "A")
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
