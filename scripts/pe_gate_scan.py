#!/usr/bin/env python3
"""pe_gate_scan.py — is this string actually IN that binary?

WHY THIS EXISTS
---------------
`strings` is NOT INSTALLED on the MC2 dev machine. A probe of the shape

    strings -a mc2.exe | grep -c MC2_SOME_GATE      # -> 0

returns a clean, plausible, *entirely false* zero for every needle in every
binary, because the pipe is empty (`strings: command not found` goes to stderr,
which is usually discarded). That is indistinguishable from a real
"gate absent from the deployed binary" finding, and at least one session's
conclusions were drawn through it. See ADVERSARIAL-VERIFY-1 Finding 7.

This tool replaces it with a dependency-free byte scan, and — critically —
**self-tests on every run** with a positive and a negative control, so an
empty result set can never be silently mistaken for a real answer.

USAGE
-----
    py -3 scripts/pe_gate_scan.py <binary> [NEEDLE ...]
    py -3 scripts/pe_gate_scan.py <binary> --prefix MC2_ --list
    py -3 scripts/pe_gate_scan.py <binary> --gates-from-registry

Exit codes: 0 all needles found | 1 at least one missing | 2 self-test failed.

The self-test is not optional and cannot be skipped. If the positive control
(a string that MUST be in any MC2 exe) is not found, or the negative control (a
random UUID-ish string that cannot be) IS found, the tool exits 2 and prints
nothing else. Trust no output that did not print `[selftest] OK`.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# Positive control: every MC2 exe embeds its own build fingerprint banner.
POSITIVE_CONTROL = b"[BUILD_FINGERPRINT v1]"
# Negative control: cannot occur in any real binary.
NEGATIVE_CONTROL = b"MC2_NEEDLE_THAT_CANNOT_EXIST_9f3a1c07"

ASCII_RUN = re.compile(rb"[\x20-\x7e]{4,}")


def find_all(blob: bytes, needle: bytes) -> list[int]:
    """Byte offsets of every occurrence of needle (ASCII and UTF-16LE)."""
    hits: list[int] = []
    for form in (needle, needle.decode("latin-1").encode("utf-16-le")):
        start = 0
        while True:
            i = blob.find(form, start)
            if i < 0:
                break
            hits.append(i)
            start = i + 1
    return sorted(hits)


def selftest(blob: bytes) -> bool:
    pos = find_all(blob, POSITIVE_CONTROL)
    neg = find_all(blob, NEGATIVE_CONTROL)
    if not pos:
        print(
            f"[selftest] FAIL: positive control {POSITIVE_CONTROL!r} not found. "
            "Either this is not an MC2 exe, or the scan is broken. "
            "DO NOT TRUST ANY 'not found' RESULT.",
            file=sys.stderr,
        )
        return False
    if neg:
        print("[selftest] FAIL: negative control matched — scan logic is broken.",
              file=sys.stderr)
        return False
    print(f"[selftest] OK (positive control x{len(pos)}, negative control x0)")
    return True


def list_prefixed(blob: bytes, prefix: bytes) -> list[str]:
    out: set[str] = set()
    for m in ASCII_RUN.finditer(blob):
        run = m.group()
        start = 0
        while True:
            i = run.find(prefix, start)
            if i < 0:
                break
            j = i
            while j < len(run) and (run[j : j + 1].isalnum() or run[j : j + 1] == b"_"):
                j += 1
            out.add(run[i:j].decode("ascii"))
            start = i + 1
    return sorted(out)


def registry_gates(repo_root: Path) -> list[str]:
    reg = repo_root / "RenderCore" / "RendererFeatureRegistry.h"
    if not reg.exists():
        return []
    text = reg.read_text(encoding="utf-8", errors="replace")
    return sorted(set(re.findall(r'"(MC2_[A-Z0-9_]+)"', text)))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("binary", type=Path)
    ap.add_argument("needles", nargs="*")
    ap.add_argument("--prefix", default="MC2_")
    ap.add_argument("--list", action="store_true",
                    help="list every distinct --prefix string in the binary")
    ap.add_argument("--gates-from-registry", action="store_true")
    ap.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[1])
    args = ap.parse_args()

    if not args.binary.is_file():
        print(f"ERROR: no such file: {args.binary}", file=sys.stderr)
        return 2
    blob = args.binary.read_bytes()
    print(f"[scan] {args.binary}  ({len(blob):,} bytes)")

    if not selftest(blob):
        return 2

    needles = list(args.needles)
    if args.gates_from_registry:
        needles += registry_gates(args.repo_root)

    if args.list:
        found = list_prefixed(blob, args.prefix.encode())
        print(f"[list] {len(found)} distinct {args.prefix}* strings")
        for s in found:
            print(f"  {s}")
        if not needles:
            return 0

    if not needles:
        print("(no needles given; use --list or pass names)")
        return 0

    missing = 0
    for n in needles:
        hits = find_all(blob, n.encode())
        if hits:
            print(f"  PRESENT  {n}  (x{len(hits)}, first @0x{hits[0]:x})")
        else:
            print(f"  ABSENT   {n}")
            missing += 1
    print(f"[result] {len(needles) - missing}/{len(needles)} present, {missing} absent")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
