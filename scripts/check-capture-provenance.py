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

Usage:
    py -3 scripts/check-capture-provenance.py [ROOT ...]      # verify (default)
    py -3 scripts/check-capture-provenance.py --write [ROOT]  # backfill truth
    py -3 scripts/check-capture-provenance.py --selftest      # prove it fires

Exit: 0 clean | 1 violation(s) | 2 self-test failed.
"""
from __future__ import annotations

import argparse
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

#: Artifact trees the repo treats as EVIDENCE. `scratch/` is operator working
#: space and is scanned only when named explicitly on the command line.
DEFAULT_ROOTS = ("tests/visual/captures", "tests/visual/baselines", "run")

#: Manifest filenames that record a measurement of a binary.
MANIFEST_SUFFIXES = ("_capture_manifest.json",)
MANIFEST_NAMES = ("capture_run.json", "manifest.json", "set.json")

SKIP_DIRS = {".git", "build64", "__pycache__", "node_modules"}


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
    print(f"\n[provenance] scanned={len(manifests)} FAIL={len(fails)} "
          f"WARN={len(warns)}  roots={[str(r) for r in roots]}")
    if fails:
        print(f"[provenance] {len(fails)} manifest(s) cannot state which binary "
              f"they measured. A capture whose commit is unknown cannot prove "
              f"a fix; re-emit via manifest_schema.identity_block, or run "
              f"--write where the binary is still on disk.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
