#!/usr/bin/env python3
"""CAPTURE-PROVENANCE-TRUTH-1 -- read a binary's OWN build fingerprint, statically.

WHY THIS EXISTS
---------------
Every artifact manifest recorded `identity.git.commit`, derived from
`git rev-parse HEAD` in the SOURCE WORKTREE THE TOOL RAN FROM. That has no
causal relationship whatsoever to the commit compiled into the binary being
measured. The two coincide only when the operator happens to be capturing from
the same worktree that produced the exe -- which, in a multi-worktree lane
setup, is the exception rather than the rule.

Observed (scratch/mc203reg, 2026-07-29): nine capture arms recorded THREE
different `git.commit` values (5a622db5 / 5c04048c / 3ab322a0) while eight of
them shared one identical `exe.sha256`. All eight measured the same binary,
built from 79360fdb on branch claude/spotlight-direction-1 in worktree
A:/Games/mc2-spotdir1 -- a commit none of the manifests names, on a branch none
of them names. A reader of `04_animstatic` would reasonably conclude it
exercised 3ab322a0 (OVERLAY-CEMENT-ADOPT-1). It did not; that code was never in
the binary.

WHAT THE TRUTH IS
-----------------
cmake/GenerateBuildFingerprint.cmake stamps generated/build_fingerprint.h at
build time and code/mechcmd2.cpp compiles those macros into the binary:

    printf("[BUILD_FINGERPRINT v1] sha=%s dirty=%d branch=%s built=%s src=%s\\n",
           MC2_BUILD_GIT_SHA, MC2_BUILD_GIT_DIRTY, MC2_BUILD_GIT_BRANCH,
           MC2_BUILD_TIME_ISO, MC2_BUILD_SRC_WORKTREE);
    #if MC2_BUILD_GIT_DIRTY
    printf("[BUILD_FINGERPRINT v1] WARNING: BUILT-FROM-DIRTY-TREE\\n");
    #endif

The four string macros land in .rdata as NUL-terminated literals; the dirty
flag is a preprocessor conditional, so the WARNING literal exists in the image
IF AND ONLY IF the build tree had tracked modifications. Both are recoverable
from the file's bytes -- no launch, no log, no header.

READ THE EXE, NOT THE HEADER (c5d934d5 / DEPLOY-FINGERPRINT lesson)
------------------------------------------------------------------
A guard that reads generated/build_fingerprint.h answers "what did cmake stamp
last", not "what is in this binary". A failed or skipped compile leaves the
header freshly stamped and the exe stale, and the guard passes. This module
only ever reads the binary itself.

SELF-TESTING SCAN (pe_gate_scan.py lesson)
------------------------------------------
`strings` is not installed on this machine, so byte scans here carry their own
positive and negative controls. A "not found" that did not first prove the
scanner works is not a finding.

Usage:
    py -3 scripts/exe_fingerprint.py <binary> [--json]
    py -3 scripts/exe_fingerprint.py <binary> --operands

--operands dumps the pooled literals (with offsets and alignment) and the raw
bytes preceding the format literal. Reach for it when a binary reports
layout-unrecognized: it shows directly which operand went missing and why,
instead of leaving the reason code to be theorised about.

Exit: 0 fingerprint extracted | 1 absent/unreadable | 2 self-test failed.
"""
from __future__ import annotations

import argparse
import hashlib
import re
import sys
from pathlib import Path
from typing import Any

# The exact format literal emitted by code/mechcmd2.cpp. Trailing NUL pins it to
# a complete .rdata string rather than a prefix of something longer.
BANNER_FMT = (b"[BUILD_FINGERPRINT v1] sha=%s dirty=%d branch=%s built=%s "
              b"src=%s\n\x00")
# Present in the image only when MC2_BUILD_GIT_DIRTY was 1 at compile time.
DIRTY_MARKER = b"[BUILD_FINGERPRINT v1] WARNING: BUILT-FROM-DIRTY-TREE"
# Positive control: any binary carrying the banner is an engine build.
POSITIVE_CONTROL = b"[BUILD_FINGERPRINT v1]"
# Negative control: cannot occur in a real binary.
NEGATIVE_CONTROL = b"MC2_FINGERPRINT_NEEDLE_THAT_CANNOT_EXIST_41b7de92"

# The self-describing record (cmake/GenerateBuildFingerprint.cmake -> MC2_BUILD_FP_LITERAL, printed by
# gos_crashbundle.cpp so the linker keeps it): MC2FP1|sha|dirty|branch|iso-timestamp|srcdir|FP1END. It is found by
# its markers anywhere in the image, so no neighbouring string-pool literal can break the read.
FP_LITERAL_RE = re.compile(rb"MC2FP1\|([\x20-\x7e]{1,600}?)\|FP1END")

_ISO_RE = re.compile(r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d$")
_SHA_RE = re.compile(r"^[0-9a-f]{7,40}$")
_PATH_RE = re.compile(r"^(?:[A-Za-z]:[\\/]|[\\/]).+")

#: cmake writes this literal when git is unavailable or fails.
UNKNOWN = "unknown"

#: How far back from the format literal to look for the pooled operands.
_WINDOW = 1024

#: Alignment of pooled .rdata string literals; used only to skip filler bytes
#: packed into an inter-literal gap (see _printable_runs).
_ALIGN = 8

#: Minimum length of a SALVAGED run (clean tokens are never length-filtered).
#: Shorter salvage results are shards of a neighbouring binary constant, not
#: operands: sha is >= 7 and the ISO stamp is 19. Only a 1-character branch
#: name could be lost here, costing a refusal (honest) not a mis-attribution.
_MIN_OPERAND = 2


class FingerprintError(Exception):
    """Raised only by the CLI; the library returns an error dict instead."""


def _err(reason: str, detail: str = "") -> dict[str, Any]:
    return {"present": False, "reason": reason, "detail": detail}


def selftest(blob: bytes) -> tuple[bool, str]:
    """Prove the scanner works on THIS blob before any absence is reported."""
    if blob.find(NEGATIVE_CONTROL) >= 0:
        return False, "negative control matched -- scan logic is broken"
    if blob.find(POSITIVE_CONTROL) < 0:
        # Not a failure of the scanner: a genuinely pre-fingerprint or
        # non-engine binary. The caller distinguishes via the reason code.
        return True, "no positive control (not an engine build, or pre-fingerprint)"
    return True, "ok"


def _printable_runs(blob: bytes, off: int,
                    window: int = _WINDOW) -> list[tuple[int, str]]:
    """(start_offset, text) for the pooled literals preceding `off`.

    OPERAND-LOST-TO-ALIGNMENT-FILLER (FINGERPRINT-DETACHED-1, 2026-07-31)
    --------------------------------------------------------------------
    This used to split on NUL alone and drop any token that was not entirely
    printable. That silently DELETES an operand, because the linker packs small
    binary constants into the alignment gaps between pooled .rdata literals.
    Observed in A:/Games/mc2-terrain-ambient-floor's build, where the gap after
    the timestamp holds four bytes of data instead of padding:

        ...2026-07-30T12:33:10\\x00 \\xe7\\x84\\x11U claude/terrain-ambient-floor-2\\x00

    NUL-splitting welds b"\\xe7\\x84\\x11U" onto the branch literal; the
    all-printable test then discards the whole token. The branch operand
    vanishes, the positional tail-4 window slides one slot, and _classify
    reports layout-unrecognized on a perfectly good binary.

    Note the last filler byte (0x55 'U') is itself printable, so "strip leading
    non-printable bytes" would yield "Uclaude/..." -- a silently corrupted
    branch, which is worse than refusing.

    The fix is STRICTLY ADDITIVE: a NUL-delimited token that is entirely
    printable is still taken verbatim, so every binary that parses today parses
    bit-for-bit identically. Only a token the old code would have THROWN AWAY is
    salvaged, by taking its trailing printable run and then advancing that run's
    start to the next 8-byte boundary. Pooled literals are 8-byte aligned
    (verified on every operand of all 16 builds on this disk) while filler
    shards are not, so 0x...657 -> 0x...658 drops exactly the 'U' and nothing
    else. Trimming is confined to already-rejected tokens, so it can never
    corrupt an operand that reads correctly today.

    Residual limit, deliberately not handled: filler that is ITSELF entirely
    printable (b"ABCD") leaves a wholly printable token, indistinguishable from
    a literal that genuinely begins "ABCD" without assuming alignment -- and
    assuming it universally would corrupt tightly packed layouts. That case
    yields a contaminated BRANCH but a correct sha/built/src, because the shape
    checks still gate those three. See test_entirely_printable_filler_*.
    """
    start = max(0, off - window)
    chunk = blob[start:off]
    runs: list[tuple[int, str]] = []

    pos = 0
    for raw in chunk.split(b"\x00"):
        tok = start + pos
        pos += len(raw) + 1
        if not raw:
            continue
        if all(0x20 <= c < 0x7F for c in raw):
            runs.append((tok, raw.decode("ascii")))
            continue
        # Old code dropped this token whole, deleting an operand. Salvage the
        # trailing printable run, then realign past any printable filler byte.
        i = len(raw)
        while i > 0 and 0x20 <= raw[i - 1] < 0x7F:
            i -= 1
        i += -(tok + i) % _ALIGN
        if len(raw) - i < _MIN_OPERAND:
            continue
        runs.append((tok + i, raw[i:].decode("ascii")))
    return runs


def _strings_before(blob: bytes, off: int, window: int = _WINDOW) -> list[str]:
    """Printable ASCII literals immediately preceding `off`."""
    return [s for _, s in _printable_runs(blob, off, window)]


def parse_literal(blob: bytes) -> dict[str, Any] | None:
    """Fields of the MC2FP1 literal, or None when the exe predates it or the record is malformed.

    sha, dirty, then (from the right) srcdir and timestamp are fixed; the branch is everything between, so a
    '|' inside a branch name cannot shift the other fields.
    """
    m = FP_LITERAL_RE.search(blob)
    if not m:
        return None
    parts = m.group(1).decode("ascii").split("|")
    if len(parts) < 5:
        return None
    sha, dirty, src, built = parts[0], parts[1], parts[-1], parts[-2]
    branch = "|".join(parts[2:-2])
    if not (_SHA_RE.match(sha) or sha == UNKNOWN) or dirty not in ("0", "1") or not branch:
        return None
    if not (_ISO_RE.match(built) or built == UNKNOWN):
        return None
    return {"src": src, "built": built, "branch": branch, "sha": sha, "dirty": dirty == "1"}


def _classify(tail: list[str]) -> dict[str, Any] | None:
    """Map the four pooled operands onto {src, built, branch, sha}.

    MSVC pools them in reverse argument order immediately before the format
    literal -- verified identical across nine independently produced binaries
    (v0.3, v0.4, v0.4c, v0.4d-rc1, v0.5.1 and four lane deploys). Position is
    used as the hypothesis and SHAPE MUST CONFIRM IT. When shape disagrees the
    function returns None so the caller reports "layout-unrecognized" instead
    of guessing -- a wrong commit attributed confidently is the failure mode
    this whole module exists to remove.
    """
    if len(tail) < 4:
        return None
    src, built, branch, sha = tail[-4], tail[-3], tail[-2], tail[-1]
    if not (_ISO_RE.match(built) or built == UNKNOWN):
        # Another literal can land between the operands (observed: "vp" between the timestamp and the branch).
        # sha and branch are always the last two; the timestamp is the last ISO token, with the path before it.
        iso = [i for i, t in enumerate(tail[:-2]) if _ISO_RE.match(t)]
        if not iso or iso[-1] < 1:
            return None
        src, built = tail[iso[-1] - 1], tail[iso[-1]]
    if not (_PATH_RE.match(src) or src == UNKNOWN):
        return None
    if not (_ISO_RE.match(built) or built == UNKNOWN):
        return None
    if not (_SHA_RE.match(sha) or sha == UNKNOWN):
        return None
    if not branch:
        return None
    # A branch that looks like a sha or a timestamp means the ordering
    # assumption broke; refuse rather than mislabel.
    if _ISO_RE.match(branch):
        return None
    return {"src": src, "built": built, "branch": branch, "sha": sha}


def extract_from_bytes(blob: bytes) -> dict[str, Any]:
    """Extract the embedded fingerprint from an in-memory image.

    Returns either
        {"present": True, sha, sha12, dirty, branch, built, src, method}
    or  {"present": False, "reason": <code>, "detail": <str>}

    reason codes:
        selftest-failed     the scanner itself is not trustworthy here
        banner-absent       no [BUILD_FINGERPRINT v1] literal (pre-fingerprint
                            exe, or not an engine binary at all)
        format-absent       banner tag present but not the v1 format literal
                            (the source printf changed; update BANNER_FMT)
        layout-unrecognized operands not in the expected shape -- refuse to guess
    """
    ok, note = selftest(blob)
    if not ok:
        return _err("selftest-failed", note)
    lit = parse_literal(blob)
    if lit is not None:
        sha = lit["sha"].lower()
        return {"present": True, "sha": sha, "sha12": (sha[:12] if sha != UNKNOWN else UNKNOWN),
                "dirty": lit["dirty"], "branch": lit["branch"], "built": lit["built"], "src": lit["src"],
                "method": "mc2fp1-literal/v1"}
    if blob.find(POSITIVE_CONTROL) < 0:
        return _err("banner-absent", note)
    off = blob.find(BANNER_FMT)
    if off < 0:
        return _err("format-absent",
                    "[BUILD_FINGERPRINT v1] tag present but the v1 printf "
                    "format literal was not found; the emitting printf in "
                    "code/mechcmd2.cpp probably changed shape")
    tail = _strings_before(blob, off)
    fields = _classify(tail)
    if fields is None:
        # Report WHAT WAS SEEN. A bare "layout-unrecognized" sent one
        # investigation chasing a detached-HEAD theory for a build whose real
        # problem was a filler byte in an alignment gap; the observed tail
        # would have shown that in one command.
        return _err("layout-unrecognized",
                    "the four pooled operands before the format literal do "
                    "not match {path, iso-timestamp, branch, sha}; observed "
                    f"tail: {[s[:60] for s in tail[-6:]]!r}")
    sha = fields["sha"].lower()
    return {
        "present": True,
        "sha": sha,
        "sha12": (sha[:12] if sha != UNKNOWN else UNKNOWN),
        "dirty": bool(blob.find(DIRTY_MARKER) >= 0),
        "branch": fields["branch"],
        "built": fields["built"],
        "src": fields["src"],
        "method": "pe-string-scan/v1",
    }


def extract(exe_path: str | Path) -> dict[str, Any]:
    """Extract the embedded fingerprint from a binary on disk.

    Never raises: an unreadable path is reported as {"present": False,
    "reason": "unreadable"}. A manifest must never fail to be written because
    an enrichment lookup failed -- but it must also never claim provenance it
    does not have, which is why the failure is a REASON, not a silent null.
    """
    try:
        blob = Path(exe_path).read_bytes()
    except Exception as e:  # noqa: BLE001 -- any IO failure is the same answer
        return _err("unreadable", f"{type(e).__name__}: {e}")
    out = extract_from_bytes(blob)
    out["exe_sha256_at_scan"] = hashlib.sha256(blob).hexdigest()
    return out


def sha_agrees(embedded_sha: str | None, other_sha: str | None) -> bool | None:
    """Prefix-compare two git shas of possibly different lengths.

    Returns None when either side is missing or 'unknown' (unknowable, NOT
    agreement). Requires >= 7 shared hex digits, matching the DEPLOY_FINGERPRINT
    comparison in the smoke runner's fingerprint module.
    """
    if not embedded_sha or not other_sha:
        return None
    a, b = embedded_sha.lower(), other_sha.lower()
    if a == UNKNOWN or b == UNKNOWN:
        return None
    n = min(len(a), len(b))
    if n < 7:
        return None
    return a[:n] == b[:n]


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("binary", help="path to the binary to read")
    ap.add_argument("--json", action="store_true", help="emit JSON")
    ap.add_argument("--operands", action="store_true",
                    help="dump the literal window preceding the format string "
                         "(diagnostic for layout-unrecognized)")
    args = ap.parse_args(argv)

    if args.operands:
        blob = Path(args.binary).read_bytes()
        off = blob.find(BANNER_FMT)
        if off < 0:
            print("format literal not found", file=sys.stderr)
            return 1
        print(f"format literal at 0x{off:x}")
        for i, (o, s) in enumerate(_printable_runs(blob, off)):
            print(f"  [{i}] 0x{o:x} align{_ALIGN}={o % _ALIGN} {s!r}")
        print("raw bytes preceding the format literal:")
        print("  " + repr(blob[max(0, off - 160):off]))
        return 0

    fp = extract(args.binary)
    if args.json:
        import json
        print(json.dumps(fp, indent=2))
    else:
        if fp.get("present"):
            print(f"sha={fp['sha']} dirty={int(fp['dirty'])} "
                  f"branch={fp['branch']} built={fp['built']} src={fp['src']}")
            print(f"exe_sha256={fp['exe_sha256_at_scan']}")
        else:
            print(f"NO FINGERPRINT: {fp['reason']} -- {fp.get('detail', '')}",
                  file=sys.stderr)
    if fp.get("reason") == "selftest-failed":
        return 2
    return 0 if fp.get("present") else 1


if __name__ == "__main__":
    sys.exit(main())
